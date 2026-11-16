/*
 * Copyright (c) 2026 Joe Abley <jabley@strandkip.nl>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * deadman.cgi: record a ping for deadman(8).
 *
 * slowcgi(8) runs it in httpd(8)'s chroot. The last component of
 * PATH_INFO is a check's token, optionally followed by "start" or "fail".
 * A ping is recorded by setting the modification time of the matching
 * file, which deadman(8) creates for each configured check, to now.
 * Nothing is created, written or read, and nothing from the request is
 * echoed back; an unknown token is a 404.
 */

#include <sys/types.h>
#include <sys/time.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "deadman.h"

static __dead void	respond(int, int, const char *);

static __dead void
respond(int head, int code, const char *reason)
{
	printf("Status: %d %s\r\n"
	    "Content-Type: text/plain\r\n"
	    "Cache-Control: no-store\r\n"
	    "\r\n", code, reason);
	if (!head)
		printf("%s\n", reason);
	exit(0);
}

int
main(void)
{
	char		 buf[256], file[PATH_MAX];
	const char	*dir = DEADMAN_PINGDIR_CHROOT;
	const char	*method, *path, *suffix = "";
	char		*slash, *token;
	int		 head;

#ifdef DEADMAN_TESTING
	if (getenv("DEADMAN_PINGDIR") != NULL)
		dir = getenv("DEADMAN_PINGDIR");
#endif
	if (unveil(dir, "w") == -1 || pledge("stdio rpath fattr", NULL) == -1)
		respond(0, 500, "Internal Server Error");

	method = getenv("REQUEST_METHOD");
	if (method == NULL)
		respond(0, 400, "Bad Request");
	head = strcmp(method, "HEAD") == 0;
	if (!head && strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0)
		respond(0, 405, "Method Not Allowed");

	path = getenv("PATH_INFO");
	if (path == NULL || strlcpy(buf, path, sizeof(buf)) >= sizeof(buf))
		respond(head, 404, "Not Found");

	/* .../TOKEN, .../TOKEN/start or .../TOKEN/fail */
	if ((slash = strrchr(buf, '/')) == NULL)
		respond(head, 404, "Not Found");
	token = slash + 1;
	if (strcmp(token, "start") == 0 || strcmp(token, "fail") == 0) {
		suffix = strcmp(token, "start") == 0 ? ".start" : ".fail";
		*slash = '\0';
		if ((slash = strrchr(buf, '/')) == NULL)
			respond(head, 404, "Not Found");
		token = slash + 1;
	}
	if (!deadman_valid_token(token))
		respond(head, 404, "Not Found");

	if (snprintf(file, sizeof(file), "%s/%s%s", dir, token, suffix) >=
	    (int)sizeof(file))
		respond(head, 500, "Internal Server Error");
	if (utimes(file, NULL) == -1) {
		if (errno == ENOENT)
			respond(head, 404, "Not Found");
		respond(head, 500, "Internal Server Error");
	}
	respond(head, 200, "OK");
	return 0;
}
