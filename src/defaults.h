/*  defauls.h: default values of configuration options and constants.
    Copyright (C) 2006, 2007, 2008, 2009, 2014 Werner Baumann

    This file is part of davfs2.

    davfs2 is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    davfs2 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with davfs2; if not, write to the Free Software Foundation,
    Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA. */


#ifndef DAV_DEFAULTS_H
#define DAV_DEFAULTS_H


/* Misc. */
/*=======*/

#ifdef __FreeBSD__
#define MS_NOSUID  MNT_NOSUID
#define MS_NOEXEC  MNT_NOEXEC
#define MS_RDONLY  MNT_RDONLY
#define MS_MGC_VAL 0
#define MS_NODEV   0
#endif

/* File system type to be used with 'mount -t' and fstab. */
#define DAV_FS_TYPE "davfs"

/* Mount options set by mount program in case of mounting by an
   ordinary user. */
#define DAV_USER_MOPTS (MS_MGC_VAL | MS_NOSUID | MS_NOEXEC | MS_NODEV)

/* This mount options will allways be set by davfs2. Different values from
   command line and even fstab will be silently ignored. */
#define DAV_MOPTS (MS_MGC_VAL | MS_NOSUID | MS_NODEV)

/* Mode of directories.
   May be overridden by command line or fstab. */
#define DAV_DIR_MODE (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)

/* Mode of regular files.
   May be overridden by command line or fstab. */
#define DAV_FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

/* XML namespace for the cache index file. */
#define DAV_XML_NS "http://dav.sf.net/"


/* Directories and Files */
/*=======================*/

/* If _PATH_MOUNTED (the mtab file) is a symbolic link (to /proc/mounts)
   some information required for umount is missing (e.g. the option
   user=<name of the mounting user|) and in the case of davfs2 the file
   system type will not be davfs but that of the kernel file system
   (fuse or coda). Newer versions of the mount program will store this
   information in the utab-file /var/run/mount/utab or /run/mount/utab.
   davfs2 will do the same. */

/* The subdirectory of DAV_LOCALSTATE_DIR where the utab-file is placed. */
#define DAV_UTAB_DIR "mount"

/* The name of the utab-file. */
#define DAV_UTAB "utab"

/* The device directory. */
#define DAV_DEV_DIR "/dev"

/* The file davfs reads mtab entries from. If not available it will
   use _PATH_MOUNTED. */
#define DAV_MOUNTS "/proc/mounts"

/* The directory where the cache files will be stored, when mounted by
   a non root users; relative to DAV_USER_DIR.
   May be overridden by user config file. */
#define DAV_CACHE "cache"

/* The name of index files. */
#define DAV_INDEX "index"

/* Name of the directory within the davfs2 filesystem that holds local
   backup files.
   May be overridden by system config file and user config file. */
#define DAV_BACKUP_DIR "lost+found"

/* Buffer size for reading the XML index files of persistent cache. */
#define DAV_XML_BUF_SIZE 16 * 1024


/* Cache Optimization */
/*====================*/

/* Cache size in MiByte.
   May be overridden by system config file and user config file.
   (1 MiByte = 1,048,576 Byte; Mi = Mebi = Mega Binary according to IEC) */
#define DAV_CACHE_SIZE 50

/* Size of the hash table to store nodes. Should be a power of 2.
   May be overridden by system config file and user config file. */
#define DAV_TABLE_SIZE 1024

/* How long in seconds a cached directory is valid. After this time
   a new PROPFIND request for this directory must be performed.
   May be overridden by system config file and user config file. */
#define DAV_DIR_REFRESH 60

/* Wait at least that many seconds from last file access until a new
   GET If-Modified request is send to the server. If set to 0 a request
   will be send every time the file is opened. But some applications do
   open and close calls in short sequence that cause - mostly - unnecessary
   traffic.
   May be overridden by system config file and user config file. */
#define DAV_FILE_REFRESH 1

/* How long to delay uploading of locally changed files after closing. 
   May be overridden by system config file and user config file. */
#define DAV_DELAY_UPLOAD 10

/* Use PROPFIND to get the Last-Modified time of all files in a directory
   instead of GET If-Modified_Since for single files.
   May be overridden by system config file and user config file. */
#define DAV_GUI_OPTIMIZE 0

/* Remove nodes that are currently not needed to minimize memory usage. */
#define DAV_MINIMIZE_MEM 0


/* HTTP */
/*======*/

/* The default proxy port.
   May be overridden by system config file, user config file or environment
   variable. */
#define DAV_DEFAULT_PROXY_PORT 8080

/* The default proxy type.
   May be overridden by system config file, user config file or environment
   variable. */
#define DAV_DEFAULT_PROXY_TYPE dav_proxy_type_http

/* Whether to use a proxy if one is specified.
   May be overridden by command line or fstab. */
#define DAV_USE_PROXY 1

/* Whether to ask user for credentials if not given.
   May be overridden by command line, fstab or system config file. */
#define DAV_ASKAUTH 1

/* Whether to use locks.
   May be overridden by command line or fstab. */
#define DAV_LOCKS 1

/* Send expect 100-continue header in PUT requests.
   May be overridden by system config file and user config file. */
#define DAV_EXPECT100 0

/* If If-Match and If-None-Match does not work on the server, set to 1.
   Default is 1, as Apache has this bug.
   May be overridden by system config file and user config file. */
#define DAV_IF_MATCH_BUG 0

/* Some servers sends a weak invalid etag that turns into a valid strong etag
   after one second. With this flag set, the etag will not be used,
   otherwise the weakness indicator will be removed and the etag be trated
   as if it was strong.
   May be overridden by system config file and user config file. */
#define DAV_DROP_WEAK_ETAGS 0

/* How many cookies to store and include in requests.
   May be overridden by system config file and user config file. */
#define DAV_N_COOKIES 0

/* Check on server whether a file exists or has been modified before
   locking a new file or changing an existant one.
   May be overridden by system config file and user config file. */
#define DAV_PRECHECK 1

/* Ignore the information in the DAV-header, if any, because it
   may be a lie.
   May be overridden by system config file and user config file. */
#define DAV_IGNORE_DAV_HEADER 0

/* Use "Content-Encoding: gzip" for GET requests. */
#define DAV_USE_COMPRESSION 0

/* Only request a minimal set or properties (getcontentlength and
   resourcetype). For read-only filesystems to speed up PROPFIND
   requests.
   May be overridden by system config file and user config file. */
#define DAV_MIN_PROPSET 0

/* Follow redirect responses on GET requests. */
#define DAV_FOLLOW_REDIRECT 0

/* Use Neon work around for SharePoint href-elements with characters
   that are not allowed in href-elements. */
#define DAV_SHAREPOINT_HREF_BUG 0

/* Timeout in seconds used when libneon supports non blocking io
   A value of zero means use the TCP default
   May be overriden by system config file and user config file. */
#define DAV_CONNECT_TIMEOUT 10

/* Timeout in seconds used when reading from a socket.
   May be overridden by system config file and user config file. */
#define DAV_READ_TIMEOUT 30

/* Default retry time after a HTTP request failed. When the request fails
   again, the retry time will subsequently be increased up to DAV_MAX_RETRY.
   May be overridden by system config file and user config file. */
#define DAV_RETRY 30

/* Maximum retry time after a HTTP request failed.
   May be overridden by system config file and user config file. */
#define DAV_MAX_RETRY 300

/* Maximum Number of attempts made to upoad a changed file before it is
   moved into the lost+found directory.
   May be overridden by system config file and user config file. */
#define DAV_MAX_UPLOAD_ATTEMPTS 15

/* Preferred live time of locks in seconds, before they have to be refreshed.
   May be overridden by system config file and user config file. */
#define DAV_LOCK_TIMEOUT 1800

/* How many seconds before a lock expires it should be refreshed.
   May be overridden by system config file and user config file. */
#define DAV_LOCK_REFRESH 60


/* Debug Constants */
/*=================*/

#define DAV_DBG_CONFIG 0x1
#define DAV_DBG_KERNEL 0x2
#define DAV_DBG_CACHE 0x4
#define DAV_DBG_SECRETS 0x8


#endif /* DAV_DEFAULTS_H */
