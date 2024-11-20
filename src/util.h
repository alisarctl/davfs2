/*
 * Copyright (C) 2022  Ali Abdallah <ali.abdallah@suse.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __DAVFS_UTIL_H
#define __DAVFS_UTIL_H

#include "config.h"

#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include <stdarg.h>
#include <errno.h>
#include <err.h>

#ifdef ERR
#undef ERR
#endif

#define ERR(fmt, ...) { \
    if (errno != 0) \
        errx(EXIT_FAILURE, fmt, ## __VA_ARGS__); \
    else { \
        fprintf(stderr, fmt, ## __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        exit(EXIT_FAILURE); \
    } \
}

#ifdef WARN
#undef WARN
#endif

#define WARN(fmt, ...) { \
    if (errno != 0) \
        warnx(fmt, ## __VA_ARGS__); \
    else { \
        fprintf(stdout, fmt, ## __VA_ARGS__); \
        fprintf(stdout, "\n"); \
   }\
}

#ifdef HAVE_ERROR_H
#include <error.h>
#define ERR_AT_LINE(filename, lineno, fmt, ...) \
    error_at_line(EXIT_FAILURE, 0, filename, lineno, fmt, ## __VA_ARGS__);
#else
#define ERR_AT_LINE(filename, lineno, fmt, ...) {\
    fprintf(stderr, "%s:%d :", filename, lineno); \
    if (errno != 0) \
        err(EXIT_FAILURE, fmt, ## __VA_ARGS__); \
    else \
        fprintf(stderr, fmt, ## __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    exit(EXIT_FAILURE); \
}
#endif

#ifndef HAVE_CANONICALIZE_FILE_NAME
static inline char * canonicalize_file_name(const char *path)
{
	char buf[PATH_MAX] = { };

	snprintf(buf, sizeof(buf) - 1, "%s", path);

	if (!realpath(path, buf))
		return NULL;

	return strdup(buf);
}
#endif

#endif /* __DAVFS_UTIL_H */
