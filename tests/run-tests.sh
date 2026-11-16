#!/bin/sh
#
# Builds deadman and deadman.cgi with -DDEADMAN_TESTING, which lets the
# environment set their directories and the time, and runs them through
# simulated days. Prints TAP; exits non-zero if any test fails. pledge(2)
# and unveil(2) are no-ops off OpenBSD, so these tests cannot check them.
#
#   sh tests/run-tests.sh

set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
work=$(mktemp -d "${TMPDIR:-/tmp}/deadman-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

CC=${CC:-cc}
CFLAGS="-std=c99 -D_GNU_SOURCE -DDEADMAN_TESTING -I$top -Wall -Wextra -Werror"
# shellcheck disable=SC2086 # CFLAGS holds several flags
if ! $CC $CFLAGS -o "$work/deadman" "$top/deadman/deadman.c" ||
    ! $CC $CFLAGS -o "$work/deadman.cgi" "$top/deadman.cgi/deadman.cgi.c"; then
    echo "Bail out! cannot build"
    exit 1
fi

export TZ=UTC
export DEADMAN_PINGDIR="$work/pings"
export DEADMAN_STATEDIR="$work/state"
export DEADMAN_SENDMAIL="$work/sendmail"
CONF="$work/deadman.conf"
MAILBOX="$work/mailbox"

cat >"$DEADMAN_SENDMAIL" <<EOF
#!/bin/sh
# Stands in for sendmail(8): appends each message to the mailbox.
[ "\$*" = "-oi -t" ] || { echo "sendmail: unexpected arguments: \$*" >&2; exit 64; }
[ ! -f "$work/sendmail-fails" ] || exit 75
{ cat; echo "----- end of message -----"; } >>"$MAILBOX"
EOF
chmod +x "$DEADMAN_SENDMAIL"

BASE=1795000000 # 18 November 2026, 11:06:40 UTC
DAY=86400
T1=0123456789abcdef0123456789abcdef
T2=fedcba9876543210fedcba9876543210
tests=0
failures=0

ok() { tests=$((tests + 1)); echo "ok $tests - $1"; }
not_ok() { tests=$((tests + 1)); failures=$((failures + 1)); echo "not ok $tests - $1"; }
check() {
    _desc=$1
    shift
    if "$@"; then ok "$_desc"; else not_ok "$_desc"; fi
}
refute() { ! "$@"; }
quietly() { "$@" >/dev/null 2>&1; }

fresh() {
    echo "# $1"
    rm -rf "$DEADMAN_PINGDIR" "$DEADMAN_STATEDIR" "$MAILBOX" "$work/sendmail-fails"
    mkdir -p "$DEADMAN_PINGDIR" "$DEADMAN_STATEDIR"
    : >"$MAILBOX"
    cat >"$CONF" <<EOF
# name   token   period  grace  mailto
backups  $T1     1d      1d     ops@example.org   # a comment
EOF
    # deadman makes the ping files; until it has run, pings are turned away.
    DEADMAN_NOW=$BASE "$work/deadman" -f "$CONF"
    marked=0
}

# at SECONDS deadman [ARGS...]: run deadman at BASE + SECONDS.
at() {
    _t=$((BASE + $1))
    shift
    DEADMAN_NOW=$_t "$work/deadman" -f "$CONF" "$@"
}

# ping SECONDS [start|fail] [TOKEN]: ping as httpd would, at BASE + SECONDS.
ping() {
    _t=$((BASE + $1))
    _path=/ping/${3:-$T1}${2:+/$2}
    REQUEST_METHOD=GET PATH_INFO=$_path "$work/deadman.cgi" >"$work/cgi.out" || return 1
    # deadman.cgi stamps the file with the real time; move it to the simulated time.
    _file="$DEADMAN_PINGDIR/${3:-$T1}${2:+.$2}"
    [ ! -f "$_file" ] || touch -d "@$_t" "$_file"
}

cgi() { # cgi METHOD PATH_INFO: the response's status line
    env -u PATH_INFO REQUEST_METHOD="$1" ${2:+PATH_INFO="$2"} "$work/deadman.cgi" |
        sed -n '1s/\r$//p'
}

mails() { grep -c '^Subject:' "$MAILBOX"; }
mark() { marked=$(mails); }
new_mails() { echo $(($(mails) - marked)); }
last_subject() { grep '^Subject:' "$MAILBOX" | tail -n 1 | sed 's/^Subject: //'; }
last_mail() {
    awk '/^----- end of message -----$/ { last = m; m = ""; next } { m = m $0 "\n" }
        END { printf "%s", last }' "$MAILBOX"
}
mail_says() { last_mail | grep -q -- "$1"; }
expect_mail() {
    _new=$(new_mails)
    if [ "$_new" -eq 1 ] && last_subject | grep -q -- "$2"; then
        ok "$1"
    else
        not_ok "$1 (got $_new new message(s), last: $(last_subject))"
    fi
    mark
}
expect_no_mail() {
    _new=$(new_mails)
    if [ "$_new" -eq 0 ]; then ok "$1"; else not_ok "$1 (got $_new: $(last_subject))"; fi
    mark
}
state_of() { at "$1" status | awk -v n=backups '$1 == n { print $2 }'; }
mtime() { stat -c %Y "$1"; }

# ---- deadman token ------------------------------------------------------------

echo "# deadman token"
a=$("$work/deadman" token)
b=$("$work/deadman" token)
check "makes 32 lowercase hexadecimal digits" sh -c "echo '$a' | grep -qx '[0-9a-f]\{32\}'"
check "different every time" [ "$a" != "$b" ]

# ---- deadman.cgi --------------------------------------------------------------

fresh "deadman.cgi"
at 0
check "deadman makes each check's ping files" \
    [ -f "$DEADMAN_PINGDIR/$T1" ] && [ -f "$DEADMAN_PINGDIR/$T1.start" ] && [ -f "$DEADMAN_PINGDIR/$T1.fail" ]
check "dated the epoch, so they read as never" [ "$(mtime "$DEADMAN_PINGDIR/$T1")" -eq 0 ]
check "writable by the group, which httpd's user is in" \
    [ "$(stat -c %a "$DEADMAN_PINGDIR/$T1")" = 620 ]

check "a ping is a 200" [ "$(cgi GET "/ping/$T1")" = "Status: 200 OK" ]
check "and records the time" [ "$(mtime "$DEADMAN_PINGDIR/$T1")" -gt 0 ]
check "with a body saying OK" sh -c "REQUEST_METHOD=GET PATH_INFO=/ping/$T1 '$work/deadman.cgi' | tail -n 1 | grep -qx OK"
check "/start is recorded separately" [ "$(cgi GET "/ping/$T1/start")" = "Status: 200 OK" ] &&
    [ "$(mtime "$DEADMAN_PINGDIR/$T1.start")" -gt 0 ]
check "and so is /fail" [ "$(cgi POST "/ping/$T1/fail")" = "Status: 200 OK" ] &&
    [ "$(mtime "$DEADMAN_PINGDIR/$T1.fail")" -gt 0 ]
check "POST works as well as GET" [ "$(cgi POST "/ping/$T1")" = "Status: 200 OK" ]
check "HEAD gets no body" [ "$(REQUEST_METHOD=HEAD PATH_INFO=/ping/$T1 "$work/deadman.cgi" | wc -l)" -eq 4 ]
check "any prefix will do" [ "$(cgi GET "/other/place/$T1")" = "Status: 200 OK" ]

check "an unknown token is a 404" [ "$(cgi GET "/ping/$T2")" = "Status: 404 Not Found" ]
check "and creates nothing" [ ! -e "$DEADMAN_PINGDIR/$T2" ]
check "so is a malformed one" [ "$(cgi GET "/ping/0123456789ABCDEF0123456789ABCDEF")" = "Status: 404 Not Found" ]
check "or a short one" [ "$(cgi GET "/ping/0123456789abcdef")" = "Status: 404 Not Found" ]
check "or a long one" [ "$(cgi GET "/ping/${T1}0")" = "Status: 404 Not Found" ]
check "or one of the right length with other letters" \
    [ "$(cgi GET "/ping/0123456789abcdefghijklmnopqrstuv")" = "Status: 404 Not Found" ]
check "or with dots" [ "$(cgi GET "/ping/0123456789abcdef0123456789abcd..")" = "Status: 404 Not Found" ]
# Validation is the second line of defence: even a file that exists is
# refused when its name is not a token.
touch "$DEADMAN_PINGDIR/0123456789abcdefghijklmnopqrstuv"
check "a malformed token is refused even when its file exists" \
    [ "$(cgi GET "/ping/0123456789abcdefghijklmnopqrstuv")" = "Status: 404 Not Found" ]
check "or one after a trailing slash" [ "$(cgi GET "/ping/$T1/")" = "Status: 404 Not Found" ]
check "or another verb" [ "$(cgi GET "/ping/$T1/stop")" = "Status: 404 Not Found" ]
check "or a path that climbs" [ "$(cgi GET "/ping/../$T1.fail")" = "Status: 404 Not Found" ]
check "or no path" [ "$(cgi GET "")" = "Status: 404 Not Found" ]
check "or an absurdly long path" [ "$(cgi GET "/$(printf '%0300d' 0)/$T1")" = "Status: 404 Not Found" ]
check "a PUT is a 405" [ "$(cgi PUT "/ping/$T1")" = "Status: 405 Method Not Allowed" ]
check "nothing echoes the request" refute sh -c "REQUEST_METHOD=GET PATH_INFO=/ping/$T2 '$work/deadman.cgi' | grep -q '$T2'"

# ---- deadman check ------------------------------------------------------------

fresh "a check going down and coming back"
at 0
check "a new check is new" [ "$(state_of 0)" = new ]
expect_no_mail "and sends nothing"
at $DAY
expect_no_mail "even after a day without pings"

ping $DAY
at $((DAY + 60))
check "a ping makes it up" [ "$(state_of $((DAY + 60)))" = up ]
expect_no_mail "quietly"
at $((2 * DAY))
check "still up at the end of its period" [ "$(state_of $((2 * DAY)))" = up ]
at $((2 * DAY + 1))
check "late just after" [ "$(state_of $((2 * DAY + 1)))" = late ]
expect_no_mail "which sends nothing"
at $((3 * DAY))
expect_no_mail "nor at the end of its grace time"
at $((3 * DAY + 1))
check "down just after" [ "$(state_of $((3 * DAY + 1)))" = down ]
expect_mail "and says so" "\[deadman\] backups is down"
check "to its mailto" mail_says "^To: ops@example.org$"
check "giving its last ping" mail_says "its last ping was at 2026-11-19 11:06 UTC"
check "and what it expects" mail_says "It should ping every day, with a grace time of 1 day."
check "signed with the host" mail_says "^deadman on "
check "marked as automatic" mail_says "^Auto-Submitted: auto-generated$"
at $((3 * DAY + 60))
at $((5 * DAY))
expect_no_mail "only once"

ping $((5 * DAY + 10))
at $((5 * DAY + 60))
expect_mail "says when it is back" "\[deadman\] backups is up again"
check "and since when it was down" mail_says "after being down since 2026-11-21 11:06 UTC"
at $((5 * DAY + 120))
expect_no_mail "once"

fresh "failures and starts"
ping 0
at 60
ping 120 fail
at 180
expect_mail "a /fail is down at once" "backups is down"
check "saying so" mail_says "it reported a failure at 2026-11-18 11:08 UTC"
ping 240 start
at 300
expect_no_mail "a /start after a /fail does not bring it back"
check "but shows it running" sh -c "DEADMAN_NOW=$((BASE + 300)) '$work/deadman' -f '$CONF' status | grep -q '(running)'"
ping 360
at 420
expect_mail "its success brings it back" "backups is up again"
ping $DAY start
at $((DAY + 60))
expect_no_mail "a /start begins a run"
at $((2 * DAY + 60))
expect_mail "that has not finished within the grace time is down" "backups is down"
check "saying so" mail_says "it started at 2026-11-19 11:06 UTC, but"
ping $((2 * DAY + 120))
at $((2 * DAY + 180))
expect_mail "until it finishes" "backups is up again"

fresh "mail that cannot be sent"
ping 0
at 60
touch "$work/sendmail-fails"
check "a failed send is an error" refute quietly at $((2 * DAY + 61))
expect_no_mail "nothing was sent"
rm "$work/sendmail-fails"
at $((2 * DAY + 120))
expect_mail "and it is tried again on the next run" "backups is down"

fresh "deadman -n"
ping 0
at 60
at $((2 * DAY + 61)) -n >"$work/n.out"
expect_no_mail "sends nothing"
check "but shows what it would send" grep -q '^Subject: \[deadman\] backups is down' "$work/n.out"
check "and records nothing" [ "$(cat "$DEADMAN_STATEDIR/backups")" = "up $((BASE + 60))" ]
at $((2 * DAY + 120))
expect_mail "so the next real run still sends it" "backups is down"

fresh "deadman status"
printf '%s\n' "quiet  $T2  1h  10m  ops@example.org" >>"$CONF"
at 0
ping 0
at 0 status >"$work/status"
check "has a heading" grep -q '^NAME  *STATE  *LAST PING  *DUE' "$work/status"
check "lists each check" grep -q '^backups  *up  *2026-11-18 11:06 UTC  *2026-11-19 11:06 UTC' "$work/status"
check "including one that has never pinged" grep -q '^quiet  *new  *never$' "$work/status"
check "with no blanks at the ends of lines" refute grep -q ' $' "$work/status"

fresh "changing the configuration"
at 0
printf '%s\n' "renamed  $T2  1h  10m  ops@example.org" >"$CONF"
at 60
check "a token no longer configured stops working" [ ! -e "$DEADMAN_PINGDIR/$T1" ] && [ ! -e "$DEADMAN_PINGDIR/$T1.fail" ]
check "and its state goes" [ ! -e "$DEADMAN_STATEDIR/backups" ]
check "the new one works" [ -f "$DEADMAN_PINGDIR/$T2" ] && [ "$(cgi GET "/ping/$T2")" = "Status: 200 OK" ]
check "and the old one is a 404" [ "$(cgi GET "/ping/$T1")" = "Status: 404 Not Found" ]

echo "# deadman.conf errors"
bad() { # bad DESCRIPTION LINE MESSAGE
    printf '%s\n' "$2" >"$CONF"
    check "$1" sh -c "DEADMAN_NOW=$BASE '$work/deadman' -f '$CONF' 2>&1 | grep -q '$CONF:1: $3'"
}
bad "a short token" "a 0123 1d 1d x@y" "a token is 32 lowercase hexadecimal digits"
bad "a token with other letters" "a 0123456789abcdefghijklmnopqrstuv 1d 1d x@y" "a token is 32"
bad "a bad period" "a $T1 1fortnight 1d x@y" "bad period"
bad "no unit" "a $T1 86400 1d x@y" "bad period"
bad "a bad grace" "a $T1 1d 0h x@y" "bad grace"
bad "too few fields" "a $T1 1d 1d" "expected name, token, period, grace and mailto"
bad "too many" "a $T1 1d 1d x@y z@y" "too many fields"
bad "a bad name" "../a $T1 1d 1d x@y" "bad name"
bad "a mailto that is an option" "a $T1 1d 1d -oQ/tmp" "bad mailto"
printf '%s\n%s\n' "a $T1 1d 1d x@y" "b $T1 1d 1d x@y" >"$CONF"
check "a token used twice" sh -c "DEADMAN_NOW=$BASE '$work/deadman' -f '$CONF' 2>&1 | grep -q 'b has the same token as a'"
check "a missing file" sh -c "'$work/deadman' -f '$work/nonexistent' 2>&1 | grep -q 'nonexistent'"
check "an unknown command" refute quietly "$work/deadman" frobnicate

echo "1..$tests"
[ "$failures" -eq 0 ] || echo "# $failures of $tests tests failed"
[ "$failures" -eq 0 ]
