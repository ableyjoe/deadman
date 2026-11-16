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
 * deadman: send mail when the things that ping deadman.cgi(8) stop.
 *
 * Run from cron(8) as root, it reads deadman.conf(5), drops to the
 * _deadman user, makes sure each check has its ping files, works out from
 * their modification times whether each check is up, late or down, and
 * sends mail when one goes down or comes back up.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <paths.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "deadman.h"

enum state { S_NEW, S_UP, S_LATE, S_DOWN };
static const char *state_names[] = { "new", "up", "late", "down" };

/* Why a check is down. */
enum why { W_NONE, W_SILENT, W_FAILED, W_UNFINISHED };

struct check {
	char	*name;
	char	*token;
	time_t	 period;
	time_t	 grace;
	char	*mailto;
	time_t	 ping;		/* when each kind of ping last came; 0 for never */
	time_t	 start;
	time_t	 fail;
};

static const char *suffixes[] = { "", ".start", ".fail" };

static struct check	*checks;
static size_t		 nchecks;
static const char	*pingdir = DEADMAN_PINGDIR;
static const char	*statedir = DEADMAN_STATEDIR;
static const char	*sendmail = _PATH_SENDMAIL;
static char		 hostname[HOST_NAME_MAX + 1];
static time_t		 now;
static int		 noaction;

static __dead void
usage(void)
{
	fprintf(stderr, "usage: deadman [-n] [-f file] [check | status | token]\n");
	exit(1);
}

/* "90s", "15m", "36h", "1d" or "2w", to seconds. */
static int
parse_duration(const char *s, time_t *t)
{
	char		*end;
	long long	 n;

	errno = 0;
	n = strtoll(s, &end, 10);
	if (end == s || errno != 0 || n <= 0 || end[0] == '\0' || end[1] != '\0')
		return -1;
	switch (end[0]) {
	case 's':
		break;
	case 'm':
		n *= 60;
		break;
	case 'h':
		n *= 60 * 60;
		break;
	case 'd':
		n *= 24 * 60 * 60;
		break;
	case 'w':
		n *= 7 * 24 * 60 * 60;
		break;
	default:
		return -1;
	}
	if (n > 10LL * 366 * 24 * 60 * 60)	/* over ten years: a typo */
		return -1;
	*t = n;
	return 0;
}

/* "1 day", "36 hours", "90 minutes" */
static const char *
fmt_duration(time_t t, char *buf, size_t size)
{
	static const struct {
		time_t		 secs;
		const char	*unit;
	} units[] = {
		{ 7 * 24 * 60 * 60, "week" },
		{ 24 * 60 * 60, "day" },
		{ 60 * 60, "hour" },
		{ 60, "minute" },
		{ 1, "second" },
	};
	size_t	 i;

	for (i = 0; t % units[i].secs != 0; i++)
		;
	t /= units[i].secs;
	snprintf(buf, size, "%lld %s%s", (long long)t, units[i].unit,
	    t == 1 ? "" : "s");
	return buf;
}

static const char *
fmt_time(time_t t, char *buf, size_t size)
{
	struct tm	*tm;

	if (t == 0)
		strlcpy(buf, "never", size);
	else if ((tm = localtime(&t)) == NULL ||
	    strftime(buf, size, "%Y-%m-%d %H:%M %Z", tm) == 0)
		strlcpy(buf, "?", size);
	return buf;
}

static int
valid_name(const char *s)
{
	size_t	 len = strlen(s), i;

	if (len == 0 || len > 64 || s[0] == '.' || s[0] == '-')
		return 0;
	for (i = 0; i < len; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
		    (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' ||
		    s[i] == '-'))
			return 0;
	return 1;
}

static int
valid_mailto(const char *s)
{
	if (*s == '\0' || *s == '-')
		return 0;
	for (; *s != '\0'; s++)
		if (*s <= ' ' || *s > '~')
			return 0;
	return 1;
}

static void
add_check(const char *file, int lineno, char **field)
{
	struct check	*c;
	size_t		 i;

	if (!valid_name(field[0]))
		errx(1, "%s:%d: bad name: %s", file, lineno, field[0]);
	if (!deadman_valid_token(field[1]))
		errx(1, "%s:%d: a token is 32 lowercase hexadecimal digits; "
		    "deadman token makes one", file, lineno);
	for (i = 0; i < nchecks; i++) {
		if (strcmp(checks[i].name, field[0]) == 0)
			errx(1, "%s:%d: %s is defined twice", file, lineno,
			    field[0]);
		if (strcmp(checks[i].token, field[1]) == 0)
			errx(1, "%s:%d: %s has the same token as %s", file,
			    lineno, field[0], checks[i].name);
	}
	if ((checks = reallocarray(checks, nchecks + 1, sizeof(*checks))) == NULL)
		err(1, NULL);
	c = &checks[nchecks++];
	memset(c, 0, sizeof(*c));
	if (parse_duration(field[2], &c->period) == -1)
		errx(1, "%s:%d: bad period: %s", file, lineno, field[2]);
	if (parse_duration(field[3], &c->grace) == -1)
		errx(1, "%s:%d: bad grace: %s", file, lineno, field[3]);
	if (!valid_mailto(field[4]))
		errx(1, "%s:%d: bad mailto: %s", file, lineno, field[4]);
	if ((c->name = strdup(field[0])) == NULL ||
	    (c->token = strdup(field[1])) == NULL ||
	    (c->mailto = strdup(field[4])) == NULL)
		err(1, NULL);
}

static void
read_config(const char *file)
{
	FILE	*fp;
	char	*line = NULL, *p, *field[5];
	size_t	 linesize = 0;
	int	 lineno = 0, n;

	if ((fp = fopen(file, "r")) == NULL)
		err(1, "%s", file);
	while (getline(&line, &linesize, fp) != -1) {
		lineno++;
		if ((p = strchr(line, '#')) != NULL)
			*p = '\0';
		n = 0;
		for (p = strtok(line, " \t\r\n"); p != NULL;
		    p = strtok(NULL, " \t\r\n")) {
			if (n == 5)
				errx(1, "%s:%d: too many fields", file, lineno);
			field[n++] = p;
		}
		if (n == 0)
			continue;
		if (n != 5)
			errx(1, "%s:%d: expected name, token, period, grace "
			    "and mailto", file, lineno);
		add_check(file, lineno, field);
	}
	if (ferror(fp))
		err(1, "%s", file);
	free(line);
	fclose(fp);
}

static void
drop_privileges(void)
{
	struct passwd	*pw;

#ifdef DEADMAN_TESTING
	return;
#endif
	if (geteuid() != 0)
		errx(1, "must be run as root");
	if ((pw = getpwnam(DEADMAN_USER)) == NULL)
		errx(1, "there is no %s user", DEADMAN_USER);
	if (setgroups(1, &pw->pw_gid) == -1 ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) == -1 ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid) == -1)
		err(1, "cannot drop privileges to %s", DEADMAN_USER);
}

static void
ping_path(char *buf, size_t size, const struct check *c, int kind)
{
	if (snprintf(buf, size, "%s/%s%s", pingdir, c->token, suffixes[kind]) >=
	    (int)size)
		errx(1, "%s: path too long", pingdir);
}

/*
 * Read when each kind of ping for c last came. With create, also make any
 * ping file that is missing, dated the epoch so that it reads as never.
 */
static void
read_pings(struct check *c, int create)
{
	struct timeval	 never[2] = { { 0, 0 }, { 0, 0 } };
	struct stat	 sb;
	char		 path[PATH_MAX];
	time_t		*t[] = { &c->ping, &c->start, &c->fail };
	int		 fd, kind;

	for (kind = 0; kind < 3; kind++) {
		ping_path(path, sizeof(path), c, kind);
		*t[kind] = 0;
		if (lstat(path, &sb) == 0) {
			if (!S_ISREG(sb.st_mode))
				errx(1, "%s: not a regular file", path);
			*t[kind] = sb.st_mtime;
			continue;
		}
		if (errno != ENOENT)
			err(1, "%s", path);
		if (!create)
			continue;
		if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
		    0620)) == -1)
			err(1, "%s", path);
		if (fchmod(fd, 0620) == -1 || futimes(fd, never) == -1)
			err(1, "%s", path);
		close(fd);
	}
}

static int
known_ping_file(const char *name)
{
	size_t	 i;
	int	 kind;
	char	 want[DEADMAN_TOKEN_LEN + 8];

	for (i = 0; i < nchecks; i++)
		for (kind = 0; kind < 3; kind++) {
			snprintf(want, sizeof(want), "%s%s", checks[i].token,
			    suffixes[kind]);
			if (strcmp(name, want) == 0)
				return 1;
		}
	return 0;
}

static int
known_state_file(const char *name)
{
	size_t	 i;

	for (i = 0; i < nchecks; i++)
		if (strcmp(name, checks[i].name) == 0)
			return 1;
	return 0;
}

/* Remove what no configured check owns, such as a revoked token's pings. */
static void
remove_stale(const char *dir, int (*known)(const char *))
{
	DIR		*d;
	struct dirent	*de;
	char		 path[PATH_MAX];

	if ((d = opendir(dir)) == NULL)
		err(1, "%s", dir);
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 ||
		    known(de->d_name))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >=
		    (int)sizeof(path))
			continue;
		if (unlink(path) == -1)
			warn("%s", path);
	}
	closedir(d);
}

/*
 * Where c stands now; for one that is down, why; when the next ping is
 * due; and whether a run has started and not yet finished. A start does
 * not change where a check stands, as with healthchecks.io: only how
 * long it has to finish.
 */
static enum state
assess(const struct check *c, enum why *why, time_t *due, int *running)
{
	*why = W_NONE;
	*due = 0;
	*running = 0;
	if (c->ping == 0 && c->start == 0 && c->fail == 0)
		return S_NEW;
	if (c->fail > c->ping && c->fail >= c->start) {
		*why = W_FAILED;
		return S_DOWN;
	}
	if (c->start > c->ping) {
		*due = c->start + c->grace;
		if (now > *due) {
			*why = W_UNFINISHED;
			return S_DOWN;
		}
		*running = 1;
	}
	if (c->fail > c->ping) {	/* failed before this run started */
		*why = W_FAILED;
		return S_DOWN;
	}
	if (c->ping == 0)
		return S_NEW;
	if (!*running)
		*due = c->ping + c->period;
	if (now <= c->ping + c->period)
		return S_UP;
	if (now <= c->ping + c->period + c->grace)
		return S_LATE;
	*why = W_SILENT;
	return S_DOWN;
}

static void
read_state(const struct check *c, enum state *state, time_t *since)
{
	FILE		*fp;
	char		 path[PATH_MAX], name[16];
	long long	 t;
	int		 i;

	*state = S_NEW;
	*since = 0;
	snprintf(path, sizeof(path), "%s/%s", statedir, c->name);
	if ((fp = fopen(path, "r")) == NULL) {
		if (errno != ENOENT)
			err(1, "%s", path);
		return;
	}
	if (fscanf(fp, "%15s %lld", name, &t) == 2)
		for (i = S_NEW; i <= S_DOWN; i++)
			if (strcmp(name, state_names[i]) == 0) {
				*state = i;
				*since = t;
			}
	fclose(fp);
}

static void
write_state(const struct check *c, enum state state)
{
	char	 path[PATH_MAX], tmp[PATH_MAX];
	FILE	*fp;
	int	 fd;

	snprintf(path, sizeof(path), "%s/%s", statedir, c->name);
	snprintf(tmp, sizeof(tmp), "%s/.%s.XXXXXXXXXX", statedir, c->name);
	if ((fd = mkstemp(tmp)) == -1)
		err(1, "%s", tmp);
	if ((fp = fdopen(fd, "w")) == NULL)
		err(1, "%s", tmp);
	fprintf(fp, "%s %lld\n", state_names[state], (long long)now);
	if (fflush(fp) == EOF || ferror(fp) || fclose(fp) == EOF)
		err(1, "%s", tmp);
	if (rename(tmp, path) == -1)
		err(1, "%s", path);
}

static int
send_mail(const char *to, const char *subject, const char *body)
{
	FILE	*fp;
	pid_t	 pid;
	int	 fds[2], status;

	if (noaction) {
		printf("To: %s\nSubject: %s\n\n%s\n\n", to, subject, body);
		return 0;
	}
	if (pipe(fds) == -1) {
		warn("pipe");
		return -1;
	}
	switch (pid = fork()) {
	case -1:
		warn("fork");
		close(fds[0]);
		close(fds[1]);
		return -1;
	case 0:
		if (dup2(fds[0], STDIN_FILENO) == -1)
			_exit(1);
		close(fds[0]);
		close(fds[1]);
		execl(sendmail, "sendmail", "-oi", "-t", (char *)NULL);
		_exit(127);
	}
	close(fds[0]);
	if ((fp = fdopen(fds[1], "w")) == NULL) {
		warn("fdopen");
		close(fds[1]);
	} else {
		fprintf(fp, "To: %s\n"
		    "Subject: %s\n"
		    "Auto-Submitted: auto-generated\n"
		    "\n"
		    "%s\n"
		    "\n"
		    "-- \n"
		    "deadman on %s\n", to, subject, body, hostname);
		fclose(fp);
	}
	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR) {
			warn("waitpid");
			return -1;
		}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		warnx("%s failed to send mail about %s", sendmail, to);
		return -1;
	}
	return 0;
}

/* "every day", "every 36 hours" */
static const char *
fmt_every(time_t t, char *buf, size_t size)
{
	char	 d[32];

	fmt_duration(t, d, sizeof(d));
	snprintf(buf, size, "every %s", strncmp(d, "1 ", 2) == 0 ? d + 2 : d);
	return buf;
}

static int
notify_down(const struct check *c, enum why why)
{
	char	 subject[128], body[512], t[64], every[64], grace[32];

	snprintf(subject, sizeof(subject), "[deadman] %s is down", c->name);
	fmt_duration(c->grace, grace, sizeof(grace));
	switch (why) {
	case W_FAILED:
		snprintf(body, sizeof(body), "%s is down: it reported a failure "
		    "at %s.", c->name, fmt_time(c->fail, t, sizeof(t)));
		break;
	case W_UNFINISHED:
		snprintf(body, sizeof(body), "%s is down: it started at %s, but\n"
		    "has not finished within its grace time of %s.", c->name,
		    fmt_time(c->start, t, sizeof(t)), grace);
		break;
	default:
		snprintf(body, sizeof(body), "%s is down: its last ping was at "
		    "%s.\nIt should ping %s, with a grace time of %s.", c->name,
		    fmt_time(c->ping, t, sizeof(t)),
		    fmt_every(c->period, every, sizeof(every)), grace);
		break;
	}
	return send_mail(c->mailto, subject, body);
}

static int
notify_up(const struct check *c, time_t since)
{
	char	 subject[128], body[512], t1[64], t2[64];

	snprintf(subject, sizeof(subject), "[deadman] %s is up again", c->name);
	snprintf(body, sizeof(body), "%s is up again: it pinged at %s,\n"
	    "after being down since %s.", c->name,
	    fmt_time(c->ping > c->start ? c->ping : c->start, t1, sizeof(t1)),
	    fmt_time(since, t2, sizeof(t2)));
	return send_mail(c->mailto, subject, body);
}

static int
cmd_check(void)
{
	struct check	*c;
	enum state	 prev, state;
	enum why	 why;
	time_t		 since, due;
	size_t		 i;
	int		 running, sent, ret = 0;

	for (i = 0; i < nchecks; i++)
		read_pings(&checks[i], !noaction);
	if (!noaction) {
		remove_stale(pingdir, known_ping_file);
		remove_stale(statedir, known_state_file);
	}

	for (i = 0; i < nchecks; i++) {
		c = &checks[i];
		read_state(c, &prev, &since);
		state = assess(c, &why, &due, &running);
		if (state == prev)
			continue;
		sent = 0;
		if (state == S_DOWN)
			sent = notify_down(c, why);
		else if (prev == S_DOWN && state == S_UP)
			sent = notify_up(c, since);
		/* Unsent mail is tried again on the next run. */
		if (sent == -1)
			ret = 1;
		else if (!noaction)
			write_state(c, state);
	}
	return ret;
}

/*
 * One line of status output, without the blanks that the padding leaves at
 * its end when the last columns are empty.
 */
static void
status_line(int width, const char *name, const char *state, const char *last,
    const char *due, const char *note)
{
	char	 line[256];
	size_t	 n;

	snprintf(line, sizeof(line), "%-*s  %-5s  %-21s  %s%s", width, name,
	    state, last, due, note);
	n = strlen(line);
	while (n > 0 && line[n - 1] == ' ')
		line[--n] = '\0';
	puts(line);
}

static int
cmd_status(void)
{
	struct check	*c;
	enum state	 state;
	enum why	 why;
	time_t		 due;
	size_t		 i;
	int		 running, width = 4;
	char		 last[64], next[64];
	const char	*note;

	for (i = 0; i < nchecks; i++)
		if ((int)strlen(checks[i].name) > width)
			width = strlen(checks[i].name);
	status_line(width, "NAME", "STATE", "LAST PING", "DUE", "");
	for (i = 0; i < nchecks; i++) {
		c = &checks[i];
		read_pings(c, 0);
		state = assess(c, &why, &due, &running);
		note = running ? "  (running)" :
		    why == W_FAILED ? "  (reported a failure)" :
		    why == W_UNFINISHED ? "  (started, never finished)" : "";
		status_line(width, c->name, state_names[state],
		    fmt_time(c->ping, last, sizeof(last)),
		    due == 0 ? "" : fmt_time(due, next, sizeof(next)), note);
	}
	return 0;
}

static int
cmd_token(void)
{
	unsigned char	 r[DEADMAN_TOKEN_LEN / 2];
	size_t		 i;

	arc4random_buf(r, sizeof(r));
	for (i = 0; i < sizeof(r); i++)
		printf("%02x", r[i]);
	printf("\n");
	return 0;
}

int
main(int argc, char *argv[])
{
	const char	*conffile = DEADMAN_CONF, *cmd = "check";
	int		 ch;

	while ((ch = getopt(argc, argv, "f:n")) != -1) {
		switch (ch) {
		case 'f':
			conffile = optarg;
			break;
		case 'n':
			noaction = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();
	if (argc == 1)
		cmd = argv[0];
	if (strcmp(cmd, "check") != 0 && strcmp(cmd, "status") != 0 &&
	    strcmp(cmd, "token") != 0)
		usage();

	if (strcmp(cmd, "token") == 0) {
		if (pledge("stdio", NULL) == -1)
			err(1, "pledge");
		return cmd_token();
	}

#ifdef DEADMAN_TESTING
	if (getenv("DEADMAN_PINGDIR") != NULL)
		pingdir = getenv("DEADMAN_PINGDIR");
	if (getenv("DEADMAN_STATEDIR") != NULL)
		statedir = getenv("DEADMAN_STATEDIR");
	if (getenv("DEADMAN_SENDMAIL") != NULL)
		sendmail = getenv("DEADMAN_SENDMAIL");
#endif
	signal(SIGPIPE, SIG_IGN);
	tzset();
	if (gethostname(hostname, sizeof(hostname)) == -1)
		err(1, "gethostname");
	read_config(conffile);
	drop_privileges();

	if (strcmp(cmd, "status") == 0 || noaction) {
		if (unveil(pingdir, "r") == -1 || unveil(statedir, "r") == -1 ||
		    unveil(NULL, NULL) == -1)
			err(1, "unveil");
		if (pledge("stdio rpath", NULL) == -1)
			err(1, "pledge");
	} else {
		if (unveil(pingdir, "rwc") == -1 ||
		    unveil(statedir, "rwc") == -1 ||
		    unveil(sendmail, "x") == -1)
			err(1, "unveil");
#ifdef DEADMAN_TESTING
		/* The tests' stand-in for sendmail is a shell script. */
		if (unveil("/bin/sh", "x") == -1)
			err(1, "unveil");
#endif
		if (unveil(NULL, NULL) == -1)
			err(1, "unveil");
		if (pledge("stdio rpath wpath cpath fattr proc exec", NULL) == -1)
			err(1, "pledge");
	}

	now = time(NULL);
#ifdef DEADMAN_TESTING
	if (getenv("DEADMAN_NOW") != NULL)
		now = strtoll(getenv("DEADMAN_NOW"), NULL, 10);
#endif
	if (strcmp(cmd, "status") == 0)
		return cmd_status();
	return cmd_check();
}
