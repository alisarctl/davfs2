/*  mount_davfs.h: structure to collect arguments and options.
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


#ifndef DAV_MOUNT_DAVFS_H
#define DAV_MOUNT_DAVFS_H


/* Data Types */
/*============*/

/* This enum lists the supported proxy types */
typedef enum dav_proxy_type {
    dav_proxy_type_http,
    dav_proxy_type_socks4,
    dav_proxy_type_socks4a,
    dav_proxy_type_socks5
} dav_proxy_type;

/* This data structure holds almost everything davfs gathers while reading and
   checking command line and configuration files. (See comment for data origin;
   highest precedence first.)
   Some data will be copied into global or local variables to be available in
   daemon mode. The rest will be freed when forking into daemon mode. */
typedef struct {
    char *cmdline;
    char *dav_user;           /* System config file */
    char *dav_group;          /* System config file */
    char *conf;               /* Command line */
    /* Mount options */
    int user;                 /* Command line */
    int users;                /* Command line */
    int netdev;               /* Command line */
    int grpid;                /* Command line */
    unsigned long int mopts;  /* Command line */
    int use_utab;
    size_t buf_size;          /* User config file, system config file */
    /* File mode */
    uid_t uid;                /* Command line */
    gid_t gid;                /* Command line */
    mode_t dir_mode;          /* Command line */
    mode_t file_mode;         /* Command line */
    /* WebDAV-resource */
    char *scheme;             /* Command line */
    char *host;               /* Command line */
    int port;                 /* Command line */
    char *path;               /* Command line */
    char *trust_ca_cert;      /* User config file, system config file */
    char *trust_server_cert;  /* User config file, system config file */
    char *secrets;            /* User config file */
    char *username;           /* User secrets file, system secrets file */
    char *cl_username;        /* Command line */
    char *password;           /* User secrets file, system secrets file */
    char *clicert;            /* User config file, system config file */
    char *clicert_pw;         /* User secrets file, system secrets file */
    char *p_host;             /* System config file, environment */
    int p_port;               /* System config file, environment */
    dav_proxy_type p_type;    /* System config file, environment */
    char *p_user;             /* User secrets file, system secrets file */
    char *p_passwd;           /* User secrets file, system secrets file */
    int useproxy;             /* System config file, command line */
    int askauth;              /* User config file, sys conf f., command line */
    int locks;                /* User config file, sys conf f., command line */
    char * lock_owner;        /* User config file, system config file */
    time_t lock_timeout;      /* User config file, system config file */
    time_t lock_refresh;      /* User config file, system config file */
    int expect100;            /* User config file, system config file */
    int if_match_bug;         /* User config file, system config file */
    int drop_weak_etags;      /* User config file, system config file */
    int n_cookies;            /* User config file, system config file */
    int precheck;             /* User config file, system config file */
    int ignore_dav_header;    /* User config file, system config file */
    int use_compression;      /* User config file, system config file */
    int min_propset;          /* User config file, system config file */
    int follow_redirect;      /* User config file, system config file */
    int sharepoint_href_bug;  /* User config file, system config file */
    time_t connect_timeout;   /* User config file, system config file */
    time_t read_timeout;      /* User config file, system config file */
    time_t retry;             /* User config file, system config file */
    time_t max_retry;         /* User config file, system config file */
    int max_upload_attempts;  /* User config file, system config file */
    char * s_charset;         /* User config file, system config file */
    char * header;            /* User config file, system config file */
    /* Cache */
    char *sys_cache;          /* System config file */
    char *cache_dir;          /* User config file */
    char *backup_dir;         /* User config file, system config file */
    size_t cache_size;        /* User config file, system config file */
    size_t table_size;        /* User config file, system config file */
    time_t dir_refresh;       /* User config file, system config file */
    time_t file_refresh;      /* User config file, system config file */
    int delay_upload;         /* User config file, system config file */
    int gui_optimize;         /* User config file, system config file */
    int minimize_mem;         /* User config file, system config file */
    int sync_on_lookup;
    /* Debugging */
    int debug;                /* User config file, system config file */
    int neon_debug;           /* User config file, system config file */
} dav_args;


/* Public functions. */
/*===================*/

/* Main launches a daemon program that runs a directory and file cache and
   is connected to the WbDAV resource and the kernel file system module.
   It must run setuid root. After forking into  daemon mode it releases root
   permissions permanently. The daemon runs with the uid of the user that owns
   the file system. (If invoked by root and the mounted file system is owned
   by root, the daemon runs as root. This should be avoided.)
   Launching the daemon (and stopping) is done in 5 steps.
   Step 1:
   - Gathering information from command line, configuration files and
     environment.
   - Checking this information for consistency and any errors that would
     prevent successful running of the daemon.
   - Checking whether the the user has permissions to mount.
   - Checking whether the neccessary files and directories for running the
     daemon are available.
   Step 2:
   - The modules for connecting to the kernel, connecting to the WebDAV resource
     and for caching are initialised.
   If an error accurs during step 1 or step 2 an error message is printed and
   the program dies immediately. Clean up is left to the operating system.
   Step 3:
   - Forking into daemon mode.
   - While the daemon (child) writes the pid file and starts reading upcalls
     from the kernel in an endless loop, the parent process tries to mount the
     file system and write an entry into mtab (_PROC_MOUNTS).
   - If an error occurs in one of the processes it sends SIGTERM to the other.
     While the parent just dies, the daemon will run its normal exit code
     (see step 5). In rare cases this might nevertheless leave stale pid files
     or entries in mtab that must be cleaned manually by the administrator.
   - If mounting is successful the parent process exits with success.
   Step 4:
   - Running as daemon.
   Step 5:
   - Terminating.
   - The daemon has set a signal handler for SIGTERM and SIGHUP. If it gets one
     of these signals it tries to unmount the file system and resets the global
     variable keep_on_running. This will terminate the message loop gracefully.
   - If the file system is unmounted (by the umount programm), the message
     loop will terminate gracefully.
   - The close functions of the modules are called, that will clean up the
     cache, save cached information if neccessary and close the connections. */
int
main(int argc, char *argv[]);


/* Prints prompt to stdout and reads a line from stdin.
   Echoing the user input to stdout is prohibited.
   A trailing newline is removed.
   return value : the user input. */
char *
dav_user_input_hidden(const char *prompt);


#endif /* DAV_MOUNT_DAVFS_H */
