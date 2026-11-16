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
URLs on their own name, a CNAME for that host, DNS only. It runs 1.1,
from the package built from `port/`, in `_deadman`'s crontab. Its first
check is proton-firesafe's daily run on a Mac, with a period and grace of
a day each. The author runs commands on it himself: an agent cannot reach it,
so give commands to type, one step at a time, and ask for their output.
He uses ksh and doas. Tokens are capabilities: never put a real one, or
the author's ping URLs, in the repository.

`port/` is an OpenBSD port, for submission to the ports tree; see
"Releases, and the port" below.

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
| `tests/fuzz.py` | fuzzes both parsers under sanitizers; an extra, not run by `make test` |
| `port/` | the OpenBSD port, `sysutils/deadman`, laid out as in the ports tree |

`deadman/Makefile` installs `deadman` in `/usr/local/sbin`, its manual
pages in `/usr/local/man`, and `deadman.conf` as an example in
`/usr/local/share/examples/deadman`. Its `beforeinstall` refuses to go on
without the `_deadman` user, and its `afterinstall` makes the two
directories and installs `/etc/deadman.conf`, `root:_deadman` 0640, if
there is none.
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
- **The checker needs no privileges.** Since 1.1 it runs from
  `_deadman`'s crontab, as `_deadman`, which owns its two directories.
  The config is `root:_deadman` 0640: tokens are capabilities, but
  `_deadman` can already see them all as the names of its ping files, so
  reading the config as root kept nothing secret. Started as root, as by
  `doas deadman status`, it drops to `_deadman` (`setgroups`,
  `setresgid`, `setresuid`) before reading anything; started as anyone
  else, it stays as it is and file permissions decide. Like doas with
  `doas.conf`, it refuses a config not owned by root, writable by group
  or others, or readable by others, so a careless chmod cannot hand the
  tokens to every local user. It then unveils
  the config, its two directories and `sendmail`, pledges `stdio rpath
  wpath cpath fattr proc exec` (`stdio rpath` for `status` and `-n`), and
  only then reads the config. `tzset()` and `gethostname()` happen before
  unveil. 1.0 read the config as root and refused to run as anyone else.
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
  the directories, sendmail and the time, skips the privilege drop,
  expects the config to belong to whoever runs the tests rather than
  root, and
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

The tests, once made portable, passed on OpenBSD 8.0 too, with pledge
and unveil in force, at v1.0 and at v1.1, and in the ports framework,
which runs them as `_pbuild`, so the test of a config not owned by root
is skipped there.

And some things about the surroundings:

- Base has no curl and no git. ftp(1) fetches HTTPS (`ftp -V -o - URL`);
  git comes from packages.
- httpd will not load a TLS server whose certificate does not exist yet,
  so the port-80 server for acme-client comes first, then the
  certificate, then the TLS server.
- acme-client exits 2 when there was nothing to renew, so it does not go
  behind cron's `-n`, which would mail about that every day.
  `/etc/daily.local` suits it.
- Read in crontab(1), crontab(5) and cron's source, and borne out when
  the author's host moved to 1.1: cron runs a
  user's crontab with `/bin/sh`, whatever the login shell, so
  `_deadman`'s `/sbin/nologin` does not matter; `cron.allow` governs only
  who may run crontab(1), and root may install a crontab for any user
  with `crontab -u`. `MAILTO=root` sends cron's mail about a
  pseudo-user's jobs somewhere it is read, as crontab(5) suggests.
- A CDN's proxy in front of the ping name (Cloudflare's orange cloud)
  would see every token, and may turn away clients that are not browsers.
  The name should be DNS only.
- Mail is where it can fail without anyone hearing: deadman knows only
  that sendmail accepted the message. On the author's host, root's mail
  and deadman's were relayed to a provider that enforces DMARC
  (`p=reject; sp=reject; aspf=s`) and refused them all as spam until the
  host could pass. Now rspamd signs local mail, through `listen on
  socket filter rspamd` in smtpd.conf, and an SPF record on the host's
  name authorises it. The first real mails, a check going down and up
  again, arrived in November 2026.
- smtpd adds its domain to bare names in From, To and Cc (cron's `From:
  root (Cron Daemon)`, `mail root`'s `To: root`) only after filters have
  seen the message. So rspamd, as a filter, sees no domain in such a
  From (`use_domain_sign_local = "envelope"` makes it sign anyway), and
  any signature over those headers is spoilt by the rewriting. deadman
  writes every address in full, so its signatures survive; SPF covers
  the rest.

## Releases, and the port

`port/` is the port, `sysutils/deadman`, laid out as it would be in the
ports tree: copy it to `/usr/ports/mystuff/sysutils/deadman` to build
it. It follows the tree's conventions as they were in November 2026:
www/cgit for a CGI under `/var/www` with the rest under `/usr/local`
(`@cwd` in the packing list), net/soju and net/neighbot for `@newuser`
and `@sample`.

- The distfile is a release asset, `deadman-V.tar.gz`, as the ports
  template prefers to GitHub's generated archives.
- `_deadman` is UID and GID 906, the next free in
  `infrastructure/db/user.list` when the port was written. Check that it
  is still free when submitting, and include the line for that file.
- The package makes the user, `/etc/deadman.conf` (`root:_deadman`
  0640) from the example, and `/var/www/deadman` and `/var/db/deadman`,
  with `@sample`, which leaves alone any that already exist. `@newuser`
  likewise leaves an existing `_deadman` alone, whatever its UID. Nor
  does `@sample` change an existing config's mode, so going from 1.0 to
  1.1 needs the steps in the README's "Upgrading from 1.0".
- `pkg/README` is the README's installation steps, less the building.
- The port's `make test` runs `tests/run-tests.sh`.

To release version V:

1. Check that `make test` passes on OpenBSD.
2. Tag the release commit `vV`, with git or on GitHub's release page,
   which can make the tag (choose the commit as its target, not `main`).
   An agent's session may be allowed to push branches but not tags.
3. Make the distfile, which comes out the same every time, and upload it
   to a GitHub release for the tag:
   `git archive --format=tar --prefix=deadman-V/ vV | gzip -9n >deadman-V.tar.gz`
   `.gitattributes` keeps `port/` out of it, since a distfile cannot
   carry its own checksum.
4. In `port/`, set `V` and update `distinfo`, with `make makesum` on
   OpenBSD, or by hand: `SHA256` is base64, `SIZE` is bytes.

Building the port, as on the author's host for v1.0 (OpenBSD 8.0, with
`ports.tar.gz` from the release, checked with signify):

- `/etc/mk.conf` has `PORTS_PRIVSEP=Yes` and `SUDO=doas`, and
  `/etc/doas.conf` lets the author's user run commands as `_pbuild` and
  `_pfetch` without a password, as bsd.port.mk(5) suggests. Run `make`
  as yourself: `doas make` fails with "doas: Operation not permitted",
  because root has no rule for becoming `_pbuild`. `make
  fix-permissions` alone is run as root, once.
- Add the `_deadman` line to the local `user.list` before building.
- `make install` passes `TRUSTED_PKG_PATH` through doas, which a rule
  without `keepenv` or `setenv` drops, so `pkg_add` refuses the unsigned
  package and says it cannot find it. `doas pkg_add -D unsigned
  /usr/ports/packages/amd64/all/deadman-V.tgz` works.
- To replace an installed version with a newer package file, use
  `pkg_add -r -D unsigned FILE`. `-u` takes the names of installed
  packages and looks for them on `PKG_PATH`, so given a file it says
  "Problem finding" and does nothing. The update keeps
  `/etc/deadman.conf`, and says so.
- Going from 1.0 to 1.1 on the author's host followed the README's
  "Upgrading from 1.0": the config's group and mode first, then the
  package, then the deadman line out of root's crontab and into
  `_deadman`'s, in that order so that no two runs overlap.
- To replace an installation from source, remove its six files first
  (the two programs, three manual pages and the example), since
  `pkg_add` will not overwrite files no package owns. The user,
  `/etc/deadman.conf`, and the ping and state files stay. Cron fails
  each minute until the package is in, mailing root each time.
- For v1.0, `make test`, `make package`, `make port-lib-depends-check`
  and `portcheck` were all clean; for v1.1, `make test` and `make
  package` were.

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

They set `umask 027`, since deadman refuses a config others can read,
and the test of a config not owned by root runs only when the tests run
as root, since no one else can give a file away.

They build both programs with `-DDEADMAN_TESTING -Werror`, set `TZ=UTC`,
and run them through simulated days from a fixed time, `BASE`
(18 November 2026, 11:06:40 UTC); a stand-in for sendmail appends to a
mailbox file. `fresh` starts a scenario and runs deadman once, since pings
are turned away until it has made the ping files; `ping` calls the CGI as
httpd would and then moves the file's time to the simulated one; `at`
runs deadman at a time; `expect_mail` and `expect_no_mail` count new
messages.

`tests/fuzz.py` is an extra, not run by `make test`: it builds both
programs with gcc's address and undefined-behaviour sanitizers and throws
random requests at the CGI and random lines at the config parser,
comparing the answers with small models of what each should accept. It
found the overflow in `parse_duration` that 1.1 fixed. It needs python3
and gcc: `python3 tests/fuzz.py . /tmp/deadman-fuzz 6000 4000`.

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
