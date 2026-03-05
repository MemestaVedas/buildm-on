<#
.SYNOPSIS
Installs BuildMon to a user-level bin directory and adds it to the PATH.

.DESCRIPTION
This script will copy the compiled buildmon.exe to $env:LOCALAPPDATA\buildmon\bin
and ensure that this directory is in the current user's PATH environment variable.
#>

$exePath = ".\build\buildmon.exe"
if (-not (Test-Path $exePath)) {
    Write-Host "buildmon.exe not found! Please compile it first using 'cmake --build build'." -ForegroundColor Red
    exit 1
}

$installDir = "$env:LOCALAPPDATA\buildmon\bin"
if (-not (Test-Path $installDir)) {
    New-Item -ItemType Directory -Path $installDir | Out-Null
}

Copy-Item -Path $exePath -Destination $installDir -Force
Write-Host "Copied buildmon.exe to $installDir" -ForegroundColor Green

# Check if $installDir is in PATH
$userPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($userPath -notmatch [regex]::Escape($installDir)) {
    $newUserPath = "$userPath;$installDir"
    [Environment]::SetEnvironmentVariable("PATH", $newUserPath, "User")
    Write-Host "Added $installDir to your user PATH." -ForegroundColor Yellow
    Write-Host "Please restart your terminal to use the 'buildmon' command." -ForegroundColor Cyan
} else {
    Write-Host "$installDir is already in your PATH." -ForegroundColor Green
    Write-Host "You can now run 'buildmon' from anywhere!" -ForegroundColor Cyan
}
