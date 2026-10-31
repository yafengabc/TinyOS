#requires -version 5
<#
    Boot build/tinyos.img in QEMU, feed it a few commands through a TCP
    serial port and print the transcript.  Useful for CI / quick sanity
    tests on Windows.

    We do not use "-serial stdio" here: QEMU's Windows stdio backend drops
    bytes when a whole command line arrives in one burst, which makes
    scripted input unreliable.  TCP has proper flow control.

    usage:  powershell -ExecutionPolicy Bypass -File scripts/smoke.ps1
#>
param(
    [string]$Image = "",
    [string]$Qemu = "",
    [string[]]$Commands = @("ls", "cat readme.txt", "hello alpha beta", "hello_printf alpha beta", "reloc", "wfile demo.txt hello from ATA disk", "cat demo.txt", "mem", "crash", "uptime", "exit"),
    [int]$Port = 5566,
    [int]$BootWaitMs = 3000,
    [int]$CommandWaitMs = 700
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Image) { $Image = Join-Path $root "build\tinyos.img" }
if (-not (Test-Path $Image)) { throw "disk image not found: $Image (run 'tos build')" }

if (-not $Qemu) {
    if ($env:TOS_QEMU) { $Qemu = $env:TOS_QEMU }
}
if (-not $Qemu) {
    $candidates = @(
        (Join-Path $env:ProgramFiles "qemu\qemu-system-i386.exe"),
        (Join-Path $env:LOCALAPPDATA "qemu\qemu-system-i386.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\qemu\qemu-system-i386.exe"),
        "C:\msys64\ucrt64\bin\qemu-system-i386.exe",
        "C:\msys64\mingw64\bin\qemu-system-i386.exe"
    )
    if ($env:TOS_BIN) {
        foreach ($d in $env:TOS_BIN.Split([IO.Path]::PathSeparator)) {
            $candidates += (Join-Path $d "qemu-system-i386.exe")
        }
    }
    foreach ($c in $candidates) { if (Test-Path $c) { $Qemu = $c; break } }
}
if (-not $Qemu) {
    $cmd = Get-Command qemu-system-i386.exe -ErrorAction SilentlyContinue
    if ($cmd) { $Qemu = $cmd.Source }
}
if (-not $Qemu) { throw "qemu-system-i386 not found (set TOS_QEMU or TOS_BIN)" }

$qemuArgs = @(
    "-m", "128M",
    "-drive", "file=$Image,format=raw,if=ide",
    "-boot", "c",
    "-display", "none",
    # QEMU 9+ dropped the "tcp:" chardev shorthand, use an explicit socket.
    "-chardev", "socket,id=tosserial,host=127.0.0.1,port=$Port,server=on,wait=off",
    "-serial", "chardev:tosserial",
    "-monitor", "none"
)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Qemu
$psi.Arguments = ($qemuArgs | ForEach-Object {
    if ($_ -match '[\s,]') { '"' + $_ + '"' } else { $_ }
}) -join ' '
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

Write-Host "booting $Image"
$proc = [System.Diagnostics.Process]::Start($psi)
$errTask = $proc.StandardError.ReadToEndAsync()

$client = New-Object System.Net.Sockets.TcpClient
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
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true
$sb = New-Object System.Text.StringBuilder

# Kick the chardev; some QEMU TCP backends only report readability after
# the first write from the client.
$writer.Write("`n")

function Read-Available([int]$SettleMs) {
    $idle = 0
    while ($idle -lt $SettleMs) {
        if ($stream.DataAvailable) {
            $buf = New-Object byte[] 4096
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) {
                [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n))
                $idle = 0
                continue
            }
        }
        Start-Sleep -Milliseconds 100
        $idle += 100
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
    $writer.Write($c + "`n")
    Read-Available $CommandWaitMs
}

Start-Sleep -Milliseconds 800
Read-Available 300
if (-not $proc.HasExited) { $proc.Kill() }
$proc.WaitForExit()

Write-Host $sb.ToString()
$err = $errTask.Result
if ($err) { Write-Host "--- qemu stderr ---"; Write-Host $err }

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
