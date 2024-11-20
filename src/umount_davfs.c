/*  umount_davfs.c: unmount the davfs file system.
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


#include "config.h"

#include <getopt.h>
#ifdef HAVE_LIBINTL_H
#include <libintl.h>
#endif
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <ne_string.h>

#include "util.h"
#include "defaults.h"

#ifdef ENABLE_NLS
#define _(String) gettext(String)
#else
#define _(String) String
#define textdomain(Domainname)
#define bindtextdomain(Domainname, Dirname)
#endif

#if defined(__linux__)
#define UMOUNT_CMD "umount -i"
#elif defined(__FreeBSD__)
#define UMOUNT_CMD "umount"
#endif


/* This is lazy programming. All the dirty work is left to the real umount
   program, while we just sit and wait for mount.davfs to terminate.
   umount.davfs is a umount helper. It is usually called by umount and makes
   sure, that umount will not return until mount.davfs has synchronized all
   files.
   It first reads the pid-file and identifies the mount.davfs process. Then
   it calls mount again, with option -i (to not be called again), to do the
   real unmounting. In a loop it will watch the process list. When the
   mount.davfs process terminates, it will return.
   If it can't identify the mount.davfs process, it will call umount -i anyway,
   but warn the user. */
int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);

    char *short_options = "Vhflnrt:v";
    static const struct option options[] = {
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
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
            printf(_("Usage:\n"
                     "    u%s -V,--version  : print version string\n"
                     "    u%s -h,--help     : print this message\n\n"),
                  PROGRAM_NAME, PROGRAM_NAME);
            printf(_("To umount a WebDAV-resource don't call u%s directly, "
                     "but use\n"
                     "`umount' instead.\n"), PROGRAM_NAME);
            printf(_("    umount <mountpoint> : umount the WebDAV-resource as "
                     "specified in\n"
                     "                          /etc/fstab.\n"));
            exit(EXIT_SUCCESS);
        case 'f':
        case 'l':
        case 'n':
        case 'r':
        case 'v':
        case 't':
        case '?':
            break;
        default:
            ERR(_("unknown error parsing arguments"));
        }
        o = getopt_long(argc, argv, short_options, options, NULL);
    }

    if (optind > (argc - 1))
        ERR(_("missing argument"));
    if (optind < (argc - 1))
        ERR(_("too many arguments"));

    char *mpoint = canonicalize_file_name(argv[optind]);

    char *umount_command = NULL;
    if (mpoint) {
        umount_command = ne_concat(UMOUNT_CMD " '", mpoint, "'", NULL);
    } else {
        umount_command = ne_concat(UMOUNT_CMD " '", argv[optind], "'", NULL);
        WARN(_("\n"
              "  can't evaluate PID file name;\n"
              "  trying to unmount anyway;\n"
              "  please wait for %s to terminate"), PROGRAM_NAME);
        return system(umount_command);
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
    char *pidfile = ne_concat(DAV_SYS_RUN, "/", mp, ".pid", NULL);
    free(mp);

    char pid[32];
    memset (pid, '\0', sizeof(pid));
    FILE *file = fopen(pidfile, "r");
    if (!file || fscanf(file, "%s[0-9]", pid) != 1 || pid[0] == '\0') {
        WARN(_("\n"
               "  can't read PID from file %s;\n"
               "  trying to unmount anyway;\n"
               "  please wait for %s to terminate"), pidfile, PROGRAM_NAME);
        return system(umount_command);
    }
    fclose(file);

    char *ps_command = ne_concat("ps -p ", pid, NULL);
    FILE *ps_in = popen(ps_command, "r");
    if (!ps_in) {
        WARN(_("\n"
               "  can't read process list;\n"
               "  trying to unmount anyway;\n"
               "  please wait for %s to terminate"), PROGRAM_NAME);
        return system(umount_command);
    }

    int found = 0;
    size_t n = 0;
    char *ps_line = NULL;
    while (!found && getline(&ps_line, &n, ps_in) > 0)
        found = (strstr(ps_line, pid) && strstr(ps_line, PROGRAM_NAME));
    pclose(ps_in);

    if (!found) {
        WARN(_("\n"
               "  can't find %s-process with pid %s;\n"
               "  trying to unmount anyway.\n"
               "  you propably have to remove %s manually"),
             PROGRAM_NAME, pid, pidfile);
        return system(umount_command);
    }

    if (system(umount_command) != 0)
        exit(EXIT_FAILURE);

    printf(_("%s: waiting for %s (pid %s) to terminate gracefully ."),
           argv[0], PROGRAM_NAME, pid);
    fflush(stdout);

    while (found) {

        sleep(3);
        printf(".");
        fflush(stdout);

        ps_in = popen(ps_command, "r");
        if (!ps_in) {
            printf("\n");
            ERR(_("an error occurred while waiting; "
                  "please wait for %s to terminate"), PROGRAM_NAME);
        }

        found = 0;
        while (!found && getline(&ps_line, &n, ps_in) > 0)
            found = (strstr(ps_line, pid) && strstr(ps_line, PROGRAM_NAME));

        pclose(ps_in);
    }
    printf(" OK\n");

    return 0;
}
