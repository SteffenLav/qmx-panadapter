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

**Gesture design decision (open):** right-edge swipe already opens the settings
drawer. Cleanest is to make the left-edge swipe *cycle* Panadapter → FT8 →
Reader (→ future CW) instead of a 2-way toggle, with a small page-indicator.
This scales to the "future CW decode page" the operator mentioned.

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
| **B.** Purpose-built simplified feed from our own site + small markdown-subset renderer | Medium (clean headings, bold, lists, code, links-as-text) | 2–3 d | **RECOMMENDED** — we own the pipeline, no HTML parsing on-device |
| **C.** Server renders each page to a PNG; device just displays + scrolls | High (pixel-faithful) | needs a **dynamic server** | Our site is static (mkdocs → FTP, no compute), so no place to run headless-Chrome; images also heavy over this link. **Doesn't fit hosting** |

**Recommendation: Approach B.** Render a *markdown subset* into a scrollable
LVGL container:

- headings → the montserrat font sizes already built in `sdkconfig`
  (14/18/20/22/24/28/32)
- **bold**, bullet / numbered lists, monospace code blocks
- links rendered as inert text (or, at most, navigable within a whitelist of our
  own paths — see the "only this page" note)
- inline images = **phase 2** (fetch + LVGL JPEG/PNG decode into PSRAM; adds
  bandwidth + weight; ship text-first)

Renderer is a few hundred lines of LVGL, no HTML engine, no fragile parsing.

### 1d. Content pipeline on the site side  (~0.5 day)

Because we own tab5.lav.dk, emit a device-friendly variant alongside the normal
mkdocs build so the device never parses themed HTML:

- Add a small build step (mkdocs plugin/hook or a post-`mkdocs build` script) that
  writes a stripped **markdown or plain-text** copy of each page — e.g.
  `tab5.lav.dk/reader/index.md` (+ per-page files, or one concatenated file).
- The operator's existing `mkdocs build` → FTP `site/` flow carries these along
  automatically; no new manual step (see the release-process memory).
- The device fetches one small file. Keep an index/manifest listing available
  pages so the Reader can offer a table of contents.

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
   `https://api.github.com/repos/SteffenLav/qmx-panadapter/releases/latest`
   - **Must send a `User-Agent` header** — GitHub returns 403 without one.
   - HTTPS via the same mbedtls cert bundle already used for uploads.
   - `/releases/latest` excludes drafts and pre-releases → only stable releases
     trigger a notice. (Use `/releases` and take `[0]` if betas should count.)
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

### 2c. Lower-effort alternative (or fallback) — a static version file on tab5.lav.dk

Since the operator already FTPs to tab5.lav.dk, an even simpler mechanism is a
hand-or-build-maintained `tab5.lav.dk/latest.json`:

```json
{ "version": "v1.0.2", "url": "https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.0.2", "notes_url": "reader/whatsnew-v1.0.2.md" }
```

- Pros: no GitHub API dependency or rate limit; served from the same host the
  Reader already talks to; fully under the operator's control (bump it in the
  same FTP push).
- Cons: one more thing to remember to update at release time (could be emitted by
  the release/mkdocs build step so it's automatic).
- The two aren't exclusive: GitHub API as source of truth, static file as a
  zero-dependency fallback if the API call fails.

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

## Open decisions (need operator input before build)

1. Gesture model: cycle left-swipe through pages, or a separate gesture for the
   Reader?
2. Content feed: per-page files + a table-of-contents manifest, or one
   concatenated document?
3. Update check: GitHub API, static `latest.json` on tab5.lav.dk, or both
   (API primary + static fallback)?
4. Do pre-release/beta tags count as "updates", or stable-only?
5. Inline images in phase 1, or defer to phase 2?
