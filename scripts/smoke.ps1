#requires -version 5
<#
    Boot a TinyOS build in QEMU, feed it a few commands through a TCP
    serial port and print the transcript.  Useful for CI / quick sanity
    tests on Windows.

    We do not use "-serial stdio" here: QEMU's Windows stdio backend drops
    bytes when a whole command line arrives in one burst, which makes
    scripted input unreliable.  TCP has proper flow control.

    usage:  powershell -ExecutionPolicy Bypass -File scripts/smoke.ps1 [-Target rv32]
#>
param(
    [string]$Target = "x86",
    [string]$Image = "",
    [string]$Qemu = "",
    # "run spin &" plus the ps that follows it is the scheduler check: the
    # background task must still be alive (state rdy) when ps runs.
    [string[]]$Commands = @("ls", "cat readme.txt", "hello alpha beta", "hello_printf alpha beta", "reloc", "run spin &", "ps", "wfile demo.txt hello from the filesystem", "cat demo.txt", "mem", "run prot heap", "crash", "uptime", "exit"),
    [int]$Port = 5566,
    [int]$BootWaitMs = 3000,
    [int]$CommandWaitMs = 700
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$qemuName = switch ($Target) {
    "rv32" { "qemu-system-riscv32.exe" }
    "arm"  { "qemu-system-arm.exe" }
    default { "qemu-system-i386.exe" }
}
if (-not $Image) {
    if ($Target -eq "rv32") {
        $Image = Join-Path $root "build\kernel-rv32.elf"
    } elseif ($Target -eq "arm") {
        $Image = Join-Path $root "build\kernel-arm.elf"
    } else {
        $Image = Join-Path $root "build\tinyos.img"
    }
}
if (-not (Test-Path $Image)) { throw "build artifact not found: $Image (run 'tos build -target $Target')" }

if (-not $Qemu) {
    if ($env:TOS_QEMU) { $Qemu = $env:TOS_QEMU }
}
if (-not $Qemu) {
    $candidates = @(
        (Join-Path $env:ProgramFiles "qemu\$qemuName"),
        (Join-Path $env:LOCALAPPDATA "qemu\$qemuName"),
        (Join-Path $env:LOCALAPPDATA "Programs\qemu\$qemuName"),
        "C:\msys64\ucrt64\bin\$qemuName",
        "C:\msys64\mingw64\bin\$qemuName"
    )
    if ($env:TOS_BIN) {
        foreach ($d in $env:TOS_BIN.Split([IO.Path]::PathSeparator)) {
            $candidates += (Join-Path $d $qemuName)
        }
    }
    foreach ($c in $candidates) { if (Test-Path $c) { $Qemu = $c; break } }
}
if (-not $Qemu) {
    $cmd = Get-Command $qemuName -ErrorAction SilentlyContinue
    if ($cmd) { $Qemu = $cmd.Source }
}
if (-not $Qemu) { throw "$qemuName not found (set TOS_QEMU or TOS_BIN)" }

if ($Target -eq "rv32") {
    # No firmware: the kernel ELF is loaded at 0x80000000 and starts in
    # machine mode, so it brings its own trap vector and device setup.
    $qemuArgs = @(
        "-machine", "virt",
        "-m", "256M",
        "-bios", "none",
        "-kernel", $Image,
        "-display", "none",
        "-chardev", "socket,id=tosserial,host=127.0.0.1,port=$Port,server=on,wait=on",
        "-serial", "chardev:tosserial",
        "-monitor", "none",
        "-no-reboot"
    )
} elseif ($Target -eq "arm") {
    # mps2-an385 (Cortex-M3): the kernel ELF is mapped into the SSRAM at
    # address 0 by QEMU, its vector table is the reset entry and UART0 is
    # the only serial port.
    $qemuArgs = @(
        "-machine", "mps2-an385",
        "-kernel", $Image,
        "-display", "none",
        "-chardev", "socket,id=tosserial,host=127.0.0.1,port=$Port,server=on,wait=on",
        "-serial", "chardev:tosserial",
        "-monitor", "none",
        "-no-reboot"
    )
} else {
    $qemuArgs = @(
        "-m", "128M",
        "-drive", "file=$Image,format=raw,if=ide",
        "-boot", "c",
        "-display", "none",
        # QEMU 9+ dropped the "tcp:" chardev shorthand, use an explicit socket.
        "-chardev", "socket,id=tosserial,host=127.0.0.1,port=$Port,server=on,wait=on",
        "-serial", "chardev:tosserial",
        "-monitor", "none"
    )
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Qemu
$psi.Arguments = ($qemuArgs | ForEach-Object {
    if ($_ -match '[\s,]') { '"' + $_ + '"' } else { $_ }
}) -join ' '
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

Write-Host "booting $Image ($Target)"
$proc = [System.Diagnostics.Process]::Start($psi)
$errTask = $proc.StandardError.ReadToEndAsync()

$client = New-Object System.Net.Sockets.TcpClient
# Nagle holds a small write back until the previous one is acked, and a
# guest that is busy (or not draining its UART) delays that ack.  Command
# lines are tiny, so send them immediately.
$client.NoDelay = $true
$connected = $false
for ($i = 0; $i -lt 50; $i++) {
    try {
        $client.Connect("127.0.0.1", $Port)
        $connected = $true
        break
    } catch {
        Start-Sleep -Milliseconds 200
    }
}
if (-not $connected) {
    if (-not $proc.HasExited) { $proc.Kill() }
    throw "could not connect to the QEMU serial port on 127.0.0.1:$Port"
}

$stream = $client.GetStream()
$sb = New-Object System.Text.StringBuilder

# Write raw bytes: a StreamWriter buffered commands until something else
# happened, which made scripted input arrive seconds late.  QEMU is started
# with wait=on, so the VM only boots once we are connected and no early
# output is lost.
function Send-Line([string]$text) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($text + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
}

# Collects output for a fixed window.  It used to wait for a gap in the
# output instead, which never comes while a background program is printing:
# the next command was only sent once that program exited.
function Read-Available([int]$WindowMs) {
    $until = [DateTime]::Now.AddMilliseconds($WindowMs)
    while ([DateTime]::Now -lt $until) {
        if ($stream.DataAvailable) {
            $buf = New-Object byte[] 4096
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) {
                [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n))
                continue
            }
        }
        Start-Sleep -Milliseconds 50
    }
}

Read-Available 500
$deadline = [DateTime]::Now.AddMilliseconds(20000)
while ($sb.ToString() -notmatch "tos>" -and [DateTime]::Now -lt $deadline) {
    Read-Available 300
}
if ($sb.ToString() -notmatch "tos>") {
    if (-not $proc.HasExited) { $proc.Kill() }
    Write-Host $sb.ToString()
    throw "shell prompt never appeared"
}

foreach ($c in $Commands) {
    Send-Line $c
    Read-Available $CommandWaitMs
}

Start-Sleep -Milliseconds 800
Read-Available 300
if (-not $proc.HasExited) { $proc.Kill() }
$proc.WaitForExit()

Write-Host ($sb.ToString() -replace "`r", "")
$err = $errTask.Result
if ($err) { Write-Host "--- qemu stderr ---"; Write-Host $err }

# Compare on the raw transcript: QEMU's Windows socket chardev turns a bare
# LF into CRLF, so the guest's own CR shows up as a doubled "\r".
$out = $sb.ToString()
if ($out -match "KERNEL PANIC" -or $out -match "KERNEL EXCEPTION") {
    Write-Host "SMOKE TEST FAILED" -ForegroundColor Red
    exit 1
}
if ($out -notmatch "tos>") {
    Write-Host "SMOKE TEST FAILED: shell prompt never appeared" -ForegroundColor Red
    exit 1
}
Write-Host "SMOKE TEST OK" -ForegroundColor Green
