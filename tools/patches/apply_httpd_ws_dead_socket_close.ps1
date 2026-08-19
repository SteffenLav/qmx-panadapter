# Standing patch #9 - httpd must CLOSE a websocket whose socket is dead, not
# merely mark it and spin on it.
#
# THE BUG, measured in Roy KI0ER's diagnostic log 2026-08-19 (#203/#59):
#
#   19,773 x "httpd_txrx: httpd_sock_err: error in recv : 128" (ENOTCONN) at a
#   steady 158/s, spanning 125.7 seconds. During that window FT8 decoded
#   NOTHING for 117 s, the audio ring overflowed (DROPPED=49936, ring full)
#   while the QMX kept delivering 49k pairs/s, and both occupancy strips went
#   grey. The operator reported it as "the offset strips went grey for no
#   reason".
#
# WHY IT STARVES EVERYTHING: the httpd worker runs at priority 5. fft_task -
#   the audio ring's ONLY consumer - is priority 4, ws_push is 3, the spot and
#   PSK feeds are 2. A worker spinning flat out at 5 denies the core to every
#   one of them, so nothing below it can even run to clean the socket up. In
#   Roy's log the dead fd was finally closed only when a NEW browser connected
#   125 s later and our own takeover logic closed the stale one.
#
# THE CAUSE: httpd_ws_get_frame_type() cannot read the header byte from a
#   half-closed socket, and its own comment says "closing socket now" - but it
#   returns ESP_OK. httpd_req_new() then marks sd->ws_close and returns ESP_OK
#   too, so httpd_sess_process() succeeds and httpd_main.c NEVER reaches its
#   httpd_sess_delete(). The socket stays readable forever, select keeps
#   firing, and the recv is retried every ~6 ms until something external
#   intervenes.
#
# THE FIX: return ESP_FAIL instead. httpd_main.c already does the right thing
#   with it:
#       if (httpd_sess_process(ctx->hd, session) != ESP_OK) {
#           httpd_sess_delete(ctx->hd, session);
#       }
#   so one changed return value turns an endless spin into the close the
#   function's own comment always claimed to be doing.
#
# ⚠ Edits the PINNED IDF INSTALL TREE, so an IDF reinstall wipes it and it must
#   be re-applied per build machine - same model as patches #4, #5, #7 and #8.
#   tools/check_patches.py fails the build without it.

$ErrorActionPreference = "Stop"

# IDF_PATH is only exported after export.ps1 runs, and these scripts are often
# invoked from a bare shell - so fall back to the pinned install rather than
# failing in Join-Path on a null.
$file = $null
if ($env:IDF_PATH) {
    $candidate = Join-Path $env:IDF_PATH "components\esp_http_server\src\httpd_ws.c"
    if (Test-Path $candidate) { $file = $candidate }
}
if (-not $file) {
    $file = "C:\esp\v5.4.4\esp-idf\components\esp_http_server\src\httpd_ws.c"
}
if (-not (Test-Path $file)) { Write-Error "httpd_ws.c not found: $file" }

$marker = "QMX PATCH #9: dead WS socket must close, not spin"
$text   = Get-Content -Raw -LiteralPath $file

if ($text -match [regex]::Escape($marker)) {
    Write-Output "Already patched: httpd_ws.c (dead-WS-socket close)."
    exit 0
}

$old = @"
        ESP_LOGW(TAG, LOG_FMT("Failed to read header byte (socket FD invalid), closing socket now"));
        aux->ws_final = true;
        aux->ws_type = HTTPD_WS_TYPE_CLOSE;
        return ESP_OK;
"@

if ($text -notmatch [regex]::Escape($old)) {
    Write-Error "httpd_ws.c does not contain the expected block - IDF version drift? Patch NOT applied."
}

$new = @"
        ESP_LOGW(TAG, LOG_FMT("Failed to read header byte (socket FD invalid), closing socket now"));
        aux->ws_final = true;
        aux->ws_type = HTTPD_WS_TYPE_CLOSE;
        /* $marker
         * Returning ESP_OK here marked the session closed but never actually
         * closed it: the socket stayed readable, select kept firing, and this
         * recv was retried ~158 times a second indefinitely. Measured at 19,773
         * retries over 125 s, which starved fft_task (priority 4, the audio
         * ring's only consumer) under this worker at priority 5 - the ring
         * overflowed and FT8 decoded nothing for two minutes.
         * ESP_FAIL propagates to httpd_sess_process(), whose caller in
         * httpd_main.c then calls httpd_sess_delete(). */
        return ESP_FAIL;
"@

$text = $text.Replace($old, $new)
Set-Content -LiteralPath $file -Value $text -NoNewline -Encoding UTF8
Write-Output "Patched httpd_ws.c: a dead WS socket now closes instead of spinning."
Write-Output "  $file"
