# Project Statistics — QMX Panadapter

Snapshot taken **2026-10-15**, at commit `d002dc60`, release **v1.19.0**.

Every number below comes from the repository, the GitHub API, or the session
logs on the development machine. Where a figure is an estimate, the method is
stated. Nothing here is rounded up for effect.

---

## 1. Timeline

| Item | Value |
|---|---|
| First commit | 2026-04-19 18:27 — *"Phase 1 complete — LVGL UI rendering on Tab5 ST7123 display"* |
| Latest commit | 2026-10-15 12:25 |
| Calendar span | 179 days (5.9 months) |
| Days with at least one commit | 139 of 179 (78%) |
| First tagged release | v0.6.0, 2026-05-25 |
| Latest tagged release | v1.19.0, 2026-10-15 |

The project went from a blank display to a radio-connected panadapter in
36 days: first commit 04-19, USB host enumerating the QMX 05-22, real-time FFT
on the glass 05-22, full spectrum and waterfall pipeline 05-25.

### Commits per month

| Month | Commits |
|---|---|
| 2026-04 | 1 |
| 2026-05 | 116 |
| 2026-06 | 285 |
| 2026-07 | 213 |
| 2026-08 | 779 |
| 2026-09 | 551 |
| 2026-10 (to the 15th) | 296 |

August was the peak month. The rate from September onwards is lower because
the work moved from new subsystems to user-reported defects and digital-mode
protocol work, which takes more measurement per commit.

---

## 2. Commits and code churn

| Item | Value |
|---|---|
| Commits on `main` | 2,241 |
| Commits across all branches and authors | 2,377 |
| Lines inserted (all history) | 385,483 |
| Lines deleted (all history) | 105,095 |
| Net lines added | 280,388 |
| File-change events | 7,457 |

Deletions are 27% of insertions. A large part of that is code that was moved
or replaced after a measurement proved the first approach wrong — for example
the autoscaling spectrum range, the waterfall scroll method, and the AM
filter, each of which was removed once it was measured rather than left in
place.

### Contributors

| Author | Commits |
|---|---|
| Steffen OZ1LAV | 2,365 |
| Eric (Unit GPS support) | 3 |
| Other / tooling | 9 |

Eric's three commits were the first contribution from outside the bench and
shipped in v1.16.12.

---

## 3. Releases

| Item | Value |
|---|---|
| Git tags | 156 (154 version tags) |
| GitHub releases published | 153 |
| Release assets | 66 |
| Total asset downloads | 2,379 |
| Releases per week (average) | 7.5 |

154 version tags in 143 days of tagged history. The release cadence is
deliberate: a fix that a user reported goes out as its own version so the
report and the release that answers it can be matched.

---

## 4. Code size, now

Tracked source in the repository at this snapshot.

| Area | Files | Lines |
|---|---|---|
| `main/` — firmware | 329 | 159,102 |
| &nbsp;&nbsp;of which hand-written | 323 | 134,794 |
| &nbsp;&nbsp;of which generated tables | 6 | 24,308 |
| `components/` — forked and vendored | 63 | 25,322 |
| &nbsp;&nbsp;of which `ft8_lib` (vendored) | — | 14,628 |
| `test/` — host-side harnesses | 69 | 15,290 |
| Web interface (HTML + JS) | 2 | 10,934 |
| Documentation (Markdown) | 100 | 23,757 |
| Python tooling | 29 | 4,905 |
| PowerShell tooling | 39 | 5,422 |

**Total tracked source, documentation and tooling: about 245,000 lines.**

The generated tables are the DXCC prefix map (18,365 prefixes, 340 entities),
the world coastline (1,744 rings, 38,587 points, from Natural Earth), a
monospaced display font, and the WSPR metric table. All four are produced by
committed scripts from committed sources, so they are reproducible rather than
hand-maintained.

### Largest hand-written firmware files

| File | Lines |
|---|---|
| `main/ui/ui.c` | 17,807 |
| `main/net/webserver.c` | 8,141 |
| `main/ui/ft8_screen_view.c` | 4,996 |
| `main/ft8_test.c` | 4,139 |
| `main/ft8_qso.c` | 4,091 |
| `main/cat/cat.c` | 3,991 |
| `main/storage/settings.c` | 3,932 |
| `main/wspr_rx.c` | 3,482 |
| `main/ui/spot_map_view.c` | 3,336 |

---

## 5. What the firmware contains

| Item | Value |
|---|---|
| Screens and views | 20 source files |
| Web API endpoints | 37 |
| Digital modes implemented | FT8, FT4, JS8 (Normal / Fast / Turbo / Slow), WSPR |
| Analogue modes | CW, SSB (USB/LSB), AM |
| Firmware binary | 5,204,048 bytes |
| OTA slot | 8,388,608 bytes (62% used) |

### Safety inventory

| Item | Value |
|---|---|
| Code sites that can arm the transmitter | 11 |
| Of those, able to transmit unattended | 6 |

This inventory is checked by `tools/check_unattended_tx.py` on every release.
It exists because the count was once stated from memory and was wrong, and a
user found out before the operator did.

---

## 6. Test and verification infrastructure

| Item | Value |
|---|---|
| Files under `test/` | 196 |
| Standalone host harnesses (own `main()`) | 62 |
| Reference WAV captures committed | 68 |
| Build-blocking checker scripts | 38 in `tools/` |
| Standing upstream patches, auto-applied | 24 |

The 62 host harnesses run on the PC, not the Tab5. They exist so a protocol
change can be proved bit-exact against the reference implementation before
anything is flashed — the JS8 and FT8 tone generators are checked against
`genjs8.f90` and `ft8_lib` output, not against a screenshot.

The 24 standing patches are modifications to upstream ESP-IDF components. They
live in a git-ignored directory, so every one is re-applied by script and a
checker refuses the build if any is missing. That checker has already caught
one patch going stale against a component upgrade, at v1.16.0.

---

## 7. Community

| Item | Value |
|---|---|
| GitHub stars | 54 |
| Forks | 8 |
| Issues opened (all states) | 18 |
| Release downloads | 2,379 |

Named user reports that resulted in a shipped fix during this period include
Randy N4OPI, Bryan N0LUF, Ryan KK6DZB, John W5JSS, Bruce N9JCV, Sam W7STF,
Ralph, Gyula, Jim, Tony and Eric. Several of those reports found defects that
the bench could not reproduce, including a web regression that disabled every
FT8 button on hover and a transmitter that stayed keyed overnight.

---

## 8. Effort

Two independent estimates. They measure different things and they do not
agree, which is expected.

### Commit-cluster estimate — the whole project

Commits are grouped into working sessions with a 90-minute gap threshold, plus
30 minutes of lead-in per session for the work before the first commit.

| Item | Value |
|---|---|
| Working sessions identified | 373 |
| Estimated engineering hours | **763 hours** |
| Equivalent at 8 h/day | 95 working days |

This is a lower bound for thinking time and an upper bound for keyboard time.
It cannot see bench work that produced no commit — an on-air test, a capture
session, a power-cycle sequence — and there was a lot of that.

### Session-log measurement — 2026-09-03 onwards only

Measured from the development session transcripts. Older transcripts have
rotated off the machine, so this window covers 43 days, not 179.

| Item | Value |
|---|---|
| Transcript files | 66 |
| Distinct log entries (deduplicated) | 176,328 |
| Active time, gaps of 10 min or less | **237 hours** |
| Active time, gaps of 15 min or less | 267 hours |

237 active hours in 43 days is 5.5 hours per day, every day, for six weeks.

Resumed sessions re-copy their earlier history into a new transcript file, so
these figures are deduplicated by log-entry id. Without that step the same
hour is counted more than once.

### Busiest times

Peak commit hours are 21:00 and 22:00 local. The busiest weekday is Sunday
(384 commits); the quietest is Tuesday (259).

---

## 9. AI-assisted development cost

The firmware was written with Claude Code as a pair. These figures cover
**2026-09-03 to 2026-10-15 only** — 43 of the project's 179 days. Earlier
transcripts have rotated off the machine.

Read the caveats below before quoting the total. The headline number is not a
measure of text produced and it is not a measure of money.

| Item | Value |
|---|---|
| User turns (prompts I typed) | 3,344 |
| Distinct API requests | 33,884 |
| API requests per user turn (average) | 10.1 |
| Cache read | 15,818,124,089 |
| Cache write | 139,287,808 |
| Output (generated) | 21,794,070 |
| Fresh input | 69,020 |
| **Total** | **15,979,274,987** |

| Model | Requests | Tokens |
|---|---|---|
| Claude Opus 5 | 25,290 | 11,196,163,322 |
| Claude Sonnet 5 | 8,382 | 4,757,744,173 |
| Claude Haiku 4.5 | 189 | 25,367,492 |

### What the total does not mean

**It is not 16 billion tokens of text.** 99.0% of it is cache reads — the same
conversation context re-read on each request. Median 446,000 tokens of context
per request, maximum 966,000. The content that was actually created is the
cache-write plus output figure: **161 million tokens**, about 1% of the total.
That is still roughly 50 times the size of this repository, most of it written
and then discarded.

**It is not a cost.** Priced at Anthropic API list rates the 43-day window
comes to about **$21,000** — $19,700 of it Opus. That is what an API customer
would pay. A subscription is metered by request rate limits in rolling
windows, not by token count, and cache reads are the part a subscription
absorbs. The two figures are not comparable and should not be presented as if
they were.

### Rate, in the unit the plans are measured in

Published subscription limits are stated in prompts per rolling five-hour
window, not in tokens or API requests. One prompt of mine averages 10.1 API
requests, because every tool call — every file read, build, grep and capture —
is its own round trip.

| Item | Value |
|---|---|
| User turns, total | 3,344 |
| Peak user turns in any rolling 5-hour window | 105 |
| Busiest single day | 195 |
| Median active day | 74 |
| Peak API requests in any rolling 5-hour window | 876 |

Context window is a separate figure from the usage limit. Opus 5 carries a
1,000,000-token window as standard, which is why a median request reads
446,000 tokens of cache and the largest read 966,422. Each usage record
contains exactly one API iteration, so those are single-request figures and
not a sum of internal steps — that was checked, because it was the obvious
explanation and it was wrong.

### Correction to the first version of this page

**The first version was wrong by 1.9x.** It reported 30.4 billion
by summing every transcript file. Resumed sessions copy their earlier history
into a new file, usage records included, so 65,031 raw records contained only
33,884 distinct requests. Corrected above.

## 10. Summary

| Metric | Value |
|---|---|
| Duration | 179 days, 2026-04-19 to 2026-10-15 |
| Commits | 2,377 |
| Releases | 153 |
| Lines of source, docs and tooling | ~245,000 |
| Lines written and deleted over the project | 385,483 / 105,095 |
| Host test harnesses | 62 |
| Estimated engineering hours | ~763 |
| Measured AI tokens (43-day window) | 16.0 billion, 99% of it context re-reads |
| Users who found a defect the bench could not | 11 named |

The last line is the one that matters. The bench has four boards, an antenna
and a dummy load, and it still missed a hover regression, a 17-hour
transmission, a six-character grid square and a silently persistent JS8
submode. Every one of those came back from a user.

---

*Generated 2026-10-15 from the repository at `d002dc60`. Methods for each
estimate are stated inline. Token and session figures cover 2026-09-03 onwards
only, because earlier development session logs have rotated off the machine.*
