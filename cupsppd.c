/*
 * cups - Python bindings for CUPS
 * Copyright (C) 2002-2020  Tim Waugh <twaugh@redhat.com>
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

#include "cupsppd.h"
#include "cupsmodule.h"

#include <ctype.h>
#include <iconv.h>
#include <stdbool.h>

typedef struct
{
  PyObject_HEAD
  ppd_option_t *option;
  PPD *ppd;
} Option;

typedef struct
{
  PyObject_HEAD
  ppd_const_t *constraint;
  PPD *ppd;
} Constraint;

typedef struct
{
  PyObject_HEAD
  ppd_group_t *group;
  PPD *ppd;
} Group;

typedef struct
{
  PyObject_HEAD
  ppd_attr_t *attribute;
  PPD *ppd;
} Attribute;

/////////////////////////
// Encoding conversion //
/////////////////////////

static int
ppd_encoding_is_utf8 (PPD *ppd)
{
  const char *lang_encoding, *from_encoding;
  iconv_t cdf, cdt;
  if (ppd->conv_from != NULL)
    return 0;

  lang_encoding = ppd->ppd->lang_encoding;
  if (lang_encoding && !strcasecmp (lang_encoding, "UTF-8"))
    return 1;

  if (lang_encoding && !strcasecmp (lang_encoding, "ISOLatin1"))
    from_encoding = "ISO-8859-1";
  else if (lang_encoding && !strcasecmp (lang_encoding, "ISOLatin2"))
    from_encoding = "ISO-8859-2";
  else if (lang_encoding && !strcasecmp (lang_encoding, "ISOLatin5"))
    from_encoding = "ISO-8859-5";
  else if (lang_encoding && !strcasecmp (lang_encoding, "JIS83-RKSJ"))
    from_encoding = "SHIFT-JIS";
  else if (lang_encoding && !strcasecmp (lang_encoding, "MacStandard"))
    from_encoding = "MACINTOSH";
  else if (lang_encoding && !strcasecmp (lang_encoding, "WindowsANSI"))
    from_encoding = "WINDOWS-1252";
  else
    // Guess
    from_encoding = "ISO-8859-1";

  cdf = iconv_open ("UTF-8", from_encoding);
  if (cdf == (iconv_t) -1)
    cdf = iconv_open ("UTF-8", "ISO-8859-1");

  cdt = iconv_open (from_encoding, "UTF-8");
  if (cdt == (iconv_t) -1)
    cdt = iconv_open ("ISO-8859-1", "UTF-8");

  ppd->conv_from = malloc (sizeof (iconv_t));
  *ppd->conv_from = cdf;

  ppd->conv_to = malloc (sizeof (iconv_t));
  *ppd->conv_to = cdt;

  return 0;
}

static PyObject *
cautious_PyUnicode_DecodeUTF8 (const char *str, size_t len)
{
  PyObject *ret = PyUnicode_DecodeUTF8 (str, len, NULL);
  if (ret == NULL) {
    // It wasn't UTF-8 after all.  Just make the string safe for ASCII.
    char *safe;
    size_t i;

    PyErr_Clear ();
    safe = malloc (len + 1);
    for (i = 0; i < len; i++) {
      unsigned char ch = str[i];
      if (!isascii (ch))
	ch = '?';

      safe[i] = ch;
    }
    safe[i] = '\0';
    ret = PyUnicode_DecodeUTF8 (safe, len, NULL);
    printf ("Bad UTF-8 string \"%s\" changed to \"%s\"\n", str, safe);
    free (safe);
  }

  return ret;
}

static PyObject *
make_PyUnicode_from_ppd_string (PPD *ppd, const char *ppdstr)
{
  iconv_t cdf;
  size_t len;
  char *outbuf, *outbufptr;
  size_t outbytesleft;
  size_t origleft;
  PyObject *ret;

  if (ppd_encoding_is_utf8 (ppd))
    return cautious_PyUnicode_DecodeUTF8 (ppdstr, strlen (ppdstr));

  cdf = *ppd->conv_from;

  // Reset to initial state
  iconv (cdf, NULL, NULL, NULL, NULL);
  len = strlen (ppdstr); // CUPS doesn't keep track of string lengths
  origleft = outbytesleft = MB_CUR_MAX * len;
  outbufptr = outbuf = malloc (outbytesleft);
  if (iconv (cdf, (char **) &ppdstr, &len,
	     &outbufptr, &outbytesleft) == (size_t) -1) {
    free (outbuf);
    return PyErr_SetFromErrno (PyExc_RuntimeError);
  }

  ret = cautious_PyUnicode_DecodeUTF8 (outbuf, origleft - outbytesleft);
  free (outbuf);
  return ret;
}

static char *
utf8_to_ppd_encoding (PPD *ppd, const char *inbuf)
{
  char *outbuf, *ret;
  size_t len, outsize, outbytesleft;
  iconv_t cdt;

  if (ppd_encoding_is_utf8 (ppd))
    return strdup (inbuf);

  cdt = *ppd->conv_to;

  // Reset to initial state
  iconv (cdt, NULL, NULL, NULL, NULL);
  len = strlen (inbuf);
  outsize = 1 + 6 * len;
  outbytesleft = outsize - 1;
  ret = outbuf = malloc (outsize);
  if (iconv (cdt, (char **) &inbuf, &len,
	     &outbuf, &outbytesleft) == (size_t) -1) {
    free (outbuf);
    return NULL;
  }
  *outbuf = '\0';

  return ret;
}

/////////
// PPD //
/////////

static PyObject *
PPD_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  PPD *self;
  self = (PPD *) type->tp_alloc (type, 0);
  if (self != NULL) {
    self->ppd = NULL;
    self->file = NULL;
    self->conv_from = NULL;
    self->conv_to = NULL;
  }

  return (PyObject *) self;
}

static int
PPD_init (PPD *self, PyObject *args, PyObject *kwds)
{
  PyObject *filenameobj;
  char *filename;

  if (!PyArg_ParseTuple (args, "O", &filenameobj))
    return -1;

  if (UTF8_from_PyObj (&filename, filenameobj) == NULL)
    return -1;

  self->file = fopen (filename, "r");
  if (!self->file) {
    PyErr_SetString (PyExc_RuntimeError, "fopen failed");
    free (filename);
    return -1;
  }

  debugprintf ("+ PPD %p %s (fd %d)\n", self, filename, fileno (self->file));
  self->ppd = ppdOpenFile (filename);
  free (filename);
  if (!self->ppd) {
    fclose (self->file);
    self->file = NULL;
    PyErr_SetString (PyExc_RuntimeError, "ppdOpenFile failed");
    return -1;
  }
  self->conv_from = self->conv_to = NULL;

  return 0;
}

static void
PPD_dealloc (PPD *self)
{
  if (self->file) {
    debugprintf ("- PPD %p (fd %d)\n", self, fileno (self->file));
    fclose (self->file);
  } else
    debugprintf ("- PPD %p (no fd)\n", self);

  if (self->ppd)
    ppdClose (self->ppd);
  if (self->conv_from)
    iconv_close (*self->conv_from);
  if (self->conv_to)
    iconv_close (*self->conv_to);

  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

/////////
// PPD // METHODS
/////////

static PyObject *
PPD_localize (PPD *self)
{
  if (!ppdLocalize (self->ppd))
    Py_RETURN_NONE;
  return PyErr_SetFromErrno (PyExc_RuntimeError);
}

static PyObject *
PPD_localizeIPPReason (PPD *self, PyObject *args, PyObject *kwds)
{
  PyObject *ret;
  PyObject *reasonobj;
  PyObject *schemeobj = NULL;
  char *reason;
  char *scheme = NULL;
  char *buffer;
  const size_t bufsize = 1024;
  static char *kwlist[] = { "reason", "scheme", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|O", kwlist,
				    &reasonobj, &schemeobj))
    return NULL;

  if (UTF8_from_PyObj (&reason, reasonobj) == NULL)
    return NULL;

  if (schemeobj)
    if (UTF8_from_PyObj (&scheme, schemeobj) == NULL) {
      free (reason);
      return NULL;
    }

  buffer = malloc (bufsize);
  if (ppdLocalizeIPPReason (self->ppd, reason, scheme, buffer, bufsize))
  {
    ret = make_PyUnicode_from_ppd_string (self, buffer);
  } else {
    Py_RETURN_NONE;
  }

  free (reason);
  if (scheme)
    free (scheme);
  free (buffer);
  return ret;
}

static PyObject *
PPD_localizeMarkerName (PPD *self, PyObject *args)
{
  PyObject *ret;
  PyObject *nameobj;
  char *name;
  const char *lname;

  if (!PyArg_ParseTuple (args, "O", &nameobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  lname = ppdLocalizeMarkerName (self->ppd, name);
  free (name);

  if (lname != NULL)
  {
    ret = make_PyUnicode_from_ppd_string (self, lname);
  } else {
    Py_RETURN_NONE;
  }

  return ret;
}

static PyObject *
PPD_markDefaults (PPD *self)
{
  ppdMarkDefaults (self->ppd);
  Py_RETURN_NONE;
}

static PyObject *
PPD_markOption (PPD *self, PyObject *args)
{
  int conflicts;
  char *name, *value;
  char *encname, *encvalue;
  if (!PyArg_ParseTuple (args, "eses", "UTF-8", &name, "UTF-8", &value))
    return NULL;

  encname = utf8_to_ppd_encoding (self, name);
  PyMem_Free (name);
  if (!encname) {
    PyMem_Free (value); 
    return PyErr_SetFromErrno (PyExc_RuntimeError);
  }

  encvalue = utf8_to_ppd_encoding (self, value);
  PyMem_Free (value);
  if (!encvalue) {
    free (encname);
    return PyErr_SetFromErrno (PyExc_RuntimeError);
  }

  conflicts = ppdMarkOption (self->ppd, encname, encvalue);
  free (encname);
  free (encvalue);
  return Py_BuildValue ("i", conflicts);
}

static PyObject *
PPD_conflicts (PPD *self)
{
  return PyLong_FromLong (ppdConflicts (self->ppd));
}

static PyObject *
PPD_findOption (PPD *self, PyObject *args)
{
  PyObject *ret;
  PyObject *optionobj;
  char *option;
  ppd_option_t *opt;

  if (!PyArg_ParseTuple (args, "O", &optionobj))
    return NULL;

  if (UTF8_from_PyObj (&option, optionobj) == NULL)
    return NULL;

  opt = ppdFindOption (self->ppd, option);
  free (option);
  if (opt) {
    PyObject *args = Py_BuildValue ("()");
    PyObject *kwlist = Py_BuildValue ("{}");
    Option *optobj = (Option *) PyType_GenericNew (&cups_OptionType,
						   args, kwlist);
    Py_DECREF (args);
    Py_DECREF (kwlist);
    optobj->option = opt;
    optobj->ppd = self;
    Py_INCREF (self);
    ret = (PyObject *) optobj;
  } else {
    Py_RETURN_NONE;
  }

  return ret;
}

static PyObject *
PPD_findAttr (PPD *self, PyObject *args, PyObject *kwds)
{
  PyObject *ret;
  PyObject *nameobj;
  PyObject *specobj = NULL;
  char *name;
  char *spec = NULL;
  ppd_attr_t *attr;
  static char *kwlist[] = { "name", "spec", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|O", kwlist,
				    &nameobj, &specobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (specobj)
    if (UTF8_from_PyObj (&spec, specobj) == NULL) {
      free (name);
      return NULL;
    }

  attr = ppdFindAttr (self->ppd, name, spec);
  free (name);
  if (spec)
    free (spec);
  if (attr) {
    PyObject *largs = Py_BuildValue ("()");
    PyObject *lkwlist = Py_BuildValue ("{}");
    Attribute *attrobj = (Attribute *) PyType_GenericNew (&cups_AttributeType,
							  largs, lkwlist);
    Py_DECREF (largs);
    Py_DECREF (lkwlist);
    attrobj->attribute = attr;
    attrobj->ppd = self;
    Py_INCREF (self);
    ret = (PyObject *) attrobj;
  } else {
    Py_RETURN_NONE;
  }

  return ret;
}

static PyObject *
PPD_findNextAttr (PPD *self, PyObject *args, PyObject *kwds)
{
  PyObject *ret;
  PyObject *nameobj;
  PyObject *specobj = NULL;
  char *name;
  char *spec = NULL;
  ppd_attr_t *attr;
  static char *kwlist[] = { "name", "spec", NULL };

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "O|O", kwlist,
				    &nameobj, &specobj))
    return NULL;

  if (UTF8_from_PyObj (&name, nameobj) == NULL)
    return NULL;

  if (specobj)
    if (UTF8_from_PyObj (&spec, specobj) == NULL) {
      free (name);
      return NULL;
    }

  attr = ppdFindNextAttr (self->ppd, name, spec);
  free (name);
  if (spec)
    free (spec);
  if (attr) {
    PyObject *largs = Py_BuildValue ("()");
    PyObject *lkwlist = Py_BuildValue ("{}");
    Attribute *attrobj = (Attribute *) PyType_GenericNew (&cups_AttributeType,
							  largs, lkwlist);
    Py_DECREF (largs);
    Py_DECREF (lkwlist);
    attrobj->attribute = attr;
    attrobj->ppd = self;
    Py_INCREF (self);
    ret = (PyObject *) attrobj;
  } else {
    Py_RETURN_NONE;
  }

  return ret;
}

static int
nondefaults_are_marked (ppd_group_t *g)
{
  ppd_option_t *o;
  int oi;
  for (oi = 0, o = g->options;
       oi < g->num_options;
       oi++, o++) {
    ppd_choice_t *c;
    int ci;
    for (ci = 0, c = o->choices;
	 ci < o->num_choices;
	 ci++, c++) {
      if (c->marked) {
	if (strcmp (c->choice, o->defchoice))
	  return 1;
	break;
      }
    }  
  }

  return 0;
}

static PyObject *
PPD_nondefaultsMarked (PPD *self)
{
  int nondefaults_marked = 0;
  ppd_group_t *g;
  int gi;
  for (gi = 0, g = self->ppd->groups;
       gi < self->ppd->num_groups && !nondefaults_marked;
       gi++, g++) {
    ppd_group_t *sg;
    int sgi;  
    if (nondefaults_are_marked (g)) {
      nondefaults_marked = 1;
      break;
    }

    for (sgi = 0, sg = g->subgroups;
	 sgi < g->num_subgroups;
	 sgi++, sg++) {
      if (nondefaults_are_marked (sg)) {
	nondefaults_marked = 1;
	break;
      }
    }
  }

  return PyBool_FromLong (nondefaults_marked);
}

#if !(_POSIX_C_SOURCE >= 200809L || _XOPEN_SOURCE >= 700)
/*
 * A rudimentary emulation of getline() for systems that dont support it
 * natively.  Since this is used for PPD file reading, it assumes (possibly
 * falsely) that BUFSIZ is big enough.
 */
ssize_t
getline(char **line, size_t *linelen, FILE *fp)
{
  if (*linelen == 0) {
    *linelen = BUFSIZ;
    *line = malloc(*linelen);
  }

  memset(*line, 0, *linelen);
  fgets(*line, *linelen, fp);

  return (strlen(*line));
}
#endif

/*
 * emit marked options by returning a string.
 */

static PyObject *
PPD_emitString (PPD *self, PyObject *args)
{
  ppd_section_t section;
  float min_order;
  char *emitted;
  PyObject *ret;

  if (!PyArg_ParseTuple (args, "if", &section, &min_order))
    return NULL;

  emitted = ppdEmitString(self->ppd, section, min_order);

  if (emitted) {
    ret = PyUnicode_FromString (emitted);
    free (emitted);
  } else {
    Py_RETURN_NONE;
  }

  return ret;
}

/*
 * emit marked options by writing to a file object.
 */

static PyObject *
PPD_emit (PPD *self, PyObject *args)
{
  PyObject *pyFile;
  ppd_section_t section;
  FILE *f;

  if (!PyArg_ParseTuple (args, "Oi", &pyFile, &section))
    return NULL;

  int fd = PyObject_AsFileDescriptor(pyFile);
  f = fdopen(fd, "w");
  if (!f)
    return PyErr_SetFromErrno (PyExc_RuntimeError);

  if (!ppdEmit(self->ppd, f, section))
    Py_RETURN_NONE;
  return PyErr_SetFromErrno (PyExc_RuntimeError);
}

/*
 * emit marked options after order dependency by writing to a file object.
 */

static PyObject *
PPD_emitAfterOrder (PPD *self, PyObject *args)
{
  PyObject *pyFile;
  ppd_section_t section;
  FILE *f;
  int limit;
  float min_order;

  if (!PyArg_ParseTuple (args, "Oiif", &pyFile, &section, &limit, &min_order))
    return NULL;

  int fd = PyObject_AsFileDescriptor(pyFile);
  f = fdopen(fd, "w");
  if (!f)
    return PyErr_SetFromErrno (PyExc_RuntimeError);

  if (!ppdEmitAfterOrder(self->ppd, f, section, limit, min_order))
    Py_RETURN_NONE;
  return PyErr_SetFromErrno (PyExc_RuntimeError);
}

/*
 * emit marked options by writing to a file descriptor.
 */

static PyObject *
PPD_emitFd (PPD *self, PyObject *args)
{
  ppd_section_t section;
  int f;

  if (!PyArg_ParseTuple (args, "ii", &f, &section))
    return NULL;

  if (!ppdEmitFd(self->ppd, f, section))
    Py_RETURN_NONE;
  return PyErr_SetFromErrno (PyExc_RuntimeError);
}

/*
 * emit JCL options by writing to a file object.
 */

static PyObject *
PPD_emitJCL (PPD *self, PyObject *args)
{
  PyObject *pyFile;
  int job_id;
  PyObject *userobj;
  PyObject *titleobj;
  char *user;
  char *title;
  FILE *f;

  if (!PyArg_ParseTuple (args, "OiOO", &pyFile, &job_id, &userobj, &titleobj))
    return NULL;

  if (UTF8_from_PyObj (&user, userobj) == NULL)
    return NULL;

  if (UTF8_from_PyObj (&title, titleobj) == NULL) {
    free (user);
    return NULL;
  }

  int fd = PyObject_AsFileDescriptor(pyFile);
  f = fdopen(fd, "w");
  if (!f)
    return PyErr_SetFromErrno (PyExc_RuntimeError);

  if (!ppdEmitJCL(self->ppd, f, job_id, user, title))
    Py_RETURN_NONE;

  free (user);
  free (title);
  return PyErr_SetFromErrno (PyExc_RuntimeError);
}

/*
 * emit JCL end by writing to a file object.
 */

static PyObject *
PPD_emitJCLEnd (PPD *self, PyObject *args)
{
  PyObject *pyFile;
  FILE *f;

  if (!PyArg_ParseTuple (args, "O", &pyFile))
    return NULL;

  int fd = PyObject_AsFileDescriptor(pyFile);
  f = fdopen(fd, "w");
  if (!f)
    return PyErr_SetFromErrno (PyExc_RuntimeError);

  if (!ppdEmitJCLEnd(self->ppd, f))
    Py_RETURN_NONE;
  return PyErr_SetFromErrno (PyExc_RuntimeError);
}

PyObject *
PPD_writeFd (PPD *self, PyObject *args)
{
  char *line = NULL;
  size_t linelen = 0;
  FILE *out;
  int fd;
  int dfd;
  if (!PyArg_ParseTuple (args, "i", &fd))
    return NULL;

  dfd = dup (fd);
  if (dfd == -1)
    return PyErr_SetFromErrno (PyExc_RuntimeError);

  out = fdopen (dfd, "w");
  if (!out)
    return PyErr_SetFromErrno (PyExc_RuntimeError);

  rewind (self->file);
  while (!feof (self->file)) {
    int written = 0;
    ssize_t got = getline (&line, &linelen, self->file);
    if (got == -1)
      break;

    if (!strncmp (line, "*Default", 8)) {
      char *keyword;
      char *start = line + 8;
      char *end;
      ppd_choice_t *choice;
      for (end = start; *end; end++)
	if (isspace (*end) || *end == ':')
	  break;
      keyword = calloc(1, (end - start) + 1);
      strncpy(keyword, start, end - start);
      choice = ppdFindMarkedChoice (self->ppd, keyword);

      // Treat PageRegion, PaperDimension and ImageableArea specially:
      // if not marked, use PageSize option.
      if (!choice && (!strcmp (keyword, "PageRegion") ||
		      !strcmp (keyword, "PaperDimension") ||
		      !strcmp (keyword, "ImageableArea")))
	choice = ppdFindMarkedChoice (self->ppd, "PageSize");

      if (choice) {
	fprintf (out, "*Default%s: %s", keyword, choice->choice);
	if (strchr (end, '\r'))
	  fputs ("\r", out);
	fputs ("\n", out);
	written = 1;
      }
    }

    if (!written)
      fputs (line, out);
  }

  fclose (out);
  if (line)
    free (line);

  Py_RETURN_NONE;
}

/////////
// PPD // ATTRIBUTES
/////////

static PyObject *
PPD_getConstraints (PPD *self, void *closure)
{
  PyObject *ret = PyList_New (0);
  ppd_const_t *c;
  int i;
  for (i = 0, c = self->ppd->consts;
       i < self->ppd->num_consts;
       i++, c++) {
    PyObject *args = Py_BuildValue ("()");
    PyObject *kwlist = Py_BuildValue ("{}");
    Constraint *cns = (Constraint *) PyType_GenericNew (&cups_ConstraintType,
							args, kwlist);
    Py_DECREF (args);
    Py_DECREF (kwlist);
    cns->constraint = c;
    cns->ppd = self;
    Py_INCREF (self);
    PyList_Append (ret, (PyObject *) cns);
  }

  return ret;
}

static PyObject *
PPD_getAttributes (PPD *self, void *closure)
{
  PyObject *ret = PyList_New (0);
  int i;
  for (i = 0; i < self->ppd->num_attrs; i++) {
    PyObject *args = Py_BuildValue ("()");
    PyObject *kwlist = Py_BuildValue ("{}");
    Attribute *as = (Attribute *) PyType_GenericNew (&cups_AttributeType,
						     args, kwlist);
    ppd_attr_t *a = self->ppd->attrs[i];
    Py_DECREF (args);
    Py_DECREF (kwlist);
    as->attribute = a;
    as->ppd = self;
    Py_INCREF (self);
    PyList_Append (ret, (PyObject *) as);
  }

  return ret;
}

static PyObject *
PPD_getOptionGroups (PPD *self, void *closure)
{
  PyObject *ret = PyList_New (0);
  ppd_group_t *group;
  int i;

  for (i = 0, group = self->ppd->groups;
       i < self->ppd->num_groups;
       i++, group++) {
    PyObject *args = Py_BuildValue ("()");
    PyObject *kwlist = Py_BuildValue ("{}");
    Group *grp = (Group *) PyType_GenericNew (&cups_GroupType,
					      args, kwlist);
    Py_DECREF (args);
    Py_DECREF (kwlist);
    grp->group = group;
    grp->ppd = self;
    Py_INCREF (self);
    PyList_Append (ret, (PyObject *) grp);
  }

  return ret;
}

PyGetSetDef PPD_getseters[] =
  {
    { "constraints",
      (getter) PPD_getConstraints, (setter) NULL,
      "List of constraints", NULL },

    { "attributes",
      (getter) PPD_getAttributes, (setter) NULL,
      "List of attributes", NULL },

    { "optionGroups",
      (getter) PPD_getOptionGroups, (setter) NULL,
      "List of PPD option groups" },

    { NULL }, /* Sentinel */
  };

PyMethodDef PPD_methods[] =
  {
    { "localize",
      (PyCFunction) PPD_localize, METH_NOARGS,
      "localize() -> None\n\n"
      "Localize PPD to the current locale." },

    { "localizeIPPReason",
      (PyCFunction) PPD_localizeIPPReason, METH_VARARGS | METH_KEYWORDS,
      "localizeIPPReason(reason, scheme) -> string or None\n\n"
      "Localize IPP reason to the current locale." },

    { "localizeMarkerName",
      (PyCFunction) PPD_localizeMarkerName, METH_VARARGS,
      "localizeMarkerName(name) -> string or None\n\n"
      "Localize marker name to the current locale." },

    { "markDefaults",
      (PyCFunction) PPD_markDefaults, METH_NOARGS,
      "markDefaults() -> None\n\n"
      "Set (mark) all options to their default choices." },

    { "markOption",
      (PyCFunction) PPD_markOption, METH_VARARGS,
      "markOption(option, choice) -> integer\n\n"
      "Set an option to a particular choice.\n\n"
      "@type option: string\n"
      "@param option: option keyword\n"
      "@type choice: string\n"
      "@param choice: option choice\n"
      "@return: number of conflicts" },

    { "conflicts",
      (PyCFunction) PPD_conflicts, METH_NOARGS,
      "conflicts() -> integer\n\n"
      "@return: number of conflicts." },

    { "findOption",
      (PyCFunction) PPD_findOption, METH_VARARGS,
      "findOption(name)\n\n"
      "@type name: string\n"
      "@param name: option keyword\n"
      "@rtype: L{Option} or None\n"
      "@return: named option, or None if not found." },

    { "findAttr",
      (PyCFunction) PPD_findAttr, METH_VARARGS | METH_KEYWORDS,
      "findAttr(name)\n\n"
      "@type name: string\n"
      "@param name: attribute name\n"
      "@type spec: string\n"
      "@keyword spec: specifier string (optional)\n"
      "@rtype: L{Attribute} or None\n"
      "@return: matching attribute, or None if not found." },

    { "findNextAttr",
      (PyCFunction) PPD_findNextAttr, METH_VARARGS | METH_KEYWORDS,
      "findNextAttr(name)\n\n"
      "@type name: string\n"
      "@param name: attribute name\n"
      "@type spec: string\n"
      "@keyword spec: specifier string (optional)\n"
      "@rtype: L{Attribute} or None\n"
      "@return: next matching attribute, or None if not found." },

    { "nondefaultsMarked",
      (PyCFunction) PPD_nondefaultsMarked, METH_NOARGS,
      "nondefaultsMarked() -> boolean\n\n"
      "Returns true if any non-default option choices are marked." },

    { "emitString",
      (PyCFunction) PPD_emitString, METH_VARARGS,
      "emitString(section, min_order) -> string\n\n"
      "Return a string with the marked options for section, with at least min_order order dependency.\n"
      "@type section: integer\n"
      "@param section: section id\n"
      "@type min_order: float\n"
      "@param min_order: minimum order dependency\n"
      "@return: string containing emitted postscript" },

    { "emit",
      (PyCFunction) PPD_emit, METH_VARARGS,
      "emit(file, section) -> None\n\n"
      "Output marked options for section to a file.\n"
      "@type file: file\n"
      "@param file: file object\n"
      "@type section: integer\n"
      "@param section: section id" },

    { "emitAfterOrder",
      (PyCFunction) PPD_emitAfterOrder, METH_VARARGS,
      "emitAfterOrder(file, section, limit, min_order) -> None\n\n"
      "Output marked options for section to a file.\n"
      "@type file: file\n"
      "@param file: file object\n"
      "@type section: integer\n"
      "@param section: section id\n"
      "@type limit: integer\n"
      "@param limit: non-zero to use min_order\n"
      "@type min_order: float\n"
      "@param min_order: minimum order dependency" },

    { "emitFd",
      (PyCFunction) PPD_emitFd, METH_VARARGS,
      "emitFd(fd, section) -> None\n\n"
      "Output marked options for section to a file descriptor.\n"
      "@type fd: integer\n"
      "@param fd: file descriptor\n"
      "@type section: integer\n"
      "@param section: section id" },

    { "emitJCL",
      (PyCFunction) PPD_emitJCL, METH_VARARGS,
      "emitJCL(file, job_id, user, title) -> None\n\n"
      "Emit code for JCL options to a file.\n"
      "@type file: file object\n"
      "@param file: file\n"
      "@type job_id: integer\n"
      "@param job_id: job id\n"
      "@type user: string\n"
      "@param user: user name on job\n"
      "@type title: string\n"
      "@param title: title of job" },

    { "emitJCLEnd",
      (PyCFunction) PPD_emitJCLEnd, METH_VARARGS,
      "emitJCLEnd(file) -> None\n\n"
      "Emit JCLEnd code to a file.\n"
      "@type file: file object\n"
      "@param file: file" },

    { "writeFd",
      (PyCFunction) PPD_writeFd, METH_VARARGS,
      "writeFd(fd) -> None\n\n"
      "Write PPD file, with marked choices as defaults, to file\n"
      "descriptor.\n\n"
      "@type fd: integer\n"
      "@param fd: open file descriptor" },

    { NULL } /* Sentinel */
  };

PyTypeObject cups_PPDType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.PPD",                /*tp_name*/
    sizeof(PPD),               /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)PPD_dealloc,   /*tp_dealloc*/
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
    "PPD file\n"
    "========\n"
    "  A PPD file.\n\n"
    "@type constraints: L{Constraint} list\n"
    "@ivar constraints: list of constraints\n"
    "@type attributes: L{Attribute} list\n"
    "@ivar attributes: list of attributes\n"
    "@type optionGroups: L{Group} list\n"
    "@ivar optionGroups: list of PPD option groups\n"
    "",                        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PPD_methods,               /* tp_methods */
    0,                         /* tp_members */
    PPD_getseters,             /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)PPD_init,        /* tp_init */
    0,                         /* tp_alloc */
    PPD_new,                   /* tp_new */
  };

////////////
// Option //
////////////

static PyObject *
Option_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  Option *self;
  self = (Option *) type->tp_alloc (type, 0);
  return (PyObject *) self;
}

static int
Option_init (Option *self, PyObject *args, PyObject *kwds)
{    
  self->option = NULL;
  return 0;
}

static void
Option_dealloc (Option *self)
{
  Py_XDECREF (self->ppd);
  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

static PyObject *
Option_repr (Option *self)
{
  ppd_option_t *option = self->option;
  if (!option)
    return PyUnicode_FromString ("<cups.Option>");

  char buffer[256];
  snprintf (buffer, 256, "<cups.Option %s=%s>",
			  option->keyword, option->defchoice);
    return PyUnicode_FromString (buffer);
}

////////////
// Option // ATTRIBUTES
////////////

static PyObject *
Option_getConflicted (Option *self, void *closure)
{
  if (!self->option || self->option->conflicted)
    Py_RETURN_TRUE;

  Py_RETURN_FALSE;
}

static PyObject *
Option_getKeyword (Option *self, void *closure)
{
  if (!self->option) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->option->keyword);
}

static PyObject *
Option_getDefchoice (Option *self, void *closure)
{
  if (!self->option) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->option->defchoice);
}

static PyObject *
Option_getText (Option *self, void *closure)
{
  if (!self->option) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->option->text);
}

static PyObject *
Option_getUI (Option *self, void *closure)
{
  if (!self->option) {
    Py_RETURN_NONE;
  }

  return PyLong_FromLong (self->option->ui);
}

static PyObject *
Option_getChoices (Option *self, void *closure)
{
  PyObject *choices = PyList_New (0);
  ppd_choice_t *choice;
  bool defchoice_seen = false;
  int i;

  if (!self->option)
    return choices;

  for (i = 0, choice = self->option->choices;
       i < self->option->num_choices;
       i++, choice++) {
    PyObject *choice_dict = PyDict_New ();
    PyObject *u;

    u = make_PyUnicode_from_ppd_string (self->ppd, choice->choice);
    PyDict_SetItemString (choice_dict, "choice", u);
    Py_DECREF (u);

    u = make_PyUnicode_from_ppd_string (self->ppd, choice->text);
    PyDict_SetItemString (choice_dict, "text", u);
    Py_DECREF (u);

    u = PyBool_FromLong (choice->marked);
    PyDict_SetItemString (choice_dict, "marked", u);
    Py_DECREF (u);

    PyList_Append (choices, choice_dict);
    if (!strcmp (choice->choice, self->option->defchoice))
      defchoice_seen = true;
  }

  if (!defchoice_seen) {
    // This PPD option has a default choice that isn't one of the choices.
    // This really happens.
    const char *defchoice = self->option->defchoice;
    PyObject *choice_dict = PyDict_New ();
    PyObject *u;

    u = make_PyUnicode_from_ppd_string (self->ppd, defchoice);
    PyDict_SetItemString (choice_dict, "choice", u);
    Py_DECREF (u);

    u = make_PyUnicode_from_ppd_string (self->ppd, defchoice);
    PyDict_SetItemString (choice_dict, "text", u);
    Py_DECREF (u);

    PyList_Append (choices, choice_dict);
  }

  return choices;
}

PyGetSetDef Option_getseters[] =
  {
    { "conflicted",
      (getter) Option_getConflicted, (setter) NULL,
      "Whether this option is in conflict", NULL },
  
    { "keyword",
      (getter) Option_getKeyword, (setter) NULL,
      "keyword", NULL },
  
    { "defchoice",
      (getter) Option_getDefchoice, (setter) NULL,
      "defchoice", NULL },
  
    { "text",
      (getter) Option_getText, (setter) NULL,
      "text", NULL },
  
    { "ui",
      (getter) Option_getUI, (setter) NULL,
      "ui", NULL },
  
    { "choices",
      (getter) Option_getChoices, (setter) NULL,
      "choices", NULL },
  
    { NULL }
  };

PyMethodDef Option_methods[] =
  {
    { NULL } /* Sentinel */
  };

PyTypeObject cups_OptionType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.Option",             /*tp_name*/
    sizeof(Option),            /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Option_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    (reprfunc)Option_repr,     /*tp_repr*/
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
    "PPD option\n"
    "==========\n"
    "  A PPD option.\n\n"
    "@type conflicted: boolean\n"
    "@ivar conflicted: whether this option is in conflict\n"
    "@type keyword: string\n"
    "@ivar keyword: the option keyword e.g. Duplex\n"
    "@type defchoice: string\n"
    "@ivar defchoice: the default option choice\n"
    "@type text: string\n"
    "@ivar text: the user-presentable option name e.g. Double-sided printing\n"
    "@type ui: integer\n"
    "@ivar ui: the option type; one of L{PPD_UI_BOOLEAN}, \n"
    "L{PPD_UI_PICKONE}, L{PPD_UI_PICKMANY}\n"
    "@type choices: list\n"
    "@ivar choices: list of the option's choices\n"
    "",                        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    0,                         /* tp_methods */
    0,                         /* tp_members */
    Option_getseters,          /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Option_init,     /* tp_init */
    0,                         /* tp_alloc */
    Option_new,                /* tp_new */
  };

///////////
// Group //
///////////

static PyObject *
Group_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  Group *self;
  self = (Group *) type->tp_alloc (type, 0);
  return (PyObject *) self;
}

static int
Group_init (Group *self, PyObject *args, PyObject *kwds)
{    
  self->group = NULL;
  return 0;
}

static void
Group_dealloc (Group *self)
{
  Py_XDECREF (self->ppd);
  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

static PyObject *
Group_repr (Group *self)
{
  ppd_group_t *group = self->group;
  if (!group)
    return PyUnicode_FromString ("<cups.Group>");

  char buffer[256];
  snprintf (buffer, 256, "<cups.Group %s>", group->name);
    return PyUnicode_FromString (buffer);
}

///////////
// Group // ATTRIBUTES
///////////

static PyObject *
Group_getText (Group *self, void *closure)
{
  if (!self->group) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->group->text);
}

static PyObject *
Group_getName (Group *self, void *closure)
{
  if (!self->group) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->group->name);
}

static PyObject *
Group_getOptions (Group *self, void *closure)
{
  PyObject *options = PyList_New (0);
  ppd_option_t *option;
  int i;

  if (!self->group)
    return options;

  for (i = 0, option = self->group->options;
       i < self->group->num_options;
       i++, option++) {
    PyObject *args = Py_BuildValue ("()");
    PyObject *kwlist = Py_BuildValue ("{}");
    Option *opt = (Option *) PyType_GenericNew (&cups_OptionType,
						args, kwlist);
    Py_DECREF (args);
    Py_DECREF (kwlist);
    opt->option = option;
    opt->ppd = self->ppd;
    Py_INCREF (self->ppd);
    PyList_Append (options, (PyObject *) opt);
  }

  return options;
}

static PyObject *
Group_getSubgroups (Group *self, void *closure)
{
  PyObject *subgroups = PyList_New (0);
  ppd_group_t *subgroup;
  int i;

  if (!self->group)
    return subgroups;

  for (i = 0, subgroup = self->group->subgroups;
       i < self->group->num_subgroups;
       i++, subgroup++) {
    PyObject *args = Py_BuildValue ("()");
    PyObject *kwlist = Py_BuildValue ("{}");
    Group *grp = (Group *) PyType_GenericNew (&cups_GroupType,
					      args, kwlist);
    Py_DECREF (args);
    Py_DECREF (kwlist);
    grp->group = subgroup;
    grp->ppd = self->ppd;
    Py_INCREF (self->ppd);
    PyList_Append (subgroups, (PyObject *) grp);
  }

  return subgroups;
}

PyGetSetDef Group_getseters[] =
  {
    { "text",
      (getter) Group_getText, (setter) NULL,
      "text", NULL },
  
    { "name",
      (getter) Group_getName, (setter) NULL,
      "name", NULL },
  
    { "options",
      (getter) Group_getOptions, (setter) NULL,
      "options", NULL },
  
    { "subgroups",
      (getter) Group_getSubgroups, (setter) NULL,
      "subgroups", NULL },
  
    { NULL }
  };

PyMethodDef Group_methods[] =
  {
    { NULL } /* Sentinel */
  };

PyTypeObject cups_GroupType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.Group",              /*tp_name*/
    sizeof(Group),             /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Group_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    (reprfunc)Group_repr,      /*tp_repr*/
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
    "PPD option group\n"
    "================\n\n"
    "  A PPD option group.\n\n"
    "@type text: string\n"
    "@ivar text: user-presentable group name\n"
    "@type name: string\n"
    "@ivar name: unique group name\n"
    "@type options: L{Option} list\n"
    "@ivar options: list of options in the group\n"
    "@type subgroups: L{Group} list\n"
    "@ivar subgroups: list of subgroups in the group\n"
    "",                        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    0,                         /* tp_methods */
    0,                         /* tp_members */
    Group_getseters,           /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Group_init,      /* tp_init */
    0,                         /* tp_alloc */
    Group_new,                 /* tp_new */
  };

////////////////
// Constraint //
////////////////

static PyObject *
Constraint_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  Constraint *self;
  self = (Constraint *) type->tp_alloc (type, 0);
  return (PyObject *) self;
}

static int
Constraint_init (Constraint *self, PyObject *args, PyObject *kwds)
{    
  self->constraint = NULL;
  return 0;
}

static void
Constraint_dealloc (Constraint *self)
{
  Py_XDECREF (self->ppd);
  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

////////////////
// Constraint // ATTRIBUTES
////////////////

static PyObject *
Constraint_getOption1 (Constraint *self, void *closure)
{
  if (!self->constraint) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->constraint->option1);
}

static PyObject *
Constraint_getChoice1 (Constraint *self, void *closure)
{
  if (!self->constraint) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->constraint->choice1);
}

static PyObject *
Constraint_getOption2 (Constraint *self, void *closure)
{
  if (!self->constraint) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->constraint->option2);
}

static PyObject *
Constraint_getChoice2 (Constraint *self, void *closure)
{
  if (!self->constraint) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->constraint->choice2);
}

PyGetSetDef Constraint_getseters[] =
  {
    { "option1",
      (getter) Constraint_getOption1, (setter) NULL,
      "option1", NULL },
  
    { "choice1",
      (getter) Constraint_getChoice1, (setter) NULL,
      "choice1", NULL },
  
    { "option2",
      (getter) Constraint_getOption2, (setter) NULL,
      "option2", NULL },
  
    { "choice2",
      (getter) Constraint_getChoice2, (setter) NULL,
      "choice2", NULL },
  
    { NULL }
  };

PyTypeObject cups_ConstraintType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.Constraint",         /*tp_name*/
    sizeof(Constraint),        /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Constraint_dealloc, /*tp_dealloc*/
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
    "PPD constraint\n"
    "==============\n\n"
    "  A PPD constraint.\n\n"
    "@type option1: string\n"
    "@ivar option1: first option keyword\n"
    "@type choice1: string\n"
    "@ivar choice1: first option choice\n"
    "@type option2: string\n"
    "@ivar option2: second option keyword\n"
    "@type choice2: string\n"
    "@ivar choice2: secondoption choice\n"
    "",                        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    0,                         /* tp_methods */
    0,                         /* tp_members */
    Constraint_getseters,      /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Constraint_init, /* tp_init */
    0,                         /* tp_alloc */
    Constraint_new,            /* tp_new */
  };

///////////////
// Attribute //
///////////////

static PyObject *
Attribute_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  Attribute *self;
  self = (Attribute *) type->tp_alloc (type, 0);
  return (PyObject *) self;
}

static int
Attribute_init (Attribute *self, PyObject *args, PyObject *kwds)
{    
  self->attribute = NULL;
  return 0;
}

static void
Attribute_dealloc (Attribute *self)
{
  Py_XDECREF (self->ppd);
  ((PyObject *)self)->ob_type->tp_free ((PyObject *) self);
}

static PyObject *
Attribute_repr (Attribute *self)
{
  ppd_attr_t *attribute = self->attribute;
  if (!attribute)
    return PyUnicode_FromString ("<cups.Attribute>");

  char buffer[256];
  snprintf (buffer, 256, "<cups.Attribute *%s%s%s>",
			  attribute->name,
			  attribute->spec[0] != '\0' ? " ": "",
			  attribute->spec);
    return PyUnicode_FromString (buffer);
}

///////////////
// Attribute // ATTRIBUTES
///////////////

static PyObject *
Attribute_getName (Attribute *self, void *closure)
{
  if (!self->attribute) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->attribute->name);
}

static PyObject *
Attribute_getSpec (Attribute *self, void *closure)
{
  if (!self->attribute) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->attribute->spec);
}

static PyObject *
Attribute_getText (Attribute *self, void *closure)
{
  if (!self->attribute) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->attribute->text);
}

static PyObject *
Attribute_getValue (Attribute *self, void *closure)
{
  if (!self->attribute) {
    Py_RETURN_NONE;
  }

  return make_PyUnicode_from_ppd_string (self->ppd, self->attribute->value);
}

PyGetSetDef Attribute_getseters[] =
  {
    { "name",
      (getter) Attribute_getName, (setter) NULL,
      "name", NULL },
  
    { "spec",
      (getter) Attribute_getSpec, (setter) NULL,
      "spec", NULL },
  
    { "text",
      (getter) Attribute_getText, (setter) NULL,
      "text", NULL },
  
    { "value",
      (getter) Attribute_getValue, (setter) NULL,
      "value", NULL },
  
    { NULL }
  };

PyTypeObject cups_AttributeType =
  {
    PyVarObject_HEAD_INIT(NULL, 0)
    "cups.Attribute",          /*tp_name*/
    sizeof(Attribute),         /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Attribute_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    (reprfunc)Attribute_repr,  /*tp_repr*/
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
    "PPD attribute\n"
    "=============\n\n"
    "  A PPD attribute.\n\n"
    "@type name: string\n"
    "@ivar name: attribute name\n"
    "@type spec: string\n"
    "@ivar spec: specifier string (if any)\n"
    "@type text: string\n"
    "@ivar text: human-readable text (if any)\n"
    "@type value: string\n"
    "@ivar value: attribute value\n"
    "",                        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    0,                         /* tp_methods */
    0,                         /* tp_members */
    Attribute_getseters,      /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Attribute_init, /* tp_init */
    0,                         /* tp_alloc */
    Attribute_new,            /* tp_new */
  };
