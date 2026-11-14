#!/usr/bin/env python3
"""Poll the hardware issue for new replies, and judge the logs that come with
them without a model in the loop.

This is the half of "check the issue every five minutes" that can be
deterministic: fetch comments newer than the last one we saw, download any
console logs attached to them, run the same `--replay` judges the acceptance
scripts use, and leave a digest plus a desktop notification.  What it does
*not* do is change code or documents - that needs judgement, and the digest
exists so the next session starts with "here is what arrived" instead of
"let me look".

    python scripts/watch_issues.py                # one poll
    python scripts/watch_issues.py --dry-run      # show, do not touch state
    python scripts/watch_issues.py --issue 1

State lives in build/issue-watch/state.json (comment id + attachment cache),
so a restart does not re-announce old comments.  Everything it writes is under
build/, which is not committed.
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import time
import urllib.request

REPO = "yafengabc/TinyOS"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE_DIR = os.path.join(ROOT, "build", "issue-watch")
STATE = os.path.join(STATE_DIR, "state.json")
LOCK = os.path.join(STATE_DIR, "lock")
LOG = os.path.join(STATE_DIR, "watch.log")

# Overridable so the fetch path can be exercised without the real gh (and so a
# machine with gh installed somewhere odd can point at it): "python stub.py".
GH_CMD = os.environ.get("GH_CMD", "gh")

ATTACH = re.compile(r"https://github\.com/user-attachments/files/\d+/[^\s)\]]+")
IMAGES = ("rp2350rvsmp", "rp2350rv", "rp2350smp", "rp2350")


def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")
    print(line, flush=True)


def gh(*args):
    out = subprocess.run(shlex.split(GH_CMD) + list(args), capture_output=True,
                         timeout=60)
    if out.returncode != 0:
        raise RuntimeError(out.stderr.decode("utf-8", "replace").strip())
    return out.stdout.decode("utf-8", "replace")


def take_lock():
    """Return True when this process may run, False when another one holds it."""
    try:
        with open(LOCK, encoding="utf-8") as f:
            pid = int(f.read().strip() or 0)
    except (OSError, ValueError):
        pid = 0
    if pid:
        try:
            os.kill(pid, 0)          # alive? (works on Windows for existence)
            return False
        except OSError:
            pass                      # stale lock from a killed run
    with open(LOCK, "w", encoding="utf-8") as f:
        f.write(str(os.getpid()))
    return True


def drop_lock():
    try:
        os.remove(LOCK)
    except OSError:
        pass


def load_state():
    try:
        with open(STATE, encoding="utf-8") as f:
            state = json.load(f)
            state.setdefault("seen", [])
            return state
    except (OSError, ValueError):
        return {"seen": []}


def save_state(state):
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(STATE, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=1)


TOAST = r"""
$ErrorActionPreference = 'Stop'
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications,
 ContentType = WindowsRuntime] | Out-Null
$t = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent(
        [Windows.UI.Notifications.ToastTemplateType]::ToastText02)
$x = $t.GetElementsByTagName('text')
$x.Item(0).AppendChild($t.CreateTextNode($args[0])) | Out-Null
$x.Item(1).AppendChild($t.CreateTextNode($args[1])) | Out-Null
$n = New-Object Windows.UI.Notifications.ToastNotification $t
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier(
        'TinyOS watch').Show($n)
"""


def notify(title, text):
    """A toast, not a message box: a scheduled task must not block on a click.

    If the toast fails (older Windows, WinRT disabled) the digest and the
    ATTENTION marker are still there - the notification is a convenience, the
    files are the record.
    """
    try:
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive",
                        "-Command", TOAST, title, text],
                       capture_output=True, timeout=8)
    except Exception:
        pass


def judge(log_path, comment_text):
    """Run whichever acceptance judge fits this log; return a short verdict list.

    The RP2350 PIO log is image-independent (same three programs on all four
    images) so it needs no hint; the section-33 log does depend on the image,
    and rather than guess, the comment has to name it.
    """
    text = open(log_path, encoding="utf-8", errors="replace").read()
    verdicts = []

    if "pio:" in text:
        r = subprocess.run([sys.executable,
                            os.path.join(ROOT, "scripts", "rp2350", "pio_accept.py"),
                            "--image", "rp2350", "--replay", log_path],
                           capture_output=True, text=True, cwd=ROOT,
                           timeout=180)
        verdicts.append(("pio_accept", r.returncode == 0, r.stdout.strip()))

    if any(m in text for m in ("[A] pre-pll", "[mpu]", "MemManage",
                               "access fault", "fs grow")):
        image = next((i for i in IMAGES if i in comment_text), None)
        if image:
            r = subprocess.run([sys.executable,
                                os.path.join(ROOT, "scripts", "rp2350", "accept33.py"),
                                "--image", image, "--replay", log_path],
                               capture_output=True, text=True)
            verdicts.append(("accept33 %s" % image, r.returncode == 0,
                             r.stdout.strip()))
        else:
            verdicts.append(("accept33", None,
                             "log looks like a section-33 run but the comment"
                             " does not name the image; skipped"))
    return verdicts


def fetch_attachment(url, name):
    os.makedirs(os.path.join(STATE_DIR, "logs"), exist_ok=True)
    dst = os.path.join(STATE_DIR, "logs", "%d-%s" % (int(time.time()), name))
    for attempt in range(2):
        try:
            with urllib.request.urlopen(url, timeout=30) as r, open(dst, "wb") as f:
                f.write(r.read())
            return dst
        except Exception as e:
            if attempt:
                log("  attachment failed: %s (%s)" % (url, e))
                return None
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--issue", type=int, default=1)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-notify", action="store_true")
    args = ap.parse_args()

    os.makedirs(STATE_DIR, exist_ok=True)
    if not take_lock():
        return 0                      # a slow run is still going: skip this tick
    try:
        return poll(args)
    finally:
        drop_lock()


def poll(args):
    fresh = not os.path.exists(STATE)
    with open(os.path.join(STATE_DIR, "last-run"), "w", encoding="utf-8") as f:
        f.write(time.strftime("%Y-%m-%d %H:%M:%S\n"))

    state = load_state()
    try:
        raw = gh("issue", "view", str(args.issue), "--repo", REPO,
                 "--json", "title,comments")
    except RuntimeError as e:
        log("gh failed: %s" % e)
        return 2
    data = json.loads(raw)
    comments = data.get("comments", [])
    # Comment ids are GraphQL node ids (strings), so this is a set, not a
    # watermark: an id either has been handled or it has not.
    new = [c for c in comments if c.get("id") not in state["seen"]]
    if fresh:
        # First run: adopt what is already there as the baseline instead of
        # announcing a comment from hours ago.
        state["seen"] = [c.get("id") for c in comments]
        save_state(state)
        log("baseline at %d comment(s)" % len(comments))
        return 0
    if not new:
        return 0

    digest = os.path.join(STATE_DIR, "digest-%s.md" % time.strftime("%m%d-%H%M%S"))
    lines = ["# issue #%d: %d new comment(s)\n" % (args.issue, len(new)),
             "%s\n" % data.get("title", "")]
    flagged = 0

    for c in sorted(new, key=lambda c: c.get("id", 0)):
        body = c.get("body", "")
        lines.append("\n## %s at %s\n\n%s\n" %
                     (c.get("author", {}).get("login", "?"),
                      (c.get("createdAt", "") or "")[:19], body))
        attachments = ATTACH.findall(body)
        for url in attachments:
            name = re.sub(r"[^A-Za-z0-9._-]", "_", url.rsplit("/", 1)[-1])[:60]
            path = fetch_attachment(url, name)
            if not path:
                continue
            lines.append("\n### attachment `%s`\n" % os.path.basename(path))
            for name, ok, output in judge(path, body):
                mark = {True: "PASS", False: "FAIL", None: "SKIP"}[ok]
                if ok is not True:
                    flagged += 1
                lines.append("\n`%s`: **%s**\n\n```\n%s\n```\n"
                             % (name, mark, output))
        state["seen"].append(c.get("id"))

    with open(digest, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(lines))
    log("wrote %s (flagged=%d)" % (digest, flagged))

    if not args.dry_run:
        state["seen"] = state["seen"][-200:]
        save_state(state)
        with open(os.path.join(STATE_DIR, "pending.txt"), "a",
                  encoding="utf-8") as f:
            f.write("%s  %s  flagged=%d\n" % (time.strftime("%Y-%m-%d %H:%M"),
                                              digest, flagged))
        marker = os.path.join(STATE_DIR, "ATTENTION")
        if flagged:
            with open(marker, "w", encoding="utf-8") as f:
                f.write("%s\n" % digest)
        elif os.path.exists(marker):
            os.remove(marker)
        if not args.no_notify:
            notify("TinyOS issue #%d" % args.issue,
                   "%d new comment(s); %s\n%s"
                   % (len(new), "check the digest" if not flagged
                      else "%d check(s) need a look" % flagged, digest))
    # 10 means "there is something new to look at", so a task can chain a
    # follow-up on it without the poller having to know about that follow-up.
    return 10


if __name__ == "__main__":
    sys.exit(main())
