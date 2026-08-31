#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   // size_t (settings_get_activation_ref)

#ifdef __cplusplus
extern "C" {
#endif

// FT8 CQ-run reply filters: up to two "include" and two "exclude" terms,
// each independently enabled, matched against the *whole* decoded message
// text (so POTA/SOTA tags, country prefixes, grids etc. are all fair game,
// not just the callsign). Plus two standalone toggles.
#define FT8_FILTER_TEXT_LEN 16

// How the robot ranks eligible CQ callers when several pass the filters in the
// same slot. (Stored as uint8_t robot_priority.)
typedef enum {
    FT8_ROBOT_PRI_STRONGEST = 0,  // highest SNR first (best chance of completing)
    FT8_ROBOT_PRI_WEAKEST   = 1,  // lowest SNR first (help the weak ones / ragchew DX hunt)
    FT8_ROBOT_PRI_DISTANT   = 2,  // greatest great-circle distance from our grid (DX)
} ft8_robot_priority_t;

typedef struct {
    bool incl_en[2];
    char incl_text[2][FT8_FILTER_TEXT_LEN];
    bool excl_en[2];
    char excl_text[2][FT8_FILTER_TEXT_LEN];
    bool excl_worked_before; // skip callers already in the ADIF log (enforced since the robot landed)
    bool excl_plain_cq;      // hide bare "CQ ..." rows, show only replies to us
    bool incl_cq_only;       // show ONLY "CQ ..." rows (display filter; does not affect auto-reply)
    // --- Robot (auto-answer) — appended; old NVS blobs read back 0 (=off, STRONGEST) ---
    bool    robot_en;        // auto-answer CQ callers with no tap (default off)
    uint8_t robot_priority;  // ft8_robot_priority_t: which caller to pick first
    // --- Skip TX1 — appended; old NVS blobs read back 0 (=off) ---
    bool    skip_tx1;        // pounce: first TX is a signal report (skip grid exchange),
                              // straight into the roger/RR73 wait - see ft8_qso_start()
    // --- Auto-work pileup — appended; old NVS blobs read back 0 (=off) ---
    bool    auto_pileup;     // on QSO completion, auto-pounce the strongest waiting
                              // pileup caller instead of resuming CQ (unattended TX)
    // --- Manual pick while running CQ — appended; old NVS blobs read back 0 (=off) ---
    // Eric K3FNB: "When I am activating a park, I sometimes like to be a bit more
    // engaged ... could you have an option where I have to tap on a caller/hunter
    // in order to initiate the exchange? I don't mind the firmware automating the
    // rest." So this gates ONLY the decision of WHO to work. Once a caller is
    // tapped the exchange runs itself exactly as before.
    bool    cq_manual_pick;  // running CQ: never auto-answer a caller, wait for a tap
    // --- Max age in the decode list — appended; old NVS blobs read back 0 ---
    // How long a station stays in the LIVE decode list with no fresh decode,
    // in seconds - was the fixed FT8_ROW_STALE_SEC (90) in ft8_screen.c, now
    // operator-tunable (30/45/60/75/90) from the Filter modal. Deliberately
    // NOT applied to the pileup list, which has no expiry of its own by
    // design (ft8_pileup.h) - this only ever shortens/lengthens how long a
    // row survives in the live table. 0 means "never written" (an NVS blob
    // from before this field existed), read as "use the 90 s default", NOT
    // as "expire instantly" - see ft8_screen.c's row_stale_sec().
    uint8_t max_age_sec;
} ft8_filters_t;

// User-defined physical-keyboard shortcuts (#233).
//
// A binding is a modifier plus a key plus an ACTION ID. The id is what is
// stored, never a function pointer or a menu position - so reordering the
// action list in a later firmware cannot silently repoint somebody's Ctrl+M at
// something else. Adding new actions is safe; RENUMBERING existing ones is not.
//
// Ctrl (0x01) and Alt (0x04) are the only modifiers this keyboard exposes -
// measured, see the shortcut block in ui.c. Sym and Aa are consumed by the
// keyboard's own MCU.
#define KBD_BINDINGS_MAX 24
typedef struct {
    uint8_t mods;      // 0x01 Ctrl, 0x04 Alt
    char    key;       // lower-case ASCII
    uint8_t action;    // ui_kbd_action_* id; 0 = unbound/empty slot
} kbd_binding_t;

typedef struct {
    uint8_t       n;                        // how many slots are in use
    kbd_binding_t b[KBD_BINDINGS_MAX];
} kbd_bindings_t;

// All persisted settings. Floats are stored as raw 32-bit bit-patterns
// in NVS (NVS doesn't have a native float type).
typedef struct {
    float db_min;       // spectrum/waterfall floor (dBm)
    float db_max;       // spectrum/waterfall ceiling (dBm)
    float ema_alpha;    // spectrum EMA smoothing (0..1)
    bool  iq_enabled;   // I/Q balance correction on/off
    bool  flat_mode;    // Phase 5.12: flat-spectrum view (per-bin floor)
    char  wifi_ssid[33];   // WiFi SSID (32 chars + NUL, IEEE max)
    char  wifi_pass[65];   // WiFi password (64 chars + NUL, WPA2 max)
    // Static IP (Randy N4OPI). An EMPTY wifi_ip means DHCP, which is the
    // default and the only state an existing unit can be in after an upgrade -
    // so this cannot change anyone's network by appearing. All four are edited
    // as one set and share one dirty bit, the same way the LoTW station fields
    // do. Stored as dotted text so the config export stays readable and a
    // half-typed address is visibly wrong rather than silently a number.
    char  wifi_ip[16];     // e.g. "192.168.1.50"; empty = use DHCP
    char  wifi_mask[16];   // e.g. "255.255.255.0"; empty defaults to /24
    char  wifi_gw[16];     // e.g. "192.168.1.1"
    char  wifi_dns[16];    // e.g. "192.168.1.1"; empty falls back to the gateway
    uint32_t last_vfo_hz; // last QMX VFO frequency in Hz (0 = unknown)
    uint32_t ft8_freq_hz; // last FT8/FT4 preset frequency in Hz (persisted so FT8 mode doesn't inherit the panadapter's VFO; default 14074000)
    uint16_t cw_pitch_hz;  // CW sidetone offset in Hz (default 700)
    uint8_t  colormap_idx; // waterfall colour map: 0=Thermal 1=Viridis 2=Turbo 3=Grayscale
    char     my_callsign[16];  // operator callsign for FT8 (15 chars + NUL)
    char     my_grid[8];       // Maidenhead grid (6 chars + NUL, e.g. "JO45ab")
    int16_t  cw_cal_hz;        // CW LO trim (Hz), default -60, range +/-100
    float    zoom_factor;      // spectrum/waterfall zoom, 1.0=full, max 24.0
    uint8_t  brightness_pct;   // LCD backlight brightness, 0..100, default 100
    /* Last UI mode: 0=Panadapter, 1=FT8, 3=WSPR (default 0). All three are
     * restored at boot as of the 2026-08-28 WSPR launch - where the Tab5 was
     * left is where it wakes up, including after a flash, since a flash writes
     * only the app and leaves NVS alone.
     *
     * WSPR restore is still gated on wspr_feature_enabled(), so disabling the
     * feature cannot leave a unit booting into a page it no longer offers. Until
     * the launch, WSPR fell through to Panadapter on purpose - a mode that
     * shipped dark should not have been sticky across a reboot. */
    uint8_t  last_ui_mode;
    uint32_t last_unix_time;   // last UTC unix time seen from SNTP (0 = never synced)
    char     cq_msg[3][28];    // 3 user-editable CQ message presets (FT8 TX)
    uint8_t  cq_sel;           // which CQ preset is active, 0..2 (default 0)
    uint8_t  cq_max_calls;     // auto-stop CQ after N unanswered calls, 0=never (default 0)
    // FT8 Fox/Hound (DXpedition) mode - the HOUND side only. Fox is impossible on
    // this radio: it needs up to five simultaneous signals and we emit one CAT
    // "TA" tone per symbol. 0 = off, 1 = guided, 2 = automatic. See ft8_hound.h.
    uint8_t  hound_mode;
    // Roy KI0ER: while calling CQ you are deaf to your own time window, so the
    // occupancy picture for the window you transmit in goes stale. After every
    // N calls, spend one slot listening instead. 0 = never (default) - this
    // changes on-air cadence, so it is opt-in.
    uint8_t  cq_listen_every;
    bool     onboarded;        // first-boot WiFi/identity prompts shown (default false)
    bool     wifi_enabled;     // initiate WiFi at boot (default true)
    bool     qmx_gps;         // QMX/QMX+ has GPS discipline — skip Tab5→QMX time push
    bool     qmx_time_pushed; // we have set THIS radio's clock, so its agreement with
                              // ours proves nothing about GPS (see time_sync.c)
    bool     freq_kp_calc;    // freq keypad digit layout: false=phone, true=10-key/calc
    int16_t  freq_kp_dx;      // freq keypad popup position: offset from screen center, px (default 0,0)
    int16_t  freq_kp_dy;
    bool     freq_kp_small;   // freq keypad popup size: false=normal, true=small (pinch-to-resize, default false)
    char     qrz_api_key[40]; // QRZ Logbook API key (GUID-format, set via web UI)
    uint32_t qrz_uploaded_n;  // count of ADIF records already uploaded to QRZ
    char     eqsl_user[16];   // eQSL.cc username (callsign), set via web UI
    char     eqsl_pswd[32];   // eQSL.cc password
    uint32_t eqsl_uploaded_n; // count of ADIF records already uploaded to eQSL
    // Cloudlog / Wavelog (#171). Self-hosted, so the ADDRESS is the operator's -
    // the only upload target here whose host is not hardcoded. May be plain http
    // when it resolves onto our own subnet; see util/net_guard.h for the rule.
    char     cloudlog_url[96];   // base URL, e.g. http://cloudlog.lan or https://log.example.com
    char     cloudlog_key[64];   // Cloudlog API key
    char     cloudlog_station[8];// station_profile_id, as text - it is an opaque id, not arithmetic
    uint32_t cloudlog_uploaded_n;// count of ADIF records already uploaded to Cloudlog
    bool     cw_audio_en;     // CW sidetone audio on Tab5 speaker/headphone (default false)
    uint8_t  cw_audio_vol;    // CW audio output volume 0..100 (default 60)
    float    wf_black_db;     // waterfall black level: dB above floor -> black (default 9)
    float    wf_contrast_db;  // waterfall contrast: dB span filling the colour ramp (default 45)
    uint8_t  wf_floor_blend;  // waterfall per-bin floor blend 0..100% (0=global, default 100)
    uint8_t  wf_window;       // FFT window: 0=Blackman-Harris 1=Hann 2=Nuttall (default 0)
    bool     display_flip;    // landscape flipped 180 deg for upside-down mounting (default false)
    // QMX AF gain in DECIBELS - the same number the radio shows on its own LCD
    // (see cat.h's CAT_AF_GAIN_MAX comment). Stored only as a fallback slider
    // position for when the radio hasn't answered AG; yet; the live value comes
    // from cat_get_af_gain(). Deliberately NOT pushed to the radio at boot, so a
    // saved value can never change the volume behind the operator's back.
    uint8_t  qmx_vol_db;
    // CW transmit offset (Roy KI0ER, 2026-08-07): while in CW, transmit this
    // many Hz away from the station you are listening to, so a QRP call does
    // not land in the mud-pit of everyone else zero-beating the DX. 0 = off.
    // The radio does this with split (RX on VFO A, TX on VFO B); we keep VFO B
    // at A+offset for as long as the feature is on and the mode is CW.
    // Yaesu calls the equivalent "CLAR TX"; the QMX has no XIT of its own.
    int16_t  cw_tx_offset_hz;
    // SWR protection during transmit, in units of 0.1 (25 = 2.5:1). 0 = off.
    // The QMX reports SWR over CAT (SW;) while keyed, so a burst can be cut
    // short and latched off rather than driving a mismatched load for the
    // whole 12.7 s. The QMX's finals are the thing being protected here.
    uint8_t  swr_limit_x10;
    // Propagation feedback: query PSK Reporter for who has heard US. Separate
    // from pskreporter_en (which is about SENDING reports) - they are opposite
    // directions and an operator may reasonably want one without the other.
    // Off by default: it is outbound traffic on a fragile link for a question
    // not everyone is asking.
    bool     psk_rx_en;
    // BLE mouse (bt_hid_mouse.c). Off by default - it starts a second radio
    // subsystem sharing the SDIO link with WiFi, which is this board's most
    // fragile component. Opt-in until a soak says otherwise.
    bool     bt_mouse_en;
    // DX cluster spot feed. Opt-in: it is a SECOND long-lived TCP connection
    // alongside RBN on this board's most fragile link. It is also the only
    // source of PHONE spots - RBN is skimmers, and no SSB skimmer exists.
    bool     cluster_en;
    // Show only spots you can work in the mode you are currently in. ON by
    // default: tapping a spot sets the MODE as well as the frequency, so an
    // unfiltered lane will happily drop a CW operator into FT8 (Michael KZ4LY,
    // 2026-08-10). Turn it off to see the whole band, and the labels then carry
    // a two-letter mode tag so the lane is not ambiguous.
    bool     spots_mode_filter;
    uint8_t  bandplan_region; // band-plan strip region: 0=auto(from grid) 1=R1 2=R2 3=R3
    bool     distance_in_miles; // FT8 decode list: show distance in miles instead of km (default false)
    bool     rit_pill_show;     // show the RIT pill in the panadapter top bar (default TRUE)
    /* #298 STILL DISPLAY. true (the default) = the spectrum and waterfall hold
     * still and the VFO marker moves across them; false = the old behaviour,
     * the view re-centres on the dial on every tune. A real preference, not a
     * dev switch: it changes how the main screen is read. */
    bool     still_view;        // spectrum holds, VFO moves (default TRUE)
    bool     still_notice_done; // the one-time "you can switch back" toast has been shown
    uint8_t  spur_mode;         // 0=off 1=subtract 2=interpolate (see spur_map.h); default 0, opt-in
    bool     ft8_early_decode; // FT8 monitoring: cut capture ~1.8 s early so decodes surface BEFORE the
                               // slot boundary (WSJT-X-style), letting a cold pounce fire in the reply
                               // slot at a decodable DT. Trades some weak/late-station yield (default true)
    bool     ft8_sync_lines;  // Panadapter: FT8-sync-vs-SNTP waterfall slot-boundary lines + 2x waterfall speed (default false)
    bool     greylist_en;     // FT8: grey-list stations after repeated failed pounces - auto pickers skip
                              // them, rows recolour, tap offers clear (default false; RAM-only list)
    bool     spots_en;        // draw live POTA (later RBN) spots on the spectrum (default true)
    bool     rbn_en;          // add the RBN telnet feed as a second spot source (default FALSE:
                              // a continuous firehose on this board's most fragile subsystem,
                              // so it is opt-in - see net/rbn.h)
    // SOTA summit activations, fetched from spothole.app. Opt-in (default
    // FALSE) for a reason that is about somebody else's server rather than
    // ours: spothole is Ian Renton M0TRT's hobby box, shared with his blog, and
    // he granted use with an explicit reliability caveat. Defaulting this ON
    // would put every unit in the field on it forever from the moment they
    // update. Flip the default only once it has proven itself. See net/spots.c.
    bool     sota_en;
    // #239: fetch a newer release in the BACKGROUND, so the operator is only
    // ever asked the one question that matters ("restart into it?") instead of
    // starting a download and then waiting on it. Default ON, but it must stay
    // switchable: a POTA operator on a phone hotspot did not ask us to pull
    // 3.3 MB, and the download saturates this link (~12.7 KB/s) for over a
    // minute. Downloading is safe to automate; APPLYING is not, and is not -
    // see the standing rule at the top of net/ota_update.h.
    bool     ota_autodl;
    // Which half of the settings drawer is shown. Persisted because the
    // operator who wants Expert wants it every time: Samuel W7STF,
    // 2026-08-26 - "I just find myself leveraging the Expert version of
    // the menu at this time and selecting it seems to nuisance upon each
    // boot-up."
    bool     drawer_expert;
    bool     pskreporter_en;  // FT8/FT4: report real decodes to pskreporter.info (UDP, batched ~5 min;
                              // needs callsign+grid; never in simulation mode; default TRUE - same
                              // as WSJT-X ships; drawer checkbox turns it off)
    uint16_t tx_tone_hz;      // FT8/FT4 TX audio tone preference in Hz (default 1500). What the pane's
                              // "TX <n> Hz" button shows while idle; with tx_tone_hold it IS the tone used
    bool     tx_tone_hold;    // FT8/FT4: keep tx_tone_hz for every CQ/reply instead of auto-picking a
                              // clear slot - WSJT-X's "Hold Tx Freq" (default false)
    ft8_filters_t ft8_filters;        // CQ-run reply include/exclude filters
    kbd_bindings_t kbd_bindings;      // physical-keyboard shortcuts (#233)
    bool     field_day_en;    // ARRL Field Day exchange mode: TX/RX class+section instead of grid/report (default false)
    // Activation session: what WE are activating right now, if anything. Every
    // QSO logged while this is set carries MY_SIG/MY_SIG_INFO, which is what
    // POTA and SOTA read to credit an activation. Persisted deliberately - an
    // activation outlives a battery change in the field, and re-typing the
    // reference after a reboot is exactly when it gets typed wrong.
    // 0 = none, 1 = POTA, 2 = SOTA. act_ref is the park/summit reference.
    //
    // Deliberately NOT in config_io_export() (and so not in s_config_export_bits):
    // a config backup is restored weeks later on a different day, and restoring
    // "activating DL-0123" would silently stamp every QSO with a park the
    // operator is nowhere near. It persists in NVS so it survives a battery
    // change DURING an activation, which is the case that matters.
    uint8_t  act_type;
    char     act_ref[16];     // "DL-0123" (POTA) or "OZ/SJ-001" (SOTA)
    char     fd_class[4];     // Field Day class, e.g. "16A" (1-2 digit transmitter count + category letter)
    char     fd_section[4];   // Field Day ARRL/RAC section abbreviation, e.g. "EMA"
    bool     sim_mode_en;     // FT8 simulation mode: phantom stations, real radio never keyed (default false)

    /* ---- WSPR ---------------------------------------------------------
     * wspr_dial_hz is one of the standard per-band WSPR dial frequencies -
     * NOT free entry. A station outside the 200 Hz sub-band is heard by
     * nobody, so an arbitrary number is only a way to be silently wrong
     * (docs/wspr-ui-design.md).
     * wspr_tx_dbm is a DECLARED power, published worldwide with every spot.
     * It was a hardcoded 23 before this existed, so every spot claimed
     * 0.2 W whatever the operator was actually running. */
    uint32_t wspr_dial_hz;    // standard WSPR dial for the chosen band (default 20 m)
    bool     wspr_tx_en;      // WSPR transmit enabled at all (default OFF)
    uint8_t  wspr_duty_pct;   // fraction of cycles to transmit: 0/10/20/33/50
    int8_t   wspr_tx_dbm;     // declared TX power, dBm (default 23 = 200 mW)
    /* Captured windows still to be written to the SD card as WAV.
     *
     * ⛔ PERSISTED, and that is the whole point. The SD card cannot be written
     * while WiFi is up on this board - it unmounts within ~100 s as the
     * MALLOC_CAP_DMA pool collapses, and sd_archive.c's own comment records
     * that a remount then cannot succeed and that "a reboot with WiFi off
     * gives the verified continuous-mirroring behaviour". So the request has
     * to survive the reboot that makes it possible: arm it from the web while
     * WiFi is still up, switch WiFi off, reboot, and the dumps land.
     *
     * Decremented as each file is written, so an interrupted run resumes
     * rather than starting over, and a finished run cannot re-arm itself on
     * the next boot and quietly fill the card. */
    /* ---- BAND HOPPING ------------------------------------------------
     * A BITMASK over the standard WSPR band table (ui/wspr_screen_view.c's
     * kBands), one bit per band the operator ticked. A mask rather than a list
     * because the order is the table's, and because "which bands" is exactly a
     * set - the operator ticks them off and the loop visits whichever are on.
     *
     * ⚠ The mask can name a band the RADIO does not have. That is deliberate:
     * the tick list only ever OFFERS bands the QMX reports (cat_get_band_list),
     * but a stored mask outlives a change of radio, and silently dropping bits
     * would lose the operator's choice the moment CAT was slow to answer. The
     * hop skips what the radio cannot reach; it does not forget it.
     *
     * 0 bands ticked, or hopping off, means stay on wspr_dial_hz. */
    uint16_t wspr_hop_mask;
    bool     wspr_hop_en;

    /* ⭐ THE MASTER SWITCH FOR THE WHOLE WSPR PAGE, DEFAULT OFF.
     *
     * WSPR ships on the main track before it is finished, so that the update
     * path carries it and can be exercised, while nobody meets it by accident:
     * off, the page is not in the swipe cycle, /api/wspr says so rather than
     * answering, and the RX loop cannot be started.
     *
     * ⚠ WHAT THIS DOES NOT BUY. Being off does NOT make the feature free -
     * .bss is allocated whether code runs or not, and WSPR's share of internal
     * RAM had to be dealt with separately (see the EXT_RAM_BSS_ATTR note in
     * wspr_rx.c). Never reason from "the flag is off" to "it costs nothing".
     *
     * Deliberately has no drawer control: a half-finished mode should be
     * reachable by someone who went looking for it, not offered in a list of
     * settings. Turned on with /api/cmd {"action":"wspr_enable","on":true},
     * or wspr_enabled = yes in an imported config file. */
    bool     wspr_en;

    /* ⛔ PUBLISHES TO A PUBLIC DATABASE UNDER THE OPERATOR'S CALLSIGN, so it
     * starts OFF and only the operator turns it on. wsprnet is a scientific
     * dataset other people draw propagation conclusions from - an upload is
     * not a private action, and it cannot be taken back once indexed. */
    bool     wspr_net_en;

    /* WSPR PA-voltage guard (#290). WSPR keys the PA for ~110 s out of every
     * 120 - a duty cycle nothing else this radio does approaches - and the QMX
     * finals overheat at full power on it. The radio's OWN Virtual U3S WSPR
     * halves the PA voltage for exactly this reason; our WSPR TX is CAT-driven
     * and never enters that mode, so it inherits none of that protection.
     *
     * wspr_pa_reduce ON (the default) means: when WSPR transmit is enabled,
     * read the radio's Max. PA voltage, remember it here, and set it to half.
     * Restore it when transmit is turned off.
     *
     * wspr_pa_saved_x10 is the value to restore, in tenths of a volt, 0 when
     * nothing is outstanding. It is PERSISTED deliberately: if the Tab5 dies
     * mid-session the radio is left turned down, and only this tells the next
     * boot what to put back. Restoring a guess would be worse than not
     * restoring - an operator who runs a reduced PA must not be turned UP. */
    bool     wspr_pa_reduce;
    uint16_t wspr_pa_saved_x10;

    uint8_t  wspr_dump_cycles;
    uint8_t  ft8_op_mode;     // FT8/FT4 sub-mode (ft8_op_mode_t: 0=FT8 1=FT4), default 0 - see ft8_test.h
    uint32_t passband_width_hz; // last CAT-reported filter width (Hz), 0=unknown/use mode default. Persisted so the
                                 // band-plan strip's passband indicator shows the real width immediately at boot
                                 // instead of a generic default for the few seconds before the first real FW poll lands.
    bool     charge_limit_en;   // battery care: stop charging at charge_limit_pct (default false)
    uint8_t  charge_limit_pct;  // stop-charging threshold, 50..100 (default 80)
    uint8_t  display_sleep_min; // idle minutes before the backlight sleeps, 0 = never (default 0)
    bool     resmon_en;         // resource-monitor floating overlay shown (default false)
    int16_t  resmon_dx;         // its position: offset from screen top-left, px (default 0,0)
    int16_t  resmon_dy;
    // LoTW station-location fields (the cert + private key themselves live on
    // SPIFFS via lotw_upload.c, not in NVS - they're multi-KB blobs).
    char     lotw_dxcc[8];      // DXCC entity NUMBER as digits, e.g. "221" (required for upload)
    char     lotw_cqz[4];       // CQ zone, e.g. "14" (optional, part of the QSO signature when set)
    char     lotw_ituz[4];      // ITU zone, e.g. "18" (optional, ditto)
    // US station subdivision. Without these a US operator's uploads carry no
    // state/county, so their QSOs earn no WAS or county award credit - for them
    // OR for the stations they work. Both are part of the QSO signature when
    // set. lotw_county is the county NAME ALONE ("Arlington"), never the ADIF
    // "ST,County" form, which LoTW rejects outright.
    char     lotw_state[4];     // 2-letter US state, e.g. "VA"
    char     lotw_county[40];   // county name only, e.g. "Arlington"
    uint32_t lotw_uploaded_n;   // count of ADIF records already uploaded to LoTW
} qmx_settings_t;

// Initialise the settings module. Opens an NVS handle. Safe to call
// even if NVS init failed in main; in that case all setters are no-ops
// and load_all returns defaults.
void settings_init(void);

// Populate *out with stored values, or defaults for fields not yet
// written. Always succeeds (falls back to defaults silently).
void settings_load_all(qmx_settings_t *out);

// Per-field setters. Each schedules a debounced flush to NVS — fast
// repeated calls (e.g. a slider drag) only result in one flash write
// after the user pauses.
void settings_set_db_min(float v);
void settings_set_db_max(float v);
void settings_set_ema_alpha(float v);
void settings_set_iq_enabled(bool v);
void settings_set_flat_mode(bool v);
// WiFi credential setters. Pass NULL or empty string to clear.
void settings_set_wifi_ssid(const char *ssid);
void settings_set_wifi_pass(const char *pass);

// Save last-known VFO frequency (debounced flush; same wear profile as the sliders).
void settings_set_last_vfo(uint32_t hz);
// Save last FT8/FT4 preset frequency (debounced flush).
void settings_set_ft8_freq_hz(uint32_t hz);
// Save CW sidetone pitch in Hz (debounced flush).
void settings_set_cw_pitch_hz(uint16_t hz);
// Save waterfall colour-map index (debounced flush).
void settings_set_colormap_idx(uint8_t idx);

// Operator identity. Pass NULL or empty to clear. Used by the FT8
// transmitter (v0.11+) and shown in the FT8 view info pane.
void settings_set_my_callsign(const char *call);
void settings_set_my_grid(const char *grid);

// FT8 CQ message presets. idx 0..2. Pass NULL/empty to clear a slot.
// settings_set_cq_sel selects the active preset (0..2). Debounced flush.
void settings_set_cq_msg(uint8_t idx, const char *text);
void settings_set_cq_sel(uint8_t idx);

// Auto-stop a CQ run after this many unanswered calls (0 = keep calling).
// Don WB0LQW: "I usually send CQ 2-4 times and then pause". Set from the CQ
// preset modal's top-right cycle button; consumed by ft8_qso.c's CQ loop.
void settings_set_cq_max_calls(uint8_t n);
void settings_set_hound_mode(uint8_t m);    // 0 off, 1 guided, 2 automatic
/* Spend one slot listening after every N CQ calls. 0 = never. */
void settings_set_cq_listen_every(uint8_t n);


// First-boot onboarding done: once true, the WiFi/identity prompts are never
// shown again (debounced flush).
void settings_set_onboarded(bool v);

// FT8 CQ-run reply include/exclude filters (debounced flush).
void settings_set_ft8_filters(const ft8_filters_t *f);
void settings_set_kbd_bindings(const kbd_bindings_t *b);

// CW audio output: enable the on-device CW sidetone (speaker/headphone), and
// its volume 0..100 (debounced flush).
void settings_set_cw_audio_en(bool v);
void settings_set_cw_audio_vol(uint8_t v);

// Waterfall colorisation (debounced flush). Black level and contrast are in
// dB; floor blend is 0..100 (% per-bin vs global floor); window is the FFT
// analysis window index (0=Blackman-Harris 1=Hann 2=Nuttall).
void settings_set_wf_black_db(float db);
void settings_set_wf_contrast_db(float db);
void settings_set_wf_floor_blend(uint8_t pct);
void settings_set_wf_window(uint8_t idx);

// Display 180-degree flip for upside-down mounting (debounced flush).
void settings_set_display_flip(bool v);
void settings_set_qmx_vol_db(uint8_t db);
/* CW transmit offset in Hz, -1000..+1000, 0 = off. Applied only in CW mode. */
void settings_set_cw_tx_offset_hz(int16_t hz);
/* Scalar read of the same value. Exists so cat.c's poll task can check it every
 * 50 ms WITHOUT putting a ~500-byte qmx_settings_t on its 4 KB stack - the same
 * reason settings_wifi_known_count() exists (see CLAUDE.md, "Task stacks on
 * this board are TINY"). */
int16_t settings_get_cw_tx_offset_hz(void);
/* Static-IP fields ONLY, 64 bytes of caller-supplied buffers. Any argument may
 * be NULL. Empty ip means DHCP.
 *
 * ⛔ Use this, NEVER settings_load_all(), from a WiFi/IP event handler: those
 * run on `sys_evt`, whose stack is 2808 bytes, and a qmx_settings_t there is a
 * Stack protection fault. See the note on the implementation - it has already
 * boot-looped this device once. */
void settings_get_wifi_static(char ip[16], char mask[16], char gw[16], char dns[16]);
void    settings_set_psk_rx_en(bool v);          // propagation feedback (who is hearing me)
void    settings_set_bt_mouse_en(bool v);        // BLE mouse (scan/pair)
void    settings_set_cluster_en(bool v);         // DX cluster spot feed (phone spots)
void    settings_set_spots_mode_filter(bool v);  // show only spots for the current mode
void    settings_set_swr_limit_x10(uint8_t v);   // 0 = off, else limit x10 (25 = 2.5:1)
uint8_t settings_get_swr_limit_x10(void);

// FT8 distance display unit (debounced flush). When false show distance in km,
// when true show distance in miles.
void settings_set_distance_in_miles(bool v);
void settings_set_rit_pill_show(bool v);
void settings_set_still_view(bool v);
void settings_set_still_notice_done(bool v);
void settings_set_spur_mode(uint8_t v);

// FT8 early-decode / fast-pounce timing (debounced flush). When true, plain
// monitoring cuts capture ~1.8 s early so decodes land before the slot
// boundary and a hand-tapped reply can fire in its own slot at a decodable DT.
void settings_set_ft8_early_decode(bool v);
void settings_set_greylist_en(bool v);
void settings_set_pskreporter_en(bool v);
void settings_set_spots_en(bool v);
void settings_set_rbn_en(bool v);
void settings_set_sota_en(bool v);   // SOTA activations via spothole.app (opt-in)
void settings_set_ota_autodl(bool v); // #239: download a new release quietly (never applies it)
void settings_set_drawer_expert(bool v); // remember Basic vs Expert across a reboot

// ---- Known WiFi networks --------------------------------------------------
//
// A small most-recently-used list of networks that have actually worked, so the
// radio can find its way onto whichever one is present without being told again
// (Roy KI0ER's request: "remember a few SSID setups ... auto connect if that
// SSID is present"). The list is built implicitly - every successful connection
// promotes its network to the front - so there is nothing to manage by hand.
//
// Deliberately NOT part of qmx_settings_t: settings_load_all() copies that whole
// struct, and it is called from hot paths (the 1 Hz spots repaint, the RBN recv
// loop). Adding ~600 bytes to every one of those copies to hold data only the
// WiFi layer reads would be a poor trade, so these get their own accessors.
#define WIFI_KNOWN_MAX 6

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_known_t;

// How many networks are remembered. Exists so callers that only need the count
// do not have to provide a WIFI_KNOWN_MAX buffer - the WiFi event handlers run on
// the system event task, whose stack is under 3 KB.
int  settings_wifi_known_count(void);

// Copy up to max entries, most-recently-used first. Returns the count written.
int  settings_wifi_known_get(wifi_known_t *out, int max);

// Record a network that just worked, moving it to the front. Updates the stored
// password if it changed. Drops the least-recently-used entry when full.
void settings_wifi_known_remember(const char *ssid, const char *pass);

// Replace the whole list, in the given order (index 0 = most recent). This is
// what a config restore wants: remember() promotes to the front, so replaying an
// exported list through it would come back reversed.
void settings_wifi_known_set_all(const wifi_known_t *list, int n);

// Forget one network (config import uses this to honour a deleted line), or all.
void settings_wifi_known_forget(const char *ssid);
void settings_wifi_known_clear(void);

// FT8/FT4 TX tone preference and hold (debounced flush) - see ft8_tx.h for what
// "hold" means to the TX paths. Both are written by the TX tone picker's Apply.
void settings_set_tx_tone_hz(uint16_t v);
void settings_set_tx_tone_hold(bool v);

// Band-plan strip region (debounced flush): 0=auto (derive from grid), 1=R1,
// 2=R2, 3=R3.
void settings_set_bandplan_region(uint8_t v);

// ARRL Field Day exchange mode (debounced flush). When enabled, FT8 QSOs
// exchange class+section (fd_class/fd_section) instead of grid/signal report.
void settings_set_field_day_en(bool v);
// Activation session (POTA/SOTA). type: 0 none, 1 POTA, 2 SOTA. Setting a type
// with an empty reference is treated as "none" - an activation with no
// reference credits nothing and would only put a meaningless MY_SIG in the log.
void settings_set_activation(uint8_t type, const char *ref);
uint8_t settings_get_activation_type(void);
// Copies the reference into out (always NUL-terminated). Returns false when no
// activation is running, in which case out is set empty.
bool settings_get_activation_ref(char *out, size_t out_sz);
// "POTA" / "SOTA" / NULL. The exact strings ADIF's MY_SIG expects.
const char *settings_activation_sig_name(void);

void settings_set_fd_class(const char *cls);
void settings_set_fd_section(const char *section);

// FT8 simulation mode (debounced flush): phantom-station practice mode -
// see ft8_sim.h. The real QMX is never keyed while this is on.
void settings_set_sim_mode_en(bool v);
void settings_set_wspr_dial_hz(uint32_t v);
void settings_set_wspr_tx_en(bool v);
void settings_set_wspr_duty_pct(uint8_t v);
void settings_set_wspr_tx_dbm(int8_t v);
void settings_set_wspr_pa_reduce(bool v);       // #290 halve PA voltage while WSPR TX is on
void settings_set_wspr_pa_saved_x10(uint16_t v);// value to restore, tenths of a volt, 0 = none
void settings_set_wspr_hop_mask(uint16_t v);
void settings_set_wspr_hop_en(bool v);
void settings_set_wspr_en(bool v);   // master switch for the WSPR page - default OFF
void settings_set_wspr_net_en(bool v);   // publish spots to wsprnet - default OFF
void settings_set_wspr_dump_cycles(uint8_t v);
void settings_set_ft8_op_mode(uint8_t v);

// Last CAT-reported filter width in Hz (debounced flush). Restored at boot
// so the band-plan strip's passband indicator is correct immediately
// instead of showing a generic per-mode default for the first few seconds
// after QMX link-up, until the real FW poll response arrives.
void settings_set_passband_width_hz(uint32_t hz);

// WiFi boot-initiation toggle (debounced flush). When false the radio stays
// idle at boot even if credentials are stored; the user can re-enable from
// the settings drawer.
void settings_set_wifi_enabled(bool v);

// QMX/QMX+ GPS discipline flag (debounced flush). When true the Tab5 will
// NOT push its clock to the QMX after a sync, because the GPS is more
// accurate than anything the Tab5 has (SNTP, FT8, manual).
void settings_set_qmx_gps(bool v);

// Sticky record that we have push-set the connected radio's real-time clock.
// Persisted because the state it describes lives in the RADIO, which outlives a
// Tab5 reboot: a clock we set agrees with ours, so it must never be accepted as
// evidence of GPS discipline. Cleared when the radio's clock is plainly its own
// again (a power cycle drops a QMX's software RTC back to 00:00).
void settings_set_qmx_time_pushed(bool v);

// Freq keypad digit layout: false=phone (1 2 3 top), true=10-key/calculator
// (7 8 9 top). Persisted (debounced flush) so it survives a reboot.
void settings_set_freq_kp_calc(bool v);

// Freq keypad popup position: offset (dx, dy) in pixels from screen center,
// where it last sat after being dragged. Debounced flush (a drag-release
// writes once, not per-pixel during the drag). Deliberately NOT part of
// DIRTY_CONFIG_EXPORT_MASK — purely cosmetic placement, not worth a SD-card
// mirror write.
void settings_set_freq_kp_pos(int16_t dx, int16_t dy);

// Freq keypad popup size (debounced flush): false=normal, true=small
// (toggled by pinch). Persists across power cycles like the position above,
// also deliberately NOT part of DIRTY_CONFIG_EXPORT_MASK for the same reason.
void settings_set_freq_kp_small(bool v);

// QRZ Logbook API key, set via the web UI (debounced flush). Pass NULL or
// empty to clear.
void settings_set_qrz_api_key(const char *key);

// Count of ADIF records already uploaded to QRZ (offset into the log file
// for the next upload batch). Debounced flush.
void settings_set_qrz_uploaded_n(uint32_t n);

// eQSL.cc credentials, set via the web UI (debounced flush). Pass NULL or
// empty to clear. eQSL has no API-key scheme - username+password is the only
// auth eQSL's real-time interface supports.
void settings_set_eqsl_user(const char *user);
void settings_set_eqsl_pswd(const char *pswd);

// Count of ADIF records already uploaded to eQSL (offset into the log file
// for the next upload batch). Debounced flush.
void settings_set_eqsl_uploaded_n(uint32_t n);

// Cloudlog / Wavelog (#171). The URL is the operator's own server.
void settings_set_cloudlog_url(const char *url);
void settings_set_cloudlog_key(const char *key);
void settings_set_cloudlog_station(const char *station_id);
void settings_set_cloudlog_uploaded_n(uint32_t n);

// LoTW station-location fields, set via the web UI's cert-import dialog
// (debounced flush). DXCC is the entity number as a digit string; zones are
// optional and become part of every QSO's signature when set - changing them
// only affects QSOs signed after the change.
void settings_set_lotw_dxcc(const char *dxcc);
void settings_set_lotw_cqz(const char *cqz);
void settings_set_lotw_ituz(const char *ituz);
// US subdivision. NOTE: these share DIRTY_LOTW_DXCC's dirty bit - the 64-bit
// dirty bitmap in settings.c is FULL (bits 0-63 all allocated), and all three
// LoTW location fields are only ever written together (the /api/lotw_cert
// handler and config import), so one bit covers them coherently.
void settings_set_lotw_state(const char *state);
void settings_set_lotw_county(const char *county);

// Static IP. Pass NULL or "" for ip to go back to DHCP. Takes effect on the
// next connect - see wifi.c for why it is applied at STA_CONNECTED and not
// at boot.
void settings_set_wifi_static(const char *ip, const char *mask,
                              const char *gw, const char *dns);

// Count of ADIF records already uploaded to LoTW (offset into the log file
// for the next upload batch). Debounced flush.
void settings_set_lotw_uploaded_n(uint32_t n);

// QMX IF offset calibration trim (Hz). Per-unit oscillator variance shifts
// the +12 kHz IF injection; this trim corrects what users see on the spectrum/
// waterfall. Clamped to +/-200 Hz, persisted to NVS (debounced flush).
void settings_set_cw_cal_hz(int16_t hz);
void settings_set_zoom_factor(float v);

// LCD backlight brightness, 0..100 (debounced flush).
void settings_set_brightness_pct(uint8_t pct);

// Last UI mode: 0=Panadapter, 1=FT8 (debounced flush).
void settings_set_last_ui_mode(uint8_t mode);

// Save last-known UTC unix time from SNTP (debounced flush). Used as a
// "last known date" anchor so the QMX RTC time-of-day (no date) can be
// turned into a full timestamp when SNTP is unavailable (e.g. POTA).
void settings_set_last_unix_time(uint32_t unix_sec);

// Battery care (debounced flush): when enabled, charging is cut once the
// battery reaches charge_limit_pct and resumed with a hysteresis gap below
// it, to avoid holding the pack at 100% indefinitely (long-term battery
// wear). charge_limit_pct is clamped to 50..100.
void settings_set_charge_limit_en(bool v);
void settings_set_charge_limit_pct(uint8_t pct);

// Display sleep: minutes of touch inactivity before the backlight turns off
// (0 = never). Any touch wakes it; a two-finger double-tap sleeps immediately.
void settings_set_display_sleep_min(uint8_t minutes);

// Resource-monitor floating overlay: shown/hidden, and its dragged position
// (offset in pixels from the screen's top-left, debounced flush). Position
// is deliberately NOT part of DIRTY_CONFIG_EXPORT_MASK - purely cosmetic
// placement, same as the freq keypad's position.
void settings_set_resmon_en(bool v);
void settings_set_resmon_pos(int16_t dx, int16_t dy);

// Force any pending writes to flash immediately. Call before reboot
// if you want absolute certainty. Normally not needed.
void settings_flush(void);

#ifdef __cplusplus
}
#endif