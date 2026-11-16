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

2. **Build and install.** This puts `deadman` in `/usr/local/sbin`,
   `deadman.cgi` in `/var/www/cgi-bin`, the manual pages in
   `/usr/local/man`, and an empty `/etc/deadman.conf`, and makes
   `/var/www/deadman` and `/var/db/deadman`:

   ```sh
   make
   doas make install
   file /var/www/cgi-bin/deadman.cgi    # should say "statically linked"
   ```

3. **slowcgi:**

   ```sh
   doas rcctl enable slowcgi
   doas rcctl start slowcgi
   ```

4. **httpd:** add a server for the ping URLs, with the example in
   `deadman.cgi(8)`, and a certificate for its name from acme-client. Then
   `doas rcctl reload httpd`.

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
   curl -fsS https://deadman.example.org/ping/TOKEN; echo
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

## Testing

```sh
make test
```

The tests build both programs with `-DDEADMAN_TESTING`, which lets the
environment set their directories and the time, and run them through
simulated days, with a stand-in for sendmail. They need only a C compiler
and a shell, so they run off OpenBSD too, but there `pledge` and `unveil`
are no-ops, and only OpenBSD shows whether they are right. Release builds
contain none of the testing code.

## Licence

ISC; see [LICENSE](LICENSE).
