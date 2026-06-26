# Regenerates the User Guide PDF from README.md: the user-facing slice
# (between the USERGUIDE:START / USERGUIDE:END markers) as numbered chapters,
# followed by the rest of README.md as lettered Appendices, with a printable
# Contents page carrying REAL page numbers (so it's usable when printed).
#
# One-time setup:
#   winget install --id JohnMacFarlane.Pandoc -e
#   winget install --id oschwartz10612.Poppler -e   (gives us pdftotext, used
#                                                     to discover real page
#                                                     numbers for the TOC)
# Uses headless Microsoft Edge (already on Windows) to turn HTML into PDF, so
# no LaTeX install is needed.
#
# Page numbers in a TOC can't be known before the document is paginated, so
# this does two render passes: pass 1 renders with placeholder tokens in the
# Contents table, pdftotext finds which page each chapter/appendix actually
# landed on, then pass 2 substitutes the real numbers and renders the final
# PDF. A page-count sanity check re-verifies after pass 2 and redoes the
# substitution once more if the Contents page itself changed size enough to
# shift anything (rare, but cheap to guard against).
#
# The output filename and title carry the firmware version parsed from
# README's beta banner line. Re-running this deletes any previously
# generated version.
#
# Run this after editing README.md, and commit the regenerated PDF (and the
# updated download-link line in README.md, which this script also rewrites)
# alongside the README change.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$readme   = Join-Path $repoRoot "README.md"
$docsDir  = Join-Path $repoRoot "docs"
$mdSlice  = Join-Path $docsDir  "_userguide_full.md"
$htmlOut  = Join-Path $docsDir  "_userguide_full.html"
$cssFile  = Join-Path $docsDir  "_userguide_full.css"
$passPdf  = Join-Path $docsDir  "_userguide_pass.pdf"

foreach ($tool in @("pandoc", "pdftotext")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Error "$tool not found. One-time setup: winget install --id JohnMacFarlane.Pandoc -e ; winget install --id oschwartz10612.Poppler -e"
    }
}

$edge = Get-Command msedge -ErrorAction SilentlyContinue
if (-not $edge) {
    # The classic "Edge\Application\msedge.exe" path can be a stale stub on
    # machines where Edge migrated to per-channel install folders (seen on
    # this dev box: real binary under "EdgeCore\<version>\msedge.exe", with
    # only pwahelper.exe left at the old path) - glob a few more patterns,
    # newest version first, rather than relying on one fixed legacy path.
    $candidates = @(
        "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        "C:\Program Files\Microsoft\Edge\Application\msedge.exe"
    )
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $found) {
        $found = Get-ChildItem -Path @(
            "C:\Program Files (x86)\Microsoft\EdgeCore",
            "C:\Program Files\Microsoft\EdgeCore",
            "C:\Program Files (x86)\Microsoft\Edge\Application",
            "C:\Program Files\Microsoft\Edge\Application"
        ) -Filter "msedge.exe" -Recurse -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $found) { Write-Error "Microsoft Edge not found on this machine." }
    $edgePath = $found
} else {
    $edgePath = $edge.Source
}

# [char]0x2014 (em dash) instead of a literal non-ASCII character anywhere in
# this .ps1 file: Windows PowerShell 5.1 reads BOM-less script files with the
# legacy codepage, which mangles literal Unicode characters baked into the
# source (bit us once already on the title block — see git history).
$emdash = [char]0x2014

# [System.Text.Encoding]::UTF8 emits a BOM preamble on every WriteAllText call
# — silently prepended a BOM to README.md's first line once already. Use this
# instead everywhere a UTF8 encoding object is needed in this script.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# --- pull the firmware version out of README's beta banner ------------------
# Normalize to LF: README.md has CRLF line endings on this Windows checkout,
# which silently breaks every "(?m)^...$" regex below ($ matches just before
# \n, leaving a stray \r attached to the end of each "matched" line, so it
# never actually matches) — cost a debugging pass once already, keep this.
$text = (Get-Content -Raw -Encoding UTF8 $readme) -replace "`r`n", "`n"
$verMatch = [regex]::Match($text, '\*\*Beta\s+\W+\s*(v[\d.]+)\.\*\*')
if (-not $verMatch.Success) { Write-Error "Could not find the version in README's beta banner line." }
$fwVersion = $verMatch.Groups[1].Value   # e.g. "v0.18.5"
$pdfName   = "QMX-Panadapter-UserGuide-$fwVersion.pdf"
$pdfOut    = Join-Path $docsDir $pdfName

Get-ChildItem -Path $docsDir -Filter "QMX-Panadapter-UserGuide-v*.pdf" -ErrorAction SilentlyContinue |
    Remove-Item -ErrorAction SilentlyContinue

# --- locate the marker boundary ---------------------------------------------
$startMarker = "<!-- USERGUIDE:START -->"
$endMarker   = "<!-- USERGUIDE:END -->"
$startIdx = $text.IndexOf($startMarker)
$endIdx   = $text.IndexOf($endMarker)
if ($startIdx -lt 0 -or $endIdx -lt 0 -or $endIdx -le $startIdx) {
    Write-Error "Could not find USERGUIDE:START / USERGUIDE:END markers in README.md"
}
$userGuidePart = $text.Substring($startIdx + $startMarker.Length, $endIdx - $startIdx - $startMarker.Length)
$appendixPart  = $text.Substring($endIdx + $endMarker.Length)

$userGuidePart = $userGuidePart -replace '(?m)^Prefer a single printable file\?.*$', ''

# --- fix repo-relative links so they work outside a checkout ----------------
$repoBlobBase = "https://github.com/SteffenLav/qmx-panadapter/blob/main"
$repoTreeBase = "https://github.com/SteffenLav/qmx-panadapter/tree/main"
$fixLinks = {
    param($s)
    $s = $s -replace '\]\(docs/version-history\.md\)', "]($repoBlobBase/docs/version-history.md)"
    $s = $s -replace '\]\(docs/([^)]+\.png)\)', ']($1)'
    $s = $s -replace '\]\(tools/QMX-Panadapter%20flasher\)', "]($repoTreeBase/tools/QMX-Panadapter%20flasher)"
    return $s
}
$userGuidePart = & $fixLinks $userGuidePart
$appendixPart  = & $fixLinks $appendixPart

# --- chapter / appendix numbering -------------------------------------------
$chapters = @(
    @{ Id = "quick-guide";   Title = "Quick Guide";   Num = 1; Desc = "get on air in 10 minutes" },
    @{ Id = "panadapter";    Title = "Panadapter";     Num = 2; Desc = "spectrum, waterfall, zoom, touch-to-tune, S-meter, memory channels" },
    @{ Id = "web-ui";        Title = "Web UI";         Num = 3; Desc = "browser panadapter and remote control" },
    @{ Id = "ft8-receive";   Title = "FT8 Receive";    Num = 4; Desc = "onboard decoder, decode list" },
    @{ Id = "ft8-transmit";  Title = "FT8 Transmit";   Num = 5; Desc = "reply, CQ-run, auto-QSO, ADIF logging" },
    @{ Id = "time-sync";     Title = "Time sync";      Num = 6; Desc = "WiFi/SNTP, Tab5 RTC, POTA/offline use" },
    @{ Id = "reference";     Title = "Reference";      Num = 7; Desc = "gestures, settings drawer, web API, hardware" }
)
$appendices = @(
    @{ Id = "build-from-source";  Title = "Build from source";  Letter = "A"; Desc = "" },
    @{ Id = "under-the-hood";     Title = "Under the hood";     Letter = "B"; Desc = "DSP, I/Q correction, quirks" },
    @{ Id = "roadmap";            Title = "Roadmap";            Letter = "C"; Desc = "" },
    @{ Id = "related-projects";   Title = "Related projects";   Letter = "D"; Desc = "" },
    @{ Id = "license";            Title = "License";            Letter = "E"; Desc = "" }
)

foreach ($c in $chapters) {
    $numbered = "$($c.Num). $($c.Title)"
    $pattern  = "(?m)^## $([regex]::Escape($c.Title))$"
    $userGuidePart = [regex]::Replace($userGuidePart, $pattern, "## $numbered")
    $c.Numbered = $numbered
}
foreach ($a in $appendices) {
    $numbered = "Appendix $($a.Letter) $emdash $($a.Title)"
    $pattern  = "(?m)^## $([regex]::Escape($a.Title))$"
    $appendixPart = [regex]::Replace($appendixPart, $pattern, "## $numbered")
    $a.Numbered = $numbered
}

# --- build the Contents page as a dot-leader TOC with {{PAGE:id}} tokens ----
$tocLines = New-Object System.Collections.Generic.List[string]
$tocLines.Add('<div class="toc">')
$tocLines.Add('<div class="toc-section">Chapters</div>')
foreach ($c in $chapters) {
    $tocLines.Add("<div class=`"toc-row`"><a href=`"#$($c.Id)`">$($c.Numbered)</a><span class=`"toc-fill`"></span><span class=`"toc-page`">{{PAGE:$($c.Id)}}</span></div>")
    if ($c.Desc) { $tocLines.Add("<div class=`"toc-desc`">$($c.Desc)</div>") }
}
$tocLines.Add('<div class="toc-section">Appendices</div>')
foreach ($a in $appendices) {
    $tocLines.Add("<div class=`"toc-row`"><a href=`"#$($a.Id)`">$($a.Numbered)</a><span class=`"toc-fill`"></span><span class=`"toc-page`">{{PAGE:$($a.Id)}}</span></div>")
    if ($a.Desc) { $tocLines.Add("<div class=`"toc-desc`">$($a.Desc)</div>") }
}
$tocLines.Add('</div>')
$tocHtml = [string]::Join("`n", $tocLines)

$contentsIdx = $userGuidePart.IndexOf("## Contents")
if ($contentsIdx -lt 0) { Write-Error "Could not find '## Contents' in README.md" }
$sepIdx = $userGuidePart.IndexOf("---", $contentsIdx)
if ($sepIdx -lt 0) { Write-Error "Could not find the '---' separator after Contents" }
$userGuidePart = $userGuidePart.Substring(0, $contentsIdx) + "## Contents`n`n$tocHtml`n`n" + $userGuidePart.Substring($sepIdx)

# --- title page + appendix divider ------------------------------------------
$titleBlock = @"
# QMX+ Panadapter $emdash User Guide ($fwVersion)

*By Steffen Lav (OZ1LAV). Generated from the project README $emdash for the live page, issues, and releases see https://github.com/SteffenLav/qmx-panadapter*

---

"@
$appendixDivider = @"

# Appendix $emdash Technical Reference

*The sections below are the remaining, more technical parts of the same project README (build instructions, internal design notes, roadmap) $emdash included here so the links above resolve, rather than dead-ending. Skip ahead if you only want operating instructions.*

"@
$fullDocTemplate = $titleBlock + $userGuidePart + $appendixDivider + $appendixPart

# --- print-friendly CSS: typography, dot-leader TOC, and page-break control -
$css = @"
@page {
  size: A4;
  margin: 24mm 18mm 22mm 18mm;
  @bottom-center { content: counter(page); font-size: 9pt; color: #999; }
}
body { font-family: -apple-system, Segoe UI, Helvetica, Arial, sans-serif; max-width: 900px; margin: 0 auto; padding: 0 1em; line-height: 1.45; color: #222; orphans: 3; widows: 3; }
h1, h2, h3 { color: #11324d; break-after: avoid; page-break-after: avoid; }
h2 { border-bottom: 1px solid #ccc; padding-bottom: 4px; margin-top: 2em; }
p, li, blockquote, pre, table { break-inside: avoid; page-break-inside: avoid; }
tr { break-inside: avoid; page-break-inside: avoid; }
table { border-collapse: collapse; width: 100%; margin: 1em 0; }
th, td { border: 1px solid #bbb; padding: 6px 10px; text-align: left; }
th { background: #f0f0f0; }
code, pre { background: #f5f5f5; border-radius: 3px; }
pre { padding: 10px; overflow-x: auto; }
img { max-width: 100%; break-inside: avoid; page-break-inside: avoid; }
p:has(> img) { break-after: avoid; page-break-after: avoid; }
/* Keep lead-in paragraph(s) glued to the table/code-block/quote they
   introduce, so a heading + "here's what follows:" sentence(s) can't get
   stranded alone at the bottom of a page while the thing being introduced
   moves to the next page. Two levels of lookahead covers heading -> one or
   two intro sentences -> table/pre/blockquote, which is every such run in
   this document. */
p:has(+ table), p:has(+ pre), p:has(+ blockquote),
p:has(+ p + table), p:has(+ p + pre), p:has(+ p + blockquote) { break-after: avoid; page-break-after: avoid; }
blockquote { border-left: 4px solid #11324d; margin: 1em 0; padding: 0.2em 1em; background: #f7f9fb; }
a { color: #0a5ba8; text-decoration: none; }
.toc-section { font-weight: bold; color: #11324d; font-size: 1.1em; margin: 1.2em 0 0.4em 0; }
.toc-row { display: flex; align-items: baseline; margin: 7px 0 0 0; break-inside: avoid; page-break-inside: avoid; }
.toc-row a { font-weight: 600; }
.toc-fill { flex: 1; border-bottom: 1px dotted #999; margin: 0 6px; height: 0.6em; }
.toc-page { min-width: 2em; text-align: right; color: #555; }
.toc-desc { color: #777; font-size: 0.85em; margin: -2px 0 4px 0; }
"@
[System.IO.File]::WriteAllText($cssFile, $css, $utf8NoBom)

function Build-Pdf([string]$markdownText, [string]$outputPdfPath) {
    [System.IO.File]::WriteAllText($mdSlice, $markdownText, $utf8NoBom)
    & pandoc $mdSlice -f gfm -t html5 --standalone --metadata pagetitle="QMX+ Panadapter User Guide $fwVersion" --css "$([System.IO.Path]::GetFileName($cssFile))" -o $htmlOut
    if ($LASTEXITCODE -ne 0) { Write-Error "pandoc conversion failed" }

    # gfm's reader doesn't support "## Heading {#id}" header-attribute syntax
    # (and has no extension that adds it), so pandoc auto-slugs each renumbered
    # heading's own id from its visible text instead of the original stable
    # id (e.g. "Appendix A -- Build from source" rather than "build-from-
    # source"). Internal links throughout the doc still point at the original
    # ids, so pin each renumbered heading's id back by exact text match.
    $htmlContent = Get-Content -Raw -Encoding UTF8 $htmlOut
    foreach ($e in $allEntries) {
        $escText = [regex]::Escape($e.Numbered)
        $pattern = "<h2 id=`"[^`"]*`">\s*$escText\s*</h2>"
        $replacement = "<h2 id=`"$($e.Id)`">$($e.Numbered)</h2>"
        $htmlContent = $htmlContent -replace $pattern, $replacement
    }
    [System.IO.File]::WriteAllText($htmlOut, $htmlContent, $utf8NoBom)

    $htmlUri = "file:///" + ($htmlOut -replace '\\', '/')
    Remove-Item $outputPdfPath -ErrorAction SilentlyContinue
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    # Edge logs harmless internal chrome:// task-manager noise to stderr even
    # on success; don't let PowerShell's NativeCommandError treatment of that
    # abort the script.
    & $edgePath --headless --disable-gpu --no-sandbox --print-to-pdf="$outputPdfPath" --no-pdf-header-footer $htmlUri 2>$null
    $ErrorActionPreference = $prevEap

    $deadline = (Get-Date).AddSeconds(15)
    while (-not (Test-Path $outputPdfPath) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $outputPdfPath)) { Write-Error "PDF was not produced at $outputPdfPath" }
}

# Finds which page each chapter/appendix heading lands on, by exact-text
# search through the rendered PDF, skipping the Contents page itself (its own
# TOC entries repeat the same heading text, so the first real match must come
# strictly after Contents) and walking forward so matches stay in document
# order.
function Get-HeadingPages([string]$pdfPath, $entries) {
    $raw = & pdftotext -layout $pdfPath - 2>$null
    $joined = [string]::Join("`n", $raw)
    $pages = $joined -split "`f"
    $contentsPage = 0
    for ($i = 0; $i -lt $pages.Count; $i++) {
        if ($pages[$i] -match '(?m)^\s*Contents\s*$') { $contentsPage = $i; break }
    }
    $cursor = $contentsPage + 1
    $map = @{}
    foreach ($e in $entries) {
        $needle = [regex]::Escape($e.Numbered)
        $found = -1
        for ($i = $cursor; $i -lt $pages.Count; $i++) {
            if ($pages[$i] -match $needle) { $found = $i; break }
        }
        if ($found -lt 0) { $found = $cursor }
        $map[$e.Id] = $found + 1
        $cursor = $found
    }
    return $map
}

$allEntries = $chapters + $appendices

# Pass 1: placeholders still in place -> render -> measure real page numbers.
Build-Pdf $fullDocTemplate $passPdf
$pageMap = Get-HeadingPages $passPdf $allEntries

# Pass 2: substitute real page numbers -> render the final PDF.
function Apply-PageNumbers([string]$docText, $map) {
    $out = $docText
    foreach ($key in $map.Keys) {
        $out = $out.Replace("{{PAGE:$key}}", [string]$map[$key])
    }
    return $out
}
$finalDoc = Apply-PageNumbers $fullDocTemplate $pageMap
Build-Pdf $finalDoc $pdfOut

# Sanity check: confirm the page numbers we printed still match reality now
# that the Contents page itself has real numbers in it (which can, in
# principle, change its own height enough to shift later pages by one). If
# not, do one corrective re-render with the re-measured numbers.
$verifyMap = Get-HeadingPages $pdfOut $allEntries
$mismatch = $false
foreach ($key in $pageMap.Keys) {
    if ($verifyMap[$key] -ne $pageMap[$key]) { $mismatch = $true; break }
}
if ($mismatch) {
    Write-Host "Page numbers shifted after adding the TOC; re-rendering once more with corrected numbers."
    $finalDoc = Apply-PageNumbers $fullDocTemplate $verifyMap
    Build-Pdf $finalDoc $pdfOut
}

Remove-Item $mdSlice, $htmlOut, $cssFile, $passPdf -ErrorAction SilentlyContinue

# --- keep README's download link pointing at the current versioned filename -
$newReadmeText = [regex]::Replace(
    (Get-Content -Raw -Encoding UTF8 $readme),
    'docs/QMX-Panadapter-UserGuide(-v[\d.]+)?\.pdf',
    "docs/$pdfName"
)
[System.IO.File]::WriteAllText($readme, $newReadmeText, $utf8NoBom)

Write-Host "Wrote $pdfOut"
