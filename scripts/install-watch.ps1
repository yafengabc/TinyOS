# Register (or remove) the five-minute issue poller as a Windows scheduled task.
#
# The task only wakes the *deterministic* half of the hand-off: it fetches new
# comments, downloads the logs attached to them, runs the `--replay` judges
# and writes build/issue-watch/digest-*.md plus a toast.  It never edits code
# or documents - that still needs a session, and the digest is what makes that
# session a one-liner.
#
#   powershell -ExecutionPolicy Bypass -File scripts/install-watch.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/install-watch.ps1 -Remove
#   powershell -ExecutionPolicy Bypass -File scripts/install-watch.ps1 -Status
#
# Notes:
#   * runs as the logged-on user, because `gh` reads its token from the
#     Windows credential store of that user - a SYSTEM task cannot see it;
#   * the repository path and the Python interpreter are baked into the task
#     command, so re-run this if either moves;
#   * gh must be on PATH for the *task*; scripts/watch_issues.py also accepts
#     GH_CMD="python stub.py" for testing without it.

param(
    [switch]$Remove,
    [switch]$Status,
    [int]$Minutes = 5
)

$ErrorActionPreference = "Stop"
$taskName = "TinyOS issue watch"
$root = Split-Path -Parent $PSScriptRoot
$python = (Get-Command python).Source
$log = Join-Path $root "build\issue-watch\task.log"

if ($Status) {
    schtasks /query /tn $taskName /fo LIST /v
    exit $LASTEXITCODE
}

if ($Remove) {
    schtasks /delete /tn $taskName /f
    exit $LASTEXITCODE
}

New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null

$cmd = 'cmd /c cd /d "{0}" && "{1}" scripts\watch_issues.py >> "{2}" 2>&1' -f `
    $root, $python, $log
schtasks /create /tn $taskName /sc minute /mo $Minutes /f /tr $cmd | Out-Null
if ($LASTEXITCODE -ne 0) { throw "schtasks failed: $LASTEXITCODE" }

Write-Output "task '$taskName' runs every $Minutes minute(s):"
Write-Output "  $cmd"
Write-Output ""
Write-Output "watch it with: schtasks /query /tn `"$taskName`" /fo LIST /v"
Write-Output "run once now:  schtasks /run /tn `"$taskName`""
Write-Output "digest lives in build\issue-watch\ (not committed)"
