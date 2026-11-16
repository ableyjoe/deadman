# deadman

A dead man's switch for OpenBSD. Jobs ping a URL of their own when they
run, and deadman sends mail when the pings stop: the core of what
healthchecks.io does, built from the base system, without a web framework,
an interpreter or a database.

```
a job ── GET https://deadman.example.org/ping/TOKEN ──▶ httpd(8) ─▶ slowcgi(8) ─▶ deadman.cgi
                                                      (chrooted in /var/www, as www)
                                                      sets the token's ping file's mtime

cron(8), every minute ─▶ deadman (as _deadman) ─▶ reads /etc/deadman.conf and the ping
                                                   files, and mails on any change
                                                   through sendmail(8)
```

## What it does

Each check has a period, how often it should ping, and a grace time, how
much longer to wait. It is *new* until its first ping, *up* while pings
come within the period, *late* once the period has passed, and *down* once
the grace time has passed too. deadman mails when a check goes down, and
again when it comes back up.

Like healthchecks.io, a job can also ping `/start` when it starts, so that
a run that hangs counts as down once the grace time has passed, and
`/fail` when it fails, which puts the check down at once.

What it leaves out: dashboards, accounts, teams, cron-expression
schedules, and any alert but mail. `deadman status` is the dashboard.

## Security

- **Nothing new listens on the network.** httpd and slowcgi, from base,
  do that.
- **The ping side can do almost nothing.** `deadman.cgi` is a few dozen
  lines of C, statically linked and run in httpd's chroot. It accepts only
  32 lowercase hexadecimal digits as a token, and it records a ping by
  setting the modification time of a file that `deadman` created for a
  configured check. It creates, writes and reads nothing; it repeats
  nothing from the request; an unknown token is a 404. `pledge("stdio
  rpath fattr")` and `unveil` confine it to that, and the ping directory
  is not readable by `www`, so it cannot even list the tokens.
- **The checking side gives up root at once.** `deadman` reads its
  configuration, which holds the tokens and is mode 0600, as root; then it
  becomes `_deadman`, and `pledge` and `unveil` confine it to its two
  directories and `sendmail`.
- **Tokens are 128 random bits**, made by `deadman token` with
  `arc4random`. Removing a check's line revokes its URL on the next run.

## Installing

On OpenBSD, as root where it says `doas`:

1. **A user for it:**

   ```sh
   doas useradd -c deadman -d /var/empty -s /sbin/nologin -g =uid _deadman
   ```

2. **Build and install.** git is a package, not part of base
   (`doas pkg_add git`). `make install` puts `deadman` in
   `/usr/local/sbin`, `deadman.cgi` in `/var/www/cgi-bin`, the manual pages
   in `/usr/local/man`, an example configuration in
   `/usr/local/share/examples/deadman`, and a copy of it in
   `/etc/deadman.conf` if there is none yet, and makes `/var/www/deadman`
   and `/var/db/deadman`:

   ```sh
   git clone https://github.com/ableyjoe/deadman
   cd deadman
   make
   doas make install
   ldd /var/www/cgi-bin/deadman.cgi    # should list nothing but deadman.cgi
   ```

   `deadman.cgi` runs in httpd's chroot, which has no shared libraries, so
   it must be static. Ask `ldd`, not `file`: OpenBSD links static programs
   as position-independent executables, which `file` calls "shared
   objects".

3. **slowcgi:**

   ```sh
   doas rcctl enable slowcgi
   doas rcctl start slowcgi
   ```

4. **A name, a certificate, and httpd.** Give the ping URLs a name of
   their own; a CNAME for the host will do. If the name's DNS is on a CDN
   such as Cloudflare, make the record DNS only: a proxy would see every
   token, and may turn away requests that do not come from a browser.

   Then, in this order, since httpd will not load a TLS server whose
   certificate does not exist yet:

   - in `/etc/httpd.conf`, a server for the name on port 80, for
     acme-client's challenges, like the first one in
     `/etc/examples/httpd.conf`; then `doas rcctl reload httpd`;
   - in `/etc/acme-client.conf`, a `domain` block for the name, like the
     one in `/etc/examples/acme-client.conf`; then
     `doas acme-client -v deadman.example.org`;
   - in `/etc/httpd.conf`, the TLS server from the example in
     `deadman.cgi(8)`; then `doas rcctl reload httpd` again.

   acme-client renews a certificate only when it is close to expiring, so
   it wants running daily. It exits 2 when there was nothing to renew,
   which cron's `-n` flag would take for a failure, so `/etc/daily.local`
   is the place for it, beside any other certificates the host has:

   ```sh
   acme-client deadman.example.org && rcctl reload httpd
   ```

5. **cron:** add this to root's crontab with `doas crontab -e`. It runs
   deadman every minute, one at a time, mailing root only if it fails:

   ```
   *	*	*	*	*	-sn /usr/local/sbin/deadman
   ```

6. **A check:** make a token, and add a line to `/etc/deadman.conf`
   (see `deadman.conf(5)`):

   ```sh
   deadman token
   doas vi /etc/deadman.conf
   ```

   Within a minute, deadman makes the check's ping files, and its URL
   starts working:

   ```sh
   ftp -V -o - https://deadman.example.org/ping/TOKEN    # says OK
   doas deadman status
   ```

## Pinging

From the end of a job, with `ftp(1)` on OpenBSD or `curl(1)` elsewhere:

```sh
ftp -o /dev/null -V https://deadman.example.org/ping/TOKEN
curl -fsS -m 10 --retry 3 -o /dev/null https://deadman.example.org/ping/TOKEN
```

Add `/start` to the URL when a job starts, or `/fail` when it fails. A
job that only pings when it succeeds is enough: silence is what deadman
watches for.

## Packaging

`port/` is a port for OpenBSD's ports tree, `sysutils/deadman`. Until
it is in the tree, copy it to `/usr/ports/mystuff/sysutils/deadman` and
build a package there. The package does steps 1 and 2 above: the user,
the files and directories, and `/etc/deadman.conf`. Its README, in
`/usr/local/share/doc/pkg-readmes/deadman`, gives the rest.

## Testing

```sh
make test
```

The tests build both programs with `-DDEADMAN_TESTING`, which lets the
environment set their directories and the time, and run them through
simulated days, with a stand-in for sendmail. They need only a C compiler
and a shell. On OpenBSD, the programs run under `pledge` and `unveil` as
they would in use, so the tests also catch anything those forbid; on
other systems, where `pledge` and `unveil` are no-ops, they cannot.
Release builds contain none of the testing code.

`make DESTDIR=/some/where install` installs into a staging directory, as
for a package, and leaves the `_deadman` user, its two directories and
`/etc/deadman.conf` to whatever installs the result.

[AGENTS.md](AGENTS.md) explains why deadman is the way it is, for anyone
changing it.

## Licence

ISC; see [LICENSE](LICENSE).
