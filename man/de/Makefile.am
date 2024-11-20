## Makefile for man/de directory in davfs.
## Copyright (C) 2006, 2007, 2008, 2009 Werner Baumann
##
## This file is part of davfs2.
##
## davfs2 is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 3 of the License, or
## (at your option) any later version.
##
## davfs2 is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with davfs2; if not, write to the Free Software Foundation,
## Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA. */

## Process this file with automake to produce Makefile.in

pkgsysconfdir = $(sysconfdir)/@PACKAGE@
pkglocalstatedir = $(dav_localstatedir)/mount.davfs
pkgsyscachedir = $(dav_syscachedir)/@PACKAGE@
manual5dir = $(mandir)/de/man5
manual8dir = $(mandir)/de/man8

edit = sed \
       -e 's|@PACKAGE[@]|$(PACKAGE)|g' \
       -e 's|@PACKAGE_STRING[@]|$(PACKAGE_STRING)|g' \
       -e 's|@PACKAGE_BUGREPORT[@]|$(PACKAGE_BUGREPORT)|g' \
       -e 's|@PROGRAM_NAME[@]|mount.davfs|g' \
       -e 's|@SYS_CONF_DIR[@]|$(pkgsysconfdir)|g' \
       -e 's|@SYS_RUN[@]|$(pkglocalstatedir)|g' \
       -e 's|@SYS_CACHE_DIR[@]|$(pkgsyscachedir)|g' \
       -e 's|@SECRETSFILE[@]|secrets|g' \
       -e 's|@CONFIGFILE[@]|$(PACKAGE).conf|g' \
       -e 's|@CERTS_DIR[@]|certs|g' \
       -e 's|@CLICERTS_DIR[@]|private|g' \
       -e 's|@USER[@]|$(dav_user)|g' \
       -e 's|@GROUP[@]|$(dav_group)|g'

manual5_DATA = davfs2.conf.5
manual8_DATA = mount.davfs.8 umount.davfs.8

EXTRA_DIST = davfs2.conf.5 mount.davfs.8 umount.davfs.8 \
             davfs2.conf.5.po mount.davfs.8.po umount.davfs.8.po \
             davfs2.conf.5.translator mount.davfs.8.translator \
             umount.davfs.8.translator
CLEANFILES = $(manual5_DATA) $(manual8_DATA)
