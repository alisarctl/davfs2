/*  cache.c: directory and file cache.
    Copyright (C) 2006, 2007, 2008, 2009, 2014, 2020 Werner Baumann

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

#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <grp.h>
#ifdef HAVE_LIBINTL_H
#include <libintl.h>
#endif
#include <pwd.h>
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <string.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include <ne_alloc.h>
#include <ne_string.h>
#include <ne_xml.h>

#include "defaults.h"
#include "mount_davfs.h"
#include "webdav.h"
#include "cache.h"
#include "util.h"

#ifdef ENABLE_NLS
#define _(String) gettext(String)
#else
#define _(String) String
#endif


/* Private constants */
/*===================*/

/* Constants describing different types of XML elements and attributes
   in cache index file. */
enum {
    ROOT = 1,
    BACKUP,
    DDIR,
    REG,
    PATH,
    NAME,
    CACHE_PATH,
    SIZE,
    MODE,
    UID,
    GID,
    ATIME,
    MTIME,
    CTIME,
    SMTIME,
    ETAG,
    LOCK_EXPIRE,
    DIRTY,
    REMOTE_EXISTS,
    END
};

static const char* const type[] = {
    [ROOT] = "root",
    [BACKUP] = "backup",
    [DDIR] = "dir",
    [REG] = "reg",
    [PATH] = "path",
    [NAME] = "name",
    [CACHE_PATH] = "cache_path",
    [SIZE] = "size",
    [MODE] = "mode",
    [UID] = "uid",
    [GID] = "gid",
    [ATIME] = "atime",
    [MTIME] = "mtime",
    [CTIME] = "ctime",
    [SMTIME] = "smtime",
    [ETAG] = "etag",
    [LOCK_EXPIRE] = "lock_expire",
    [DIRTY] = "dirty",
    [REMOTE_EXISTS] = "remote_exists",
    [END] = NULL
};


/* Private global variables */
/*==========================*/

/* File system statistics. */
static dav_stat *fs_stat;

/* Root node of the directory cache. */
static dav_node *root;

/* Directory for local buckups. */
static dav_node *backup;

/* A hash table to store the nodes. The hash is computed from the pointer
   to the node, which is also the node number. */
static dav_node **table;

/* Size of the hash table. */
static size_t table_size;

/* Number of files in list changed. */
static int nchanged;

/* List of nodes, that have been changed and the changes must be saved
   back to the server. */
static dav_node_list_item *changed;

/* How long results of PROPFIND for directories are considered valid. */
static time_t dir_refresh;

/* Wait that many seconds from last file access before doing a new
   GET If-Modiefied request. */
static time_t file_refresh;

/* Upload file that many seconds after closing. */
static time_t delay_upload;

static int sync_on_lookup;

/* Optimize file updates for graphical user interfaces = use PROPFIND to get
   the Last-Modified-dates for the whole directory instead of
   GET If-Modified-Since for single files. */
static int gui_optimize;

/* Remove nodes that are currently not needed to minimize memory usage. */
static int minimize_mem;

/* When to next run minimize_tree. 0 means to not run minimize_tree.
   Must be updated when a node is created. */
static time_t next_minimize;

/* Time interval to wait, before a directory is updated. Usually equal to
   dir_refresh, but will be varied in case the connection timed out.*/
static time_t retry;

/* Minimum retry time. */
static time_t min_retry;

/* Maximum retry time. */
static time_t max_retry;

/* Maximum number of upload attempts. */
static int max_upload_attempts;

/* Refresh locks this much seconds before they time out. */
static time_t lock_refresh;

/* Defaults for file ownership and mode. */
static uid_t default_uid;
static gid_t default_gid;
static mode_t default_file_mode;
static mode_t default_dir_mode;

/* New files get the group id of the directory in which they are created
   if this variable is set to 1. */
static int grpid;

/* Directory for cached files and directories. */
static char *cache_dir;

/* Maximum cache size. If open files require more space, this will
   be ignored. */
static unsigned long long max_cache_size;

/* Actual cache size. */
static unsigned long long cache_size;

/* Alignment boundary of dav_node in byte.
   Used to compute a hash value and file numbers from node pointers. */
static size_t alignment;

/* Pointers that will be set by dav_register_kernel_interface(), which must be
   called by the kernel-interface. Initialized to point to dummies as long as
   dav_register_kernel_interface() has not been called. */

/* A call back functions, that writes one direntry. The dummy returns EIO.
   Registering a working function is essential for mount.davfs. */
static off_t write_dir_entry_dummy(int fd, off_t off, const dav_node *node,
                                   const char *name) {
    return -1;
}
static dav_write_dir_entry_fn write_dir_entry = write_dir_entry_dummy;

/* Whether to create debug messages. */
static int debug;

/* Buffer for xml_cdata callback. */
static char *xml_data = NULL;


/* Private function prototypes and inline functions */
/*==================================================*/

/* Node operations. */

static void
add_node(dav_node *parent, dav_props *props);

static void
add_to_changed(dav_node *node);

static inline void
attr_from_cache_file(dav_node *node)
{
    struct stat st;
    if (!node->cache_path || stat(node->cache_path, &st) != 0)
        return;
    off_t old_size = node->size;
    node->size = st.st_size;
    cache_size += node->size - old_size;
    node->atime = (st.st_atime > node->atime) ? st.st_atime : node->atime;
    node->mtime = (st.st_mtime > node->mtime) ? st.st_mtime : node->mtime;
}

static void
backup_node(dav_node *orig);

static void
clean_tree(dav_node *node, volatile int *got_sigterm);

static void
delete_node(dav_node *node);

static void
delete_tree(dav_node *node);

static inline time_t
get_upload_time(dav_node *node)
{
    dav_node_list_item *item = changed;
    while (item && item->node != node)
        item = item->next;
    if (!item) return 0;
    return item->save_at;
}

static void
minimize_tree(dav_node *node);

static int
move_dir(dav_node *src, dav_node *dst, dav_node *dst_parent,
         const char *dst_name);

static int
move_no_remote(dav_node *src, dav_node *dst, dav_node *dst_parent,
               const char *dst_name);

static int
move_reg(dav_node *src, dav_node *dst, dav_node *dst_parent,
         const char *dst_name);

static dav_node *
new_node(dav_node *parent, mode_t mode);

static void
remove_from_changed(dav_node *node);

static void
remove_from_table(dav_node *node);

static void
remove_from_tree(dav_node *node);

static void remove_node(dav_node *node);

static inline int
set_next_upload_attempt(dav_node *node)
{
    dav_node_list_item *item = changed;
    while (item && item->node != node)
        item = item->next;
    if (!item) return 0;
    item->attempts++;
    if (item->attempts > max_upload_attempts)
        return -1;
    time_t delay = item->attempts * min_retry;
    item->save_at += (delay > max_retry) ? max_retry : delay;
    return 0;
}

static inline void
set_upload_time(dav_node *node)
{
    dav_node_list_item *item = changed;
    while (item && item->node != node)
        item = item->next;
    if (item)
        item->save_at = time(NULL) + delay_upload;
}

static int
update_directory(dav_node *dir, time_t refresh);

static int
update_node(dav_node *node, dav_props *props);

static void
update_path(dav_node *node, const char *src_path, const char *dst_path);

/* Get information about node. */

static int
exists(const dav_node *node);

static inline dav_node *
get_child(const dav_node *parent, const char *name)
{
    dav_node *node = parent->childs;
    while (node && strcmp(name, node->name) != 0)
        node = node->next;
    return node;
}

static dav_handle *
get_file_handle(dav_node * node, int fd, int accmode, pid_t pid, pid_t pgid);

static int
has_permission(const dav_node *node, uid_t uid, int how);

static inline int
is_backup(const dav_node *node)
{
    return (node == backup || node->parent == backup);
}

static int
is_busy(const dav_node *node);

static inline int
is_cached(dav_node *node)
{
    return (S_ISREG(node->mode) && node->cache_path);
}

static inline int
is_created(const dav_node *node)
{
    return (S_ISREG(node->mode) && node->cache_path && !node->remote_exists
            && node->parent && node->parent != backup);
}

static inline int
is_dir(const dav_node *node)
{
    return S_ISDIR(node->mode);
}

static inline int
is_dirty(const dav_node *node)
{
    return (S_ISREG(node->mode) && node->dirty);
}

static inline int
is_locked(const dav_node *node)
{
    return (S_ISREG(node->mode) && node->lock_expire);
}

static inline int
is_open(const dav_node *node)
{
    return (node->handles != NULL);
}

static inline int
is_open_write(const dav_node *node)
{
    dav_handle *fh = node->handles;
    while (fh) {
        if (fh->flags == O_RDWR || fh->flags == O_WRONLY)
            return 1;
        fh = fh->next;
    }
    return 0;
}

static inline int
is_reg(const dav_node *node)
{
    return S_ISREG(node->mode);
}

static int
is_valid(const dav_node *node);

/* Cache file functions. */

static void
close_fh(dav_node *node, dav_handle *fh);

static int
create_cache_file(dav_node *node);

static int
create_dir_cache_file(dav_node *node);

static inline void
delete_cache_file(dav_node *node)
{
    if (node->cache_path) {
        remove(node->cache_path);
        free(node->cache_path);
        node->cache_path = NULL;
        node->dirty = 0;
        if (is_reg(node))
            cache_size -= node->size;
    }
}

static inline void
set_cache_file_times(dav_node *node)
{
    if (!node->cache_path)
        return;
    struct utimbuf t;
    t.actime = node->atime;
    t.modtime = node->mtime;
    utime(node->cache_path, &t);
}

static int
open_file(int *fd, dav_node *node, int flags, pid_t pid, pid_t pgid,
          uid_t uid); 

static int
update_cache_file(dav_node *node);

static off_t
write_dir(dav_node *dir, int fd);


/* Permanent cache maintenance. */

static void
check_cache_dir(const char *dir, const char *host, const char *path,
                const char *mpoint);

static void
clean_cache(void);

static void
parse_index(void);

static void resize_cache(void);

static int
write_node(dav_node *node, FILE *file, const char *indent);

static int
xml_cdata(void *userdata, int state, const char *cdata, size_t len);

static int
xml_end_backup(void *userdata, int state, const char *nspace,
               const char *name);

static int
xml_end_date(void *userdata, int state, const char *nspace, const char *name);

static int
xml_end_decimal(void *userdata, int state, const char *nspace,
                const char *name);

static int
xml_end_dir(void *userdata, int state, const char *nspace, const char *name);

static int
xml_end_mode(void *userdata, int state, const char *nspace, const char *name);

static int
xml_end_reg(void *userdata, int state, const char *nspace, const char *name);

static int
xml_end_root(void *userdata, int state, const char *nspace, const char *name);

static int
xml_end_size(void *userdata, int state, const char *nspace, const char *name);

static int
xml_end_string(void *userdata, int state, const char *nspace,
               const char *name);

static int
xml_end_string_old(void *userdata, int state, const char *nspace,
                   const char *name);

static int
xml_start_backup(void *userdata, int parent, const char *nspace,
                 const char *name, const char **atts);

static int
xml_start_date(void *userdata, int parent, const char *nspace,
               const char *name, const char **atts);

static int
xml_start_decimal(void *userdata, int parent, const char *nspace,
                  const char *name, const char **atts);

static int
xml_start_dir(void *userdata, int parent, const char *nspace,
              const char *name, const char **atts);

static int
xml_start_mode(void *userdata, int parent, const char *nspace,
               const char *name, const char **atts);

static int
xml_start_reg(void *userdata, int parent, const char *nspace,
              const char *name, const char **atts);

static int
xml_start_root(void *userdata, int parent, const char *nspace,
               const char *name, const char **atts);

static int
xml_start_size(void *userdata, int parent, const char *nspace,
               const char *name, const char **atts);

static int
xml_start_string(void *userdata, int parent, const char *nspace,
                 const char *name, const char **atts);

/* Auxiliary. */

static size_t
test_alignment();


/* Public functions */
/*==================*/

void
dav_init_cache(const dav_args *args, const char *mpoint)
{
    debug = args->debug & DAV_DBG_CACHE;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Initializing cache");

    alignment = test_alignment();
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Alignment of dav_node: %i",
               (int) alignment);

    default_uid = args->uid;
    default_gid = args->gid;

    default_file_mode = args->file_mode;
    default_dir_mode = args->dir_mode;
    grpid = args->grpid;

    table_size = args->table_size;
    table = ne_calloc(sizeof(*table) * table_size);

    dir_refresh = args->dir_refresh;
    file_refresh = args->file_refresh;
    delay_upload = args->delay_upload;
    sync_on_lookup = args->sync_on_lookup;
    gui_optimize = args->gui_optimize;
    minimize_mem = args->minimize_mem;
    retry = dir_refresh;
    min_retry = args->retry;
    max_retry = args->max_retry;
    max_upload_attempts = args->max_upload_attempts;
    lock_refresh = args->lock_refresh;

    fs_stat = (dav_stat *) malloc(sizeof(dav_stat));
    if (!fs_stat) abort();

    fs_stat->blocks = 333333333;
    fs_stat->bavail = 133333333;
    fs_stat->n_nodes = 0;
    fs_stat->ffree = fs_stat->bavail / 4;
    fs_stat->bsize = 4096;
    fs_stat->namelen = 256;
    fs_stat->utime = 0;

    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Checking cache directory");
    max_cache_size = (unsigned long long) args->cache_size * 0x100000;
    check_cache_dir(args->cache_dir, args->host, args->path, mpoint);
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  %s", cache_dir);

    root = new_node(NULL, default_dir_mode);
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Reading stored cache data");
    parse_index();
    root->name = ne_strdup("");
    root->path = dav_conv_to_server_enc(args->path);
    root->mode = default_dir_mode;

    if (!backup)
        backup = new_node(root, S_IFDIR | S_IRWXU);
    backup->name = ne_strdup(args->backup_dir);
    backup->mode = S_IFDIR | S_IRWXU;

    clean_cache();
    next_minimize = 0;

    int ret = update_directory(root, 0);
    if (ret == EAGAIN) {
        root->utime = 0;
        ret = update_directory(root, 0);
    }
    if (ret == EAGAIN) {
        WARN(_("connection timed out two times;\n"
             "trying one last time"));
        root->utime = 0;
        ret = update_directory(root, 0);
        if (!ret)
            printf(_("Last try succeeded.\n"));
    }
    if (ret == EAGAIN) {
        WARN(_("server temporarily unreachable;\n"
             "mounting anyway"));
    } else if (ret) {
        ERR(_("Mounting failed.\n%s"), dav_get_webdav_error());
    } else {
        dav_statfs();
    }
}


void
dav_close_cache(volatile int *got_sigterm)
{
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Closing cache");

    write_dir_entry = &write_dir_entry_dummy;

    clean_tree(root, got_sigterm);

    char *new_index = ne_concat(cache_dir, "/", DAV_INDEX, ".new", NULL);
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Creating index %s.",
               new_index);
    FILE *new_file = fopen(new_index, "w");
    if (new_file) {

        int ret = fprintf(new_file,
                          "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n");
        if (ret >= 0)
            ret = write_node(root, new_file, "");

        fclose(new_file);

        if (ret >= 0) {
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                       "Replacing old index");
            char *old_index = ne_concat(cache_dir, "/", DAV_INDEX, NULL);
            if (rename(new_index, old_index) != 0)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
                       _("can't replace %s with %s"), old_index, new_index);
            free(old_index);
        } else {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
                   _("error writing new index file %s"), new_index);
        }
    } else {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("can't create new index file for %s"), cache_dir);
    }
    free(new_index);
}


size_t
dav_register_kernel_interface(dav_write_dir_entry_fn write_fn,
                              unsigned int *blksize)
{
    if (write_fn)
        write_dir_entry = write_fn;

    if (blksize)
        *blksize = fs_stat->bsize;

    return alignment;
}


int
dav_tidy_cache(void)
{
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "tidy: %i of %llu nodes changed", nchanged,
               (long long int) fs_stat->n_nodes);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "cache-size: %llu MiBytes.",
               (unsigned long long) ((cache_size + 0x80000) / 0x100000));
    }

    static dav_node_list_item *item = NULL;

    dav_node_list_item *next_item = changed;
    dav_node *node = NULL;
    int found = 0;
    while (next_item) {
        node = next_item->node;
        if (is_locked(node) && node->lock_expire < time(NULL) + lock_refresh)
            dav_lock_refresh(node->path, &node->lock_expire);
        if (next_item == item)
            found = 1;
        next_item = next_item->next;
    }
    if (!found)
        item = changed;

    time_t save_at = 0;
    if (item) {
        node = item->node;
        save_at = item->save_at;
        item = item->next;
    } else {
        node = NULL;
    }

    if (node && (is_dirty(node) || is_created(node)) && !is_open_write(node)
            && !is_backup(node) && save_at && save_at <= time(NULL)) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "tidy: %s", node->path);
        int set_execute = -1;
        if (is_created(node) && node->mode & (S_IXUSR | S_IXGRP | S_IXOTH))
            set_execute = 1;
        int ret = dav_put(node->path, node->cache_path, &node->remote_exists,
                          &node->lock_expire, &node->etag, &node->smtime,
                          set_execute);
        if (!ret) {
            node->utime = time(NULL);
            node->dirty = 0;
            if (dav_unlock(node->path, &node->lock_expire) == 0)
                remove_from_changed(node);
        } else {
            if (debug) {
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "tidy: neon error");
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "      %s",
                       dav_get_webdav_error());
            }
            if (ret == EACCES || ret == EINVAL || ret == ENOENT
                      || ret == EPERM || ret == ENOSPC || ret == EEXIST
                      || set_next_upload_attempt(node) < 0) {
                dav_unlock(node->path, &node->lock_expire);
                delete_cache_file(node->parent);
                node->parent->utime = 0;
                remove_node(node);
            }
        }
    } else if (node && is_locked(node) && !is_dirty(node) && !is_created(node)
            && !is_open_write(node)) {
        if (dav_unlock(node->path, &node->lock_expire) == 0)
            remove_from_changed(node);
    }

    if (cache_size > max_cache_size)
        resize_cache();

    if (minimize_mem && next_minimize && time(NULL) > next_minimize) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "minimize_tree");
        next_minimize = 0;
        minimize_tree(root);
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                    "minimize_tree: %llu nodes remaining",
                    (unsigned long long) fs_stat->n_nodes);
    }

    if (item)
        return 1;
    return 0;
}


/* Upcalls from the kernel. */

int
dav_access(dav_node *node, uid_t uid, int how)
{
    if (!is_valid(node))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "access %s", node->path);
    if (!has_permission(node, uid, how))
        return EACCES;

    return 0;
}


int
dav_close(dav_node *node, int fd, int flags, pid_t pid, pid_t pgid)
{
    if (!exists(node))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), " close %s", node->path);

    dav_handle *fh = get_file_handle(node, fd,
                                     is_dir(node) ? O_RDWR : flags & O_ACCMODE,
                                     pid, pgid);
    if (!fh)
        return EBADF;

    close_fh(node, fh);

    if (!node->parent && node != root && !is_open(node)) {
        remove_from_table(node);
        delete_node(node);
        return 0;
    }

    if (is_dir(node)) {
        node->atime = time(NULL);
        delete_cache_file(node);
        return 0;
    }

    attr_from_cache_file(node);
    set_upload_time(node);
    fs_stat->utime = 0;

    if (delay_upload == 0 && (is_dirty(node) || is_created(node))
            && !is_open_write(node) && !is_backup(node)) {
        int set_execute = -1;
        if (is_created(node) && node->mode & (S_IXUSR | S_IXGRP | S_IXOTH))
            set_execute = 1;
        int ret = dav_put(node->path, node->cache_path, &node->remote_exists,
                          &node->lock_expire, &node->etag, &node->smtime,
                          set_execute);
        if (!ret) {
            node->utime = time(NULL);
            node->dirty = 0;
            if (dav_unlock(node->path, &node->lock_expire) == 0)
                remove_from_changed(node);
        } else {
            if (debug) {
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "close: neon error");
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "      %s",
                       dav_get_webdav_error());
            }
            if (ret == EACCES || ret == EINVAL || ret == ENOENT
                    || ret == EPERM || ret == ENOSPC || ret == EEXIST
                    || set_next_upload_attempt(node) < 0) {
                dav_unlock(node->path, &node->lock_expire);
                delete_cache_file(node->parent);
                node->parent->utime = 0;
                remove_node(node);
            }
        }
        return ret;
    }

    return 0;
}


int
dav_create(dav_node **nodep, dav_node *parent, const char *name, uid_t uid,
           mode_t mode)
{
    if (!is_valid(parent))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "create %s%s", parent->path,
               name);
    if (!is_dir(parent))
        return ENOTDIR;
    if ((parent == root && strcmp(name, backup->name) == 0) || parent == backup)
        return EINVAL;
    if (!has_permission(parent, uid, X_OK | W_OK))
        return EACCES;

    if (get_child(parent, name))
        return EEXIST;

    struct passwd *pw = getpwuid(uid);
    if (!pw)
        return EINVAL;

    char *name_conv = dav_conv_to_server_enc(name);
    char *path = ne_concat(parent->path, name_conv, NULL);
    free(name_conv);

    *nodep = new_node(parent, mode | S_IFREG);
    (*nodep)->path = path;
    (*nodep)->name = ne_strdup(name);
    (*nodep)->uid = uid;
    if (grpid && parent->gid != 0) {
        (*nodep)->gid = parent->gid;
    } else {
        (*nodep)->gid = pw->pw_gid;
    }
    int ret = create_cache_file(*nodep);

    if (!ret)
        ret = dav_lock(path, &(*nodep)->lock_expire, &(*nodep)->remote_exists);
    if (ret == EEXIST)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("File %s exists on the server but should not. "
                 "Maybe it is an error in the server's LOCK implementation. "
                 "You may try option 'use_locks 0' in davfs2.conf."),
                 (*nodep)->path);

    if (!ret) {
        (*nodep)->smtime = (*nodep)->mtime;
        if (!is_created(*nodep))
            dav_head((*nodep)->path, &(*nodep)->etag, &(*nodep)->smtime, NULL);
        (*nodep)->utime = (*nodep)->smtime;
        delete_cache_file(parent);
        parent->mtime = (*nodep)->mtime;
        parent->ctime = (*nodep)->mtime;
        add_to_changed(*nodep);
    } else {
        remove_from_tree(*nodep);
        remove_from_table(*nodep);
        delete_node(*nodep);
    }

    return ret;
}


int
dav_getattr(dav_node *node, uid_t uid)
{
    if (!is_valid(node))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "getattr %s", node->path);
    if (node->parent && !has_permission(node->parent, uid, X_OK | R_OK))
        return EACCES;

    if (is_dir(node)) {
        if (!node->utime)
            update_directory(node, retry);
        if (!node->cache_path) {
            if (create_dir_cache_file(node) != 0)
                return EIO;
            delete_cache_file(node);
        }
    } else if (is_open(node)) {
        attr_from_cache_file(node);
    }

    return 0;
}


int
dav_lookup(dav_node **nodep, dav_node *parent, const char *name, uid_t uid)
{
    if (!is_valid(parent))
        return ENOENT;

    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "lookup %s%s", parent->path,
               name);
    if (!is_dir(parent))
        return ENOTDIR;
    if (!has_permission(parent, uid, X_OK | R_OK))
        return EACCES;

    update_directory(parent, retry);
    *nodep = get_child(parent, name);
    if (!*nodep) {
        update_directory(parent, file_refresh);
        *nodep = get_child(parent, name);
    }
    if (!*nodep)
        return ENOENT;

    if (is_dir(*nodep)) {
        if (!(*nodep)->utime)
            update_directory(*nodep, retry);
    } else {

        if (is_open(*nodep))
            attr_from_cache_file(*nodep);
        else if (sync_on_lookup && !is_dirty(*nodep) && !is_backup(*nodep)) {
            int ret;

            dav_props *props = NULL;
            ret = dav_get_collection((*nodep)->path, &props);

            if (ret == 0) {
                if ((*nodep)->size != props->size) {
                    (*nodep)->size = props->size;
                    delete_cache_file(*nodep);
                }
                dav_delete_props(props);
            }
        }
    }

    return 0;
}


int
dav_mkdir(dav_node **nodep, dav_node *parent, const char *name, uid_t uid,
          mode_t mode)
{
    if (!is_valid(parent))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "mkdir %s%s", parent->path,
               name);
    if (!is_dir(parent))
        return ENOTDIR;
    if (parent == backup)
        return EINVAL;
    if (!has_permission(parent, uid, X_OK | W_OK))
        return EACCES;

    update_directory(parent, retry);
    if (get_child(parent, name))
        return EEXIST;

    struct passwd *pw = getpwuid(uid);
    if (!pw)
        return EINVAL;

    char *name_conv = dav_conv_to_server_enc(name);
    char *path = ne_concat(parent->path, name_conv, "/", NULL);
    free(name_conv);
    int ret = dav_make_collection(path);

    if (!ret) {
        *nodep = new_node(parent, mode | S_IFDIR);
        (*nodep)->path = path;
        (*nodep)->name = ne_strdup(name);
        (*nodep)->uid = uid;
        if (grpid && parent->gid != 0) {
            (*nodep)->gid = parent->gid;
        } else {
            (*nodep)->gid = pw->pw_gid;
        }
        (*nodep)->smtime = (*nodep)->mtime;
        (*nodep)->utime = (*nodep)->mtime;
        delete_cache_file(parent);
        parent->mtime = (*nodep)->mtime;
        parent->ctime = (*nodep)->mtime;
    } else {
        free(path);
    }

    return ret;
}


int
dav_open(int *fd, dav_node *node, int flags, pid_t pid, pid_t pgid, uid_t uid,
         int open_create)
{
    if (!is_valid(node))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "open %s", node->path);
    if (flags & (O_EXCL | O_CREAT))
        return EINVAL;

    int how;
    if ((O_ACCMODE & flags) == O_WRONLY) {
        how = W_OK;
    } else if ((O_ACCMODE & flags) == O_RDONLY) {
        how = R_OK;
    } else {
        how = R_OK | W_OK;
    }
    if (!open_create && !has_permission(node, uid, how))
        return EACCES;

    if (is_dir(node)) {
        if ((how & W_OK) || (flags & O_TRUNC))
            return EINVAL;
        update_directory(node, file_refresh);
        if (create_dir_cache_file(node) != 0)
            return EIO;
        node->atime = time(NULL);
        return open_file(fd, node, O_RDWR, pid, pgid, uid);
    }

    int ret = 0;
    if ((O_ACCMODE & flags) == O_RDONLY) {

        ret = update_cache_file(node);
        if (!ret) 
            ret = open_file(fd, node, flags & O_ACCMODE, pid, pgid, uid);

    } else {

        if (!is_locked(node) && !is_backup(node))
            ret = dav_lock(node->path, &node->lock_expire,
                           &node->remote_exists);

        if (!ret && (flags & O_TRUNC)) {
            ret = create_cache_file(node);
            if (!ret) {
                ret = open_file(fd, node,
                                flags & (O_ACCMODE | O_TRUNC | O_APPEND),
                                pid, pgid, uid);
            }
        } else if (!ret) {
            ret = update_cache_file(node);
            if (!ret)
                ret = open_file(fd, node, flags & (O_ACCMODE | O_APPEND),
                                pid, pgid, uid);
        }
 
        if (!ret)
            add_to_changed(node);
    }

    return ret;
}


int
dav_read(ssize_t *len, dav_node * node, int fd, char *buf, size_t size,
         off_t offset)
{
    if (!exists(node))
        return ENOENT;

    dav_handle *fh = get_file_handle(node, fd, 0, 0, 0);
    if (!fh)
        return EBADF;
    if (fh->flags == O_WRONLY)
        return EINVAL;

    if (offset == 0 && is_dir(node) && !node->cache_path) {
        if (ftruncate(fh->fd, 0) != 0) return EIO;
        off_t sz = write_dir(node, fh->fd);
        if (sz <= 0) return EIO;
        node->size = sz;
    }

    *len = pread(fd, buf, size, offset);
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "read %lli",
               (long long int) *len);
    if (*len < 0)
        return errno;

    if (*len < size)
        memset(buf + *len, '\0', size - *len);

    return 0;
}


int
dav_remove(dav_node *parent, const char *name, uid_t uid)
{
    if (!is_valid(parent))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "remove %s%s", parent->path,
               name);
    if (!is_dir(parent))
        return ENOTDIR;
    if (!has_permission(parent, uid, X_OK | W_OK))
        return EACCES;

    update_directory(parent, retry);
    dav_node *node = get_child(parent, name);
    if (!node) {
        delete_cache_file(parent);
        parent->utime = 0;
        return ENOENT;
    }
    if (is_dir(node))
        return EISDIR;

    int ret = 0;
    if (is_created(node)) {
        if (is_locked(node))
            ret = dav_unlock(node->path, &node->lock_expire);
    } else if (!is_backup(node)) {
        ret = dav_delete(node->path, &node->lock_expire);
        if (ret == ENOENT)
            ret = 0;
    }
    if (ret)
        return ret;

    fs_stat->utime = 0;
    remove_from_tree(node);
    remove_from_changed(node);
    if (is_open(node)) {
        node->parent = NULL;
    } else {
        remove_from_table(node);
        delete_node(node);
    }
    delete_cache_file(parent);
    parent->mtime = time(NULL);
    parent->ctime = parent->mtime;

    return 0;
}


int
dav_rename(dav_node *src_parent, const char *src_name, dav_node *dst_parent,
           const char *dst_name, uid_t uid)
{
    if (!is_valid(src_parent) || !is_valid(dst_parent))
        return ENOENT;
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "rename %s%s",
               src_parent->path, src_name);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  into %s%s",
               dst_parent->path, dst_name);
    }
    if (!is_dir(src_parent) || !is_dir(dst_parent))
        return ENOTDIR;
    if (is_backup(dst_parent))
        return EINVAL;
    if (!has_permission(src_parent, uid, X_OK | W_OK)
            || !has_permission(dst_parent, uid, X_OK | W_OK))
        return EACCES;

    update_directory(src_parent, retry);
    dav_node *src = get_child(src_parent, src_name);
    dav_node *dst = get_child(dst_parent, dst_name);
    if (!src) {
        delete_cache_file(src_parent);
        src_parent->utime = 0;
        return ENOENT;
    }
    if (src == backup || (dst && is_backup(dst)))
        return EINVAL;

    int ret;
    if (is_dir(src)) {
        ret = move_dir(src, dst, dst_parent, dst_name);
    } else {
        if (is_created(src) || is_backup(src)) {
            ret = move_no_remote(src, dst, dst_parent, dst_name);
        } else {
            ret = move_reg(src, dst, dst_parent, dst_name);
        }
    }

    if (!ret) {
        if (src_parent != dst_parent) {
            remove_from_tree(src);
            delete_cache_file(src_parent);
            src_parent->mtime = time(NULL);
            src_parent->ctime = src_parent->mtime;
            src->parent = dst_parent;
            src->next = dst_parent->childs;
            dst_parent->childs = src;
            if (is_dir(src))
                ++src->parent->nref;
        }
        delete_cache_file(dst_parent);
        dst_parent->mtime = time(NULL);
        dst_parent->ctime = dst_parent->mtime;
    }

    return ret;
}


int
dav_rmdir(dav_node *parent, const char *name, uid_t uid)
{
    if (!is_valid(parent))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "rmdir %s%s", parent->path,
               name);
    if (!is_dir(parent))
        return ENOTDIR;
    if (!has_permission(parent, uid, X_OK | W_OK))
        return EACCES;

    update_directory(parent, retry);
    dav_node *node = get_child(parent, name);
    if (!node) {
        delete_cache_file(parent);
        parent->utime = 0;
        return ENOENT;
    }
    if (node == backup)
        return EINVAL;
    if (!is_dir(node))
        return ENOTDIR;
    if (node->childs)
        return ENOTEMPTY;

    int ret = dav_delete_dir(node->path);
    if (!ret) {
        remove_node(node);
        delete_cache_file(parent);
        parent->mtime = time(NULL);
        parent->ctime = parent->mtime;
    }

    return ret;
}


int
dav_root(dav_node **nodep, uid_t uid)
{
    if (uid != 0)
        return EPERM;
    *nodep = root;
    return 0;
}


int
dav_setattr(dav_node *node, uid_t uid, int sm, mode_t mode, int so,
            uid_t owner, int sg, gid_t gid, int sat, time_t atime, int smt,
            time_t mtime, int ssz, off_t size)
{
    if (!is_valid(node))
        return ENOENT;
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "setattr %s", node->path);
    if (node->parent != NULL && !has_permission(node->parent, uid, X_OK))
        return EACCES;

    if (so) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  set owner to %i",
                   owner);
        if (node == backup)
            return EINVAL;
        if (uid != 0 && (uid != owner || uid != node->uid))
            return EPERM;
        if (!getpwuid(owner))
            return EINVAL;
    }

    if (sg) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  set group to %i",
                   gid);
        if (node == backup)
            return EINVAL;
        if (uid != node->uid && uid != 0)
            return EPERM;
        if (uid != 0) {
            struct passwd *pw = getpwuid(uid);
            if (!pw)
                return EPERM;
            if (pw->pw_gid != gid) {
                struct group *gr = getgrgid(gid);
                if (!gr)
                    return EPERM;
                char **member = gr->gr_mem;
                while (*member != NULL && strcmp(*member, pw->pw_name) != 0)
                    ++member;
                if (*member == NULL)
                    return EPERM;
            }
        }
        if (!getgrgid(gid))
            return EINVAL;
    }

    if (sm) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  set mode to %o",
                   mode);
        if (node == backup)
            return EINVAL;
        if (uid != node->uid && uid != 0)
            return EPERM;
    }

    if (sat || smt) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  set times");
        if (uid != node->uid && uid != 0 && !has_permission(node, uid, W_OK))
            return EPERM;
    }

    if (ssz) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  set size");
        if (is_dir(node))
            return EINVAL;
        if (uid != node->uid && uid != 0 && !has_permission(node, uid, W_OK))
            return EPERM;
        int ret = 0;
        if (!is_locked(node) && !is_backup(node))
            ret = dav_lock(node->path, &node->lock_expire,
                           &node->remote_exists);
        if (!ret && size == 0) {
            ret = create_cache_file(node);
        } else if (!ret) {
            ret = update_cache_file(node);
        }
        if (!ret)
            ret = truncate(node->cache_path, size);
        if (ret)
            return ret;
        attr_from_cache_file(node);
        node->dirty = 1;
        add_to_changed(node);
        set_upload_time(node);
    }

    if (so)
        node->uid = owner;

    if (sg)
        node->gid = gid;

    if (sm) {
        if (!is_backup(node) && !is_created(node)) {
            int set_execute = -1;
            if ((node->mode & (S_IXUSR | S_IXGRP | S_IXOTH))
                    && !(mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
                set_execute = 0;
            if (!(node->mode & (S_IXUSR | S_IXGRP | S_IXOTH))
                    && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
                set_execute = 1;
            if (set_execute != -1) {
                if (is_dirty(node) && !is_locked(node)) {
                    int err = 0;
                    time_t smtime = 0;
                    char *etag = NULL;
                    dav_head(node->path, &etag, &smtime, NULL);
                    if (etag && node->etag && strcmp(etag, node->etag) != 0)
                        err = EIO;
                    if (smtime && smtime > node->smtime)
                        err = EIO;
                    if (etag)
                        free(etag);
                    if (err)
                        return EIO;
                }
                dav_set_execute(node->path, set_execute);
                if (is_dirty(node))
                    dav_head(node->path, &node->etag, &node->smtime, NULL);
            }
        }
        node->mode = (node->mode & ~DAV_A_MASK) | mode;
    }

    if (sat)
        node->atime = atime;

    if (smt)
        node->mtime = mtime;

    if (sat || smt)
        set_cache_file_times(node);

    node->ctime = time(NULL);
    return 0;
}


dav_stat *
dav_statfs(void)
{
    if (time(NULL) > (fs_stat->utime + retry)) {
        uint64_t available = 0;
        uint64_t used = 0;
        if (dav_quota(root->path, &available, &used) == 0) {
            fs_stat->bavail = available / fs_stat->bsize;
            fs_stat->ffree = fs_stat->bavail / 4;
            if (used > 0) {
                fs_stat->blocks = fs_stat->bavail + (used / fs_stat->bsize);
            } else {
                fs_stat->blocks = fs_stat->bavail + (fs_stat->n_nodes * 4);
            }
            fs_stat->utime = time(NULL);
        }
    }
    fs_stat->files = fs_stat->ffree + fs_stat->n_nodes;

    return fs_stat;
}


int
dav_sync(dav_node *node)
{
    if (!exists(node))
        return ENOENT;

    dav_handle *fh = node->handles;
    while (fh) {
        if (fh->flags != O_RDONLY)
            fsync(fh->fd);
        fh = fh->next;
    }

    return 0;
}


int
dav_write(size_t *written, dav_node * node, int fd, char *buf, size_t size,
          off_t offset)
{
    if (!exists(node))
        return ENOENT;
    if (is_dir(node))
        return EBADF;

    dav_handle *fh = get_file_handle(node, fd, 0, 0, 0);
    if (!fh)
        return EBADF;
    if (fh->flags == O_RDONLY)
        return EINVAL;

    *written = 0;
    ssize_t n = 0;
    while (*written < size && n >= 0) {
        n = pwrite(fd, buf + *written, size - *written, offset + *written);
        if (n < 0)
            return errno;
        *written += n;
    }

    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  written %lli",
               (unsigned long long) *written);
    return 0;
}


/* Private functions */
/*===================*/

/* Node maintenance. */

/* Creates a new node taking properties from props and adds it to the
   child list of parent and to the hash table.
   If props references directory backup,  no node will be created.
   parent : The parent directory node for the new node.
   props  : Properties retrieved from the server. Will be freed. */
static void
add_node(dav_node *parent, dav_props *props)
{
    if (parent == root && strcmp(props->name, backup->name) == 0) {
        dav_delete_props(props);
        return;
    }

    dav_node *node;

    if (props->is_dir) {
        node = new_node(parent, default_dir_mode);
    } else {
        node = new_node(parent, default_file_mode);
        node->size = props->size;
        node->remote_exists = 1;
        if (props->is_exec == 1) {
            node->mode |= (node->mode & S_IRUSR) ? S_IXUSR : 0;
            node->mode |= (node->mode & S_IRGRP) ? S_IXGRP : 0;
            node->mode |= (node->mode & S_IROTH) ? S_IXOTH : 0;
        } else if (props->is_exec == 0) {
            node->mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
        }
    }

    if (grpid && parent->gid != 0)
        node->gid = parent->gid;

    node->path = props->path;
    node->name = props->name;
    node->etag = props->etag;
    node->smtime = props->mtime;
    if (node->smtime > 0)
        node->mtime = node->smtime;
    node->ctime = node->mtime;

    free(props);
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "added %s", node->path);
}


/* Checks whether node is allready in the list of changed nodes. If not
   it will be appended at the end of the list. */
static void
add_to_changed(dav_node *node)
{
    dav_node_list_item **chp = &changed;
    while (*chp) {
        if ((*chp)->node == node)
            return;
        chp = &(*chp)->next;
    }
    *chp = (dav_node_list_item *) malloc(sizeof(dav_node_list_item));
    if (!*chp)
        abort();
    (*chp)->node = node;
    (*chp)->next = NULL;
    (*chp)->attempts = 0;
    (*chp)->save_at = 0;
    nchanged++;
}


/* Creates a new file in directory backup. The name will be the name of the
   cache file of orig and attributes will be taken from orig. The cache
   file will be moved from orig to the new node. Open file descriptors
   will stay with orig.
   orig : the node to be backed up. */
static void
backup_node(dav_node *orig)
{
    if (!orig->cache_path)
        return;
    dav_node *node = new_node(backup, orig->mode);
    node->name = ne_strdup(orig->cache_path + strlen(cache_dir) +1);
    node->cache_path = orig->cache_path;
    orig->cache_path = NULL;
    orig->dirty = 0;
    node->size = orig->size;
    node->uid = default_uid;
    node->gid = default_gid;
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "created backup of %p",
               orig);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  %s", orig->path);
    }
    delete_cache_file(backup);
    backup->mtime = time(NULL);
    backup->ctime = backup->mtime;
}


/* Scans the directory tree starting from node and
   - if *got_sigterm == 1: saves dirty files to the server and unlocks them.
     If it can not be safed, a local backup is created and the node is deleted. 
   - removes any file nodes without cached file
   - removes all dir nodes that have not at least one file node below.
   Short: removes everthing that is not necessary to correctly reference
   the cached files.
   Node node itself will be removed and deleted if possible.
   Directory backup and root will never be removed.
   Kernel will *not* be notified about changes.
   Member nref of directories will be adjusted. */
static void
clean_tree(dav_node *node, volatile int *got_sigterm)
{
    if (node == backup) {
        delete_cache_file(backup);
        return;
    }

    if (is_dir(node)) {

        dav_node *child = node->childs;
        while (child) {
            dav_node *next = child->next;
            clean_tree(child, got_sigterm);
            child = next;
        }
        if (!node->childs && node != root && node != backup) {
            remove_from_tree(node);
            remove_from_table(node);
            delete_node(node);
        } else {
            delete_cache_file(node);
        }

    } else if (!is_cached(node) || access(node->cache_path, F_OK) != 0) {

        if (is_locked(node) && !*got_sigterm)
            dav_unlock(node->path, &node->lock_expire);
        remove_from_tree(node);
        remove_from_table(node);
        delete_node(node);

    } else if ((is_dirty(node) || is_created(node)) && !*got_sigterm) {

        int set_execute = -1;
        if (is_created(node)
                && node->mode & (S_IXUSR | S_IXGRP | S_IXOTH))
            set_execute = 1;
        int ret = dav_put(node->path, node->cache_path,
                          &node->remote_exists, &node->lock_expire,
                          &node->etag, &node->smtime, set_execute);
        if (is_locked(node))
            dav_unlock(node->path, &node->lock_expire);
        if (!ret) {
            node->mtime = node->smtime;
            if (node->mtime > node->atime)
                node->atime = node->mtime;
            set_cache_file_times(node);
            node->dirty = 0;
        } else if (ret == EACCES || ret == EINVAL || ret == ENOENT
                      || ret == EPERM || ret == ENOSPC || ret == EEXIST) {
            backup_node(node);
            remove_from_tree(node);
            remove_from_table(node);
            delete_node(node);
        }
    } else {
        node->mtime = node->smtime;
        if (node->mtime > node->atime)
            node->atime = node->mtime;
        set_cache_file_times(node);
    }
}


/* Frees any resources held by node and finally frees node.
   If there are open file descriptors, this will be closed. */
static void
delete_node(dav_node *node)
{
    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "deleting node %p", node);
    if (node->path)
        free(node->path);
    if (node->name)
        free(node->name);
    delete_cache_file(node);
    if (node->etag)
        free(node->etag);
    while (node->handles) {
        dav_handle *tofree = node->handles;
        node->handles = node->handles->next;
        close(tofree->fd);
        free(tofree);
    }
    free(node);
    fs_stat->n_nodes--;
}


/* Deletes the tree starting at and including node. The tree is freed
   uncontionally, no checks for lost update problem and the like are
   done, also backup will be deleted if in tree.
   Exeption: the root node will not be deleted. */
static void
delete_tree(dav_node *node)
{
    while (node->childs)
        delete_tree(node->childs);

    if (node != root) {
        remove_from_tree(node);
        remove_from_table(node);
        delete_node(node);
        if (node == backup)
            backup = NULL;
    }
}


/* Removes file nodes that are currently not needed to minimize
   memory usage. */
static void
minimize_tree(dav_node *node)
{
    if (node == backup) return;

    if (is_dir(node)) {

        int rm = !is_open(node)
                    && (time(NULL) > (node->utime + 2 * file_refresh))
                    && (time(NULL) > (node->atime + 2 * file_refresh));
        dav_node *child = node->childs;
        while (child) {
            dav_node *next = child->next;
            if (rm || is_dir(child)) {
                minimize_tree(child);
            } else if (next_minimize == 0) {
                next_minimize = time(NULL) + 2 * file_refresh;
            }
            child = next;
        }

    } else if (!is_cached(node) && !is_locked(node) && !is_created(node)) {

        remove_from_tree(node);
        remove_from_table(node);
        delete_node(node);

    } else if (next_minimize == 0) {

        next_minimize = time(NULL) + file_refresh;

    }
}


/* Moves directory src to dst using WebDAV method MOVE. */
static int
move_dir(dav_node *src, dav_node *dst, dav_node *dst_parent,
         const char *dst_name)
{
    if (dst && !is_dir(dst))
        return ENOTDIR;
    if (dst && is_busy(dst))
        return EBUSY;

    char *dst_path;
    if (!dst) {
        char *dst_conv = dav_conv_to_server_enc(dst_name);
        dst_path = ne_concat(dst_parent->path, dst_conv, "/", NULL);
        free(dst_conv);
    } else {
        dst_path = ne_strdup(dst->path);
    }

    if (dav_move(src->path, dst_path) != 0) {
        free(dst_path);
        return EIO;
    }

    if (dst)
        remove_node(dst);

    free(src->name);
    src->name = ne_strdup(dst_name);
    update_path(src, src->path, dst_path);
    free(dst_path);

    return 0;
}


/* src does not exist on the server, but there may be a locked null-resource.
   - if dst exists it will be removed, local and remote
   - if src is locked, it is unlocked
   - the path of src is changed according dst_name
   - dst is locked on the server
   - src is moved to its new position in the tree. */
static int
move_no_remote(dav_node *src, dav_node *dst, dav_node *dst_parent,
               const char *dst_name)
{
    if (dst && is_dir(dst))
        return EISDIR;

    char *dst_path;
    if (!dst) {
        char *dst_conv = dav_conv_to_server_enc(dst_name);
        dst_path = ne_concat(dst_parent->path, dst_conv, NULL);
        free(dst_conv);
    } else {
        dst_path = ne_strdup(dst->path);
    }

    if (dst) {
        int ret = 0;
        if (is_created(dst)) {
            if (is_locked(dst))
                ret = dav_unlock(dst_path, &dst->lock_expire);
        } else {
            ret = dav_delete(dst_path, &dst->lock_expire);
        }
        if (ret == ENOENT)
            ret = 0;
        if (ret) {
            free(dst_path);
            return EIO;
        }
        remove_from_tree(dst);
        remove_from_changed(dst);
        if (is_open(dst)) {
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                       "invalidating node %p", dst);
            dst->parent = NULL;
        } else {
            remove_from_table(dst);
            delete_node(dst);
        }
    }

    if (is_created(src) && is_locked(src))
        dav_unlock(src->path, &src->lock_expire);
    src->remote_exists = 0;

    dav_lock(dst_path, &src->lock_expire, &src->remote_exists);
    if (!is_created(src) && (src->mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
        dav_set_execute(dst_path, 1);

    free(src->name);
    src->name = ne_strdup(dst_name);
    free(src->path);
    src->path = dst_path;
    if (src->etag) {
        free(src->etag);
        src->etag = NULL;
    }
    src->smtime = time(NULL);

    if (!is_created(src))
        dav_head(src->path, &src->etag, &src->smtime, NULL);
    src->utime = time(NULL);

    return 0;
}


/* Moves file src to dst using WebDAV method MOVE. */
static int
move_reg(dav_node *src, dav_node *dst, dav_node *dst_parent,
         const char *dst_name)
{
    if (dst && is_dir(dst))
        return EISDIR;

    char *dst_path;
    if (!dst) {
        char *dst_conv = dav_conv_to_server_enc(dst_name);
        dst_path = ne_concat(dst_parent->path, dst_conv, NULL);
        free(dst_conv);
    } else {
        dst_path = ne_strdup(dst->path);
    }

    if (dav_move(src->path, dst_path) != 0) {
        free(dst_path);
        return EIO;
    }

    if (is_locked(src)) {
        src->lock_expire = 0;
        if (is_dirty(src))
            dav_lock(dst_path, &src->lock_expire, &src->remote_exists);
    }
    if (is_cached(src))
        dav_head(dst_path, &src->etag, &src->smtime, NULL);
    if (dst) {
        remove_from_tree(dst);
        remove_from_changed(dst);
        if (is_open(dst)) {
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                       "invalidating node %p", dst);
            dst->parent = NULL;
        } else {
            remove_from_table(dst);
            delete_node(dst);
        }
    }

    free(src->name);
    src->name = ne_strdup(dst_name);
    free(src->path);
    src->path = dst_path;
    src->utime = time(NULL);

    return 0;
}


/* Creates a new node. mode must have the I_ISDIR or I_ISREG bit set.
   node->mode is set to mode. All other
   members are set to reasonable defaults. The new node will be inserted
   into the child list of parent and the hash table. Member nref of the
   parent will be updated.
   parent : The parent of the new node, may be NULL.
   mode   : Tthe mode of the new node.
   return value : A pointer to the new node. */
static dav_node *
new_node(dav_node *parent, mode_t mode)
{
    dav_node *node = (dav_node *) ne_malloc(sizeof(dav_node));

    node->parent = parent;
    node->childs = NULL;
    if (parent) {
        if (S_ISDIR(mode))
            ++parent->nref;
        node->next = parent->childs;
        parent->childs = node;
    } else {
        node->next = NULL;
    }

    size_t i = ((size_t) node / alignment) % table_size;
    node->table_next = table[i];
    table[i] = node;

    node->path = NULL;
    node->name = NULL;
    node->cache_path = NULL;
    node->etag = NULL;
    node->handles = NULL;
    node->size = 0;

    node->atime = time(NULL);
    node->mtime = node->atime;
    node->ctime = node->atime;
    node->utime = 0;
    node->smtime = 0;
    node->lock_expire = 0;

    if (S_ISDIR(mode)) {
        node->nref = 2;
    } else {
        node->nref = 1;
    }
    node->remote_exists = 0;
    node->dirty = 0;
    node->uid = default_uid;
    node->gid = default_gid;
    node->mode = mode;

    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "new node: %p->%p",
               node->parent, node);
    fs_stat->n_nodes++;
    if (next_minimize == 0)
        next_minimize = node->atime + file_refresh;

    return node;
}


/* Removes a node from the list of changed nodes. */
static void
remove_from_changed(dav_node *node)
{
    dav_node_list_item **chp = &changed;
    while (*chp && (*chp)->node != node)
        chp = &(*chp)->next;
    if (*chp) {
        dav_node_list_item *tofree = *chp;
        *chp = (*chp)->next;
        free(tofree);
        nchanged--;
    }
}

/* Removes a node from the hash table. The root node can not be removed. */
static void
remove_from_table(dav_node *node)
{
    if (node == root)
        return;

    size_t i = ((size_t) node / alignment) % table_size;
    dav_node **np = &table[i];
    while (*np && *np != node)
        np = &(*np)->table_next;
    if (*np)
        *np = (*np)->table_next;
}


/* Removes a node from the directory tree. The root node can not be removed.
   If node is a directory, member nref of the parent will be decremented.
   But no other attributes of the parent directory will be changed. */
static void
remove_from_tree(dav_node *node)
{
    if (node == root)
        return;

    dav_node **np = &node->parent->childs;
    while (*np && *np != node)
        np = &(*np)->next;
    if (*np) {
        *np = node->next;
        if (is_dir(node))
            --node->parent->nref;
    }
}


/* Frees locks, removes the node from the tree and from the hash table,
   and deletes it.
   Depending on the kind of node and its state additional action will be taken:
   - For directories the complete tree below is removed too.
   - If a regular file is dirty, open for writing or created, a backup in
     driectory backup will be created, that holds the cached local copy of the
     file.
   - If a file is open, it will not be removed from the hash table to allow
     proper closing of open file descriptors. */
static void
remove_node(dav_node *node)
{
    remove_from_tree(node);

    if (is_dir(node)) {

        while (node->childs != NULL)
            remove_node(node->childs);
        remove_from_table(node);
        delete_node(node);

    } else {

        remove_from_changed(node);

        if (is_locked(node))
            dav_unlock(node->path, &node->lock_expire);

        if (is_dirty(node) || is_open_write(node) || is_created(node))
            backup_node(node);

        if (is_open(node)) {
            if (debug)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                       "invalidating node %p", node);
            node->parent = NULL;
            node->dirty = 0;
            node->remote_exists = 0;
        } else {
            remove_from_table(node);
            delete_node(node);
        }

    }

}


/* Gets a property list from the server and updates node dir and its childs
   accordingly.
   If there are inconsistencies between the information from the server and
   the locally stored state, the local information is updated. Backups are
   created if necessary.
   This will only be done if the utime of dear is reached, otherwise this
   function will do nothing.
   utime and retry will be updated.
   If the contents or the mtime of the dir has changed, the dir-cache-file
   will be deleted. */
static int
update_directory(dav_node *dir, time_t refresh)
{
    if (dir == backup || time(NULL) <= (dir->utime + refresh))
        return 0;

    dav_props *props = NULL;
    int ret = dav_get_collection(dir->path, &props);

    dir->utime = time(NULL);
    if (ret) {
        if (retry == dir_refresh) {
            retry = min_retry;
        } else {
            retry *= 2;
            retry = (retry > max_retry) ? max_retry : retry;
            retry = (retry == dir_refresh) ? (retry + 1) : retry;
        }
        return ret;
    } else {
        retry = dir_refresh;
    }

    int changed = 0;
    dav_node *child = dir->childs;
    while (child) {
        dav_node *next = child->next;
        if (!is_backup(child)) {
            dav_props **pp = &props;
            while (*pp && strcmp((*pp)->path, child->path) != 0)
                pp = &(*pp)->next;
            if (*pp) {
                dav_props *p = *pp;
                *pp = p->next;
                changed |= update_node(child, p);
            } else if (!is_created(child)) {
                remove_node(child);
                changed = 1;
            }
        }
        child = next;
    }

    while (props) {
        dav_props *next = props->next;
        if (strlen(props->name) > 0) {
            add_node(dir, props);
            changed = 1;
        } else {
            if (props->mtime > dir->smtime) {
                dir->smtime = props->mtime;
                dir->mtime = props->mtime;
            }
            if (dir->mtime > dir->ctime)
                dir->ctime = dir->mtime;
            if (dir->etag)
                free(dir->etag);
            dir->etag = props->etag;
            props->etag = NULL;
            dav_delete_props(props);
        }
        props = next;
    }

    if (changed) {
        delete_cache_file(dir);
    }

    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "directory updated: %p->%p",
               dir->parent, dir);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  %s", dir->path);
    }
    return 0;
}


/* Updates the properties of node according to props and frees props.
   If props is incompatibel with node or indicates a lost update problem,
   a new node is created from props and the old node is deleted, creating
   a local back up if necessary.
   node  : The node to be updated. It must not be the root node and have a
           valid parent.
   props : The properties retrieved from the server. They will be freed.
   return value: Value 1 indicates that the contents of the parent directory
                 has changed and therefore the parents dir-cache-file has to
                 be updated; 0 otherwise.
   NOTE: node may be removed and the pointer node may become invalid. */
static int
update_node(dav_node *node, dav_props *props)
{
    if (debug) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "updating node: %p->%p",
               node->parent, node);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  %s", node->path);
    }

    if (!node->parent)
        return 0;
    int ret = 0;

    if ((is_dir(node) && !props->is_dir)
            || (!is_dir(node) && props->is_dir)) {
        add_node(node->parent, props);
        remove_node(node);
        return 1;
    }

    if (strcmp(node->name, props->name) != 0) {
        free(node->name);
        node->name = ne_strdup(props->name);
        ret = 1;
    }

    if (is_created(node)) {
        if (!is_open(node) && (props->size > 0)) {
            add_node(node->parent, props);
            remove_node(node);
            return 1;
        } else {
            dav_delete_props(props);
            return ret;
        }
    }

    if (is_cached(node)) {
        if ((!props->etag && props->mtime > node->smtime)
                || (node->etag && props->etag
                    && strcmp(node->etag, props->etag) != 0)) {
            if (is_open(node)) {
                node->utime = 0;
                dav_delete_props(props);
                return ret;
            } else if (is_dirty(node)) {
                add_node(node->parent, props);
                remove_node(node);
                return 1;
            } else {
                delete_cache_file(node);
            }
        } else {
            node->utime = time(NULL);
            dav_delete_props(props);
            return ret;
        }
    }

    if (props->mtime > node->atime)
        node->atime = props->mtime;
    if (props->mtime > node->smtime) {
        node->mtime = props->mtime;
        node->smtime = props->mtime;
        node->utime = 0;
        delete_cache_file(node);
    }

    if (node->etag)
        free(node->etag);
    node->etag = props->etag;
    props->etag = NULL;

    if (is_reg(node)) {
        if (props->is_exec == 1
                && !(node->mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            node->mode |= (node->mode & S_IWUSR) ? S_IXUSR : 0;
            node->mode |= (node->mode & S_IWGRP) ? S_IXGRP : 0;
            node->mode |= (node->mode & S_IWOTH) ? S_IXOTH : 0;
        } else if (props->is_exec == 0
                && (node->mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            node->mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
        }
        if (props->size && props->size != node->size) {
            node->size = props->size;
        }
    }

    dav_delete_props(props);

    return ret;
}


/* For node and all nodes in the tree below nodethe compinent src_path in its
   path will be replaced by dst_path. If the path of a node does not start with
   src_path the node will *not* be removed, but its parent directory will be
   invalidated, so an update is forced. */
static void
update_path(dav_node *node, const char *src_path, const char *dst_path)
{
    dav_node *n = node->childs;
    while (n) {
        update_path(n, src_path, dst_path);
        n = n->next;
    }

    if (!node->path || strstr(node->path, src_path) != node->path) {
        delete_cache_file(node->parent);
        node->parent->utime = 0;
        return;
    }

    char *path = ne_concat(dst_path, node->path + strlen(src_path), NULL);
    free(node->path);
    node->path = path;
}


/* Get information about node. */

static int
exists(const dav_node *node)
{
    size_t i = ((size_t) node / alignment) % table_size;
    dav_node *n = table[i];
    while (n && n != node)
        n = n->table_next;

    if (n) {
        return 1;
    } else {
        return 0;
    }
}


static dav_handle *
get_file_handle(dav_node * node, int fd, int accmode, pid_t pid, pid_t pgid)
{
    dav_handle *fh = node->handles;
    if (fd) {
        while (fh && fh->fd != fd)
            fh = fh->next;
    } else {
        while (fh && (fh->flags != accmode || fh->pid != pid))
            fh = fh->next;
        if (!fh) {
            fh = node->handles;
            while (fh && (fh->flags != accmode || fh->pgid != pgid))
                fh = fh->next;
        }
    }

    return fh;
}


/* Checks whether user uid has access to node according to how.
   In any case the user must have execute permission for the parent of node
   and all of its parents up to the root node.
   int how : How to acces the node. May be any combination of R_OK, W_OK, X_OK
             and F_OK.
   return value: 1 access is allowed.
                 0 access is denied. */
static int
has_permission(const dav_node *node, uid_t uid, int how)
{
    if (uid == 0)
        return 1;

    if (node->parent && !has_permission(node->parent, uid, X_OK))
        return 0;

    mode_t a_mode = (how & R_OK) ? (S_IRUSR | S_IRGRP | S_IROTH) : 0;
    a_mode |= (how & W_OK) ? (S_IWUSR | S_IWGRP | S_IWOTH) : 0;
    a_mode |= (how & X_OK) ? (S_IXUSR | S_IXGRP | S_IXOTH) : 0;

    if (node->uid == uid) {
        if (~node->mode & S_IRWXU & a_mode)
            return 0;
        return 1;
    }

    struct passwd *pw = getpwuid(uid);
    if (!pw)
        return 0;
    if (pw->pw_gid == node->gid) {
        if (~node->mode & S_IRWXG & a_mode)
            return 0;
        return 1;
    }

    struct group *grp = getgrgid(node->gid);
    if (!grp)
        return 0;
    char **members = grp->gr_mem;
    while (*members && strcmp(*members, pw->pw_name) != 0)
        members++;
    if (*members) {
        if (~node->mode & S_IRWXG & a_mode)
            return 0;
        return 1;
    }
    
    if (!(~node->mode & S_IRWXO & a_mode))
        return 1;
    return 0;
}


/* A node is considered busy if it is open for writing or, in case of a
   directory, if in the tree below the node there is any file open for write.
   return value : 1 if busy, 0 if not. */
static int
is_busy(const dav_node *node)
{
    dav_node *child = node->childs;
    while (child) {
        if (is_busy(child))
            return 1;
        child = child->next;
    }

    return (is_reg(node) && is_open_write(node));
}


/* Checks whether node exists and is valid. The parent directory is
   updated if necessary. */
static int
is_valid(const dav_node *node)
{
    if (!exists(node) || (!node->parent && node != root))
        return 0;

    if (node == root || node == backup)
        return 1;

    update_directory(node->parent, retry);
    if (!exists(node) || (!node->parent && node != root))
        return 0;

    return 1;
}


/* Cache file functions. */

static void
close_fh(dav_node *node, dav_handle *fh)
{
    close(fh->fd);

    dav_handle **fhp = &node->handles;
    while (*fhp && *fhp != fh)
        fhp = &(*fhp)->next;
    if (*fhp)
        *fhp = (*fhp)->next;
    free(fh);
}


/* It creates a new empty cache file for node. If a cache file already exists,
   it does nothing.
   return value : 0 on success, EIO if no cache file could be created. */
static int
create_cache_file(dav_node *node)
{
    if (node->cache_path) {
        if (access(node->cache_path, F_OK) == 0) {
            return 0;
        } else {
            free(node->cache_path);
        }
    }

    node->cache_path = ne_concat(cache_dir, "/", node->name, "-XXXXXX", NULL);

    int fd = mkstemp(node->cache_path);
    if (fd <= 0) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("can't create cache file %s"), node->cache_path);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "%s", strerror(errno));
        free(node->cache_path);
        node->cache_path = NULL;
        return EIO;
    }

    close(fd);
    return 0;
}


/* Creates a file in the cache that holds dir-entries of directory dir.
   dir will be updated if necessary.
   If this file already exists, it does nothing.
   To write the dir-entries it calls the write_dir_entry function of the
   kernel interface.
   return value : 0 on success, EIO if no cache file could be created. */
static int
create_dir_cache_file(dav_node *dir)
{
    if (dir->cache_path) {
        if (access(dir->cache_path, F_OK) == 0) {
            return 0;
        } else {
            free(dir->cache_path);
        }
    }

    dir->cache_path = ne_concat(cache_dir, "/dir-", dir->name, "-XXXXXX",
                                 NULL);
    int fd = mkstemp(dir->cache_path);
    if (fd <= 0) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("can't create cache file %s"), dir->cache_path);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "%s", strerror(errno));
        free(dir->cache_path);
        dir->cache_path = NULL;
        return EIO;
    }

    off_t size = write_dir(dir, fd);
    close(fd);

    if (size > 0) {
        dir->size = size;
        return 0;
    } else {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("error writing directory %s"), dir->cache_path);
        remove(dir->cache_path);
        free(dir->cache_path);
        dir->cache_path = NULL;
        return EIO;
    }
}


/* Opens the cache file of node using flags and stores the file descriptor
   in fd. A new structure dav_handle is created and added to the list
   of handles.
   return value : 0 on success, EIO if the file could not be opend. */
static int
open_file(int *fd, dav_node *node, int flags, pid_t pid, pid_t pgid, uid_t uid)
{
    *fd = open(node->cache_path, flags, node->mode);
    if (*fd <= 0)
        return EIO;
    dav_handle *fh = (dav_handle *) ne_malloc(sizeof(dav_handle));
    fh->fd = *fd;
    fh->flags = O_ACCMODE & flags;
    fh->pid = pid;
    fh->pgid = pgid;
    fh->uid = uid;
    fh->next = node->handles;
    node->handles = fh;
    if ((O_ACCMODE & flags) == O_WRONLY || (O_ACCMODE & flags) == O_RDWR)
    		node->dirty = 1;

    return 0;
}

 
/* Updates the cached file from the server if necessary and possible, or
   retrieves one from the server if no cache file exists.
   It is not necessary or possible if
   - node is in directory backup
   - node is created (does not yet exist on the server)
   - node is open for writing
   - it has been updated within the last second
   - node is dirty.
   If the node is dirty but not open for write, it will be stored back on
   the server. */
static int
update_cache_file(dav_node *node)
{
    if (is_backup(node) || is_created(node) || is_open_write(node)
            || (is_dirty(node) && is_locked(node))) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  no update");
        return 0;
    }

    int ret = 0;

    if (is_dirty(node)) {
        if (get_upload_time(node) >= time(NULL))
            return 0;
        ret = dav_put(node->path, node->cache_path, &node->remote_exists,
                      &node->lock_expire, &node->etag, &node->smtime, -1);
        if (!ret) {
            node->utime = time(NULL);
            node->dirty = 0;
            remove_from_changed(node);
        } else if (ret == EACCES || ret == EINVAL || ret == ENOENT
                   || ret == EPERM || ret == ENOSPC) {
            delete_cache_file(node->parent);
            node->parent->utime = 0;
            remove_node(node);
            ret = EIO;
        }
        return ret;
    }

    if (gui_optimize && is_cached(node)
    				&& time(NULL) > (node->utime +file_refresh)) {
        update_directory(node->parent, file_refresh);
        if (!exists(node) || node->parent == NULL)
            return ENOENT;
    }

    if (is_cached(node) && access(node->cache_path, F_OK) == 0) {
        if (time(NULL) <= (node->utime + file_refresh))
            return 0;
        int modified = 0;
        off_t old_size = node->size;
        ret = dav_get_file(node->path, node->cache_path, &node->size,
                           &node->etag, &node->smtime, &modified);
        if (!ret) {
            if (modified) {
                node->mtime = node->smtime;
                node->atime = node->smtime;
                set_cache_file_times(node);
            }
            node->utime = time(NULL);
            cache_size += node->size - old_size;
        }
    } else {
        if (create_cache_file(node) != 0)
            return EIO;
        time_t smtime = 0;
        char *etag = NULL;
        ret = dav_get_file(node->path, node->cache_path, &node->size, &etag,
                           &smtime, NULL);
        if (!ret) {
            if (node->etag) free(node->etag);
            node->etag = etag;
            if (smtime) {
                node->atime = smtime;
                node->mtime = smtime;
                node->smtime = smtime;
            }
            node->utime = time(NULL);
            set_cache_file_times(node);
            cache_size += node->size;
        } else {
            if (ret == ENOENT) {
                delete_cache_file(node->parent);
                node->parent->utime = 0;
                remove_node(node);
            }
            delete_cache_file(node);
            if (etag) free(etag);
        }
    }

    return ret;
}


static off_t
write_dir(dav_node *dir, int fd)
{
    off_t size = write_dir_entry(fd, 0, dir, ".");
    if (size > 0 && dir->parent != NULL)
        size = write_dir_entry(fd, size, dir->parent, "..");
    dav_node *node = dir->childs;
    while (size > 0 && node) {
        size = write_dir_entry(fd, size, node, node->name);
        node = node->next;
    }

    if (size > 0)
        size = write_dir_entry(fd, size, NULL, NULL);

    return size;
}


/* Permanent cache maintenance. */

/* Checks whether there is an cache directory for the server host, path path,
   mountpoint mpoint and the default_user in the top level cache directory dir.
   If not it will create one. In case of an error it will print an error
   message and terminate the program.
   dir    : The top level cache directory.
   host   : Domain name of the server.
   path   : Path of the resource onthe server.
   mpoint : Mount point. */
static void
check_cache_dir(const char *dir, const char *host, const char *path,
                const char *mpoint)
{
    struct passwd *pw = getpwuid(default_uid);
    if (!pw || !pw->pw_name)
        ERR(_("can't read user data base"));
    char *dir_name = ne_concat(host, path, mpoint + 1, "+", pw->pw_name, NULL);
    *(dir_name + strlen(host) + strlen(path) - 1) = '+';
    char *pos = strchr(dir_name, '/');
    while (pos) {
        *pos = '-';
        pos = strchr(pos, '/');
    }

    DIR *tl_dir = opendir(dir);
    if (!tl_dir)
        ERR(_("can't open cache directory %s"), dir);

    struct dirent *de = readdir(tl_dir);
    while (de && !cache_dir) {
        if (strcmp(de->d_name, dir_name) == 0) {
            cache_dir = ne_concat(dir, "/", de->d_name, NULL);
        }
        de = readdir(tl_dir);
    }

    closedir(tl_dir);

    if (!cache_dir) {
        cache_dir = ne_concat(dir, "/", dir_name, NULL);
        if (mkdir(cache_dir, S_IRWXU) != 0)
            ERR(_("can't create cache directory %s"),
            cache_dir);
    }
    free(dir_name);

    struct stat st;
    if (stat(cache_dir, &st) != 0)
        ERR(_("can't access cache directory %s"), cache_dir);
    if (st.st_uid != geteuid())
        ERR(_("wrong owner of cache directory %s"), cache_dir);
    if ((DAV_A_MASK & st.st_mode) != S_IRWXU)
        ERR(_("wrong permissions set for cache directory %s"), cache_dir);
}


/* Searches cache for orphaned files and puts them into backup. */
static void
clean_cache(void)
{
    DIR *dir = opendir(cache_dir);
    if (!dir)
        return;

    struct dirent *de = readdir(dir);
    while (de) {

        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0
                && strcmp(de->d_name, DAV_INDEX) != 0) {
            char *path = ne_concat(cache_dir, "/", de->d_name, NULL);
            int i = 0;
            dav_node *node = NULL;
            while (!node && i < table_size) {
                node = table[i];
                while (node && (!is_reg(node) || !node->cache_path
                                || strcmp(path, node->cache_path) != 0)) {
                    node = node->table_next;
                }
                i++;
            }
            if (!node) {
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
                                   _("found orphaned file in cache:"));
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), "  %s", path);
                dav_node *found = new_node(backup, default_file_mode);
                found->mode &= ~(S_IRWXG | S_IRWXO);
                found->cache_path = path;
                found->name = ne_strdup(de->d_name);
                attr_from_cache_file(found);
                backup->mtime = time(NULL);
                backup->ctime = backup->mtime;
            } else {
                free(path);
            }
        }

        de = readdir(dir);
    }

    closedir(dir);
}


/* Reads the index file of the cache and creates a tree of nodes from the
   XML data in the index file. Will be called when the cache module is 
   initialized. The root node must already exist.
   If an error occurs all nodes created up to this will be deleted. */
static void
parse_index(void)
{
    char *index = ne_concat(cache_dir, "/", DAV_INDEX, NULL);
    FILE *idx = fopen(index, "r");
    if (!idx) {
        free(index);
        return;
    }

    char *buf = ne_malloc(DAV_XML_BUF_SIZE);
    size_t len = fread(buf, 1, DAV_XML_BUF_SIZE, idx);
    if (len <= 0) {
        free(buf);
        fclose(idx);
        free(index);
        return;
    }

    dav_node *node = root;
    ne_xml_parser *parser = ne_xml_create();
    ne_xml_push_handler(parser, xml_start_root, NULL, xml_end_root, &node);
    ne_xml_push_handler(parser, xml_start_backup, NULL, xml_end_backup, &node);
    ne_xml_push_handler(parser, xml_start_dir, NULL, xml_end_dir, &node);
    ne_xml_push_handler(parser, xml_start_reg, NULL, xml_end_reg, &node);
    ne_xml_push_handler(parser, xml_start_date, xml_cdata, xml_end_date, &node);
    ne_xml_push_handler(parser, xml_start_decimal, xml_cdata, xml_end_decimal,
                        &node);
    ne_xml_push_handler(parser, xml_start_mode, xml_cdata, xml_end_mode, &node);
    ne_xml_push_handler(parser, xml_start_size, xml_cdata, xml_end_size, &node);
    if (strstr(buf, "<?xml version=\"1.0\" encoding=\"utf-8\" ?>") == buf) {
        ne_xml_push_handler(parser, xml_start_string, xml_cdata, xml_end_string,
                            &node);
    } else {
        ne_xml_push_handler(parser, xml_start_string, xml_cdata,
                            xml_end_string_old, &node);
    }

    int ret = 0;
    while (len > 0 && !ret) {
        ret = ne_xml_parse(parser, buf, len);
        len = fread(buf, 1, DAV_XML_BUF_SIZE, idx);
    }

    ret = ne_xml_parse(parser, buf, 0);
    free(buf);

    if (ret) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("error parsing %s"), index);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("  at line %i"),
               ne_xml_currentline(parser));
        delete_tree(root);
    }

    ne_xml_destroy(parser);
    fclose(idx);
    free(index);
}


/* Removes the files with lowest access time from cache until the cache size
   is smaller than max_cache_size or there are no more files that may be
   removed. */
static void
resize_cache(void)
{
    int free_more_cache = 0;
    int last = 0;

    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "resize cache: %llu of %llu MiBytes used.",
               (cache_size + 0x80000) / 0x100000,
               (max_cache_size + 0x80000) / 0x100000);

try_once_more:
    while (1) {
        dav_node *least_recent = NULL;
        cache_size = 0;
        size_t i;
        for (i = 0; i < table_size; i++) {
            dav_node *node = table[i];
            while (node) {
                if (is_cached(node)) {
                    if (!is_open(node)
                            && (free_more_cache || !is_dirty(node))
                            && (free_more_cache || !is_backup(node))
                            && (!least_recent
                                || node->atime < least_recent->atime))
                        least_recent = node;
                    cache_size += node->size;
                }
                node = node->table_next;
            }
        }
        if (cache_size < max_cache_size)
            break;
        if (!least_recent) {
            unsigned long long oversize;
            free_more_cache = 1;

            oversize = (cache_size - max_cache_size + 0x80000) / 0x100000;
            if (oversize)
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
                       _("open files exceed max cache size by %llu MiBytes"),
                       (cache_size - max_cache_size + 0x80000) / 0x100000);
            break;
        }
        delete_cache_file(least_recent);
        cache_size -= least_recent->size;
    }

    if (free_more_cache && !last) {
        last = 1;
        goto try_once_more;
    }

    if (debug)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "              %llu of %llu MiBytes used.",
               (cache_size + 0x80000) / 0x100000,
               (max_cache_size + 0x80000) / 0x100000);
    return;
}


/* Creates an entry for node in the index file file, removes the node from
   the tree and the hash table and deletes the node. The entry will be
   indented by indent to get proper alignment of nested entries.
   node   : the node.
   file   : the index file for this cache directory.
   indent : a string of spaces to indent the entry.
   return value : 0 on success, -1 if an error occurs. */
static int
write_node(dav_node *node, FILE *file, const char *indent)
{
    if (node == root) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "writing root %p", node);
        if (fprintf(file, "<d:%s xmlns:d=\"%s\">\n", type[ROOT],
                    DAV_XML_NS) < 0)
            return -1;
    } else if (node == backup) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "writing backup %p",
                   node);
        if (fprintf(file, "%s<d:%s>\n", indent, type[BACKUP]) < 0)
            return -1;
    } else if (is_dir(node)) {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "writing directory %p",
                   node);
        if (fprintf(file, "%s<d:%s>\n", indent, type[DDIR]) < 0)
            return -1;
    } else {
        if (debug)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "writing file %p", node);
        if (fprintf(file, "%s<d:%s>\n", indent, type[REG]) < 0)
            return -1;
    }

    char *ind = ne_concat(indent, "  ", NULL);

    if (node != root && !is_backup(node)) {
        if (fprintf(file, "%s<d:%s><![CDATA[%s]]></d:%s>\n", ind, type[PATH], node->path,
                    type[PATH]) < 0)
            return -1;
    }

    if (node != root && node != backup) {
        char *name = dav_conv_to_utf_8(node->name);
        int ret = fprintf(file, "%s<d:%s><![CDATA[%s]]></d:%s>\n", ind, type[NAME], name,
                          type[NAME]);
        free(name);
        if (ret < 0)
            return -1;
    }

    if (is_reg(node) && node->cache_path != NULL) {
        char *path = dav_conv_to_utf_8(node->cache_path);
        int ret = fprintf(file, "%s<d:%s><![CDATA[%s]]></d:%s>\n", ind, type[CACHE_PATH],
                          path, type[CACHE_PATH]);
        free(path);
        if (ret < 0)
            return -1;
    }

    if (is_reg(node) && !is_backup(node) && node->etag != NULL) {
        if (fprintf(file, "%s<d:%s><![CDATA[%s]]></d:%s>\n", ind, type[ETAG], node->etag,
                    type[ETAG]) < 0)
        return -1;
    }

    if (is_reg(node)) {
        if (fprintf(file, "%s<d:%s>%lli</d:%s>\n", ind, type[SIZE],
            (long long int) node->size, type[SIZE]) < 0)
            return -1;
    }

    char t[64];
    struct tm *lt = localtime(&node->atime);
    strftime(t, 64, "(%FT%T%z)", lt);
    if (fprintf(file, "%s<d:%s>%li%s</d:%s>\n", ind, type[ATIME], node->atime,
                t, type[ATIME]) < 0)
        return -1;

    lt = localtime(&node->mtime);
    strftime(t, 64, "(%FT%T%z)", lt);
    if (fprintf(file, "%s<d:%s>%li%s</d:%s>\n", ind, type[MTIME], node->mtime,
                t, type[MTIME]) < 0)
        return -1;

    lt = localtime(&node->ctime);
    strftime(t, 64, "(%FT%T%z)", lt);
    if (fprintf(file, "%s<d:%s>%li%s</d:%s>\n", ind, type[CTIME], node->ctime,
                t, type[CTIME]) < 0)
        return -1;

    if (is_reg(node) && !is_backup(node)) {
        lt = localtime(&node->smtime);
        strftime(t, 64, "(%FT%T%z)", lt);
        if (fprintf(file, "%s<d:%s>%li%s</d:%s>\n", ind, type[SMTIME],
                    node->smtime, t, type[SMTIME]) < 0)
            return -1;
    }

    if (is_reg(node) && !is_backup(node)
            && (is_dirty(node) || is_created(node))) {
        lt = localtime(&node->lock_expire);
        strftime(t, 64, "(%FT%T%z)", lt);
        if (fprintf(file, "%s<d:%s>%li%s</d:%s>\n", ind, type[LOCK_EXPIRE],
                    node->lock_expire, t, type[LOCK_EXPIRE]) < 0)
            return -1;
        if (fprintf(file, "%s<d:%s>%i</d:%s>\n", ind, type[REMOTE_EXISTS],
                    node->remote_exists, type[REMOTE_EXISTS]) < 0)
            return -1;
        if (fprintf(file, "%s<d:%s>%i</d:%s>\n", ind, type[DIRTY], node->dirty,
                    type[DIRTY]) < 0)
            return -1;
    }

    if (node != backup) {
        if (fprintf(file, "%s<d:%s>%o</d:%s>\n", ind, type[MODE], node->mode,
                    type[MODE]) < 0)
            return -1;
    }

    if (node != root && node != backup) {
        if (fprintf(file, "%s<d:%s>%i</d:%s>\n", ind, type[UID], node->uid,
                    type[UID]) < 0)
            return -1;
        if (fprintf(file, "%s<d:%s>%i</d:%s>\n", ind, type[GID], node->gid,
                    type[GID]) < 0)
            return -1;
    }

    dav_node *child = node->childs;
    while (child != NULL) {
        if (write_node(child, file, ind) < 0)
            return -1;
        child = child->next;
    }

    if (node == root) {
        if (fprintf(file, "%s</d:%s>\n", indent, type[ROOT]) < 0)
            return -1;
    } else if (node == backup) {
        if (fprintf(file, "%s</d:%s>\n", indent, type[BACKUP]) < 0)
            return -1;
    } else if (is_dir(node)) {
        if (fprintf(file, "%s</d:%s>\n", indent, type[DDIR]) < 0)
            return -1;
    } else {
        if (fprintf(file, "%s</d:%s>\n", indent, type[REG]) < 0)
            return -1;
    }

    free(ind);
    return 0;
}


/* Concatenate data from subsequent callbacks into xml_data. */
static int
xml_cdata(void *userdata, int state, const char *cdata, size_t len)
{
    if (!xml_data) {
        xml_data = ne_strndup(cdata, len);
    } else {
        char *add = ne_strndup(cdata, len);
        char *new = ne_concat(xml_data, add, NULL);
        free(add);
        free(xml_data);
        xml_data = new;
    }

    return 0;
}


/* Finishes the creation of directory backup.
   userdata is set to the parent of backup.
   return value : allways 0. */
static int
xml_end_backup(void *userdata, int state, const char *nspace, const char *name)
{
    dav_node *dir = *((dav_node **) userdata);
    *((dav_node **) userdata) = dir->parent;

    if (dir->path) {
        free(dir->path);
        dir->path = NULL;
    }
    if (dir->name) {
        free(dir->name);
        dir->name = NULL;
    }
    delete_cache_file(dir);
    if (dir->etag) {
        free(dir->etag);
        dir->etag = NULL;
    }
    dir->smtime = 0;

    return 0;
}


/* xml_data must be a string representing a value as a decimal number.
   Its value is assigned to the appropriate member of node userdata.
   state indacates the member of node.
   return value : 0 on success, -1 if an error occurs. */
static int
xml_end_date(void *userdata, int state, const char *nspace, const char *name)
{
    if (!xml_data)
        return -1;

    char *tail;
    time_t t = strtol(xml_data, &tail, 10);
    if (*tail != '\0' && *tail != '(') {
        free(xml_data);
        xml_data = NULL;
        return -1;
    }
    free(xml_data);
    xml_data = NULL;

    switch (state) {
    case ATIME:
        (*((dav_node **) userdata))->atime = t;
        break;
    case MTIME:
        (*((dav_node **) userdata))->mtime = t;
        break;
    case CTIME:
        (*((dav_node **) userdata))->ctime = t;
        break;
    case SMTIME:
        (*((dav_node **) userdata))->smtime = t;
        break;
    case LOCK_EXPIRE:
        (*((dav_node **) userdata))->lock_expire = t;
        break;
    default:
        return -1;
    }

    return 0;
}


/* xml_dat must be a string representation of a decimal number. Its value is
   assigned to the appropriate member of node userdata. state indacates the
   member of node.
   return value : 0 on success, -1 if an error occurs. */
static int
xml_end_decimal(void *userdata, int state, const char *nspace,
                const char *name)
{
    if (!xml_data)
        return -1;

    char *tail;
    long int n = strtol(xml_data, &tail, 10);
    if (*tail != '\0') {
        free(xml_data);
        xml_data = NULL;
        return -1;
    }
    free(xml_data);
    xml_data = NULL;

    switch (state) {
    case UID:
        (*((dav_node **) userdata))->uid = n;
        break;
    case GID:
        (*((dav_node **) userdata))->gid = n;
        break;
    case DIRTY:
        (*((dav_node **) userdata))->dirty = n;
        break;
    case REMOTE_EXISTS:
        (*((dav_node **) userdata))->remote_exists = n;
        break;
    default:
        return -1;
    }

    return 0;
}


/* Finishes the creation of a directory. Members name and path of the
   not must not be NULL, or the direcotry tree will be deleted.
   userdata is set to the parent of the directory.
   return value : allways 0. */
static int
xml_end_dir(void *userdata, int state, const char *nspace, const char *name)
{
    dav_node *dir = *((dav_node **) userdata);
    *((dav_node **) userdata) = dir->parent;

    if (!dir->name || !dir->path) {
        delete_tree(dir);
        return 0;
    }

    delete_cache_file(dir);
    if (dir->etag) {
        free(dir->etag);
        dir->etag = NULL;
    }
    dir->size = 0;
    dir->smtime = 0;

    return 0;
}


/* xml_data must be a string representation of an octal number. Its value is
   assigned to member mode of node userdata.
   return value : 0 on success, -1 if an error occurs. */
static int
xml_end_mode(void *userdata, int state, const char *nspace, const char *name)
{
    if (!xml_data)
        return -1;

    char *tail;
    (*((dav_node **) userdata))->mode = strtol(xml_data, &tail, 8);
    if (*tail != '\0') {
        free(xml_data);
        xml_data = NULL;
        return -1;
    }
    free(xml_data);
    xml_data = NULL;

    return 0;
}


/* Finishes the creation of a node of a regular file. Members path, name
   and cach_path must not be NULL and the cache file must exist, or the node
   will be deleted.
   If the file is in directory backup, member path may be NULL.
   userdata is set to the parent of the file node.
   return value : allways 0. */
static int
xml_end_reg(void *userdata, int state, const char *nspace, const char *name)
{
    dav_node *reg = *((dav_node **) userdata);
    *((dav_node **) userdata) = reg->parent;

    struct stat st;
    if (!reg->name || !reg->cache_path || stat(reg->cache_path, &st) != 0
            || reg->size != st.st_size || (!reg->path && !is_backup(reg))) {
        if (reg->cache_path) {
            free(reg->cache_path);
            reg->cache_path = NULL;
        }
        delete_tree(reg);
        return 0;
    }

    cache_size += reg->size;

    if (is_backup(reg)) {
        if (reg->path) {
            free(reg->path);
            reg->path = NULL;
        }
        if (reg->etag) {
            free(reg->etag);
            reg->etag = NULL;
        }
        reg->smtime = 0;
    } else if (is_dirty(reg) || is_created(reg)) {
        add_to_changed(reg);
        set_upload_time(reg);
    }

    return 0;
}


/* Finishes the creation of the root directory. userdata must be equal to root,
   or the complete tree will be deleted.
   Members path, name, cache_path and etag will be NULL.
   return value : allways 0. */
static int
xml_end_root(void *userdata, int state, const char *nspace, const char *name)
{
    dav_node *dir = *((dav_node **) userdata);

    if (dir != root)
        delete_tree(dir);

    if (dir->path) {
        free(dir->path);
        dir->path = NULL;
    }
    if (dir->name) {
        free(dir->name);
        dir->name = NULL;
    }
    delete_cache_file(dir);
    if (dir->etag) {
        free(dir->etag);
        dir->etag = NULL;
    }
    dir->size = 0;
    dir->smtime = 0;

    return 0;
}


/* xml_data must be a string representation of a decimal number representing
   a file size. Its value is assigned to member size of the node.
   return value : 0 on success, -1 if an error occurs. */
static int
xml_end_size(void *userdata, int state, const char *nspace, const char *name)
{
    if (!xml_data)
        return -1;

    char *tail;

#if _FILE_OFFSET_BITS == 64
    (*((dav_node **) userdata))->size = strtoll(xml_data, &tail, 10);
#else
    (*((dav_node **) userdata))->size = strtol(xml_data, &tail, 10);
#endif

    if (*tail != '\0') {
        free(xml_data);
        xml_data = NULL;
        return -1;
    }
    free(xml_data);
    xml_data = NULL;

    return 0;
}


/* Stores xml_data in the appropriate member of node userdata.
   state indicates the member of node.
   return value : 0 on success, -1 if an error occurs. */
static int
xml_end_string(void *userdata, int state, const char *nspace, const char *name)
{
    if (!xml_data)
        return -1;

    switch (state) {
    case PATH:
        (*((dav_node **) userdata))->path = xml_data;
        break;
    case NAME:
        (*((dav_node **) userdata))->name = dav_conv_from_utf_8(xml_data);
        free(xml_data);
        break;
    case CACHE_PATH:
        (*((dav_node **) userdata))->cache_path = dav_conv_from_utf_8(xml_data);
        free(xml_data);
        break;
    case ETAG:
        (*((dav_node **) userdata))->etag = xml_data;
        break;
    default:
        free(xml_data);
        xml_data = NULL;
        return -1;
    }

    xml_data = NULL;
    return 0;
}


/* Stores xml_data in the appropriate member of node userdata.
   state indicates the member of node.
   return value : 0 on success, -1 if an error occurs. */
static int
xml_end_string_old(void *userdata, int state, const char *nspace,
                   const char *name)
{
    if (!xml_data)
        return -1;

    switch (state) {
    case PATH:
        (*((dav_node **) userdata))->path = xml_data;
        break;
    case NAME:
        (*((dav_node **) userdata))->name = xml_data;
        break;
    case CACHE_PATH:
        (*((dav_node **) userdata))->cache_path = xml_data;
        break;
    case ETAG:
        (*((dav_node **) userdata))->etag = xml_data;
        break;
    default:
        free(xml_data);
        xml_data = NULL;
        return -1;
    }

    xml_data = NULL;
    return 0;
}


/* Will be called when the start tag of a XML-element is found, and tests
   wheather it is a BACKUP elemt. In this case parent must be ROOT.
   userdata will be set to the newly created node backup and also the global
   variable backup will be set.
   return value : 0 not responsible for this kind of element.
                  BACKUP if it is the backup element
                  -1 XML error, parent is not root. */
static int
xml_start_backup(void *userdata, int parent, const char *nspace,
                 const char *name, const char **atts)
{
    if (strcmp(name, type[BACKUP]) != 0)
        return 0;

    if (parent != ROOT)
        return -1;

    dav_node *dir = new_node(*((dav_node **) userdata), S_IFDIR | S_IRWXU);
    backup = dir;
    *((dav_node **) userdata) = dir;

    return BACKUP;
}


/* Will be called when the start tag of a XML-element is found, and tests
   whether it is an element that contains a date. In this case
   the parent must be either a directory (including root and backup) or
   a file node.
   return value : 0 not responsible for this kind of element.
                  A value that indicates which member of a node the decimal
                  value must be assigned to.
                  -1 XML error, parent must not contain this property. */
static int
xml_start_date(void *userdata, int parent, const char *nspace,
               const char *name, const char **atts)
{
    int ret;
    if (strcmp(name, type[ATIME]) == 0) {
        ret = ATIME;
    } else if (strcmp(name, type[MTIME]) == 0) {
        ret = MTIME;
    } else if (strcmp(name, type[CTIME]) == 0) {
        ret = CTIME;
    } else if (strcmp(name, type[SMTIME]) == 0) {
        ret = SMTIME;
    } else if (strcmp(name, type[LOCK_EXPIRE]) == 0) {
        ret = LOCK_EXPIRE;
    } else {
        return 0;
    }

    if (parent != DDIR && parent != REG && parent != BACKUP && parent != ROOT)
        return -1;

    return ret;
}


/* Will be called when the start tag of a XML-element is found, and tests
   whether it is an element that contains a decimal value. In this case
   the parent must be either a directory (including root and backup) or
   a file node.
   If parent is ROOT or BACKUP the element type must not be UID or GID.
   DIRTY and REMOTE_EXISTS are only allowed for parent REG.
   return value : 0 not responsible for this kind of element.
                  A value that indicates which member of a node the decimal
                  value must be assigned to.
                  -1 XML error, parent must not contain this property. */
static int
xml_start_decimal(void *userdata, int parent, const char *nspace,
                  const char *name, const char **atts)
{
    int ret;
    if (strcmp(name, type[UID]) == 0) {
        ret = UID;
    } else if (strcmp(name, type[GID]) == 0) {
        ret = GID;
    } else if (strcmp(name, type[DIRTY]) == 0) {
        ret = DIRTY;
    } else if (strcmp(name, type[REMOTE_EXISTS]) == 0) {
        ret = REMOTE_EXISTS;
    } else {
        return 0;
    }

    if (parent != DDIR && parent != REG && parent != BACKUP && parent != ROOT)
        return -1;

    if ((parent == BACKUP || parent == ROOT) && (ret == UID || ret == GID))
        return -1;

    if (parent != REG && (ret == DIRTY || ret == REMOTE_EXISTS))
        return -1;

    return ret;
}


/* Will be called when the start tag of a XML-element is found, and tests
   whether it is an directory element. Inthis case parent must be a directory
   (including root but nur backup).
   userdata will be set to the newly created directory node.
   return value : 0 not responsible for this kind of element.
                  DDIR if it is a directory element.
                  -1 XML error, parent must not contain this property. */
static int
xml_start_dir(void *userdata, int parent, const char *nspace,
              const char *name, const char **atts)
{
    if (strcmp(name, type[DDIR]) != 0)
        return 0;

    if (parent != DDIR && parent != ROOT)
        return -1;

    dav_node *dir = new_node(*((dav_node **) userdata), default_dir_mode);
    *((dav_node **) userdata) = dir;

    return DDIR;
}


/* Will be called when the start tag of a XML-element is found, and tests
   whether it is an mode element. In this case parent must be a regular
   node or a directory (including root but not backup).
   return value : 0 not responsible for this kind of element.
                  MODE if it is a mode element.
                  -1 XML error, parent must not contain this property. */
static int
xml_start_mode(void *userdata, int parent, const char *nspace,
               const char *name, const char **atts)
{
    if (strcmp(name, type[MODE]) != 0)
        return 0;

    if (parent != DDIR && parent != REG && parent != ROOT)
        return -1;

    return MODE;
}


/* Will be called when the start tag of a XML-element is found, and tests
   whether it is an element that represents a file node. In this case parent
   must be a directory (including root and backup).
   userdata will be set to the newly created file node.
   return value : 0 not responsible for this kind of element.
                  REG if the element represents a file node.
                  -1 XML error, parent must not contain this property. */
static int
xml_start_reg(void *userdata, int parent, const char *nspace,
              const char *name, const char **atts)
{
    if (strcmp(name, type[REG]) != 0)
        return 0;

    if (parent != DDIR && parent != BACKUP && parent != ROOT)
        return -1;

    dav_node *reg = new_node(*((dav_node **) userdata), default_file_mode);
    *((dav_node **) userdata) = reg;
    if (parent != BACKUP)
        reg->remote_exists = 1;

    return REG;
}


/* Will be called when the start tag of a XML-element is found, and tests
   whether the element represents the root node. In this case parent must be 0.
   return value : 0 not responsible for this kind of element.
                  ROOT if it is the root node element.
                  -1 XML error, root property with parent not equal 0. */
static int
xml_start_root(void *userdata, int parent, const char *nspace,
               const char *name, const char **atts)
{
    if (strcmp(name, type[ROOT]) != 0)
        return 0;

    if (parent != 0)
        return -1;

    return ROOT;
}


/* Will be called when the start tag of a XML-element is found, and tests
   whether it is an element that contains the file size. In this case
   the parent must be a file node.
   return value : 0 not responsible for this kind of element.
                  SIZE if the element represents the file size.
                  value must be assigned to.
                  -1 XML error, parent must not contain this property. */
static int
xml_start_size(void *userdata, int parent, const char *nspace,
               const char *name, const char **atts)
{
    if (strcmp(name, type[SIZE]) != 0) {
        return 0;
    }

    if (parent != REG)
        return -1;

    return SIZE;
}


/* Will be called when the start tag of a XML-element is found, and tests
   whether it is an element that rcontains a string. In this case parent must
   be a directory or a file node.
   return value : 0 not responsible for this kind of element.
                  A value that indicates which member of a node the string must
                  be assigned to.
                  -1 XML error, parent must not contain this property. */
static int
xml_start_string(void *userdata, int parent, const char *nspace,
                 const char *name, const char **atts)
{
    int ret;
    if (strcmp(name, type[PATH]) == 0) {
        ret = PATH;
    } else if (strcmp(name, type[NAME]) == 0) {
        ret = NAME;
    } else if (strcmp(name, type[CACHE_PATH]) == 0) {
        ret = CACHE_PATH;
    } else if (strcmp(name, type[ETAG]) == 0) {
        ret = ETAG;
    } else {
        return 0;
    }

    if (parent != DDIR && parent != REG)
        return -1;

    return ret;
}


/* Auxiliary. */

/* Tries to evaluate the alignment of structure dav_node. It allocates
   dav_node structures and random length strings alternatively and inspects the
   address.
   return value : the alignment (e.g. alignment = 4 means addresses
                  are always multiples of 4 */
static size_t
test_alignment()
{
    srand(time(0));
    size_t align = 64;
    size_t trials = 100;
    char *s[trials];
    dav_node *n[trials];

    size_t j = 0;
    while (align > 0 && j < trials) {
        s[j] = (char *) ne_malloc((rand() / (RAND_MAX / 1024)) % (4 *align));
        n[j] = (dav_node *) ne_malloc(sizeof(dav_node));
        while (align > 0 && ((size_t) n[j] % align) > 0)
            align /= 2;
        ++j;
    }

    for (j = 0; j < trials; j++) {
        if (n[j])
            free(n[j]);
        if (s[j])
            free(s[j]);
    }
    return align;
}

