## Makefile for program src directory in davfs.
## Copyright (C) 2006, 2007, 2008, 2009, 2020 Werner Baumann
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

localedir = $(datadir)/locale
pkgsysconfdir = $(sysconfdir)/@PACKAGE@
pkglocalstatedir = $(dav_localstatedir)/mount.davfs
pkgsyscachedir = $(dav_syscachedir)/@PACKAGE@

sbin_PROGRAMS = mount.davfs umount.davfs
mount_davfs_SOURCES = cache.c \
        dav_fuse.c kernel_interface.c mount_davfs.c webdav.c \
        cache.h defaults.h fuse_kernel.h util.h \
        kernel_interface.h mount_davfs.h webdav.h
umount_davfs_SOURCES = umount_davfs.c defaults.h

AM_CFLAGS = -Wall -Werror=format-security \
        -fstack-protector-strong --param=ssp-buffer-size=4
DEFS = -DPROGRAM_NAME=\"mount.davfs\" \
       -DDAV_SYS_CONF_DIR=\"$(pkgsysconfdir)\" \
       -DDAV_LOCALSTATE_DIR=\"$(dav_localstatedir)\" \
       -DDAV_SYS_RUN=\"$(pkglocalstatedir)\" \
       -DDAV_SYS_CACHE=\"$(pkgsyscachedir)\" \
       -DDAV_SECRETS=\"secrets\" \
       -DDAV_CONFIG=\"$(PACKAGE).conf\" \
       -DDAV_CERTS_DIR=\"certs\" \
       -DDAV_CLICERTS_DIR=\"private\" \
       -DDAV_DATA_DIR=\"$(pkgdatadir)\" \
       -DLOCALEDIR=\"$(localedir)\" \
       -DDAV_USER=\"$(dav_user)\" \
       -DDAV_GROUP=\"$(dav_group)\" \
       -D_FORTIFY_SOURCE=2 @DEFS@
LIBS = $(NEON_LIBS) @LIBS@ $(INTLLIBS)

if HOST_TYPE_FREEBSD
LIBS += @LIBICONV@
endif

install-exec-hook:
	chmod u+s $(DESTDIR)$(sbindir)/mount.davfs;
