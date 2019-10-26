/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2018, 2019 Joachim Nilsson <troglobit@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <config.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SYSLOG_NAMES
#include <syslog.h>
#include <unistd.h>
#include <sys/stat.h>
#include "compat.h"

static const char version_info[] = PACKAGE_NAME " v" PACKAGE_VERSION;


static int create(char *path, mode_t mode, uid_t uid, gid_t gid)
{
	return mknod(path, S_IFREG | mode, 0) || chown(path, uid, gid);
}

/*
 * This function triggers a log rotates of @file when size >= @sz bytes
 * At most @num old versions are kept and by default it starts gzipping
 * .2 and older log files.  If gzip is not available in $PATH then @num
 * files are kept uncompressed.
 */
static int logrotate(char *file, int num, off_t sz)
{
	int cnt;
	struct stat st;

	if (stat(file, &st))
		return 1;

	if (sz > 0 && S_ISREG(st.st_mode) && st.st_size > sz) {
		if (num > 0) {
			size_t len = strlen(file) + 10 + 1;
			char   ofile[len];
			char   nfile[len];

			/* First age zipped log files */
			for (cnt = num; cnt > 2; cnt--) {
				snprintf(ofile, len, "%s.%d.gz", file, cnt - 1);
				snprintf(nfile, len, "%s.%d.gz", file, cnt);

				/* May fail because ofile doesn't exist yet, ignore. */
				(void)rename(ofile, nfile);
			}

			for (cnt = num; cnt > 0; cnt--) {
				snprintf(ofile, len, "%s.%d", file, cnt - 1);
				snprintf(nfile, len, "%s.%d", file, cnt);

				/* May fail because ofile doesn't exist yet, ignore. */
				(void)rename(ofile, nfile);

				if (cnt == 2 && !access(nfile, F_OK)) {
					size_t len = 5 + strlen(nfile) + 1;
					char cmd[len];

					snprintf(cmd, len, "gzip %s", nfile);
					system(cmd);

					remove(nfile);
				}
			}

			if (rename(file, nfile))
				(void)truncate(file, 0);
			else
				create(file, st.st_mode, st.st_uid, st.st_gid);
		} else {
			if (truncate(file, 0))
				syslog(LOG_ERR | LOG_PERROR, "Failed truncating %s during logrotate: %s", file, strerror(errno));
		}
	}

	return 0;
}

static int checksz(FILE *fp, off_t sz)
{
	struct stat st;

	if (sz <= 0)
		return 0;

	if (!fstat(fileno(fp), &st) && st.st_size > sz) {
		fclose(fp);
		return 1;
	}

	return 0;
}

static int logit(int level, char *buf, size_t len)
{
	if (buf[0]) {
		syslog(level, "%s", buf);
		return 0;
	}

	while ((fgets(buf, len, stdin)))
		syslog(level, "%s", buf);

	return 0;
}

static int flogit(char *logfile, int num, off_t sz, char *buf, size_t len)
{
	FILE *fp;

reopen:
	fp = fopen(logfile, "a");
	if (!fp) {
		syslog(LOG_ERR | LOG_PERROR, "Failed opening %s: %s", logfile, strerror(errno));
		return 1;
	}

	if (buf[0]) {
		fprintf(fp, "%s\n", buf);
		fsync(fileno(fp));
		if (checksz(fp, sz))
			return logrotate(logfile, num, sz);
	} else {
		while ((fgets(buf, len, stdin))) {
			fputs(buf, fp);
			fsync(fileno(fp));

			if (checksz(fp, sz)) {
				logrotate(logfile, num, sz);
				buf[0] = 0;
				goto reopen;
			}
		}
	}

	return fclose(fp);
}

static int parse_prio(char *arg, int *f, int *l)
{
	char *ptr;

	ptr = strchr(arg, '.');
	if (ptr) {
		*ptr++ = 0;

		for (int i = 0; facilitynames[i].c_name; i++) {
			if (!strcmp(facilitynames[i].c_name, arg)) {
				*f = facilitynames[i].c_val;
				break;
			}
		}

		arg = ptr;
	}

	for (int i = 0; prioritynames[i].c_name; i++) {
		if (!strcmp(prioritynames[i].c_name, arg)) {
			*l = prioritynames[i].c_val;
			break;
		}
	}

	return 0;
}

static int usage(int code)
{
	printf("Usage: logger [OPTIONS] [MESSAGE]\n"
	       "\n"
	       "Write MESSAGE (or stdin) to syslog, or file (with logrotate)\n"
	       "\n"
	       "  -p PRIO  Log message priority (numeric or facility.level pair)\n"
	       "  -t TAG   Log using the specified tag (defaults to user name)\n"
	       "  -s       Log to stderr as well as the system log\n"
	       "\n"
	       "  -f FILE  Log file to write messages to, instead of syslog daemon\n"
	       "  -r S:R   Log file rotation, default: 200 kB max \e[4ms\e[0mize, 5 \e[4mr\e[0motations\n"
	       "\n"
	       "  -?       This help text\n"
	       "  -v       Show program version\n"
	       "\n"
	       "This version of logger is distributed as part of sysklogd.\n"
	       "Bug report address: %s\n", PACKAGE_BUGREPORT);

	return code;
}

static void parse_rotation(char *optarg, off_t *size, int *num)
{
	char buf[100];
	char *c;
	int sz = 0, cnt = 0;

	strlcpy(buf, optarg, sizeof(buf));
	c = strchr(buf, ':');
	if (c) {
		*c++ = 0;
		cnt  = atoi(c);
	}

	sz = strtobytes(buf);
	if (sz > 0)
		*size = sz;
	if (cnt)
		*num = cnt;
}

int main(int argc, char *argv[])
{
	int c, rc, num = 5;
	int facility = LOG_USER;
	int level = LOG_INFO;
	int log_opts = LOG_NOWAIT;
	off_t size = 200 * 1024;
	char *ident = NULL, *logfile = NULL;
	char buf[512] = "";

	while ((c = getopt(argc, argv, "?f:p:r:st:v")) != EOF) {
		switch (c) {
		case 'f':
			logfile = optarg;
			break;

		case 'p':
			if (parse_prio(optarg, &facility, &level))
				return usage(1);
			break;

		case 'r':
			parse_rotation(optarg, &size, &num);
			break;

		case 's':
			log_opts |= LOG_PERROR;
			break;

		case 't':
			ident = optarg;
			break;

		case 'v':	/* version */
			fprintf(stderr, "%s\n", version_info);
			return 0;

		default:
			return usage(0);
		}
	}

	if (!ident)
		ident = getenv("LOGNAME") ?: getenv("USER");

	if (optind < argc) {
		size_t pos = 0, len = sizeof(buf);

		while (optind < argc) {
			size_t bytes;

			bytes = snprintf(&buf[pos], len, "%s ", argv[optind++]);
			pos += bytes;
			len -= bytes;
		}
	}

	openlog(ident, log_opts, facility);

	if (logfile)
		rc = flogit(logfile, num, size, buf, sizeof(buf));
	else
		rc = logit(level, buf, sizeof(buf));

	closelog();

	return rc;
}