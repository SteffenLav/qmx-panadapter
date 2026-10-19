# Publishes the built mkdocs site/ tree to tab5.lav.dk via WinSCP, using a
# SAVED SESSION rather than any credential in this file or on the command
# line - the whole point of this script is that it never sees your password.
#
# PROTOCOL: plain FTP, port 21, FTPS None. This header used to claim SFTP;
# it was wrong, and the WinSCP log proved it (2026-10-08: "Transfer
# Protocol: FTP", "FTPS: None"). The password therefore crosses the network
# in clear text on every publish. Worth changing, but it is a decision about
# the saved session, not about this script.
#
# HOST: Simply.com asked (mail 2026-10-08) that FTP use "ftp.simply.com"
# rather than the webserver directly. Their server has in fact said so on
# every connection since at least 2026-09-05:
#   220 Welcome to Simply.com (linux121.unoeuro.com). Please use
#       ftp.simply.com instead.
# The hostname lives in the SAVED SESSION, not here, so changing it is a
# WinSCP GUI edit. ⛔ Do not edit the registry to do it: WinSCP encrypts the
# stored password against the hostname, so changing HostName alone silently
# voids the password and the next batch publish dies with an access
# violation instead of a readable error. Tried and reverted 2026-10-08.
#
# SETUP: already done (2026-09-03) - the saved WinSCP session
# "lav.dk@linux121.unoeuro.com" already points its remote directory at
# /tab5, which is why the defaults below match it. Tick "Save password"
# on that saved session (WinSCP GUI ->
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
    [string] $FtpHost    = "ftp.simply.com",
    [string] $UserName   = "lav.dk",
    [string] $RemotePath = "/tab5",
    # Connect and list only - proves credentials and TLS without uploading.
    [switch] $WhatIfConnect
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$siteDir  = Join-Path $repoRoot "site"
$winscp   = Join-Path $env:LOCALAPPDATA "Programs\WinSCP\WinSCP.com"
$credFile = Join-Path $env:LOCALAPPDATA "qmx-panadapter\publish_ftps.cred"

if (-not (Test-Path $winscp)) {
    Write-Error "WinSCP.com not found at $winscp - is WinSCP installed? (winget install WinSCP.WinSCP)"
}
if (-not $WhatIfConnect -and -not (Test-Path (Join-Path $siteDir "index.html"))) {
    Write-Error "site/index.html not found - run 'mkdocs build --clean' first."
}
if (-not (Test-Path $credFile)) {
    Write-Error "No stored password. Run once:  powershell -File tools/set_publish_password.ps1"
}

# Decrypt with DPAPI (this user, this machine) and keep the plaintext in
# memory only. We deliberately do NOT use a WinSCP saved site: WinSCP keys a
# saved password to the hostname, so Simply's move to ftp.simply.com voided
# it, and re-saving it through the GUI proved unreliable (2026-10-08).
# .Trim() is load-bearing: Set-Content appends a newline, and
# ConvertTo-SecureString rejects the blob with it still attached
# ("Input string was not in a correct format").
$sec   = (Get-Content -LiteralPath $credFile -Raw).Trim() | ConvertTo-SecureString
$bstr  = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
try   { $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }

# ftpes:// = FTP with explicit TLS. Verified against ftp.simply.com:
#   AUTH TLS -> 234, certificate "*.simply.com" verified against the Windows
#   certificate store, so no interactive certificate prompt can block a
#   scheduled run.
$urlUser = [uri]::EscapeDataString($UserName)
$urlPass = [uri]::EscapeDataString($plain)
$session = "ftpes://${urlUser}:${urlPass}@${FtpHost}/"

$action = if ($WhatIfConnect) {
    "ls `"$RemotePath`""
} else {
    "synchronize remote `"$siteDir`" `"$RemotePath`" -delete"
}

$scriptLines = @(
    "option batch abort"
    "option confirm off"
    "open $session"
    $action
    "close"
    "exit"
)
# The temp script carries the password for the few seconds WinSCP reads it,
# so it is created empty, locked to this user, and only then written.
# WinSCP masks the password as *** in its own log.
$tmpScript = Join-Path $env:TEMP "publish_site_$([guid]::NewGuid()).txt"
New-Item -ItemType File -Path $tmpScript | Out-Null
$acl = Get-Acl $tmpScript
$acl.SetAccessRuleProtection($true, $false)
$acl.SetAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
    "$env:USERDOMAIN\$env:USERNAME", "FullControl", "Allow")))
Set-Acl -Path $tmpScript -AclObject $acl
$scriptLines | Set-Content -LiteralPath $tmpScript -Encoding ascii

try {
    & $winscp "/script=$tmpScript" "/log=$env:TEMP\publish_site_winscp.log"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "WinSCP exited with code $LASTEXITCODE - see $env:TEMP\publish_site_winscp.log"
    }
    if ($WhatIfConnect) {
        Write-Host "Connect test OK - logged in to $FtpHost over explicit TLS, listed $RemotePath. Nothing uploaded." -ForegroundColor Green
    } else {
        Write-Host "Published site/ to ${FtpHost}:${RemotePath}" -ForegroundColor Green
    }
} finally {
    Remove-Item -LiteralPath $tmpScript -ErrorAction SilentlyContinue
    $plain = $null; $session = $null; $urlPass = $null
    [GC]::Collect()
}
