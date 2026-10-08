# Stores the FTPS password for publish_site.ps1, encrypted so that only this
# Windows user on this machine can read it back.
#
# WHY THIS EXISTS (2026-10-08): publishing used to rely on WinSCP's own
# "saved site" password. Simply.com asked that FTP move from the webserver
# host to ftp.simply.com, and WinSCP encrypts a stored password against the
# hostname - so changing the host silently voided it, and an unattended
# publish then died with an access violation instead of a readable error.
# Re-saving it through the WinSCP GUI turned into a long hunt for a Login
# dialog that the config (AutoStartSession + AutoWorkspace) never shows.
# This takes WinSCP's session store out of the loop entirely.
#
# The password is protected with Windows DPAPI at user scope: the file is
# useless on another machine or under another Windows account, and it is
# never written to the repo.
#
# USAGE - run once, type the password at the prompt:
#   powershell -File tools/set_publish_password.ps1
#
# To change it later, just run it again. To remove it, delete the file this
# prints the path to.

$ErrorActionPreference = "Stop"

$store = Join-Path $env:LOCALAPPDATA "qmx-panadapter"
$file  = Join-Path $store "publish_ftps.cred"

if (-not (Test-Path $store)) { New-Item -ItemType Directory -Path $store | Out-Null }

Write-Host ""
Write-Host "FTPS password for lav.dk@ftp.simply.com" -ForegroundColor Cyan
Write-Host "Typing is hidden. Nothing is echoed and nothing is logged." -ForegroundColor DarkGray
$sec = Read-Host -Prompt "Password" -AsSecureString

if ($sec.Length -eq 0) { Write-Error "Nothing entered - no change made." }

# ConvertFrom-SecureString uses DPAPI (CurrentUser) when no key is given.
ConvertFrom-SecureString -SecureString $sec | Set-Content -LiteralPath $file -Encoding ascii

Write-Host ""
Write-Host "Stored: $file" -ForegroundColor Green
Write-Host "Encrypted for user '$env:USERNAME' on machine '$env:COMPUTERNAME' only." -ForegroundColor Green
Write-Host "Now run: powershell -File tools/publish_site.ps1 -WhatIfConnect" -ForegroundColor DarkGray
Write-Host ""
