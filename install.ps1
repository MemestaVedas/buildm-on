<#
.SYNOPSIS
Installs Buildm-on to a user-level bin directory and adds it to the PATH.

.DESCRIPTION
This script will copy the compiled buildm-on.exe to $env:LOCALAPPDATA\buildm-on\bin
and ensure that this directory is in the current user's PATH environment variable.
#>

$exePath = ".\build\buildm-on.exe"
if (-not (Test-Path $exePath)) {
    Write-Host "buildm-on.exe not found! Please compile it first using 'cmake --build build'." -ForegroundColor Red
    exit 1
}

$installDir = "$env:LOCALAPPDATA\buildm-on\bin"
if (-not (Test-Path $installDir)) {
    New-Item -ItemType Directory -Path $installDir | Out-Null
}

# Cleanup old buildmon.exe if it exists
if (Test-Path "$installDir\buildmon.exe") {
    Remove-Item -Path "$installDir\buildmon.exe" -Force
    Write-Host "Removed old buildmon.exe from $installDir" -ForegroundColor Yellow
}

Copy-Item -Path $exePath -Destination $installDir -Force
Write-Host "Copied buildm-on.exe to $installDir" -ForegroundColor Green

# Check if $installDir is in PATH
$userPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($userPath -notmatch [regex]::Escape($installDir)) {
    $newUserPath = "$userPath;$installDir"
    [Environment]::SetEnvironmentVariable("PATH", $newUserPath, "User")
    Write-Host "Added $installDir to your user PATH." -ForegroundColor Yellow
    Write-Host "Please restart your terminal to use the 'buildm-on' command." -ForegroundColor Cyan
}
else {
    Write-Host "$installDir is already in your PATH." -ForegroundColor Green
    Write-Host "You can now run 'buildm-on' from anywhere!" -ForegroundColor Cyan
}
