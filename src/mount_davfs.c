/*  mount_davfs.c: mount the davfs file system.
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

#include <ctype.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <getopt.h>
#include <grp.h>
#ifdef HAVE_LIBINTL_H
#include <libintl.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif
#include <pwd.h>
#include <signal.h>
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
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#ifndef __linux__
#include <sys/sysctl.h>
#endif

#ifdef HAVE_FSTAB_H
#include <fstab.h>
#endif

#include <ne_string.h>
#include <ne_uri.h>
#include <ne_utils.h>

#include "util.h"
#include "defaults.h"
#include "mount_davfs.h"
#include "kernel_interface.h"
#include "cache.h"
#include "webdav.h"

#ifdef ENABLE_NLS
#define _(String) gettext(String)
#else
#define _(String) String
#define textdomain(Domainname)
#define bindtextdomain(Domainname, Dirname)
#endif


#ifdef __FreeBSD__
#define VFS_USERMOUNT "vfs.usermount"
#endif


/* Private global variables */
/*==========================*/

/* The URL of the WebDAV server as taken from commandline.  */
static char *url;

/* The canonicalized mointpoint. */
static char *mpoint;

#ifdef __linux__
/* The file that holds information about mounted filesystems
   (/proc/mounts or /etc/mtab) */

static char *mounts;

#ifndef _PATH_MOUNTED
# define _PATH_MOUNTED "/proc/mounts"
#endif

#endif /* __linux__ */

#ifndef _PATH_FSTAB
# define _PATH_FSTAB "/etc/fstab"
#endif

/* The PID file */
static char *pidfile;

/* This flags signals the message loop of the kernel interface whether it
   shall do what it says or just stop. Will be reset by termination_handler. */
static volatile int keep_on_running = 1;

/* This flag signals that SIGTERM was received. mount.davfs will then
   terminate without uploading dirty files. */
static volatile int got_sigterm;


/* Private function prototypes */
/*=============================*/

/* Parsing, checking and mounting. */

static void
change_persona(dav_args *args);

static void
check_dirs(dav_args *args);

static char *
check_double_mounts(dav_args *args);

static void
check_fstab(const dav_args *args);

static void
check_permissions(dav_args *args);

static int
is_mounted(void);

static dav_args *
parse_commandline(int argc, char *argv[]);

static void
parse_config(dav_args *args);

static void
parse_secrets(dav_args *args);

static int
save_pid(void);

static void
termination_handler(int signo);

static void
write_mtab_entry(const dav_args *args);

/* Helper functions. */

static int
arg_to_int(const char *arg, int base, const char *opt);

static void
cp_file(const char *src, const char *dest);

static int
debug_opts(const char *s);

static int
debug_opts_neon(const char *s);

static void
delete_args(dav_args *args);

static void
get_options(dav_args *args, char *option);

static dav_args *
new_args(void);

static void
log_dbg_config(dav_args *args);

static int
parse_line(char *line, int parmc, char *parmv[]);

static void
proxy_from_env(dav_args *args);

static void
read_config(dav_args *args, const char * filename, int system);

static void
read_no_proxy_list(dav_args *args);

static void
read_secrets(dav_args *args, const char *filename);

static int
split_uri(char **scheme, char **host, int *port,char **path, const char *uri);

static void
usage(void);

static char *
user_input(const char *prompt);


/* Public functions */
/*==================*/

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);

    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), PACKAGE_STRING);

    dav_args *args = parse_commandline(argc, argv);

    if (geteuid() != 0)
        ERR(_("program is not setuid root"));
    if (seteuid(getuid()) != 0)
        ERR(_("can't change effective user id"));

    if (getuid() != 0)
        check_fstab(args);

    parse_config(args);

    check_dirs(args);

    check_permissions(args);

    parse_secrets(args);

    pidfile = check_double_mounts(args);

    change_persona(args);

    dav_init_webdav(args);

    dav_init_cache(args, mpoint);

    int dev = 0;
    size_t buf_size = args->buf_size * 1024;
    dav_init_kernel_interface(&dev, &buf_size, url, mpoint, args);

    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Fork into daemon mode");
    pid_t childpid = fork();
    if (childpid > 0) {

        if (args->debug & DAV_DBG_CONFIG)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                   "Parent: parent pid: %i, child pid: %i", getpid(), childpid);

        if (args->debug & DAV_DBG_CONFIG)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
                   "Parent: writing mtab entry");
        write_mtab_entry(args);

        if (args->debug & DAV_DBG_CONFIG)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Parent: leaving now");
        delete_args(args);
        exit(EXIT_SUCCESS);

    } else if (childpid < 0) {
        delete_args(args);
        ERR(_("can't start daemon process"));
    }

    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Set signal handler");
    struct sigaction action;
    action.sa_handler = termination_handler;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGTERM);
    sigaddset(&action.sa_mask, SIGHUP);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    int ret = 0;

    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Releasing root privileges");
    uid_t daemon_id = geteuid();
    if (seteuid(0) != 0)
        ERR(_("can't change effective user id"));
    ret = setuid(daemon_id);
    if (ret) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("can't release root privileges"));
        kill(getppid(), SIGHUP);
    }

    time_t idle_time = args->delay_upload;
    if (!idle_time)
        idle_time = DAV_DELAY_UPLOAD;
    if (idle_time > args->lock_refresh / 2)
        idle_time = args->lock_refresh / 2;
    int debug = args->debug;
    delete_args(args);
    setsid();
    if (!ret) {
        if (debug & DAV_DBG_CONFIG)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Releasing terminal");
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (open("/dev/null", O_RDONLY) != 0 || open("/dev/null", O_WRONLY) != 1
                || open("/dev/null", O_WRONLY) != 2) {
            ret = -1;
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
                   _("failed to release tty properly"));
            kill(getppid(), SIGHUP);
        }
        dav_set_no_terminal();
    }

    if (!ret) {
        if (debug & DAV_DBG_CONFIG)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Writing pid file");
        ret = save_pid();
        if (ret) {
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
                   _("can't write pid file %s"), pidfile);
            kill(getppid(), SIGHUP);
        }
    }

    if (!ret) {
        if (debug & DAV_DBG_CONFIG)
            syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Starting message loop");
        dav_fuse_loop(dev, mpoint, buf_size, idle_time, is_mounted,
                      &keep_on_running, debug & DAV_DBG_KERNEL);
    }

    if (debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Closing");
    dav_close_cache(&got_sigterm);
    dav_close_webdav();
    if (debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Removing %s", pidfile);
    remove(pidfile);
    if (debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Done.");
    return 0;
}


char *
dav_user_input_hidden(const char *prompt)
{
    fprintf(stdout, "  %s ", prompt);
    fflush(stdout);

    struct termios old;
    if (tcgetattr(fileno(stdin), &old) != 0)
        return NULL;
    struct termios new = old;
    new.c_lflag &= ~ECHO;
    if (tcsetattr(fileno(stdin), TCSAFLUSH, &new) != 0)
        return NULL;
    char *line = NULL;
    size_t n = 0;
    ssize_t len = getline(&line, &n, stdin);
    if (tcsetattr(fileno(stdin), TCSAFLUSH, &old) != 0)
        return NULL;
    printf("\n");

    if (len < 0) abort();
    if (len > 0 && *(line + len - 1) == '\n')
        *(line + len - 1) = '\0';

    return line;
}


/* Private functions */
/*===================*/

/* Changes the group id of the process permanently to dav_group. The
   effective user id of the process will be changed too, but the real
   user id still has to be changed permanently. */
static void
change_persona(dav_args *args)
{
    struct group *grp = getgrnam(args->dav_group);
    if (!grp)
        ERR(_("group %s does not exist"),
              args->dav_group);
    if (seteuid(0) != 0)
        ERR(_("can't change effective user id"));
    if (setgid(grp->gr_gid) != 0)
        ERR(_("can't change group id"));

    if (getuid() == 0) {
        struct passwd *pw = getpwnam(args->dav_user);
        if (!pw)
            ERR(_("user %s does not exist"),
                  args->dav_user);
        if (seteuid(pw->pw_uid) != 0)
            ERR(_("can't change effective user id"));
    } else {
        if (seteuid(getuid()) != 0)
            ERR(_("can't change effective user id"));
    }
    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "changing persona: euid %i, gid %i", geteuid(), getgid());
}


/* Checks for the existence of necessary and usefull directories and files.
   - checks whether it can use the proc file system for information about
     mounted file systems, or has to use mtab
   - whether the directory to save pid files exists and has correct owner and
     permissions; if not it tries to create it and/or set owner and permissions
   - when invoked by non-root user: checks for configuration directory in the
     users homepage and creates missing directories and files
   - checks wether args->cache_dir is accessible. */
static void
check_dirs(dav_args *args)
{
    struct stat st;

#ifdef __linux__

    if (lstat(_PATH_MOUNTED, &st) != 0)
        ERR(_("can't access file %s"), _PATH_MOUNTED);
    int mtab_is_link = S_ISLNK(st.st_mode);

    if (stat(DAV_MOUNTS, &st) == 0) {
        mounts = DAV_MOUNTS;
        args->use_utab = mtab_is_link;
    } else {
        mounts = _PATH_MOUNTED;
    }
    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "mounts in: %s", mounts);

    if (args->use_utab) {
        char *utab_dir = NULL;
        if (asprintf(&utab_dir,"%s/%s", DAV_LOCALSTATE_DIR, DAV_UTAB_DIR) < 0)
            abort();
        if (stat(utab_dir, &st) != 0) {
            if (seteuid(0) != 0)
                ERR(_("can't change effective user id"));
            if (mkdir(utab_dir, S_IRWXU | S_IRGRP | S_IXGRP
                                        | S_IROTH | S_IXOTH) == 0) {
                syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "  and %s/%s",
                       utab_dir, DAV_UTAB);
            } else {
                WARN(_("can't create directory %s"), utab_dir);
            }
            if (seteuid(getuid()) != 0)
                ERR(_("can't change effective user id"));
        }
        free(utab_dir);
    }
#endif

    if (seteuid(0) != 0)
        ERR(_("can't change effective user id"));
    if (stat(DAV_SYS_RUN, &st) != 0) {
        if (mkdir(DAV_SYS_RUN, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH | S_ISVTX)
                != 0 && errno != EEXIST)
            ERR(_("can't create directory %s"),
                  DAV_SYS_RUN);
    }
    if (stat(DAV_SYS_RUN, &st) != 0)
        ERR(_("can't access directory %s"),
              DAV_SYS_RUN);
    if ((st.st_mode & (S_IRWXG | S_ISVTX)) != (S_IRWXG | S_ISVTX)) {
        if (chmod(DAV_SYS_RUN, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH | S_ISVTX)
                != 0)
            ERR(_("can't change mode of directory %s"), DAV_SYS_RUN);
    }
    struct group *grp = getgrnam(args->dav_group);
    if (!grp)
        ERR(_("group %s does not exist"),
              args->dav_group);
    if (st.st_gid != grp->gr_gid) {
        if (chown(DAV_SYS_RUN, 0, grp->gr_gid) != 0)
            ERR(_("can't change group of directory %s"), DAV_SYS_RUN);
    }
    if (seteuid(getuid()) != 0)
        ERR(_("can't change effective user id"));

    if (getuid() != 0) {

        char *path = NULL;
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir)
            path = ne_concat(pw->pw_dir, "/.", PACKAGE, NULL);
        if (path && stat(path, &st) != 0)
            mkdir(path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

        if (path && stat(path, &st) == 0) {
            char *dir = ne_concat(path, "/", DAV_CACHE, NULL);
            if (stat(dir, &st) != 0)
                mkdir(dir, S_IRWXU);
            free(dir);

            dir = ne_concat(path, "/", DAV_CERTS_DIR, NULL);
            if (stat(dir, &st) != 0)
                mkdir(dir, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            free(dir);

            dir = ne_concat(path, "/", DAV_CERTS_DIR, "/", DAV_CLICERTS_DIR,
                            NULL);
            if (stat(dir, &st) != 0)
                mkdir(dir, S_IRWXU);
            free(dir);

            char *file_name = ne_concat(path, "/", DAV_CONFIG, NULL);
            if (stat(file_name, &st) != 0) {
                char *template = ne_concat(DAV_DATA_DIR, "/", DAV_CONFIG, NULL);
                cp_file(template, file_name);
                free(template);
            }
            free(file_name);

            file_name = ne_concat(path, "/", DAV_SECRETS, NULL);
            if (stat(file_name, &st) != 0) {
                char *template = ne_concat(DAV_DATA_DIR, "/", DAV_SECRETS,
                                           NULL);
                cp_file(template, file_name);
                chmod(file_name, S_IRUSR | S_IWUSR);
                free(template);
            }
            free(file_name);
        }
        free(path);
    }

    if (strcmp(args->cache_dir, args->sys_cache) == 0) {

        if (seteuid(0) != 0)
            ERR(_("can't change effective user id"));
        if (stat(args->sys_cache, &st) != 0) {
            if (mkdir(args->sys_cache, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)
                    != 0)
                ERR(_("can't create directory %s"),
                      args->sys_cache);
        }
        if (stat(args->sys_cache, &st) != 0)
            ERR(_("can't access directory %s"),
                  args->sys_cache);
        if ((st.st_mode & S_IRWXG) != S_IRWXG) {
            if (chmod(args->sys_cache, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)
                    != 0)
                ERR(_("can't change mode of directory %s"),
                      args->sys_cache);
        }
        struct group *grp = getgrnam(args->dav_group);
        if (!grp)
            ERR(_("group %s does not exist"),
                  args->dav_group);
        if (st.st_gid != grp->gr_gid) {
            if (chown(args->sys_cache, 0, grp->gr_gid) != 0)
                ERR(_("can't change group of directory %s"),
                      args->sys_cache);
        }
        if (seteuid(getuid()) != 0)
            ERR(_("can't change effective user id"));

    } else {

        struct passwd *pw = getpwuid(getuid());
        if (!pw)
            ERR(_("can't read user data base"));
        if (!pw->pw_name)
            ERR(_("can't read user data base"));
        if (stat(args->cache_dir, &st) != 0) {
            if (mkdir(args->cache_dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)
                    != 0)
                ERR(_("can't create directory %s"),
                      args->cache_dir);
        }
        if (stat(args->cache_dir, &st) != 0)
            ERR(_("can't access directory %s"),
                  args->cache_dir);
        if ((st.st_uid != getuid() || (st.st_mode & S_IRWXU) != S_IRWXU)
                &&  (st.st_mode & S_IRWXO) != S_IRWXO) {
            if ((st.st_mode & S_IRWXG) != S_IRWXG)
                ERR(_("can't access directory %s"),
                      args->cache_dir);
            struct group *grp = getgrgid(st.st_gid);
            if (!grp)
                ERR(_("can't read group data base"));
            char **members = grp->gr_mem;
            while (*members && strcmp(*members, pw->pw_name) != 0)
                members++;
            if (!*members)
                ERR(_("can't access directory %s"),
                args->cache_dir);
        }

    }
}


/* Checks whether url is already mounted on mpoint, creates the name of the
   pid file and checks whether it already exists.
   If one of these tests is positive, it prints an error message and
   terminates the program. Otherwise it returns the name of the pid file.
   return value : the name of the pid file. */
static char *
check_double_mounts(dav_args *args)
{
    if (is_mounted()) {
        ERR(_("%s is already mounted on %s"), url,
            mpoint);
    }

    char *m = mpoint;
    while (*m == '/')
        m++;
    char *mp = ne_strdup(m);
    m = strchr(mp, '/');
    while (m) {
        *m = '-';
        m = strchr(mp, '/');
    }
    char *pidf = NULL;
    if (asprintf(&pidf, "%s/%s.pid", DAV_SYS_RUN, mp) < 0) abort();
    free(mp);
    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "PID file: %s", pidf);

    FILE *file = fopen(pidf, "r");
    if (file)
        ERR(_("found PID file %s.\n"
              "Either %s is used by another process,\n"
              "or another mount process ended irregular"), pidf, mpoint);

    return pidf;
}


/* Checks fstab whether there is an entry for the mountpoint specified in args
   and compares the values in args with the values in fstab.
   If there is no such entry, or this entry does not allow user-mount, or the
   values differ, an error message is printed and the program terminates. */
static void
check_fstab(const dav_args *args)
{
    dav_args *n_args;
#ifdef HAVE_FSTAB_H
    struct fstab *ft;
#else
    FILE *fstab;
    struct mntent *ft;
#endif

#ifdef __FreeBSD__
    int status;
    int val;
    size_t val_len = sizeof(int);

    status = sysctlbyname (VFS_USERMOUNT, &val, &val_len, NULL, 0);

    if (status != 0) {
        WARN(_("sysctl on " VFS_USERMOUNT " failed"));
    } else if (val == 1) {
        /* User mount is set, allow user mouting */
            return;
    }
#endif

    n_args = new_args();
    n_args->mopts = DAV_USER_MOPTS;

#ifdef HAVE_FSTAB_H
    if (setfsent() == 0)
        ERR(_("can't open file %s"), _PATH_FSTAB);

    while ((ft = getfsent()) != NULL) {
        if (ft->fs_file) {
            char *mp = canonicalize_file_name(ft->fs_file);
            if (mp) {
                if (strcmp(mp, mpoint) == 0) {
                    free(mp);
                    break;
                }
                free(mp);
            }
        }
    }
    (void) endfsent();

#else
    fstab = setmntent(_PATH_MNTTAB, "r");
    if (!fstab)
        ERR(_("can't open file %s"), _PATH_MNTTAB);
    ft = getmntent(fstab);
    while (ft) {
        if (ft->mnt_dir) {
            char *mp = canonicalize_file_name(ft->mnt_dir);
            if (mp) {
                if (strcmp(mp, mpoint) == 0) {
                    free(mp);
                    break;
                }
                free(mp);
            }
        }
        ft = getmntent(fstab);
    }

    endmntent(fstab);
#endif

    if (!ft)
        ERR(_("no entry for %s found in %s"), mpoint,
              _PATH_FSTAB);

#ifdef HAVE_FSTAB_H
    if (strcmp(url, ft->fs_spec) != 0) {
        ERR(_("different URL in %s"), _PATH_FSTAB);
    }

    if (!ft->fs_vfstype || strcmp(DAV_FS_TYPE, ft->fs_vfstype) != 0)
        ERR(_("different file system type in %s %s %s"), ft->fs_vfstype, DAV_FS_TYPE,
              _PATH_FSTAB);
    if (ft->fs_mntops)
        get_options(n_args, ft->fs_mntops);
#else
    if (strcmp(url, ft->mnt_fsname) != 0) {
        ERR(_("different URL in %s"), _PATH_MNTTAB);
    }

    if (!ft->mnt_type || strcmp(DAV_FS_TYPE, ft->mnt_type) != 0)
        ERR(_("different file system type in %s"), _PATH_MNTTAB);
    if (ft->mnt_opts)
        get_options(n_args, ft->mnt_opts);
#endif

    if (args->conf || n_args->conf) {
        if (!args->conf || !n_args->conf
                || strcmp(args->conf, n_args->conf) != 0)
            ERR(_("different config file in %s"),
                  _PATH_FSTAB);
    }
    if (args->cl_username || n_args->cl_username) {
        if (!args->cl_username || !n_args->cl_username
                || strcmp(args->cl_username, n_args->cl_username) != 0)
            ERR(_("different username in %s"), _PATH_FSTAB);
    }
    if (!n_args->user && !n_args->users)
        ERR(_("neither option `user' nor option `users' set in %s"),
             _PATH_FSTAB);
    if (args->mopts != n_args->mopts || args->grpid != n_args->grpid)
        ERR(_("different mount options in %s"),
              _PATH_FSTAB);
    if (args->uid != n_args->uid)
        ERR(_("different uid in %s"), _PATH_FSTAB);
    if (args->gid != n_args->gid)
        ERR(_("different gid in %s"), _PATH_FSTAB);
    if (args->dir_mode != n_args->dir_mode)
        ERR(_("different dir_mode in %s"), _PATH_FSTAB);
    if (args->file_mode != n_args->file_mode)
        ERR(_("different file_mode in %s"), _PATH_FSTAB);

    delete_args(n_args);
}


/* The mounting user must be either root or meet the following conditions:
   - The  uid must not differ from the option uid, if this option is used.
   - The user must belong to the group specified in option gid (if used).
   - The user must be member of group args->dav_group.
   If this conditions are not met or an error occurs, an error message is
   printed and exit(EXIT_FAILURE) is called. */
static void
check_permissions(dav_args *args)
{
    if (getuid() == 0)
        return;

    if (args->uid != getuid())
        ERR(_("you can't set file owner different from your uid"));
    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "uid ok");

    if (getgid() != args->gid) {
        struct passwd *pw = getpwuid(getuid());
        if (!pw)
            ERR(_("can't read user data base"));
        if (!pw->pw_name)
            ERR(_("can't read user data base"));
        struct group *grp = getgrgid(args->gid);
        if (!grp)
            ERR(_("can't read group data base"));
        char **members = grp->gr_mem;
        while (*members && strcmp(*members, pw->pw_name) != 0)
            members++;
        if (!*members)
            ERR(_("you must be member of the group of the file system"));
    }
    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "gid ok");

    struct passwd *pw;
    pw = getpwuid(getuid());
    if (!pw)
        ERR(_("can't read user data base"));
    if (!pw->pw_name)
        ERR(_("can't read user data base"));
    struct group *grp = getgrnam(args->dav_group);
    if (!grp)
        ERR(_("group %s does not exist"),
              args->dav_group);
    if (pw->pw_gid != grp->gr_gid) {
        int ngroups = getgroups(0, NULL);
        gid_t *groups = NULL;
        if (ngroups > 0) {
            groups = (gid_t *) malloc(ngroups * sizeof(gid_t));
            if (!groups) abort();
            if (getgroups(ngroups, groups) < 0)
                ERR(_("can't read group data base"));
        } else {
            ERR(_("can't read group data base"));
        }
        int i;
        for (i = 0; i < ngroups; i++) {
            if (grp->gr_gid == groups[i])
                break;
        }
        free(groups);
        if (i == ngroups)
            ERR(_("user %s must be member of group %s"),
                  pw->pw_name, grp->gr_name);
    }
    if (args->debug & DAV_DBG_CONFIG)
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "memeber of group %s", args->dav_group);
}


/* Checks wether the file system is mounted.
   It uses information from the private global variables mounts (mtab-file),
   url (must be device in the mtab entry) and mpoint (mount point).
   return value : 0 - no matching entry in the mtab-file (not mounted)
                  1 - matching entry in the mtab-file (mounted) */
#ifdef __FreeBSD__
static int is_mounted_freebsd(void)
{
    static struct statfs *sfsbuf;
    size_t sfsbufsize;
    int mntsize;
    int i;
    int found = 0;

    mntsize = getfsstat(NULL, 0, MNT_NOWAIT);
    if (mntsize > 0) {
        sfsbufsize = (mntsize + 1) * sizeof(struct statfs);
        if ((sfsbuf = malloc(sfsbufsize)) == NULL)
            ERR("malloc");
        mntsize = getfsstat(sfsbuf, (long)sfsbufsize, MNT_NOWAIT);
        for (i = 0; i < mntsize; i++) {
            struct statfs *sfs;
            sfs = &sfsbuf[i];
            if (strcmp(mpoint, sfs->f_mntonname) == 0
                && strcmp(url, sfs->f_mntfromname) == 0) {
                found = 1;
                break;
            }
        }
    }
    free(sfsbuf);
    return found;
}
#endif

#ifdef __linux__
static int is_mounted_linux(void)
{
    int found = 0;
    FILE *mtab = setmntent(mounts, "r");
    if (mtab) {
        struct mntent *mt = getmntent(mtab);
        while (mt && !found) {
            if (strcmp(mpoint, mt->mnt_dir) == 0
                        && strcmp(url, mt->mnt_fsname) == 0)
                found = 1;
            mt = getmntent(mtab);
        }
    }
    endmntent(mtab);
    return found;
}
#endif

static int
is_mounted(void)
{
#ifdef __linux__
    return is_mounted_linux();
#elif __FreeBSD__
    return is_mounted_freebsd();
#else
    return 0;
#endif
}

/* Parses commandline arguments and options and stores them in args and the
   private global variables url and mpoint.
   For arguments and options please see the usage()-funktion.
   As soon as 'version' or 'help' is found, an appropriate message is printed
   and exit(EXIT_SUCCESS) is called.
   If it does not find exactly two non-option-arguments (url and mointpoint)
   it prints an error message and calls exit(EXIT_FAILURE).
   argc    : the number of arguments.
   argv[]  : array of argument strings.
   return value : args, containig the parsed options and arguments. The args
                  structure and all strings are newly allocated. The calling
                  function is responsible to free them. */
static dav_args *
parse_commandline(int argc, char *argv[])
{
    dav_args *args = new_args();

    size_t len = argc;
    int i;
    for (i = 0; i < argc; i++)
        len += strlen(argv[i]);
    args->cmdline = ne_malloc(len);
    char *p = args->cmdline;
    for (i = 0; i < argc - 1; i++) {
        strcpy(p, argv[i]);
        p += strlen(argv[i]);
        *p = ' ';
        p++;
    }
    strcpy(p, argv[argc - 1]);

    char *short_options = "nvwVho:";
    static const struct option options[] = {
        {"no-mtab", no_argument, NULL, 'n'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {"option", required_argument, NULL, 'o'},
        {0, 0, 0, 0}
    };

    int o;
    o = getopt_long(argc, argv, short_options, options, NULL);
    while (o != -1) {
        switch (o) {
        case 'V':
            printf("%s  <%s>\n\n", PACKAGE_STRING, PACKAGE_BUGREPORT);
            printf(_("This is free software; see the source for copying "
                     "conditions.  There is NO\n"
                     "warranty; not even for MERCHANTABILITY or FITNESS "
                     "FOR A PARTICULAR PURPOSE.\n"));
            exit(EXIT_SUCCESS);
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 'o':
            get_options(args, optarg);
            break;
        case 'n':
        case 'v':
        case 'w':
        case '?':
            break;
        default:
            ERR(_("unknown error parsing arguments"));
        }
        o = getopt_long(argc, argv, short_options, options, NULL);
    }

    i = optind;
    switch (argc - i) {
    case 0:
    case 1:
        WARN(_("missing argument"));
        usage();
        exit(EXIT_FAILURE);
    case 2:
        if (*argv[i] == '\"' || *argv[i] == '\'') {
            url = ne_strndup(argv[i] + 1, strlen(argv[i]) -2);
        } else {
            url = ne_strdup(argv[i]);
        }
        i++;
        mpoint = canonicalize_file_name(argv[i]);
        if (!mpoint)
            ERR(_("can't evaluate path of mount point %s"), mpoint);
        break;
    default:
        WARN(_("too many arguments"));
        usage();
        exit(EXIT_FAILURE);
    }

    if (getuid() != 0 && *argv[i] != '/') {
        struct passwd *pw = getpwuid(getuid());
        if (!pw || !pw->pw_dir)
            ERR(_("can't get home directory for uid %i"), getuid());
        if (strstr(mpoint, pw->pw_dir) != mpoint)
            ERR(_("A relative mount point must lie "
                  "within your home directory"));
    }

    if (!url)
        ERR(_("no WebDAV-server specified"));
    if (split_uri(&args->scheme, &args->host, &args->port, &args->path,
                  url) != 0)
        ERR(_("invalid URL"));
    if (!args->port)
        args->port = ne_uri_defaultport(args->scheme);

    return args;
}


/* Reads and parses the configuration files and stores the values in args.
   The system wide configuration file is parsed first. If args->conf is
   given it will be parsed too and overwrites the values from the system
   wide configuration file. */
static void
parse_config(dav_args *args)
{
    read_config(args, DAV_SYS_CONF_DIR "/" DAV_CONFIG, 1);

    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir)
        ERR(_("can't determine home directory"));

    if (args->conf) {
        if (*args->conf == '~') {
            int p = 1;
            if (*(args->conf + p) == '/')
                p++;
            char *f = ne_concat(pw->pw_dir, "/", args->conf + p, NULL);
            free(args->conf);
            args->conf = f;
        }
        read_config(args, args->conf, 0);
    }

    args->mopts |= DAV_MOPTS;

    args->dir_mode |= S_IFDIR;
    args->file_mode |= S_IFREG;

    struct stat st;
    if (args->trust_ca_cert && *args->trust_ca_cert == '~') {
        int p = 1;
        if (*(args->trust_ca_cert + p) == '/')
            p++;
        char *f = ne_concat(pw->pw_dir, "/", args->trust_ca_cert + p, NULL);
        free(args->trust_ca_cert);
        args->trust_ca_cert = f;
    }
    if (args->trust_ca_cert && *args->trust_ca_cert != '/' && getuid() != 0) {
        char *f = ne_concat(pw->pw_dir, "/.", PACKAGE, "/", DAV_CERTS_DIR, "/",
                            args->trust_ca_cert, NULL);
        if (stat(f, &st) == 0) {
            free(args->trust_ca_cert);
            args->trust_ca_cert = f;
        } else {
            free(f);
        }
    }
    if (args->trust_ca_cert && *args->trust_ca_cert != '/') {
        char *f = ne_concat(DAV_SYS_CONF_DIR, "/", DAV_CERTS_DIR, "/",
                            args->trust_ca_cert, NULL);
        free(args->trust_ca_cert);
        args->trust_ca_cert = f;
    }

    if (args->trust_server_cert && *args->trust_server_cert == '~') {
        int p = 1;
        if (*(args->trust_server_cert + p) == '/')
            p++;
        char *f = ne_concat(pw->pw_dir, "/", args->trust_server_cert + p, NULL);
        free(args->trust_server_cert);
        args->trust_server_cert = f;
    }
    if (args->trust_server_cert && *args->trust_server_cert != '/'
                                && getuid() != 0) {
        char *f = ne_concat(pw->pw_dir, "/.", PACKAGE, "/", DAV_CERTS_DIR, "/",
                            args->trust_server_cert, NULL);
        if (stat(f, &st) == 0) {
            free(args->trust_server_cert);
            args->trust_server_cert = f;
        } else {
            free(f);
        }
    }
    if (args->trust_server_cert && *args->trust_server_cert != '/') {
        char *f = ne_concat(DAV_SYS_CONF_DIR, "/", DAV_CERTS_DIR, "/",
                            args->trust_server_cert, NULL);
        free(args->trust_server_cert);
        args->trust_server_cert = f;
    }

    if (args->secrets && *args->secrets == '~') {
        int p = 1;
        if (*(args->secrets + p) == '/')
            p++;
        char *f = ne_concat(pw->pw_dir, "/", args->secrets + p, NULL);
        free(args->secrets);
        args->secrets = f;
    }

    if (args->clicert && *args->clicert == '~') {
        int p = 1;
        if (*(args->clicert + p) == '/')
            p++;
        char *f = ne_concat(pw->pw_dir, "/", args->clicert + p, NULL);
        free(args->clicert);
        args->clicert = f;
    }
    if (args->clicert && *args->clicert != '/' && getuid() != 0) {
        char *f = ne_concat(pw->pw_dir, "/.", PACKAGE, "/", DAV_CERTS_DIR, "/",
                            DAV_CLICERTS_DIR, "/", args->clicert, NULL);
        if (stat(f, &st) == 0) {
            free(args->clicert);
            args->clicert = f;
        }
    }
    if (args->clicert && *args->clicert != '/' && getuid() == 0) {
        char *f = ne_concat(DAV_SYS_CONF_DIR, "/", DAV_CERTS_DIR, "/",
                            DAV_CLICERTS_DIR, "/", args->clicert, NULL);
        free(args->clicert);
        args->clicert = f;
    }
    if (args->clicert) {
        struct stat st;
        if (seteuid(0) != 0)
            ERR(_("can't change effective user id"));
        if (stat(args->clicert, &st) < 0)
            ERR(_("can't read client certificate %s"),
                  args->clicert);
        if (seteuid(getuid()) != 0)
            ERR(_("can't change effective user id"));
        if (st.st_uid != getuid() && st.st_uid != 0)
            ERR(_("client certificate file %s has wrong owner"),
                 args->clicert);
        if ((st.st_mode &
                (S_IXUSR | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX))
                != 0)
            ERR(_("client certificate file %s has wrong permissions"),
                 args->clicert);
    }

    if (getuid() == 0 && !args->p_host) {
        proxy_from_env(args);
        read_no_proxy_list(args);
    }

    if (!args->p_host)
        args->useproxy = 0;

    if (!args->cache_dir) {
        args->cache_dir = ne_strdup(args->sys_cache);
    } else if (*args->cache_dir == '~') {
        int p = 1;
        if (*(args->cache_dir + p) == '/')
            p++;
        char *f = ne_concat(pw->pw_dir, "/", args->cache_dir + p, NULL);
        free(args->cache_dir);
        args->cache_dir = f;
    }

    if (args->debug & DAV_DBG_CONFIG)
        log_dbg_config(args);
}


/* Reads the secrets file and asks the user interactivly for credentials if
   necessary. The user secrets file is parsed after the system wide secrets
   file, so it will have precedence. */
static void
parse_secrets(dav_args *args)
{
    if (seteuid(0) != 0)
        ERR(_("can't change effective user id"));
    read_secrets(args, DAV_SYS_CONF_DIR "/" DAV_SECRETS);
    if (seteuid(getuid()) != 0)
        ERR(_("can't change effective user id"));

    if (args->secrets) {
        read_secrets(args, args->secrets);
    }

    if (args->cl_username) {
        if (args->username)
            free(args->username);
        args->username = args->cl_username;
        args->cl_username = NULL;
        if (args->password)
            free(args->password);
        args->password = NULL;
        args->password = dav_user_input_hidden(_("Password: "));
    }

    if (args->askauth && args->useproxy && !args->p_user) {
        printf(_("Please enter the username to authenticate with proxy\n"
                 "%s or hit enter for none.\n"), args->p_host);
        args->p_user = user_input(_("Username:"));
    }

    if (args->askauth && args->useproxy && args->p_user && !args->p_passwd) {
        printf(_("Please enter the password to authenticate user %s with proxy\n"
                 "%s or hit enter for none.\n"), args->p_user, args->p_host);
        if (isatty(fileno(stdin))) {
            args->p_passwd = dav_user_input_hidden(_("Password: "));
        } else {
            args->p_passwd = user_input(_("Password: "));
        }
        if (args->p_passwd && strlen(args->p_passwd) == 0) {
            free(args->p_passwd);
            args->p_passwd = NULL;
        }
    }

    if (args->askauth && !args->username) {
        printf(_("Please enter the username to authenticate with server\n"
                 "%s or hit enter for none.\n"), url);
        args->username = user_input(_("Username:"));
    }

    if (args->askauth && args->username && !args->password) {
        printf(_("Please enter the password to authenticate user %s with "
                 "server\n%s or hit enter for none.\n"), args->username, url);
        if (isatty(fileno(stdin))) {
            args->password = dav_user_input_hidden(_("Password: "));
        } else {
            args->password = user_input(_("Password: "));
        }
        if (args->password && strlen(args->password) == 0) {
            free(args->password);
            args->password = NULL;
        }
    }

    if (args->debug & DAV_DBG_SECRETS) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG), "Secrets:");
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "  username: %s", args->username);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "  cl_username: %s", args->cl_username);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "  password: %s", args->password);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "  p_user: %s", args->p_user);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "  p_passwd: %s", args->p_passwd);
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
               "  clicert_pw: %s", args->clicert_pw);
    }
}


/* Saves the pid of the mount.davfs daemon in the pid-file. The name of the
   pid-file is taken from the private global variable pidfile. If an error
   occurs during opening of the pid-file, the function returns with -1. */
static int
save_pid(void)
{
    FILE *file = fopen(pidfile, "w");
    if (!file)
        return -1;
    int ret = 0;
    if (fprintf(file, "%i\n", getpid()) <= 0)
        ret = -1;
    fclose(file);
    chmod(pidfile, S_IRWXU | S_IRGRP | S_IROTH);
    return ret;
}


/* Signal handler for the daemon process.
   Sets global variable keep_on_running to 0, so the message loop will stop
   and the daemon will terminate gracefully. */
static void
termination_handler(int signo)
{
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR), _("pid %i, got signal %i"),
                       getpid(), signo);

    keep_on_running = 0;
    got_sigterm = 1;
}


/* Adds an entry to _PATH_MOUNTED for the mounted file system.
   If _PATH_MOUNTED is a symbolic link to /proc/mounts it will write an
   entry into /var/run/mount/utab instead.
   If this fails a warning will be printed, but this will not stop mounting. */
static void
write_mtab_entry(const dav_args *args)
{
#ifdef __linux__
    struct mntent mntent;
    mntent.mnt_opts = NULL;
    char *utab_line = NULL;
    char *tab_file = NULL;
    char *lock_file = NULL;
    int privileged = (getuid() == 0);
    struct passwd *pw = getpwuid(getuid());
    if (!pw && !privileged) {
        WARN(_("Warning: can't read user data base. Mounting "
               "anyway, but there is no entry in mtab."));
        return;
    }
    char *uid_name = pw->pw_name;

    if (args->use_utab) {
        if (asprintf(&utab_line,
                     "SRC=%s TARGET=%s ROOT=/ "
                     "OPTS=uid=%i,gid=%i%s%s%s%s,helper=%s\n",
                     url, mpoint, args->uid, args->gid,
                     (args->grpid) ? ",grpid" : "",
                     (args->netdev) ? ",_netdev" : "",
                     (!privileged) ? ",user=" : "",
                     (!privileged) ? uid_name : "",
                     DAV_FS_TYPE) < 0)
            abort();
        if (asprintf(&tab_file, "%s/%s/%s", DAV_LOCALSTATE_DIR, DAV_UTAB_DIR,
                     DAV_UTAB) < 0)
            abort();
        if (asprintf(&lock_file, "%s,lock", tab_file) < 0) abort();

    } else {
        mntent.mnt_fsname = url;
        mntent.mnt_dir = mpoint;
        mntent.mnt_type = DAV_FS_TYPE;
        if (asprintf(&mntent.mnt_opts, "%s%s%s%s%s%s,uid=%i,gid=%i%s%s",
                     (args->mopts & MS_RDONLY) ? "ro" : "rw",
                     (args->mopts & MS_NOSUID) ? ",nosuid" : "",
                     (args->mopts & MS_NOEXEC) ? ",noexec" : "",
                     (args->mopts & MS_NODEV) ? ",nodev" : "",
                     (args->grpid) ? ",grpid" : "",
                     (args->netdev) ? ",_netdev" : "",
                     args->uid, args->gid,
                     (!privileged) ? ",user=" : "",
                     (!privileged) ? uid_name : "") < 0)
            abort();
        mntent. mnt_freq = 0;
        mntent. mnt_passno = 0;
        tab_file = ne_strdup(_PATH_MOUNTED);
        if (asprintf(&lock_file, "%s~", tab_file) < 0)
            abort();
    }

    sigset_t oldset;
    sigemptyset(&oldset);
    sigset_t newset;
    sigfillset(&newset);
    sigprocmask(SIG_BLOCK, &newset, &oldset);

    uid_t orig = geteuid();
    if (seteuid(0) != 0)
        ERR(_("can't change effective user id"));

    int ld = open(lock_file, O_RDONLY | O_CREAT,
                  S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
    if (!ld)
        ERR(_("can't create file %s"), lock_file);
    while (flock(ld, LOCK_EX) != 0) {
        if (errno == EAGAIN || errno == EINTR)
            continue;
        ERR(_("can't lock file %s"), lock_file);
    }

    FILE *tab = NULL;
    if (args->use_utab) {
        tab = fopen(tab_file, "a");
    } else {
        tab = setmntent(tab_file, "a");
    }
    int err = 0;
    if (tab) {
        if (args->use_utab) {
            if (fputs(utab_line, tab) == EOF)
                err = 1;
            fclose(tab);
        } else {
            if (addmntent(tab, &mntent) != 0)
                err = 1;
            endmntent(tab);
        }
    }
    if (!tab || err)
        WARN(_("Warning: can't write entry into %s, but will mount "
               "the file system anyway"), tab_file);

    close(ld);
    remove(lock_file);
    if (seteuid(orig) != 0)
        ERR(_("can't change effective user id"));

    sigprocmask(SIG_SETMASK, &oldset, NULL);
    if (lock_file)
        free(lock_file);
    if (tab_file)
        free(tab_file);
    if (utab_line)
        free(utab_line);
    if (mntent.mnt_opts)
        free(mntent.mnt_opts);
#endif
}


/* Helper functions. */

/* Searches arg from the beginning for digits, valid in base, and converts
   them to an integer. If arg does not start with valid digits an error
   message is printed and exit(EXIT_FAILURE) is called.
   Otherwise the integer is returned.
   arg    : string to be converted
   base   : radix of the number; value between 2 and 36
   opt    : name of the option, arg belongs to. Used in the error message.
   return value: the value of the integer number in arg */
static int

arg_to_int(const char *arg, int base, const char *opt)
{
    char *tail = NULL;
    int n = strtol(arg, &tail, base);
    if (n < 0 || !tail) {
        if (base == 10) {
            ERR(_("option %s has invalid argument;"
                  "it must be a decimal number"), opt);
        } else if (base == 8) {
            ERR(_("option %s has invalid argument;"
                  "it must be an octal number"), opt);
        } else {
            ERR(_("option %s has invalid argument;"
                  "it must be a number"), opt);
        }
    }

    return n;
}


/* Creates a copy of src with name dest. */
static void
cp_file(const char *src, const char *dest)
{
    FILE *in = fopen(src, "r");
    if (!in)
        ERR(_("can't open file %s"), src);

    FILE *out = fopen(dest, "w");
    if (!out)
        ERR(_("can't open file %s"), dest);

    size_t n = 0;
    char *line = NULL;
    int length = getline(&line, &n, in);
    while (length > 0) {
        if (fputs(line, out) == EOF)
            ERR(_("error writing to file %s"), dest);
        length = getline(&line, &n, in);
    }

    if (line)
        free(line);
    fclose(out);
    fclose(in);
}


/* Converts a debug option string s into numerical value. If s is not a
   valid debug option, it returns 0. */
static int
debug_opts(const char *s)
{
    if (strcmp(s, "config") == 0)
        return DAV_DBG_CONFIG;
    if (strcmp(s, "kernel") == 0)
        return DAV_DBG_KERNEL;
    if (strcmp(s, "cache") == 0)
        return DAV_DBG_CACHE;
    if (strcmp(s, "secrets") == 0)
        return DAV_DBG_SECRETS;
    if (strcmp(s, "most") == 0)
        return DAV_DBG_CONFIG | DAV_DBG_KERNEL | DAV_DBG_CACHE;
    return 0;
}


/* Converts a debug option string s into numerical value. If s is not a
   valid neon debug option, it returns 0. */
static int
debug_opts_neon(const char *s)
{
    if (strcmp(s, "http") == 0)
        return NE_DBG_HTTP | NE_DBG_SOCKET;
    if (strcmp(s, "xml") == 0)
        return NE_DBG_XML;
    if (strcmp(s, "httpauth") == 0)
        return NE_DBG_HTTPAUTH;
    if (strcmp(s, "locks") == 0)
        return NE_DBG_LOCKS;
    if (strcmp(s, "httpbody") == 0)
        return NE_DBG_HTTPBODY;
    if (strcmp(s, "ssl") == 0)
        return NE_DBG_SSL;
    if (strcmp(s, "secrets") == 0)
        return NE_DBG_HTTPPLAIN;
    if (strcmp(s, "most") == 0)
        return NE_DBG_SOCKET | NE_DBG_HTTP;
    return 0;
}


/* Frees all strings held by args and finally frees args. */
static void
delete_args(dav_args *args)
{
    if (args->cmdline)
        free(args->cmdline);
    if (args->dav_user)
        free(args->dav_user);
    if (args->dav_group)
        free(args->dav_group);
    if (args->conf)
        free(args->conf);
    if (args->scheme)
        free(args->scheme);
    if (args->host)
        free(args->host);
    if (args->path)
        free(args->path);
    if (args->trust_ca_cert)
        free(args->trust_ca_cert);
    if (args->trust_server_cert)
        free(args->trust_server_cert);
    if (args->secrets)
        free(args->secrets);
    if (args->username) {
        memset(args->username, '\0', strlen(args->username));
        free(args->username);
    }
    if (args->cl_username)
        free(args->cl_username);
    if (args->password) {
        memset(args->password, '\0', strlen(args->password));
        free(args->password);
    }
    if (args->clicert)
        free(args->clicert);
    if (args->clicert_pw) {
        memset(args->clicert_pw, '\0', strlen(args->clicert_pw));
        free(args->clicert_pw);
    }
    if (args->p_host)
        free(args->p_host);
    if (args->p_user) {
        memset(args->p_user, '\0', strlen(args->p_user));
        free(args->p_user);
    }
    if (args->p_passwd) {
        memset(args->p_passwd, '\0', strlen(args->p_passwd));
        free(args->p_passwd);
    }
    if (args->lock_owner)
        free(args->lock_owner);
    if (args->s_charset)
        free(args->s_charset);
    if (args->header)
        free(args->header);
    if (args->sys_cache)
        free(args->sys_cache);
    if (args->cache_dir)
        free(args->cache_dir);
    if (args->backup_dir)
        free(args->backup_dir);
    free(args);
}


/* Parses the string option and stores the values in the appropriate fields of
   args.
   All strings returned in args are newly allocated, and the calling function
   is responsible to free them.
   option : a comma separated list of options (like the options in fstab and
            in the -o option of the mount-programm).
            For known options see the declaration at the beginning of the
            the function definition. */
static void
get_options(dav_args *args, char *option)
{
#ifdef NODEV
#undef NODEV
#endif
    enum {
        CONF = 0,
        USERNAME,
        UID,
        GID,
        FILE_MODE,
        DIR_MODE,
        USER,
        NOUSER,
        USERS,
        NETDEV,
        NONETDEV,
        GRPID,
        NOGRPID,
        RW,
        RO,
        SUID,
        NOSUID,
        EXEC,
        NOEXEC,
        DEV,
        NODEV,
        END
    };
    char *suboptions[] = {
        [CONF] = "conf",
        [USERNAME] = "username",
        [UID] = "uid",
        [GID] = "gid",
        [FILE_MODE] = "file_mode",
        [DIR_MODE] = "dir_mode",
        [USER] = "user",
        [NOUSER] = "nouser",
        [USERS] = "users",
        [NETDEV] = "_netdev",
        [NONETDEV] = "no_netdev",
        [GRPID] = "grpid",
        [NOGRPID] = "nogrpid",
        [RW] = "rw",
        [RO] = "ro",
        [SUID] = "suid",
        [NOSUID] = "nosuid",
        [EXEC] = "exec",
        [NOEXEC] = "noexec",
        [DEV] = "dev",
        [NODEV] = "nodev",
        [END] = NULL
    };

    int so;
    char *argument = NULL;
    struct passwd *pwd;
    struct group *grp;

    while (*option != 0) {
        so = getsubopt(&option, suboptions, &argument);
        if ((!argument) && (so < USER))
            ERR(_("option %s requires argument"), suboptions[so]);
        switch (so) {
        case CONF:
            if (args->conf)
                free(args->conf);
            args->conf = ne_strdup(argument);
            break;
        case USERNAME:
            if (args->cl_username)
                free(args->cl_username);
            args->cl_username = ne_strdup(argument);
            break;
        case UID:
            pwd = getpwnam(argument);
            if (!pwd) {
                args->uid = arg_to_int(argument, 10, suboptions[so]);
            } else {
                args->uid = pwd->pw_uid;
            }
            break;
        case GID:
            grp = getgrnam(argument);
            if (!grp) {
                args->gid = arg_to_int(argument, 10, suboptions[so]);
            } else {
                args->gid = grp->gr_gid;
            }
            break;
        case FILE_MODE:
            args->file_mode = arg_to_int(argument, 8, suboptions[so]);
            break;
        case DIR_MODE:
            args->dir_mode = arg_to_int(argument, 8, suboptions[so]);
            break;
        case USER:
            args->user = 1;
            break;
        case NOUSER:
            args->user = 0;
            break;
        case USERS:
            args->users = 1;
            break;
        case NETDEV:
            args->netdev = 1;
            break;
        case NONETDEV:
            args->netdev = 0;
            break;
        case GRPID:
            args->grpid = 1;
            break;
        case NOGRPID:
            args->grpid = 0;
            break;
        case RW:
            args->mopts &= ~MS_RDONLY;
            break;
        case RO:
            args->mopts |= MS_RDONLY;
            break;
        case SUID:
            args->mopts &= ~MS_NOSUID;
            break;
        case NOSUID:
            args->mopts |= MS_NOSUID;
            break;
        case EXEC:
            args->mopts &= ~MS_NOEXEC;
            break;
        case NOEXEC:
            args->mopts |= MS_NOEXEC;
            break;
        case DEV:
            args->mopts &= ~MS_NODEV;
            break;
        case NODEV:
            args->mopts |= MS_NODEV;
            break;
        default:
            break;
        }
    }
}


/* Allocates a new dav_args-structure and initializes it.
   All members are set to reasonable defaults. */
static dav_args *
new_args(void)
{
    char *user_dir = NULL;
    if (getuid() != 0) {
        struct passwd *pw = getpwuid(getuid());
        if (!pw)
            ERR(_("can't read user data base"));
        if (!pw->pw_dir)
            ERR(_("can't read user data base"));
        user_dir = ne_concat(pw->pw_dir, "/.", PACKAGE, NULL);
    }

    dav_args *args = ne_malloc(sizeof(*args));

    args->cmdline = NULL;
    args->dav_user = ne_strdup(DAV_USER);
    args->dav_group = ne_strdup(DAV_GROUP);

    if (getuid() != 0) {
        args->conf = ne_concat(user_dir, "/", DAV_CONFIG, NULL);
    } else {
        args->conf = NULL;
    }

    args->user = 0;
    args->users = 0;
    args->netdev = 1;
    args->grpid = 0;
    args->mopts = DAV_MOPTS;
    args->buf_size = 0;

    args->uid = getuid();
    args->gid = getgid();
    args->dir_mode = DAV_DIR_MODE;
    args->file_mode = DAV_FILE_MODE;

    args->scheme = NULL;
    args->host = NULL;
    args->port = 0;
    args->path = NULL;
    args->trust_ca_cert = NULL;
    args->trust_server_cert = NULL;

    if (getuid() != 0) {
        args->secrets = ne_concat(user_dir, "/", DAV_SECRETS, NULL);
    } else {
        args->secrets = NULL;
    }
    args->username = NULL;
    args->cl_username = NULL;
    args->password = NULL;
    args->clicert = NULL;
    args->clicert_pw = NULL;

    args->p_host = NULL;
    args->p_port = DAV_DEFAULT_PROXY_PORT;
    args->p_user = NULL;
    args->p_passwd = NULL;
    args->useproxy = DAV_USE_PROXY;

    args->lock_owner = NULL;
    args->lock_timeout = DAV_LOCK_TIMEOUT;
    args->lock_refresh = DAV_LOCK_REFRESH;

    args->askauth = DAV_ASKAUTH;
    args->locks = DAV_LOCKS;
    args->expect100 = DAV_EXPECT100;
    args->if_match_bug = DAV_IF_MATCH_BUG;
    args->drop_weak_etags = DAV_DROP_WEAK_ETAGS;
    args->n_cookies = DAV_N_COOKIES;
    args->precheck = DAV_PRECHECK;
    args->ignore_dav_header = DAV_IGNORE_DAV_HEADER;
    args->use_compression = DAV_USE_COMPRESSION;
    args->min_propset = DAV_MIN_PROPSET;
    args->follow_redirect = DAV_FOLLOW_REDIRECT;
    args->sharepoint_href_bug = DAV_SHAREPOINT_HREF_BUG;
    args->connect_timeout = DAV_CONNECT_TIMEOUT;
    args->read_timeout = DAV_READ_TIMEOUT;
    args->retry = DAV_RETRY;
    args->max_retry = DAV_MAX_RETRY;
    args->max_upload_attempts = DAV_MAX_UPLOAD_ATTEMPTS;
    args->s_charset = NULL;
    args->header = NULL;

    args->sys_cache = ne_strdup(DAV_SYS_CACHE);
    if (getuid() != 0) {
        args->cache_dir = ne_concat(user_dir, "/", DAV_CACHE, NULL);
    } else {
        args->cache_dir = NULL;
    }
    args->backup_dir = ne_strdup(DAV_BACKUP_DIR);
    args->cache_size = DAV_CACHE_SIZE;
    args->table_size = DAV_TABLE_SIZE;
    args->dir_refresh = DAV_DIR_REFRESH;
    args->file_refresh = DAV_FILE_REFRESH;
    args->delay_upload = DAV_DELAY_UPLOAD;
    args->gui_optimize = DAV_GUI_OPTIMIZE;
    args->minimize_mem = DAV_MINIMIZE_MEM;

    args->debug = 0;
    args->neon_debug = 0;

    if (user_dir)
        free(user_dir);
    return args;
}


static void
log_dbg_config(dav_args *args)
{
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "%s", args->cmdline);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "Configuration:");
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  url: %s", url);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  mount point: %s", mpoint);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  dav_user: %s", args->dav_user);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  dav_group: %s", args->dav_group);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  conf: %s", args->conf);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  user: %i", args->user);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  netdev: %i", args->netdev);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  grpid: %i", args->grpid);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  mopts: %#lx", args->mopts);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  buf_size: %llu KiB", (unsigned long long) args->buf_size);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  uid: %i", args->uid);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  gid: %i", args->gid);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  dir_mode: %#o", args->dir_mode);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  file_mode: %#o", args->file_mode);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  scheme: %s", args->scheme);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  host: %s", args->host);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  port: %i", args->port);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  path: %s", args->path);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  trust_ca_cert: %s", args->trust_ca_cert);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  trust_server_cert: %s", args->trust_server_cert);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  secrets: %s", args->secrets);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  clicert: %s", args->clicert);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  p_host: %s", args->p_host);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  p_port: %i", args->p_port);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  useproxy: %i", args->useproxy);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  askauth: %i", args->askauth);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  locks: %i", args->locks);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  lock_owner: %s", args->lock_owner);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  lock_timeout: %li s", args->lock_timeout);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  lock_refresh: %li s", args->lock_refresh);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  expect100: %i", args->expect100);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  if_match_bug: %i", args->if_match_bug);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  drop_weak_etags: %i", args->drop_weak_etags);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  n_cookies: %i", args->n_cookies);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  precheck: %i", args->precheck);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  ignore_dav_header: %i", args->ignore_dav_header);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  use_compression: %i", args->use_compression);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  follow_redirect: %i", args->follow_redirect);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  sharepoint_href_bug: %i", args->sharepoint_href_bug);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  connect_timeout: %li s", args->connect_timeout);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  read_timeout: %li s", args->read_timeout);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  retry: %li s", args->retry);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  max_retry: %li s", args->max_retry);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  s_charset: %s", args->s_charset);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  header: %s", args->header);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  sys_cache: %s", args->sys_cache);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  cache_dir: %s", args->cache_dir);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  backup_dir: %s", args->backup_dir);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  cache_size: %llu MiB", (unsigned long long) args->cache_size);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  table_size: %llu", (unsigned long long) args->table_size);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  dir_refresh: %li s", args->dir_refresh);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  file_refresh: %li s", args->file_refresh);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  delay_upload: %i", args->delay_upload);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  gui_optimize: %i", args->gui_optimize);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  minimize_mem: %i", args->minimize_mem);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  debug: %#x", args->debug);
    syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_DEBUG),
           "  neon_debug: %#x", args->neon_debug);
}


/* Parses line for max. parmc white-space separated parameter tokens and
   returns them in parmv[].
   The '#' character marks the beginning of a comment and the rest of the line
   is ignored.
   Parameters containing one of the characters ' ' (space), '\t' (tab), '\',
   '"' or '#' must be enclosed in double quotes '"' *or* this character has to
   be escaped by preceeding a '\'-character.
   Inside double quotes the '"'-character must be escaped. '\' may be escaped;
   it must be escaped if there is more than on '\'-character in succession.
   Whitespace characters other than ' ' and tab must only occur at the end of
   the line.
   line    : the line to be parsed. It will be changed by this function.
   parmc   : the max. number of parameters. It is an error, if more than parmc
             parameters are found
   parmv[] : the parameters found are returned in this array. It contains
             pointers into the rearranged line parameter.
   reurn value : the numer of parameters or -1 if an error occurs. */
static int
parse_line(char *line, int parmc, char *parmv[])
{
    enum {
        SPACE,
        SPACE_EXP,
        PARM,
        PARM_ESC,
        PARM_QUO,
        PARM_QUO_ESC,
        ERROR,
        END
    };

    int state = SPACE;
    int parm_no = 0;
    char *pos = line;
    char *p = line;
    parmv[0] = line;

    while (state != END) {
        switch (state) {
        case SPACE:
            if (*p == '\0' || *p == '#' || *p == '\f' || *p == '\n'
                    || *p == '\r' || *p == '\v') {
                state = END;
            } else if (*p == '\"') {
                if (parm_no < parmc) {
                    parmv[parm_no] = pos;
                    state = PARM_QUO;
                } else {
                    return -1;
                }
            } else if (*p == '\\') {
                if (parm_no < parmc) {
                    parmv[parm_no] = pos;
                    state = PARM_ESC;
                } else {
                    return -1;
                }
                state = PARM_ESC;
            } else if (isspace(*p)) {
                ;
            } else {
                if (parm_no < parmc) {
                    parmv[parm_no] = pos;
                    *pos++ = *p;
                    state = PARM;
                } else {
                    return -1;
                }
            }
            break;
        case SPACE_EXP:
            if (*p == ' ' || *p == '\t') {
                state = SPACE;
            } else if (*p == '\0' || *p == '#' || *p == '\f' || *p == '\n'
                       || *p == '\r' || *p == '\v') {
                state = END;
            } else {
                return -1;
            }
            break;
        case PARM:
            if (*p == '\"') {
                return -1;
            } else if (*p == '\\') {
                state = PARM_ESC;
            } else if (*p == ' ' || *p == '\t') {
                *pos++ = '\0';
                parm_no++;
                state = SPACE;
            } else if (isspace(*p) || *p == '\0' || *p == '#') {
                *pos = '\0';
                parm_no++;
                state = END;
            } else {
                *pos++ = *p;
            }
            break;
        case PARM_ESC:
            if (*p == '\"' || *p == '\\' || *p == '#' || *p == ' '
                    || *p == '\t') {
                *pos++ = *p;
                state = PARM;
            } else {
                return -1;
            }
            break;
        case PARM_QUO:
            if (*p == '\\') {
                state = PARM_QUO_ESC;
            } else if (*p == '\"') {
                *pos++ = '\0';
                parm_no++;
                state = SPACE_EXP;
            } else if (*p == '\0' || *p == '\f' || *p == '\n'
                       || *p == '\r' || *p == '\v') {
                return -1;
            } else {
                *pos++ = *p;
            }
            break;
        case PARM_QUO_ESC:
            if (*p == '\"' || *p == '\\') {
                *pos++ = *p;
                state = PARM_QUO;
            } else if (*p == '\0' || *p == '\f' || *p == '\n'
                       || *p == '\r' || *p == '\v') {
                return -1;
            } else {
                *pos++ = '\\';
                *pos++ = *p;
                state = PARM_QUO;
            }
            break;
        }
        p++;
    }

    int i;
    for (i = parm_no; i < parmc; i++)
        parmv[i] = NULL;
    return parm_no;
}


/* Checks for a matching xxx_proxy environment variable, and if found
   stores values in args->p_host and ars->p_port. */
static void
proxy_from_env(dav_args *args)
{
    const char *env = NULL;
    if (args->scheme && strcmp(args->scheme, "https") == 0)
        env = getenv("https_proxy");
    if (!env)
        env = getenv("http_proxy");
    if (!env)
        env = getenv("all_proxy");
    if (!env)
        return;

    char *scheme = NULL;
    char *host = NULL;
    int port = 0;
    split_uri(&scheme, &host, &port, NULL, env);

    if (scheme && strcmp(scheme, "http") == 0 && host) {
        if (args->p_host) free(args->p_host);
        args->p_host = host;
        host = NULL;
        if (port)
            args->p_port = port;
    }

    if (scheme) free(scheme);
    if (host) free(host);
}


/* Reads the configuration file filename and stores the values in args.
   filename : name of the configuration file.
   system   : boolean value. 1 means it is the system wide configuration
              file. Some parameters are allowed only in the system wide
              configuration file, some only in the user configuration file. */
static void
read_config(dav_args *args, const char * filename, int system)
{
    FILE *file = fopen(filename, "r");
    if (!file) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("opening %s failed"), filename);
        return;
    }

    size_t n = 0;
    char *line = NULL;
    int length = getline(&line, &n, file);
    int lineno = 1;
    int applies = 1;

    while (length > 0) {

        int parmc = 3;
        char *parmv[parmc];
        int count;
        count = parse_line(line, parmc, parmv);

        if (count == 1) {

            if (*parmv[0] != '[' || *(parmv[0] + strlen(parmv[0]) - 1) != ']')
                ERR_AT_LINE(filename, lineno, _("malformed line"));
            *(parmv[0] + strlen(parmv[0]) - 1) = '\0';
            char *mp = canonicalize_file_name(parmv[0] + 1);
            if (mp) {
                applies = (strcmp(mp, mpoint) == 0);
                free(mp);
            }

        } else if (applies && count == 2) {

            if (system && strcmp(parmv[0], "dav_user") == 0) {
                if (args->dav_user)
                    free(args->dav_user);
                args->dav_user = ne_strdup(parmv[1]);
            } else if (system && strcmp(parmv[0], "dav_group") == 0) {
                if (args->dav_group)
                    free(args->dav_group);
                args->dav_group = ne_strdup(parmv[1]);
            } else if (strcmp(parmv[0], "buf_size") == 0) {
                args->buf_size = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "trust_ca_cert") == 0
                       || strcmp(parmv[0], "servercert") == 0) {
                if (args->trust_ca_cert)
                    free(args->trust_ca_cert);
                args->trust_ca_cert = ne_strdup(parmv[1]);
            } else if (strcmp(parmv[0], "trust_server_cert") == 0) {
                if (args->trust_server_cert)
                    free(args->trust_server_cert);
                args->trust_server_cert = ne_strdup(parmv[1]);
            } else if (!system && strcmp(parmv[0], "secrets") == 0) {
                if (args->secrets)
                    free(args->secrets);
                args->secrets = ne_strdup(parmv[1]);
            } else if (strcmp(parmv[0], "clientcert") == 0) {
                if (args->clicert)
                    free(args->clicert);
                args->clicert = ne_strdup(parmv[1]);
            } else if (system && strcmp(parmv[0], "proxy") == 0) {
                if (split_uri(NULL, &args->p_host, &args->p_port, NULL,
                              parmv[1]) != 0)
                    ERR_AT_LINE(filename, lineno, _("malformed line"));
            } else if (system && strcmp(parmv[0], "use_proxy") == 0) {
                args->useproxy = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "ask_auth") == 0) {
                args->askauth = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "use_locks") == 0) {
                args->locks = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "lock_owner") == 0) {
                if (args->lock_owner)
                    free(args->lock_owner);
                args->lock_owner = ne_strdup(parmv[1]);
            } else if (strcmp(parmv[0], "lock_timeout") == 0) {
                args->lock_timeout = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "lock_refresh") == 0) {
                args->lock_refresh = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "use_expect100") == 0) {
                args->expect100 = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "if_match_bug") == 0) {
                args->if_match_bug = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "drop_weak_etags") == 0) {
                args->drop_weak_etags = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "n_cookies") == 0) {
                args->n_cookies = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "precheck") == 0) {
                args->precheck = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "ignore_dav_header") == 0) {
                args->ignore_dav_header = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "use_compression") == 0) {
                args->use_compression = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "min_propset") == 0) {
                args->min_propset = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "follow_redirect") == 0) {
                args->follow_redirect = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "sharepoint_href_bug") == 0) {
#if NE_VERSION_MAJOR > 0 || NE_VERSION_MINOR > 30
                args->sharepoint_href_bug = arg_to_int(parmv[1], 10, parmv[0]);
#else
                ERR(_("Option sharepoint_href_bug requires "
                      "Neon version 0.31 or newer"));
#endif
            } else if (strcmp(parmv[0], "connect_timeout") == 0) {
                args->connect_timeout = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "read_timeout") == 0) {
                args->read_timeout = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "retry") == 0) {
                args->retry = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "max_retry") == 0) {
                args->max_retry = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "max_upload_attempts") == 0) {
                args->max_upload_attempts = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "server_charset") == 0) {
                if (args->s_charset)
                    free(args->s_charset);
                args->s_charset = ne_strdup(parmv[1]);
            } else if (system && strcmp(parmv[0], "cache_dir") == 0) {
                if (args->sys_cache)
                    free(args->sys_cache);
                args->sys_cache = ne_strdup(parmv[1]);
            } else if (!system && strcmp(parmv[0], "cache_dir") == 0) {
                if (args->cache_dir != NULL)
                    free(args->cache_dir);
                args->cache_dir = ne_strdup(parmv[1]);
            } else if (strcmp(parmv[0], "backup_dir") == 0) {
                if (args->backup_dir)
                    free(args->backup_dir);
                args->backup_dir = ne_strdup(parmv[1]);
            } else if (strcmp(parmv[0], "cache_size") == 0) {
                args->cache_size = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "table_size") == 0) {
                args->table_size = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "dir_refresh") == 0) {
                args->dir_refresh = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "file_refresh") == 0) {
                args->file_refresh = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "delay_upload") == 0) {
                args->delay_upload = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "gui_optimize") == 0) {
                args->gui_optimize = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "minimize_mem") == 0) {
                args->minimize_mem = arg_to_int(parmv[1], 10, parmv[0]);
            } else if (strcmp(parmv[0], "debug") == 0) {
                args->debug |= debug_opts(parmv[1]);
                args->neon_debug |= debug_opts_neon(parmv[1]);
            } else {
                ERR_AT_LINE(filename, lineno, _("unknown option"));
            }

        } else if (applies && count == 3) {

            if (strcmp(parmv[0], "add_header") == 0) {
                char *tmp = args->header;
                args->header = ne_concat(parmv[1], ": ", parmv[2], "\r\n", tmp,
                                         NULL);
                if (tmp)
                    free(tmp);
            } else {
                ERR_AT_LINE(filename, lineno, _("unknown option"));
            }

        } else if (count < 0 || count > 3) {

            ERR_AT_LINE(filename, lineno, _("malformed line"));
        }

        length = getline(&line, &n, file);
        lineno++;
    }

    if (line)
        free(line);
    fclose(file);
}

/* Reads environment variable no_proxy. no_proxy must be a comma separated
   list of domain names. If no_proxy is "*" or args->p_host matches any of
   the entries in the no_proxy-list, args->p_host is removed. */
static void
read_no_proxy_list(dav_args *args)
{
    if (!args->p_host || !args->host)
        return;

    const char *env = getenv("no_proxy");
    if (!env)
        return;

    if (strcmp(env,"*") == 0) {
        free(args->p_host);
        args->p_host = NULL;
        return;
    }

    char *noproxy_list = ne_strdup(env);
    char *np = strtok(noproxy_list, ", ");
    while (np && args->p_host) {

        char *host = NULL;
        if (strchr(np, ':')) {
            if (asprintf(&host, "%s:%d", args->host, args->port) < 0)
                abort();
        } else {
            host = strdup(args->host);
        }

        if (*np == '.') {
            char *substr = strcasestr(host, np);
            if (substr && *(substr + strlen(np)) == '\0') {
                free(args->p_host);
                args->p_host = NULL;
            }
        } else {
            if (strcasecmp(host, np) == 0) {
                free(args->p_host);
                args->p_host = NULL;
            }
        }

        free(host);
        np = strtok(NULL, ", ");
    }
    free(noproxy_list);
}


/* Searches the file filename for credentials for server url and for the proxy
   args->p_host and stores them in args. */
static void
read_secrets(dav_args *args, const char *filename)
{
    struct stat st;
    if (stat(filename, &st) < 0) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("opening %s failed"), filename);
        return;
    }
    if (st.st_uid != geteuid())
        ERR(_("file %s has wrong owner"), filename);
    if ((st.st_mode &
          (S_IXUSR | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX)) != 0)
        ERR(_("file %s has wrong permissions"), filename);

    FILE *file = fopen(filename, "r");
    if (!file) {
        syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_ERR),
               _("opening %s failed"), filename);
        return;
    }

    size_t n = 0;
    char *line = NULL;
    int length = getline(&line, &n, file);
    int lineno = 1;

    while (length > 0) {
        int parmc = 3;
        char *parmv[parmc];
        int count;
        count = parse_line(line, parmc, parmv);
        if (count != 0 && count != 3 && count != 2)
            ERR_AT_LINE(filename, lineno, _("malformed line"));

        if (count == 2 || count == 3) {

            char *scheme = NULL;
            char *host = NULL;
            int port = 0;
            char *path = 0;
            split_uri(&scheme, &host, &port, &path, parmv[0]);
            int p_port = port;
            if (scheme && !port)
                port = ne_uri_defaultport(scheme);

            char *mp = canonicalize_file_name(parmv[0]);

            char *ccert = NULL;
            if (args->clicert) {
                ccert = strrchr(args->clicert, '/');
                if (ccert && *(ccert + 1) == '\0')
                    ccert = NULL;
                if (ccert)
                    ccert++;
            }

            if ((mp && strcmp(mp, mpoint) == 0)
                    || (scheme && args->scheme
                        && strcmp(scheme, args->scheme) == 0
                        && host && args->host && strcmp(host, args->host) == 0
                        && port == args->port
                        && path && args->path
                        && strcmp(path, args->path) == 0)) {

                if (args->username) {
                    memset(args->username, '\0', strlen(args->username));
                    free(args->username);
                }
                if (args->password) {
                    memset(args->password, '\0', strlen(args->password));
                    free(args->password);
                }
                args->username = ne_strdup(parmv[1]);
                if (count == 3)
                    args->password = ne_strdup(parmv[2]);

            } else if (strcmp(parmv[0], "proxy") == 0
                       || (host && args->p_host
                           && strcmp(host, args->p_host) == 0
                           && (!p_port || p_port == args->p_port))) {

                if (args->p_user) {
                    memset(args->p_user, '\0', strlen(args->p_user));
                    free(args->p_user);
                }
                if (args->p_passwd) {
                    memset(args->p_passwd, '\0', strlen(args->p_passwd));
                    free(args->p_passwd);
                }
                args->p_user = ne_strdup(parmv[1]);
                if (count == 3)
                    args->p_passwd = ne_strdup(parmv[2]);

            } else if (args->clicert
                       && (strcmp(parmv[0], args->clicert) == 0
                           || strcmp(parmv[0], ccert) == 0)) {

                if (count != 2)
                    ERR_AT_LINE(filename, lineno, _("malformed line"));
                if (args->clicert_pw) {
                    memset(args->clicert_pw, '\0', strlen(args->clicert_pw));
                    free(args->clicert_pw);
                }
                args->clicert_pw = ne_strdup(parmv[1]);
            }

            if (scheme) free(scheme);
            if (host) free(host);
            if (path) free(path);
            if (mp) free(mp);
        }

        memset(line, '\0', strlen(line));
        length = getline(&line, &n, file);
        lineno++;
    }

    if (line) {
        memset(line, '\0', strlen(line));
        free(line);
    }
    fclose(file);
}


/* Splits an uri and returns the components.
   The uri must contain a host, the other components are optional. It must
   not contain userinfo. It shall not contain a query or fragment component;
   they would be treated as part of path.
   The path component must *not* be %-encoded. scheme, if present in uri,
   must be either http or https. If host is a IPv6 address, it must be enclosed
   in square brackets.
   The pointers to the components may be NULL. If they point to a non-NULL
   string, it is freed and then replaced by a newly allocated string.
   If no scheme is foud the default sheme "http" is returned.
   If no path is found "/" is returned as path. path will always end with "/".
   There is *no* default value returned for port.
   return value : 0 on success, -1 otherwise. */
static int
split_uri(char **scheme, char **host, int *port,char **path, const char *uri)
{
    if (!uri || !*uri) return -1;

    const char *sch = NULL;
    int po = 0;
    const char *ho = strstr(uri, "://");
    if (ho) {
        if ((ho - uri) == 4 && strcasestr(uri, "http") == uri) {
            sch = "http";
        } else if ((ho - uri) == 5 && strcasestr(uri, "https") == uri) {
            sch = "https";
        } else {
            return -1;
        }
        ho += 3;
    } else {
        ho = uri;
    }
    if (!*ho) return -1;

    const char *pa = strchrnul(ho, '/');
    if (pa == ho) return -1;

    const char *end = strchr(ho, '@');
    if (end && end < pa) return -1;

    if (*ho == '[') {
        end = strchr(ho, ']');
        if (!end || end >= pa) return -1;
        end++;
    } else {
        end = strchr(ho, ':');
        if (!end)
            end = pa;
    }

    if (end < pa) {
        if (end == ho || end == (pa - 1) || *end != ':') return -1;
        char *tail = NULL;
        po = strtol(end + 1, &tail, 10);
        if (po <= 0 || tail != pa) return -1;
    }

    if (scheme) {
        if (*scheme) free(*scheme);
        if (sch) {
            *scheme = strdup(sch);
        } else {
            *scheme = strdup("http");
        }
        if (!*scheme) abort();
    }

    if (port && po)
        *port = po;

    if (host) {
        if (*host) free(*host);
        *host = malloc(end - ho + 1);
        if (!*host) abort();
        int i;
        for (i = 0; i < (end - ho); i++) {
            if (*ho == '[') {
                *(*host + i) = islower(*(ho + i))
                               ? toupper(*(ho + i)) : *(ho + i);
            } else {
                *(*host + i) = isupper(*(ho + i))
                               ? tolower(*(ho + i)) : *(ho + i);
            }
        }
        *(*host + i) = '\0';
    }

    if (path) {
        if (*path) free(*path);
        if (!*pa) {
            *path = strdup("/");
        } else if (*(pa + strlen(pa) - 1) == '/') {
            *path = strdup(pa);
        } else {
            if (asprintf(path, "%s/", pa) < 1) abort();
        }
        if (!*path) abort();
    }

    return 0;
}


/* Prints version und help text. */
static void
usage(void)
{
    printf(_("Usage:\n"
             "    %s -V,--version   : print version string\n"
             "    %s -h,--help      : print this message\n\n"),
           PROGRAM_NAME, PROGRAM_NAME);
    printf(_("To mount a WebDAV-resource don't call %s directly, but use\n"
             "`mount' instead.\n"), PROGRAM_NAME);
    printf(_("    mount <mountpoint>  : or\n"
             "    mount <server-url>  : mount the WebDAV-resource as specified in\n"
             "                          /etc/fstab.\n"));
    printf(_("    mount -t davfs <server-url> <mountpoint> [-o options]\n"
             "                        : mount the WebDAV-resource <server-url>\n"
             "                          on mountpoint <mountpoint>. Only root\n"
             "                          is allowed to do this. options is a\n"
             "                          comma separated list of options.\n\n"));
    printf(_("Recognised options:\n"
             "    conf=        : absolute path of user configuration file\n"
             "    uid=         : owner of the filesystem (username or numeric id)\n"
             "    gid=         : group of the filesystem (group name or numeric id)\n"
             "    file_mode=   : default file mode (octal)\n"
             "    dir_mode=    : default directory mode (octal)\n"));
    printf(_("    ro           : mount read-only\n"
             "    rw           : mount read-write\n"
             "    [no]exec     : (don't) allow execution of binaries\n"
             "    [no]suid     : (don't) allow suid and sgid bits to take effect\n"
             "    [no]grpid    : new files (don't) get the group id of the directory\n"
             "                   in which they are created.\n"
             "    [no]_netdev  : (no) network connection needed\n"));
}


/* Prints prompt to stdout and reads a line from stdin.
   A trailing newline is removed.
   return value : the user input. */
static char *
user_input(const char *prompt)
{
    fprintf(stdout, "  %s ", prompt);
    fflush(stdout);
    char *line = NULL;
    size_t n = 0;
    ssize_t len = getline(&line, &n, stdin);
    if (len < 0) abort();
    if (len > 0 && *(line + len - 1) == '\n')
        *(line + len - 1) = '\0';

    return line;
}


