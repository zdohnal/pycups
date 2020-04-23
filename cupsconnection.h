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

#ifndef HAVE_CUPSCONNECTION_H
#define HAVE_CUPSCONNECTION_H

#include <Python.h>
#include <cups/http.h>

#include "cupsmodule.h"

extern PyMethodDef Connection_methods[];
extern PyTypeObject cups_ConnectionType;
extern PyTypeObject cups_DestType;

typedef struct
{
  PyObject_HEAD
  http_t *http;
  char *host; /* for repr */
  char *cb_password;
  PyThreadState *tstate;
} Connection;

typedef struct
{
  PyObject_HEAD
  int is_default;
  char *destname;
  char *instance;

  // Options
  int num_options;
  char **name;
  char **value;
} Dest;

extern PyObject *HTTPError;
extern PyObject *IPPError;

void Connection_begin_allow_threads (void *connection);
void Connection_end_allow_threads (void *connection);

const char *password_callback_newstyle (const char *prompt,
					http_t *http,
					const char *method,
					const char *resource,
					void *user_data);
const char *password_callback_oldstyle (const char *prompt,
					http_t *http,
					const char *method,
					const char *resource,
					void *user_data);
int cups_dest_cb (void *user_data,
		  unsigned flags,
		  cups_dest_t *dest);

void set_ipp_error (ipp_status_t status,
		    const char *message);
#endif /* HAVE_CUPSCONNECTION_H */
