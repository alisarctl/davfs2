/*  dav_fuse.c: interface to the fuse kernel module FUSE_KERNEL_VERSION 7.
    Copyright (C) 2006, 2007, 2008. 2009, 2014, 2020 Werner Baumann
    Copyright (C) 2022  Ali Abdallah <ali.abdallah@suse.com>

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


#include "config.h"

#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_LIBINTL_H
#include <libintl.h>
#endif
#ifdef HAVE_STDDEF_H
#include <stddef.h>
#endif
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#include "defaults.h"
#include "mount_davfs.h"
#include "cache.h"
#include "kernel_interface.h"
#include "fuse_kernel.h"

#ifdef ENABLE_NLS
#define _(String) gettext(String)
#else
#define _(String) String
#endif


/* Data Types */
/*============*/

/* There is no struct fuse_create_out in fuse_kernel.h. */

struct create_out {
    struct fuse_entry_out entry;
    struct fuse_open_out open;
};


/* Private global variables */
/*==========================*/

/* Buffer used for communication with the kernel module (in and out). */
static size_t buf_size;
static char *buf;

/* fuse wants the nodeid of the root node to be 1, so we have to translate
   between the real nodeid and what fuse wants. */
static uint64_t root;

/* Send debug messages to syslog if != 0. */
static int debug;


/* Private function prototypes */
/*=============================*/

/* Functions to handle upcalls fromthe kernel module. */

static uint32_t
fuse_access(void);

static uint32_t
fuse_create(void);

static uint32_t
fuse_getattr(void);

static uint32_t
fuse_init(void);

static uint32_t
fuse_lookup(void);

static uint32_t
fuse_mkdir(void);

static uint32_t
fuse_mknod(void);

static uint32_t
fuse_open(void);

static uint32_t
fuse_read(void);

static uint32_t
fuse_release(void);

static uint32_t
fuse_rename(void);

static uint32_t
fuse_setattr(void);

static uint32_t
fuse_stat(void);

static uint32_t
fuse_write(void);

/* Auxiliary functions. */

static off_t
write_dir_entry(int fd, off_t off, const dav_node *node, const char *name);

static void
set_attr(struct fuse_attr *attr, const dav_node *node);


/* Public functions */
/*==================*/

void
dav_fuse_loop(int device, char *mpoint, size_t bufsize, time_t idle_time,
              dav_is_mounted_fn is_mounted, volatile int *keep_on_running,
              int dbg)
{
    debug = dbg;
    char *mountpoint = mpoint;
    int unmounting = 0;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "fuse kernel version 7");

    buf_size = bufsize;
    buf = malloc(buf_size);
    if (!buf) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("can't allocate message buffer"));
        return;
    }

    dav_register_kernel_interface(&write_dir_entry, NULL);

    struct timeval tv;
    tv.tv_sec = idle_time;
    tv.tv_usec = 0;
    time_t last_tidy_cache = time(NULL);

    while (1) {

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(device, &fds);
        int ret = select(device + 1, &fds, NULL, NULL, &tv);
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "SELECT: %i", ret);

        if (!*keep_on_running && !unmounting) {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("unmounting %s"),
                   mountpoint);
            unmounting = 1;
            pid_t pid = fork();
            if (pid == 0) {
#if defined(__linux__)
                execl("/bin/umount", "umount", "-il", mountpoint, NULL);
#elif defined(__FreeBSD__)
                execl("/sbin/umount", "umount", "-v", mountpoint, NULL);
#endif
                _exit(EXIT_FAILURE);
            }
        }

        if (ret > 0) {
            ssize_t bytes_read = read(device, buf, buf_size);
            if (bytes_read <= 0) {
                if (debug)
                    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "READ: %s",
                           strerror(errno));
                if (bytes_read == 0 || errno == EINTR || errno == EAGAIN ||
                        errno == ENOENT) {
                    if (time(NULL) < (last_tidy_cache + idle_time)) {
                        tv.tv_sec = last_tidy_cache + idle_time - time(NULL);
                    } else {
                        tv.tv_sec = 0;
                    }
                    continue;
                }
                break;
            }
        } else if (ret == 0) {
            if (dav_tidy_cache() == 0) {
                tv.tv_sec = idle_time;
                last_tidy_cache = time(NULL);
            } else {
                tv.tv_sec = 0;
            }
            continue;
        } else {
            if (errno == EINTR)
                continue;
            break;
        }

        struct fuse_in_header *ih = (struct fuse_in_header *) buf;
        struct fuse_out_header *oh = (struct fuse_out_header *) buf;
        if (ih->nodeid == 1)
              ih->nodeid = root;

        switch (ih->opcode) {
        case FUSE_LOOKUP:
            oh->len = fuse_lookup();
            break;
        case FUSE_FORGET:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                       "FUSE_FORGET: no reply");
            oh->error = 0;
            oh->len = 0;
            break;
        case FUSE_GETATTR:
            oh->len = fuse_getattr();
            break;
        case FUSE_SETATTR:
            oh->len = fuse_setattr();
            break;
        case FUSE_READLINK:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_READLINK:");
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_SYMLINK:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_SYMLINK:");
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_MKNOD:
            oh->len = fuse_mknod();
            break;
        case FUSE_MKDIR:
            oh->len = fuse_mkdir();
            break;
        case FUSE_UNLINK:
            if (debug) {
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_UNLINK:");
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  p 0x%llx, %s",
                       (unsigned long long) ih->nodeid,
                       (char *) (buf + sizeof(struct fuse_in_header)));
            }
            oh->error = dav_remove((dav_node *) ((size_t) ih->nodeid),
                             (char *) (buf + sizeof(struct fuse_in_header)),
                             ih->uid);
            if (oh->error)
                oh->error *= -1;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_RMDIR:
            if (debug) {
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_RMDIR:");
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  p 0x%llx, %s",
                       (unsigned long long) ih->nodeid,
                       (char *) (buf + sizeof(struct fuse_in_header)));
            }
            oh->error = dav_rmdir((dav_node *) ((size_t) ih->nodeid),
                            (char *) (buf + sizeof(struct fuse_in_header)),
                            ih->uid);
            if (oh->error)
                oh->error *= -1;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_RENAME:
            oh->len = fuse_rename();
            break;
        case FUSE_LINK:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_LINK:");
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_OPEN:
            oh->len = fuse_open();
            break;
        case FUSE_READ:
            oh->len = fuse_read();
            break;
        case FUSE_WRITE:
            oh->len = fuse_write();
            break;
        case FUSE_STATFS:
            oh->len = fuse_stat();
            break;
        case FUSE_RELEASE:
            oh->len = fuse_release();
            last_tidy_cache = 0;
            break;
        case FUSE_FSYNC:
            if (debug) {
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_FSYNC:");
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx",
                (unsigned long long) ih->nodeid);
            }
            oh->error = dav_sync((dav_node *) ((size_t) ih->nodeid));
            if (oh->error)
                oh->error *= -1;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_SETXATTR:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_SETXATTR:");
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_GETXATTR:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_GETXATTR:");
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_LISTXATTR:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_LISTXATTR:");
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_REMOVEXATTR:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_REMOVEXATTR:");
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_FLUSH:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                       "FUSE_FLUSH: ignored");
            oh->error = 0;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_INIT:
            oh->len = fuse_init();
            break;
        case FUSE_OPENDIR:
            oh->len = fuse_open();
            break;
        case FUSE_READDIR:
            oh->len = fuse_read();
            break;
        case FUSE_RELEASEDIR:
            oh->len = fuse_release();
            break;
        case FUSE_FSYNCDIR:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_FSYNCDIR:");
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        case FUSE_ACCESS:
            oh->len = fuse_access();
            break;
        case FUSE_CREATE:
            oh->len = fuse_create();
            break;
        default:
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                       "UNKNOWN FUSE CALL %i", ih->opcode);
            oh->error = -ENOSYS;
            oh->len = sizeof(struct fuse_out_header);
            break;
        }

        if (debug && oh->len)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "RET: %s",
                   strerror(-oh->error));

        ssize_t n = 0;
        ssize_t w = 0;
        while (n < oh->len && w >= 0) {
            w = write(device, buf + n, oh->len - n);
            n += w;
        }

        if (time(NULL) < (last_tidy_cache + idle_time)) {
            tv.tv_sec = last_tidy_cache + idle_time - time(NULL);
        } else {
            dav_tidy_cache();
            tv.tv_sec = idle_time;
            last_tidy_cache = time(NULL);
        }

    }
}


/* Private functions */
/*===================*/

/* Functions to handle upcalls fromthe kernel module.
   The cache module only uses data types from the C-library. For file access,
   mode and the like it only uses symbolic constants defined in the C-library.
   So the main porpose of this functions is to translate from kernel specific
   types and constants to types and constants from the C-library, and back.
   All of this functions return the amount of data in buf that is to be
   send to the kernel module. */

static uint32_t
fuse_access(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    struct fuse_access_in *in = (struct fuse_access_in *)
                                (buf + sizeof(struct fuse_in_header));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_ACCESS:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx, f 0%o",
               (unsigned long long) ih->nodeid, in->mask);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  uid %i", ih->uid);
    }

    oh->error = dav_access((dav_node *) ((size_t) ih->nodeid), ih->uid,
                           in->mask);

    if (oh->error)
        oh->error *= -1;

    return sizeof(struct fuse_out_header);
}


static uint32_t
fuse_create(void)
{
    struct fuse_in_header *ih= (struct fuse_in_header *) buf;
    struct fuse_open_in *in = (struct fuse_open_in *)
                              (buf + sizeof(struct fuse_in_header));
    char *name = buf + sizeof(struct fuse_in_header)
                 + sizeof(struct fuse_open_in);
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct create_out *out = (struct create_out *)
                             (buf + sizeof(struct fuse_out_header));
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_CREATE:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx, f 0%o",
               (unsigned long long) ih->nodeid, in->flags);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  pid %i, mode 0%o",
               ih->pid, in->mode);
    }

    int created = 0;
    dav_node *node = NULL;
    oh->error = dav_lookup(&node, (dav_node *) ((size_t) ih->nodeid), name,
                           ih->uid);

    if (!oh->error) {
        if (!node) {
            oh->error = -EIO;
            return sizeof(struct fuse_out_header);
        } else if (in->flags & O_EXCL) {
            oh->error = -EEXIST;
            return sizeof(struct fuse_out_header);
        }
    } else if (oh->error == ENOENT) {
        oh->error = dav_create(&node, (dav_node *) ((size_t) ih->nodeid), name,
                               ih->uid, in->mode & DAV_A_MASK);
        if (oh->error || !node) {
            if (!oh->error)
                oh->error = EIO;
            oh->error *= -1;
            return sizeof(struct fuse_out_header);
        }
        created = 1;
    } else {
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    int fd = 0;
    oh->error = dav_open(&fd, node, in->flags & ~(O_EXCL | O_CREAT), ih->pid,
                         0, ih->uid, 1);

    if (oh->error || !fd) {
        if (created)
            dav_remove((dav_node *) ((size_t) ih->nodeid), name, ih->uid);
        if (!oh->error)
            oh->error = EIO;
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    out->entry.nodeid = (size_t) node;
    out->entry.generation = out->entry.nodeid;
    out->entry.entry_valid = 1;
    out->entry.attr_valid = 1;
    out->entry.entry_valid_nsec = 0;
    out->entry.attr_valid_nsec = 0;
    set_attr(&out->entry.attr, node);

    out->open.open_flags = in->flags & (O_ACCMODE | O_APPEND);
    out->open.fh = fd;
    out->open.padding = 0;

    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  fd %i", fd);
    return sizeof(struct fuse_out_header) + sizeof(struct create_out);
}


static uint32_t
fuse_getattr(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_attr_out *out = (struct fuse_attr_out *)
                                (buf + sizeof(struct fuse_out_header));
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_GETATTR:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx",
               (unsigned long long) ih->nodeid);
    }

    oh->error = dav_getattr((dav_node *) ((size_t) ih->nodeid), ih->uid);

    if (oh->error) {
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    set_attr(&out->attr, (dav_node *) ((size_t) ih->nodeid));
    out->attr_valid = 1;
    out->attr_valid_nsec = 0;
    out->dummy = 0;

    return sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out);
}


static uint32_t
fuse_init(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    struct fuse_init_in *in = (struct fuse_init_in *)
                                  (buf + sizeof(struct fuse_in_header));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_init_out *out = (struct fuse_init_out *)
                                (buf + sizeof(struct fuse_out_header));
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_INIT:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  version %i.%i",
               in->major, in->minor);
    }

    dav_node *node;
    oh->error = dav_root(&node, ih->uid);

    if (oh->error || !node) {
        if (!oh->error)
            oh->error = EIO;
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    root = (size_t) node;
    out->major = FUSE_KERNEL_VERSION;
    out->minor = FUSE_KERNEL_MINOR_VERSION;
    out->unused[0] = 0;
    out->unused[1] = 0;
    out->unused[2] = 0;
    out->max_write = buf_size - sizeof(struct fuse_in_header)
                     - sizeof(struct fuse_write_in) - 4095;

    return sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out);
}


static uint32_t
fuse_lookup(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    char * name = (char *) (buf + sizeof(struct fuse_in_header));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_entry_out *out = (struct fuse_entry_out *)
                                 (buf + sizeof(struct fuse_out_header));
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_LOOKUP:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  p 0x%llx, %s",
               (unsigned long long) ih->nodeid, name);
    }

    dav_node *node = NULL;
    oh->error = dav_lookup(&node, (dav_node *) ((size_t) ih->nodeid), name,
                           ih->uid);

    if (oh->error || !node) {
        if (!oh->error)
            oh->error = EIO;
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    out->nodeid = (size_t) node;
    out->generation = out->nodeid;
    out->entry_valid = 1;
    out->attr_valid = 1;
    out->entry_valid_nsec = 0;
    out->attr_valid_nsec = 0;
    set_attr(&out->attr, node);

    return sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out);
}


static uint32_t
fuse_mkdir(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    struct fuse_mkdir_in *in = (struct fuse_mkdir_in *)
                               (buf + sizeof(struct fuse_in_header));
    char *name = (char *) (buf + sizeof(struct fuse_in_header)
                           + sizeof(struct fuse_mkdir_in));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_entry_out *out = (struct fuse_entry_out *)
                                 (buf + sizeof(struct fuse_out_header));
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_MKDIR:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  p 0x%llx, %s",
               (unsigned long long) ih->nodeid, name);
    }

    dav_node *node = NULL;
    oh->error = dav_mkdir(&node, (dav_node *) ((size_t) ih->nodeid), name,
                          ih->uid, in->mode & DAV_A_MASK);

    if (oh->error || !node) {
        if (!oh->error)
            oh->error = EIO;
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    out->nodeid = (size_t) node;
    out->generation = out->nodeid;
    out->entry_valid = 1;
    out->attr_valid = 1;
    out->entry_valid_nsec = 0;
    out->attr_valid_nsec = 0;
    set_attr(&out->attr, node);

    return sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out);
}


static uint32_t
fuse_mknod(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    struct fuse_mknod_in *in = (struct fuse_mknod_in *)
                               (buf + sizeof(struct fuse_in_header));
    char *name = (char *) (buf + sizeof(struct fuse_in_header)
                           + sizeof(struct fuse_mknod_in));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_entry_out *out = (struct fuse_entry_out *)
                                 (buf + sizeof(struct fuse_out_header));
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_MKNOD:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  p 0x%llx, m 0%o",
               (unsigned long long) ih->nodeid, in->mode);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  %s", name);
    }

    if (!S_ISREG(in->mode)) {
        oh->error = -ENOTSUP;
        return sizeof(struct fuse_out_header);
    }

    dav_node *node = NULL;
    oh->error = dav_create(&node, (dav_node *) ((size_t) ih->nodeid), name,
                           ih->uid, in->mode & DAV_A_MASK);

    if (oh->error || !node) {
        if (!oh->error)
            oh->error = EIO;
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    out->nodeid = (size_t) node;
    out->generation = out->nodeid;
    out->entry_valid = 1;
    out->attr_valid = 1;
    out->entry_valid_nsec = 0;
    out->attr_valid_nsec = 0;
    set_attr(&out->attr, node);

    return sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out);
}


static uint32_t
fuse_open(void)
{
    struct fuse_in_header *ih= (struct fuse_in_header *) buf;
    struct fuse_open_in *in = (struct fuse_open_in *)
                              (buf + sizeof(struct fuse_in_header));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_open_out *out = (struct fuse_open_out *)
                                (buf + sizeof(struct fuse_out_header));
    if (debug) {
        if (ih->opcode == FUSE_OPENDIR) {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_OPENDIR:");
        } else {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_OPEN:");
        }
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx, f 0%o",
               (unsigned long long) ih->nodeid, in->flags);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  pid %i, mode 0%o",
               ih->pid, in->mode);
    }

    int fd = 0;
    oh->error = dav_open(&fd, (dav_node *) ((size_t) ih->nodeid), in->flags,
                         ih->pid, 0, ih->uid, 0);

    if (oh->error || !fd) {
        if (!oh->error)
            oh->error = EIO;
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    out->open_flags = in->flags & (O_ACCMODE | O_APPEND);
    out->fh = fd;
    out->padding = 0;

    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  fd %i", fd);
    return sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
}


static uint32_t
fuse_read(void)
{
    struct fuse_in_header *ih= (struct fuse_in_header *) buf;
    struct fuse_read_in *in = (struct fuse_read_in *)
                              (buf + sizeof(struct fuse_in_header));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    if (debug) {
        if (ih->opcode == FUSE_READDIR) {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_READDIR:");
        } else {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_READ:");
        }
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx, fd %llu",
               (unsigned long long) ih->nodeid, (unsigned long long) in->fh);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  pid %i", ih->pid);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  size %u, off %llu",
               in->size, (unsigned long long) in->offset);
    }

    if (in->size > (buf_size - sizeof(struct fuse_out_header))) {
        oh->error = -EINVAL;
        return sizeof(struct fuse_out_header);
    }

    ssize_t len;
    oh->error = dav_read(&len, (dav_node *) ((size_t) ih->nodeid),
                         in->fh, buf + sizeof(struct fuse_out_header),
                         in->size, in->offset);

    if (oh->error)
        oh->error *= -1;

    return len + sizeof(struct fuse_out_header);
}


static uint32_t
fuse_release(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    struct fuse_release_in *in = (struct fuse_release_in *)
                                 (buf + sizeof(struct fuse_in_header));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    if (debug) {
        if (ih->opcode == FUSE_RELEASEDIR) {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_RELEASEDIR:");
        } else {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_RELEASE:");
        }
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx, f 0%o",
               (unsigned long long) ih->nodeid, in->flags);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  pid %i, fd %llu",
               ih->pid, (unsigned long long) in->fh);
    }

    oh->error = dav_close((dav_node *) ((size_t) ih->nodeid), in->fh,
                          in->flags, ih->pid, 0);

    if (oh->error)
        oh->error *= -1;

    return sizeof(struct fuse_out_header);
}


static uint32_t
fuse_rename(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    struct fuse_rename_in *in = (struct fuse_rename_in *)
                                (buf + sizeof(struct fuse_in_header));
    char *old = (char *) (buf + sizeof(struct fuse_in_header)
                          + sizeof(struct fuse_rename_in));
    char *new = old + strlen(old) + 1;
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_RENAME:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  sp 0x%llx, %s",
               (unsigned long long) ih->nodeid, old);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  dp 0x%llx, %s",
               (unsigned long long) in->newdir, new);
    }

    if (in->newdir == 1)
        in->newdir = root;
    oh->error = dav_rename((dav_node *) ((size_t) ih->nodeid), old,
                           (dav_node *) ((size_t) in->newdir), new, ih->uid);

    if (oh->error)
        oh->error *= -1;

    return sizeof(struct fuse_out_header);
}


static uint32_t
fuse_setattr(void)
{
    struct fuse_in_header *ih = (struct fuse_in_header *) buf;
    struct fuse_setattr_in *in = (struct fuse_setattr_in *)
                                 (buf + sizeof(struct fuse_in_header));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_attr_out *out = (struct fuse_attr_out *)
                                (buf + sizeof(struct fuse_out_header));
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_SETATTR:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx, m 0%o",
               (unsigned long long) ih->nodeid, in->mode);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  uid %i, gid %i",
               in->uid, in->gid);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  sz %llu, at %llu,",
               (unsigned long long) in->size, (unsigned long long) in->atime);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  mt %llu",
               (unsigned long long) in->mtime);
    }

    oh->error = dav_setattr((dav_node *) ((size_t) ih->nodeid), ih->uid,
                            in->valid & FATTR_MODE, in->mode,
                            in->valid & FATTR_UID, in->uid,
                            in->valid & FATTR_GID, in->gid,
                            in->valid & FATTR_ATIME, in->atime,
                            in->valid & FATTR_MTIME, in->mtime,
                            in->valid & FATTR_SIZE, in->size);

    if (oh->error) {
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    set_attr(&out->attr, (dav_node *) ((size_t) ih->nodeid));
    out->attr_valid = 1;
    out->attr_valid_nsec = 0;
    out->dummy = 0;

    return sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out);
}


static uint32_t
fuse_stat(void)
{
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_statfs_out *out = (struct fuse_statfs_out *)
                                  (buf + sizeof(struct fuse_out_header));
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_STATFS:");

    dav_stat *st = dav_statfs();
    if (!st) {
        oh->error = -ENOSYS;
        return sizeof(struct fuse_out_header);
    }

    out->st.blocks = st->blocks;
    out->st.bfree = st->bavail;
    out->st.bavail = st->bavail;
    out->st.bsize = st->bsize;
    out->st.files = st->files;
    out->st.ffree = st->ffree;
    out->st.namelen = st->namelen;
    out->st.frsize = st->bsize;
    out->st.padding = 0;
    int i;
    for (i = 0; i < 6; i++)
        out->st.spare[i] = 0;

    oh->error = 0;
    return sizeof(struct fuse_out_header) + sizeof(struct fuse_statfs_out);
}


static uint32_t
fuse_write(void)
{
    struct fuse_in_header *ih= (struct fuse_in_header *) buf;
    struct fuse_write_in *in = (struct fuse_write_in *)
                               (buf + sizeof(struct fuse_in_header));
    struct fuse_out_header *oh = (struct fuse_out_header *) buf;
    struct fuse_write_out *out = (struct fuse_write_out *)
                                 (buf + sizeof(struct fuse_out_header));
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "FUSE_WRITE:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  n 0x%llx, fd %llu",
               (unsigned long long) ih->nodeid, (unsigned long long) in->fh);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  pid %i, flags 0%o",
               ih->pid, in->write_flags);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  size %u, off %llu",
               in->size, (unsigned long long) in->offset);
    }

    if (in->size > (buf_size - sizeof(struct fuse_in_header)
                    - sizeof(struct fuse_write_in))) {
        oh->error = -EINVAL;
        return sizeof(struct fuse_out_header);
    }

    size_t size;
    oh->error = dav_write(&size, (dav_node *) ((size_t) ih->nodeid),
                          in->fh, buf + sizeof(struct fuse_in_header)
                          + sizeof(struct fuse_write_in),
                          in->size, in->offset);

    if (oh->error) {
        oh->error *= -1;
        return sizeof(struct fuse_out_header);
    }

    out->size = size;
    out->padding = 0;

    return sizeof(struct fuse_out_header) + sizeof(struct fuse_write_out);
}


/* Auxiliary functions. */

/* Writes a struct fuse_dirent to file with file descriptor fd.
   fd     : An open file descriptor to write to.
   off    : The current file size.
   name   : File name; if NULL, the last, empty entry is written.
   return value : New size of the file. -1 in case of an error. */
static off_t
write_dir_entry(int fd, off_t off, const dav_node *node, const char *name)
{
    if (!name)
        return off;

    struct fuse_dirent entry;
    size_t head = offsetof(struct fuse_dirent, name);
    size_t reclen = (head + strlen(name) + sizeof(uint64_t) -1)
                    & ~(sizeof(uint64_t) - 1);

    entry.ino = (((size_t) node) == root) ? 1 : (size_t) node;
    entry.off = off + reclen;
    entry.namelen = strlen(name);
    entry.type = (node->mode & S_IFMT) >> 12;

    size_t size = 0;
    ssize_t ret = 0;
    while (ret >= 0 && size < head) {
        ret = write(fd, (char *) &entry + size, head - size);
        size += ret;
    }
    if (size != head)
        return -1;

    ret = 0;
    while (ret >= 0 && size < (head + entry.namelen)) {
        ret = write(fd, name + size - head, entry.namelen - size + head);
        size += ret;
    }
    if (size != (head + entry.namelen))
        return -1;

    ret = 0;
    while (ret >= 0 && size < reclen) {
        ret = write(fd, "\0", 1);
        size += ret;
    }
    if (size != reclen)
        return -1;

    return off + reclen;
}


static void
set_attr(struct fuse_attr *attr, const dav_node *node)
{
    attr->ino = (((size_t) node) == root) ? 1 : (size_t) node;
    attr->size = node->size;
    attr->blocks = (node->size + 511) / 512;
    attr->atime = node->atime;
    attr->mtime = node->mtime;
    attr->ctime = node->ctime;
    attr->atimensec = 0;
    attr->mtimensec = 0;
    attr->ctimensec = 0;
    attr->mode = node->mode;
    if (S_ISDIR(node->mode)) {
        attr->nlink = node->nref;
    } else {
        attr->nlink = 1;
    }
    attr->uid = node->uid;
    attr->gid = node->gid;
    attr->rdev = 0;
}
