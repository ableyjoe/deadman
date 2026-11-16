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

#ifndef DEADMAN_H
#define DEADMAN_H

/* Where deadman.cgi records pings, as it sees it inside httpd's chroot. */
#define DEADMAN_PINGDIR_CHROOT	"/deadman"
/* The same directory, as deadman sees it. */
#define DEADMAN_PINGDIR		"/var/www/deadman"
#define DEADMAN_STATEDIR	"/var/db/deadman"
#define DEADMAN_CONF		"/etc/deadman.conf"
#define DEADMAN_USER		"_deadman"

/* A token is 32 lowercase hexadecimal digits: 128 random bits. */
#define DEADMAN_TOKEN_LEN	32

static inline int
deadman_valid_token(const char *s)
{
	int i;

	for (i = 0; i < DEADMAN_TOKEN_LEN; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return 0;
	return s[i] == '\0';
}

#ifndef __dead
#define __dead	__attribute__((__noreturn__))
#endif

/*
 * The tests build both programs on other systems, with -DDEADMAN_TESTING,
 * which also lets the environment override paths and the time. Release
 * builds have none of that.
 */
#ifndef __OpenBSD__
#define pledge(promises, execpromises)	0
#define unveil(path, permissions)	0
#endif

#endif /* DEADMAN_H */
