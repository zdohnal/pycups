/*
 * cups - Python bindings for CUPS
 * Copyright (C) 2006-2020  Tim Waugh <twaugh@redhat.com>
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

#ifndef HAVE_CUPSMODULE_H
#define HAVE_CUPSMODULE_H

#include <cups/cups.h>
#include <cups/language.h>
#include <cups/adminutil.h>

/* GCC attributes */
#if !defined(__GNUC__) || __GNUC__ < 2 || \
    (__GNUC__ == 2 && __GNUC_MINOR__ < 5) || __STRICT_ANSI__
# define FORMAT(x)
#else /* GNU C: */
# define FORMAT(x) __attribute__ ((__format__ x))
#endif

extern void debugprintf (const char *fmt, ...) FORMAT ((__printf__, 1, 2));

struct TLS
{
    PyObject *cups_password_callback;
    PyObject *cups_password_callback_context;
};

extern struct TLS *get_TLS (void);

typedef struct
{
  PyObject *cb;
  PyObject *user_data;
} CallbackContext;

extern const char * UTF8_from_PyObj (char **const utf8, PyObject *obj);

#endif /* HAVE_CUPSMODULE_H */
