#include <Python.h>
#include <structmember.h>

#include "collectd.h"
#include "common.h"

#include "cpython.h"

typedef struct cpy_callback_s {
	char *name;
	PyObject *callback;
	PyObject *data;
	struct cpy_callback_s *next;
} cpy_callback_t;

/* This is our global thread state. Python saves some stuff in thread-local
 * storage. So if we allow the interpreter to run in the background
 * (the scriptwriters might have created some threads from python), we have
 * to save the state so we can resume it later after shutdown. */

static PyThreadState *state;

static cpy_callback_t *cpy_config_callbacks;
static cpy_callback_t *cpy_init_callbacks;
static cpy_callback_t *cpy_shutdown_callbacks;

static void cpy_destroy_user_data(void *data) {
	cpy_callback_t *c = data;
	free(c->name);
	Py_DECREF(c->callback);
	Py_XDECREF(c->data);
	free(c);
}

/* You must hold the GIL to call this function!
 * But if you managed to extract the callback parameter then you probably already do. */

static void cpy_build_name(char *buf, size_t size, PyObject *callback, const char *name) {
	const char *module;
	PyObject *mod = NULL, *n = NULL;
	
	if (name != NULL && strchr(name, '.') != NULL) {
		snprintf(buf, size, "python.%s", name);
		return;
	}
	
	mod = PyObject_GetAttrString(callback, "__module__"); /* New reference. */
	if (mod != NULL)
		module = PyString_AsString(mod);
	else
		module = "collectd";
	if (name != NULL) {
		snprintf(buf, size, "python.%s.%s", module, name);
		Py_XDECREF(mod);
		return;
	}
	
	n = PyObject_GetAttrString(callback, "__name__"); /* New reference. */
	if (n != NULL)
		name = PyString_AsString(n);
	
	if (name != NULL)
		snprintf(buf, size, "python.%s.%s", module, name);
	else
		snprintf(buf, size, "python.%s.%p", module, callback);
	Py_XDECREF(mod);
	Py_XDECREF(n);
}

static int cpy_read_callback(user_data_t *data) {
	cpy_callback_t *c = data->data;
	PyObject *ret;

	CPY_LOCK_THREADS
		if (c->data == NULL)
			ret = PyObject_CallFunctionObjArgs(c->callback, (void *) 0);
		else
			ret = PyObject_CallFunctionObjArgs(c->callback, c->data, (void *) 0);
		if (ret == NULL) {
			/* FIXME */
			PyErr_Print();
		} else {
			Py_DECREF(ret);
		}
	CPY_RELEASE_THREADS
	return 0;
}

static int cpy_write_callback(const data_set_t *ds, const value_list_t *value_list, user_data_t *data) {
	int i;
	cpy_callback_t *c = data->data;
	PyObject *ret, *v, *list;

	CPY_LOCK_THREADS
		list = PyList_New(value_list->values_len); /* New reference. */
		if (list == NULL) {
			PyErr_Print();
			CPY_RETURN_FROM_THREADS 0;
		}
		for (i = 0; i < value_list->values_len; ++i) {
			if (ds->ds->type == DS_TYPE_COUNTER) {
				if ((long) value_list->values[i].counter == value_list->values[i].counter)
					PyList_SetItem(list, i, PyInt_FromLong(value_list->values[i].counter));
				else
					PyList_SetItem(list, i, PyLong_FromUnsignedLongLong(value_list->values[i].counter));
			} else if (ds->ds->type == DS_TYPE_GAUGE) {
				PyList_SetItem(list, i, PyFloat_FromDouble(value_list->values[i].gauge));
			} else if (ds->ds->type == DS_TYPE_DERIVE) {
				if ((long) value_list->values[i].derive == value_list->values[i].derive)
					PyList_SetItem(list, i, PyInt_FromLong(value_list->values[i].derive));
				else
					PyList_SetItem(list, i, PyLong_FromLongLong(value_list->values[i].derive));
			} else if (ds->ds->type == DS_TYPE_ABSOLUTE) {
				if ((long) value_list->values[i].absolute == value_list->values[i].absolute)
					PyList_SetItem(list, i, PyInt_FromLong(value_list->values[i].absolute));
				else
					PyList_SetItem(list, i, PyLong_FromUnsignedLongLong(value_list->values[i].absolute));
			} else {
				ERROR("cpy_write_callback: Unknown value type %d.", ds->ds->type);
				Py_DECREF(list);
				CPY_RETURN_FROM_THREADS 0;
			}
			if (PyErr_Occurred() != NULL) {
				PyErr_Print();
				CPY_RETURN_FROM_THREADS 0;
			}
		}
		v = PyObject_CallFunction((PyObject *) &ValuesType, "sOssssdi", value_list->type, list,
				value_list->plugin_instance, value_list->type_instance, value_list->plugin,
				value_list->host, (double) value_list->time, value_list->interval);
		Py_DECREF(list);
		ret = PyObject_CallFunctionObjArgs(c->callback, v, (void *) 0);
		if (ret == NULL) {
			/* FIXME */
			PyErr_Print();
		} else {
			Py_DECREF(ret);
		}
	CPY_RELEASE_THREADS
	return 0;
}

static void cpy_log_callback(int severity, const char *message, user_data_t *data) {
	cpy_callback_t * c = data->data;
	PyObject *ret;

	CPY_LOCK_THREADS
	if (c->data == NULL)
		ret = PyObject_CallFunction(c->callback, "is", severity, message); /* New reference. */
	else
		ret = PyObject_CallFunction(c->callback, "isO", severity, message, c->data); /* New reference. */

	if (ret == NULL) {
		/* FIXME */
		PyErr_Print();
	} else {
		Py_DECREF(ret);
	}
	CPY_RELEASE_THREADS
}

static PyObject *cpy_register_generic(cpy_callback_t **list_head, PyObject *args, PyObject *kwds) {
	cpy_callback_t *c;
	const char *name = NULL;
	PyObject *callback = NULL, *data = NULL, *mod = NULL;
	static char *kwlist[] = {"callback", "data", "name", NULL};
	
	if (PyArg_ParseTupleAndKeywords(args, kwds, "O|Oz", kwlist, &callback, &data, &name) == 0) return NULL;
	if (PyCallable_Check(callback) == 0) {
		PyErr_SetString(PyExc_TypeError, "callback needs a be a callable object.");
		return NULL;
	}
	if (name == NULL) {
		mod = PyObject_GetAttrString(callback, "__module__"); /* New reference. */
		if (mod != NULL) name = PyString_AsString(mod);
		if (name == NULL) {
			Py_XDECREF(mod);
			PyErr_SetString(PyExc_ValueError, "No module name specified and "
				"callback function does not have a \"__module__\" attribute.");
			return NULL;
		}
	}
	Py_INCREF(callback);
	Py_XINCREF(data);
	c = malloc(sizeof(*c));
	c->name = strdup(name);
	c->callback = callback;
	c->data = data;
	c->next = *list_head;
	*list_head = c;
	Py_XDECREF(mod);
	Py_RETURN_NONE;
}

static PyObject *cpy_register_config(PyObject *self, PyObject *args, PyObject *kwds) {
	return cpy_register_generic(&cpy_config_callbacks, args, kwds);
}

static PyObject *cpy_register_init(PyObject *self, PyObject *args, PyObject *kwds) {
	return cpy_register_generic(&cpy_init_callbacks, args, kwds);
}

typedef int reg_function_t(const char *name, void *callback, void *data);

static PyObject *cpy_register_generic_userdata(void *reg, void *handler, PyObject *args, PyObject *kwds) {
	char buf[512];
	reg_function_t *register_function = (reg_function_t *) reg;
	cpy_callback_t *c = NULL;
	user_data_t *user_data = NULL;
	const char *name = NULL;
	PyObject *callback = NULL, *data = NULL;
	static char *kwlist[] = {"callback", "data", "name", NULL};
	
	if (PyArg_ParseTupleAndKeywords(args, kwds, "O|Oz", kwlist, &callback, &data, &name) == 0) return NULL;
	if (PyCallable_Check(callback) == 0) {
		PyErr_SetString(PyExc_TypeError, "callback needs a be a callable object.");
		return NULL;
	}
	cpy_build_name(buf, sizeof(buf), callback, name);
	
	Py_INCREF(callback);
	Py_XINCREF(data);
	c = malloc(sizeof(*c));
	c->name = strdup(buf);
	c->callback = callback;
	c->data = data;
	c->next = NULL;
	user_data = malloc(sizeof(*user_data));
	user_data->free_func = cpy_destroy_user_data;
	user_data->data = c;
	register_function(buf, handler, user_data);
	return PyString_FromString(buf);
}

static PyObject *cpy_register_read(PyObject *self, PyObject *args, PyObject *kwds) {
	char buf[512];
	cpy_callback_t *c = NULL;
	user_data_t *user_data = NULL;
	double interval = 0;
	const char *name = NULL;
	PyObject *callback = NULL, *data = NULL;
	struct timespec ts;
	static char *kwlist[] = {"callback", "interval", "data", "name", NULL};
	
	if (PyArg_ParseTupleAndKeywords(args, kwds, "O|dOz", kwlist, &callback, &interval, &data, &name) == 0) return NULL;
	if (PyCallable_Check(callback) == 0) {
		PyErr_SetString(PyExc_TypeError, "callback needs a be a callable object.");
		return NULL;
	}
	cpy_build_name(buf, sizeof(buf), callback, name);
	
	Py_INCREF(callback);
	Py_XINCREF(data);
	c = malloc(sizeof(*c));
	c->name = strdup(buf);
	c->callback = callback;
	c->data = data;
	c->next = NULL;
	user_data = malloc(sizeof(*user_data));
	user_data->free_func = cpy_destroy_user_data;
	user_data->data = c;
	ts.tv_sec = interval;
	ts.tv_nsec = (interval - ts.tv_sec) * 1000000000;
	plugin_register_complex_read(buf, cpy_read_callback, &ts, user_data);
	return PyString_FromString(buf);
}

static PyObject *cpy_register_log(PyObject *self, PyObject *args, PyObject *kwds) {
	return cpy_register_generic_userdata(plugin_register_log, cpy_log_callback, args, kwds);
}

static PyObject *cpy_register_write(PyObject *self, PyObject *args, PyObject *kwds) {
	return cpy_register_generic_userdata(plugin_register_write, cpy_write_callback, args, kwds);
}

static PyObject *cpy_register_shutdown(PyObject *self, PyObject *args, PyObject *kwds) {
	return cpy_register_generic(&cpy_shutdown_callbacks, args, kwds);
}

static PyObject *cpy_Error(PyObject *self, PyObject *args) {
	const char *text;
	if (PyArg_ParseTuple(args, "s", &text) == 0) return NULL;
	Py_BEGIN_ALLOW_THREADS
	plugin_log(LOG_ERR, "%s", text);
	Py_END_ALLOW_THREADS
	Py_RETURN_NONE;
}

static PyObject *cpy_Warning(PyObject *self, PyObject *args) {
	const char *text;
	if (PyArg_ParseTuple(args, "s", &text) == 0) return NULL;
	Py_BEGIN_ALLOW_THREADS
	plugin_log(LOG_WARNING, "%s", text);
	Py_END_ALLOW_THREADS
	Py_RETURN_NONE;
}

static PyObject *cpy_Notice(PyObject *self, PyObject *args) {
	const char *text;
	if (PyArg_ParseTuple(args, "s", &text) == 0) return NULL;
	Py_BEGIN_ALLOW_THREADS
	plugin_log(LOG_NOTICE, "%s", text);
	Py_END_ALLOW_THREADS
	Py_RETURN_NONE;
}

static PyObject *cpy_Info(PyObject *self, PyObject *args) {
	const char *text;
	if (PyArg_ParseTuple(args, "s", &text) == 0) return NULL;
	Py_BEGIN_ALLOW_THREADS
	plugin_log(LOG_INFO, "%s", text);
	Py_END_ALLOW_THREADS
	Py_RETURN_NONE;
}

static PyObject *cpy_Debug(PyObject *self, PyObject *args) {
#ifdef COLLECT_DEBUG
	const char *text;
	if (PyArg_ParseTuple(args, "s", &text) == 0) return NULL;
	plugin_log(LOG_DEBUG, "%s", text);
#endif
	Py_RETURN_NONE;
}

static PyMethodDef cpy_methods[] = {
	{"Debug", cpy_Debug, METH_VARARGS, "This is an unhelpful text."},
	{"Info", cpy_Info, METH_VARARGS, "This is an unhelpful text."},
	{"Notice", cpy_Notice, METH_VARARGS, "This is an unhelpful text."},
	{"Warning", cpy_Warning, METH_VARARGS, "This is an unhelpful text."},
	{"Error", cpy_Error, METH_VARARGS, "This is an unhelpful text."},
	{"register_log", (PyCFunction) cpy_register_log, METH_VARARGS | METH_KEYWORDS, "This is an unhelpful text."},
	{"register_init", (PyCFunction) cpy_register_init, METH_VARARGS | METH_KEYWORDS, "This is an unhelpful text."},
	{"register_config", (PyCFunction) cpy_register_config, METH_VARARGS | METH_KEYWORDS, "This is an unhelpful text."},
	{"register_read", (PyCFunction) cpy_register_read, METH_VARARGS | METH_KEYWORDS, "This is an unhelpful text."},
	{"register_write", (PyCFunction) cpy_register_write, METH_VARARGS | METH_KEYWORDS, "This is an unhelpful text."},
	{"register_shutdown", (PyCFunction) cpy_register_shutdown, METH_VARARGS | METH_KEYWORDS, "This is an unhelpful text."},
	{0, 0, 0, 0}
};

static int cpy_shutdown(void) {
	cpy_callback_t *c;
	PyObject *ret;
	
	/* This can happen if the module was loaded but not configured. */
	if (state != NULL)
		PyEval_RestoreThread(state);

	for (c = cpy_shutdown_callbacks; c; c = c->next) {
		if (c->data == NULL)
			ret = PyObject_CallObject(c->callback, NULL); /* New reference. */
		else
			ret = PyObject_CallFunctionObjArgs(c->callback, c->data, (void *) 0); /* New reference. */
		if (ret == NULL)
			PyErr_Print(); /* FIXME */
		else
			Py_DECREF(ret);
	}
	Py_Finalize();
	return 0;
}

static int cpy_init(void) {
	cpy_callback_t *c;
	PyObject *ret;
	
	PyEval_InitThreads();
	/* Now it's finally OK to use python threads. */
	for (c = cpy_init_callbacks; c; c = c->next) {
		if (c->data == NULL)
			ret = PyObject_CallObject(c->callback, NULL); /* New reference. */
		else
			ret = PyObject_CallFunctionObjArgs(c->callback, c->data, (void *) 0); /* New reference. */
		if (ret == NULL)
			PyErr_Print(); /* FIXME */
		else
			Py_DECREF(ret);
	}
	state = PyEval_SaveThread();
	return 0;
}

static PyObject *cpy_oconfig_to_pyconfig(oconfig_item_t *ci, PyObject *parent) {
	int i;
	PyObject *item, *values, *children, *tmp;
	
	if (parent == NULL)
		parent = Py_None;
	
	values = PyTuple_New(ci->values_num); /* New reference. */
	for (i = 0; i < ci->values_num; ++i) {
		if (ci->values[i].type == OCONFIG_TYPE_STRING) {
			PyTuple_SET_ITEM(values, i, PyString_FromString(ci->values[i].value.string));
		} else if (ci->values[i].type == OCONFIG_TYPE_NUMBER) {
			PyTuple_SET_ITEM(values, i, PyFloat_FromDouble(ci->values[i].value.number));
		} else if (ci->values[i].type == OCONFIG_TYPE_BOOLEAN) {
			PyTuple_SET_ITEM(values, i, PyBool_FromLong(ci->values[i].value.boolean));
		}
	}
	
	item = PyObject_CallFunction((PyObject *) &ConfigType, "sONO", ci->key, parent, values, Py_None);
	if (item == NULL)
		return NULL;
	children = PyTuple_New(ci->children_num); /* New reference. */
	for (i = 0; i < ci->children_num; ++i) {
			PyTuple_SET_ITEM(children, i, cpy_oconfig_to_pyconfig(ci->children + i, item));
	}
	tmp = ((Config *) item)->children;
	((Config *) item)->children = children;
	Py_XDECREF(tmp);
	return item;
}

static int cpy_config(oconfig_item_t *ci) {
	int i;
	PyObject *sys;
	PyObject *sys_path;
	PyObject *module;
	
	/* Ok in theory we shouldn't do initialization at this point
	 * but we have to. In order to give python scripts a chance
	 * to register a config callback we need to be able to execute
	 * python code during the config callback so we have to start
	 * the interpreter here. */
	/* Do *not* use the python "thread" module at this point! */
	Py_Initialize();
	
	PyType_Ready(&ConfigType);
	PyType_Ready(&ValuesType);
	sys = PyImport_ImportModule("sys"); /* New reference. */
	if (sys == NULL) {
		ERROR("python module: Unable to import \"sys\" module.");
		/* Just print the default python exception text to stderr. */
		PyErr_Print();
		return 1;
	}
	sys_path = PyObject_GetAttrString(sys, "path"); /* New reference. */
	Py_DECREF(sys);
	if (sys_path == NULL) {
		ERROR("python module: Unable to read \"sys.path\".");
		PyErr_Print();
		return 1;
	}
	module = Py_InitModule("collectd", cpy_methods); /* Borrowed reference. */
	PyModule_AddObject(module, "Config", (PyObject *) &ConfigType); /* Steals a reference. */
	PyModule_AddObject(module, "Values", (PyObject *) &ValuesType); /* Steals a reference. */
	PyModule_AddIntConstant(module, "LOG_DEBUG", LOG_DEBUG);
	PyModule_AddIntConstant(module, "LOG_INFO", LOG_INFO);
	PyModule_AddIntConstant(module, "LOG_NOTICE", LOG_NOTICE);
	PyModule_AddIntConstant(module, "LOG_WARNING", LOG_WARNING);
	PyModule_AddIntConstant(module, "LOG_ERROR", LOG_ERR);
	for (i = 0; i < ci->children_num; ++i) {
		oconfig_item_t *item = ci->children + i;
		
		if (strcasecmp(item->key, "ModulePath") == 0) {
			char *dir = NULL;
			PyObject *dir_object;
			
			if (cf_util_get_string(item, &dir) != 0) 
				continue;
			dir_object = PyString_FromString(dir); /* New reference. */
			if (dir_object == NULL) {
				ERROR("python plugin: Unable to convert \"%s\" to "
				      "a python object.", dir);
				free(dir);
				PyErr_Print();
				continue;
			}
			if (PyList_Append(sys_path, dir_object) != 0) {
				ERROR("python plugin: Unable to append \"%s\" to "
				      "python module path.", dir);
				PyErr_Print();
			}
			Py_DECREF(dir_object);
			free(dir);
		} else if (strcasecmp(item->key, "Import") == 0) {
			char *module_name = NULL;
			PyObject *module;
			
			if (cf_util_get_string(item, &module_name) != 0) 
				continue;
			module = PyImport_ImportModule(module_name); /* New reference. */
			if (module == NULL) {
				ERROR("python plugin: Error importing module \"%s\".", module_name);
				PyErr_Print();
			}
			free(module_name);
			Py_XDECREF(module);
		} else if (strcasecmp(item->key, "Module") == 0) {
			char *name = NULL;
			cpy_callback_t *c;
			PyObject *ret;
			
			if (cf_util_get_string(item, &name) != 0)
				continue;
			for (c = cpy_config_callbacks; c; c = c->next) {
				if (strcasecmp(c->name, name) == 0)
					break;
			}
			if (c == NULL) {
				WARNING("python plugin: Found a configuration for the \"%s\" plugin, "
					"but the plugin isn't loaded or didn't register "
					"a configuration callback.", name);
				free(name);
				continue;
			}
			free(name);
			if (c->data == NULL)
				ret = PyObject_CallFunction(c->callback, "N",
					cpy_oconfig_to_pyconfig(item, NULL)); /* New reference. */
			else
				ret = PyObject_CallFunction(c->callback, "NO",
					cpy_oconfig_to_pyconfig(item, NULL), c->data); /* New reference. */
			if (ret == NULL)
				PyErr_Print();
			else
				Py_DECREF(ret);
		} else {
			WARNING("python plugin: Ignoring unknown config key \"%s\".", item->key);
		}
	}
	Py_DECREF(sys_path);
	return 0;
}

void module_register(void) {
	plugin_register_complex_config("python", cpy_config);
	plugin_register_init("python", cpy_init);
//	plugin_register_read("python", cna_read);
	plugin_register_shutdown("python", cpy_shutdown);
}
