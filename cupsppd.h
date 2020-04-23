/*
 * cups - Python bindings for CUPS
 * Copyright (C) 2002-2020  Tim Waugh <twaugh@redhat.com>
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

#ifndef HAVE_CUPSPPD_H
#define HAVE_CUPSPPD_H

#include <Python.h>
#include <cups/ppd.h>
#include <iconv.h>

extern PyMethodDef PPD_methods[];
extern PyTypeObject cups_PPDType;
extern PyTypeObject cups_OptionType;
extern PyTypeObject cups_GroupType;
extern PyTypeObject cups_ConstraintType;
extern PyTypeObject cups_AttributeType;

typedef struct
{
  PyObject_HEAD
  ppd_file_t *ppd;
  FILE *file;
  iconv_t *conv_from;
  iconv_t *conv_to;
} PPD;

extern PyObject *PPD_writeFd (PPD *self, PyObject *args);

#endif /* HAVE_CUPSPPD_H */
