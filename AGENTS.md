# AGENTS.md

Context for coding agents, and for anyone else, picking up deadman.
README.md says what it is and how to install it; the manual pages are the
reference. This says why it is the way it is, and what was learnt
deploying it. Read all of them before changing anything: the code and
tests come to about 1200 lines, so reading them all is quicker than
guessing.

## What it is, and for whom

A dead man's switch for OpenBSD, built from the base system: httpd(8) and
slowcgi(8) take the pings, cron(8) runs the checks, sendmail(8) (smtpd)
sends the mail. It was written to replace healthchecks.io for the
author's own jobs, starting with proton-firesafe
(github.com/ableyjoe/proton-firesafe), whose `HEARTBEAT_URL` points at it.
It should feel like part of OpenBSD: small, readable C in style(9),
pledge and unveil throughout, mdoc manual pages, `bsd.prog.mk`, and a
security argument that fits in a paragraph.

The author's deployment, in use since November 2026: OpenBSD 8.0 on
amd64, a host that already runs httpd with acme-client certificates
(renewed from `/etc/daily.local`) and smtpd with dovecot, with the ping
URLs on their own name, a CNAME for that host, DNS only. Its first check
is proton-firesafe's daily run on a Mac, with a period and grace of a day
each. The author runs commands on it himself: an agent cannot reach it,
so give commands to type, one step at a time, and ask for their output.
He uses ksh and doas. Tokens are capabilities: never put a real one, or
the author's ping URLs, in the repository.

Next: an OpenBSD port, to submit to the ports maintainers.

## Files

| File | What |
|---|---|
| `deadman.h` | paths, the token format, and the testing shims |
| `deadman/deadman.c` | the checker: `check`, `status`, `token` |
| `deadman.cgi/deadman.cgi.c` | the ping recorder |
| `deadman/deadman.8`, `deadman/deadman.conf.5`, `deadman.cgi/deadman.cgi.8` | the manual |
| `deadman.conf` | installed as `/etc/deadman.conf` if there is none |
| `Makefile`, `deadman/Makefile`, `deadman.cgi/Makefile` | `bsd.subdir.mk` and `bsd.prog.mk`; `make test` |
| `tests/run-tests.sh` | the tests |
| `tests/filetime.c` | gets and sets file times for the tests, since stat(1) and touch(1) differ between systems |

`deadman/Makefile` installs `deadman` in `/usr/local/sbin`, its manual
pages in `/usr/local/man`, and `deadman.conf` as an example in
`/usr/local/share/examples/deadman`. Its `beforeinstall` refuses to go on
without the `_deadman` user, and its `afterinstall` makes the two
directories and installs `/etc/deadman.conf`, 0600, if there is none.
`deadman.cgi/Makefile` installs `deadman.cgi`, static, in
`/var/www/cgi-bin`.

With `DESTDIR` set, as when the ports framework stages a package, both
Makefiles make the directories they install into, and skip the user
check, the two directories and `/etc/deadman.conf`: the package does
those, and the build machine may have no `_deadman`. The ports framework
also strips `-o` and `-g` from `install` while staging.

## Design decisions, and why

- **No daemon, no listener, no database.** httpd and slowcgi already
  handle the network and run CGI programs in a chroot; cron already runs
  things every minute; smtpd already sends mail. deadman adds two small
  programs between them, and its whole state is file modification times
  and one small file per check.
- **A ping is a modification time.** deadman creates three files per
  check in `/var/www/deadman`, named after its token, with no suffix,
  `.start` and `.fail`, mode 0620, group `www` (inherited from the
  directory, as on any BSD), dated the epoch to mean never. The CGI calls
  `utimes(path, NULL)`, which needs only write permission and creates
  nothing, so an unknown token is ENOENT, a 404, and random tokens cannot
  fill the disk. The directory is 0710 `_deadman:www`, so the CGI can
  reach the files but not list them.
- **The URL.** The CGI takes the last component of `PATH_INFO` as the
  token, or the one before it if the last is `start` or `fail`; what comes
  before does not matter, since httpd.conf decides which paths reach it.
  GET, HEAD and POST work; anything else is a 405. The body is the status
  line's reason (`OK`, `Not Found`), as text/plain with
  `Cache-Control: no-store`, so it repeats nothing from the request.
- **Validation as well as existence.** The CGI also refuses anything that
  is not exactly 32 lowercase hexadecimal digits, before touching the file
  system, as a second line of defence. The tests plant a file with a
  malformed name to check that this layer works on its own.
- **The checker reads its config as root, then drops privileges** to
  `_deadman` (`setgroups`, `setresgid`, `setresuid`), and then unveils its
  two directories and `sendmail`, and pledges `stdio rpath wpath cpath
  fattr proc exec` (`stdio rpath` for `status` and `-n`). The config is
  0600 root because tokens are capabilities. `tzset()` and `gethostname()`
  happen before unveil.
- **States follow healthchecks.io's simple schedules:** new, up, late,
  down, from the last success, start and fail times. A start does not
  change the state, only the time a run has to finish (`grace`): a check
  that is down stays down until a success. Mail goes on entering down and
  on returning to up, never on late. The state file records only changes;
  if mail cannot be sent, the state is not written, so the next run tries
  again, and deadman exits non-zero, so that cron mails root.
- **Revocation is removal.** Each `check` removes ping and state files
  that no configured check owns.
- **`status` is the dashboard**, so its output is for people and for
  awk: fixed columns, no blanks at the ends of lines.
- **Testing builds only.** `-DDEADMAN_TESTING` lets the environment set
  the directories, sendmail and the time, skips the privilege drop, and
  unveils `/bin/sh` too, because the tests' stand-in for sendmail is a
  script, whose interpreter may need unveiling for it to run (cheaper to
  allow than to find out); off OpenBSD, `deadman.h` turns
  `pledge` and `unveil` into no-ops. Release builds have none of that:
  environment variables must never redirect a program that starts as
  root.

## Learnt on OpenBSD

All five assumptions the code was written on held at the first install,
on OpenBSD 8.0 amd64, in November 2026:

- httpd, given `root "/cgi-bin/deadman.cgi"` in a `location "/ping/*"`,
  runs the CGI for every path under it, and passes the whole path,
  `/ping/TOKEN`, as `PATH_INFO`.
- `utimes(path, NULL)` works under `pledge("stdio rpath fattr")` with the
  directory unveiled `"w"`.
- `LDSTATIC= ${STATIC}` makes `deadman.cgi` static. file(1) calls it a
  shared object, because OpenBSD links static programs as PIE; `ldd`
  listing nothing but the program is the test.
- `bsd.prog.mk` runs `beforeinstall` and `afterinstall`.
- `_deadman`, without being in group `www`, creates files that are group
  `www` in its `_deadman:www` directory.

And some things about the surroundings:

- Base has no curl and no git. ftp(1) fetches HTTPS (`ftp -V -o - URL`);
  git comes from packages.
- httpd will not load a TLS server whose certificate does not exist yet,
  so the port-80 server for acme-client comes first, then the
  certificate, then the TLS server.
- acme-client exits 2 when there was nothing to renew, so it does not go
  behind cron's `-n`, which would mail about that every day.
  `/etc/daily.local` suits it.
- A CDN's proxy in front of the ping name (Cloudflare's orange cloud)
  would see every token, and may turn away clients that are not browsers.
  The name should be DNS only.

## Testing

```sh
make test                                  # or: sh tests/run-tests.sh
mandoc -T lint -W warning deadman/*.[58] deadman.cgi/*.8
```

The tests need only a C compiler and a shell, and must run on OpenBSD,
where the port runs them and where pledge and unveil are real, as well as
on Linux. So the script is plain POSIX sh, with no GNU options:
`tests/filetime.c` stands in for `stat -c` and `touch -d @SECONDS`, and a
subshell with `unset` for `env -u`. Try it under dash, `bash --posix` and
mksh (a pdksh, like OpenBSD's sh) as well as sh, with gcc and clang.

They build both programs with `-DDEADMAN_TESTING -Werror`, set `TZ=UTC`,
and run them through simulated days from a fixed time, `BASE`
(18 November 2026, 11:06:40 UTC); a stand-in for sendmail appends to a
mailbox file. `fresh` starts a scenario and runs deadman once, since pings
are turned away until it has made the ping files; `ping` calls the CGI as
httpd would and then moves the file's time to the simulated one; `at`
runs deadman at a time; `expect_mail` and `expect_no_mail` count new
messages.

Every behaviour change gets tests. A bug fix gets a test that fails
without the fix. When adding tests, break the code on purpose and check
that they notice: tests that pass against broken code are no tests. Mind
the mechanics when doing that: a mutant that does not compile shows as a
"Bail out", not as failures, and a mutation script stopped early (piping
it into `head`, say) leaves its mutant in place.

## Conventions

- C in style(9). Warnings stay clean with the flags in the Makefiles.
- British English in the manual and in mail; plain words; no Latin
  plurals for English words.
- Keep README.md and the manual pages in step with the code in the same
  commit.
- Commit messages: a short imperative subject, then why.
- Propose design changes, and agree them with the author, before
  committing them.
