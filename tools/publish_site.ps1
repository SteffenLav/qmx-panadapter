# Publishes the built mkdocs site/ tree to tab5.lav.dk via WinSCP, using a
# SAVED SESSION rather than any credential in this file or on the command
# line - the whole point of this script is that it never sees your password.
#
# SETUP: already done (2026-09-03) - the saved WinSCP session
# "lav.dk@linux121.unoeuro.com" (SFTP, host linux121.unoeuro.com) already
# points its remote directory at /tab5, which is why the defaults below
# match it. Tick "Save password" on that saved session (WinSCP GUI ->
# right-click the site -> Edit) if you want this script to run with no
# prompt at all; otherwise WinSCP asks for the password interactively.
#
# USAGE:
#   powershell -File tools/publish_site.ps1
#   powershell -File tools/publish_site.ps1 -RemotePath "/somewhere-else"
#
# This uploads the CONTENTS of site/ (built by `mkdocs build --clean`) to
# the given remote directory, mirroring deletions
# too (-delete), so a page removed locally also disappears on the live site.
# Run `mkdocs build --clean` yourself first if site/ might be stale - this
# script does not rebuild it, only uploads what is already there.

param(
    [string] $SessionName = "lav.dk@linux121.unoeuro.com",
    [string] $RemotePath  = "/tab5"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$siteDir  = Join-Path $repoRoot "site"
$winscp   = Join-Path $env:LOCALAPPDATA "Programs\WinSCP\WinSCP.com"

if (-not (Test-Path $winscp)) {
    Write-Error "WinSCP.com not found at $winscp - is WinSCP installed? (winget install WinSCP.WinSCP)"
}
if (-not (Test-Path (Join-Path $siteDir "index.html"))) {
    Write-Error "site/index.html not found - run 'mkdocs build --clean' first."
}

# /command mode: open the saved session by NAME (no host/user/password here
# at all), synchronize local site/ -> remote path, mirroring deletions.
$scriptLines = @(
    "option batch abort"
    "option confirm off"
    "open $SessionName"
    "synchronize remote `"$siteDir`" `"$RemotePath`" -delete"
    "close"
    "exit"
)
$tmpScript = Join-Path $env:TEMP "publish_site_$([guid]::NewGuid()).txt"
$scriptLines | Set-Content -LiteralPath $tmpScript -Encoding ascii

try {
    & $winscp "/script=$tmpScript" "/log=$env:TEMP\publish_site_winscp.log"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "WinSCP exited with code $LASTEXITCODE - see $env:TEMP\publish_site_winscp.log"
    }
    Write-Host "Published site/ to '$SessionName':$RemotePath" -ForegroundColor Green
} finally {
    Remove-Item -LiteralPath $tmpScript -ErrorAction SilentlyContinue
}
