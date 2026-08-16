# Regenerates the User Guide PDF from README.md + guide markdown files:
# - README.md provides the structure and main content (between USERGUIDE:START/END markers)
# - Separate guide files in docs/mkdocs/guide/ provide detailed subsections with ### headers
# - The script assembles them, generates proper nested numbering (1.1, 1.2, 2.1, etc.),
#   and builds a multi-level TOC with clickable anchors and page numbers.
#
# One-time setup:
#   winget install --id JohnMacFarlane.Pandoc -e
#   winget install --id oschwartz10612.Poppler -e   (for pdftotext)
# Uses headless Microsoft Edge (already on Windows) for HTML→PDF rendering.
#
# Two-pass rendering: pass 1 with {{PAGE:id}} placeholders, pdftotext discovers
# real page numbers, pass 2 substitutes them. Contents page is regenerated with
# full nested TOC showing chapter.subsection numbers and their page numbers.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$readme   = Join-Path $repoRoot "README.md"
$docsDir  = Join-Path $repoRoot "docs"
$guideDir = Join-Path (Join-Path $docsDir "mkdocs") "guide"
$mdSlice  = Join-Path $docsDir "_userguide_full.md"
$htmlOut  = Join-Path $docsDir "_userguide_full.html"
$cssFile  = Join-Path $docsDir "_userguide_full.css"
$passPdf  = Join-Path $docsDir "_userguide_pass.pdf"

foreach ($tool in @("pandoc", "pdftotext")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Error "$tool not found. One-time setup: winget install --id JohnMacFarlane.Pandoc -e ; winget install --id oschwartz10612.Poppler -e"
    }
}

$edge = Get-Command msedge -ErrorAction SilentlyContinue
if (-not $edge) {
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

$emdash = [char]0x2014
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# --- extract firmware version from README ----
$text = (Get-Content -Raw -Encoding UTF8 $readme) -replace "`r`n", "`n"
$verMatch = [regex]::Match($text, '\*\*(?:Beta|Release)\s+\W+\s*(v[\d.]+)\.\*\*')
if (-not $verMatch.Success) { Write-Error "Could not find the version in README's banner line (expects '**Beta — vX.Y.Z.**' or '**Release — vX.Y.Z.**')." }
$fwVersion = $verMatch.Groups[1].Value
$pdfName   = "QMX-Panadapter-UserGuide-$fwVersion.pdf"
$pdfOut    = Join-Path $docsDir $pdfName

Get-ChildItem -Path $docsDir -Filter "QMX-Panadapter-UserGuide-v*.pdf" -ErrorAction SilentlyContinue |
    Remove-Item -ErrorAction SilentlyContinue

# --- extract USERGUIDE markers from README ----
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

# --- fix repo-relative links ----
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

# --- define chapters and their guide files ----
$chapters = @(
    @{ Id = "quick-guide";   Title = "Quick Guide";   Num = 1; GuideFile = $null;                                    Desc = "get on air in 10 minutes" },
    @{ Id = "panadapter";    Title = "Panadapter";     Num = 2; GuideFile = (Join-Path $guideDir "panadapter.md");    Desc = "spectrum, waterfall, zoom, touch-to-tune, S-meter, memory channels" },
    @{ Id = "spots";         Title = "Live spots";     Num = 3; GuideFile = (Join-Path $guideDir "spots.md");        Desc = "POTA, RBN and DX cluster callsigns drawn on the spectrum" },
    @{ Id = "web-ui";        Title = "Web UI";         Num = 4; GuideFile = $null;                                    Desc = "browser panadapter and remote control" },
    @{ Id = "ft8-receive";   Title = "FT8 Receive";    Num = 5; GuideFile = (Join-Path $guideDir "ft8-rx.md");       Desc = "onboard decoder, decode list" },
    @{ Id = "ft8-transmit";  Title = "FT8 Transmit";   Num = 6; GuideFile = (Join-Path $guideDir "ft8-tx.md");       Desc = "reply, CQ-run, auto-QSO, ADIF logging" },
    @{ Id = "time-sync";     Title = "Time sync";      Num = 7; GuideFile = (Join-Path $guideDir "time-sync.md");    Desc = "WiFi/SNTP, Tab5 RTC, POTA/offline use" },
    @{ Id = "settings";      Title = "Settings";       Num = 8; GuideFile = (Join-Path $guideDir "settings.md");     Desc = "every drawer control, group by group" },
    @{ Id = "radio-menus";   Title = "Radio menus";    Num = 9; GuideFile = (Join-Path $guideDir "radio-menus.md");  Desc = "the QMX's own menu system on the Tab5 - the only way into a headless QMX+" },
    @{ Id = "reference";     Title = "Reference";      Num = 10; GuideFile = $null;                                   Desc = "gestures, web API, hardware" }
)

$appendices = @(
    @{ Id = "build-from-source";  Title = "Build from source";  Letter = "A"; Desc = "" },
    @{ Id = "under-the-hood";     Title = "Under the hood";     Letter = "B"; Desc = "DSP, I/Q correction, quirks" },
    @{ Id = "roadmap";            Title = "Roadmap";            Letter = "C"; Desc = "" },
    @{ Id = "related-projects";   Title = "Related projects";   Letter = "D"; Desc = "" },
    @{ Id = "license";            Title = "License";            Letter = "E"; Desc = "" }
)

# --- number chapters and inject guide file content ----
$allSubsections = @{}  # Track subsections for TOC: $chapterId => @( @{Id, Num, Title}, ... )

foreach ($c in $chapters) {
    $numbered = "$($c.Num). $($c.Title)"
    $pattern  = "(?m)^## $([regex]::Escape($c.Title))$"

    # Replace chapter header
    $userGuidePart = [regex]::Replace($userGuidePart, $pattern, "## $numbered")
    $c.Numbered = $numbered

    # Inject guide file content if available
    if ($c.GuideFile -and (Test-Path $c.GuideFile)) {
        $guideContent = (Get-Content -Raw -Encoding UTF8 $c.GuideFile) -replace "`r`n", "`n"

        # Remove the top-level heading (# Title)
        $guideContent = $guideContent -replace '(?m)^# [^\n]+\n+', ''

        # Some guide pages are written with plain "## Section" headings and no
        # numbering (settings.md has 21 of them). Those would land at CHAPTER
        # level in the PDF and break the hierarchy, which is why such pages used
        # to be left out of the printable guide altogether - and why whole
        # features were missing from it. Normalise them here instead of
        # renumbering the source, because those headings are anchors: the
        # context-help table and the A-Z index both point at them by text, and
        # pack_manual.py fails the build if one moves.
        if ($guideContent -notmatch '(?m)^###\s+\d+\.\s') {
            $guideContent = [regex]::Replace($guideContent, '(?m)^###\s+', '#### ')
            $n = 0
            $guideContent = [regex]::Replace($guideContent, '(?m)^##\s+([^\n]+)$', {
                param($m)
                $script:n++
                "### $($script:n). $($m.Groups[1].Value)"
            })
        }

        # Extract and number subsections (### and #### both)
        $subsections = @()
        $currentSubNum = 0
        $guideContent = [regex]::Replace($guideContent, '(?m)^(###|####) (\d+)\. ([^\n]+)$', {
            param($match)
            $level = $match.Groups[1].Value
            $origNum = [int]$match.Groups[2].Value
            $title = $match.Groups[3].Value
            $subId = ($title.ToLower() -replace '[^a-z0-9]+', '-' -replace '^-|-$', '').Substring(0, [Math]::Min(40, ($title.ToLower() -replace '[^a-z0-9]+', '-' -replace '^-|-$', '').Length))

            if ($level -eq '###') {
                $currentSubNum = $origNum
                $subsections += @{ Num = $origNum; Title = $title; Id = $subId }
                "### $($c.Num).$origNum. $title"
            } else {
                # #### nested - number as C.S.N
                if ($subsections.Count -gt 0) {
                    if (-not ($subsections[-1].ContainsKey('Nested'))) {
                        $subsections[-1]['Nested'] = @()
                    }
                    $subsections[-1].Nested += @{ Num = $origNum; Title = $title; Id = "$subId" }
                }
                "#### $($c.Num).$currentSubNum.$origNum. $title"
            }
        })

        $allSubsections[$c.Id] = $subsections

        # Append guide content after the chapter header
        # Use string replacement instead of regex to avoid escaping issues
        $userGuidePart = $userGuidePart -replace "(?m)^## $([regex]::Escape($numbered))$", "## $numbered`n`n$guideContent"
    }
}

foreach ($a in $appendices) {
    $numbered = "Appendix $($a.Letter) $emdash $($a.Title)"
    $pattern  = "(?m)^## $([regex]::Escape($a.Title))$"
    $appendixPart = [regex]::Replace($appendixPart, $pattern, "## $numbered")
    $a.Numbered = $numbered
}

# --- build nested TOC with chapter.subsection numbering ----
$tocLines = New-Object System.Collections.Generic.List[string]
$tocLines.Add('<div class="toc">')
$tocLines.Add('<div class="toc-section">Chapters</div>')

foreach ($c in $chapters) {
    $tocLines.Add("<div class=`"toc-row toc-chapter`"><a href=`"#$($c.Id)`">$($c.Numbered)</a><span class=`"toc-fill`"></span><span class=`"toc-page`">{{PAGE:$($c.Id)}}</span></div>")
    if ($c.Desc) { $tocLines.Add("<div class=`"toc-desc`">$($c.Desc)</div>") }

    # Add subsections if this chapter has guide content
    if ($allSubsections.ContainsKey($c.Id)) {
        foreach ($sub in $allSubsections[$c.Id]) {
            $subId = "$($c.Id)-$($sub.Id)"
            $tocLines.Add("<div class=`"toc-row toc-subsection`"><a href=`"#$subId`">$($c.Num).$($sub.Num). $($sub.Title)</a><span class=`"toc-fill`"></span><span class=`"toc-page`">{{PAGE:$subId}}</span></div>")

            # Add nested sub-subsections if they exist
            if ($sub.ContainsKey('Nested') -and $sub.Nested.Count -gt 0) {
                foreach ($nested in $sub.Nested) {
                    $nestedId = "$subId-$($nested.Id)"
                    $tocLines.Add("<div class=`"toc-row toc-subsubsection`"><a href=`"#$nestedId`">$($c.Num).$($sub.Num).$($nested.Num). $($nested.Title)</a><span class=`"toc-fill`"></span><span class=`"toc-page`">{{PAGE:$nestedId}}</span></div>")
                }
            }
        }
    }
}

$tocLines.Add('<div class="toc-section">Appendices</div>')
foreach ($a in $appendices) {
    $tocLines.Add("<div class=`"toc-row toc-chapter`"><a href=`"#$($a.Id)`">$($a.Numbered)</a><span class=`"toc-fill`"></span><span class=`"toc-page`">{{PAGE:$($a.Id)}}</span></div>")
    if ($a.Desc) { $tocLines.Add("<div class=`"toc-desc`">$($a.Desc)</div>") }
}
$tocLines.Add('</div>')
$tocHtml = [string]::Join("`n", $tocLines)

$contentsIdx = $userGuidePart.IndexOf("## Contents")
if ($contentsIdx -lt 0) { Write-Error "Could not find '## Contents' in README.md" }
$sepIdx = $userGuidePart.IndexOf("---", $contentsIdx)
if ($sepIdx -lt 0) { Write-Error "Could not find the '---' separator after Contents" }
$userGuidePart = $userGuidePart.Substring(0, $contentsIdx) + "## Contents`n`n$tocHtml`n`n" + $userGuidePart.Substring($sepIdx)

# --- title page + appendix divider ----
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

# --- print-friendly CSS ----
$css = @"
@page {
  size: A4;
  margin: 24mm 18mm 22mm 18mm;
  @bottom-center { content: counter(page); font-size: 9pt; color: #999; }
}
body { font-family: -apple-system, Segoe UI, Helvetica, Arial, sans-serif; max-width: 900px; margin: 0 auto; padding: 0 1em; line-height: 1.45; color: #222; orphans: 3; widows: 3; }
h1, h2, h3 { color: #11324d; break-after: avoid; page-break-after: avoid; }
h2 { border-bottom: 1px solid #ccc; padding-bottom: 4px; margin-top: 2em; }
h3 { font-size: 1.1em; margin-top: 1.2em; }
p, li, blockquote, pre, table { break-inside: avoid; page-break-inside: avoid; }
tr { break-inside: avoid; page-break-inside: avoid; }
table { border-collapse: collapse; width: 100%; margin: 1em 0; }
th, td { border: 1px solid #bbb; padding: 6px 10px; text-align: left; }
th { background: #f0f0f0; }
code, pre { background: #f5f5f5; border-radius: 3px; }
pre { padding: 10px; overflow-x: auto; }
img { max-width: 100%; break-inside: avoid; page-break-inside: avoid; }
p:has(> img) { break-after: avoid; page-break-after: avoid; }
p:has(+ table), p:has(+ pre), p:has(+ blockquote),
p:has(+ p + table), p:has(+ p + pre), p:has(+ p + blockquote) { break-after: avoid; page-break-after: avoid; }
blockquote { border-left: 4px solid #11324d; margin: 1em 0; padding: 0.2em 1em; background: #f7f9fb; }
a { color: #0a5ba8; text-decoration: none; }
.toc { column-count: 2; column-gap: 2em; break-inside: avoid; page-break-inside: avoid; }
.toc-section { font-weight: bold; color: #11324d; font-size: 1.15em; margin: 1.4em 0 0.6em 0; column-span: all; }
.toc-row { display: flex; align-items: baseline; margin: 0.35em 0; break-inside: avoid; page-break-inside: avoid; }
.toc-chapter { margin: 0.7em 0 0.2em 0; }
.toc-chapter a { font-weight: 700; font-size: 1em; color: #11324d; }
.toc-subsection { margin: 0.25em 0 0 2.5em; }
.toc-subsection a { font-weight: 600; font-size: 0.95em; color: #1a4d7a; }
.toc-subsubsection { margin: 0em 0 0 5em; }
.toc-subsubsection a { font-weight: 500; font-size: 0.88em; color: #2d5a8c; }
.toc-row a { text-decoration: none; }
.toc-row a:hover { text-decoration: underline; }
.toc-fill { flex: 1; border-bottom: 1px dotted #bbb; margin: 0 0.4em; height: 0.5em; }
.toc-page { min-width: 2.5em; text-align: right; color: #666; font-size: 0.9em; font-weight: 500; }
.toc-desc { color: #888; font-size: 0.82em; margin: -0.15em 0 0.3em 2.5em; font-style: italic; }
"@
[System.IO.File]::WriteAllText($cssFile, $css, $utf8NoBom)

function Build-Pdf([string]$markdownText, [string]$outputPdfPath) {
    [System.IO.File]::WriteAllText($mdSlice, $markdownText, $utf8NoBom)
    & pandoc $mdSlice -f gfm -t html5 --standalone --metadata pagetitle="QMX+ Panadapter User Guide $fwVersion" --css "$([System.IO.Path]::GetFileName($cssFile))" -o $htmlOut
    if ($LASTEXITCODE -ne 0) { Write-Error "pandoc conversion failed" }

    $htmlContent = Get-Content -Raw -Encoding UTF8 $htmlOut

    # Fix chapter header IDs
    foreach ($e in $chapters + $appendices) {
        $escText = [regex]::Escape($e.Numbered)
        $pattern = "<h2 id=`"[^`"]*`">\s*$escText\s*</h2>"
        $replacement = "<h2 id=`"$($e.Id)`">$($e.Numbered)</h2>"
        $htmlContent = $htmlContent -replace $pattern, $replacement
    }

    # Fix subsection and nested subsection header IDs
    foreach ($c in $chapters) {
        if ($allSubsections.ContainsKey($c.Id)) {
            foreach ($sub in $allSubsections[$c.Id]) {
                $escText = [regex]::Escape("$($c.Num).$($sub.Num). $($sub.Title)")
                $subId = "$($c.Id)-$($sub.Id)"
                $pattern = "<h3 id=`"[^`"]*`">\s*$escText\s*</h3>"
                $replacement = "<h3 id=`"$subId`">$($c.Num).$($sub.Num). $($sub.Title)</h3>"
                $htmlContent = $htmlContent -replace $pattern, $replacement

                # Fix nested #### headers
                if ($sub.ContainsKey('Nested') -and $sub.Nested.Count -gt 0) {
                    foreach ($nested in $sub.Nested) {
                        $escNestedText = [regex]::Escape("$($c.Num).$($sub.Num).$($nested.Num). $($nested.Title)")
                        $nestedId = "$subId-$($nested.Id)"
                        $nestedPattern = "<h4 id=`"[^`"]*`">\s*$escNestedText\s*</h4>"
                        $nestedReplacement = "<h4 id=`"$nestedId`">$($c.Num).$($sub.Num).$($nested.Num). $($nested.Title)</h4>"
                        $htmlContent = $htmlContent -replace $nestedPattern, $nestedReplacement
                    }
                }
            }
        }
    }

    [System.IO.File]::WriteAllText($htmlOut, $htmlContent, $utf8NoBom)

    $htmlUri = "file:///" + ($htmlOut -replace '\\', '/')
    Remove-Item $outputPdfPath -ErrorAction SilentlyContinue
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $edgePath --headless --disable-gpu --no-sandbox --print-to-pdf="$outputPdfPath" --no-pdf-header-footer $htmlUri 2>$null
    $ErrorActionPreference = $prevEap

    # Wait for the file to APPEAR and then to STOP GROWING.
    #
    # This was a flat 15 s, which quietly became too short as the guide grew:
    # at 93 pages a cold headless render overran it, the script errored, and a
    # second run "fixed" it only because the browser was warm. A build step that
    # fails roughly every other time is one people learn to re-run instead of
    # believe, so give it real headroom - it costs nothing when the render is
    # quick. Waiting for a stable size also stops the page-number pass from
    # reading a half-written PDF.
    $deadline = (Get-Date).AddSeconds(120)
    while (-not (Test-Path $outputPdfPath) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $outputPdfPath)) {
        Write-Error "PDF was not produced at $outputPdfPath within 120 s (headless render timed out)"
        return
    }
    $lastLen = -1
    while ((Get-Date) -lt $deadline) {
        $len = (Get-Item $outputPdfPath).Length
        if ($len -gt 0 -and $len -eq $lastLen) { break }
        $lastLen = $len
        Start-Sleep -Milliseconds 400
    }
}

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

# Also collect subsection and nested subsection page numbers
$allEntriesForPages = New-Object System.Collections.Generic.List[object]
foreach ($e in $allEntries) {
    $allEntriesForPages.Add($e)
}
foreach ($c in $chapters) {
    if ($allSubsections.ContainsKey($c.Id)) {
        foreach ($sub in $allSubsections[$c.Id]) {
            $subId = "$($c.Id)-$($sub.Id)"
            $allEntriesForPages.Add(@{ Id = $subId; Numbered = "$($c.Num).$($sub.Num). $($sub.Title)" })

            # Also add nested subsections
            if ($sub.ContainsKey('Nested') -and $sub.Nested.Count -gt 0) {
                foreach ($nested in $sub.Nested) {
                    $nestedId = "$subId-$($nested.Id)"
                    $allEntriesForPages.Add(@{ Id = $nestedId; Numbered = "$($c.Num).$($sub.Num).$($nested.Num). $($nested.Title)" })
                }
            }
        }
    }
}

# Pass 1: render with placeholders
Build-Pdf $fullDocTemplate $passPdf
$pageMap = Get-HeadingPages $passPdf $allEntriesForPages.ToArray()

# Pass 2: substitute real page numbers
function Apply-PageNumbers([string]$docText, $map) {
    $out = $docText
    foreach ($key in $map.Keys) {
        $out = $out.Replace("{{PAGE:$key}}", [string]$map[$key])
    }
    return $out
}
$finalDoc = Apply-PageNumbers $fullDocTemplate $pageMap
Build-Pdf $finalDoc $pdfOut

# Sanity check
$verifyMap = Get-HeadingPages $pdfOut $allEntriesForPages.ToArray()
$mismatch = $false
foreach ($key in $pageMap.Keys) {
    if ($verifyMap[$key] -ne $pageMap[$key]) { $mismatch = $true; break }
}
if ($mismatch) {
    Write-Host "Page numbers shifted; re-rendering with corrected numbers..."
    $finalDoc = Apply-PageNumbers $fullDocTemplate $verifyMap
    Build-Pdf $finalDoc $pdfOut
}

Remove-Item $mdSlice, $htmlOut, $cssFile, $passPdf -ErrorAction SilentlyContinue

# --- keep README link current ----
$newReadmeText = [regex]::Replace(
    (Get-Content -Raw -Encoding UTF8 $readme),
    'docs/QMX-Panadapter-UserGuide(-v[\d.]+)?\.pdf',
    "docs/$pdfName"
)
[System.IO.File]::WriteAllText($readme, $newReadmeText, $utf8NoBom)

Write-Host "✅ Wrote $pdfOut with nested chapter.subsection numbering and clickable TOC"
