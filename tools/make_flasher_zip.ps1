# Build the flasher archive that ships as a GitHub release asset.
#
# ⛔ EXPLICIT FILE LIST, NEVER A WILDCARD.
#
# v1.8.8 shipped a 44 MB asset because the zip was built with
#     Compress-Archive -Path "tools\QMX-Panadapter flasher\*"
# and that directory accumulates every historical flasher zip - 27 of them,
# 40 MB - so the release carried a decade of its own past. They are gitignored,
# so nothing in the repo or in `git status` hinted at it, and the only reason it
# was ever noticed is that a user said the download had got big (Gyula HA3HZ).
# Users were downloading 44 MB to receive 3.4 MB of firmware.
#
# A wildcard over a directory that accumulates build output will eventually
# sweep something up. This lists what belongs and fails on anything missing, so
# a too-SMALL archive is caught as loudly as a too-large one.

$ErrorActionPreference = "Stop"
$repo  = Split-Path -Parent $PSScriptRoot
$dir   = Join-Path $repo "tools\QMX-Panadapter flasher"
$stage = Join-Path $repo "scratchpad\flasher_stage"

# The version comes from the BINARY's own descriptor, not from a parameter -
# an asset named for one version containing another is the worst outcome here.
$binPath = Join-Path $dir "qmx_panadapter.bin"
if (-not (Test-Path $binPath)) { Write-Error "No qmx_panadapter.bin in the flasher directory - run the release build first." }
$bytes = [System.IO.File]::ReadAllBytes($binPath)
# 0xabcd5432 exceeds Int32.MaxValue, and PowerShell parses a bare hex literal
# that big as a NEGATIVE Int32 - the comparison then always fails. Use the
# decimal value so the check tests the binary rather than the parser.
# (0xabcd5432 == 2882360370 - computed, not done in my head, which is how
#  the first attempt got it wrong and made a good binary look corrupt.)
if ([BitConverter]::ToUInt32($bytes, 32) -ne 2882360370) { Write-Error "qmx_panadapter.bin has no valid app descriptor." }
$ver = [System.Text.Encoding]::ASCII.GetString($bytes, 48, 32).Split([char]0)[0]
if ($ver -notmatch '^v\d+\.\d+\.\d+$') { Write-Error "Refusing to package a non-release version: '$ver'" }

$files = @(
    "README.txt",
    "flash.bat", "flash.command",
    "flash-recovery.bat", "flash-recovery.command",
    "bootloader.bin", "partition-table.bin", "qmx_panadapter.bin",
    # 0x920000 - resets the boot slot so a cable flash BOOTS what it just wrote.
    # Without it a device with a staged OTA boots the staged image instead, and
    # the operator concludes the flash failed. Caught on the bench at v1.9.3.
    "ota_data_initial.bin"
)

Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in $files) {
    $src = Join-Path $dir $f
    if (-not (Test-Path $src)) { Write-Error "Missing from the flasher directory: $f" }
    Copy-Item $src $stage
}
$rec = Join-Path $dir "recovery-files"
if (-not (Test-Path $rec)) { Write-Error "Missing recovery-files/" }
Copy-Item $rec $stage -Recurse

$zip = Join-Path $repo "scratchpad\QMX-Panadapter-flasher-$ver.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip

$mb = (Get-Item $zip).Length / 1MB
Write-Output ("Built {0}  ({1:N2} MB) for {2}" -f (Split-Path $zip -Leaf), $mb, $ver)
# A correct archive is ~3 MB. Anything near ten is the wildcard bug returning.
if ($mb -gt 10) { Write-Error "Archive is ${mb} MB - far larger than expected. Something swept in extra files." }
Write-Output "Attach with: gh release upload $ver `"$zip`""
