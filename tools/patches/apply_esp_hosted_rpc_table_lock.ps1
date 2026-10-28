<#
.SYNOPSIS
    Puts a lock on esp_hosted's synchronous-RPC table, and gives each waiter
    ownership of its own semaphore.

.DESCRIPTION
    sync_rsp_table[] and the `uid` counter in
    host/drivers/rpc/core/rpc_core.c are touched by EVERY task that makes a
    synchronous RPC, and stock has no lock on either. On this device that is
    at least four tasks: httpd (twice per /api/status, for ssid and rssi),
    the status task, the WiFi watchdog probe every 30 s, and the WiFi config
    UI.

    The claim site is a bare scan-then-write:

        if (!sync_rsp_table[i].uid) {
            ... destroy whatever semaphore the slot still holds ...
            sync_rsp_table[i].uid = app_req->uid;
            sync_rsp_table[i].sem = app_req->rx_sem;

    Two tasks can both see slot i free. The second destroys the semaphore the
    first has just installed and is about to block on - and
    wait_for_sync_response() re-read that semaphore OUT OF THE TABLE at take
    time rather than using its own. Taking a freed FreeRTOS queue gives:

        assert failed: spinlock_acquire spinlock.h:142 (lock->count == 0)
        task: httpd  core 1   after 332.375 s of uptime

    Bench 2026-10-28, v1.22.0-84-g6a19e6bf, in
    status_handler -> wifi_get_ssid -> rpc_wifi_sta_get_ap_info, fifteen
    seconds after a Req[0x126] timeout.

    THE 2026-10-24 tlsf_free CRASH WAS THE SAME MISSING LOCK FROM THE OTHER
    END. apply_esp_hosted_rpc_orphan_resp.ps1's QMX_RPC_SEM_UAF half moved the
    destroy out of the waiter and into the claim path. That stopped the RX
    thread freeing a live semaphore and started the claim path doing it. It
    moved which thread trips over the race; it did not remove the race.

    What this does:

      - one mutex guards every read and write of sync_rsp_table[];
      - `uid` is incremented under it, so two tasks cannot be handed the same
        uid and then both match the FIRST slot carrying it;
      - the waiter blocks on ITS OWN app_req->rx_sem, never on a pointer read
        back out of the table;
      - the waiter clears its slot under the lock and then destroys its own
        semaphore. The deferred destroy is gone and nothing frees a semaphore
        it does not own;
      - the RX thread holds the same lock across check -> queue -> post,
        because a waiter that times out between the check and the post leaves
        an element in rpc_rx_q with nobody to dequeue it - the orphan wedge
        QMX_RPC_DROP_UNCLAIMED_RESP exists to prevent;
      - RPC_RX_QUEUE_SIZE goes 3 -> 8.
        CONFIG_ESP_HOSTED_MAX_SIMULTANEOUS_SYNC_RPC_REQUESTS is 5, so stock
        allowed five callers in flight with room to queue only three replies.
        Deeper than the slot count, the enqueue cannot fill while the lock is
        held.

    It also plugs a leak on the "out of buffer space" path, where stock
    created a semaphore, failed to register it and returned without freeing
    it.

    RUN AFTER apply_esp_hosted_rpc_orphan_resp.ps1 - this patches that
    script's output. Alphabetical order already does that ("orphan" before
    "table"); the script refuses to run if the marker is absent.

    Edits managed_components/ (git-ignored): wiped by `idf.py fullclean`, a
    dependency refresh, or the release's `rm -r managed_components/`.
    Idempotent. tools/check_patches.py fails the build if it is missing.

    NOT PROVEN TO FIX THE CRASH. The race was found by reading, and the crash
    it explains has happened exactly once in 9.2 million lines of bench
    capture. There is no way to make it recur on demand, so this ships on the
    argument, not on a reproduction.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_esp_hosted_rpc_table_lock.ps1
#>

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$core = Join-Path $repo "managed_components/espressif__esp_hosted/host/drivers/rpc/core/rpc_core.c"
$hdr  = Join-Path $repo "managed_components/espressif__esp_hosted/host/drivers/rpc/slaveif/rpc_slave_if.h"

foreach ($f in @($core, $hdr)) {
    if (-not (Test-Path $f)) {
        Write-Host "not found: $f" -ForegroundColor Red
        Write-Host "(managed_components/ not fetched yet - run a build first, then re-run this.)"
        exit 1
    }
}

$HELPERS = @'
/* QMX_RPC_TABLE_LOCK - see tools/patches/apply_esp_hosted_rpc_table_lock.ps1.
 * The table and the uid counter are shared by every task that makes a
 * synchronous RPC and stock locks neither. Everything that touches either
 * one goes through the helpers below. */
static void *sync_tbl_lock = NULL;

static inline void qmx_sync_tbl_lock(void)
{
	if (sync_tbl_lock)
		g_h.funcs->_h_lock_mutex(sync_tbl_lock, HOSTED_BLOCK_MAX);
}

static inline void qmx_sync_tbl_unlock(void)
{
	if (sync_tbl_lock)
		g_h.funcs->_h_unlock_mutex(sync_tbl_lock);
}

/* Caller holds the lock. */
static int qmx_sync_avail_locked(uint32_t u)
{
	int i;

	for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (sync_rsp_table[i].uid == u)
			return CALLBACK_AVAILABLE;
	}
	return CALLBACK_NOT_REGISTERED;
}

/* Caller holds the lock. */
static int qmx_sync_post_locked(uint32_t u)
{
	int i;

	for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (sync_rsp_table[i].uid == u)
			return g_h.funcs->_h_post_semaphore(sync_rsp_table[i].sem);
	}
	return CALLBACK_NOT_REGISTERED;
}

static int qmx_sync_post(uint32_t u)
{
	int ret;

	qmx_sync_tbl_lock();
	ret = qmx_sync_post_locked(u);
	qmx_sync_tbl_unlock();
	return ret;
}

static int qmx_sync_slot_claim(uint32_t u, void *sem)
{
	int i, ret = CALLBACK_NOT_REGISTERED;

	qmx_sync_tbl_lock();
	for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (!sync_rsp_table[i].uid) {
			sync_rsp_table[i].uid = u;
			sync_rsp_table[i].sem = sem;
			ret = CALLBACK_SET_SUCCESS;
			break;
		}
	}
	qmx_sync_tbl_unlock();
	return ret;
}

/* Make this uid unreachable, so no later post can aim at its semaphore. */
static void qmx_sync_slot_release(uint32_t u)
{
	int i;

	qmx_sync_tbl_lock();
	for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (sync_rsp_table[i].uid == u) {
			sync_rsp_table[i].uid = 0;
			sync_rsp_table[i].sem = NULL;
			break;
		}
	}
	qmx_sync_tbl_unlock();
}
'@

$CLAIM = @'
		(void)i;
		if (qmx_sync_slot_claim(app_req->uid, app_req->rx_sem) != CALLBACK_SET_SUCCESS) {
			ESP_LOGE(TAG, "sync sem not registered: out of buffer space");
			/* QMX_RPC_TABLE_LOCK: it never reached the table, so free it here.
			 * Stock returned on this path with the semaphore still allocated. */
			g_h.funcs->_h_destroy_semaphore(app_req->rx_sem);
			app_req->rx_sem = NULL;
			return CALLBACK_NOT_REGISTERED;
		}
		return CALLBACK_SET_SUCCESS;

'@

$WAIT = @'
	/* QMX_RPC_TABLE_LOCK: block on OUR OWN semaphore. Reading it back out of
	 * sync_rsp_table[] let a concurrent claim swap it - or free it - between
	 * the lookup and the take, which is how a freed FreeRTOS queue reached
	 * xSemaphoreTake() and asserted in spinlock_acquire on the httpd task.
	 * The slot is cleared under the lock BEFORE the destroy, so no post can
	 * still be aimed at it, and we free only what we created. */
	(void)i;
	if (!app_req->rx_sem)
		return MSG_ID_OUT_OF_ORDER;
	ret = g_h.funcs->_h_get_semaphore(app_req->rx_sem, timeout_sec);
	qmx_sync_slot_release(app_req->uid);
	g_h.funcs->_h_destroy_semaphore(app_req->rx_sem);
	app_req->rx_sem = NULL;
	return ret;

'@

$c = (Get-Content -Raw -Path $core) -replace "`r`n", "`n"

if ($c -match "QMX_RPC_TABLE_LOCK") {
    Write-Host "Already patched (RPC table lock) - nothing to do." -ForegroundColor Green
} else {
    if ($c -notmatch "QMX_RPC_SEM_UAF") {
        Write-Host "apply_esp_hosted_rpc_orphan_resp.ps1 has not run yet." -ForegroundColor Red
        Write-Host "Run it first - this patch edits its output."
        exit 1
    }

    $anchor = "static async_rsp_t async_rsp_table[MAX_ASYNC_RPC_TRANSACTIONS] = { 0 };"
    if (-not $c.Contains($anchor)) { Write-Host "anchor 1 missing" -ForegroundColor Red; exit 1 }
    $c = $c.Replace($anchor, $anchor + "`n" + $HELPERS)

    $o2 = "`tint ret = SUCCESS;`n`n`t/* semaphore init */"
    $p2 = "`tint ret = SUCCESS;`n`n`t/* QMX_RPC_TABLE_LOCK - before any thread is spawned. */`n`tif (!sync_tbl_lock) {`n`t`tsync_tbl_lock = g_h.funcs->_h_create_mutex();`n`t`tif (!sync_tbl_lock) {`n`t`t`tESP_LOGE(TAG, `"sync table mutex init failed, exiting`");`n`t`t`tgoto free_bufs;`n`t`t}`n`t}`n`n`t/* semaphore init */"
    if (-not $c.Contains($o2)) { Write-Host "anchor 2 missing" -ForegroundColor Red; exit 1 }
    $c = $c.Replace($o2, $p2)

    $o3 = "`tuid++;`n`t// handle rollover in uid value`n`tif (!uid)`n`t`tuid++;`n`tapp_req->uid = uid;"
    $p3 = "`t/* QMX_RPC_TABLE_LOCK: read-modify-write reachable from any task. Two`n`t * callers handed the same uid both match the FIRST slot carrying it, so`n`t * one waits on the other's semaphore and the loser's slot is never`n`t * cleared. */`n`tqmx_sync_tbl_lock();`n`tuid++;`n`t// handle rollover in uid value`n`tif (!uid)`n`t`tuid++;`n`tapp_req->uid = uid;`n`tqmx_sync_tbl_unlock();"
    if (-not $c.Contains($o3)) { Write-Host "anchor 3 missing" -ForegroundColor Red; exit 1 }
    $c = $c.Replace($o3, $p3)

    $o4 = "`tfor (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {`n`t`tif (sync_rsp_table[i].uid == app_resp->uid) {`n`t`t`treturn g_h.funcs->_h_post_semaphore(sync_rsp_table[i].sem);`n`t`t}`n`t}`n`n`treturn CALLBACK_NOT_REGISTERED;"
    $p4 = "`t(void)i;`n`treturn qmx_sync_post(app_resp->uid);"
    if (-not $c.Contains($o4)) { Write-Host "anchor 4 missing" -ForegroundColor Red; exit 1 }
    $c = $c.Replace($o4, $p4)

    $re5 = [regex]"(?s)		for \(i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i\+\+\) \{.*?Symc sem not registered: out of buffer space`"\);
		return CALLBACK_NOT_REGISTERED;
"
    if ($re5.Matches($c).Count -ne 1) { Write-Host ("anchor 5 matched " + $re5.Matches($c).Count + " times") -ForegroundColor Red; exit 1 }
    $c = $re5.Replace($c, $CLAIM)

    # Swallows the old loop AND the fallthrough that followed it - after this
    # the function returns above them, and unreachable code in a vendored file
    # is what the next reader trips over.
    $re6 = [regex]"(?s)	for \(i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i\+\+\) \{
		if \(sync_rsp_table\[i\]\.uid == app_req->uid\) \{.*?			return ret;
		\}
	\}
	ESP_LOGW\(TAG, `"Not able to map new request to resp id`"\);
	return MSG_ID_OUT_OF_ORDER;
"
    if ($re6.Matches($c).Count -ne 1) { Write-Host ("anchor 6 matched " + $re6.Matches($c).Count + " times") -ForegroundColor Red; exit 1 }
    $c = $re6.Replace($c, $WAIT)

    $o7 = "`t`t`tif (CALLBACK_AVAILABLE != is_sync_resp_sem_available(app_resp->uid)) {"
    $p7 = "`t`t`tqmx_sync_tbl_lock();`n`t`t`tif (CALLBACK_AVAILABLE != qmx_sync_avail_locked(app_resp->uid)) {`n`t`t`t`tqmx_sync_tbl_unlock();"
    if (-not $c.Contains($o7)) { Write-Host "anchor 7 missing" -ForegroundColor Red; exit 1 }
    $c = $c.Replace($o7, $p7)

    $o7b = "`t`t`tif (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, pdMS_TO_TICKS(1000))) {`n`t`t`t`tESP_LOGE(TAG, `"RPC Q put fail`");`n`t`t`t`tgoto free_buffers;`n`t`t`t}`n`n`t`t`t/* Call up rx ind to unblock caller */`n`t`t`tpost_sync_resp_sem(app_resp);"
    $p7b = "`t`t`tif (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, pdMS_TO_TICKS(1000))) {`n`t`t`t`tqmx_sync_tbl_unlock();`n`t`t`t`tESP_LOGE(TAG, `"RPC Q put fail`");`n`t`t`t`tgoto free_buffers;`n`t`t`t}`n`n`t`t`t/* Call up rx ind to unblock caller */`n`t`t`tqmx_sync_post_locked(app_resp->uid);`n`t`t`tqmx_sync_tbl_unlock();"
    if (-not $c.Contains($o7b)) { Write-Host "anchor 7b missing" -ForegroundColor Red; exit 1 }
    $c = $c.Replace($o7b, $p7b)

    [System.IO.File]::WriteAllText($core, $c, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Patched rpc_core.c: sync RPC table and uid are now locked; each waiter owns its semaphore." -ForegroundColor Green
}

$h = (Get-Content -Raw -Path $hdr) -replace "`r`n", "`n"
if ($h -match "QMX_RPC_TABLE_LOCK") {
    Write-Host "Already patched (rpc_rx_q depth) - nothing to do." -ForegroundColor Green
} else {
    $o8 = "#define RPC_RX_QUEUE_SIZE 3"
    $p8 = "/* QMX_RPC_TABLE_LOCK: was 3, while`n * CONFIG_ESP_HOSTED_MAX_SIMULTANEOUS_SYNC_RPC_REQUESTS is 5 - five callers`n * allowed in flight with room to queue only three replies. The RX thread now`n * holds the table lock across its enqueue, so a full queue would stall every`n * other RPC for the enqueue timeout. Deeper than the slot count, it cannot`n * fill. */`n#define RPC_RX_QUEUE_SIZE 8"
    if (-not $h.Contains($o8)) { Write-Host "RPC_RX_QUEUE_SIZE anchor missing" -ForegroundColor Red; exit 1 }
    $h = $h.Replace($o8, $p8)
    [System.IO.File]::WriteAllText($hdr, $h, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Patched rpc_slave_if.h: rpc_rx_q depth 3 -> 8." -ForegroundColor Green
}
