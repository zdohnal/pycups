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

#include "cupsconnection.h"
#include "cupsppd.h"
#include "cupsmodule.h"

#ifndef __SVR4
#include <paths.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _PATH_TMP
#define _PATH_TMP P_tmpdir
#endif

#define DICT_POS_TYPE Py_ssize_t

PyObject *HTTPError;
PyObject *IPPError;

static Connection **Connections = NULL;
static long unsigned int NumConnections = 0;

static void
set_http_error (http_status_t status)
{
  PyObject *v = Py_BuildValue ("i", status);
  debugprintf("set_http_error: %d\n", (int) status);
  if (v != NULL) {
    PyErr_SetObject (HTTPError, v);
    Py_DECREF (v);
  }
}

void
set_ipp_error (ipp_status_t status, const char *message)
{
  if (!message)
    message = ippErrorString (status);

  debugprintf("set_ipp_error: %d, %s\n", (int) status, message);
  PyObject *v = Py_BuildValue ("(is)", status, message);
  if (v != NULL) {
    PyErr_SetObject (IPPError, v);
    Py_DECREF (v);
  }
}

static PyObject *
PyObj_from_UTF8 (const char *utf8)
{
  PyObject *val = PyUnicode_Decode (utf8, strlen (utf8), "utf-8", NULL);
  if (!val) {
    // CUPS 1.2 always gives us UTF-8.  Before CUPS 1.2, the
    // ppd-* strings come straight from the PPD with no
    // transcoding, but the attributes-charset is still 'utf-8'
    // so we've no way of knowing the real encoding.
    // In that case, detect the error and force it to ASCII.
    const char *orig = utf8;
    char *ascii;
    int i;
    PyErr_Clear ();
    ascii = malloc (1 + strlen (orig));
    for (i = 0; orig[i]; i++)
      ascii[i] = orig[i] & 0x7f;
    ascii[i] = '\0';
    val = PyUnicode_FromString (ascii);
    free (ascii);
  }

  return val;
}

const char *
UTF8_from_PyObj (char **const utf8, PyObject *obj)
// converts PyUnicode or PyBytes to char *
{
  if (PyUnicode_Check (obj)) {
    const char *string;
    PyObject *stringobj = PyUnicode_AsUTF8String (obj);
    if (stringobj == NULL)
      return NULL;

    string = PyBytes_AsString (stringobj);
    if (string == NULL) {
      Py_DECREF (stringobj);
      return NULL;
    }

    *utf8 = strdup (string);
    Py_DECREF (stringobj);
    return *utf8;
  }
  else if (PyBytes_Check (obj)) {
    const char *ret;
    PyObject *unicodeobj = PyUnicode_FromEncodedObject (obj, "utf-8", NULL);
    if (unicodeobj == NULL)
      return NULL;

    ret = UTF8_from_PyObj (utf8, unicodeobj);
    Py_DECREF (unicodeobj);
    return ret;
  }

  PyErr_SetString (PyExc_TypeError, "unicode or bytes object required");
  return NULL;
}

static void
construct_uri (char *buffer, size_t buflen, const char *base, const char *value)
{
  char *d = buffer;
  const unsigned char *s = (const unsigned char *) value;
  if (strlen (base) < buflen) {
    strcpy (buffer, base);
    d += strlen (base);
  } else {
    strncpy (buffer, base, buflen);
    d += buflen;
  }

  while (*s && d < buffer + buflen) {
    if (isalpha (*s) || isdigit (*s) || *s == '-')
      *d++ = *s++;
    else if (*s == ' ') {
      *d++ = '+';
      s++;
    } else {
      if (d + 2 < buffer + buflen) {
	*d++ = '%';
	*d++ = "0123456789ABCDEF"[((*s) & 0xf0) >> 4];
	*d++ = "0123456789ABCDEF"[((*s) & 0x0f)];
	s++;
      } else
	break;
    }
  }

  if (d < buffer + buflen)
    *d = '\0';
}

////////////////
// Connection //
////////////////

static PyObject *
Connection_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  Connection *self;
  self = (Connection *) type->tp_alloc (type, 0);
  if (self != NULL) {
    self->http = NULL;
    self->host = NULL;
    self->tstate = NULL;
    self->cb_password = NULL;
  }

  return (PyObject *) self;
}

static int
Connection_init (Connection *self, PyObject *args, PyObject *kwds)
{
  const char *host = cupsServer ();
  int port = ippPort ();
  int encryption = (http_encryption_t) cupsEncryption ();
  static char *kwlist[] = { "host", "port", "encryption", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "|sii", kwlist,
				    &host, &port, &encryption))
    return -1;

  debugprintf ("-> Connection_init(host=%s)\n", host);
  self->host = strdup (host);
  if (!self->host) {
    debugprintf ("<- Connection_init() = -1\n");
    return -1;
  }

  Connection_begin_allow_threads (self);
  debugprintf ("httpConnect2(...)\n");
  self->http = httpConnect2(host, port, NULL, AF_UNSPEC, cupsEncryption(), 1, 30000, NULL);
  Connection_end_allow_threads (self);

  if (!self->http) {
    PyErr_SetString (PyExc_RuntimeError, "failed to connect to server");
    debugprintf ("<- Connection_init() = -1\n");
    return -1;
  }

  if (NumConnections == 0)
  {
    Connections = malloc (sizeof (Connection *));
    if (Connections == NULL) {
      PyErr_SetString (PyExc_RuntimeError, "insufficient memory");
      debugprintf ("<- Connection_init() = -1\n");
      return -1;
    }
  }
  else
  {
    Connection **old_array = Connections;

    if ((1 + NumConnections) >= UINT_MAX / sizeof (Connection *))
    {
      PyErr_SetString (PyExc_RuntimeError, "too many connections");
      debugprintf ("<- Connection_init() == -1\n");
      return -1;
    }

    Connections = realloc (Connections,
			   (1 + NumConnections) * sizeof (Connection *));
    if (Connections == NULL) {
      Connections = old_array;
      PyErr_SetString (PyExc_RuntimeError, "insufficient memory");
      debugprintf ("<- Connection_init() = -1\n");
      return -1;
    }
  }

  Connections[NumConnections++] = self;

  debugprintf ("<- Connection_init() = 0\n");
  return 0;
}

static void
Connection_dealloc (Connection *self)
{
  long unsigned int i, j;

  for (j = 0; j < NumConnections; j++)
    if (Connections[j] == self)
      break;

  if (j < NumConnections)
  {
    if (NumConnections > 1)
    {
      Connection **new_array = calloc (NumConnections - 1,
				       sizeof (Connection *));

      if (new_array)
      {
	int k;
	for (i = 0, k = 0; i < NumConnections; i++)
	{
	  if (i == j)
	    continue;

	  new_array[k++] = Connections[i];
	}

	free (Connections);
	Connections = new_array;
	NumConnections--;
      } else
	/* Failed to allocate memory. Just clear out the reference. */
	Connections[j] = NULL;
    }
    else
    {
      /* The only element is the one we no longer need. */
      free (Connections);
      Connections = NULL;
      NumConnections = 0;
    }
  }

  if (self->http) {  
    debugprintf ("httpClose()\n");
    httpClose (self->http);
    free (self->host);
    free (self->cb_password);
  }

  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

static PyObject *
Connection_repr (Connection *self)
{
  char buffer[256];
  snprintf (buffer, 256, "<cups.Connection object for %s at %p>",
			  self->host, self);
  return PyUnicode_FromString (buffer);
}

void
Connection_begin_allow_threads (void *connection)
{
  Connection *self = (Connection *) connection;
  debugprintf ("begin allow threads\n");

  self->tstate = PyEval_SaveThread ();
}

void
Connection_end_allow_threads (void *connection)
{
  Connection *self = (Connection *) connection;
  debugprintf ("end allow threads\n");
  PyEval_RestoreThread (self->tstate);
  self->tstate = NULL;
}

////////////////
// Connection // METHODS
////////////////

static const char *
password_callback (int newstyle,
		   const char *prompt,
		   http_t *http,
		   const char *method,
		   const char *resource,
		   void *user_data)
{
  struct TLS *tls = get_TLS ();
  PyObject *cb_context = user_data;
  Connection *self = NULL;
  PyObject *args;
  PyObject *result;
  long unsigned int i;

  debugprintf ("-> password_callback for http=%p, newstyle=%d\n",
	       http, newstyle);

  for (i = 0; i < NumConnections; i++)
    if (Connections[i]->http == http)
    {
      self = Connections[i];
      break;
    }

  if (!self)
  {
    debugprintf ("cannot find self!\n");
    return "";
  }

  Connection_end_allow_threads (self);
  if (newstyle)
  {
    /* New-style callback. */
    if (cb_context)
      args = Py_BuildValue ("(sOssO)", prompt, self, method, resource,
			    cb_context);
    else
      args = Py_BuildValue ("(sOss)", prompt, self, method, resource);
  } else
    args = Py_BuildValue ("(s)", prompt);

  result = PyEval_CallObject (tls->cups_password_callback, args);
  Py_DECREF (args);
  if (result == NULL)
  {
    debugprintf ("<- password_callback (exception)\n");
    Connection_begin_allow_threads (self);
    return NULL;
  }

  free (self->cb_password);
  if (result == Py_None ||
      !UTF8_from_PyObj (&self->cb_password, result))
    self->cb_password = NULL;

  Py_DECREF (result);
  if (!self->cb_password || !*self->cb_password)
  {
    debugprintf ("<- password_callback (empty/null)\n");
    Connection_begin_allow_threads (self);
    return NULL;
  }

  Connection_begin_allow_threads (self);
  debugprintf ("<- password_callback\n");
  return self->cb_password;
}

const char *
password_callback_oldstyle (const char *prompt,
			    http_t *http,
			    const char *method,
			    const char *resource,
			    void *user_data)
{
  return password_callback (0, prompt, http, method, resource, user_data);
}

const char *
password_callback_newstyle (const char *prompt,
			    http_t *http,
			    const char *method,
			    const char *resource,
			    void *user_data)
{
  return password_callback (1, prompt, http, method, resource, user_data);
}

static PyObject *
do_printer_request (Connection *self, PyObject *args, PyObject *kwds,
		    ipp_op_t op)
{
  PyObject *nameobj;
  PyObject *reasonobj = NULL;
  char *name;
  char uri[HTTP_MAX_URI];
  ipp_t *request, *answer;

  switch (op) {
  case IPP_PAUSE_PRINTER:
  case CUPS_REJECT_JOBS:
    {
      static char *kwlist[] = { "name", "reason", NULL };
      if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|O", kwlist,
					&nameobj, &reasonobj))
	return NULL;
      break;
    }

  default:
    if (!PyArg_ParseTuple (args, "O", &nameobj))
      return NULL;
    break;
  }

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  debugprintf ("-> do_printer_request(op:%d, name:%s)\n", (int) op, name);

  request = ippNewRequest (op);
  construct_uri (uri, sizeof (uri), "ipp://localhost/printers/", name);
  free (name);

  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, uri);

  if (reasonobj) {
    char *reason;
    if (UTF8_from_PyObj (&reason, reasonobj) == NULL) {
      ippDelete (request);
      return NULL;
    }

    debugprintf ("reason: %s\n", reason);
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_TEXT,
		  "printer-state-message", NULL, reason);
    free (reason);
  }

  debugprintf ("cupsDoRequest(\"/admin/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/admin/");
  Connection_end_allow_threads (self);
  if (PyErr_Occurred ()) {
    if (answer)
      ippDelete (answer);
    debugprintf("<- do_printer_request (error)\n");
    return NULL;
  }

  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf("<- do_printer_request (error)\n");
    return NULL;
  }

  ippDelete (answer);
  debugprintf("<- do_printer_request (None)\n");
  Py_RETURN_NONE;
}

static int
copy_dest (Dest *dst, cups_dest_t *src)
{
  int i;
  dst->is_default = src->is_default;
  dst->destname = strdup (src->name);
  dst->instance = (src->instance ? strdup (src->instance) : NULL );
  dst->num_options = src->num_options;
  dst->name = malloc (src->num_options * sizeof (char *));
  dst->value = malloc (src->num_options * sizeof (char *));
  for (i = 0; i < src->num_options; i++) {
    dst->name[i] = strdup (src->options[i].name);
    dst->value[i] = strdup (src->options[i].value);
  }
  return 0;
}

static PyObject *
Connection_getDests (Connection *self)
{
  cups_dest_t *dests;
  int num_dests;
  PyObject *pydests = PyDict_New ();
  int i;

  debugprintf ("-> Connection_getDests()\n");
  debugprintf ("cupsGetDests2()\n");
  Connection_begin_allow_threads (self);
  num_dests = cupsGetDests2 (self->http, &dests);
  Connection_end_allow_threads (self);

  // Create a dict indexed by (name,instance)
  for (i = 0; i <= num_dests; i++) {
    PyObject *largs = Py_BuildValue ("()");
    PyObject *lkwlist = Py_BuildValue ("{}");
    Dest *destobj = (Dest *) PyType_GenericNew (&cups_DestType,
						largs, lkwlist);
    Py_DECREF (largs);
    Py_DECREF (lkwlist);

    cups_dest_t *dest;
    PyObject *nameinstance;
    if (i == num_dests)
      {
	// Add a (None,None) entry for the default printer.
	dest = cupsGetDest (NULL, NULL, num_dests, dests);
	if (dest == NULL) {
	  /* No default printer. */
	  Py_DECREF ((PyObject *) destobj);
	  break;
	}

	nameinstance = Py_BuildValue ("(ss)", NULL, NULL);
      }
    else
      {
	dest = dests + i;
	nameinstance = Py_BuildValue ("(ss)", dest->name, dest->instance);
      }

    copy_dest (destobj, dest);

    PyDict_SetItem (pydests, nameinstance, (PyObject *) destobj);
    Py_DECREF ((PyObject *) destobj);
  }

  debugprintf ("cupsFreeDests()\n");
  cupsFreeDests (num_dests, dests);
  debugprintf ("<- Connection_getDests()\n");
  return pydests;
}

int
cups_dest_cb (void *user_data, unsigned flags, cups_dest_t *dest)
{
  CallbackContext *context = user_data;
  PyObject *largs = Py_BuildValue ("()");
  PyObject *lkwlist = Py_BuildValue ("{}");
  Dest *destobj;
  PyObject *args;
  PyObject *result;
  int ret = 0;

  debugprintf ("-> cups_dest_cb\n");
  destobj = (Dest *) PyType_GenericNew (&cups_DestType,
					largs, lkwlist);
  Py_DECREF (largs);
  Py_DECREF (lkwlist);
  copy_dest (destobj, dest);
  args = Py_BuildValue ("(OiO)",
			context->user_data,
			flags,
			destobj);
  Py_DECREF ((PyObject *) destobj);

  result = PyEval_CallObject (context->cb, args);
  Py_DECREF (args);
  if (result == NULL) {
    debugprintf ("<- cups_dest_cb (exception from cb func)\n");
    ret = 0;
  }


  if (result && PyLong_Check (result)) {
    ret = PyLong_AsLong (result);
    debugprintf ("   cups_dest_cb: cb func returned %d\n", ret);
  }

  debugprintf ("<- cups_dest_cb (%d)\n", ret);

  return ret;
}

static PyObject *
PyObject_from_attr_value (ipp_attribute_t *attr, int i)
{
  PyObject *val = NULL;
  char unknown[100];
  int lower, upper;
  int xres, yres;
  ipp_res_t units;

  switch (ippGetValueTag(attr)) {
  case IPP_TAG_NAME:
  case IPP_TAG_TEXT:
  case IPP_TAG_KEYWORD:
  case IPP_TAG_URI:
  case IPP_TAG_CHARSET:
  case IPP_TAG_MIMETYPE:
  case IPP_TAG_LANGUAGE:
    val = PyObj_from_UTF8 (ippGetString (attr, i, NULL));
    break;
  case IPP_TAG_INTEGER:
  case IPP_TAG_ENUM:
    val = PyLong_FromLong (ippGetInteger (attr, i));
    break;
  case IPP_TAG_BOOLEAN:
    val = PyBool_FromLong (ippGetBoolean (attr, i));
    break;
  case IPP_TAG_RANGE:
    lower = ippGetRange (attr, i, &upper);
    val = Py_BuildValue ("(ii)",
			 lower,
			 upper);
    break;
  case IPP_TAG_NOVALUE:
    Py_RETURN_NONE;
    break;

    // TODO:
  case IPP_TAG_DATE:
    val = PyUnicode_FromString ("(IPP_TAG_DATE)");
    break;
  case IPP_TAG_RESOLUTION:
    xres = ippGetResolution(attr, i, &yres, &units);
    val = Py_BuildValue ("(iii)",
			 xres,
			 yres,
			 units);
    break;
  default:
    snprintf (unknown, sizeof (unknown),
	      "(unknown IPP value tag 0x%x)", ippGetValueTag(attr));
    val = PyUnicode_FromString (unknown);
    break;
  }

  return val;
}

static PyObject *
PyList_from_attr_values (ipp_attribute_t *attr)
{
  PyObject *list = PyList_New (0);
  int i;
  debugprintf ("-> PyList_from_attr_values()\n");
  for (i = 0; i < ippGetCount (attr); i++) {
    PyObject *val = PyObject_from_attr_value (attr, i);
    if (val) {
      PyList_Append (list, val);
      Py_DECREF (val);
    }
  }

  debugprintf ("<- PyList_from_attr_values()\n");
  return list;
}

static PyObject *
Connection_getPrinters (Connection *self)
{
  PyObject *result;
  ipp_t *request = ippNewRequest(CUPS_GET_PRINTERS), *answer;
  ipp_attribute_t *attr;
  const char *attributes[] = {
    "printer-name",
    "printer-type",
    "printer-location",
    "printer-info",
    "printer-make-and-model",
    "printer-state",
    "printer-state-message",
    "printer-state-reasons",
    "printer-uri-supported",
    "device-uri",
    "printer-is-shared",
  };

  debugprintf ("-> Connection_getPrinters()\n");

  ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		 "requested-attributes",
		 sizeof (attributes) / sizeof (attributes[0]),
		 NULL, attributes);
  debugprintf ("cupsDoRequest(\"/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    if (answer && ippGetStatusCode (answer) == IPP_NOT_FOUND) {
      // No printers.
      debugprintf ("<- Connection_getPrinters() = {} (no printers)\n");
      ippDelete (answer);
      return PyDict_New ();
    }

    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getPrinters() (error)\n");
    return NULL;
  }

  result = PyDict_New ();
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer)) {
    PyObject *dict;
    char *printer = NULL;

    while (attr && ippGetGroupTag (attr) != IPP_TAG_PRINTER)
      attr = ippNextAttribute (answer);

    if (!attr)
      break;

    dict = PyDict_New ();
    for (; attr && ippGetGroupTag (attr) == IPP_TAG_PRINTER;
	 attr = ippNextAttribute (answer)) {
      PyObject *val = NULL;

      debugprintf ("Attribute: %s\n", ippGetName (attr));
      if (!strcmp (ippGetName (attr), "printer-name") &&
	  ippGetValueTag (attr) == IPP_TAG_NAME)
	printer = (char *) ippGetString (attr, 0, NULL);
      else if ((!strcmp (ippGetName (attr), "printer-type") ||
		!strcmp (ippGetName (attr), "printer-state")) &&
	       ippGetValueTag (attr) == IPP_TAG_ENUM) {
	int ptype = ippGetInteger (attr, 0);
	val = PyLong_FromLong (ptype);
      }
      else if ((!strcmp (ippGetName (attr),
			 "printer-make-and-model") ||
		!strcmp (ippGetName (attr), "printer-info") ||
		!strcmp (ippGetName (attr), "printer-location") ||
		!strcmp (ippGetName (attr), "printer-state-message")) &&
	       ippGetValueTag (attr) == IPP_TAG_TEXT) {
	val = PyObj_from_UTF8 (ippGetString (attr, 0, NULL));
      }
      else if (!strcmp (ippGetName (attr),
			"printer-state-reasons") &&
	       ippGetValueTag (attr) == IPP_TAG_KEYWORD) {
	val = PyList_from_attr_values (attr);
      }
      else if (!strcmp (ippGetName (attr),
			"printer-is-accepting-jobs") &&
	       ippGetValueTag (attr) == IPP_TAG_BOOLEAN) {
	int b = ippGetBoolean (attr, 0);
	val = PyLong_FromLong (b);
      }
      else if ((!strcmp (ippGetName (attr),
			 "printer-up-time") ||
		!strcmp (ippGetName (attr),
			 "queued-job-count")) &&
	       ippGetValueTag (attr) == IPP_TAG_INTEGER) {
	int u = ippGetInteger (attr, 0);
	val = PyLong_FromLong (u);
      }
      else if ((!strcmp (ippGetName (attr), "device-uri") ||
		!strcmp (ippGetName (attr), "printer-uri-supported")) &&
	       ippGetValueTag (attr) == IPP_TAG_URI) {
	val = PyObj_from_UTF8 (ippGetString (attr, 0, NULL));
      }
      else if (!strcmp (ippGetName (attr), "printer-is-shared") &&
	       ippGetValueTag (attr) == IPP_TAG_BOOLEAN) {
	val = PyBool_FromLong (ippGetBoolean (attr, 0));
      }

      if (val) {
	debugprintf ("Added %s to dict\n", ippGetName (attr));
	PyDict_SetItemString (dict, ippGetName (attr), val);
	Py_DECREF (val);
      }
    }

    if (printer) {
      PyObject *key = PyObj_from_UTF8 (printer);
      PyDict_SetItem (result, key, dict);
      Py_DECREF (key);
    }

    Py_DECREF (dict);
    if (!attr)
      break;
  }

  ippDelete (answer);
  debugprintf ("<- Connection_getPrinters() = dict\n");
  return result;
}

static PyObject *
Connection_getClasses (Connection *self)
{
  PyObject *result;
  ipp_t *request = ippNewRequest(CUPS_GET_CLASSES), *answer;
  ipp_attribute_t *attr;
  const char *attributes[] = {
    "printer-name",
    "member-names",
  };

  debugprintf ("-> Connection_getClasses()\n");
  ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		 "requested-attributes",
		 sizeof (attributes) / sizeof (attributes[0]),
		 NULL, attributes);
  debugprintf ("cupsDoRequest(\"/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    if (answer && ippGetStatusCode (answer) == IPP_NOT_FOUND) {
      // No classes.
      debugprintf ("<- Connection_getClasses() = {} (no classes)\n");
      ippDelete (answer);
      return PyDict_New ();
    }

    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getClasses() (error)\n");
    return NULL;
  }

  result = PyDict_New ();
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer)) {
    PyObject *members = NULL;
    char *classname = NULL;
    char *printer_uri = NULL;

    while (attr && ippGetGroupTag (attr) != IPP_TAG_PRINTER)
      attr = ippNextAttribute (answer);

    if (!attr)
      break;

     for (; attr && ippGetGroupTag (attr) == IPP_TAG_PRINTER;
	 attr = ippNextAttribute (answer)) {
      debugprintf ("Attribute: %s\n", ippGetName (attr));
      if (!strcmp (ippGetName (attr), "printer-name") &&
	  ippGetValueTag (attr) == IPP_TAG_NAME)
	classname = (char *) ippGetString (attr, 0, NULL);
      else if (!strcmp (ippGetName (attr), "printer-uri-supported") &&
	       ippGetValueTag (attr) == IPP_TAG_URI)
	printer_uri = (char *) ippGetString (attr, 0, NULL);
      else if (!strcmp (ippGetName (attr), "member-names") &&
	       ippGetValueTag (attr) == IPP_TAG_NAME) {
	Py_XDECREF (members);
	members = PyList_from_attr_values (attr);
      }
    }

    if (printer_uri) {
      Py_XDECREF (members);
      members = PyObj_from_UTF8 (printer_uri);
    }

    if (!members)
      members = PyList_New (0);

    if (classname) {
      PyObject *key = PyObj_from_UTF8 (classname);
      debugprintf ("Added class %s\n", classname);
      PyDict_SetItem (result, key, members);
      Py_DECREF (key);
    }

    Py_DECREF (members);
    if (!attr)
      break;
  }

  ippDelete (answer);
  debugprintf ("<- Connection_getClasses() = dict\n");
  return result;
}

static PyObject *
do_getPPDs (Connection *self, PyObject *args, PyObject *kwds, int all_lists)
{
  PyObject *result = NULL;
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  int limit = 0;
  PyObject *exclude_schemes_obj = NULL;	/* string list */
  PyObject *include_schemes_obj = NULL;	/* string list */
  char *ppd_natural_language = NULL;
  PyObject *ppd_device_id_obj = NULL;	/* UTF-8 string */
  char *ppd_device_id;
  PyObject *ppd_make_obj = NULL;	/* UTF-8 string */
  char *ppd_make;
  PyObject *ppd_make_and_model_obj = NULL; /* UTF-8 string */
  char *ppd_make_and_model;
  int ppd_model_number = -1;
  PyObject *ppd_product_obj = NULL;	/* UTF-8 string */
  char *ppd_product;
  PyObject *ppd_psversion_obj = NULL;	/* UTF-8 string */
  char *ppd_psversion;
  char *ppd_type = NULL;
  static char *kwlist[] = { "limit",
			    "exclude_schemes",
			    "include_schemes",
			    "ppd_natural_language",
			    "ppd_device_id",
			    "ppd_make",
			    "ppd_make_and_model",
			    "ppd_model_number",
			    "ppd_product",
			    "ppd_psversion",
			    "ppd_type",
			    NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "|iOOsOOOiOOs", kwlist,
				    &limit,
				    &exclude_schemes_obj, &include_schemes_obj,
				    &ppd_natural_language,
				    &ppd_device_id_obj, &ppd_make_obj,
				    &ppd_make_and_model_obj,
				    &ppd_model_number,
				    &ppd_product_obj, &ppd_psversion_obj,
				    &ppd_type))
    return NULL;

  request = ippNewRequest(CUPS_GET_PPDS);
  if (limit > 0)
    ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		   "limit", limit);

  if (exclude_schemes_obj)
    {
      size_t i, n;
      char **ss;
      if (!PyList_Check (exclude_schemes_obj))
	{
	  PyErr_SetString (PyExc_TypeError, "List required (exclude_schemes)");
	  ippDelete (request);
	  return NULL;
	}

      n = PyList_Size (exclude_schemes_obj);
      ss = calloc (n + 1, sizeof (char *));
      for (i = 0; i < n; i++)
	{
	  PyObject *val = PyList_GetItem (exclude_schemes_obj, i); // borrowed
	  if (!PyUnicode_Check (val) && !PyBytes_Check (val))
	    {
	      PyErr_SetString (PyExc_TypeError,
			       "String list required (exclude_schemes)");
	      ippDelete (request);
	      while (i > 0)
		free (ss[--i]);
	      free (ss);
	      return NULL;
	    }

	  UTF8_from_PyObj (&ss[i], val);
	}
      ss[n] = NULL;
      ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		     "exclude-schemes", n, NULL, (const char **) ss);
      for (i = 0; i < n; i++)
	free (ss[i]);
      free (ss);
    }

  if (include_schemes_obj)
    {
      size_t i, n;
      char **ss;
      if (!PyList_Check (include_schemes_obj))
	{
	  PyErr_SetString (PyExc_TypeError, "List required (include_schemes)");
	  ippDelete (request);
	  return NULL;
	}

      n = PyList_Size (include_schemes_obj);
      ss = calloc (n + 1, sizeof (char *));
      for (i = 0; i < n; i++)
	{
	  PyObject *val = PyList_GetItem (include_schemes_obj, i); // borrowed
	  if (!PyUnicode_Check (val) && !PyBytes_Check (val))
	    {
	      PyErr_SetString (PyExc_TypeError,
			       "String list required (include_schemes)");
	      ippDelete (request);
	      while (i > 0)
		free (ss[--i]);
	      free (ss);
	      return NULL;
	    }

	  UTF8_from_PyObj (&ss[i], val);
	}
      ss[n] = NULL;
      ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		     "include-schemes", n, NULL, (const char **) ss);
      for (i = 0; i < n; i++)
	free (ss[i]);
      free (ss);
    }

  if (ppd_device_id_obj)
    {
      if (UTF8_from_PyObj (&ppd_device_id, ppd_device_id_obj) == NULL)
	{
	  ippDelete (request);
	  return NULL;
	}

      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_TEXT,
		    "ppd-device-id", NULL, ppd_device_id);
      free (ppd_device_id);
    }

  if (ppd_make_obj)
    {
      if (UTF8_from_PyObj (&ppd_make, ppd_make_obj) == NULL)
	{
	  ippDelete (request);
	  return NULL;
	}

      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_TEXT,
		    "ppd-make", NULL, ppd_make);
      free (ppd_make);
    }

  if (ppd_make_and_model_obj)
    {
      if (UTF8_from_PyObj (&ppd_make_and_model, ppd_make_and_model_obj) == NULL)
	{
	  ippDelete (request);
	  return NULL;
	}

      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_TEXT,
		    "ppd-make-and-model", NULL, ppd_make_and_model);
      free (ppd_make_and_model);
    }

  if (ppd_model_number >= 0)
    ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		   "ppd-model-number", ppd_model_number);

  if (ppd_product_obj)
    {
      if (UTF8_from_PyObj (&ppd_product, ppd_product_obj) == NULL)
	{
	  ippDelete (request);
	  return NULL;
	}

      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_TEXT,
		    "ppd-product", NULL, ppd_product);
      free (ppd_product);
    }

  if (ppd_psversion_obj)
    {
      if (UTF8_from_PyObj (&ppd_psversion, ppd_psversion_obj) == NULL)
	{
	  ippDelete (request);
	  return NULL;
	}

      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_TEXT,
		    "ppd-psversion", NULL, ppd_psversion);
      free (ppd_psversion);
    }

  if (ppd_natural_language)
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_LANGUAGE,
		  "ppd-natural-language", NULL, ppd_natural_language);

  if (ppd_type)
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		  "ppd-type", NULL, ppd_type);

  debugprintf ("-> Connection_getPPDs()\n");
  debugprintf ("cupsDoRequest(\"/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getPPDs() (error)\n");
    return NULL;
  }

  result = PyDict_New ();
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer)) {
    PyObject *dict;
    char *ppdname = NULL;

    while (attr && ippGetGroupTag (attr) != IPP_TAG_PRINTER)
      attr = ippNextAttribute (answer);

    if (!attr)
      break;

    dict = PyDict_New ();
    for (; attr && ippGetGroupTag (attr) == IPP_TAG_PRINTER;
	 attr = ippNextAttribute (answer)) {
      PyObject *val = NULL;
      debugprintf ("Attribute: %s\n", ippGetName (attr));
      if (!strcmp (ippGetName (attr), "ppd-name") &&
	  ippGetValueTag (attr) == IPP_TAG_NAME)
	ppdname = (char *) ippGetString (attr, 0, NULL);
      else {
	if (all_lists)
	  val = PyList_from_attr_values (attr);
	else
	  val = PyObject_from_attr_value (attr, 0);

	if (val) {
	  debugprintf ("Adding %s to ppd dict\n", ippGetName (attr));
	  PyDict_SetItemString (dict, ippGetName (attr), val);
	  Py_DECREF (val);
	}
      }
    }

    if (ppdname) {
      PyObject *key = PyObj_from_UTF8 (ppdname);
      debugprintf ("Adding %s to result dict\n", ppdname);
      PyDict_SetItem (result, key, dict);
      Py_DECREF (key);
    }

    Py_DECREF (dict);
    if (!attr)
      break;
  }

  ippDelete (answer);
  debugprintf ("<- Connection_getPPDs() = dict\n");
  return result;
}

static PyObject *
Connection_getPPDs (Connection *self, PyObject *args, PyObject *kwds)
{
  return do_getPPDs (self, args, kwds, 0);
}

static PyObject *
Connection_getPPDs2 (Connection *self, PyObject *args, PyObject *kwds)
{
  return do_getPPDs (self, args, kwds, 1);
}

static PyObject *
Connection_getServerPPD (Connection *self, PyObject *args)
{
  const char *ppd_name, *filename;
  if (!PyArg_ParseTuple (args, "s", &ppd_name))
    return NULL;
  debugprintf ("-> Connection_getServerPPD()\n");
  Connection_begin_allow_threads (self);
  filename = cupsGetServerPPD (self->http, ppd_name);
  Connection_end_allow_threads (self);
  if (!filename) {
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    debugprintf ("<- Connection_getServerPPD() (error)\n");
    return NULL;
  }
  debugprintf ("<- Connection_getServerPPD(\"%s\") = \"%s\"\n",
	       ppd_name, filename);
  return PyUnicode_FromString (filename);
}

static PyObject *
Connection_getDocument (Connection *self, PyObject *args)
{
  PyObject *dict;
  PyObject *obj;
  PyObject *uriobj;
  char *uri;
  int jobid, docnum;
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  const char *format = NULL;
  const char *name = NULL;
  char docfilename[PATH_MAX];
  int fd;

  if (!PyArg_ParseTuple (args, "Oii", &uriobj, &jobid, &docnum))
    return NULL;

  if (UTF8_from_PyObj (&uri, uriobj) == NULL)
    return NULL;

  debugprintf ("-> Connection_getDocument(\"%s\",%d)\n", uri, jobid);
  request = ippNewRequest (CUPS_GET_DOCUMENT);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, uri);
  free (uri);
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		 "job-id", jobid);
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		 "document-number", docnum);

  snprintf(docfilename, sizeof (docfilename), "%s/jobdoc-XXXXXX", _PATH_TMP);
  fd = mkstemp (docfilename);
  if (fd < 0) {
    debugprintf ("<- Connection_getDocument() EXCEPTION\n");
    ippDelete (request);
    return PyErr_SetFromErrno (PyExc_RuntimeError);
  }

  Connection_begin_allow_threads (self);
  answer = cupsDoIORequest (self->http, request, "/", -1, fd);
  Connection_end_allow_threads (self);

  close (fd);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    unlink (docfilename);
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getDocument() (error)\n");
    return NULL;
  }

  if ((attr = ippFindAttribute (answer, "document-format",
				IPP_TAG_MIMETYPE)) != NULL)
    format = ippGetString (attr, 0, NULL);

  if ((attr = ippFindAttribute (answer, "document-name",
				IPP_TAG_NAME)) != NULL)
    name = ippGetString (attr, 0, NULL);

  dict = PyDict_New ();

  obj = PyUnicode_FromString (docfilename);
  PyDict_SetItemString (dict, "file", obj);
  Py_DECREF (obj);

  if (format) {
    obj = PyUnicode_FromString (format);
    PyDict_SetItemString (dict, "document-format", obj);
    Py_DECREF (obj);
  }

  if (name) {
    obj = PyObj_from_UTF8 (name);
    PyDict_SetItemString (dict, "document-name", obj);
    Py_DECREF (obj);
  }

  debugprintf ("<- Connection_getDocument() = {'file':\"%s\","
	       "'document-format':\"%s\",'document-name':\"%s\"}\n",
	       docfilename, format ? format : "(nul)",
	       name ? name : "(nul)");
  ippDelete (answer);
  return dict;
}

static PyObject *
Connection_getDevices (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *result;
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  int limit = 0;
  int timeout = 0;
  PyObject *exclude_schemes = NULL;
  PyObject *include_schemes = NULL;
  static char *kwlist[] = { "limit",
			    "exclude_schemes",
			    "include_schemes",
			    "timeout",
			    NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "|iOOi", kwlist, &limit,
				    &exclude_schemes, &include_schemes,
				    &timeout))
    return NULL;

  request = ippNewRequest(CUPS_GET_DEVICES);
  if (limit > 0)
    ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		   "limit", limit);

  if (exclude_schemes)
    {
      size_t i, n;
      char **ss;
      if (!PyList_Check (exclude_schemes))
	{
	  PyErr_SetString (PyExc_TypeError, "List required (exclude_schemes)");
	  ippDelete (request);
	  return NULL;
	}

      n = PyList_Size (exclude_schemes);
      ss = calloc (n + 1, sizeof (char *));
      for (i = 0; i < n; i++)
	{
	  PyObject *val = PyList_GetItem (exclude_schemes, i); // borrowed ref
	  if (!PyUnicode_Check (val) && !PyBytes_Check (val))
	    {
	      PyErr_SetString (PyExc_TypeError,
			       "String list required (exclude_schemes)");
	      ippDelete (request);
	      while (i > 0)
		free (ss[--i]);
	      free (ss);
	      return NULL;
	    }

	  UTF8_from_PyObj (&ss[i], val);
	}

      ss[n] = NULL;
      ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		     "exclude-schemes", n, NULL, (const char **) ss);
      for (i = 0; i < n; i++)
	free (ss[i]);
      free (ss);
    }

  if (include_schemes)
    {
      size_t i, n;
      char **ss;
      if (!PyList_Check (include_schemes))
	{
	  PyErr_SetString (PyExc_TypeError, "List required (include_schemes)");
	  ippDelete (request);
	  return NULL;
	}

      n = PyList_Size (include_schemes);
      ss = calloc (n + 1, sizeof (char *));
      for (i = 0; i < n; i++)
	{
	  PyObject *val = PyList_GetItem (include_schemes, i); // borrowed ref
	  if (!PyUnicode_Check (val) && !PyBytes_Check (val))
	    {
	      PyErr_SetString (PyExc_TypeError,
			       "String list required (include_schemes)");
	      ippDelete (request);
	      while (i > 0)
		free (ss[--i]);
	      free (ss);
	      return NULL;
	    }

	  UTF8_from_PyObj (&ss[i], val);
	}

      ss[n] = NULL;
      ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		     "include-schemes", n, NULL, (const char **) ss);
      for (i = 0; i < n; i++)
	free (ss[i]);
      free (ss);
    }

  if (timeout > 0)
    ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		   "timeout", timeout);

  debugprintf ("-> Connection_getDevices()\n");
  debugprintf ("cupsDoRequest(\"/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getDevices() (error)\n");
    return NULL;
  }

  result = PyDict_New ();
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer)) {
    PyObject *dict;
    char *device_uri = NULL;

    while (attr && ippGetGroupTag (attr) != IPP_TAG_PRINTER)
      attr = ippNextAttribute (answer);

    if (!attr)
      break;

    dict = PyDict_New ();
    for (; attr && ippGetGroupTag (attr) == IPP_TAG_PRINTER;
	 attr = ippNextAttribute (answer)) {
      PyObject *val = NULL;

      debugprintf ("Attribute: %s\n", ippGetName (attr));
      if (!strcmp (ippGetName (attr), "device-uri") &&
	  ippGetValueTag (attr) == IPP_TAG_URI)
	device_uri = (char *) ippGetString (attr, 0, NULL);
      else
	val = PyObject_from_attr_value (attr, 0);

      if (val) {
	debugprintf ("Adding %s to device dict\n", ippGetName (attr));
	PyDict_SetItemString (dict, ippGetName (attr), val);
	Py_DECREF (val);
      }
    }

    if (device_uri) {
      PyObject *key = PyObj_from_UTF8 (device_uri);
      debugprintf ("Adding %s to result dict\n", device_uri);
      PyDict_SetItem (result, key, dict);
      Py_DECREF (key);
    }

    Py_DECREF (dict);
    if (!attr)
      break;
  }

  ippDelete (answer);
  debugprintf ("<- Connection_getDevices() = dict\n");
  return result;
}

static int
get_requested_attrs (PyObject *requested_attrs, size_t *n_attrs, char ***attrs)
{
  long unsigned int i;
  size_t n;
  char **as;

  if (!PyList_Check (requested_attrs)) {
    PyErr_SetString (PyExc_TypeError, "List required");
    return -1;
  }

  n = PyList_Size (requested_attrs);
  as = malloc ((n + 1) * sizeof (char *));
  for (i = 0; i < n; i++) {
    PyObject *val = PyList_GetItem (requested_attrs, i); // borrowed ref
    if (!PyUnicode_Check (val) && !PyBytes_Check (val)) {
      PyErr_SetString (PyExc_TypeError, "String required");
      while (--i >= 0)
	free (as[i]);
      free (as);
      return -1;
    }

    UTF8_from_PyObj (&as[i], val);
  }
  as[n] = NULL;

  debugprintf ("Requested attributes:\n");
  for (i = 0; as[i] != NULL; i++)
    debugprintf ("  %s\n", as[i]);

  *n_attrs = n;
  *attrs = as;
  return 0;
}

static void
free_requested_attrs (size_t n_attrs, char **attrs)
{
  long unsigned int i;
  for (i = 0; i < n_attrs; i++)
    free (attrs[i]);
  free (attrs);
}

static PyObject *
Connection_getJobs (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *result;
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  char *which = NULL;
  int my_jobs = 0;
  int limit = -1;
  int first_job_id = -1;
  PyObject *requested_attrs = NULL;
  char **attrs = NULL; /* initialised to calm compiler */
  size_t n_attrs = 0; /* initialised to calm compiler */
  static char *kwlist[] = { "which_jobs", "my_jobs", "limit", "first_job_id", 
			    "requested_attributes", NULL };
  if (!PyArg_ParseTupleAndKeywords (args, kwds, "|siiiO", kwlist,
				    &which, &my_jobs, &limit, &first_job_id,
				    &requested_attrs))
    return NULL;

  debugprintf ("-> Connection_getJobs(%s,%d)\n",
	       which ? which : "(null)", my_jobs);
  request = ippNewRequest(IPP_GET_JOBS);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri",
		NULL, "ipp://localhost/printers/");

  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs",
		NULL, which ? which : "not-completed");

  ippAddBoolean (request, IPP_TAG_OPERATION, "my-jobs", my_jobs);
  if (my_jobs)
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		  "requesting-user-name", NULL, cupsUser());

  if (limit > 0)
    ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		   "limit", limit);

  if (first_job_id > 0)
    ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		   "first-job-id", first_job_id);

  if (requested_attrs) {
    if (get_requested_attrs (requested_attrs, &n_attrs, &attrs) == -1) {
      ippDelete (request);
      return NULL;
    }

    ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		   "requested-attributes", n_attrs, NULL,
		   (const char **) attrs);
    free_requested_attrs (n_attrs, attrs);
  }

  debugprintf ("cupsDoRequest(\"/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getJobs() (error)\n");
    return NULL;
  }

  result = PyDict_New ();
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer)) {
    PyObject *dict;
    int job_id = -1;

    while (attr && ippGetGroupTag (attr) != IPP_TAG_JOB)
      attr = ippNextAttribute (answer);

    if (!attr)
      break;

    dict = PyDict_New ();
    for (; attr && ippGetGroupTag (attr) == IPP_TAG_JOB;
	 attr = ippNextAttribute (answer)) {
      PyObject *val = NULL;

      debugprintf ("Attribute: %s\n", ippGetName (attr));
      if (!strcmp (ippGetName (attr), "job-id") &&
	  ippGetValueTag (attr) == IPP_TAG_INTEGER)
	job_id = ippGetInteger (attr, 0);
      else if (((!strcmp (ippGetName (attr), "job-k-octets") ||
		 !strcmp (ippGetName (attr), "job-priority") ||
		 !strcmp (ippGetName (attr), "time-at-creation") ||
		 !strcmp (ippGetName (attr), "time-at-processing") ||
		 !strcmp (ippGetName (attr), "time-at-completed") ||
		 !strcmp (ippGetName (attr), "job-media-sheets") ||
		 !strcmp (ippGetName (attr), "job-media-sheets-completed")) &&
		ippGetValueTag (attr) == IPP_TAG_INTEGER) ||
	       (!strcmp (ippGetName (attr), "job-state") &&
		ippGetValueTag (attr) == IPP_TAG_ENUM))
	val = PyLong_FromLong (ippGetInteger (attr, 0));
      else if ((!strcmp (ippGetName (attr), "job-name") &&
		ippGetValueTag (attr) == IPP_TAG_NAME) ||
	       (!strcmp (ippGetName (attr), "job-originating-user-name") &&
		ippGetValueTag (attr) == IPP_TAG_NAME) ||
	       (!strcmp (ippGetName (attr), "job-printer-uri") &&
		ippGetValueTag (attr) == IPP_TAG_URI))
	val = PyObj_from_UTF8 (ippGetString (attr, 0, NULL));
      else if (!strcmp (ippGetName (attr), "job-preserved") &&
	       ippGetValueTag (attr) == IPP_TAG_BOOLEAN)
	val = PyBool_FromLong (ippGetInteger (attr, 0));
      else {
	if (ippGetCount (attr) > 1)
	  val = PyList_from_attr_values (attr);
	else
	  val = PyObject_from_attr_value (attr, 0);
      }

      if (val) {
	debugprintf ("Adding %s to job dict\n", ippGetName (attr));
	PyDict_SetItemString (dict, ippGetName (attr), val);
	Py_DECREF (val);
      }
    }

    if (job_id != -1) {
      debugprintf ("Adding %d to result dict\n", job_id);
      PyObject *job_obj = PyLong_FromLong (job_id);
      PyDict_SetItem (result, job_obj, dict);
      Py_DECREF (job_obj);
    }

    Py_DECREF (dict);

    if (!attr)
      break;
  }

  ippDelete (answer);
  debugprintf ("<- Connection_getJobs() = dict\n");
  return result;
}

static PyObject *
Connection_getJobAttributes (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *result;
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  int job_id;
  PyObject *requested_attrs = NULL;
  char **attrs = NULL; /* initialised to calm compiler */
  size_t n_attrs = 0; /* initialised to calm compiler */
  char uri[1024];
  static char *kwlist[] = { "job_id", "requested_attributes", NULL };
  if (!PyArg_ParseTupleAndKeywords (args, kwds, "i|O", kwlist,
				    &job_id, &requested_attrs))
    return NULL;

  if (requested_attrs) {
    if (get_requested_attrs (requested_attrs, &n_attrs, &attrs) == -1)
      return NULL;
  }

  debugprintf ("-> Connection_getJobAttributes(%d)\n", job_id);
  request = ippNewRequest(IPP_GET_JOB_ATTRIBUTES);
  snprintf (uri, sizeof (uri), "ipp://localhost/jobs/%d", job_id);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri",
		NULL, uri);
  if (requested_attrs)
    ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		   "requested-attributes", n_attrs, NULL,
		   (const char **) attrs);

  debugprintf ("cupsDoRequest(\"/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (requested_attrs)
    free_requested_attrs (n_attrs, attrs);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getJobAttributes() (error)\n");
    return NULL;
  }

  result = PyDict_New ();
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer)) {
    PyObject *obj;

    debugprintf ("Attr: %s\n", ippGetName (attr));
    if (ippGetCount (attr) > 1 ||
	!strcmp (ippGetName (attr), "job-printer-state-reasons"))
      obj = PyList_from_attr_values (attr);
    else
      obj = PyObject_from_attr_value (attr, 0);

    if (!obj)
      // Can't represent this.
      continue;

    PyDict_SetItemString (result, ippGetName (attr), obj);
    Py_DECREF (obj);
  }

  ippDelete (answer);
  debugprintf ("<- Connection_getJobAttributes() = dict\n");
  return result;
}

static PyObject *
Connection_cancelJob (Connection *self, PyObject *args, PyObject *kwds)
{
  ipp_t *request, *answer;
  int job_id;
  int purge_job = 0;
  char uri[1024];
  static char *kwlist[] = { "job_id", "purge_job", NULL };
  if (!PyArg_ParseTupleAndKeywords (args, kwds, "i|i", kwlist,
				    &job_id, &purge_job))
    return NULL;

  debugprintf ("-> Connection_cancelJob(%d)\n", job_id);
  request = ippNewRequest(IPP_CANCEL_JOB);
  snprintf (uri, sizeof (uri), "ipp://localhost/jobs/%d", job_id);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", NULL, uri);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  if (purge_job)
    ippAddBoolean(request, IPP_TAG_OPERATION, "purge-job", 1);
  debugprintf ("cupsDoRequest(\"/jobs/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/jobs/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_cancelJob() (error)\n");
    return NULL;
  }

  debugprintf ("<- Connection_cancelJob() = None\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_cancelAllJobs (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *nameobj = NULL;
  char *name;
  PyObject *uriobj = NULL;
  char *uri;
  char consuri[HTTP_MAX_URI];
  ipp_t *request, *answer;
  int my_jobs = 0;
  int purge_jobs = 1;
  int i;
  static char *kwlist[] = { "name", "uri", "my_jobs", "purge_jobs", NULL };
  if (!PyArg_ParseTupleAndKeywords (args, kwds, "|OOii", kwlist,
				    &nameobj, &uriobj, &my_jobs, &purge_jobs))
    return NULL;

  if (nameobj && uriobj) {
    PyErr_SetString (PyExc_RuntimeError,
		     "name or uri must be specified but not both");
    return NULL;
  }

  if (nameobj) {
    if (UTF8_from_PyObj (&name, nameobj) == NULL)
      return NULL;
  } else if (uriobj) {
    if (UTF8_from_PyObj (&uri, uriobj) == NULL)
      return NULL;
  } else {
    PyErr_SetString (PyExc_RuntimeError,
		     "name or uri must be specified");
    return NULL;
  }

  debugprintf ("-> Connection_cancelAllJobs(%s, my_jobs=%d, purge_jobs=%d)\n",
	       nameobj ? name : uri, my_jobs, purge_jobs);
  if (nameobj) {
    construct_uri (consuri, sizeof (consuri),
		   "ipp://localhost/printers/", name);
    uri = consuri;
  }

  for (i = 0; i < 2; i++) {
    request = ippNewRequest(IPP_PURGE_JOBS);
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri",
		  NULL, uri);

    if (my_jobs)
    {
      ippAddBoolean (request, IPP_TAG_OPERATION, "my-jobs", my_jobs);
      ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		    "requesting-user-name", NULL, cupsUser());
    }

    ippAddBoolean (request, IPP_TAG_OPERATION, "purge-jobs", purge_jobs);
    debugprintf ("cupsDoRequest(\"/admin/\") with printer-uri=%s\n", uri);
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      if (uriobj)
	break;

      // Perhaps it's a class, not a printer.
      construct_uri (consuri, sizeof (consuri),
		     "ipp://localhost/classes/", name);
    } else break;
  }

  if (nameobj)
    free (name);

  if (uriobj)
    free (uri);

  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);

    debugprintf ("<- Connection_cancelAllJobs() (error)\n");
    return NULL;
  }

  debugprintf ("<- Connection_cancelAllJobs() = None\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_createJob (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "printer", "title", "options", NULL };
  PyObject *printer_obj;
  char *printer;
  PyObject *title_obj;
  char *title;
  PyObject *options_obj, *key, *val;
  int num_settings = 0;
  DICT_POS_TYPE pos = 0;
  cups_option_t *settings = NULL;
  int jobid;

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "OOO", kwlist,
				    &printer_obj, &title_obj,
				    &options_obj))
    return NULL;

  if (UTF8_from_PyObj (&printer, printer_obj) == NULL)
    return NULL;
  if (UTF8_from_PyObj (&title, title_obj) == NULL) {
    free (printer);
    return NULL;
  }
  debugprintf ("-> Connection_createJob(printer=%s, title=%s)\n", printer, title);

  if (!PyDict_Check (options_obj)) {
    free (title);
    free (printer);
    PyErr_SetString (PyExc_TypeError, "options must be a dict");
    return NULL;
  }
  while (PyDict_Next (options_obj, &pos, &key, &val)) {
    char *name, *value;
    if ((!PyUnicode_Check (key) && !PyBytes_Check (key)) ||
        (!PyUnicode_Check (val) && !PyBytes_Check (val))) {
      cupsFreeOptions (num_settings, settings);
      free (title);
      free (printer);
      PyErr_SetString (PyExc_TypeError, "Keys and values must be strings");
      return NULL;
    }

    num_settings = cupsAddOption (UTF8_from_PyObj (&name, key),
				  UTF8_from_PyObj (&value, val),
				  num_settings,
				  &settings);
    free (name);
    free (value);
  }

  Connection_begin_allow_threads (self);
  jobid = cupsCreateJob(self->http, printer, title, num_settings, settings);
  Connection_end_allow_threads (self);

  if (jobid == 0) {
    cupsFreeOptions (num_settings, settings);
    free (title);
    free (printer);
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    debugprintf ("<- Connection_createJob() = NULL\n");
    return NULL;
  }

  cupsFreeOptions (num_settings, settings);
  free (title);
  free (printer);
  debugprintf ("<- Connection_createJob() = %d\n", jobid);
  return PyLong_FromLong (jobid);
}

static PyObject *
Connection_startDocument (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "printer", "job_id", "doc_name", "format", "last_document", NULL };
  PyObject *printer_obj;
  char *printer;
  int jobid;
  PyObject *doc_name_obj;
  char *doc_name;
  PyObject *format_obj;
  char *format;
  int last_document;
  http_status_t	status;		/* Write status */

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "OiOOi", kwlist,
				    &printer_obj, &jobid, &doc_name_obj,
				    &format_obj, &last_document))
    return NULL;

  if (UTF8_from_PyObj (&printer, printer_obj) == NULL)
    return NULL;
  if (UTF8_from_PyObj (&doc_name, doc_name_obj) == NULL) {
    free (printer);
    return NULL;
  }
  if (UTF8_from_PyObj (&format, format_obj) == NULL) {
    free (doc_name);
    free (printer);
    return NULL;
  }
  debugprintf ("-> Connection_startDocument(printer=%s, jobid=%d, doc_name=%s, format=%s)\n",
              printer, jobid, doc_name, format);

  Connection_begin_allow_threads (self);
  status = cupsStartDocument(self->http, printer, jobid, doc_name, format, last_document);
  Connection_end_allow_threads (self);

  if (status != HTTP_CONTINUE)
  {
    free (format);
    free (doc_name);
    free (printer);
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    debugprintf ("<- Connection_startDocument() = NULL\n");
    return NULL;
  }

  free (format);
  free (doc_name);
  free (printer);
  debugprintf ("<- Connection_startDocument() = %d\n", status);
  return PyLong_FromLong (status);
}

static PyObject *
Connection_writeRequestData (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "buffer", "length", NULL };
  PyObject *buffer_obj;
  char *buffer;
  int length;
  http_status_t	status;		/* Write status */

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "Oi", kwlist,
				    &buffer_obj, &length))
    return NULL;

  buffer = (char *) malloc((size_t) length);
  memcpy(buffer, PyBytes_AsString(buffer_obj), length);
  debugprintf ("-> Connection_writeRequestData(length=%d)\n", length);

  Connection_begin_allow_threads (self);
  status = cupsWriteRequestData(self->http, buffer, length);
  Connection_end_allow_threads (self);

  if (status != HTTP_CONTINUE)
  {
    free (buffer);
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    debugprintf ("<- Connection_writeRequestData() = NULL\n");
    return NULL;
  }

  free (buffer);
  debugprintf ("<- Connection_writeRequestData() = %d\n", status);
  return PyLong_FromLong (status);
}

static PyObject *
Connection_finishDocument (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "printer", NULL };
  PyObject *printer_obj;
  char *printer;
  int answer;

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O", kwlist, &printer_obj))
    return NULL;

  if (UTF8_from_PyObj (&printer, printer_obj) == NULL)
    return NULL;

  debugprintf ("-> Connection_finishDocument(printer=%s)\n", printer);
  Connection_begin_allow_threads (self);
  answer = cupsFinishDocument(self->http, printer);
  Connection_end_allow_threads (self);

  if (answer != IPP_OK) {
    free (printer);
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    debugprintf ("<- Connection_finishDocument() = NULL\n");
    return NULL;
  }

  free (printer);
  debugprintf ("<- Connection_finishDicument() = %d\n", answer);
  return PyLong_FromLong (answer);
}

static PyObject *
Connection_moveJob (Connection *self, PyObject *args, PyObject *kwds)
{
  int job_id = -1;
  PyObject *printeruriobj = NULL;
  char *printeruri;
  PyObject *jobprinteruriobj = NULL;
  char *jobprinteruri;
  ipp_t *request, *answer;
  static char *kwlist[] = { "printer_uri", "job_id", "job_printer_uri", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "|OiO", kwlist,
				    &printeruriobj, &job_id,
				    &jobprinteruriobj))
    return NULL;

  if (!jobprinteruriobj) {
    PyErr_SetString (PyExc_RuntimeError,
		     "No job_printer_uri (destination) given");
    return NULL;
  }

  if (printeruriobj) {
    if (UTF8_from_PyObj (&printeruri, printeruriobj) == NULL)
      return NULL;
  } else if (job_id == -1) {
    PyErr_SetString (PyExc_RuntimeError,
		     "job_id or printer_uri required");
    return NULL;
  }

  if (UTF8_from_PyObj (&jobprinteruri, jobprinteruriobj) == NULL) {
    if (printeruriobj)
      free (printeruri);
    return NULL;
  }

  request = ippNewRequest (CUPS_MOVE_JOB);
  if (!printeruriobj) {
    char joburi[HTTP_MAX_URI];
    snprintf (joburi, sizeof (joburi), "ipp://localhost/jobs/%d", job_id);
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", NULL,
		  joburi);
  } else {
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL,
		  printeruri);
    free (printeruri);

    if (job_id != -1)
      ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id",
		     job_id);
  }

  ippAddString (request, IPP_TAG_JOB, IPP_TAG_URI, "job-printer-uri", NULL,
		jobprinteruri);
  free (jobprinteruri);
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/jobs");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_authenticateJob (Connection *self, PyObject *args)
{
  ipp_t *request, *answer;
  int job_id;
  PyObject *auth_info_list = NULL;
  long unsigned int num_auth_info = 0; /* initialised to calm compiler */
  char *auth_info_values[3];
  long unsigned int i;
  char uri[1024];

  if (!PyArg_ParseTuple (args, "i|O", &job_id, &auth_info_list))
    return NULL;

  if (auth_info_list) {
    if (!PyList_Check (auth_info_list)) {
      PyErr_SetString (PyExc_TypeError, "List required");
      return NULL;
    }

    num_auth_info = PyList_Size (auth_info_list);
    debugprintf ("sizeof values = %Zd\n", sizeof (auth_info_values));
    if (num_auth_info > sizeof (auth_info_values))
      num_auth_info = sizeof (auth_info_values);

    for (i = 0; i < num_auth_info; i++) {
      PyObject *val = PyList_GetItem (auth_info_list, i); // borrowed ref
      if (UTF8_from_PyObj (&auth_info_values[i], val) == NULL) {
	while (--i >= 0)
	  free (auth_info_values[i]);
	return NULL;
      }
    }
  }

  debugprintf ("-> Connection_authenticateJob(%d)\n", job_id);
  request = ippNewRequest(CUPS_AUTHENTICATE_JOB);
  snprintf (uri, sizeof (uri), "ipp://localhost/jobs/%d", job_id);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", NULL, uri);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  if (auth_info_list) 
    {
      ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_TEXT, "auth-info",
		     num_auth_info, NULL,
		     (const char *const *) auth_info_values);
      for (i = 0; i < num_auth_info; i++)
	free (auth_info_values[i]);
    }

  debugprintf ("cupsDoRequest(\"/jobs/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/jobs/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_authenticateJob() (error)\n");
    return NULL;
  }

  debugprintf ("<- Connection_authenticateJob() = None\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_setJobHoldUntil (Connection *self, PyObject *args)
{
  ipp_t *request, *answer;
  int job_id;
  PyObject *job_hold_until_obj;
  char *job_hold_until;
  char uri[1024];
  cups_option_t *options = NULL;
  int num_options = 0;
  if (!PyArg_ParseTuple (args, "iO", &job_id, &job_hold_until_obj))
    return NULL;

  if (UTF8_from_PyObj (&job_hold_until, job_hold_until_obj) == NULL)
    return NULL;

  debugprintf ("-> Connection_setJobHoldUntil(%d,%s)\n",
	       job_id, job_hold_until);
  request = ippNewRequest(IPP_SET_JOB_ATTRIBUTES);
  snprintf (uri, sizeof (uri), "ipp://localhost/jobs/%d", job_id);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", NULL, uri);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  num_options = cupsAddOption ("job-hold-until", job_hold_until,
			       num_options, &options);
  cupsEncodeOptions (request, num_options, options);
  free (job_hold_until);

  debugprintf ("cupsDoRequest(\"/jobs/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/jobs/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_setJobHoldUntil() (error)\n");
    return NULL;
  }

  debugprintf ("<- Connection_setJobHoldUntil() = None\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_restartJob (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "job_id", "job_hold_until", NULL };
  ipp_t *request, *answer;
  int job_id;
  char *job_hold_until = NULL;
  char uri[1024];
  if (!PyArg_ParseTupleAndKeywords (args, kwds, "i|s", kwlist,
				    &job_id, &job_hold_until))
    return NULL;

  debugprintf ("-> Connection_restartJob(%d)\n", job_id);
  request = ippNewRequest(IPP_RESTART_JOB);
  snprintf (uri, sizeof (uri), "ipp://localhost/jobs/%d", job_id);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", NULL, uri);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  if (job_hold_until)
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		  "job-hold-until", NULL, job_hold_until);

  debugprintf ("cupsDoRequest(\"/jobs/\")\n");
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/jobs/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_restartJob() (error)\n");
    return NULL;
  }

  debugprintf ("<- Connection_restartJob() = None\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_getFile (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "resource", "filename", "fd", "file", NULL };
  const char *resource, *filename = NULL;
  int fd = -1;
  PyObject *fileobj = NULL;
  http_status_t status;

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "s|siO", kwlist,
				    &resource, &filename, &fd, &fileobj))
    return NULL;

  if ((fd > -1 && (filename || fileobj)) ||
      (filename && fileobj)) {
    PyErr_SetString (PyExc_RuntimeError,
		     "Only one destination type may be specified");
    return NULL;
  }

  if (fileobj) {
    fd = PyObject_AsFileDescriptor(fileobj);
  }

  if (filename) {
    debugprintf ("-> Connection_getFile(%s, %s)\n", resource, filename);
    debugprintf ("cupsGetFile()\n");
    Connection_begin_allow_threads (self);
    status = cupsGetFile (self->http, resource, filename);
    Connection_end_allow_threads (self);
  } else {
    debugprintf ("-> Connection_getFile(%s, %d)\n", resource, fd);
    debugprintf ("cupsGetFd()\n");
    Connection_begin_allow_threads (self);
    status = cupsGetFd (self->http, resource, fd);
    Connection_end_allow_threads (self);
  }

  if (status != HTTP_OK) {
    set_http_error (status);
    debugprintf ("<- Connection_getFile() (error)\n");
    return NULL;
  }

  debugprintf ("<- Connection_getFile() = None\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_putFile (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "resource", "filename", "fd", "file", NULL };
  const char *resource, *filename = NULL;
  int fd = -1;
  PyObject *fileobj = NULL;
  http_status_t status;

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "s|siO", kwlist,
				    &resource, &filename, &fd, &fileobj))
    return NULL;

  if ((fd > -1 && (filename || fileobj)) ||
      (filename && fileobj)) {
    PyErr_SetString (PyExc_RuntimeError,
		     "Only one destination type may be specified");
    return NULL;
  }

  if (fileobj) {
    fd = PyObject_AsFileDescriptor(fileobj);
  }

  if (filename) {
    debugprintf ("-> Connection_putFile(%s, %s)\n", resource, filename);
    debugprintf ("cupsPutFile()\n");
    Connection_begin_allow_threads (self);
    status = cupsPutFile (self->http, resource, filename);
    Connection_end_allow_threads (self);
  } else {
    debugprintf ("-> Connection_putFile(%s, %d)\n", resource, fd);
    debugprintf ("cupsPutFd()\n");
    Connection_begin_allow_threads (self);
    status = cupsPutFd (self->http, resource, fd);
    Connection_end_allow_threads (self);
  }

  if (status != HTTP_OK && status != HTTP_CREATED) {
    set_http_error (status);
    debugprintf ("<- Connection_putFile() (error)\n");
    return NULL;
  }

  debugprintf ("<- Connection_putFile() = None\n");
  Py_RETURN_NONE;
}

static ipp_t *
add_modify_printer_request (const char *name)
{
  char uri[HTTP_MAX_URI];
  ipp_t *request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
  construct_uri (uri, sizeof (uri), "ipp://localhost/printers/", name);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, uri);
  return request;
}

static ipp_t *
add_modify_class_request (const char *name)
{
  char uri[HTTP_MAX_URI];
  ipp_t *request = ippNewRequest (CUPS_ADD_MODIFY_CLASS);
  construct_uri (uri, sizeof (uri), "ipp://localhost/classes/", name);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, uri);
  return request;
}

static PyObject *
Connection_addPrinter (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *nameobj = NULL;
  char *name = NULL;
  PyObject *ppdfileobj = NULL;
  char *ppdfile = NULL;
  PyObject *ppdnameobj = NULL;
  char *ppdname = NULL;
  PyObject *infoobj = NULL;
  char *info = NULL;
  PyObject *locationobj = NULL;
  char *location = NULL;
  PyObject *deviceobj = NULL;
  char *device = NULL;
  PyObject *ppd = NULL;
  ipp_t *request, *answer;
  int ppds_specified = 0;
  static char *kwlist[] = { "name", "filename", "ppdname", "info",
			    "location", "device", "ppd", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|OOOOOO", kwlist,
				    &nameobj, &ppdfileobj, &ppdnameobj,
				    &infoobj, &locationobj, &deviceobj, &ppd))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL ||
      (ppdfileobj && UTF8_from_PyObj (&ppdfile, ppdfileobj) == NULL) ||
      (ppdnameobj && UTF8_from_PyObj (&ppdname, ppdnameobj) == NULL) ||
      (infoobj && UTF8_from_PyObj (&info, infoobj) == NULL) ||
      (locationobj && UTF8_from_PyObj (&location, locationobj) == NULL) ||
      (deviceobj && UTF8_from_PyObj (&device, deviceobj) == NULL)) {
    free (name);
    free (ppdfile);
    free (ppdname);
    free (info);
    free (location);
    free (device);
    return NULL;
  }

  debugprintf ("-> Connection_addPrinter(%s,%s,%s,%s,%s,%s,%s)\n",
	       name, ppdfile ? ppdfile: "", ppdname ? ppdname: "",
	       info ? info: "", location ? location: "",
	       device ? device: "", ppd ? "(PPD object)": "");

  if (ppdfile)
    ppds_specified++;
  if (ppdname)
    ppds_specified++;
  if (ppd) {
    if (!PyObject_TypeCheck (ppd, &cups_PPDType)) {
      PyErr_SetString (PyExc_TypeError, "Expecting cups.PPD");
      debugprintf ("<- Connection_addPrinter() EXCEPTION\n");
      free (name);
      free (ppdfile);
      free (ppdname);
      free (info);
      free (location);
      free (device);
      return NULL;
    }

    ppds_specified++;
  }
  if (ppds_specified > 1) {
    PyErr_SetString (PyExc_RuntimeError,
		     "Only one PPD may be given");
    debugprintf ("<- Connection_addPrinter() EXCEPTION\n");
    free (name);
    free (ppdfile);
    free (ppdname);
    free (info);
    free (location);
    free (device);
    return NULL;
  }

  if (ppd) {
    // We've been given a cups.PPD object.  Construct a PPD file.
    char template[PATH_MAX];
    int fd;
    PyObject *args, *result;

    snprintf(template, sizeof (template), "%s/scp-ppd-XXXXXX", _PATH_TMP);
    ppdfile = strdup(template);
    fd = mkstemp (ppdfile);
    if (fd < 0) {
      debugprintf ("<- Connection_addPrinter() EXCEPTION\n");
      free (name);
      free (ppdfile);
      free (ppdname);
      free (info);
      free (location);
      free (device);
      return PyErr_SetFromErrno (PyExc_RuntimeError);
    }

    args = Py_BuildValue ("(i)", fd);
    result = PPD_writeFd ((PPD *) ppd, args);
    Py_DECREF (args);
    close (fd);

    if (result == NULL) {
      unlink (ppdfile);
      debugprintf ("<- Connection_addPrinter() EXCEPTION\n");
      free (name);
      free (ppdfile);
      free (ppdname);
      free (info);
      free (location);
      free (device);
      return NULL;
    }
  }

  request = add_modify_printer_request (name);
  free (name);
  if (ppdname) {
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		  "ppd-name", NULL, ppdname);
    free (ppdname);
  }
  if (info) {
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		  "printer-info", NULL, info);
    free (info);
  }
  if (location) {
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		  "printer-location", NULL, location);
    free (location);
  }
  if (device) {
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_URI,
		  "device-uri", NULL, device);
    free (device);
  }
  if (ppds_specified)
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
		  "printer-state-reasons", NULL, "none");
  
  Connection_begin_allow_threads (self);
  if (ppdfile) {
    answer = cupsDoFileRequest (self->http, request, "/admin/", ppdfile);
  } else
    answer = cupsDoRequest (self->http, request, "/admin/");
  Connection_end_allow_threads (self);

  if (ppd) {
    unlink (ppdfile);
    free (ppdfile);
  } else if (ppdfile)
    free (ppdfile);

  if (PyErr_Occurred ()) {
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_addPrinter() EXCEPTION\n");
    return NULL;
  }

  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);

    debugprintf ("<- Connection_addPrinter() EXCEPTION\n");
    return NULL;
  }

  ippDelete (answer);
  debugprintf ("<- Connection_addPrinter() = None\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_setPrinterDevice (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  PyObject *device_uriobj;
  char *name;
  char *device_uri;
  ipp_t *request, *answer;

  if (!PyArg_ParseTuple (args, "OO", &nameobj, &device_uriobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&device_uri, device_uriobj) == NULL) {
    free (name);
    return NULL;
  }

  request = add_modify_printer_request (name);
  free (name);
  ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_URI,
		"device-uri", NULL, device_uri);
  free (device_uri);
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/admin/");
  Connection_end_allow_threads (self);
  if (PyErr_Occurred ()) {
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_setPrinterInfo (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  PyObject *infoobj;
  char *name;
  char *info;
  ipp_t *request, *answer;
  int i;

  if (!PyArg_ParseTuple (args, "OO", &nameobj, &infoobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&info, infoobj) == NULL) {
    free (name);
    return NULL;
  }

  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		  "printer-info", NULL, info);
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      // Perhaps it's a class, not a printer.
      ippDelete (answer);
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  free (info);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_setPrinterLocation (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  PyObject *locationobj;
  char *name;
  char *location;
  ipp_t *request, *answer;
  int i;

  if (!PyArg_ParseTuple (args, "OO", &nameobj, &locationobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&location, locationobj) == NULL) {
    free (name);
    return NULL;
  }

  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		  "printer-location", NULL, location);
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      // Perhaps it's a class, not a printer.
      ippDelete (answer);
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  free (location);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_setPrinterShared (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  char *name;
  int sharing;
  ipp_t *request, *answer;
  int i;

  if (!PyArg_ParseTuple (args, "Oi", &nameobj, &sharing))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    ippAddBoolean (request, IPP_TAG_OPERATION, "printer-is-shared", sharing);
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      // Perhaps it's a class, not a printer.
      ippDelete (answer);
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_setPrinterJobSheets (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  PyObject *startobj;
  PyObject *endobj;
  char *name;
  char *start;
  char *end;
  ipp_t *request, *answer;
  ipp_attribute_t *a;
  int i;

  if (!PyArg_ParseTuple (args, "OOO", &nameobj, &startobj, &endobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&start, startobj) == NULL) {
    free (name);
    return NULL;
  }

  if (UTF8_from_PyObj (&end, endobj) == NULL) {
    free (name);
    free (start);
    return NULL;
  }

  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    a = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
		       "job-sheets-default", 2, NULL, NULL);
    ippSetString(request, &a, 0, strdup (start));
    ippSetString(request, &a, 1, strdup (end));
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      // Perhaps it's a class, not a printer.
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  free (start);
  free (end);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_setPrinterErrorPolicy (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  char *name;
  PyObject *policyobj;
  char *policy;
  ipp_t *request, *answer;
  int i;

  if (!PyArg_ParseTuple (args, "OO", &nameobj, &policyobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&policy, policyobj) == NULL) {
    free (name);
    return NULL;
  }

  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
		  "printer-error-policy", NULL, policy);
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      // Perhaps it's a class, not a printer.
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  free (policy);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_setPrinterOpPolicy (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  char *name;
  PyObject *policyobj;
  char *policy;
  ipp_t *request, *answer;
  int i;

  if (!PyArg_ParseTuple (args, "OO", &nameobj, &policyobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&policy, policyobj) == NULL) {
    free (name);
    return NULL;
  }

  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
		  "printer-op-policy", NULL, policy);
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      // Perhaps it's a class, not a printer.
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  free (policy);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
do_requesting_user_names (Connection *self, PyObject *args,
			  const char *requeststr)
{
  PyObject *nameobj;
  char *name;
  char *tmp;
  PyObject *users;
  int num_users, i, j;
  ipp_t *request, *answer;
  ipp_attribute_t *attr;

  if (!PyArg_ParseTuple (args, "OO", &nameobj, &users))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (!PyList_Check (users)) {
    PyErr_SetString (PyExc_TypeError, "List required");
    return NULL;
  }
  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    num_users = PyList_Size (users);
    if (num_users) {
      attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
			    requeststr, num_users, NULL, NULL);
      for (j = 0; j < num_users; j++) {
	PyObject *username = PyList_GetItem (users, j); // borrowed ref
	if (!PyUnicode_Check (username) && !PyBytes_Check (username)) {
	  int k;
	  PyErr_SetString (PyExc_TypeError, "String required");
	  for (k = 0; k < j; k++) {
	    free ((void *)ippGetString (attr, k, NULL));
	    ippSetString(request, &attr, k, NULL);
	  }
	  ippDelete (request);
	  return NULL;
	}
	ippSetString(request, &attr, j, UTF8_from_PyObj (&tmp, username));
	free(tmp);
      }
    } else {
      attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
			    requeststr, 1, NULL, NULL);
      if (strstr (requeststr, "denied"))
	ippSetString(request, &attr, 0, strdup ("none"));
      else
	ippSetString(request, &attr, 0, strdup ("all"));
    }
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      // Perhaps it's a class, not a printer.
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_setPrinterUsersAllowed (Connection *self, PyObject *args)
{
  return do_requesting_user_names (self, args, "requesting-user-name-allowed");
}

static PyObject *
Connection_setPrinterUsersDenied (Connection *self, PyObject *args)
{
  return do_requesting_user_names (self, args, "requesting-user-name-denied");
}

static char *
PyObject_to_string (PyObject *pyvalue)
{
  char string[BUFSIZ];
  char *value = "{unknown type}";

  if (PyUnicode_Check (pyvalue) || PyBytes_Check (pyvalue)) {
    UTF8_from_PyObj (&value, pyvalue);
  } else if (PyBool_Check (pyvalue)) {
    value = (pyvalue == Py_True) ? "true" : "false";
  } else if (PyLong_Check (pyvalue)) {
    long v = PyLong_AsLong (pyvalue);
    snprintf (string, sizeof (string), "%ld", v);
    value = string;
  } else if (PyFloat_Check (pyvalue)) {
    double v = PyFloat_AsDouble (pyvalue);
    snprintf (string, sizeof (string), "%f", v);
    value = string;
  }

  return strdup (value);
}

static PyObject *
Connection_addPrinterOptionDefault (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  char *name;
  PyObject *optionobj;
  char *option;
  PyObject *pyvalue;
  const char suffix[] = "-default";
  char *opt;
  ipp_t *request, *answer;
  int i;
  size_t optionlen;

  if (!PyArg_ParseTuple (args, "OOO", &nameobj, &optionobj, &pyvalue))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&option, optionobj) == NULL) {
    free (name);
    return NULL;
  }

  optionlen = strlen (option);
  opt = malloc (optionlen + sizeof (suffix) + 1);
  memcpy (opt, option, optionlen);
  strcpy (opt + optionlen, suffix);
  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    if (!PyUnicode_Check (pyvalue) && !PyBytes_Check (pyvalue) &&
	PySequence_Check (pyvalue)) {
      ipp_attribute_t *attr;
      int len = PySequence_Size (pyvalue);
      int j;
      attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
			    opt, len, NULL, NULL);
      for (j = 0; j < len; j++) {
	PyObject *each = PySequence_GetItem (pyvalue, j);
	ippSetString(request, &attr, j, PyObject_to_string (each));
      }
    } else
      ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
		    opt, NULL, PyObject_to_string (pyvalue));

    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      // Perhaps it's a class, not a printer.
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  free (option);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_deletePrinterOptionDefault (Connection *self, PyObject *args)
{
  PyObject *nameobj;
  char *name;
  PyObject *optionobj;
  char *option;
  const char suffix[] = "-default";
  char *opt;
  ipp_t *request, *answer;
  int i;
  size_t optionlen;

  if (!PyArg_ParseTuple (args, "OO", &nameobj, &optionobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&option, optionobj) == NULL) {
    free (name);
    return NULL;
  }

  optionlen = strlen (option);
  opt = malloc (optionlen + sizeof (suffix) + 1);
  memcpy (opt, option, optionlen);
  strcpy (opt + optionlen, suffix);
  request = add_modify_printer_request (name);
  for (i = 0; i < 2; i++) {
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_DELETEATTR,
		  opt, NULL, NULL);
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/admin/");
    Connection_end_allow_threads (self);
    if (PyErr_Occurred ()) {
      if (answer)
	ippDelete (answer);
      return NULL;
    }

    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      // Perhaps it's a class, not a printer.
      request = add_modify_class_request (name);
    } else break;
  }

  free (name);
  free (option);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_deletePrinter (Connection *self, PyObject *args, PyObject *kwds)
{
  return do_printer_request (self, args, kwds, CUPS_DELETE_PRINTER);
}

static PyObject *
Connection_getPrinterAttributes (Connection *self, PyObject *args,
				 PyObject *kwds)
{
  PyObject *ret;
  PyObject *nameobj = NULL;
  char *name;
  PyObject *uriobj = NULL;
  char *uri;
  PyObject *requested_attrs = NULL;
  char **attrs = NULL; /* initialised to calm compiler */
  size_t n_attrs = 0; /* initialised to calm compiler */
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  char consuri[HTTP_MAX_URI];
  int i;
  static char *kwlist[] = { "name", "uri", "requested_attributes", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "|OOO", kwlist,
				    &nameobj, &uriobj, &requested_attrs))
    return NULL;

  if (nameobj && uriobj) {
    PyErr_SetString (PyExc_RuntimeError,
		     "name or uri must be specified but not both");
    return NULL;
  }

  if (nameobj) {
    if (UTF8_from_PyObj (&name, nameobj) == NULL)
      return NULL;
  } else if (uriobj) {
    if (UTF8_from_PyObj (&uri, uriobj) == NULL)
      return NULL;
  } else {
    PyErr_SetString (PyExc_RuntimeError,
		     "name or uri must be specified");
    return NULL;
  }

  if (requested_attrs) {
    if (get_requested_attrs (requested_attrs, &n_attrs, &attrs) == -1) {
      if (nameobj)
	free (name);
      else if (uriobj)
	free (uri);
      return NULL;
    }
  }

  debugprintf ("-> Connection_getPrinterAttributes(%s)\n",
	       nameobj ? name : uri);

  if (nameobj) {
    construct_uri (consuri, sizeof (consuri),
		   "ipp://localhost/printers/", name);
    uri = consuri;
  }

  for (i = 0; i < 2; i++) {
    request = ippNewRequest (IPP_GET_PRINTER_ATTRIBUTES);
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		  "printer-uri", NULL, uri);
    if (requested_attrs)
      ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		     "requested-attributes", n_attrs, NULL,
		     (const char **) attrs);
    debugprintf ("trying request with uri %s\n", uri);
    Connection_begin_allow_threads (self);
    answer = cupsDoRequest (self->http, request, "/");
    Connection_end_allow_threads (self);
    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      if (uriobj)
	break;

      // Perhaps it's a class, not a printer.
      construct_uri (consuri, sizeof (consuri),
		     "ipp://localhost/classes/", name);
    } else break;
  }

  if (nameobj)
    free (name);

  if (uriobj)
    free (uri);

  if (requested_attrs)
    free_requested_attrs (n_attrs, attrs);

  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);

    debugprintf ("<- Connection_getPrinterAttributes() (error)\n");
    return NULL;
  }

  ret = PyDict_New ();
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer)) {
    while (attr && ippGetGroupTag (attr) != IPP_TAG_PRINTER)
      attr = ippNextAttribute (answer);

    if (!attr)
      break;

    for (; attr && ippGetGroupTag (attr) == IPP_TAG_PRINTER;
	 attr = ippNextAttribute (answer)) {
      size_t namelen = strlen (ippGetName (attr));
      int is_list = ippGetCount (attr) > 1;

      debugprintf ("Attribute: %s\n", ippGetName (attr));
      // job-sheets-default is special, since it is always two values.
      // Make it a tuple.
      if (!strcmp (ippGetName (attr), "job-sheets-default") &&
	  ippGetValueTag (attr) == IPP_TAG_NAME) {
	PyObject *startobj, *endobj, *tuple;
	const char *start, *end;
	start = ippGetString (attr, 0, NULL);
	if (ippGetCount (attr) >= 2)
	  end = ippGetString (attr, 1, NULL);
	else
	  end = "";

	startobj = PyObj_from_UTF8 (start);
	endobj = PyObj_from_UTF8 (end);
	tuple = Py_BuildValue ("(OO)", startobj, endobj);
	Py_DECREF (startobj);
	Py_DECREF (endobj);
	PyDict_SetItemString (ret, "job-sheets-default", tuple);
	Py_DECREF (tuple);
	continue;
      }

      // Check for '-supported' suffix.  Any xxx-supported attribute
      // that is a text type must be a list.
      //
      // Also check for attributes that are known to allow multiple
      // string values, and make them lists.
      if (!is_list && namelen > 10) {
	const char *multivalue_options[] =
	  {
	    "notify-events-default",
	    "requesting-user-name-allowed",
	    "requesting-user-name-denied",
	    "printer-state-reasons",
	    "marker-colors",
	    "marker-names",
	    "marker-types",
	    "marker-levels",
	    "member-names",
	    NULL
	  };

	switch (ippGetValueTag (attr)) {
	case IPP_TAG_NAME:
	case IPP_TAG_TEXT:
	case IPP_TAG_KEYWORD:
	case IPP_TAG_URI:
	case IPP_TAG_CHARSET:
	case IPP_TAG_MIMETYPE:
	case IPP_TAG_LANGUAGE:
	case IPP_TAG_ENUM:
	case IPP_TAG_INTEGER:
	case IPP_TAG_RESOLUTION:
	  is_list = !strcmp (ippGetName (attr) + namelen - 10, "-supported");

	  if (!is_list) {
	    const char **opt;
	    for (opt = multivalue_options; !is_list && *opt; opt++)
	      is_list = !strcmp (ippGetName (attr), *opt);
	  }

	default:
	  break;
	}
      }

      if (is_list) {
	PyObject *list = PyList_from_attr_values (attr);
	PyDict_SetItemString (ret, ippGetName (attr), list);
	Py_DECREF (list);
      } else {
	PyObject *val = PyObject_from_attr_value (attr, i);
	PyDict_SetItemString (ret, ippGetName (attr), val);
      }
    }

    if (!attr)
      break;
  }

  debugprintf ("<- Connection_getPrinterAttributes() = dict\n");
  return ret;
}

static PyObject *
Connection_addPrinterToClass (Connection *self, PyObject *args)
{
  PyObject *printernameobj;
  char *printername;
  PyObject *classnameobj;
  char *classname;
  char classuri[HTTP_MAX_URI];
  char printeruri[HTTP_MAX_URI];
  ipp_t *request, *answer;

  if (!PyArg_ParseTuple (args, "OO", &printernameobj, &classnameobj))
    return NULL;

  if (UTF8_from_PyObj (&printername, printernameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&classname, classnameobj) == NULL) {
    free (printername);
    return NULL;
  }

  // Does the class exist, and is the printer already in it?
  request = ippNewRequest (IPP_GET_PRINTER_ATTRIBUTES);
  construct_uri (classuri, sizeof (classuri),
		 "ipp://localhost/classes/", classname);
	    
  free (classname);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, classuri);
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (answer) {
    ipp_attribute_t *printers;
    printers = ippFindAttribute (answer, "member-names", IPP_TAG_NAME);
    if (printers) {
      int i;
      for (i = 0; i < ippGetCount (printers); i++) {
	if (!strcasecmp (ippGetString (printers, i, NULL), printername)) {
	  ippDelete (answer);
	  PyErr_SetString (PyExc_RuntimeError, "Printer already in class");
	  free (printername);
	  return NULL;
	}
      }
    }
  }

  request = ippNewRequest (CUPS_ADD_CLASS);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, classuri);
  construct_uri (printeruri, sizeof (printeruri),
		 "ipp://localhost/printers/", printername);
  free (printername);
  if (answer) {
    ipp_attribute_t *printers;
    printers = ippFindAttribute (answer, "member-uris", IPP_TAG_URI);
    if (printers) {
      ipp_attribute_t *attr;
      int i;
      attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_URI,
			    "member-uris", ippGetCount (printers) + 1,
			    NULL, NULL);
      for (i = 0; i < ippGetCount (printers); i++)
        ippSetString(request, &attr, i,
                     strdup (ippGetString (printers, i, NULL)));
      ippSetString(request, &attr, ippGetCount (printers), strdup (printeruri));

    }

    ippDelete (answer);
  }

  // If the class didn't exist, create a new one.
  if (!ippFindAttribute (request, "member-uris", IPP_TAG_URI))
    ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_URI,
		  "member-uris", NULL, printeruri);

  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/admin/");
  Connection_end_allow_threads (self);
  if (PyErr_Occurred ()) {
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_deletePrinterFromClass (Connection *self, PyObject *args)
{
  const char *requested_attrs[] = {
    "member-names",
    "member-uris"
  };
  PyObject *printernameobj;
  char *printername;
  PyObject *classnameobj;
  char *classname;
  char classuri[HTTP_MAX_URI];
  ipp_t *request, *answer;
  ipp_attribute_t *printers;
  int i;

  if (!PyArg_ParseTuple (args, "OO", &printernameobj, &classnameobj))
    return NULL;

  if (UTF8_from_PyObj (&printername, printernameobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&classname, classnameobj) == NULL) {
    free (printername);
    return NULL;
  }

  // Does the class exist, and is the printer in it?
  request = ippNewRequest (IPP_GET_PRINTER_ATTRIBUTES);
  construct_uri (classuri, sizeof (classuri),
		 "ipp://localhost/classes/", classname);
  free (classname);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, classuri);
  ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		 "requested-attributes",
		 sizeof(requested_attrs) / sizeof(requested_attrs[0]),
		 NULL, requested_attrs);
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer) {
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    free (printername);
    return NULL;
  }

  printers = ippFindAttribute (answer, "member-names", IPP_TAG_NAME);
  for (i = 0; printers && i < ippGetCount (printers); i++)
    if (!strcasecmp (ippGetString (printers, i, NULL), printername))
      break;

  free (printername);
  if (!printers || i == ippGetCount (printers)) {
    ippDelete (answer);
    PyErr_SetString (PyExc_RuntimeError, "Printer not in class");
    return NULL;
  }

  printers = ippFindAttribute (answer, "member-uris", IPP_TAG_URI);
  if (!printers || i >= ippGetCount (printers)) {
    ippDelete (answer);
    PyErr_SetString (PyExc_RuntimeError, "No member URIs returned");
    return NULL;
  }

  request = ippNewRequest (CUPS_ADD_CLASS);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, classuri);

  // Only printer in class?  Delete the class.
  if (ippGetCount (printers) == 1)
    ippSetOperation (request, CUPS_DELETE_CLASS);
  else {
    // Trim the printer from the list.
    ipp_attribute_t *newlist;
    int j;
    newlist = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_URI,
			     "member-uris", ippGetCount (printers) - 1,
			     NULL, NULL);
    for (j = 0; j < i; j++)
      ippSetString(request, &newlist, j,
                   strdup (ippGetString (printers, j, NULL)));
    for (j = i; j < ippGetCount(newlist); j++)
      ippSetString(request, &newlist, j,
                   strdup (ippGetString (printers, j+1, NULL)));
  }

  ippDelete (answer);
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/admin/");
  Connection_end_allow_threads (self);
  if (PyErr_Occurred ()) {
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_deleteClass (Connection *self, PyObject *args)
{
  PyObject *classnameobj;
  char *classname;
  char classuri[HTTP_MAX_URI];
  ipp_t *request, *answer;

  if (!PyArg_ParseTuple (args, "O", &classnameobj))
    return NULL;

  if (UTF8_from_PyObj (&classname, classnameobj) == NULL)
    return NULL;

  request = ippNewRequest (CUPS_DELETE_CLASS);
  construct_uri (classuri, sizeof (classuri),
		 "ipp://localhost/classes/", classname);
  free (classname);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, classuri);
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/admin/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  ippDelete (answer);
  Py_RETURN_NONE;
}

static PyObject *
Connection_enablePrinter (Connection *self, PyObject *args, PyObject *kwds)
{
  return do_printer_request (self, args, kwds, IPP_RESUME_PRINTER);
}

static PyObject *
Connection_disablePrinter (Connection *self, PyObject *args, PyObject *kwds)
{
  return do_printer_request (self, args, kwds, IPP_PAUSE_PRINTER);
}

static PyObject *
Connection_acceptJobs (Connection *self, PyObject *args, PyObject *kwds)
{
  return do_printer_request (self, args, kwds, CUPS_ACCEPT_JOBS);
}

static PyObject *
Connection_rejectJobs (Connection *self, PyObject *args, PyObject *kwds)
{
  return do_printer_request (self, args, kwds, CUPS_REJECT_JOBS);
}

static PyObject *
Connection_getDefault (Connection *self, PyObject *args)
{
  const char *def;
  PyObject *ret;
  debugprintf ("-> Connection_getDefault()\n");
  Connection_begin_allow_threads (self);
  def = cupsGetDefault2 (self->http);
  Connection_end_allow_threads (self);
  if (def == NULL) {
    debugprintf ("<- Connection_getDefault() = None\n");
    Py_RETURN_NONE;
  } else {
    debugprintf ("<- Connection_getDefault() = \"%s\"\n", def);
    ret = PyUnicode_FromString (def);
  }

  return ret;
}

static PyObject *
Connection_setDefault (Connection *self, PyObject *args, PyObject *kwds)
{
  return do_printer_request (self, args, kwds, CUPS_SET_DEFAULT);
}

static PyObject *
Connection_getPPD (Connection *self, PyObject *args)
{
  PyObject *ret;
  PyObject *printerobj;
  char *printer;
  const char *ppdfile;

  if (!PyArg_ParseTuple (args, "O", &printerobj))
    return NULL;

  if (UTF8_from_PyObj (&printer, printerobj) == NULL)
    return NULL;

  debugprintf ("-> Connection_getPPD()\n");
  Connection_begin_allow_threads (self);
  ppdfile = cupsGetPPD2 (self->http, printer);
  Connection_end_allow_threads (self);
  free (printer);
  if (!ppdfile) {
    ipp_status_t err = cupsLastError ();
    if (err)
      set_ipp_error (err, cupsLastErrorString ());
    else
      PyErr_SetString (PyExc_RuntimeError, "cupsGetPPD2 failed");

    debugprintf ("<- Connection_getPPD() (error)\n");
    return NULL;
  }

  ret = PyUnicode_FromString (ppdfile);
  debugprintf ("<- Connection_getPPD() = %s\n", ppdfile);
  return ret;
}

static PyObject *
Connection_getPPD3 (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *ret, *obj;
  PyObject *printerobj;
  PyObject *fmodtime = NULL;
  char *printer;
  time_t modtime;
  PyObject *filenameobj = NULL;
  char *filename = NULL;
  char fname[PATH_MAX];
  http_status_t status;
  static char *kwlist[] = { "name", "modtime", "filename", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|OO", kwlist,
				    &printerobj, &fmodtime, &filenameobj))
    return NULL;

  if (UTF8_from_PyObj (&printer, printerobj) == NULL)
    return NULL;

  if (fmodtime) {
    double d = PyFloat_AsDouble (fmodtime);
    if (PyErr_Occurred()) {
      free (printer);
      return NULL;
    }

    modtime = (time_t) d;
  } else
    modtime = 0;

  if (filenameobj)
    if (UTF8_from_PyObj (&filename, filenameobj) == NULL) {
      free (printer);
      return NULL;
    }
  if (filename) {
    if (strlen (filename) > sizeof (fname)) {
      PyErr_SetString (PyExc_TypeError, "overlength filename");
      free (printer);
      free (filename);
      return NULL;
    }
    strcpy (fname, filename);
  } else
    fname[0] = '\0';

  debugprintf ("-> Connection_getPPD3()\n");
  Connection_begin_allow_threads (self);
  status = cupsGetPPD3 (self->http, printer, &modtime,
			fname, sizeof (fname));
  Connection_end_allow_threads (self);

  free (printer);
  free (filename);

  ret = PyTuple_New (3);
  if (!ret)
    return NULL;

  obj = PyLong_FromLong ((long) status);

  if (!obj) {
    Py_DECREF (ret);
    return NULL;
  }

  PyTuple_SetItem (ret, 0, obj);

  obj = PyFloat_FromDouble ((double) modtime);
  if (!obj) {
    Py_DECREF (ret);
    return NULL;
  }

  PyTuple_SetItem (ret, 1, obj);

  obj = PyUnicode_FromString (fname);
  if (!obj) {
    Py_DECREF (ret);
    return NULL;
  }

  PyTuple_SetItem (ret, 2, obj);

  debugprintf ("<- Connection_getPPD3() = (%d,%ld,%s)\n",
	       status, modtime, fname);
  return ret;
}

static PyObject *
Connection_printTestPage (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *printerobj;
  char *printer;
  PyObject *fileobj = NULL;
  char *file = NULL;
  PyObject *titleobj = NULL;
  char *title = NULL;
  PyObject *formatobj = NULL;
  char *format = NULL;
  PyObject *userobj = NULL;
  char *user = NULL;
  const char *datadir;
  char filename[PATH_MAX];
  char uri[HTTP_MAX_URI];
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  char *resource;
  int jobid = 0;
  int i;
  static char *kwlist[] = { "name", "file", "title", "format", "user", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|OOOO", kwlist,
				    &printerobj, &fileobj, &titleobj,
				    &formatobj, &userobj))
    return NULL;

  if (UTF8_from_PyObj (&printer, printerobj) == NULL)
    return NULL;

  if ((fileobj && UTF8_from_PyObj (&file, fileobj) == NULL) ||
      (titleobj && UTF8_from_PyObj (&title, titleobj) == NULL) ||
      (formatobj && UTF8_from_PyObj (&format, formatobj) == NULL) ||
      (userobj && UTF8_from_PyObj (&user, userobj) == NULL)) {
    free (printer);
    free (file);
    free (title);
    free (format);
    free (user);
    return NULL;
  }
    
  if (!fileobj) {
    const char *testprint[] = { "%s/data/testprint",
				"%s/data/testprint.ps",
				NULL };
    if ((datadir = getenv ("CUPS_DATADIR")) != NULL) {
      const char **pattern;
      for (pattern = testprint; *pattern != NULL; pattern++) {
	snprintf (filename, sizeof (filename), *pattern, datadir);
	if (access (filename, R_OK) == 0)
	  break;
      }
    } else {
      const char *const dirs[] = { "/usr/share/cups",
				   "/usr/local/share/cups",
				   NULL };
      int found = 0;
      int i;
      for (i = 0; (datadir = dirs[i]) != NULL; i++) {
	const char **pattern;
	for (pattern = testprint; *pattern != NULL; pattern++) {
	  snprintf (filename, sizeof (filename), *pattern, datadir);
	  if (access (filename, R_OK) == 0) {
	    found = 1;
	    break;
	  }
	}

	if (found)
	  break;
      }

      if (datadir == NULL)
	/* We haven't found the testprint.ps file, so just pick a path
	 * and try it.  This will certainly fail with
	 * client-error-not-found, but we'll let that happen rather
	 * than raising an exception so as to be consistent with the
	 * case where CUPS_DATADIR is set and we trust it. */
	snprintf (filename, sizeof (filename), testprint[0], dirs[0]);
    }

    file = filename;
  }

  if (!titleobj)
    title = "Test Page";

  if (!userobj)
	  user = (char *) cupsUser();

  construct_uri (uri, sizeof (uri),
		 "ipp://localhost/printers/", printer);
  resource = uri + strlen ("ipp://localhost");
  for (i = 0; i < 2; i++) {
    request = ippNewRequest (IPP_PRINT_JOB);
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri",
		  NULL, uri);
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		  "requesting-user-name", NULL, user);
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name",
		  NULL, title);
    if (format)
      ippAddString (request, IPP_TAG_JOB, IPP_TAG_MIMETYPE, "document-format",
		    NULL, format);

    Connection_begin_allow_threads (self);
    answer = cupsDoFileRequest (self->http, request, resource, file);
    Connection_end_allow_threads (self);
    if (answer && ippGetStatusCode (answer) == IPP_NOT_POSSIBLE) {
      ippDelete (answer);
      // Perhaps it's a class, not a printer.
      construct_uri (uri, sizeof (uri),
		     "ipp://localhost/classes/", printer);
    } else break;
  }

  free (printer);
  if (fileobj)
    free (file);
  if (titleobj)
    free (title);
  if (formatobj)
    free (format);
  if (userobj)
    free (user);

  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    return NULL;
  }

  attr = ippFindAttribute (answer, "job-id", IPP_TAG_INTEGER);
  if (attr)
    jobid = ippGetInteger (attr, 0);

  ippDelete (answer);
  return Py_BuildValue ("i", jobid);
}

static PyObject *
Connection_adminExportSamba (Connection *self, PyObject *args)
{
  int ret;
  PyObject *nameobj;
  char *name;
  PyObject *serverobj;
  char *server;
  PyObject *userobj;
  char  *user;
  PyObject *passwordobj;
  char *password;
  char ppdfile[1024];
  FILE *tmpfile(void);
  FILE *tf;
  char str[80];

  if (!PyArg_ParseTuple (args, "OOOO", &nameobj, &serverobj, &userobj,
                         &passwordobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL ||
      UTF8_from_PyObj (&server, serverobj) == NULL ||
      UTF8_from_PyObj (&user, userobj) == NULL ||
      UTF8_from_PyObj (&password, passwordobj) == NULL) {
    free (name);
    free (server);
    free (user);
    free (password);
    PyErr_SetString(PyExc_RuntimeError,
                    "name, samba_server, samba_username, samba_password "
                    "must be specified");
    return NULL;
  }

  if (!cupsAdminCreateWindowsPPD(self->http, name, ppdfile, sizeof(ppdfile))) {
    PyErr_SetString(PyExc_RuntimeError,
                    "No PPD file found for the printer");
    return NULL;
  }

  debugprintf ("-> Connection_adminExportSamba()\n");
  tf = tmpfile();
  Connection_begin_allow_threads (self);
  ret = cupsAdminExportSamba(name, ppdfile, server, user, password, tf);
  Connection_end_allow_threads (self);

  free (name);
  free (server);
  free (user);
  free (password);
  unlink (ppdfile);

  if (!ret) {
    rewind(tf);
    // Read logfile line by line to get Exit status message on the last line.
    while (fgets (str, sizeof(str), tf) != NULL) { }
    fclose (tf);
    if (str[strlen(str) -1] == '\n') {
      str[strlen(str) -1] = '\0';
    }
    PyErr_SetString (PyExc_RuntimeError, str);
    debugprintf ("<- Connection_adminExportSamba() EXCEPTION\n");
    return NULL;
  }
  fclose (tf);
  debugprintf ("<- Connection_adminExportSamba()\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_adminGetServerSettings (Connection *self)
{
  PyObject *ret = PyDict_New ();
  int num_settings, i;
  cups_option_t *settings;
  debugprintf ("-> Connection_adminGetServerSettings()\n");
  Connection_begin_allow_threads (self);
  cupsAdminGetServerSettings (self->http, &num_settings, &settings);
  Connection_end_allow_threads (self);
  for (i = 0; i < num_settings; i++) {
    PyObject *string = PyUnicode_FromString (settings[i].value);
    PyDict_SetItemString (ret, settings[i].name, string);
    Py_DECREF (string);
  }

  cupsFreeOptions (num_settings, settings);
  debugprintf ("<- Connection_adminGetServerSettings()\n");
  return ret;
}

static PyObject *
Connection_adminSetServerSettings (Connection *self, PyObject *args)
{
  PyObject *dict, *key, *val;
  int ret;
  int num_settings = 0;
  DICT_POS_TYPE pos = 0;
  cups_option_t *settings = NULL;
  if (!PyArg_ParseTuple (args, "O", &dict))
    return NULL;
  if (!PyDict_Check (dict)) {
    PyErr_SetString (PyExc_TypeError, "Expecting dict");
    return NULL;
  }

  debugprintf ("-> Connection_adminSetServerSettings()\n");
  while (PyDict_Next (dict, &pos, &key, &val)) {
    char *name, *value;
    if ((!PyUnicode_Check (key) && !PyBytes_Check (key)) ||
        (!PyUnicode_Check (val) && !PyBytes_Check (val))) {
      cupsFreeOptions (num_settings, settings);
      PyErr_SetString (PyExc_TypeError, "Keys and values must be strings");
      debugprintf ("<- Connection_adminSetServerSettings() EXCEPTION\n");
      return NULL;
    }

    UTF8_from_PyObj (&name, key);
    UTF8_from_PyObj (&value, val);
    debugprintf ("%s: %s\n", name, value);
    num_settings = cupsAddOption (name,
				  value,
				  num_settings,
				  &settings);
    free (name);
    free (value);
  }

  debugprintf ("num_settings=%d, settings=%p\n", num_settings, settings);
  Connection_begin_allow_threads (self);
  ret = cupsAdminSetServerSettings (self->http, num_settings, settings);
  Connection_end_allow_threads (self);
  if (!ret) {
    cupsFreeOptions (num_settings, settings);
    PyErr_SetString (PyExc_RuntimeError, "Failed to set settings");
    debugprintf ("<- Connection_adminSetServerSettings() EXCEPTION\n");
    return NULL;
  }

  cupsFreeOptions (num_settings, settings);
  debugprintf ("<- Connection_adminSetServerSettings()\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_getSubscriptions (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *uriobj;
  char *uri;
  PyObject *my_subscriptions = Py_False;
  int job_id = -1;
  ipp_t *request, *answer;
  ipp_attribute_t *attr;
  PyObject *result, *subscription;
  static char *kwlist[] = { "uri", "my_subscriptions", "job_id", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|Oi", kwlist,
				    &uriobj, &my_subscriptions, &job_id))
    return NULL;

  if (UTF8_from_PyObj (&uri, uriobj) == NULL)
    return NULL;

  if (my_subscriptions && !PyBool_Check (my_subscriptions)) {
    PyErr_SetString (PyExc_TypeError, "my_subscriptions must be a bool");
    return NULL;
  }

  debugprintf ("-> Connection_getSubscriptions()\n");
  request = ippNewRequest (IPP_GET_SUBSCRIPTIONS);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, uri);
  free (uri);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());

  if (my_subscriptions == Py_True)
    ippAddBoolean (request, IPP_TAG_OPERATION, "my-subscriptions", 1);

  if (job_id != -1)
    ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		   "job-id", job_id);

  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getSubscriptions() EXCEPTION\n");
    return NULL;
  }

  result = PyList_New (0);
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer))
    if (ippGetGroupTag (attr) == IPP_TAG_SUBSCRIPTION)
      break;

  subscription = NULL;
  for (; attr; attr = ippNextAttribute (answer)) {
    PyObject *obj;
    if (ippGetGroupTag (attr) == IPP_TAG_ZERO) {
      // End of subscription.
      if (subscription) {
	PyList_Append (result, subscription);
	Py_DECREF (subscription);
      }

      subscription = NULL;
      continue;
    }

    if (ippGetCount (attr) > 1 || !strcmp (ippGetName (attr), "notify-events"))
      obj = PyList_from_attr_values (attr);
    else
      obj = PyObject_from_attr_value (attr, 0);

    if (!obj)
      // Can't represent this.
      continue;

    if (!subscription)
      subscription = PyDict_New ();

    PyDict_SetItemString (subscription, ippGetName (attr), obj);
    Py_DECREF (obj);
  }

  if (subscription) {
    PyList_Append (result, subscription);
    Py_DECREF (subscription);
  }

  ippDelete (answer);
  debugprintf ("<- Connection_getSubscriptions()\n");
  return result;
}

static PyObject *
Connection_createSubscription (Connection *self, PyObject *args,
			       PyObject *kwds)
{
  PyObject *resource_uriobj;
  char *resource_uri;
  PyObject *events = NULL;
  int job_id = -1, lease_duration = -1, time_interval = -1;
  PyObject *recipient_uriobj = NULL, *user_dataobj = NULL;
  char *recipient_uri = NULL, *user_data = NULL;
  ipp_t *request, *answer;
  int i, n = 0;
  char *tmp;
  ipp_attribute_t *attr;
  static char *kwlist[] = { "uri", "events", "job_id", "recipient_uri",
			    "lease_duration", "time_interval", "user_data",
			    NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|OiOiiO", kwlist,
				    &resource_uriobj, &events, &job_id,
				    &recipient_uriobj, &lease_duration,
				    &time_interval, &user_dataobj))
    return NULL;

  if (UTF8_from_PyObj (&resource_uri, resource_uriobj) == NULL)
    return NULL;

  if (recipient_uriobj &&
      UTF8_from_PyObj (&recipient_uri, recipient_uriobj) == NULL) {
    free (resource_uri);
    return NULL;
  }

  if (user_dataobj && UTF8_from_PyObj (&user_data, user_dataobj) == NULL) {
    free (resource_uri);
    if (recipient_uriobj)
      free (recipient_uri);
    return NULL;
  }

  if (events) {
    if (!PyList_Check (events)) {
      PyErr_SetString (PyExc_TypeError, "events must be a list");
      return NULL;
    }

    n = PyList_Size (events);
    for (i = 0; i < n; i++) {
      PyObject *event = PyList_GetItem (events, i);
      if (!PyUnicode_Check (event) && !PyBytes_Check (event)) {
	PyErr_SetString (PyExc_TypeError, "events must be a list of strings");
	return NULL;
      }
    }
  }

  debugprintf ("-> Connection_createSubscription(%s)\n", resource_uri);
  request = ippNewRequest (IPP_CREATE_PRINTER_SUBSCRIPTION);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, resource_uri);
  ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD,
		"notify-pull-method", NULL, "ippget");
  ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_CHARSET,
		"notify-charset", NULL, "utf-8");
  ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());

  if (recipient_uriobj) {
    ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_URI,
		  "notify-recipient-uri", NULL, recipient_uri);
    free (recipient_uri);
  }

  if (user_dataobj) {
    ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_STRING,
		  "notify-user-data", NULL, user_data);
    free (user_data);
  }

  if (events) {
    attr = ippAddStrings (request, IPP_TAG_SUBSCRIPTION,
			  IPP_TAG_KEYWORD, "notify-events",
			  n, NULL, NULL);
    for (i = 0; i < n; i++) {
      PyObject *event = PyList_GetItem (events, i);
      ippSetString(request, &attr, i, UTF8_from_PyObj (&tmp, event));
      free(tmp);
    }
  }

  if (lease_duration != -1)
    ippAddInteger (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER,
		   "notify-lease-duration", lease_duration);

  if (time_interval != -1)
    ippAddInteger (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER,
		   "notify-time-interval", time_interval);

  if (job_id != -1)
    ippAddInteger (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER,
		   "notify-job-id", job_id);

  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_createSubscription() EXCEPTION\n");
    return NULL;
  }

  i = -1;
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer)) {
    if (ippGetGroupTag (attr) == IPP_TAG_SUBSCRIPTION) {
      if (ippGetValueTag (attr) == IPP_TAG_INTEGER &&
	  !strcmp (ippGetName (attr), "notify-subscription-id"))
	i = ippGetInteger (attr, 0);
      else if (ippGetValueTag (attr) == IPP_TAG_ENUM &&
	       !strcmp (ippGetName (attr), "notify-status-code"))
	debugprintf ("notify-status-code = %d\n", ippGetInteger (attr, 0));
    }
  }

  ippDelete (answer);
  debugprintf ("<- Connection_createSubscription() = %d\n", i);
  return PyLong_FromLong (i);
}

static PyObject *
Connection_getNotifications (Connection *self, PyObject *args, PyObject *kwds)
{
  PyObject *subscription_ids, *sequence_numbers = NULL;
  ipp_t *request, *answer;
  int i, num_ids, num_seqs = 0;
  ipp_attribute_t *attr;
  PyObject *result, *events, *event;
  static char *kwlist[] = { "subscription_ids", "sequence_numbers", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|O", kwlist,
				    &subscription_ids, &sequence_numbers))
    return NULL;

  if (!PyList_Check (subscription_ids)) {
    PyErr_SetString (PyExc_TypeError, "subscriptions_ids must be a list");
    return NULL;
  }

  num_ids = PyList_Size (subscription_ids);
  for (i = 0; i < num_ids; i++) {
    PyObject *id = PyList_GetItem (subscription_ids, i);
    if (!PyLong_Check (id)) {
      PyErr_SetString (PyExc_TypeError, "subscription_ids must be a list "
		       "of integers");
      return NULL;
    }
  }

  if (sequence_numbers) {
    if (!PyList_Check (sequence_numbers)) {
      PyErr_SetString (PyExc_TypeError, "sequence_numbers must be a list");
      return NULL;
    }

    num_seqs = PyList_Size (sequence_numbers);
    for (i = 0; i < num_seqs; i++) {
      PyObject *id = PyList_GetItem (sequence_numbers, i);
      if (!PyLong_Check (id)) {
	PyErr_SetString (PyExc_TypeError, "sequence_numbers must be a list "
			 "of integers");
	return NULL;
      }
    }
  }

  debugprintf ("-> Connection_getNotifications()\n");
  request = ippNewRequest (IPP_GET_NOTIFICATIONS);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, "/");
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());

  attr = ippAddIntegers (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
			 "notify-subscription-ids", num_ids, NULL);
  for (i = 0; i < num_ids; i++) {
    PyObject *id = PyList_GetItem (subscription_ids, i);
    ippSetInteger (request, &attr, i, PyLong_AsLong (id));
  }

  if (sequence_numbers) {
    attr = ippAddIntegers (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
			   "notify-sequence-numbers", num_seqs, NULL);
    for (i = 0; i < num_seqs; i++) {
      PyObject *num = PyList_GetItem (sequence_numbers, i);
      ippSetInteger (request, &attr, i, PyLong_AsLong (num));
    }
  }
  
  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_getNotifications() EXCEPTION\n");
    return NULL;
  }

  result = PyDict_New ();

  // Result-wide attributes.
  attr = ippFindAttribute (answer, "notify-get-interval", IPP_TAG_INTEGER);
  if (attr) {
    PyObject *val = PyLong_FromLong (ippGetInteger (attr, 0));
    PyDict_SetItemString (result, ippGetName (attr), val);
    Py_DECREF (val);
  }

  attr = ippFindAttribute (answer, "printer-up-time", IPP_TAG_INTEGER);
  if (attr) {
    PyObject *val = PyLong_FromLong (ippGetInteger (attr, 0));
    PyDict_SetItemString (result, ippGetName (attr), val);
    Py_DECREF (val);
  }

  events = PyList_New (0);
  for (attr = ippFirstAttribute (answer); attr; attr = ippNextAttribute (answer))
    if (ippGetGroupTag (attr) == IPP_TAG_EVENT_NOTIFICATION)
      break;

  event = NULL;
  for (; attr; attr = ippNextAttribute (answer)) {
    PyObject *obj;
    if (ippGetGroupTag (attr) == IPP_TAG_ZERO) {
      // End of event notification.
      if (event) {
	PyList_Append (events, event);
	Py_DECREF (event);
      }

      event = NULL;
      continue;
    }

    if (ippGetCount (attr) > 1 ||
	!strcmp (ippGetName (attr), "notify-events") ||
	!strcmp (ippGetName (attr), "printer-state-reasons") ||
	!strcmp (ippGetName (attr), "job-printer-state-reasons"))
      obj = PyList_from_attr_values (attr);
    else
      obj = PyObject_from_attr_value (attr, 0);

    if (!obj)
      // Can't represent this.
      continue;

    if (!event)
      event = PyDict_New ();

    PyDict_SetItemString (event, ippGetName (attr), obj);
    Py_DECREF (obj);
  }

  if (event) {
    PyList_Append (events, event);
    Py_DECREF (event);
  }

  ippDelete (answer);
  PyDict_SetItemString (result, "events", events);
  Py_DECREF (events);
  debugprintf ("<- Connection_getNotifications()\n");
  return result;
}

static PyObject *
Connection_renewSubscription (Connection *self, PyObject *args, PyObject *kwds)
{
  int id;
  int lease_duration = -1;
  ipp_t *request, *answer;
  static char *kwlist[] = { "id", "lease_duration", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "i|i", kwlist,
				    &id, &lease_duration))
    return NULL;

  debugprintf ("-> Connection_renewSubscription()\n");
  request = ippNewRequest (IPP_RENEW_SUBSCRIPTION);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, "/");
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		 "notify-subscription-id", id);

  if (lease_duration != -1)
    ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		   "notify-lease-duration", lease_duration);

  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_renewSubscription() EXCEPTION\n");
    return NULL;
  }

  ippDelete (answer);
  debugprintf ("<- Connection_renewSubscription()\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_cancelSubscription (Connection *self, PyObject *args)
{
  int id;
  ipp_t *request, *answer;

  if (!PyArg_ParseTuple (args, "i", &id))
    return NULL;

  debugprintf ("-> Connection_cancelSubscription()\n");
  request = ippNewRequest (IPP_CANCEL_SUBSCRIPTION);
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		"printer-uri", NULL, "/");
  ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
		"requesting-user-name", NULL, cupsUser ());
  ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
		"notify-subscription-id", id);

  Connection_begin_allow_threads (self);
  answer = cupsDoRequest (self->http, request, "/");
  Connection_end_allow_threads (self);
  if (!answer || ippGetStatusCode (answer) > IPP_OK_CONFLICT) {
    set_ipp_error (answer ? ippGetStatusCode (answer) : cupsLastError (),
		   answer ? NULL : cupsLastErrorString ());
    if (answer)
      ippDelete (answer);
    debugprintf ("<- Connection_cancelSubscription() EXCEPTION\n");
    return NULL;
  }

  ippDelete (answer);
  debugprintf ("<- Connection_cancelSubscription()\n");
  Py_RETURN_NONE;
}

static PyObject *
Connection_printFile (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "printer", "filename", "title", "options", NULL };
  PyObject *printer_obj;
  char *printer;
  PyObject *filename_obj;
  char *filename;
  PyObject *title_obj;
  char *title;
  PyObject *options_obj, *key, *val;
  int num_settings = 0;
  DICT_POS_TYPE pos = 0;
  cups_option_t *settings = NULL;
  int jobid;

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "OOOO", kwlist,
				    &printer_obj, &filename_obj, &title_obj,
				    &options_obj))
    return NULL;

  if (UTF8_from_PyObj (&printer, printer_obj) == NULL)
    return NULL;
  if (UTF8_from_PyObj (&filename, filename_obj) == NULL) {
    free (printer);
    return NULL;
  }
  if (UTF8_from_PyObj (&title, title_obj) == NULL) {
    free (filename);
    free (printer);
    return NULL;
  }

  if (!PyDict_Check (options_obj)) {
    free (title);
    free (filename);
    free (printer);
    PyErr_SetString (PyExc_TypeError, "options must be a dict");
    return NULL;
  }
  while (PyDict_Next (options_obj, &pos, &key, &val)) {
    char *name, *value;
    if ((!PyUnicode_Check (key) && !PyBytes_Check (key)) ||
        (!PyUnicode_Check (val) && !PyBytes_Check (val))) {
      cupsFreeOptions (num_settings, settings);
      free (title);
      free (filename);
      free (printer);
      PyErr_SetString (PyExc_TypeError, "Keys and values must be strings");
      return NULL;
    }

    num_settings = cupsAddOption (UTF8_from_PyObj (&name, key),
				  UTF8_from_PyObj (&value, val),
				  num_settings,
				  &settings);
    free (name);
    free (value);
  }

  Connection_begin_allow_threads (self);
  jobid = cupsPrintFile2 (self->http, printer, filename, title, num_settings,
                          settings);
  Connection_end_allow_threads (self);

  if (jobid == 0) {
    cupsFreeOptions (num_settings, settings);
    free (title);
    free (filename);
    free (printer);
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    return NULL;
  }

  cupsFreeOptions (num_settings, settings);
  free (title);
  free (filename);
  free (printer);
  return PyLong_FromLong (jobid);
}

static void
free_string_list (int num_string, char **strings)
{
  int i;
  for (i = 0; i < num_string; ++i) {
    free (strings[i]);
  }
  free (strings);
}

static PyObject *
Connection_printFiles (Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = { "printer", "filenames", "title", "options", NULL };
  PyObject *printer_obj;
  char *printer;
  PyObject *filenames_obj;
  int num_filenames;
  char **filenames;
  PyObject *title_obj;
  char *title;
  PyObject *options_obj, *key, *val;
  int num_settings = 0;
  DICT_POS_TYPE pos = 0;
  cups_option_t *settings = NULL;
  int jobid;

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "OOOO", kwlist,
				    &printer_obj, &filenames_obj, &title_obj,
				    &options_obj))
    return NULL;

  if (UTF8_from_PyObj (&printer, printer_obj) == NULL)
    return NULL;

  if (!PyList_Check (filenames_obj)) {
    free (printer);
    PyErr_SetString (PyExc_TypeError, "filenames must be a list");
    return NULL;
  }
  num_filenames = PyList_Size (filenames_obj);
  if (num_filenames == 0) {
    free (printer);
    PyErr_SetString (PyExc_RuntimeError, "filenames list is empty");
    return NULL;
  }
  filenames = malloc (num_filenames * sizeof(char*));
  for (pos = 0; pos < num_filenames; ++pos) {
    PyObject *filename_obj = PyList_GetItem(filenames_obj, pos);
    if (UTF8_from_PyObj (&filenames[pos], filename_obj) == NULL) {
      free_string_list (pos, filenames);
      free (printer);
      return NULL;
    }
  }
  if (UTF8_from_PyObj (&title, title_obj) == NULL) {
    free_string_list (num_filenames, filenames);
    free (printer);
    return NULL;
  }

  if (!PyDict_Check (options_obj)) {
    free (title);
    free_string_list (num_filenames, filenames);
    free (printer);
    PyErr_SetString (PyExc_TypeError, "options must be a dict");
    return NULL;
  }
  while (PyDict_Next (options_obj, &pos, &key, &val)) {
    char *name, *value;
    if ((!PyUnicode_Check (key) && !PyBytes_Check (key)) ||
        (!PyUnicode_Check (val) && !PyBytes_Check (val))) {
      cupsFreeOptions (num_settings, settings);
      free (title);
      free_string_list (num_filenames, filenames);
      free (printer);
      PyErr_SetString (PyExc_TypeError, "Keys and values must be strings");
      return NULL;
    }

    num_settings = cupsAddOption (UTF8_from_PyObj (&name, key),
				  UTF8_from_PyObj (&value, key),
				  num_settings,
				  &settings);
    free (name);
    free (value);
  }

  Connection_begin_allow_threads (self);
  jobid = cupsPrintFiles2 (self->http, printer, num_filenames,
                           (const char **) filenames, title, num_settings,
						   settings);
  Connection_end_allow_threads (self);

  if (jobid == 0) {
    cupsFreeOptions (num_settings, settings);
    free (title);
    free_string_list (num_filenames, filenames);
    free (printer);
    set_ipp_error (cupsLastError (), cupsLastErrorString ());
    return NULL;
  }

  cupsFreeOptions (num_settings, settings);
  free (title);
  free_string_list (num_filenames, filenames);
  free (printer);
  return PyLong_FromLong (jobid);
}

PyMethodDef Connection_methods[] =
  {
    { "getPrinters",
      (PyCFunction) Connection_getPrinters, METH_NOARGS,
      "getPrinters() -> dict\n\n"
      "@return: a dict, indexed by name, of dicts representing\n"
      "queues, indexed by attribute.\n"
      "@raise IPPError: IPP problem" },

    { "getDests",
      (PyCFunction) Connection_getDests, METH_NOARGS,
      "getDests() -> dict\n\n"
      "@return: a dict representing available destinations.  Each \n"
      "dictionary key is a pair of (queue, instance) strings, and the \n"
      "dictionary value is a L{cups.Dest} object.  In addition to the \n"
      "available destinations, a special dictionary key (None,None) is \n"
      "provided for looking up the default destination; this destination \n"
      "will also be available under its own key.\n"
      "@raise IPPError: IPP problem" },

    { "getClasses",
      (PyCFunction) Connection_getClasses, METH_NOARGS,
      "getClasses() -> dict\n\n"
      "@return: a dict, indexed by name, of objects representing\n"
      "classes.  Each class object is either a string, in which case it\n"
      "is for the remote class; or a list, in which case it is a list of\n"
      "queue names.\n"
      "@raise IPPError: IPP problem" },

    { "getPPDs",
      (PyCFunction) Connection_getPPDs, METH_VARARGS | METH_KEYWORDS,
      "getPPDs(limit=0, exclude_schemes=None, include_schemes=None, \n"
      "ppd_natural_language=None, ppd_device_id=None, ppd_make=None, \n"
      "ppd_make_and_model=None, ppd_model_number=-1, ppd_product=None, \n"
      "ppd_psversion=None, ppd_type=None) -> dict\n\n"
      "@type limit: integer\n"
      "@param limit: maximum number of PPDs to return\n"
      "@type exclude_schemes: string list\n"
      "@param exclude_schemes: list of PPD schemes to exclude\n"
      "@type include_schemes: string list\n"
      "@param include_schemes: list of PPD schemes to include\n"
      "@type ppd_natural_language: string\n"
      "@param ppd_natural_language: required language\n"
      "@type ppd_device_id: string\n"
      "@param ppd_device_id: IEEE 1284 Device ID to match against\n"
      "@type ppd_make: string\n"
      "@param ppd_make: required printer manufacturer\n"
      "@type ppd_make_and_model: string\n"
      "@param ppd_make_and_model: required make and model\n"
      "@type ppd_model_number: integer\n"
      "@param ppd_model_number: model number required (from cupsModelNumber \n"
      "in PPD file)\n"
      "@type ppd_product: string\n"
      "@param ppd_product: required PostScript product string (Product)\n"
      "@type ppd_psversion: string\n"
      "@param ppd_psversion: required PostScript version (PSVersion)\n"
      "@type ppd_type: string\n"
      "@param ppd_type: required type of PPD. Valid values are fax; pdf; \n"
      "postscript; raster; unknown.\n"
      "@return: a dict, indexed by PPD name, of dicts representing\n"
      "PPDs, indexed by attribute.\n"
      "@raise IPPError: IPP problem" },

    { "getPPDs2",
      (PyCFunction) Connection_getPPDs2, METH_VARARGS | METH_KEYWORDS,
      "getPPDs2(limit=0, exclude_schemes=None, include_schemes=None, \n"
      "ppd_natural_language=None, ppd_device_id=None, ppd_make=None, \n"
      "ppd_make_and_model=None, ppd_model_number=-1, ppd_product=None, \n"
      "ppd_psversion=None, ppd_type=None) -> dict\n\n"
      "@type limit: integer\n"
      "@param limit: maximum number of PPDs to return\n"
      "@type exclude_schemes: string list\n"
      "@param exclude_schemes: list of PPD schemes to exclude\n"
      "@type include_schemes: string list\n"
      "@param include_schemes: list of PPD schemes to include\n"
      "@type ppd_natural_language: string\n"
      "@param ppd_natural_language: required language\n"
      "@type ppd_device_id: string\n"
      "@param ppd_device_id: IEEE 1284 Device ID to match against\n"
      "@type ppd_make: string\n"
      "@param ppd_make: required printer manufacturer\n"
      "@type ppd_make_and_model: string\n"
      "@param ppd_make_and_model: required make and model\n"
      "@type ppd_model_number: integer\n"
      "@param ppd_model_number: model number required (from cupsModelNumber \n"
      "in PPD file)\n"
      "@type ppd_product: string\n"
      "@param ppd_product: required PostScript product string (Product)\n"
      "@type ppd_psversion: string\n"
      "@param ppd_psversion: required PostScript version (PSVersion)\n"
      "@type ppd_type: string\n"
      "@param ppd_type: required type of PPD. Valid values are fax; pdf; \n"
      "postscript; raster; unknown.\n"
      "@return: a dict, indexed by PPD name, of dicts representing\n"
      "PPDs, indexed by attribute.  All attribute values are lists.\n"
      "@raise IPPError: IPP problem" },

    { "getServerPPD",
      (PyCFunction) Connection_getServerPPD, METH_VARARGS,
      "getServerPPD(ppd_name) -> string\n\n"
      "Fetches the named PPD and stores it in a temporary file.\n\n"
      "@type ppd_name: string\n"
      "@param ppd_name: the ppd-name of a PPD\n"
      "@return: temporary filename holding the PPD\n"
      "@raise RuntimeError: Not supported in libcups until 1.3\n"
      "@raise IPPError: IPP problem" },
    
    { "getDocument",
      (PyCFunction) Connection_getDocument, METH_VARARGS,
      "getDocument(printer_uri, job_id, document_number) -> dict\n\n"
      "Fetches the job document and stores it in a temporary file.\n\n"
      "@type printer_uri: string\n"
      "@param printer_uri: the printer-uri for the printer\n"
      "@type job_id: integer\n"
      "@param job_id: the job ID\n"
      "@type document_number: integer\n"
      "@param document_number: the document number to retrieve\n"
      "@return: a dict with the following keys: "
      " 'file' (string), temporary filename holding the job document; "
      " 'document-format' (string), its MIME type.  There may also be a "
      " 'document-name' key, in which case this is for the document name.\n"
      "@raise RuntimeError: Not supported in libcups until 1.4\n"
      "@raise IPPError: IPP problem" },
    
    { "getDevices",
      (PyCFunction) Connection_getDevices, METH_VARARGS | METH_KEYWORDS,
      "getDevices(limit=0, exclude_schemes=None, include_schemes=None) -> dict\n\n"
      "@type limit: integer\n"
      "@param limit: maximum number of devices to return\n"
      "@type exclude_schemes: string list\n"
      "@param exclude_schemes: URI schemes to exclude\n"
      "@type include_schemes: string list\n"
      "@param include_schemes: URI schemes to include\n"
      "@return: a dict, indexed by device URI, of dicts representing\n"
      "devices, indexed by attribute.\n"
      "@raise IPPError: IPP problem" },    

    { "getJobs",
      (PyCFunction) Connection_getJobs, METH_VARARGS | METH_KEYWORDS,
      "getJobs(which_jobs='not-completed', my_jobs=False, limit=-1, first_job_id=-1, requested_attributes=None) -> dict\n"
      "Fetch a list of jobs.\n"
      "@type which_jobs: string\n"
      "@param which_jobs: which jobs to fetch; possible values: \n"
      "'completed', 'not-completed', 'all'\n"
      "@type my_jobs: boolean\n"
      "@param my_jobs: whether to restrict the returned jobs to those \n"
      "owned by the current CUPS user (as set by L{cups.setUser}).\n"
      "@return: a dict, indexed by job ID, of dicts representing job\n"
      "attributes.\n"
      "@type limit: integer\n"
      "@param limit: maximum number of jobs to return\n"
      "@type first_job_id: integer\n"
      "@param first_job_id: lowest job ID to return\n"
      "@type requested_attributes: string list\n"
      "@param requested_attributes: list of requested attribute names\n"
      "@raise IPPError: IPP problem" },

    { "getJobAttributes",
      (PyCFunction) Connection_getJobAttributes, METH_VARARGS | METH_KEYWORDS,
      "getJobAttributes(jobid, requested_attributes=None) -> dict\n\n"
      "Fetch job attributes.\n"
      "@type jobid: integer\n"
      "@param jobid: job ID\n"
      "@type requested_attributes: string list\n"
      "@param requested_attributes: list of requested attribute names\n"
      "@return: a dict representing job attributes.\n"
      "@raise IPPError: IPP problem" },

    { "cancelJob",
      (PyCFunction) Connection_cancelJob, METH_VARARGS | METH_KEYWORDS,
      "cancelJob(jobid, purge_job=False) -> None\n\n"
      "@type jobid: integer\n"
      "@param jobid: job ID to cancel\n"
      "@type purge_job: boolean\n"
      "@param purge_job: whether to remove data and control files\n"
      "@raise IPPError: IPP problem" },

    { "cancelAllJobs",
      (PyCFunction) Connection_cancelAllJobs, METH_VARARGS | METH_KEYWORDS,
      "cancelAllJobs(name=None, uri=None, my_jobs=False, purge_jobs=True) -> None\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type uri: string\n"
      "@param uri: printer URI\n"
      "@type my_jobs: boolean\n"
      "@param my_jobs: whether to restrict operation to jobs owned by \n"
      "the current CUPS user (as set by L{cups.setUser}).\n"
      "@type purge_jobs: boolean\n"
      "@param purge_jobs: whether to remove data and control files\n"
      "@raise IPPError: IPP problem" },

    { "moveJob",
      (PyCFunction) Connection_moveJob, METH_VARARGS | METH_KEYWORDS,
      "moveJob(printer_uri=None, job_id=-1, job_printer_uri) -> None\n\n"
      "Move a job specified by printer_uri and jobid (only one need be given)\n"
      "to the printer specified by job_printer_uri.\n\n"
      "@type job_id: integer\n"
      "@param job_id: job ID to move\n"
      "@type printer_uri: string\n"
      "@param printer_uri: printer to move job(s) from\n"
      "@type job_printer_uri: string\n"
      "@param job_printer_uri: printer to move job(s) to\n"
      "@raise IPPError: IPP problem" },

    { "authenticateJob",
      (PyCFunction) Connection_authenticateJob, METH_VARARGS,
      "authenticateJob(jobid, auth_info=None) -> None\n\n"
      "@type jobid: integer\n"
      "@param jobid: job ID to authenticate\n"
      "@type auth_info: optional string list\n"
      "@param auth_info: authentication details\n"
      "@raise IPPError: IPP problem" },

    { "setJobHoldUntil",
      (PyCFunction) Connection_setJobHoldUntil, METH_VARARGS,
      "setJobHoldUntil(jobid, job_hold_until) -> None\n\n"
      "Specifies when a job should be printed.\n"
      "@type jobid: integer\n"
      "@param jobid: job ID to adjust\n"
      "@type job_hold_until: string\n"
      "@param job_hold_until: when to print the job; examples: 'hold', \n"
      "'immediate', 'restart', resume'\n"
      "@raise IPPError: IPP problem"},
    
    { "restartJob",
      (PyCFunction) Connection_restartJob, METH_VARARGS | METH_KEYWORDS,
      "restartJob(job_id, job_hold_until=None) -> None\n\n"
      "Restart a job.\n\n"
      "@type job_id: integer\n"
      "@param job_id: job ID to restart\n"
      "@type job_hold_until: string\n"
      "@param job_hold_until: new job-hold-until value for job\n"
      "@raise IPPError: IPP problem" },

    { "getFile",
      (PyCFunction) Connection_getFile, METH_VARARGS | METH_KEYWORDS,
      "getFile(resource, filename=None, fd=-1, file=None) -> None\n\n"
      "Fetch a CUPS server resource to a local file.\n\n"
      "This is for obtaining CUPS server configuration files and \n"
      "log files.\n\n"
      "@type resource: string\n"
      "@param resource: resource name\n"
      "@type filename: string\n"
      "@param filename: name of local file for storage\n"
      "@type fd: int\n"
      "@param fd: file descriptor of local file\n"
      "@type file: file\n"
      "@param file: Python file object for local file\n"
      "@raise HTTPError: HTTP problem" },

    { "putFile",
      (PyCFunction) Connection_putFile, METH_VARARGS | METH_KEYWORDS,
      "putFile(resource, filename=None, fd=-1, file=None) -> None\n\n"
      "This is for uploading new configuration files for the CUPS \n"
      "server.  Note: L{adminSetServerSettings} is a way of \n"
      "adjusting server settings without needing to parse the \n"
      "configuration file.\n"
      "@type resource: string\n"
      "@param resource: resource name\n"
      "@type filename: string\n"
      "@param filename: name of local file to upload\n"
      "@type fd: int\n"
      "@param fd: file descriptor of local file\n"
      "@type file: file\n"
      "@param file: Python file object for local file\n"
      "@raise HTTPError: HTTP problem"},

    { "addPrinter",
      (PyCFunction) Connection_addPrinter, METH_VARARGS | METH_KEYWORDS,
      "addPrinter(name) -> None\n\n"
      "Add or adjust a print queue.  Several parameters can select which\n"
      "PPD to use (filename, ppdname, and ppd) but only one may be\n"
      "given.\n\n"
      "@type filename: string\n"
      "@keyword filename: local filename of PPD file\n"
      "@type ppdname: string\n"
      "@keyword ppdname: filename from L{getPPDs}\n"
      "@type info: string\n"
      "@keyword info: human-readable information about the printer\n"
      "@type location: string\n"
      "@keyword location: human-readable printer location\n"
      "@type device: string\n"
      "@keyword device: device URI string\n"
      "@type ppd: L{cups.PPD} instance\n"
      "@keyword ppd: PPD object\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterDevice",
      (PyCFunction) Connection_setPrinterDevice, METH_VARARGS,
      "setPrinterDevice(name, device_uri) -> None\n\n"
      "Set the device URI for a printer.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type device_uri: string\n"
      "@param device_uri: device URI\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterInfo",
      (PyCFunction) Connection_setPrinterInfo, METH_VARARGS,
      "setPrinterInfo(name, info) -> None\n\n"
      "Set the human-readable information about a printer.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type info: string\n"
      "@param info: human-readable information about the printer\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterLocation",
      (PyCFunction) Connection_setPrinterLocation, METH_VARARGS,
      "setPrinterLocation(name, location) -> None\n\n"
      "Set the human-readable printer location\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type location: string\n"
      "@param location: human-readable printer location\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterShared",
      (PyCFunction) Connection_setPrinterShared, METH_VARARGS,
      "setPrinterShared(name, shared) -> None\n\n"
      "Set whether a printer is shared with other people.  This works \n"
      "with CUPS servers of at least version 1.2, by setting the \n"
      "printer-is-shared printer attribute.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type shared: boolean\n"
      "@param shared: whether printer should be shared\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterJobSheets",
      (PyCFunction) Connection_setPrinterJobSheets, METH_VARARGS,
      "setPrinterJobSheets(name, start, end) -> None\n\n"
      "Specifies job sheets for a printer.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type start: string\n"
      "@param start: name of a sheet to print before each job\n"
      "@type end: string\n"
      "@param end: name of a sheet to print after each job\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterErrorPolicy",
      (PyCFunction) Connection_setPrinterErrorPolicy, METH_VARARGS,
      "setPrinterErrorPolicy(name, policy) -> None\n\n"
      "Set the printer's error policy.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type policy: string\n"
      "@param policy: policy name; supported policy names can be found \n"
      "by using the L{getPrinterAttributes} function and looking for the \n"
      "'printer-error-policy-supported' attribute\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterOpPolicy",
      (PyCFunction) Connection_setPrinterOpPolicy, METH_VARARGS,
      "setPrinterOpPolicy(name, policy) -> None\n\n"
      "Set the printer's operation policy.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type policy: string\n"
      "@param policy: policy name; supported policy names can be found \n"
      "by using the L{getPrinterAttributes} function and looking for the \n"
      "'printer-op-policy-supported' attribute\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterUsersAllowed",
      (PyCFunction) Connection_setPrinterUsersAllowed, METH_VARARGS,
      "setPrinterUsersAllowed(name, allowed) -> None\n\n"
      "Set the list of users allowed to use a printer.  This works \n"
      "with CUPS server of at least version 1.2, by setting the \n"
      "requesting-user-name-allowed printer attribute.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type allowed: string list\n"
      "@param allowed: list of allowed users; ['all'] \n"
      "means there will be no user-name restriction.\n"
      "@raise IPPError: IPP problem" },

    { "setPrinterUsersDenied",
      (PyCFunction) Connection_setPrinterUsersDenied, METH_VARARGS,
      "setPrinterUsersDenied(name, denied) -> None\n\n"
      "Set the list of users denied the use of a printer.  This works \n"
      "with CUPS servers of at least version 1.2, by setting the \n"
      "requesting-user-name-denied printer attribute.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type denied: string list\n"
      "@param denied: list of denied users; ['none'] \n"
      "means there will be no user-name restriction.\n"
      "@raise IPPError: IPP problem" },

    { "addPrinterOptionDefault",
      (PyCFunction) Connection_addPrinterOptionDefault, METH_VARARGS,
      "addPrinterOptionDefault(name, option, value) -> None\n\n"
      "Set a network default option.  Jobs submitted to the named queue \n"
      "will have the job option added if it is not already present in the \n"
      "job.  This works with CUPS servers of at least version 1.2.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type option: string\n"
      "@param option: option name, for example 'job-priority'\n"
      "@type value: string\n"
      "@param value: option value as a string\n"
      "@raise IPPError: IPP problem" },

    { "deletePrinterOptionDefault",
      (PyCFunction) Connection_deletePrinterOptionDefault, METH_VARARGS,
      "deletePrinterOptionDefault(name, option) -> None\n\n"
      "Removes a network default option.  See L{addPrinterOptionDefault}.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type option: string\n"
      "@param option: option name, for example 'job-priority'\n"
      "@raise IPPError: IPP problem" },

    { "deletePrinter",
      (PyCFunction) Connection_deletePrinter, METH_VARARGS | METH_KEYWORDS,
      "deletePrinter(name) -> None\n\n"
      "Delete a printer.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@raise IPPError: IPP problem" },

    { "getPrinterAttributes",
      (PyCFunction) Connection_getPrinterAttributes,
      METH_VARARGS | METH_KEYWORDS,
      "getPrinterAttributes(name=None, uri=None, requested_attributes=None) -> dict\n"
      "Fetch the attributes for a printer, specified either by name or by \n"
      "uri but not both.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type uri: string\n"
      "@param uri: queue URI\n"
      "@type requested_attributes: string list\n"
      "@param requested_attributes: list of requested attribute names\n"
      "@return: a dict, indexed by attribute, of printer attributes\n"
      "for the specified printer.\n\n"
      "Attributes:\n"
      "  - 'job-sheets-supported': list of strings\n"
      "  - 'job-sheets-default': tuple of strings (start, end)\n"
      "  - 'printer-error-policy-supported': if present, list of strings\n"
      "  - 'printer-error-policy': if present, string\n"
      "  - 'printer-op-policy-supported': if present, list of strings\n"
      "  - 'printer-op-policy': if present, string\n\n"
      "There are other attributes; the exact list of attributes returned \n"
      "will depend on the IPP server.\n"
      "@raise IPPError: IPP problem"},

    { "addPrinterToClass",
      (PyCFunction) Connection_addPrinterToClass, METH_VARARGS,
      "addPrinterToClass(name, class) -> None\n\n"
      "Add a printer to a class.  If the class does not yet exist, \n"
      "it is created.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type class: string\n"
      "@param class: class name\n"
      "@raise IPPError: IPP problem" },

    { "deletePrinterFromClass",
      (PyCFunction) Connection_deletePrinterFromClass, METH_VARARGS,
      "deletePrinterFromClass(name, class) -> None\n\n"
      "Remove a printer from a class.  If the class would be left empty, \n"
      "it is removed.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type class: string\n"
      "@param class: class name\n"
      "@raise IPPError: IPP problem" },

    { "deleteClass",
      (PyCFunction) Connection_deleteClass, METH_VARARGS,
      "deleteClass(class) -> None\n\n"
      "Delete a class.\n\n"
      "@type class: string\n"
      "@param class: class name\n"
      "@raise IPPError: IPP problem" },

    { "getDefault",
      (PyCFunction) Connection_getDefault, METH_NOARGS,
      "getDefault() -> string or None\n\n"
      "Get the system default printer.\n\n"
      "@return: default printer name or None" },

    { "setDefault",
      (PyCFunction) Connection_setDefault, METH_VARARGS | METH_KEYWORDS,
      "setDefault(name) -> None\n\n"
      "Set the system default printer.  Note that this can be over-ridden \n"
      "on a per-user basis using the lpoptions command.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@raise IPPError: IPP problem" },

    { "getPPD",
      (PyCFunction) Connection_getPPD, METH_VARARGS,
      "getPPD(name) -> string\n\n"
      "Fetch a printer's PPD.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@return: temporary PPD file name\n"
      "@raise IPPError: IPP problem" },

    { "getPPD3",
      (PyCFunction) Connection_getPPD3, METH_VARARGS | METH_KEYWORDS,
      "getPPD3(name[, modtime, filename]) -> (status,modtime,filename)\n\n"
      "Fetch a printer's PPD if it is newer.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type modtime: float\n"
      "@param modtime: modification time of existing file\n"
      "@type filename: string\n"
      "@param filename: filename of existing file\n"
      "@return: tuple of HTTP status, modification time, and filename\n" },

    { "enablePrinter",
      (PyCFunction) Connection_enablePrinter, METH_VARARGS | METH_KEYWORDS,
      "enablePrinter(name) -> None\n\n"
      "Enable printer.  This allows the printer to process its job queue.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@raise IPPError: IPP problem" },

    { "disablePrinter",
      (PyCFunction) Connection_disablePrinter, METH_VARARGS | METH_KEYWORDS,
      "disablePrinter(name) -> None\n\n"
      "Disable printer.  This prevents the printer from processing its \n"
      "job queue.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type reason: string\n"
      "@keyword reason: optional human-readable reason for disabling the \n"
      "printer\n"
      "@raise IPPError: IPP problem" },

    { "acceptJobs",
      (PyCFunction) Connection_acceptJobs, METH_VARARGS | METH_KEYWORDS,
      "acceptJobs(name) -> None\n\n"
      "Cause printer to accept jobs.\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@raise IPPError: IPP problem" },

    { "rejectJobs",
      (PyCFunction) Connection_rejectJobs, METH_VARARGS | METH_KEYWORDS,
      "rejectJobs(name)\n\n"
      "Cause printer to reject jobs.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type reason: string\n"
      "@keyword reason: optional human-readable reason for rejecting jobs\n"
      "@raise IPPError: IPP problem" },

    { "printTestPage",
      (PyCFunction) Connection_printTestPage, METH_VARARGS | METH_KEYWORDS,
      "printTestPage(name) -> job ID\n\n"
      "Print a test page.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type file: string\n"
      "@keyword file: input file (default is CUPS test page)\n"
      "@type title: string\n"
      "@keyword title: job title (default 'Test Page')\n"
      "@type format: string\n"
      "@keyword format: document format (default 'application/postscript')\n"
      "@type user: string\n"
      "@keyword user: user to submit the job as\n"
      "@raise IPPError: IPP problem" },

    { "adminExportSamba",
      (PyCFunction) Connection_adminExportSamba, METH_VARARGS,
      "adminExportSamba(name, samba_server, samba_username,\n"
      "                 samba_password) -> None\n\n"
      "Export a printer to Samba.\n\n"
      "@type name: string\n"
      "@param name: queue name\n"
      "@type samba_server: string\n"
      "@param samba_server: samba server\n"
      "@type samba_username: string\n"
      "@param samba_username: samba username\n"
      "@type samba_password: string\n"
      "@param samba_password: samba password\n"
      "@raise IPPError: IPP problem" },

    { "adminGetServerSettings",
      (PyCFunction) Connection_adminGetServerSettings, METH_NOARGS,
      "adminGetServerSettings() -> dict\n\n"
      "Get server settings.\n\n"
      "@return: dict representing server settings; keywords include \n"
      "L{CUPS_SERVER_DEBUG_LOGGING}, L{CUPS_SERVER_REMOTE_ADMIN}, \n"
      "L{CUPS_SERVER_REMOTE_PRINTERS}, L{CUPS_SERVER_SHARE_PRINTERS}, \n"
      "L{CUPS_SERVER_USER_CANCEL_ANY}\n"
      "@see: L{adminSetServerSettings}\n"
      "@raise IPPError: IPP problem" },

    { "adminSetServerSettings",
      (PyCFunction) Connection_adminSetServerSettings, METH_VARARGS,
      "adminSetServerSettings(settings) -> None\n\n"
      "Set server settings.\n\n"
      "@type settings: dict\n"
      "@param settings: dict of server settings\n"
      "@see: L{adminGetServerSettings}\n"
      "@raise IPPError: IPP problem" },

    { "getSubscriptions",
      (PyCFunction) Connection_getSubscriptions, METH_VARARGS | METH_KEYWORDS,
      "getSubscriptions(uri) -> integer list\n\n"
      "Get subscriptions.\n\n"
      "@type uri: string\n"
      "@param uri: URI for object\n"
      "@type my_subscriptions: boolean\n"
      "@keyword my_subscriptions: only return subscriptions belonging to \n"
      "the current user (default False)\n"
      "@type job_id: integer\n"
      "@keyword job_id: only return subscriptions relating to this job\n"
      "@return: list of subscriptions\n"
      "@raise IPPError: IPP problem" },

    { "createSubscription",
      (PyCFunction) Connection_createSubscription,
      METH_VARARGS | METH_KEYWORDS,
      "createSubscription(uri, events=[], job_id=-1, recipient_uri="",\n"
      "                   lease_duration=-1, time_interval=-1,\n"
      "                   user_data="") -> integer\n\n"
      "Create a subscription.\n\n"
      "@type uri: string\n"
      "@param uri: URI for object\n"
      "@type events: string list\n"
      "@keyword events: events to receive notifications for\n"
      "@type job_id: integer\n"
      "@keyword job_id: job ID to receive notifications for\n"
      "@type recipient_uri: string\n"
      "@keyword recipient_uri: URI for notifications recipient\n"
      "@type lease_duration: integer\n"
      "@keyword lease_duration: lease duration in seconds\n"
      "@type time_interval: integer\n"
      "@keyword time_interval: time interval\n"
      "@type user_data: string\n"
      "@keyword user_data: user data to receive with notifications\n"
      "@return: subscription ID\n"
      "@raise IPPError: IPP problem" },

    { "getNotifications",
      (PyCFunction) Connection_getNotifications, METH_VARARGS | METH_KEYWORDS,
      "getNotifications(subscription_ids) -> list\n\n"
      "Get notifications for subscribed events.\n\n"
      "@type subscription_ids: integer list\n"
      "@param subscription_ids: list of subscription IDs to receive \n"
      "notifications for\n"
      "@return: list of dicts, each representing an event\n"
      "@raise IPPError: IPP problem" },

    { "cancelSubscription",
      (PyCFunction) Connection_cancelSubscription, METH_VARARGS,
      "cancelSubscription(id) -> None\n\n"
      "Cancel a subscription.\n\n"
      "@type id: integer\n"
      "@param id: subscription ID\n"
      "@raise IPPError: IPP problem" },

    { "renewSubscription",
      (PyCFunction) Connection_renewSubscription, METH_VARARGS | METH_KEYWORDS,
      "renewSubscription(id, lease_duration=-1) -> None\n\n"
      "Renew a subscription.\n\n"
      "@type id: integer\n"
      "@param id: subscription ID\n"
      "@type lease_duration: integer\n"
      "@param lease_duration: lease duration in seconds\n"
      "@raise IPPError: IPP problem" },

    { "createJob",
      (PyCFunction) Connection_createJob, METH_VARARGS | METH_KEYWORDS,
      "createJob(printer, title, options) -> integer\n\n"
      "Create an empty job for streaming.\n\n"
      "@type printer: string\n"
      "@param printer: queue name\n"
      "@type title: string\n"
      "@param title: title of the print job\n"
      "@type options: dict\n"
      "@param options: dict of options\n"
      "@return: job ID\n"
      "@raise IPPError: IPP problem" },

    { "startDocument",
      (PyCFunction) Connection_startDocument, METH_VARARGS | METH_KEYWORDS,
      "startDocument(printer, job_id, doc_name, format, last_document) -> integer\n\n"
      "Add a document to a job created with createJob.\n\n"
      "@type printer: string\n"
      "@param printer: queue name\n"
      "@type job_id: integer\n"
      "@param job_id: job ID to create document\n"
      "@type doc_name: string\n"
      "@param doc_name: name of the document\n"
      "@type format: string\n"
      "@param format: MIME type\n"
      "@type last_document: integer\n"
      "@param last_document: 1 for last document of job, 0 otherwise\n"
      "@return: HTTP status\n"
      "@raise IPPError: IPP problem" },

    { "writeRequestData",
      (PyCFunction) Connection_writeRequestData, METH_VARARGS | METH_KEYWORDS,
      "writeRequestData(buffer, length) -> integer\n\n"
      "Write data after an IPP request.\n\n"
      "@type buffer: string\n"
      "@param buffer: bytes to write\n"
      "@type length: integer\n"
      "@param length: number of bytes to write\n"
      "@return: HTTP status\n"
      "@raise IPPError: IPP problem" },

    { "finishDocument",
      (PyCFunction) Connection_finishDocument, METH_VARARGS | METH_KEYWORDS,
      "finishDocument(printer) -> integer\n\n"
      "Finish sending a document.\n\n"
      "@type printer: string\n"
      "@param printer: queue name\n"
      "@return: HTTP status\n"
      "@raise IPPError: IPP problem" },

    { "printFile",
      (PyCFunction) Connection_printFile, METH_VARARGS | METH_KEYWORDS,
      "printFile(printer, filename, title, options) -> integer\n\n"
      "Print a file.\n\n"
      "@type printer: string\n"
      "@param printer: queue name\n"
      "@type filename: string\n"
      "@param filename: local file path to the document\n"
      "@type title: string\n"
      "@param title: title of the print job\n"
      "@type options: dict\n"
      "@param options: dict of options\n"
      "@return: job ID\n"
      "@raise IPPError: IPP problem" },

    { "printFiles",
      (PyCFunction) Connection_printFiles, METH_VARARGS | METH_KEYWORDS,
      "printFiles(printer, filenames, title, options) -> integer\n\n"
      "Print a list of files.\n\n"
      "@type printer: string\n"
      "@param printer: queue name\n"
      "@type filenames: list\n"
      "@param filenames: list of local file paths to the documents\n"
      "@type title: string\n"
      "@param title: title of the print job\n"
      "@type options: dict\n"
      "@param options: dict of options\n"
      "@return: job ID\n"
      "@raise IPPError: IPP problem" },

    { NULL } /* Sentinel */
  };

PyTypeObject cups_ConnectionType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.Connection",         /*tp_name*/
    sizeof(Connection),        /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Connection_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    (reprfunc)Connection_repr, /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT |
    Py_TPFLAGS_BASETYPE,       /*tp_flags*/
    "CUPS connection\n"
    "===============\n\n"

    "  A connection to the CUPS server.  Before it is created the \n"
    "  connection server and username should be set using \n"
    "  L{cups.setServer} and L{cups.setUser}; otherwise the defaults will \n"
    "  be used.  When a Connection object is instantiated it results in a \n"
    "  call to the libcups function httpConnectEncrypt().\n\n"
    "  The constructor takes optional arguments host, port, and encryption, \n"
    "  which default to the values of L{cups.getServer}(), \n"
    "  L{cups.getPort}(), and L{cups.getEncryption}().\n"
    "",         /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    Connection_methods,        /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Connection_init, /* tp_init */
    0,                         /* tp_alloc */
    Connection_new,            /* tp_new */
  };

//////////
// Dest //
//////////

static PyObject *
Dest_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  Dest *self;
  self = (Dest *) type->tp_alloc (type, 0);
  return (PyObject *) self;
}

static int
Dest_init (Dest *self, PyObject *args, PyObject *kwds)
{    
  self->num_options = 0;
  return 0;
}

static void
Dest_dealloc (Dest *self)
{
  if (self->num_options) {
    int i;
    for (i = 0; i < self->num_options; i++) {
      free (self->name[i]);
      free (self->value[i]);
    }

    free (self->name);
    free (self->value);
    self->num_options = 0;

    free (self->destname);
    free (self->instance);
  }
  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

static PyObject *
Dest_repr (Dest *self)
{
  char buffer[256];
  snprintf (buffer, 256, "<cups.Dest %s%s%s%s>",
			  self->destname,
			  self->instance ? "/" : "",
			  self->instance ? self->instance : "",
			  self->is_default ? " (default)" : "");
  return PyUnicode_FromString (buffer);
}

//////////
// Dest // ATTRIBUTES
//////////

static PyObject *
Dest_getName (Dest *self, void *closure)
{
  return PyUnicode_FromString (self->destname);
}

static PyObject *
Dest_getInstance (Dest *self, void *closure)
{
  if (self->instance)
    return PyUnicode_FromString (self->instance);

  Py_RETURN_NONE;
}

static PyObject *
Dest_getIsDefault (Dest *self, void *closure)
{
  return PyBool_FromLong (self->is_default);
}

static PyObject *
Dest_getOptions (Dest *self, void *closure)
{
  PyObject *pyoptions = PyDict_New ();
  int i;
  for (i = 0; i < self->num_options; i++) {
    PyObject *string = PyUnicode_FromString (self->value[i]);
    PyDict_SetItemString (pyoptions, self->name[i], string);
    Py_DECREF (string);
  }

  return pyoptions;
}

PyGetSetDef Dest_getseters[] =
  {
    { "name",
      (getter) Dest_getName, (setter) NULL,
      "name", NULL },

    { "instance",
      (getter) Dest_getInstance, (setter) NULL,
      "instance", NULL },

    { "is_default",
      (getter) Dest_getIsDefault, (setter) NULL,
      "is_default", NULL },
  
    { "options",
      (getter) Dest_getOptions, (setter) NULL,
      "options", NULL },

    { NULL }
  };

PyTypeObject cups_DestType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.Dest",               /*tp_name*/
    sizeof(Dest),              /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Dest_dealloc,  /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    (reprfunc)Dest_repr,       /*tp_repr*/
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
    "CUPS destination\n"
    "================\n\n"
    "  A destination print queue, as returned by L{Connection.getDests}.\n\n"
    "@type name: string\n"
    "@ivar name: destination queue name\n"
    "@type instance: string\n"
    "@ivar instance: destination instance name\n"
    "@type is_default: boolean\n"
    "@ivar is_default: whether this is the default destination\n"
    "@type options: dict\n"
    "@ivar options: string:string dict of default options for this \n"
    "destination, indexed by option name\n"
    "",        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    0,                         /* tp_methods */
    0,                         /* tp_members */
    Dest_getseters,            /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Dest_init,       /* tp_init */
    0,                         /* tp_alloc */
    Dest_new,                  /* tp_new */
  };
