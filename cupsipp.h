/*
 * cups - Python bindings for CUPS
 * Copyright (C) 2002-2020  Red Hat, Inc
 * Author: Tim Waugh <twaugh@redhat.com>
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

#ifndef HAVE_CUPSIPP_H
#define HAVE_CUPSIPP_H

#include <Python.h>
#include <cups/ipp.h>

extern PyMethodDef IPPRequest_methods[];
extern PyTypeObject cups_IPPRequestType;

extern PyMethodDef IPPAttribute_methods[];
extern PyTypeObject cups_IPPAttributeType;

typedef struct
{
  PyObject_HEAD

  ipp_t *ipp;
} IPPRequest;

typedef struct
{
  PyObject_HEAD

  ipp_tag_t group_tag;
  ipp_tag_t value_tag;
  char *name;

  /* Python List of values */
  PyObject *values;
} IPPAttribute;

#endif /* HAVE_CUPSIPP_H */
