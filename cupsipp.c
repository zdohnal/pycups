/*
 * cups - Python bindings for CUPS
 * Copyright (C) 2002-2020  Red Hat, Inc.
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

#include "cupsipp.h"
#include "cupsmodule.h"

//////////////////
// IPPAttribute //
//////////////////

static PyObject *
IPPAttribute_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  IPPAttribute *self;
  self = (IPPAttribute *) type->tp_alloc (type, 0);
  if (self != NULL) {
    self->group_tag = IPP_TAG_ZERO;
    self->value_tag = IPP_TAG_ZERO;
    self->name = NULL;
    self->values = NULL;
  }

  return (PyObject *) self;
}

static int
IPPAttribute_init (IPPAttribute *self, PyObject *args, PyObject *kwds)
{
  ipp_tag_t group_tag, value_tag;
  PyObject *nameobj;
  char *name;
  PyObject *value = NULL;
  PyObject *list = NULL;

  if (!PyArg_ParseTuple (args, "iiO|O", &group_tag, &value_tag, &nameobj,
			 &value))
    return -1;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return -1;

  if (value == NULL) {
    switch (value_tag) {
    case IPP_TAG_ZERO:
    case IPP_TAG_NOVALUE:
    case IPP_TAG_NOTSETTABLE:
    case IPP_TAG_ADMINDEFINE:
      break;

    default:
      PyErr_SetString (PyExc_RuntimeError, "missing value list");
      return -1;
    }
  } else {
    size_t i, n;
    int valid = 1;

    if (PyList_Check (value)) {
      list = value;
      Py_INCREF (value);
      n = PyList_Size (value);
    } else {
      list = PyList_New (0);
      PyList_Append (list, value);
      n = 1;
    }

    for (i = 0; valid && i < n; i++) {
      PyObject *v = PyList_GetItem (list, i); // borrowed
      switch (value_tag) {
      case IPP_TAG_INTEGER:
      case IPP_TAG_ENUM:
      case IPP_TAG_RANGE:
	valid = PyLong_Check (v);
	break;

      case IPP_TAG_BOOLEAN:
	valid = PyBool_Check (v);
	break;

      case IPP_TAG_TEXT:
	valid = PyUnicode_Check (v);
	break;

      case IPP_TAG_NAME:
      case IPP_TAG_KEYWORD:
      case IPP_TAG_URI:
      case IPP_TAG_MIMETYPE:
      case IPP_TAG_CHARSET:
      case IPP_TAG_LANGUAGE:
	valid = (PyUnicode_Check (v) || PyBytes_Check (v));
	break;

      default:
	valid = 0;
      }
    }

    if (!valid) {
      PyErr_SetString (PyExc_RuntimeError, "invalid value");
      Py_DECREF (list);
      return -1;
    }
  }

  self->group_tag = group_tag;
  self->value_tag = value_tag;
  self->values = list;
  self->name = name;
  return 0;
}

static void
IPPAttribute_dealloc (IPPAttribute *self)
{
  Py_XDECREF (self->values);
  if (self->name)
    free (self->name);

  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

static PyObject *
IPPAttribute_repr (IPPAttribute *self)
{
  PyObject *ret;
  PyObject *values_repr = NULL;
  char *values = NULL;
  char buffer[1024];
  if (self->values) {
    values_repr = PyList_Type.tp_repr (self->values);
    UTF8_from_PyObj (&values, values_repr);
  }

  snprintf (buffer, sizeof (buffer), "<cups.IPPAttribute %s (%d:%d)%s%s>",
	    self->name, self->group_tag, self->value_tag,
	    values ? ": " : "",
	    values ? values : "");
  ret = PyUnicode_FromString (buffer);

  free (values);
  Py_XDECREF (values_repr);
  return ret;
}

//////////////////
// IPPAttribute // ATTRIBUTES
//////////////////

static PyObject *
IPPAttribute_getGroupTag (IPPAttribute *self, void *closure)
{
  return PyLong_FromLong (self->group_tag);
}

static PyObject *
IPPAttribute_getValueTag (IPPAttribute *self, void *closure)
{
  return PyLong_FromLong (self->value_tag);
}

static PyObject *
IPPAttribute_getName (IPPAttribute *self, void *closure)
{
  return PyUnicode_FromString (self->name);
}

static PyObject *
IPPAttribute_getValues (IPPAttribute *self, void *closure)
{
  Py_INCREF (self->values);
  return self->values;
}

PyGetSetDef IPPAttribute_getseters[] =
  {
    { "group_tag",
      (getter) IPPAttribute_getGroupTag, (setter) NULL,
      "IPP group tag", NULL },

    { "value_tag",
      (getter) IPPAttribute_getValueTag, (setter) NULL,
      "IPP value tag", NULL },

    { "name",
      (getter) IPPAttribute_getName, (setter) NULL,
      "IPP attribute name", NULL },

    { "values",
      (getter) IPPAttribute_getValues, (setter) NULL,
      "List of values" },

    { NULL }, /* Sentinel */
  };

PyMethodDef IPPAttribute_methods[] =
  {
    { NULL } /* Sentinel */
  };

PyTypeObject cups_IPPAttributeType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.IPPAttribute",       /*tp_name*/
    sizeof(IPPAttribute),      /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)IPPAttribute_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    (reprfunc)IPPAttribute_repr, /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,        /*tp_flags*/
    "IPP Attribute\n"
    "=============\n"
    "  An IPP attribute.\n\n"
    "@type group_tag: int\n"
    "@ivar group_tag: IPP group tag\n"
    "@type value_tag: int\n"
    "@ivar value_tag: IPP value tag\n"
    "@type values: list\n"
    "@ivar value: list of values\n"
    "",                        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    IPPAttribute_methods,      /* tp_methods */
    0,                         /* tp_members */
    IPPAttribute_getseters,    /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)IPPAttribute_init, /* tp_init */
    0,                         /* tp_alloc */
    IPPAttribute_new,          /* tp_new */
  };

////////////////
// IPPRequest //
////////////////

static PyObject *
IPPRequest_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  IPPRequest *self;
  self = (IPPRequest *) type->tp_alloc (type, 0);
  if (self != NULL)
    self->ipp = NULL;

  return (PyObject *) self;
}

static int
IPPRequest_init (IPPRequest *self, PyObject *args, PyObject *kwds)
{
  long op = -1;
  if (!PyArg_ParseTuple (args, "|i", &op))
    return -1;

  if (op == -1)
    self->ipp = ippNew ();
  else
    self->ipp = ippNewRequest (op);

  return 0;
}

static void
IPPRequest_dealloc (IPPRequest *self)
{
  ippDelete (self->ipp);
  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

static IPPAttribute *
build_IPPAttribute (ipp_attribute_t *attr)
{
  IPPAttribute *attribute = NULL;
  PyObject *largs = NULL;
  PyObject *lkwlist = NULL;
  PyObject *values = NULL;

  debugprintf ("%s: ", ippGetName (attr));
  if (ippGetValueTag (attr) == IPP_TAG_ZERO ||
      ippGetValueTag (attr) == IPP_TAG_NOVALUE ||
      ippGetValueTag (attr) == IPP_TAG_NOTSETTABLE ||
      ippGetValueTag (attr) == IPP_TAG_ADMINDEFINE) {
    debugprintf ("no value\n");
  } else {
    PyObject *value = NULL;
    int i;
    int unknown_value_tag = 0;

    values = PyList_New (0);
    if (!values)
      goto out;

    for (i = 0; i < ippGetCount (attr); i++) {
      switch (ippGetValueTag (attr)) {
      case IPP_TAG_INTEGER:
      case IPP_TAG_ENUM:
      case IPP_TAG_RANGE:
	value = PyLong_FromLong (ippGetInteger (attr, i));
	debugprintf ("i%d", ippGetInteger (attr, i));
	break;

      case IPP_TAG_BOOLEAN:
	value = PyBool_FromLong (ippGetBoolean (attr, i));
	debugprintf ("b%d", ippGetInteger (attr, i));
	break;

      case IPP_TAG_TEXT:
	value = PyUnicode_Decode (ippGetString (attr, i, NULL),
				  strlen (ippGetString (attr, i, NULL)),
				  "utf-8", NULL);
	debugprintf ("u%s", ippGetString (attr, i, NULL));
	break;

      case IPP_TAG_NAME:
      case IPP_TAG_KEYWORD:
      case IPP_TAG_URI:
      case IPP_TAG_MIMETYPE:
      case IPP_TAG_CHARSET:
      case IPP_TAG_LANGUAGE:
	value = PyUnicode_FromString (ippGetString (attr, i, NULL));
	debugprintf ("s%s", ippGetString (attr, i, NULL));
	break;

      default:
	value = NULL;
	unknown_value_tag = 1;
	debugprintf ("Unable to encode value tag %d\n", ippGetValueTag (attr));
      }

      if (!value)
	break; /* out of values loop */

      debugprintf ("(%p), ", value);
      if (PyList_Append (values, value) != 0) {
	Py_DECREF (values);
	Py_DECREF (value);
	goto out;
      }

      Py_DECREF (value);
    }

    if (unknown_value_tag) {
      Py_DECREF (values);
      goto out;
    }

    debugprintf ("\n");
  }

  if (values) {
    largs = Py_BuildValue ("(iisO)",
			   ippGetGroupTag (attr),
			   ippGetValueTag (attr),
			   ippGetName (attr),
			   values);
    Py_DECREF (values);
    values = NULL;
  } else
    largs = Py_BuildValue ("(iis)", ippGetGroupTag (attr), ippGetValueTag (attr),
			   ippGetName (attr) ? ippGetName (attr) : "");

  if (!largs)
    goto out;

  lkwlist = Py_BuildValue ("{}");
  if (!lkwlist)
    goto out;

  attribute = (IPPAttribute *) PyType_GenericNew (&cups_IPPAttributeType,
						  largs, lkwlist);
  if (!attribute)
    goto out;

  if (IPPAttribute_init (attribute, largs, lkwlist) != 0) {
    Py_DECREF (attribute);
    attribute = NULL;
  }

out:
  if (values)
    Py_DECREF (values);

  if (largs)
    Py_DECREF (largs);

  if (lkwlist)
    Py_DECREF (lkwlist);

  return attribute;
}

static PyObject *
IPPRequest_addSeparator (IPPRequest *self)
{
  ipp_attribute_t *attr = ippAddSeparator (self->ipp);
  return (PyObject *) build_IPPAttribute (attr);
}

static PyObject *
IPPRequest_add (IPPRequest *self, PyObject *args)
{
  PyObject *obj;
  IPPAttribute *attribute;
  Py_ssize_t num_values;
  void *values = NULL;
  size_t value_size = 0;
  int i;

  if (!PyArg_ParseTuple (args, "O", &obj))
    return NULL;

  if (Py_TYPE(obj) != &cups_IPPAttributeType) {
    PyErr_SetString (PyExc_TypeError, "Parameter must be IPPAttribute");
    return NULL;
  }

  attribute = (IPPAttribute *) obj;
  num_values = PyList_Size (attribute->values);
  switch (attribute->value_tag) {
  case IPP_TAG_INTEGER:
  case IPP_TAG_ENUM:
  case IPP_TAG_RANGE:
    value_size = sizeof (int);
    break;

  case IPP_TAG_BOOLEAN:
    value_size = sizeof (char);
    break;

  case IPP_TAG_NAME:
  case IPP_TAG_KEYWORD:
  case IPP_TAG_URI:
  case IPP_TAG_MIMETYPE:
  case IPP_TAG_CHARSET:
  case IPP_TAG_LANGUAGE:
    value_size = sizeof (char *);
    break;

  default:
    break;
  }

  values = calloc (num_values, value_size);
  if (!values) {
    PyErr_SetString (PyExc_MemoryError, "Unable to allocate memory");
    return NULL;
  }

  switch (attribute->value_tag) {
  case IPP_TAG_INTEGER:
  case IPP_TAG_ENUM:
  case IPP_TAG_RANGE:
    for (i = 0; i < num_values; i++) {
      PyObject *item = PyList_GetItem (attribute->values, i);
      if (PyLong_Check (item))
	((int *)values)[i] = PyLong_AsLong (item);
    }
    ippAddIntegers (self->ipp,
		    attribute->group_tag,
		    attribute->value_tag,
		    attribute->name,
		    num_values,
		    values);
    break;

  case IPP_TAG_BOOLEAN:
    for (i = 0; i < num_values; i++) {
      PyObject *item = PyList_GetItem (attribute->values, i);
      ((char *)values)[i] = (item == Py_True ? 1 : 0);
    }
    ippAddBooleans (self->ipp,
		    attribute->group_tag,
		    attribute->name,
		    num_values,
		    values);
    break;

  case IPP_TAG_NAME:
  case IPP_TAG_KEYWORD:
  case IPP_TAG_URI:
  case IPP_TAG_MIMETYPE:
  case IPP_TAG_CHARSET:
  case IPP_TAG_LANGUAGE:
    for (i = 0; i < num_values; i++) {
      PyObject *item = PyList_GetItem (attribute->values, i);
      char *str;
      str = strdup (PyUnicode_AsUTF8 (item));
      ((char **)values)[i] = str;
      if (!str)
	break;
    }
    if (i < num_values) {
      int j;
      for (j = 0; j < i; j++)
	free (((char **)values)[j]);
      PyErr_SetString (PyExc_MemoryError, "Unable to allocate memory");
      free (values);
      return NULL;
    }
    ippAddStrings (self->ipp,
		   attribute->group_tag,
		   attribute->value_tag,
		   attribute->name,
		   num_values,
		   NULL,
		   values);
    for (i = 0; i < num_values; i++)
      free (((char **)values)[i]);

    break;

  default:
    break;
  }

  free (values);
  Py_INCREF (obj);
  return obj;
}

static ssize_t
cupsipp_iocb_read (PyObject *callable, ipp_uchar_t *buffer, size_t len)
{
  PyObject *args = Py_BuildValue ("(i)", len);
  PyObject *result;
  Py_ssize_t got = -1;
  char *gotbuffer;

  debugprintf ("-> cupsipp_iocb_read\n");

  if (!args) {
    debugprintf ("Py_BuildValue failed\n");
    goto out;
  }

  result = PyEval_CallObject (callable, args);
  Py_DECREF (args);

  if (result == NULL) {
    debugprintf ("Exception in read callback\n");
    goto out;
  }

  if (PyUnicode_Check (result) || PyBytes_Check (result)) {
    if (PyUnicode_Check (result)) {
      PyObject *stringobj = PyUnicode_AsUTF8String (result);
      PyBytes_AsStringAndSize (stringobj, &gotbuffer, &got);
    } else {
      PyBytes_AsStringAndSize (result, &gotbuffer, &got);
    }

    if (got < 0)
    {
      debugprintf("No returned data.\n");
      goto out;
    }

    if ((unsigned long)got > len) {
      debugprintf ("More data returned than requested!  Truncated...\n");
      got = len;
    }
    memcpy (buffer, gotbuffer, got);
  } else {
    debugprintf ("Unknown result object type!\n");
  }

  Py_DECREF (result);

 out:
  debugprintf ("<- cupsipp_iocb_read() == %zd\n", got);
  return got;
}

static ssize_t
cupsipp_iocb_write (PyObject *callable, ipp_uchar_t *buffer, size_t len)
{
  PyObject *args = Py_BuildValue ("(y#)", buffer, len);
  PyObject *result = NULL;
  Py_ssize_t wrote = -1;

  debugprintf ("-> cupsipp_iocb_write\n");

  if (!args) {
    debugprintf ("Py_BuildValue failed\n");
    goto out;
  }

  result = PyEval_CallObject (callable, args);
  Py_DECREF (args);

  if (result == NULL) {
    debugprintf ("Exception in write callback\n");
    goto out;
  }

  if (PyLong_Check (result))
    wrote = PyLong_AsLong (result);
  else {
    debugprintf ("Bad return value\n");
    goto out;
  }

out:
  if (result)
    Py_DECREF (result);

  debugprintf ("<- cupsipp_iocb_write()\n");
  return wrote;
}

static PyObject *
IPPRequest_readIO (IPPRequest *self, PyObject *args, PyObject *kwds)
{
  PyObject *cb;
  char blocking = 1;
  ipp_state_t state;
  static char *kwlist[] = { "read_fn", "blocking", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|b", kwlist,
				    &cb, &blocking))
    return NULL;

  if (!PyCallable_Check (cb)) {
    PyErr_SetString (PyExc_TypeError, "Parameter must be callable");
    return NULL;
  }

  state = ippReadIO (cb, (ipp_iocb_t) cupsipp_iocb_read,
		     blocking, NULL, self->ipp);
  return PyLong_FromLong (state);
}

static PyObject *
IPPRequest_writeIO (IPPRequest *self, PyObject *args, PyObject *kwds)
{
  PyObject *cb;
  char blocking = 1;
  ipp_state_t state;
  static char *kwlist[] = { "write_fn", "blocking", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|b", kwlist,
				    &cb, &blocking))
    return NULL;

  if (!PyCallable_Check (cb)) {
    PyErr_SetString (PyExc_TypeError, "Parameter must be callable");
    return NULL;
  }

  state = ippWriteIO (cb, (ipp_iocb_t) cupsipp_iocb_write,
		     blocking, NULL, self->ipp);
  return PyLong_FromLong (state);
}

/* Request properties */

static PyObject *
IPPRequest_getAttributes (IPPRequest *self, void *closure)
{
  PyObject *attrs = PyList_New (0);
  ipp_attribute_t *attr;
  for (attr = ippFirstAttribute (self->ipp); attr; attr = ippNextAttribute(self->ipp))
    {
      IPPAttribute *attribute = build_IPPAttribute (attr);
      if (!attribute)
	goto fail;

      if (PyList_Append (attrs, (PyObject *) attribute) != 0)
	goto fail;
    }

  return attrs;

 fail:
  Py_DECREF (attrs);
  return NULL;
}

static PyObject *
IPPRequest_getOperation (IPPRequest *self, void *closure)
{
  return PyLong_FromLong (ippGetOperation (self->ipp));
}

static PyObject *
IPPRequest_getStatuscode (IPPRequest *self, void *closure)
{
  return PyLong_FromLong (ippGetStatusCode (self->ipp));
}

static int
IPPRequest_setState (IPPRequest *self, PyObject *value, void *closure)
{
  int state;

  if (value == NULL)
  {
    PyErr_SetString(PyExc_TypeError, "Cannot delete state");
    return -1;
  }

  if (PyLong_Check(value))
    state = PyLong_AsLong (value);
  else
  {
    PyErr_SetString(PyExc_TypeError, "state must be an integer");
    return -1;
  }

  ippSetState (self->ipp, state);
  return 0;
}

static PyObject *
IPPRequest_getState (IPPRequest *self, void *closure)
{
  return PyLong_FromLong (ippGetState (self->ipp));
}

static int
IPPRequest_setStatuscode (IPPRequest *self, PyObject *value, void *closure)
{
  int statuscode;

  if (value == NULL)
  {
    PyErr_SetString(PyExc_TypeError, "Cannot delete statuscode");
    return -1;
  }

  if (PyLong_Check(value))
    statuscode = PyLong_AsLong (value);
  else
  {
    PyErr_SetString(PyExc_TypeError, "statuscode must be an integer");
    return -1;
  }

  ippSetStatusCode (self->ipp, statuscode);
  return 0;
}

PyGetSetDef IPPRequest_getseters[] =
  {
    { "attributes",
      (getter) IPPRequest_getAttributes, (setter) NULL,
      "IPP request attributes", NULL },

    { "operation",
      (getter) IPPRequest_getOperation, (setter) NULL,
      "IPP request operation", NULL },

    { "state",
      (getter) IPPRequest_getState,
      (setter) IPPRequest_setState,
      "IPP request transfer state", NULL },

    { "statuscode",
      (getter) IPPRequest_getStatuscode,
      (setter) IPPRequest_setStatuscode,
      "IPP response status code", NULL },

    { NULL } /* Sentinel */
  };

PyMethodDef IPPRequest_methods[] =
  {
    { "addSeparator",
      (PyCFunction) IPPRequest_addSeparator, METH_NOARGS,
      "addSeparator() -> IPPAttribute\n\n"
      "@return: IPP request attribute" },

    { "add",
      (PyCFunction) IPPRequest_add, METH_VARARGS,
      "add(attr) -> IPPAttribute\n\n"
      "@type attr: IPPAttribute\n"
      "@param attr: Attribute to add to the request\n"
      "@return: IPP request attribute" },

    { "readIO",
      (PyCFunction) IPPRequest_readIO, METH_VARARGS | METH_KEYWORDS,
      "readIO(read_fn, blocking=True) -> IPP state\n\n"
      "@type read_fn: Callable function\n"
      "@param read_fn: A callback, taking a single integer argument for\n"
      "'size', for reading IPP data\n"
      "@type blocking: Boolean\n"
      "@param blocking: whether to continue reading until a complete\n"
      "request is read\n"
      "@return: IPP state value" },

    { "writeIO",
      (PyCFunction) IPPRequest_writeIO, METH_VARARGS | METH_KEYWORDS,
      "writeIO(write_fn, blocking=True) -> IPP state\n\n"
      "@type write_fn: Callable function\n"
      "@param write_fn: A callback, taking a bytes object, for writing\n"
      "IPP data\n"
      "@type blocking: Boolean\n"
      "@param blocking: whether to continue reading until a complete\n"
      "request is written\n"
      "@return: IPP state value" },

    { NULL } /* Sentinel */
  };

PyTypeObject cups_IPPRequestType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.IPPRequest",         /*tp_name*/
    sizeof(IPPRequest),        /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)IPPRequest_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,        /*tp_flags*/
    "IPP Request\n"
    "===========\n"
    "  An IPP request.  The constructor takes an optional argument of the\n"
    "operation code.\n\n"
    "",                        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    IPPRequest_methods,        /* tp_methods */
    0,                         /* tp_members */
    IPPRequest_getseters,      /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)IPPRequest_init, /* tp_init */
    0,                         /* tp_alloc */
    IPPRequest_new,          /* tp_new */
  };
