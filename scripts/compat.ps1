#requires -version 5
<#
    Cross-target compatibility check.

    PLATFORM-DESIGN section 9 promises that the same command sequence gives
    the same output on every target.  smoke.ps1 only looks for a shell prompt
    and the absence of a panic, so a divergence such as "prot rodata faults
    on rv32 but prints survived on x86" slips through it.  This script boots
    each target, feeds it the same lines through a TCP serial port, strips
    whatever is allowed to differ (pid, window address, uptime, tick count)
    and compares the transcripts.

    Genuine capability differences are listed in $AllowedDiffs: the text that
    target must print instead.  Anything else is a failure.

    One run per target is not enough.  The shell has no input queue, so a
    burst of lines is type-ahead that gets echoed when it arrives: two
    commands can come back on one line and their outputs run together.  Each
    command therefore gets its own run - boot, one line, compare - and only
    the strict prefix shares a session.

    usage:  powershell -ExecutionPolicy Bypass -File scripts/compat.ps1
            [-Targets x86,rv32,arm] [-NoBuild] [-Verbose]
#>
param(
    [string[]]$Targets = @("x86", "rv32", "arm"),
    [int]$Port = 5570,
    [int]$CommandWaitMs = 900,
    [switch]$NoBuild,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# Run together at boot, before anything has had time to become nondeterministic.
# ls, cat and exit come last so that a file written earlier cannot show up.
$Boot = @(
    "hello alpha beta",
    "hello_printf alpha beta"
)
$StrictOne = @(
    "echo alpha beta gamma",
    "cat nonexistent.txt",
    "ls",
    "exit"
)

# Run one per session.  Every "run" starts a fresh shell, so pid 1 / pid 2
# are the same everywhere.
$Loose = @(
    "run prot null",
    "run prot rodata",
    "run prot heap",
    "run prot stack"
)

# "<target>|<command>" -> what that target must print, as a regular
# expression matched against the normalised output.
#
# Only genuine, documented capability differences belong here.  An entry
# added because the output looks wobbly is how this test stops being useful.
$AllowedDiffs = @{
    # x86 has no MMU and no protection unit: ring 3 isolates privilege, not
    # memory, so every prot case survives.  See PLATFORM-DESIGN 5.
    "x86|run prot null"   = "^survived$"
    "x86|run prot rodata" = "^survived$"
    "x86|run prot heap"   = "^survived$"
    "x86|run prot stack"  = "^survived$"

    # rv32 PMP and the ARM MPU stop all four cases except the heap on ARM -
    # see below - and the kernel kills only the task: no "survived" line and
    # a trap naming the faulting address.
    "rv32|run prot null"   = "^killed: protection fault$"
    "rv32|run prot rodata" = "^killed: protection fault$"
    "rv32|run prot heap"   = "^killed: protection fault$"
    "rv32|run prot stack"  = "^killed: protection fault$"

    # ARMv7-M MPU regions are naturally aligned power-of-two blocks with no
    # TOR form, so the break cannot be a region edge: a write one byte past
    # it stays inside the window region and survives.  See 5.2.
    "arm|run prot null"    = "^killed: protection fault$"
    "arm|run prot rodata"  = "^killed: protection fault$"
    "arm|run prot heap"    = "^survived$"
    "arm|run prot stack"   = "^killed: protection fault$"

    # The RP2040 has no MPU at all (Cortex-M0+), so like x86 the task runs
    # unprivileged but with the whole flat address space visible.  See 5.2.
    "rp2040|run prot null"   = "^survived$"
    "rp2040|run prot rodata" = "^survived$"
    "rp2040|run prot heap"   = "^survived$"
    "rp2040|run prot stack"  = "^survived$"
}

# ---------------------------------------------------------------------------

function Invoke-Target {
    param([string]$Target, [string[]]$Commands, [int]$Port)

    $smoke = Join-Path $PSScriptRoot "smoke.ps1"
    $prevCmds = $env:TOS_COMPAT_COMMANDS
    $env:TOS_COMPAT_COMMANDS = (@($Commands) | ConvertTo-Json -Compress)
    $child = "`$c = @(ConvertFrom-Json `$env:TOS_COMPAT_COMMANDS); " +
             "& '$smoke' -Target '$Target' -Port $Port " +
             "-CommandWaitMs $CommandWaitMs -Commands `$c"
    try {
        return (& powershell -NoProfile -ExecutionPolicy Bypass -Command $child 2>&1 | Out-String)
    } finally {
        $env:TOS_COMPAT_COMMANDS = $prevCmds
    }
}

# The guest's own output only: no harness footer and no transcript preamble.
function Get-Output {
    param([string]$Text)

    $t = ($Text -replace "`r", "")
    $cut = $t.IndexOf("--- qemu stderr ---")
    if ($cut -ge 0) { $t = $t.Substring(0, $cut) }
    $cut = $t.IndexOf("SMOKE TEST")
    if ($cut -ge 0) { $t = $t.Substring(0, $cut) }
    return $t
}

# What the shell printed in answer to a command.
function Normalize {
    param([string]$Text)

    $t = Get-Output $Text
    # Drop the boot log and the echoed command: everything up to the first
    # "tos> " is boot output, and the rest of that line is the echo (which
    # can be several lines' worth of type-ahead if an earlier ran late).
    $at = $t.IndexOf("tos> ")
    if ($at -ge 0) {
        $t = $t.Substring($at)
        # The echo covers the rest of its line plus any further lines that
        # start with a prompt; the answer starts at the first plain line.
        $lines = $t -split "`n"
        $i = 1
        while ($i -lt $lines.Count -and ($lines[$i].StartsWith("tos>") -or $lines[$i].Trim() -eq "")) {
            $i++
        }
        $t = ($lines | Select-Object -Skip $i) -join "`n"
    }
    # pids: "pid 3", "pid=4", "pid 1", "[pid 2]"
    $t = $t -replace "\[pid \d+\]", "[pid N]"
    $t = $t -replace "pid=?\s*\d+", "pid N"
    # task windows, fault addresses and any other RAM address
    $t = $t -replace "0x[0-9a-fA-F]{8}", "0xADDR"
    # uptime, tick counts, "8760 ms" in spin output
    $t = $t -replace "\d+(\.\d+)? ms", "N ms"
    $t = $t -replace "\d+ tick\(s\)", "N tick(s)"
    $lines = @()
    foreach ($line in ($t -split "`n")) {
        $line = $line.Trim()
        if ($line.Length -gt 0 -and $line -ne "tos>") { $lines += $line }
    }
    return ($lines -join "`n")
}

# A faulted task exits 139 and the shell only waits for it at "exit", so the
# trap message is the only sign that protection worked.  Say so, in words
# that do not depend on the architecture's exception names.
function Get-Verdict {
    param([string]$Normalized)

    # Only the trap line counts.  Program output can mention the words
    # ("prot rodata" says .rodata, and a crash demo prints the phrase before
    # it faults).
    $trap = ($Normalized -split "`n") | Where-Object { $_ -match "\[pid N\]" }

    if ($trap -match "MemManage|BusFault|access fault") {
        return "killed: protection fault"
    }
    if ($trap -match "illegal instruction|UsageFault") {
        return "killed: illegal instruction"
    }
    if ($Normalized -match "survived: this machine cannot stop us") {
        return "survived"
    }
    return $Normalized
}

$script:Unreliable = $false

# Compare one command across the targets.  Returns $true when they agree.
function Compare-Command {
    param([string]$Cmd, [hashtable]$Raw)

    $set = @{}
    $diffs = @()
    foreach ($t in $Targets) {
        $text = ""
        if ($Raw.ContainsKey($t)) { $text = Normalize $Raw[$t] }
        if ($text -eq "") { $script:Unreliable = $true }
        $key = "$t|$Cmd"
        # The expectation, when there is one, is matched against the whole
        # answer so that "survived" cannot be satisfied by the lead-in line.
        $seen = if ($AllowedDiffs.ContainsKey($key)) { Get-Verdict $text } else { $text }
        if ($AllowedDiffs.ContainsKey($key)) {
            $set[$t] = $AllowedDiffs[$key]
            if ($seen -notmatch $AllowedDiffs[$key]) {
                $diffs += ("[{0}] expected /{1}/ but saw {2}" -f $t, $AllowedDiffs[$key], ($text -replace "`n", " / "))
            }
        } else {
            $set[$t] = $text
        }
    }
    $ref = $Targets[0]
    foreach ($t in $Targets) {
        if (-not $AllowedDiffs.ContainsKey("$t|$Cmd") -and $set[$t] -ne $set[$ref]) {
            $diffs += ("[{0}] {1}" -f $t, ($set[$t] -replace "`n", " / "))
        }
    }
    if ($diffs.Count -eq 0) {
        Write-Host ("  ok    {0}" -f $Cmd) -ForegroundColor Green
        return $true
    }
    Write-Host ("  DIFF  {0}" -f $Cmd) -ForegroundColor Red
    foreach ($d in $diffs) { Write-Host ("        " + $d) }
    Write-Host "        (a real capability difference belongs in PLATFORM-DESIGN 9" -ForegroundColor Yellow
    Write-Host "         and in `$AllowedDiffs of this script)" -ForegroundColor Yellow
    return $false
}

function Assert-Run {
    param([string]$Target, [string]$Out)
    if ($Out -match "shell prompt never appeared") {
        throw "shell prompt never appeared on $Target"
    }
    if ($Out -notmatch "SMOKE TEST OK") {
        throw "smoke on $Target failed:`n$Out"
    }
}

# ---------------------------------------------------------------------------

if (-not $NoBuild) {
    foreach ($t in $Targets) {
        Write-Host "building $t" -ForegroundColor Cyan
        & (Join-Path $root "tos.exe") build -target $t 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "build failed for $t" }
    }
}

$failed = 0
$port = $Port
# Most commands get one session per boot; this counts the ones that had to
# be run on their own.
$script:Isolated = 0

function Compare-Isolated {
    param([string]$Cmd, [int]$StartPort)

    $script:Isolated++
    $raw = @{}
    foreach ($t in $Targets) {
        # One line per boot, and retried once: a line that arrives before the
        # shell has reached its read() is type-ahead that the next tty_poll()
        # may drop, which looks exactly like a divergence.
        for ($attempt = 1; $attempt -le 2; $attempt++) {
            $out = Invoke-Target -Target $t -Commands @($Cmd) -Port $StartPort
            $StartPort++
            Assert-Run $t $out
            if ($Verbose) { Write-Host $out }
            $body = Get-Output $out
            $raw[$t] = $body.Substring([Math]::Max(0, $body.IndexOf("tos> ")))
            if ((Normalize $raw[$t]) -ne "") { break }
        }
    }
    return (Compare-Command $Cmd $raw)
}

# 1. One session per target: the boot-time commands, in order.
$sessions = @{}
foreach ($t in $Targets) {
    $out = Invoke-Target -Target $t -Commands $Boot -Port $port
    $port++
    Assert-Run $t $out
    if ($Verbose) { Write-Host $out } else { Write-Host ("  {0}: ran {1} boot commands" -f $t, $Boot.Count) }
    # One section per line the shell echoed.
    $t2 = Get-Output $out
    $at = $t2.IndexOf("tos>")
    if ($at -lt 0) { throw "no prompt from $t" }
    $sessions[$t] = @([regex]::Split($t2.Substring($at), "(?m)^tos> ") | Select-Object -Skip 1)
}
for ($i = 0; $i -lt $Boot.Count; $i++) {
    $raw = @{}
    foreach ($t in $Targets) {
        if ($i -lt $sessions[$t].Count) { $raw[$t] = $sessions[$t][$i] }
    }
    # A session that lost a section cannot be compared: rerun the command
    # alone rather than reporting a difference that is really type-ahead.
    $missing = $false
    foreach ($t in $Targets) {
        if (-not $raw.ContainsKey($t)) { $missing = $true }
    }
    if ($missing) {
        if (-not (Compare-Isolated $Boot[$i] $port)) { $failed++ }
        $port += 2 * $Targets.Count
        continue
    }
    if (-not (Compare-Command $Boot[$i] $raw)) { $failed++ }
}

# 2. Each remaining command on its own: a fresh boot, one line.
foreach ($cmd in (@($StrictOne) + @($Loose))) {
    if (-not (Compare-Isolated $cmd $port)) { $failed++ }
    $port += 2 * $Targets.Count
}

Write-Host ""
if ($script:Unreliable) {
    Write-Host "note: a target printed nothing for one command even after a retry." -ForegroundColor Yellow
    Write-Host "      Check the session by hand before trusting the comparison." -ForegroundColor Yellow
}
if ($script:Isolated -gt 0) {
    Write-Host ("note: {0} command(s) of the boot session were rerun in isolation" -f $script:Isolated) -ForegroundColor Yellow
    Write-Host "      because their output ran together (no input queue)." -ForegroundColor Yellow
}
if ($failed -eq 0) {
    Write-Host "COMPAT OK" -ForegroundColor Green
    exit 0
}
Write-Host ("COMPAT FAILED: {0} difference(s)" -f $failed) -ForegroundColor Red
exit 1
