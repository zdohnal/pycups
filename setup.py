#!/usr/bin/python

## Copyright (C) 2002-2020  Red Hat, Inc
## Authors: Tim Waugh <twaugh@redhat.com>
##          Zdenek Dohnal <zdohnal@redhat.com>

## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.

## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.

## You should have received a copy of the GNU General Public License
## along with this program; if not, write to the Free Software
## Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

"""This is a set of Python bindings for the libcups library from the
CUPS project.

>>> # Example of getting a list of printers
>>> import cups
>>> conn = cups.Connection ()
>>> printers = conn.getPrinters ()
>>> for printer in printers:
...     print printer, printers[printer]["device-uri"]
...
HP ipp://192.168.1.1:631/printers/HP
duplex ipp://192.168.1.1:631/printers/duplex
HP-LaserJet-6MP ipp://192.168.1.1:631/printers/HP-LaserJet-6MP
EPSON-Stylus-D78 usb://EPSON/Stylus%20D78
"""

from distutils.core import setup, Extension
import sys
VERSION="2.0.0"
libraries=["cups"]

if sys.platform == "darwin" or sys.platform.startswith("freebsd"):
	libraries.append ("iconv")

setup (name="pycups",
       version=VERSION,
       description="Python bindings for libcups",
       long_description=__doc__,
       maintainer="Zdenek Dohnal",
       maintainer_email="zdohnal@redhat.com",
       url="https://github.com/zdohnal/pycups",
       download_url="https://github.com/zdohnal/pycups/releases",
       classifiers=[
		"Intended Audience :: Developers",
		"Topic :: Software Development :: Libraries :: Python Modules",
		"License :: OSI Approved :: GNU General Public License (GPL)",
		"Development Status :: 5 - Production/Stable",
		"Operating System :: Unix",
		"Programming Language :: C",
		"Programming Language :: Python",
		"Programming Language :: Python :: 3",
		],
       license="GPLv2+",
       ext_modules=[Extension("cups",
                              ["cupsmodule.c", "cupsconnection.c",
                               "cupsppd.c", "cupsipp.c"],
                              libraries=libraries,
                              define_macros=[("VERSION", '"%s"' % VERSION)])])
