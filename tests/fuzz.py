#!/usr/bin/env python3
"""Differential fuzzing of deadman.cgi's request handling and deadman's
configuration parser, built with sanitizers, against small models of what
each should accept. Not run by make test; see AGENTS.md.

    python3 tests/fuzz.py SRCDIR WORKDIR CGI-RUNS CONF-RUNS"""

import os
import random
import re
import subprocess
import sys

src, work, cgi_runs, conf_runs = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
random.seed(20261116)
os.makedirs(work, exist_ok=True)
cc = ["gcc", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
      "-D_GNU_SOURCE", "-DDEADMAN_TESTING", "-I" + src]
for prog, path in (("deadman", "deadman/deadman.c"), ("deadman.cgi", "deadman.cgi/deadman.cgi.c")):
    subprocess.run(cc + ["-o", os.path.join(work, prog), os.path.join(src, path)], check=True)

T1 = "0123456789abcdef0123456789abcdef"
T2 = "fedcba9876543210fedcba9876543210"
pings = os.path.join(work, "pings")
os.makedirs(pings, exist_ok=True)
for name in (T1, T1 + ".start", T1 + ".fail", "0123456789abcdefghijklmnopqrstuv"):
    open(os.path.join(pings, name), "w").close()

problems = 0


def report(what, detail):
    global problems
    problems += 1
    if problems <= 20:
        print("PROBLEM:", what, repr(detail)[:300])


# ---- deadman.cgi ------------------------------------------------------------

def cgi_model(method, path):
    if method is None:
        return 400
    if method not in (b"GET", b"HEAD", b"POST"):
        return 405
    if path is None or len(path) >= 256:
        return 404
    if b"/" not in path:
        return 404
    head, tail = path.rsplit(b"/", 1)
    suffix = b""
    if tail in (b"start", b"fail"):
        suffix = b"." + tail
        if b"/" not in head:
            return 404
        head, tail = head.rsplit(b"/", 1)
    if not re.fullmatch(rb"[0-9a-f]{32}", tail):
        return 404
    return 200 if os.path.exists(os.path.join(pings.encode(), tail + suffix)) else 404


pieces = [b"/", b"//", b".", b"..", b"start", b"fail", b"stop", b"ping", T1.encode(),
          T2.encode(), T1.upper().encode(), T1[:31].encode(), (T1 + "0").encode(),
          b"0123456789abcdefghijklmnopqrstuv", b"%2f", b"\\", b"\xff", b"\x01", b" ",
          b"\t", b"?", b"#", b"*", b"A" * 100]
methods = [b"GET", b"HEAD", b"POST", b"PUT", b"get", b"", b"DELETE", b"GET ", None]

for i in range(cgi_runs):
    method = random.choice(methods)
    r = random.random()
    if r < 0.1:
        path = None
    elif r < 0.2:
        path = bytes(random.randrange(1, 256) for _ in range(random.randrange(0, 300)))
    elif r < 0.3:  # around the 256-byte limit
        n = random.choice([250, 254, 255, 256, 257, 300])
        tail = b"/" + T1.encode() + random.choice([b"", b"/start", b"/fail"])
        path = b"/" * max(0, n - len(tail)) + tail
    else:
        path = b"".join(random.choice(pieces) for _ in range(random.randrange(1, 8)))
    env = {b"DEADMAN_PINGDIR": pings.encode(), b"PATH": os.environb[b"PATH"]}
    if method is not None:
        env[b"REQUEST_METHOD"] = method
    if path is not None:
        env[b"PATH_INFO"] = path
    p = subprocess.run([os.path.join(work, "deadman.cgi")], env=env, capture_output=True)
    if p.stderr or p.returncode != 0:
        report("cgi crashed or complained", (method, path, p.returncode, p.stderr[:200]))
        continue
    first = p.stdout.split(b"\r\n", 1)[0]
    m = re.fullmatch(rb"Status: (\d+) .*", first)
    if m is None:
        report("cgi status line", (method, path, first))
        continue
    want = cgi_model(method, path)
    if int(m.group(1)) != want:
        report("cgi answer", (method, path, int(m.group(1)), want))
    if path is not None and len(path) > 8 and path in p.stdout:
        report("cgi echoed the request", (method, path))
    if method == b"HEAD" and p.stdout.endswith(b"\r\n\r\n") is False:
        report("cgi HEAD has a body", (path, p.stdout))

print("cgi: %d runs" % cgi_runs)

# ---- deadman.conf -----------------------------------------------------------

UNITS = {"s": 1, "m": 60, "h": 3600, "d": 86400, "w": 604800}
MAX = 10 * 366 * 24 * 60 * 60
durations = ["1s", "90s", "15m", "36h", "1d", "2w", "0d", "-1d", "+1d", "1.5d",
             "1dd", "d", "1", "1y", "522w", "523w", "3660d", "3661d", "316224000s",
             "316224001s", "9223372036854775807s", "9223372036854775808s",
             "30500568904944w", "99999999999999999999d", "07m", "0x10m"]


def duration_ok(s):
    m = re.fullmatch(r"\+?(\d+)([smhdw])", s)
    if m is None:
        return False
    n = int(m.group(1))
    if n == 0 or n > 2**63 - 1:
        return False
    return n * UNITS[m.group(2)] <= MAX


names = ["backups", "a", "A.b_c-d", "x" * 64, "x" * 65, ".hidden", "-n", "../up",
         "a/b", "caf\xe9", "a;b", "ok1"]
tokens = [T1, T2, T1.upper(), T1[:31], T1 + "0", "0123456789abcdefghijklmnopqrstuv"]
mailtos = ["root", "ops@example.org", "a@b,c@d", "-oQ/tmp", "x\x7fy", "ok"]

conf = os.path.join(work, "deadman.conf")
for i in range(conf_runs):
    name, token = random.choice(names), random.choice(tokens)
    period, grace = random.choice(durations), random.choice(durations)
    mailto = random.choice(mailtos)
    line = "\t".join([name, token, period, grace, mailto])
    if random.random() < 0.2:
        data = bytes(random.randrange(0, 256) for _ in range(random.randrange(0, 200)))
        model = None
    else:
        data = (line + "\n").encode("latin-1")
        model = (re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9._-]{0,63}", name) is not None
                 and re.fullmatch(r"[0-9a-f]{32}", token) is not None
                 and duration_ok(period) and duration_ok(grace)
                 and mailto[0] != "-" and all(" " < c <= "~" for c in mailto))
    with open(conf, "wb") as f:
        f.write(data)
    os.chmod(conf, 0o640)
    env = {"DEADMAN_PINGDIR": pings, "DEADMAN_STATEDIR": work, "PATH": os.environ["PATH"]}
    p = subprocess.run([os.path.join(work, "deadman"), "-f", conf, "status"],
                       env=env, capture_output=True)
    if b"runtime error" in p.stderr or b"Sanitizer" in p.stderr or p.returncode not in (0, 1):
        report("deadman crashed", (data, p.returncode, p.stderr[:300]))
        continue
    if p.returncode == 1 and not p.stderr.startswith(b"deadman: "):
        report("deadman error without a message", (data, p.stderr))
    if model is not None and (p.returncode == 0) != model:
        report("deadman accepted wrongly" if model is False else "deadman refused wrongly",
               (line, p.stderr))

print("conf: %d runs" % conf_runs)
print("problems: %d" % problems)
sys.exit(1 if problems else 0)
