# Plan: On-device docs Reader page + automatic GitHub update check

Status: **proposed** (not started). Author note: assessment + design, no code yet.

## Goal

Add a dedicated full-screen "Reader / Guide" view to the Tab5 UI, reachable by
swiping past the Panadapter / FT8 (and future CW-decode) pages, that shows the
content of **tab5.lav.dk** — a single-site kiosk, not a general web browser.
Layered on top: the same page checks GitHub automatically for newer firmware
releases and tells the operator when an update is available.

Two hard constraints shape everything below:

1. **There is no HTML/browser engine on this device and no practical way to put
   one there.** LVGL is a widget toolkit. tab5.lav.dk is Material-for-MkDocs
   (full HTML+CSS+JS). Faithful rendering of that on ESP32-P4 is out of scope by
   orders of magnitude. The realistic feature is a *docs reader*, not a browser.
2. **The C6/esp_hosted WiFi link is low-throughput and fragile.** Keep payloads
   small, cache aggressively, never run a big fetch concurrently with the
   spectrum WebSocket (see the LoTW forge.min.js wedge lesson in CLAUDE.md).

---

## Part 1 — The Reader page

### 1a. Navigation / mode plumbing  (~0.5 day)

`main/ui/ui_mode.h` is currently a 2-state enum (`UI_MODE_PANADAPTER` /
`UI_MODE_FT8`), toggled by `left_edge_swipe_cb` in `main/ui/ui.c`. Two options:

- **Preferred: full-screen overlay that does NOT touch `ui_mode`.** The DSP tasks
  (`fft_task`, `ft8_task`) read `ui_mode` to decide what to do with audio; if the
  Reader is a plain LVGL overlay screen, DSP keeps doing whatever it was and we
  avoid adding an "idle" branch to the audio path. The overlay is shown/hidden by
  the swipe handler and re-foregrounded the same way the edge-swipe strips and
  sim-border already are.
- Alternative: extend the enum to `UI_MODE_READER` and add a discard/idle branch
  to the audio consumers. More invasive; only worth it if the Reader should
  actively quiesce DSP.

**Gesture design — DECIDED:** left-edge swipe *cycles* Panadapter → FT8 →
Reader (→ future CW) instead of the current 2-way toggle, with a small
page-indicator. Right-edge swipe still opens the settings drawer. This scales to
the "future CW decode page" and gives one consistent gesture for all pages.

### 1b. Fetching over the network  (~0.5 day)

HTTPS to our own site is a solved problem: `esp_http_client` + the mbedtls cert
bundle already drive QRZ / eQSL / LoTW (`main/adif/qrz_upload.c`), and mbedtls
allocations are already routed to PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`). Fetching
a page body is the same machinery in reverse. Use a PSRAM-stack background task
(`util/psram_task.h`) and a spinner in the UI while it loads.

### 1c. Rendering — pick a fidelity tier

| Approach | Fidelity | Effort | Verdict |
|---|---|---|---|
| **A.** Fetch live HTML, strip tags → text | Low (layout lost, nav chrome leaks in) | 1–1.5 d | Fragile on-device HTML parsing of a Material theme; **not recommended** |
| **B.** Render the **mkdocs source `.md` files directly** with a small markdown-subset renderer | Medium (clean headings, bold, lists, code, links-as-text) | 2–3 d | **CHOSEN** — same files that build the site, no second copy to maintain, no HTML parsing on-device |
| **C.** Server renders each page to a PNG; device just displays + scrolls | High (pixel-faithful) | needs a **dynamic server** | Our site is static (mkdocs → FTP, no compute), so no place to run headless-Chrome; images also heavy over this link. **Doesn't fit hosting** |

**DECIDED: Approach B, reading the real mkdocs source markdown** (no derived /
stripped export — single source of truth, see 1d). Render a *markdown subset*
into a scrollable LVGL container:

- headings → the montserrat font sizes already built in `sdkconfig`
  (14/18/20/22/24/28/32)
- **bold**, bullet / numbered lists, monospace code blocks
- links rendered as inert text (or, at most, navigable within a whitelist of our
  own paths — see the "only this page" note)
- inline images = **phase 2** (fetch + LVGL JPEG/PNG decode into PSRAM; adds
  bandwidth + weight; ship text-first)

Renderer is a few hundred lines of LVGL, no HTML engine, no fragile parsing.

### 1d. Content pipeline on the site side — DECIDED: no second copy  (~0.5 day)

**Hard requirement from the operator: do NOT maintain a separate stripped/derived
version of the docs.** The Reader consumes the **same mkdocs *source* markdown
files** that build tab5.lav.dk — single source of truth. The only site-side work
is *publishing the raw `.md` tree* alongside the built HTML, not transforming it.

- The site source lives in `docs/mkdocs/` (repo) and builds to `site/` (the
  themed HTML that gets FTP'd). Add a **trivial copy step** to the build/FTP flow
  that mirrors the source `.md` tree to a public path, e.g. `site/md/…` →
  `tab5.lav.dk/md/…`. One line in the release/mkdocs step; carried by the
  existing FTP push (see the release-process memory). No content duplication.
- **Table of contents = mkdocs `nav:`**, the same single source. The device
  reads `mkdocs.yml`'s `nav` (or a tiny index derived from it at build time) to
  get page order + titles that exactly match the site. No hand-maintained
  manifest.
- **Renderer must degrade gracefully on mkdocs/pymdownx syntax** it doesn't fully
  support, since it's reading real source: YAML front-matter (skip), admonitions
  (`!!! note` / `???`), `pymdownx.superfences`/tabbed blocks, tables, snippet
  includes, and **relative image links** (see images = phase 2 below). Unknown
  syntax falls back to readable plain text — never an error.
- The device fetches per-page files on demand (small fetches suit the fragile
  WiFi link) and caches them to SPIFFS.

### 1e. Offline caching  (folded into 1b/1c)

Cache the last successfully-fetched feed to SPIFFS so the Reader still works in
the field with no WiFi (POTA). Show a "cached — last updated <date>" note when
offline. Respect the WiFi-enabled setting; never force WiFi up just to fetch.

### "Only this page" is a freebie

The Reader knows exactly one hard-coded base URL and follows no arbitrary links.
No address bar, links are inert (or whitelisted to our own paths only). The
single-site kiosk property is the default, not extra work.

---

## Part 2 — Automatic GitHub update check

Firmware releases go to **https://github.com/SteffenLav/qmx-panadapter/releases**
(the operator publishes there and then FTPs the docs to tab5.lav.dk). The device
knows its own version via `esp_app_get_description()->version`, which the build
system populates from `git describe` (e.g. `v1.0.1`) — already surfaced in the
boot log (`util/bsp_info.c`), the diag header (`util/diag_log.c`), the bottom bar
(`util/status.c` → `ui_set_bottom_version`), and `/api/status` JSON
(`net/webserver.c`, field `tab5_fw`). No new version-tracking is needed.

### 2a. Primary approach — GitHub Releases API

New module `main/net/update_check.c` (background task, low priority, PSRAM stack):

1. After WiFi is up (and only if WiFi is enabled), and then on a slow timer
   (once per 6–24 h), `GET`:
   `https://api.github.com/repos/SteffenLav/qmx-panadapter/releases?per_page=5`
   - **DECIDED: pre-releases/betas count as updates** → use `/releases` (not
     `/releases/latest`, which excludes them) and take the newest non-draft entry.
   - **Must send a `User-Agent` header** — GitHub returns 403 without one.
   - HTTPS via the same mbedtls cert bundle already used for uploads.
2. Parse the JSON with cJSON (already a dependency). Fields of interest:
   - `tag_name`  → e.g. `"v1.0.2"` (the version to compare)
   - `name` / `body` → release title + **markdown release notes**
   - `html_url` → link to the release page
3. Compare `tag_name` against `esp_app_get_description()->version` with a small
   semver-ish comparator (strip leading `v`, split on `.`, numeric compare;
   tolerate the `vX.Y.Z.N` 4-part tags that exist in history like `v0.9.9.1`).
4. Persist the result to NVS/SPIFFS with a timestamp so reboots don't re-hammer
   the API, and so the "update available" state survives offline.

**Rate limits:** unauthenticated GitHub API is 60 requests/hour per IP — a once-
or-twice-a-day check is comfortably inside that. No token needed (and we must not
ship one).

### 2b. Surfacing it to the operator

- A subtle indicator on the **Reader page header**: "Firmware v1.0.2 available
  (you have v1.0.1)".
- Optionally a small static dot on the bottom bar (same ambient, non-pulsing
  style as the SD dot — informational, not attention-grabbing).
- **Nice synergy with Part 1:** the release `body` is markdown, so the Reader's
  markdown-subset renderer can display the GitHub **"What's new"** notes directly
  in-app — no separate viewer needed. The update check and the Reader reinforce
  each other.
- The device never self-updates; it only *informs*. Flashing stays the operator's
  deliberate act via the flasher (matches the "confirm outward/irreversible
  actions" posture).

### 2c. DECIDED: static latest.json as a fallback (both sources)

GitHub API is the source of truth; a static `tab5.lav.dk/latest.json` is a
zero-dependency **fallback** used only if the API call fails (offline GitHub,
rate limit, DNS). To honour the "no extra manual step" principle, the release /
mkdocs build step should **auto-emit** `latest.json` so it's never hand-bumped —
FTP'd with everything else:

```json
{ "version": "v1.0.2", "url": "https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.0.2", "notes_url": "reader/whatsnew-v1.0.2.md" }
```

- Pros: no GitHub API dependency or rate limit; served from the same host the
  Reader already talks to; fully under the operator's control (bump it in the
  same FTP push).
- Since it's the fallback (not primary), a slightly stale `latest.json` is
  harmless — the API normally answers first.
- Semver compare must tolerate the `vX.Y.Z.N` 4-part tags in history (e.g.
  `v0.9.9.1`) and pre-release suffixes.

---

## Effort summary

| Piece | Effort |
|---|---|
| Nav / gesture / mode plumbing | 0.5 d |
| Network fetch + SPIFFS cache | 0.5–1 d |
| Markdown-subset renderer + scroll/UX | 1.5–2 d |
| mkdocs-side simplified export step | 0.5 d |
| GitHub update-check module + semver compare + NVS persist | 0.5–1 d |
| Surfacing (Reader header, optional bottom-bar dot, in-app release notes) | 0.5 d |
| **Total (nice, field-useful version)** | **~4–5 days** |
| Text-only first cut (no images, GitHub check only) | ~2.5–3 days |

Set expectations up front: the Reader is a **clean, readable text view of the
docs**, not a pixel mirror of the Material-themed site. For an at-the-rig
reference on a 5" panel, text-forward is arguably better than the full desktop
layout anyway.

## Resolved decisions (operator, 2026-07-19)

1. **Gesture:** left-swipe *cycles* Panadapter → FT8 → Reader → (future CW) with
   a page indicator. Not a separate gesture.
2. **Content feed:** the device reads the **actual mkdocs source `.md` files** —
   no derived/stripped copy to maintain. Site-side work is just publishing the
   raw `.md` tree (+ deriving the TOC from `mkdocs.yml` `nav`). Renderer degrades
   gracefully on mkdocs/pymdownx syntax.
3. **Update source:** **both** — GitHub Releases API primary, static
   `latest.json` fallback (build-emitted, not hand-maintained).
4. **Betas:** **pre-releases count** as updates → use `/releases`, take newest
   non-draft.
5. **Images:** deferred to **phase 2** (ship text-first). Because the Reader now
   reads real source markdown, inline images are relative links to real assets on
   tab5.lav.dk — phase 2 fetches + LVGL-decodes them into PSRAM.

### Still to confirm at build time (not blockers)

- Exact public path for the raw `.md` tree (`tab5.lav.dk/md/…` assumed).
- Update-check interval (6 h vs 24 h) and where the "update available" notice
  appears beyond the Reader header (optional bottom-bar dot).
