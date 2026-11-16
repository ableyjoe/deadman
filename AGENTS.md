# AGENTS.md

Context for coding agents, and for anyone else, picking up deadman.
README.md says what it is and how to install it; the manual pages are the
reference. This says why it is the way it is. Read all of them before
changing anything.

## What it is, and for whom

A dead man's switch for OpenBSD, built from the base system: httpd(8) and
slowcgi(8) take the pings, cron(8) runs the checks, sendmail(8) (smtpd)
sends the mail. It was written to replace healthchecks.io for the
author's own jobs, starting with proton-firesafe
(github.com/ableyjoe/proton-firesafe), whose `HEARTBEAT_URL` points at it.
It should feel like part of OpenBSD: small, readable C in style(9),
pledge and unveil throughout, mdoc manual pages, `bsd.prog.mk`, and a
security argument that fits in a paragraph.

The author's deployment: OpenBSD 8.0 on amd64, a host that already runs
httpd with acme-client certificates and smtpd with dovecot, with the ping
URLs on their own name, a CNAME for that host. The author runs commands on
it himself: an agent cannot reach it, so give commands to type, one step
at a time, and ask for their output. He uses ksh and doas.

## Files

| File | What |
|---|---|
| `deadman.h` | paths, the token format, and the testing shims |
| `deadman/deadman.c` | the checker: `check`, `status`, `token` |
| `deadman.cgi/deadman.cgi.c` | the ping recorder |
| `deadman/deadman.8`, `deadman/deadman.conf.5`, `deadman.cgi/deadman.cgi.8` | the manual |
| `deadman.conf` | installed as `/etc/deadman.conf` if there is none |
| `tests/run-tests.sh` | the tests |

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
  again.
- **Revocation is removal.** Each `check` removes ping and state files
  that no configured check owns.
- **Testing builds only.** `-DDEADMAN_TESTING` lets the environment set
  the directories, sendmail and the time, and skips the privilege drop;
  off OpenBSD, `deadman.h` turns `pledge` and `unveil` into no-ops.
  Release builds have none of that: environment variables must never
  redirect a program that starts as root.

## Unverified until the first OpenBSD install

These were written from the manuals and from memory, and could not be
tried where this was written. Confirm each on OpenBSD, then move it to the
next section:

- httpd, given `root "/cgi-bin/deadman.cgi"` in a location, finds the
  script by trimming the request path, and passes the whole path as
  `PATH_INFO`, as cgit set-ups rely on.
- `utimes` works under `pledge("stdio rpath fattr")` with the directory
  unveiled `"w"`.
- `LDSTATIC= ${STATIC}` makes a static `deadman.cgi`.
- `bsd.prog.mk` runs `beforeinstall` and `afterinstall`.
- `_deadman`, without being in group `www`, can create files that are
  group `www` in a `_deadman:www` directory.

## Learnt on OpenBSD

Nothing yet.

## Testing

```sh
make test                                  # or: sh tests/run-tests.sh
mandoc -T lint -W warning deadman/*.[58] deadman.cgi/*.8
```

Every behaviour change gets tests. A bug fix gets a test that fails
without the fix. When adding tests, break the code on purpose and check
that they notice: tests that pass against broken code are no tests. Mind
the mechanics when doing that: a mutant that does not compile shows as a
"Bail out", not as failures, and a mutation script stopped early (piping
it into `head`, say) leaves its mutant in place.

## Conventions

- C in style(9). Warnings stay clean with the flags in the Makefiles.
- British English in the manual and in mail; plain words.
- Commit messages: a short imperative subject, then why.
- Propose design changes, and agree them with the author, before
  committing them.
