/*
 * cups - Python bindings for CUPS
 * Copyright (C) 2002-2020  Red Hat, Inc
 * Authors: Tim Waugh <twaugh@redhat.com>
 *          Zdenek Dohnal <zdohnal@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdarg.h>
#include <Python.h>
#include <cups/cups.h>
#include <cups/language.h>

#include "cupsmodule.h"

#include <locale.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>

#include "cupsconnection.h"
#include "cupsppd.h"
#include "cupsipp.h"

static pthread_key_t tls_key = -1;
static pthread_once_t tls_key_once = PTHREAD_ONCE_INIT;

# define CUPS_SERVER_REMOTE_PRINTERS	"_remote_printers"

//////////////////////
// Worker functions //
//////////////////////

/*
 * Thread local storage functions
 */

static void
destroy_TLS (void *value)
{
  struct TLS *tls = (struct TLS *) value;

  /* CPython API needs to be run only when our process holds
   * Python global interpret lock */
  PyGILState_STATE gilstate;
  gilstate = PyGILState_Ensure();

  Py_XDECREF (tls->cups_password_callback);

  Py_XDECREF (tls->cups_password_callback_context);

  // release global interpret lock
  PyGILState_Release(gilstate);

  free (value);
}

static void
init_TLS (void)
{
  pthread_key_create (&tls_key, destroy_TLS);
}

struct TLS *
get_TLS (void)
{
  struct TLS *tls;
  pthread_once (&tls_key_once, init_TLS);
  tls = (struct TLS *) pthread_getspecific (tls_key);
  if (tls == NULL)
    {
      tls = calloc (1, sizeof (struct TLS));
      pthread_setspecific (tls_key, tls);
    }

  return tls;
}

static int
do_model_compare (const wchar_t *a, const wchar_t *b)
{
  const wchar_t *digits = L"0123456789";
  wchar_t quick_a, quick_b;
  while ((quick_a = *a) != L'\0' && (quick_b = *b) != L'\0') {
    int end_a = wcsspn (a, digits);
    int end_b = wcsspn (b, digits);
    int min;
    int a_is_digit = 1;
    int cmp;

    if (quick_a != quick_b && !iswdigit (quick_a) && !iswdigit (quick_b)) {
      if (quick_a < quick_b) return -1;
      else return 1;
    }

    if (!end_a) {
      end_a = wcscspn (a, digits);
      a_is_digit = 0;
    }

    if (!end_b) {
      if (a_is_digit)
	return -1;
      end_b = wcscspn (b, digits);
    } else if (!a_is_digit)
      return 1;

    if (a_is_digit) {
      unsigned long n_a, n_b;
      n_a = wcstoul (a, NULL, 10);
      n_b = wcstoul (b, NULL, 10);
      if (n_a < n_b) cmp = -1;
      else if (n_a == n_b) cmp = 0;
      else cmp = 1;
    } else {
      min = end_a < end_b ? end_a : end_b;
      cmp = wcsncmp (a, b, min);
    }

    if (!cmp) {
      if (end_a != end_b)
	return end_a < end_b ? -1 : 1;

      a += end_a;
      b += end_b;
      continue;
    }

    return cmp;
  }

  if (quick_a == L'\0') {
    if (*b == L'\0')
      return 0;

    return -1;
  }

  return 1;
}

//////////////////////////
// Module-level methods //
//////////////////////////

static PyObject *
cups_modelSort (PyObject *self, PyObject *args)
{
  PyObject *Oa, *Ob, *a, *b;
  long unsigned int len_a, len_b;
  size_t size_a, size_b;
  wchar_t *wca, *wcb;

  if (!PyArg_ParseTuple (args, "OO", &Oa, &Ob))
    return NULL;

  a = PyUnicode_FromObject (Oa);
  b = PyUnicode_FromObject (Ob);
  if (a == NULL || b == NULL || !PyUnicode_Check (a) || !PyUnicode_Check (b)) {
    if (a) {
      Py_DECREF (a);
    }
    if (b) {
      Py_DECREF (b);
    }

    PyErr_SetString (PyExc_TypeError, "Unable to convert to Unicode");
    return NULL;
  }

  len_a = 1 + (long unsigned)PyUnicode_GetLength (a);
  size_a = len_a * sizeof (wchar_t);
  if ((size_a / sizeof (wchar_t)) != len_a) {
    Py_DECREF (a);
    Py_DECREF (b);
    PyErr_SetString (PyExc_RuntimeError, "String too long");
    return NULL;
  }

  len_b = 1 + (long unsigned)PyUnicode_GetLength (b);
  size_b = len_b * sizeof (wchar_t);
  if ((size_b / sizeof (wchar_t)) != len_b) {
    Py_DECREF (a);
    Py_DECREF (b);
    PyErr_SetString (PyExc_RuntimeError, "String too long");
    return NULL;
  }

  wca = malloc (size_a);
  wcb = malloc (size_b);
  if (wca == NULL || wcb == NULL) {
    Py_DECREF (a);
    Py_DECREF (b);
    free (wca);
    free (wcb);
    PyErr_SetString (PyExc_RuntimeError, "Insufficient memory");
    return NULL;
  }
  PyUnicode_AsWideChar (a, wca, size_a);
  PyUnicode_AsWideChar (b, wcb, size_b);
  Py_DECREF (a);
  Py_DECREF (b);
  return Py_BuildValue ("i", do_model_compare (wca, wcb));
}

static PyObject *
cups_setUser (PyObject *self, PyObject *args)
{
  PyObject *userobj;
  char *user;

  if (!PyArg_ParseTuple (args, "O", &userobj))
    return NULL;

  if (UTF8_from_PyObj (&user, userobj) == NULL)
    return NULL;

  cupsSetUser (user);
  free (user);
  Py_RETURN_NONE;
}

static PyObject *
cups_setServer (PyObject *self, PyObject *args)
{
  PyObject *serverobj;
  char *server;

  if (!PyArg_ParseTuple (args, "O", &serverobj))
    return NULL;

  if (UTF8_from_PyObj (&server, serverobj) == NULL)
    return NULL;

  cupsSetServer (server);
  free (server);
  Py_RETURN_NONE;
}

static PyObject *
cups_setPort (PyObject *self, PyObject *args)
{
  int port;

  if (!PyArg_ParseTuple (args, "i", &port))
    return NULL;

  ippSetPort (port);
  Py_RETURN_NONE;
}

static PyObject *
cups_setEncryption (PyObject *self, PyObject *args)
{
  int e;
  if (!PyArg_ParseTuple (args, "i", &e))
    return NULL;

  cupsSetEncryption (e);
  Py_RETURN_NONE;
}

static PyObject *
cups_getUser (PyObject *self)
{
  return PyUnicode_FromString (cupsUser ());
}

static PyObject *
cups_getServer (PyObject *self)
{
  return PyUnicode_FromString (cupsServer ());
}

static PyObject *
cups_getPort (PyObject *self)
{
  return Py_BuildValue ("i", ippPort ());
}

static PyObject *
cups_getEncryption (PyObject *self)
{
  return Py_BuildValue ("i", cupsEncryption ());
}

static PyObject *
cups_setPasswordCB (PyObject *self, PyObject *args)
{
  struct TLS *tls = get_TLS ();
  PyObject *cb;

  if (!PyArg_ParseTuple (args, "O:cups_setPasswordCB", &cb))
    return NULL;

  if (!PyCallable_Check (cb)) {
    PyErr_SetString (PyExc_TypeError, "Parameter must be callable");
    return NULL;
  }

  debugprintf ("-> cups_setPasswordCB\n");
  Py_XDECREF (tls->cups_password_callback_context);
  tls->cups_password_callback_context = NULL;

  Py_XINCREF (cb);
  Py_XDECREF (tls->cups_password_callback);
  tls->cups_password_callback = cb;

  cupsSetPasswordCB2 (password_callback_oldstyle, NULL);

  debugprintf ("<- cups_setPasswordCB\n");
  Py_RETURN_NONE;
}

static PyObject *
cups_setPasswordCB2 (PyObject *self, PyObject *args)
{
  struct TLS *tls = get_TLS ();
  PyObject *cb;
  PyObject *cb_context = NULL;

  if (!PyArg_ParseTuple (args, "O|O", &cb, &cb_context))
    return NULL;

  if (cb == Py_None && cb_context != NULL) {
    PyErr_SetString (PyExc_TypeError, "Default callback takes no context");
    return NULL;
  }
  else if (cb != Py_None && !PyCallable_Check (cb)) {
    PyErr_SetString (PyExc_TypeError, "Parameter must be callable");
    return NULL;
  }

  debugprintf ("-> cups_setPasswordCB2\n");

  Py_XINCREF (cb_context);
  Py_XDECREF (tls->cups_password_callback_context);
  tls->cups_password_callback_context = cb_context;

  if (cb == Py_None)
  {
    Py_XDECREF (tls->cups_password_callback);
    tls->cups_password_callback = NULL;
    cupsSetPasswordCB2 (NULL, NULL);
  }
  else
  {
    Py_XINCREF (cb);
    Py_XDECREF (tls->cups_password_callback);
    tls->cups_password_callback = cb;
    cupsSetPasswordCB2 (password_callback_newstyle, cb_context);
  }

  debugprintf ("<- cups_setPasswordCB2\n");
  Py_RETURN_NONE;
}

static PyObject *
cups_ppdSetConformance (PyObject *self, PyObject *args)
{
  int level;
  if (!PyArg_ParseTuple (args, "i", &level))
    return NULL;

  ppdSetConformance (level);
  Py_RETURN_NONE;
}

static PyObject *
cups_enumDests (PyObject *self, PyObject *args, PyObject *kwds)
{
  PyObject *cb;
  int flags = 0;
  int msec = -1;
  int type = 0;
  int mask = 0;
  PyObject *user_data = NULL;
  CallbackContext context;
  int ret;
  static char *kwlist[] = { "cb",
			    "flags",
			    "msec",
			    "type",
			    "mask",
			    "user_data",
			    NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|iiiiO", kwlist,
				    &cb,
				    &flags,
				    &msec,
				    &type,
				    &mask,
				    &user_data))
    return NULL;

  if (!PyCallable_Check (cb)) {
    PyErr_SetString (PyExc_TypeError, "cb must be callable");
    return NULL;
  }

  if (!user_data)
    user_data = Py_None;

  Py_XINCREF (cb);
  Py_XINCREF (user_data);
  context.cb = cb;
  context.user_data = user_data;
  ret = cupsEnumDests (flags,
		       msec,
		       NULL,
		       type,
		       mask,
		       cups_dest_cb,
		       &context);
  Py_XDECREF (cb);
  Py_XDECREF (user_data);

  if (!ret) {
    PyErr_SetString (PyExc_RuntimeError, "cupsEnumDests failed");
    return NULL;
  }

  Py_RETURN_NONE;
}

static PyObject *
cups_connectDest (PyObject *self, PyObject *args, PyObject *kwds)
{
  PyObject *destobj;
  PyObject *cb;
  int flags = 0;
  int msec = -1;
  PyObject *user_data = NULL;
  CallbackContext context;
  char resource[HTTP_MAX_URI];
  http_t *conn;
  Connection *connobj;
  Dest *dest_o;
  cups_dest_t dest;
  PyObject *ret;
  static char *kwlist[] = { "dest",
			    "cb",
			    "flags",
			    "msec",
			    "user_data",
			    NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "OO|iiO", kwlist,
				    &destobj,
				    &cb,
				    &flags,
				    &msec,
				    &user_data))
    return NULL;

  if (Py_TYPE(destobj) != &cups_DestType) {
    PyErr_SetString (PyExc_TypeError, "dest must be Dest object");
    return NULL;
  }

  if (!PyCallable_Check (cb)) {
    PyErr_SetString (PyExc_TypeError, "cb must be callable");
    return NULL;
  }

  if (!user_data)
    user_data = Py_None;

  Py_XINCREF (cb);
  Py_XINCREF (user_data);
  context.cb = cb;
  context.user_data = user_data;
  resource[0] = '\0';

  dest_o = (Dest *) destobj;
  dest.is_default = dest_o->is_default;
  dest.name = dest_o->destname;
  dest.instance = dest_o->instance;
  dest.num_options = dest_o->num_options;
  dest.options = malloc (dest_o->num_options * sizeof (dest.options[0]));
  int i;
  for (i = 0; i < dest_o->num_options; i++) {
    dest.options[i].name = dest_o->name[i];
    dest.options[i].value = dest_o->value[i];
  }

  conn = cupsConnectDest (&dest,
			  flags,
			  msec,
			  NULL,
			  resource,
			  sizeof (resource),
			  cups_dest_cb,
			  &context);
  Py_XDECREF (cb);
  Py_XDECREF (user_data);
  free (dest.options);

  if (!conn) {
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    return NULL;
  }

  PyObject *largs = Py_BuildValue ("()");
  PyObject *lkwlist = Py_BuildValue ("{}");
  connobj = (Connection *) PyType_GenericNew (&cups_ConnectionType,
					      largs, lkwlist);
  Py_DECREF (largs);
  Py_DECREF (lkwlist);
  connobj->host = strdup ("");
  connobj->http = conn;

  ret = Py_BuildValue ("(Os)", (PyObject *) connobj, resource);
  return ret;
}

static PyObject *
cups_ippErrorString (PyObject *self, PyObject *args)
{
  int op;

  if (!PyArg_ParseTuple (args, "i", &op))
    return NULL;

  return PyUnicode_FromString (ippErrorString (op));
}

static PyObject *
cups_ippOpString (PyObject *self, PyObject *args)
{
  int op;

  if (!PyArg_ParseTuple (args, "i", &op))
    return NULL;

  return PyUnicode_FromString (ippOpString (op));
}

static PyObject *
cups_require (PyObject *self, PyObject *args)
{
  const char *version = VERSION;
  const char *required;
  const char *pver, *preq;
  char *end;
  unsigned long nreq, nver;

  if (!PyArg_ParseTuple (args, "s", &required))
    return NULL;

  pver = version;
  preq = required;
  nreq = strtoul (preq, &end, 0);
  while (preq != end)
  {
    preq = end;
    if (*preq == '.')
      preq++;

    nver = strtoul (pver, &end, 0);
    if (pver == end)
      goto fail;
    else {
      pver = end;
      if (*pver == '.')
	pver++;
    }

    if (nver < nreq)
      goto fail;

    nreq = strtoul (preq, &end, 0);
  }

  Py_RETURN_NONE;
fail:
  PyErr_SetString (PyExc_RuntimeError, "I am version " VERSION);
  return NULL;
}

struct module_state {
    PyObject *error;
};

#define GETSTATE(m) ((struct module_state*)PyModule_GetState(m))


static PyMethodDef cups_methods[] = {
  { "modelSort", cups_modelSort, METH_VARARGS,
    "modelSort(s1,s2) -> integer\n\n"
    "Sort two model strings.\n\n"
    "@type s1: string\n"
    "@param s1: first string\n"
    "@type s2: string\n"
    "@param s2: second string\n"
    "@return: strcmp-style comparison result"},

  { "setUser", cups_setUser, METH_VARARGS,
    "setUser(user) -> None\n\n"
    "Set user to connect as.\n\n"
    "@type user: string\n"
    "@param user: username"},

  { "setServer", cups_setServer, METH_VARARGS,
    "setServer(server) -> None\n\n"
    "Set server to connect to.\n\n"
    "@type server: string\n"
    "@param server: server hostname" },

  { "setPort", cups_setPort, METH_VARARGS,
    "setPort(port) -> None\n\n"
    "Set IPP port to connect to.\n\n"
    "@type port: integer\n"
    "@param port: IPP port" },

  { "setEncryption", cups_setEncryption, METH_VARARGS,
    "setEncryption(policy) -> None\n\n"
    "Set encryption policy.\n\n"
    "@type policy: integer\n"
    "@param policy: L{HTTP_ENCRYPT_ALWAYS}, L{HTTP_ENCRYPT_IF_REQUESTED}, \n"
    "L{HTTP_ENCRYPT_NEVER}, or L{HTTP_ENCRYPT_REQUIRED}" },

  { "getUser", (PyCFunction) cups_getUser, METH_NOARGS,
    "getUser() -> string\n\n"
    "@return: user to connect as." },

  { "getServer", (PyCFunction) cups_getServer, METH_NOARGS,
    "getServer() -> string\n\n"
    "@return: server to connect to." },

  { "getPort", (PyCFunction) cups_getPort, METH_NOARGS,
    "getPort() -> integer\n\n"
    "@return: IPP port to connect to." },

  { "getEncryption", (PyCFunction) cups_getEncryption, METH_NOARGS,
    "getEncryption() -> integer\n\n"
    "Get encryption policy.\n"
    "@see: L{setEncryption}" },

  { "setPasswordCB", cups_setPasswordCB, METH_VARARGS,
    "setPasswordCB(fn) -> None\n\n"
    "Set password callback function.  This Python function will be called \n"
    "when a password is required.  It must take one string parameter \n"
    "(the password prompt) and it must return a string (the password), or \n"
    "None to abort the operation.\n\n"
    "@type fn: callable object\n"
    "@param fn: callback function" },

  { "setPasswordCB2", cups_setPasswordCB2, METH_VARARGS,
    "setPasswordCB2(fn, context=None) -> None\n\n"
    "Set password callback function.  This Python function will be called \n"
    "when a password is required.  It must take parameters of type string \n"
    "(the password prompt), instance (the cups.Connection), string (the \n"
    "HTTP method), string (the HTTP resource) and, optionally, the user-\n"
    "supplied context.  It must return a string (the password), or None \n"
    "to abort the operation.\n\n"
    "@type fn: callable object, or None for default handler\n"
    "@param fn: callback function" },

  { "ppdSetConformance", cups_ppdSetConformance, METH_VARARGS,
    "ppdSetConformance(level) -> None\n\n"
    "Set PPD conformance level.\n\n"
    "@type level: integer\n"
    "@param level: PPD_CONFORM_RELAXED or PPD_CONFORM_STRICT" },

  { "enumDests",
    (PyCFunction) cups_enumDests, METH_VARARGS | METH_KEYWORDS,
    "enumDests(cb,flags=0,msec=-1,type=0,mask=0,user_data=None) -> None\n\n"
    "@type cb: callable\n"
    "@param cb: callback function, given user_data, dest flags, and dest.\n"
    "Should return 1 to continue enumeration and 0 to cancel.\n"
    "@type flags: integer\n"
    "@param flags: enumeration flags\n"
    "@type msec: integer\n"
    "@param msec: timeout, or -1 for no timeout\n"
    "@type type: integer\n"
    "@param type: bitmask of printer types to return\n"
    "@type mask: integer\n"
    "@param mask: bitmask of type bits to examine\n"
    "@type user_data: object\n"
    "@param user_data: user data to pass to callback function\n"},

  { "connectDest",
    (PyCFunction) cups_connectDest, METH_VARARGS | METH_KEYWORDS,
    "connectDest(dest,cb,flags=0,msec=-1,user_data=None) -> (conn, resource)\n\n"
    "@type dest: Dest object\n"
    "@param dest: destination to connect to\n"
    "@type cb: callable\n"
    "@param cb: callback function, given user_data, dest flags, and dest.\n"
    "Should return 1 to continue enumeration and 0 to cancel.\n"
    "@type flags: integer\n"
    "@param flags: enumeration flags\n"
    "@type msec: integer\n"
    "@param msec: timeout, or -1 for no timeout\n"
    "@type user_data: object\n"
    "@param user_data: user data to pass to callback function\n"
    "@return: a 2-tuple of the Connection object and the HTTP resource.\n"},

  { "ippErrorString",
    (PyCFunction) cups_ippErrorString, METH_VARARGS,
    "ippErrorString(statuscode) -> name\n\n"
    "@type statuscode: integer\n"
    "@param statuscode: IPP Request status code\n"
    "@return: a string describing the status code\n"},

  { "ippOpString",
    (PyCFunction) cups_ippOpString, METH_VARARGS,
    "ippOpString(op) -> name\n\n"
    "@type op: integer\n"
    "@param op: IPP Request operation\n"
    "@return: a string representing the operation name\n"},

  { "require", cups_require, METH_VARARGS,
    "require(version) -> None\n\n"
    "Require pycups version.\n\n"
    "@type version: string\n"
    "@param version: minimum pycups version required\n"
    "@raise RuntimeError: requirement not met" },  

  { NULL, NULL, 0, NULL }
};

static int cups_traverse(PyObject *m, visitproc visit, void *arg) {
    Py_VISIT(GETSTATE(m)->error);
    return 0;
}

static int cups_clear(PyObject *m) {
    Py_CLEAR(GETSTATE(m)->error);
    return 0;
}


static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        "cups",
        NULL,
        sizeof(struct module_state),
        cups_methods,
        NULL,
        cups_traverse,
        cups_clear,
        NULL
};

#define INITERROR return NULL

PyObject *
PyInit_cups(void)
{
    PyObject *m = PyModule_Create(&moduledef);

  if (m == NULL)
    INITERROR;
  struct module_state *st = GETSTATE(m);

  st->error = PyErr_NewException("cups.Error", NULL, NULL);
  if (st->error == NULL) {
    Py_DECREF(m);
    INITERROR;
  }

  PyObject *d = PyModule_GetDict (m);
  PyObject *obj;

  // Connection type
  cups_ConnectionType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_ConnectionType) < 0)
    INITERROR;

  PyModule_AddObject (m, "Connection",
		      (PyObject *)&cups_ConnectionType);

  // PPD type
  cups_PPDType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_PPDType) < 0)
    INITERROR;

  PyModule_AddObject (m, "PPD",
		      (PyObject *)&cups_PPDType);

  // Option type
  cups_OptionType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_OptionType) < 0)
    INITERROR;

  PyModule_AddObject (m, "Option",
		      (PyObject *)&cups_OptionType);

  // Group type
  cups_GroupType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_GroupType) < 0)
    INITERROR;

  PyModule_AddObject (m, "Group",
		      (PyObject *)&cups_GroupType);

  // Constraint type
  cups_ConstraintType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_ConstraintType) < 0)
    INITERROR;

  PyModule_AddObject (m, "Constraint",
		      (PyObject *)&cups_ConstraintType);

  // Attribute type
  cups_AttributeType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_AttributeType) < 0)
    INITERROR;

  PyModule_AddObject (m, "Attribute",
		      (PyObject *)&cups_AttributeType);

  // Dest type
  cups_DestType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_DestType) < 0)
    INITERROR;

  PyModule_AddObject (m, "Dest",
		      (PyObject *)&cups_DestType);

  // IPPRequest type
  cups_IPPRequestType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_IPPRequestType) < 0)
      INITERROR;

  PyModule_AddObject (m, "IPPRequest",
		      (PyObject *)&cups_IPPRequestType);

  // IPPAttribute type
  cups_IPPAttributeType.tp_new = PyType_GenericNew;
  if (PyType_Ready (&cups_IPPAttributeType) < 0)
      INITERROR;

  PyModule_AddObject (m, "IPPAttribute",
		      (PyObject *)&cups_IPPAttributeType);

  // Constants
#  define INT_CONSTANT(name)					\
    PyDict_SetItemString (d, #name, PyLong_FromLong (name))
#  define INT_CONSTANT_AS(name,alias)				\
    PyDict_SetItemString (d, alias, PyLong_FromLong (name))
#  define INT_CONSTANT_ALIAS(name,alias)			\
    PyDict_SetItemString (d, #name, PyLong_FromLong (name));	\
    PyDict_SetItemString (d, alias, PyLong_FromLong (name))
#define STR_CONSTANT(name)					\
  PyDict_SetItemString (d, #name, PyUnicode_FromString (name))

#  define INT_17_CONSTANT(newname,oldname)	\
  INT_CONSTANT_ALIAS(newname,#oldname)
#  define INT_17_CONSTANT_NEWNAME(newname,oldname)	\
  INT_CONSTANT(newname)

  // CUPS printer types
  INT_CONSTANT (CUPS_PRINTER_LOCAL);
  INT_CONSTANT (CUPS_PRINTER_CLASS);
  INT_CONSTANT (CUPS_PRINTER_REMOTE);
  INT_CONSTANT (CUPS_PRINTER_BW);
  INT_CONSTANT (CUPS_PRINTER_COLOR);
  INT_CONSTANT (CUPS_PRINTER_DUPLEX);
  INT_CONSTANT (CUPS_PRINTER_STAPLE);
  INT_CONSTANT (CUPS_PRINTER_COPIES);
  INT_CONSTANT (CUPS_PRINTER_COLLATE);
  INT_CONSTANT (CUPS_PRINTER_PUNCH);
  INT_CONSTANT (CUPS_PRINTER_COVER);
  INT_CONSTANT (CUPS_PRINTER_BIND);
  INT_CONSTANT (CUPS_PRINTER_SORT);
  INT_CONSTANT (CUPS_PRINTER_SMALL);
  INT_CONSTANT (CUPS_PRINTER_MEDIUM);
  INT_CONSTANT (CUPS_PRINTER_LARGE);
  INT_CONSTANT (CUPS_PRINTER_VARIABLE);
  INT_CONSTANT (CUPS_PRINTER_IMPLICIT);
  INT_CONSTANT (CUPS_PRINTER_DEFAULT);
  INT_CONSTANT (CUPS_PRINTER_FAX);
  INT_CONSTANT (CUPS_PRINTER_REJECTING);
  INT_CONSTANT (CUPS_PRINTER_DELETE);
  INT_CONSTANT (CUPS_PRINTER_NOT_SHARED);
  INT_CONSTANT (CUPS_PRINTER_AUTHENTICATED);
  INT_CONSTANT (CUPS_PRINTER_COMMANDS);
  INT_CONSTANT (CUPS_PRINTER_OPTIONS);
  INT_CONSTANT (CUPS_PRINTER_DISCOVERED);

  // HTTP encryption
  INT_CONSTANT (HTTP_ENCRYPT_IF_REQUESTED);
  INT_CONSTANT (HTTP_ENCRYPT_NEVER);
  INT_CONSTANT (HTTP_ENCRYPT_REQUIRED);
  INT_CONSTANT (HTTP_ENCRYPT_ALWAYS);

  // Document formats
  STR_CONSTANT (CUPS_FORMAT_AUTO);
  STR_CONSTANT (CUPS_FORMAT_COMMAND);
  STR_CONSTANT (CUPS_FORMAT_PDF);
  STR_CONSTANT (CUPS_FORMAT_POSTSCRIPT);
  STR_CONSTANT (CUPS_FORMAT_RAW);
  STR_CONSTANT (CUPS_FORMAT_TEXT);

  // Selected HTTP status codes
  /* Also define legacy names */
#  define INT_HTTP_STATUS_CONSTANT(name)		\
  INT_CONSTANT_ALIAS(HTTP_STATUS_##name, "HTTP_"#name)

  INT_HTTP_STATUS_CONSTANT (ERROR);
  INT_HTTP_STATUS_CONSTANT (OK);
  INT_HTTP_STATUS_CONSTANT (NOT_MODIFIED);
  INT_HTTP_STATUS_CONSTANT (BAD_REQUEST);
  INT_HTTP_STATUS_CONSTANT (UNAUTHORIZED);
  INT_HTTP_STATUS_CONSTANT (FORBIDDEN);
  INT_HTTP_STATUS_CONSTANT (NOT_FOUND);
  INT_HTTP_STATUS_CONSTANT (REQUEST_TIMEOUT);
  INT_HTTP_STATUS_CONSTANT (UPGRADE_REQUIRED);
  INT_HTTP_STATUS_CONSTANT (SERVER_ERROR);
  INT_HTTP_STATUS_CONSTANT (NOT_IMPLEMENTED);
  INT_HTTP_STATUS_CONSTANT (BAD_GATEWAY);
  INT_HTTP_STATUS_CONSTANT (SERVICE_UNAVAILABLE);
  INT_HTTP_STATUS_CONSTANT (GATEWAY_TIMEOUT);
  INT_HTTP_STATUS_CONSTANT (NOT_SUPPORTED);
  INT_17_CONSTANT (HTTP_STATUS_CUPS_AUTHORIZATION_CANCELED,
		   HTTP_AUTHORIZATION_CANCELED);
  INT_17_CONSTANT (HTTP_STATUS_CUPS_PKI_ERROR, HTTP_PKI_ERROR);

  // PPD UI enum
  INT_CONSTANT (PPD_UI_BOOLEAN);
  INT_CONSTANT (PPD_UI_PICKONE);
  INT_CONSTANT (PPD_UI_PICKMANY);

  // PPD Order dependency enum
  INT_CONSTANT (PPD_ORDER_ANY);
  INT_CONSTANT (PPD_ORDER_DOCUMENT);
  INT_CONSTANT (PPD_ORDER_EXIT);
  INT_CONSTANT (PPD_ORDER_JCL);
  INT_CONSTANT (PPD_ORDER_PAGE);
  INT_CONSTANT (PPD_ORDER_PROLOG);

  // Job states
  INT_CONSTANT (IPP_JOB_PENDING);
  INT_CONSTANT (IPP_JOB_HELD);
  INT_CONSTANT (IPP_JOB_PROCESSING);
  INT_CONSTANT (IPP_JOB_STOPPED);
  INT_CONSTANT (IPP_JOB_CANCELED);
  INT_CONSTANT (IPP_JOB_ABORTED);
  INT_CONSTANT (IPP_JOB_COMPLETED);

  // Printer states
  INT_CONSTANT (IPP_PRINTER_IDLE);
  INT_CONSTANT (IPP_PRINTER_PROCESSING);
  INT_CONSTANT (IPP_PRINTER_STOPPED);

  // IPP resolution units
  INT_CONSTANT (IPP_RES_PER_CM);
  INT_CONSTANT (IPP_RES_PER_INCH);

  // IPP finishings
  INT_CONSTANT (IPP_FINISHINGS_NONE);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE);
  INT_CONSTANT (IPP_FINISHINGS_PUNCH);
  INT_CONSTANT (IPP_FINISHINGS_COVER);
  INT_CONSTANT (IPP_FINISHINGS_BIND);
  INT_CONSTANT (IPP_FINISHINGS_SADDLE_STITCH);
  INT_CONSTANT (IPP_FINISHINGS_EDGE_STITCH);
  INT_CONSTANT (IPP_FINISHINGS_FOLD);
  INT_CONSTANT (IPP_FINISHINGS_TRIM);
  INT_CONSTANT (IPP_FINISHINGS_BALE);
  INT_CONSTANT (IPP_FINISHINGS_BOOKLET_MAKER);
  INT_CONSTANT (IPP_FINISHINGS_JOB_OFFSET);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE_TOP_LEFT);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE_BOTTOM_LEFT);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE_TOP_RIGHT);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE_BOTTOM_RIGHT);
  INT_CONSTANT (IPP_FINISHINGS_EDGE_STITCH_LEFT);
  INT_CONSTANT (IPP_FINISHINGS_EDGE_STITCH_TOP);
  INT_CONSTANT (IPP_FINISHINGS_EDGE_STITCH_RIGHT);
  INT_CONSTANT (IPP_FINISHINGS_EDGE_STITCH_BOTTOM);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE_DUAL_LEFT);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE_DUAL_TOP);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE_DUAL_RIGHT);
  INT_CONSTANT (IPP_FINISHINGS_STAPLE_DUAL_BOTTOM);
  INT_CONSTANT (IPP_FINISHINGS_BIND_LEFT);
  INT_CONSTANT (IPP_FINISHINGS_BIND_TOP);
  INT_CONSTANT (IPP_FINISHINGS_BIND_RIGHT);
  INT_CONSTANT (IPP_FINISHINGS_BIND_BOTTOM);

  // IPP orientations
  /* Also define legacy names */
#  define INT_IPP_ORIENT_CONSTANT(name)	\
  INT_CONSTANT_ALIAS(IPP_ORIENT_##name, "IPP_"#name)

  INT_IPP_ORIENT_CONSTANT (PORTRAIT);
  INT_IPP_ORIENT_CONSTANT (LANDSCAPE);
  INT_IPP_ORIENT_CONSTANT (REVERSE_PORTRAIT);
  INT_IPP_ORIENT_CONSTANT (REVERSE_LANDSCAPE);

  // IPP qualities
  INT_CONSTANT (IPP_QUALITY_DRAFT);
  INT_CONSTANT (IPP_QUALITY_NORMAL);
  INT_CONSTANT (IPP_QUALITY_HIGH);

  // IPP errors
  /* Also define legacy names */
#  define INT_IPP_STATUS_ERROR_CONSTANT(name)	\
  INT_CONSTANT_ALIAS(IPP_STATUS_ERROR_##name, "IPP_"#name)
#  define INT_IPP_STATUS_OK_CONSTANT(name)	\
  INT_CONSTANT_ALIAS(IPP_STATUS_##name, "IPP_"#name)

  INT_IPP_STATUS_OK_CONSTANT (OK);
  INT_17_CONSTANT (IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED, IPP_OK_SUBST);
  INT_17_CONSTANT (IPP_STATUS_OK_CONFLICTING, IPP_OK_CONFLICT);
  INT_IPP_STATUS_OK_CONSTANT (OK_IGNORED_SUBSCRIPTIONS);
  INT_IPP_STATUS_OK_CONSTANT (OK_IGNORED_NOTIFICATIONS);
  INT_IPP_STATUS_OK_CONSTANT (OK_TOO_MANY_EVENTS);
  INT_IPP_STATUS_OK_CONSTANT (OK_BUT_CANCEL_SUBSCRIPTION);
  INT_IPP_STATUS_OK_CONSTANT (OK_EVENTS_COMPLETE);
  INT_17_CONSTANT (IPP_STATUS_REDIRECTION_OTHER_SITE,
		   IPP_REDIRECTION_OTHER_SITE);
  INT_IPP_STATUS_ERROR_CONSTANT (BAD_REQUEST);
  INT_IPP_STATUS_ERROR_CONSTANT (FORBIDDEN);
  INT_IPP_STATUS_ERROR_CONSTANT (NOT_AUTHENTICATED);
  INT_IPP_STATUS_ERROR_CONSTANT (NOT_AUTHORIZED);
  INT_IPP_STATUS_ERROR_CONSTANT (NOT_POSSIBLE);
  INT_IPP_STATUS_ERROR_CONSTANT (TIMEOUT);
  INT_IPP_STATUS_ERROR_CONSTANT (NOT_FOUND);
  INT_IPP_STATUS_ERROR_CONSTANT (GONE);
  INT_IPP_STATUS_ERROR_CONSTANT (REQUEST_ENTITY);
  INT_IPP_STATUS_ERROR_CONSTANT (REQUEST_VALUE);
  INT_17_CONSTANT (IPP_STATUS_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED,
		   IPP_DOCUMENT_FORMAT);
  INT_17_CONSTANT (IPP_STATUS_ERROR_ATTRIBUTES_OR_VALUES, IPP_ATTRIBUTES);
  INT_IPP_STATUS_ERROR_CONSTANT (URI_SCHEME);
  INT_IPP_STATUS_ERROR_CONSTANT (CHARSET);
  INT_17_CONSTANT (IPP_STATUS_ERROR_CONFLICTING, IPP_CONFLICT);
  INT_IPP_STATUS_ERROR_CONSTANT (COMPRESSION_NOT_SUPPORTED);
  INT_IPP_STATUS_ERROR_CONSTANT (COMPRESSION_ERROR);
  INT_IPP_STATUS_ERROR_CONSTANT (DOCUMENT_FORMAT_ERROR);
  INT_17_CONSTANT (IPP_STATUS_ERROR_DOCUMENT_ACCESS, IPP_DOCUMENT_ACCESS_ERROR);
  INT_IPP_STATUS_ERROR_CONSTANT (ATTRIBUTES_NOT_SETTABLE);
  INT_IPP_STATUS_ERROR_CONSTANT (IGNORED_ALL_SUBSCRIPTIONS);
  INT_IPP_STATUS_ERROR_CONSTANT (TOO_MANY_SUBSCRIPTIONS);
  INT_IPP_STATUS_ERROR_CONSTANT (IGNORED_ALL_NOTIFICATIONS);
  INT_IPP_STATUS_ERROR_CONSTANT (PRINT_SUPPORT_FILE_NOT_FOUND);
  INT_17_CONSTANT (IPP_STATUS_ERROR_INTERNAL, IPP_INTERNAL_ERROR);
  INT_IPP_STATUS_ERROR_CONSTANT (OPERATION_NOT_SUPPORTED);
  INT_IPP_STATUS_ERROR_CONSTANT (SERVICE_UNAVAILABLE);
  INT_IPP_STATUS_ERROR_CONSTANT (VERSION_NOT_SUPPORTED);
  INT_17_CONSTANT (IPP_STATUS_ERROR_DEVICE, IPP_DEVICE_ERROR);
  INT_17_CONSTANT (IPP_STATUS_ERROR_TEMPORARY, IPP_TEMPORARY_ERROR);
  INT_17_CONSTANT (IPP_STATUS_ERROR_NOT_ACCEPTING_JOBS, IPP_NOT_ACCEPTING);
  INT_17_CONSTANT (IPP_STATUS_ERROR_BUSY, IPP_PRINTER_BUSY);
  INT_17_CONSTANT (IPP_STATUS_ERROR_JOB_CANCELED, IPP_ERROR_JOB_CANCELED);
  INT_IPP_STATUS_ERROR_CONSTANT (MULTIPLE_JOBS_NOT_SUPPORTED);
  INT_IPP_STATUS_ERROR_CONSTANT (PRINTER_IS_DEACTIVATED);
  INT_17_CONSTANT (IPP_STATUS_ERROR_CUPS_AUTHENTICATION_CANCELED,
		   IPP_AUTHENTICATION_CANCELED);
  INT_17_CONSTANT (IPP_STATUS_ERROR_CUPS_PKI, IPP_PKI_ERROR);
  INT_17_CONSTANT (IPP_STATUS_ERROR_CUPS_UPGRADE_REQUIRED,
		   IPP_UPGRADE_REQUIRED);

  // IPP states
  /* Also define legacy names */
#  define INT_IPP_STATE_CONSTANT(name)	\
  INT_CONSTANT_ALIAS(IPP_STATE_##name, "IPP_"#name)

  INT_IPP_STATE_CONSTANT (ERROR);
  INT_IPP_STATE_CONSTANT (IDLE);
  INT_IPP_STATE_CONSTANT (HEADER);
  INT_IPP_STATE_CONSTANT (ATTRIBUTE);
  INT_IPP_STATE_CONSTANT (DATA);

  // IPP attribute tags
  INT_CONSTANT (IPP_TAG_ZERO);
  INT_CONSTANT (IPP_TAG_OPERATION);
  INT_CONSTANT (IPP_TAG_JOB);
  INT_CONSTANT (IPP_TAG_PRINTER);
  INT_CONSTANT (IPP_TAG_INTEGER);
  INT_CONSTANT (IPP_TAG_BOOLEAN);
  INT_CONSTANT (IPP_TAG_ENUM);
  INT_CONSTANT (IPP_TAG_STRING);
  INT_CONSTANT (IPP_TAG_RANGE);
  INT_CONSTANT (IPP_TAG_TEXT);
  INT_CONSTANT (IPP_TAG_NAME);
  INT_CONSTANT (IPP_TAG_KEYWORD);
  INT_CONSTANT (IPP_TAG_URI);
  INT_CONSTANT (IPP_TAG_CHARSET);
  INT_CONSTANT (IPP_TAG_LANGUAGE);
  INT_CONSTANT (IPP_TAG_MIMETYPE);

  // IPP operations
#  define INT_IPP_OP_CONSTANT(name)		\
  INT_CONSTANT(IPP_OP_##name)

  INT_IPP_OP_CONSTANT (PRINT_JOB);
  INT_IPP_OP_CONSTANT (PRINT_URI);
  INT_IPP_OP_CONSTANT (VALIDATE_JOB);
  INT_IPP_OP_CONSTANT (CREATE_JOB);
  INT_IPP_OP_CONSTANT (SEND_DOCUMENT);
  INT_IPP_OP_CONSTANT (SEND_URI);
  INT_IPP_OP_CONSTANT (CANCEL_JOB);
  INT_IPP_OP_CONSTANT (GET_JOB_ATTRIBUTES);
  INT_IPP_OP_CONSTANT (GET_JOBS);
  INT_IPP_OP_CONSTANT (GET_PRINTER_ATTRIBUTES);
  INT_IPP_OP_CONSTANT (HOLD_JOB);
  INT_IPP_OP_CONSTANT (RELEASE_JOB);
  INT_IPP_OP_CONSTANT (RESTART_JOB);
  INT_IPP_OP_CONSTANT (PAUSE_PRINTER);
  INT_IPP_OP_CONSTANT (RESUME_PRINTER);
  INT_IPP_OP_CONSTANT (PURGE_JOBS);
  INT_IPP_OP_CONSTANT (SET_PRINTER_ATTRIBUTES);
  INT_IPP_OP_CONSTANT (SET_JOB_ATTRIBUTES);
  INT_IPP_OP_CONSTANT (GET_PRINTER_SUPPORTED_VALUES);
  INT_17_CONSTANT (IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS,
		   IPP_CREATE_PRINTER_SUBSCRIPTION);
  INT_17_CONSTANT (IPP_OP_CREATE_JOB_SUBSCRIPTIONS,
		   IPP_CREATE_JOB_SUBSCRIPTION);
  INT_IPP_OP_CONSTANT (GET_SUBSCRIPTIONS);
  INT_IPP_OP_CONSTANT (RENEW_SUBSCRIPTION);
  INT_IPP_OP_CONSTANT (CANCEL_SUBSCRIPTION);
  INT_IPP_OP_CONSTANT (GET_NOTIFICATIONS);
  INT_IPP_OP_CONSTANT (SEND_NOTIFICATIONS);
  INT_IPP_OP_CONSTANT (GET_RESOURCE_ATTRIBUTES);
  INT_IPP_OP_CONSTANT (GET_RESOURCE_DATA);
  INT_IPP_OP_CONSTANT (GET_RESOURCES);
  INT_IPP_OP_CONSTANT (GET_PRINT_SUPPORT_FILES);
  INT_IPP_OP_CONSTANT (ENABLE_PRINTER);
  INT_IPP_OP_CONSTANT (DISABLE_PRINTER);
  INT_IPP_OP_CONSTANT (PAUSE_PRINTER_AFTER_CURRENT_JOB);
  INT_IPP_OP_CONSTANT (HOLD_NEW_JOBS);
  INT_IPP_OP_CONSTANT (RELEASE_HELD_NEW_JOBS);
  INT_IPP_OP_CONSTANT (DEACTIVATE_PRINTER);
  INT_IPP_OP_CONSTANT (ACTIVATE_PRINTER);
  INT_IPP_OP_CONSTANT (RESTART_PRINTER);
  INT_IPP_OP_CONSTANT (SHUTDOWN_PRINTER);
  INT_IPP_OP_CONSTANT (STARTUP_PRINTER);
  INT_IPP_OP_CONSTANT (REPROCESS_JOB);
  INT_IPP_OP_CONSTANT (CANCEL_CURRENT_JOB);
  INT_IPP_OP_CONSTANT (SUSPEND_CURRENT_JOB);
  INT_IPP_OP_CONSTANT (RESUME_JOB);
  INT_IPP_OP_CONSTANT (PROMOTE_JOB);
  INT_IPP_OP_CONSTANT (SCHEDULE_JOB_AFTER);
  INT_IPP_OP_CONSTANT (CANCEL_JOBS);
  INT_IPP_OP_CONSTANT (CANCEL_MY_JOBS);
  INT_IPP_OP_CONSTANT (RESUBMIT_JOB);
  INT_IPP_OP_CONSTANT (CLOSE_JOB);
  INT_IPP_OP_CONSTANT (IDENTIFY_PRINTER);
  INT_IPP_OP_CONSTANT (VALIDATE_DOCUMENT);
  INT_IPP_OP_CONSTANT (SEND_HARDCOPY_DOCUMENT);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_GET_DEFAULT, CUPS_GET_DEFAULT);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_GET_PRINTERS, CUPS_GET_PRINTERS);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_ADD_MODIFY_PRINTER,
			   CUPS_ADD_MODIFY_PRINTER);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_DELETE_PRINTER, CUPS_DELETE_PRINTER);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_GET_CLASSES, CUPS_GET_CLASSES);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_ADD_MODIFY_CLASS, CUPS_ADD_MODIFY_CLASS);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_DELETE_CLASS, CUPS_DELETE_CLASS);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_ACCEPT_JOBS, CUPS_ACCEPT_JOBS);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_REJECT_JOBS, CUPS_REJECT_JOBS);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_SET_DEFAULT, CUPS_SET_DEFAULT);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_GET_PPDS, CUPS_GET_PPDS);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_MOVE_JOB, CUPS_MOVE_JOB);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_AUTHENTICATE_JOB, CUPS_AUTHENTICATE_JOB);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_GET_PPD, CUPS_GET_PPD);
  INT_17_CONSTANT_NEWNAME (IPP_OP_CUPS_GET_DOCUMENT, CUPS_GET_DOCUMENT);

  // Limits
  INT_CONSTANT (IPP_MAX_NAME);

  // PPD conformance levels
  INT_CONSTANT (PPD_CONFORM_RELAXED);
  INT_CONSTANT (PPD_CONFORM_STRICT);

  // Admin Util constants
  STR_CONSTANT (CUPS_SERVER_DEBUG_LOGGING);
  STR_CONSTANT (CUPS_SERVER_REMOTE_ADMIN);
  STR_CONSTANT (CUPS_SERVER_REMOTE_PRINTERS);
  STR_CONSTANT (CUPS_SERVER_SHARE_PRINTERS);
  STR_CONSTANT (CUPS_SERVER_USER_CANCEL_ANY);
  STR_CONSTANT (CUPS_SERVER_REMOTE_ANY);

  // Dest enumeration flags
  INT_CONSTANT (CUPS_DEST_FLAGS_NONE);
  INT_CONSTANT (CUPS_DEST_FLAGS_UNCONNECTED);
  INT_CONSTANT (CUPS_DEST_FLAGS_MORE);
  INT_CONSTANT (CUPS_DEST_FLAGS_REMOVED);
  INT_CONSTANT (CUPS_DEST_FLAGS_ERROR);
  INT_CONSTANT (CUPS_DEST_FLAGS_RESOLVING);
  INT_CONSTANT (CUPS_DEST_FLAGS_CONNECTING);
  INT_CONSTANT (CUPS_DEST_FLAGS_CANCELED);

  // Exceptions
  obj = PyDict_New ();
  PyDict_SetItemString (obj, "__doc__", PyUnicode_FromString(
    "This exception is raised when an HTTP problem has occurred.  It \n"
    "provides an integer HTTP status code.\n\n"
    "Use it like this::\n"
    "  try:\n"
    "    ...\n"
    "  except cups.HTTPError as (status):\n"
    "    print 'HTTP status is %d' % status\n"));
  HTTPError = PyErr_NewException ("cups.HTTPError", NULL, obj);
  Py_DECREF (obj);
  if (HTTPError == NULL)
    INITERROR;
  Py_INCREF (HTTPError);
  PyModule_AddObject (m, "HTTPError", HTTPError);

  obj = PyDict_New ();
  PyDict_SetItemString (obj, "__doc__", PyUnicode_FromString(
    "This exception is raised when an IPP error has occurred.  It \n"
    "provides an integer IPP status code, and a human-readable string \n"
    "describing the error.\n\n"
    "Use it like this::\n"
    "  try:\n"
    "    ...\n"
    "  except cups.IPPError as (status, description):\n"
    "    print 'IPP status is %d' % status\n"
    "    print 'Meaning:', description\n"));
  IPPError = PyErr_NewException ("cups.IPPError", NULL, obj);
  Py_DECREF (obj);
  if (IPPError == NULL)
    INITERROR;
  Py_INCREF (IPPError);
  PyModule_AddObject (m, "IPPError", IPPError);

    return m;
}

///////////////
// Debugging //
///////////////

#define ENVAR "PYCUPS_DEBUG"
static int debugging_enabled = -1;

void
debugprintf (const char *fmt, ...)
{
  if (!debugging_enabled)
    return;

  if (debugging_enabled == -1)
    {
      if (!getenv (ENVAR))
	{
	  debugging_enabled = 0;
	  return;
	}

      debugging_enabled = 1;
    }
  
  {
    va_list ap;
    va_start (ap, fmt);
    vfprintf (stderr, fmt, ap);
    va_end (ap);
  }
}
