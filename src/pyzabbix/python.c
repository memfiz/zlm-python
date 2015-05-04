
/*
 ** Vladimir Ulogov (c) 2015. ver 0.3 (no-context)
 ** Use it at your own risk
*/


#include "settings.h"


#include "sysinc.h"
#include "common.h"
#include "module.h"
#include "cfg.h"
#include "log.h"
#include <time.h>
#include <dlfcn.h>
#include <Python.h>
#include <frameobject.h>

#define BUFSIZE 4096

extern char *CONFIG_LOAD_MODULE_PATH;
char        *modpath;
char        *lib_path = NULL;
char        *python_path = NULL;
/* the variable keeps timeout setting for item processing */
static int	item_timeout = 0;

int     zbx_python_call_module(char* modname, AGENT_REQUEST *request, AGENT_RESULT *result, int pass_cmd);
int	zbx_module_python_ping(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_python_version(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_python_call(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_python_call_prof(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_python_call_wrap(AGENT_REQUEST *request, AGENT_RESULT *result);
int     zbx_set_return_value(AGENT_RESULT *result, PyObject *retvalue);

static ZBX_METRIC keys[] =
/*      KEY                     FLAG		FUNCTION        	TEST PARAMETERS */
{
   {"python.ping",	0,		zbx_module_python_ping,	NULL},
   {"python.version",	0,		zbx_module_python_version, NULL},
   {"py",  		CF_HAVEPARAMS,	zbx_module_python_call_wrap, ""},
   {NULL}
};


/* Read Python enviroment from configuration file */

void load_python_env_config(void)  {
   char conf_file[BUFSIZE];
   static struct cfg_line cfg[] = 
     {
	{ "PYTHONPATH", &python_path, TYPE_STRING, PARM_MAND, 0, 0 },
	{ "PYTHONLIB", &lib_path, TYPE_STRING, PARM_MAND, 0, 0 },
	{ NULL },
     };
   zbx_snprintf(conf_file, BUFSIZE, "%s/python.cfg", CONFIG_LOAD_MODULE_PATH);
   parse_cfg_file(conf_file, cfg, ZBX_CFG_FILE_OPTIONAL, ZBX_CFG_STRICT);
   printf("!!! %s %s %s\n", conf_file, lib_path, python_path); 
}





/******************************************************************************
 *                                                                            *
 * Function: zbx_python_call_module                                           *
 *                                                                            *
 * Purpose: importing module and calling it's "main()" function               *
 *                                                                            *
 * Return value: Return value from "main()" as a string                       *
 *                                                                            *
 ******************************************************************************/


int zbx_set_return_value(AGENT_RESULT *result, PyObject *ret) {
   char *cstrret;
   if (PyInt_Check(ret)) {
      SET_UI64_RESULT(result, PyLong_AsLong(ret));
   } else if (PyLong_Check(ret)) {
      SET_UI64_RESULT(result, PyLong_AsLong(ret));
   } else if (PyFloat_Check(ret)) {	   
      SET_DBL_RESULT(result, PyFloat_AsDouble(ret));
   } else if (PyString_Check(ret)) {
      PyArg_Parse(ret, "s", &cstrret);
      SET_STR_RESULT(result, strdup(cstrret));
   } else {
      SET_MSG_RESULT(result, strdup("Python returned unsupported value"));
      return 0;
   }
   return 1;
}


int zbx_python_call_module(char* modname, AGENT_REQUEST *request, AGENT_RESULT *result, int pass_cmd)  {
   int i, retvalue_ret_code;
   PyObject* mod, *param, *fun, *ret;

   mod = PyImport_ImportModule(modname);
   
   if (mod == NULL) {
      SET_MSG_RESULT(result, strdup("Error importing of Python module"));
      PyErr_Print();
      return 0;
   }
   if (pass_cmd == 0)   {
      param = PyTuple_New(request->nparam);
      for (i=1;i<request->nparam;i++) {
	    PyTuple_SET_ITEM(param,i,PyString_FromString(get_rparam(request, i)));
      }
   } else {
      param = PyTuple_New(request->nparam+1);
      PyTuple_SET_ITEM(param, 0, PyString_FromString(get_rparam(request, 0)));
      for (i=0;i<request->nparam;i++) {
	    PyTuple_SET_ITEM(param,i+1,PyString_FromString(get_rparam(request, i)));
      }
   }

   fun = PyObject_GetAttrString(mod, "main");
   if (fun == NULL) {
      SET_MSG_RESULT(result, strdup("Python module does not have a main() function"));
      return 0;
   }
   
   ret = PyEval_CallObject(fun, param);
   
   if (ret == NULL) {
      SET_MSG_RESULT(result, strdup("Python code threw a traceback"));
      PyErr_Print();
      return 0;
   }
   
   Py_DECREF(mod);
   Py_DECREF(fun);
   
   if (PyTuple_Check(ret) && PyTuple_Size(ret) == 3) {
      long wrapper_ret_code;
      wrapper_ret_code = PyLong_AsLong(PyTuple_GetItem(ret, 0));
      if ( wrapper_ret_code == 0 ) {
	    retvalue_ret_code = 0;
	    SET_MSG_RESULT(result, strdup(PyString_AsString(PyTuple_GetItem(ret, 2))));
      } else {
	    retvalue_ret_code = zbx_set_return_value(result, PyTuple_GetItem(ret, 1));
      }
   } else {	
      retvalue_ret_code = zbx_set_return_value(result, ret);
   }
   Py_DECREF(ret);
   return retvalue_ret_code;
}


/******************************************************************************
 *                                                                            *
 * Function: zbx_module_api_version                                           *
 *                                                                            *
 * Purpose: returns version number of the module interface                    *
 *                                                                            *
 * Return value: ZBX_MODULE_API_VERSION_ONE - the only version supported by   *
 *               Zabbix currently                                             *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_api_version()
{
	return ZBX_MODULE_API_VERSION_ONE;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_item_timeout                                          *
 *                                                                            *
 * Purpose: set timeout value for processing of items                         *
 *                                                                            *
 * Parameters: timeout - timeout in seconds, 0 - no timeout set               *
 *                                                                            *
 ******************************************************************************/
void	zbx_module_item_timeout(int timeout)
{
	item_timeout = timeout;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_item_list                                             *
 *                                                                            *
 * Purpose: returns list of item keys supported by the module                 *
 *                                                                            *
 * Return value: list of item keys                                            *
 *                                                                            *
 ******************************************************************************/
ZBX_METRIC	*zbx_module_item_list()
{
   return keys;
}

int	zbx_module_python_ping(AGENT_REQUEST *request, AGENT_RESULT *result)
{
   PyGILState_STATE gstate;
   gstate = PyGILState_Ensure();
   if (Py_IsInitialized() != 0)
     SET_UI64_RESULT(result, 1);
   else
     SET_UI64_RESULT(result, 0);
   PyGILState_Release(gstate);
   return SYSINFO_RET_OK;
}

int	zbx_module_python_version(AGENT_REQUEST *request, AGENT_RESULT *result)
{
   
   PyGILState_STATE gstate;
   gstate = PyGILState_Ensure();
   SET_STR_RESULT(result, strdup(Py_GetVersion()));
   PyGILState_Release(gstate);
   return SYSINFO_RET_OK;
}


int	zbx_module_python_call_wrap(AGENT_REQUEST *request, AGENT_RESULT *result)
{
   int ret_code;
 
   PyGILState_STATE gstate;
   

   if ((request->nparam < 1) || (strlen(get_rparam(request, 0)) == 0))   {
      SET_MSG_RESULT(result, strdup("Invalid number of parameters."));
      return SYSINFO_RET_FAIL;
   }

   gstate = PyGILState_Ensure();
   ret_code = zbx_python_call_module("ZBX_call", request, result, 1);
   PyGILState_Release(gstate);

   if (ret_code == 1) {
      return SYSINFO_RET_OK;
   }
   return SYSINFO_RET_FAIL;
}



/******************************************************************************
 *                                                                            *
 * Function: zbx_module_init                                                  *
 *                                                                            *
 * Purpose: the function is called on agent startup                           *
 *          It should be used to call any initialization routines             *
 *                                                                            *
 * Return value: ZBX_MODULE_OK - success                                      *
 *               ZBX_MODULE_FAIL - module initialization failed               *
 *                                                                            *
 * Comment: the module won't be loaded in case of ZBX_MODULE_FAIL             *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_init() {
   PyObject *mod, *param, *fun;
   char error[MAX_STRING_LEN];
   char cmd[BUFSIZE];
   char *pythonpath;
   #if HAVE_SECURE_GETENV==1
   pythonpath=secure_getenv("PYTHONPATH");
   #else
   pythonpath=getenv("PYTHONPATH");
   #endif

   load_python_env_config();
   if (lib_path == NULL)
     return ZBX_MODULE_FAIL;
   
   if (pythonpath == NULL) {
      if (python_path == NULL) {
	    return ZBX_MODULE_FAIL;
      }
      modpath=malloc(strlen(CONFIG_LOAD_MODULE_PATH)+4096);
      zbx_snprintf(modpath, 4096, "%s/pymodules:%s/pymodules/lib:%s", CONFIG_LOAD_MODULE_PATH, CONFIG_LOAD_MODULE_PATH,python_path);
      printf("*** %s\n", modpath);
      free(python_path);
   } else  {
      modpath=malloc(strlen(CONFIG_LOAD_MODULE_PATH)+strlen(pythonpath)+4096);
      zbx_snprintf(modpath, 4096, "%s/pymodules:%s/pymodules/lib:%s", CONFIG_LOAD_MODULE_PATH, CONFIG_LOAD_MODULE_PATH, pythonpath);
   } 


   if (dlopen(lib_path, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL) == NULL)   {
      free(lib_path);
      return ZBX_MODULE_FAIL;
   }
   
   /* We do not need those anymore */
   free(lib_path);
   
   /* Set-up Python environment */
   Py_SetProgramName("zabbix_agentd");
   Py_Initialize();
   if (Py_IsInitialized() == 0) {
      /* Python initialization had failed */
      return ZBX_MODULE_FAIL;
   }
   PySys_SetPath(modpath);
   /* printf("1\n"); */
   
   zabbix_log(LOG_LEVEL_WARNING, "Executing ZBX_startup");
   if ((mod = PyImport_ImportModule("ZBX_startup"))!=NULL) {
      zabbix_log(LOG_LEVEL_WARNING, "mod: %x", mod); 
      fun = PyObject_GetAttrString(mod, "main");
      if (fun == NULL) 	{
	    return ZBX_MODULE_FAIL;
      }
      zabbix_log(LOG_LEVEL_WARNING, "FUN: %x", fun);
      PyEval_CallObject(fun, Py_BuildValue("(s)", CONFIG_LOAD_MODULE_PATH));
   }
   /* printf("init fin\n"); */
   return ZBX_MODULE_OK;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_uninit                                                *
 *                                                                            *
 * Purpose: the function is called on agent shutdown                          *
 *          It should be used to cleanup used resources if there are any      *
 *                                                                            *
 * Return value: ZBX_MODULE_OK - success                                      *
 *               ZBX_MODULE_FAIL - function failed                            *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_uninit() {
   PyObject *mod, *param, *fun;
   int ret_code;
   if ((mod = PyImport_ImportModule("ZBX_finish"))!=NULL) {
      fun = PyObject_GetAttrString(mod, "main");
      if (fun != 0) {
	    param = PyTuple_New(0);
	    if (param != NULL)   {
	        PyEval_CallObject(fun, param);
	        Py_DECREF(param);
	    }
	    Py_DECREF(fun);
      }
      Py_DECREF(mod);
      Py_Finalize();
      free(modpath);
      ret_code = ZBX_MODULE_OK;
   } else {
      free(modpath);
      ret_code = ZBX_MODULE_FAIL;
   }
   return ret_code;
}
