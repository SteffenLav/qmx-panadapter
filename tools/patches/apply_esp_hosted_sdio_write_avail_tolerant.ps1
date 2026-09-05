# Standing patch #12 - esp_hosted sdio_drv.c, sdio_is_write_buffer_available().
#
# WHY
# ---
# Stock, this function gives the slave TWO chances to answer with a buffer count
# and then calls _h_restart_host() - a clean esp_restart(). That is a REBOOT of
# the whole device for a transient on the WiFi link, and because it is a
# deliberate restart rather than a panic it leaves NO crash record: the operator
# sees an unexplained reboot with a clean log, which is the worst possible shape
# for a field fault.
#
# Observed here 2026-09-05 at 58 s of uptime:
#   E H_SDIO_DRV: sdio_is_write_buffer_available: SDIO slave unresponsive, restart host
#   I os_wrapper_esp: Restarting host
#   rst:0xc (SW_CPU_RESET)
# and on this board a warm reset with the radio attached is the documented #74
# trigger, so the QMX then needs a manual power cycle. One transient costs the
# session.
#
# The two retries also run back to back with NO delay, so both land inside a
# few microseconds - not a real attempt at riding out a transient. This is the
# same mistake, in the same file, that the TX path had (MAX_SDIO_WRITE_RETRY 2,
# patched 2026-08-16 to 8 attempts with a 2 ms pause).
#
# WHAT THIS DOES
# --------------
# Follows the precedent the RX drain and the TX retry both set in this file:
# prefer losing one frame to killing the link.
#   * 8 attempts with a 2 ms pause instead of 2 with none.
#   * On exhausting them, RETURN A FAILURE rather than restarting. The caller
#     (sdio_write_task) already handles that - it logs and drops the frame - so
#     TCP/UDP retransmits and the link survives.
#   * The restart is KEPT as a last resort behind a consecutive-failure streak,
#     so a genuinely dead link still recovers, while transients cannot
#     accumulate toward a reboot. Any success clears the streak.
#   * Logs the state at each give-up, which is what makes the next occurrence
#     diagnosable at all - stock, it printed one line and rebooted.
#
# Idempotent and marker-guarded. managed_components/ is git-ignored and wiped by
# fullclean, a dependency refresh, and the release process, so re-run this after
# any of those - alongside the other managed_components patches.

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$f = Join-Path $repo "managed_components\espressif__esp_hosted\host\drivers\transport\sdio\sdio_drv.c"

if (-not (Test-Path $f)) { Write-Error "not found: $f  (run idf.py reconfigure first)"; exit 1 }

$src = Get-Content -Raw -Encoding UTF8 $f
if ($src -match "QMX_SDIO_WRITE_AVAIL_TOLERANT") {
    Write-Host "already patched (sdio_is_write_buffer_available tolerant)."
    exit 0
}

$old = @'
	static uint32_t buf_available = 0;
	uint8_t retry = MAX_WRITE_BUF_RETRIES;
	uint32_t max_retry_sdio_not_responding = 2;
'@

$new = @'
	/* QMX_SDIO_WRITE_AVAIL_TOLERANT - see tools/patches/. Stock gave the slave
	 * two back-to-back chances and then rebooted the whole device, leaving no
	 * crash record. 8 attempts with a pause, then drop the frame; restart only
	 * on a sustained streak. */
	static uint32_t buf_available = 0;
	static uint32_t qmx_unresp_streak = 0;
	uint8_t retry = MAX_WRITE_BUF_RETRIES;
	uint32_t max_retry_sdio_not_responding = 8;
'@

if ($src -notmatch [regex]::Escape($old)) { Write-Error "anchor A not found - upstream changed, re-check by hand"; exit 1 }
$src = $src.Replace($old, $new)

$oldB = @'
				max_retry_sdio_not_responding--;
				/* restart the host to avoid the sdio locked out state */

				if (!max_retry_sdio_not_responding) {
					ESP_LOGE(TAG, "%s: SDIO slave unresponsive, restart host", __func__);
					g_h.funcs->_h_restart_host();
				}
				continue;
'@

$newB = @'
				max_retry_sdio_not_responding--;
				if (!max_retry_sdio_not_responding) {
					/* Out of attempts. Drop this frame and let the caller carry
					 * on - one lost packet is retransmitted, a reboot is not.
					 * The counters are logged because stock printed one line and
					 * restarted, which is why this was never diagnosable. */
					qmx_unresp_streak++;
					ESP_LOGE(TAG,
						"%s: slave unresponsive after 8 tries - dropping frame "
						"(need=%u avail=%u streak=%u)",
						__func__, (unsigned)buf_needed,
						(unsigned)buf_available, (unsigned)qmx_unresp_streak);
					if (qmx_unresp_streak >= 32) {
						ESP_LOGE(TAG, "%s: %u consecutive failures - link is dead, restarting",
							__func__, (unsigned)qmx_unresp_streak);
						qmx_unresp_streak = 0;
						g_h.funcs->_h_restart_host();
					}
					return BUFFER_UNAVAILABLE;
				}
				/* A pause, so the eight attempts actually span a transient
				 * instead of all landing inside a few microseconds. */
				g_h.funcs->_h_msleep(2);
				continue;
'@

if ($src -notmatch [regex]::Escape($oldB)) { Write-Error "anchor B not found - upstream changed, re-check by hand"; exit 1 }
$src = $src.Replace($oldB, $newB)

# Any success clears the streak, so transients cannot accumulate toward a reboot.
$oldC = @'
			if (buf_available < buf_needed) {
'@
$newC = @'
			qmx_unresp_streak = 0;   /* the slave answered - see the patch header */
			if (buf_available < buf_needed) {
'@
if ($src -notmatch [regex]::Escape($oldC)) { Write-Error "anchor C not found"; exit 1 }
$src = $src.Replace($oldC, $newC)

Set-Content -Path $f -Value $src -Encoding UTF8 -NoNewline
Write-Host "Patched sdio_is_write_buffer_available: 8 tries with a pause, drop the frame, restart only on a streak."
Write-Host "  $f"
