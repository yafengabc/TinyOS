# Build toscc only.
$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force bin | Out-Null
    go build -trimpath -ldflags="-s -w" -o bin\toscc.exe .
    Write-Output "done: bin/toscc.exe"
} finally {
    Pop-Location
}
