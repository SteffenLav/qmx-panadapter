#!/usr/bin/env python3
"""T187: is the FIRST KY after a QMX power cycle keyed at once, or held?

⛔ THIS KEYS THE RADIO. Run it only with the operator's go-ahead, on a clear
frequency, straight after a QMX power cycle and before any other CW send.

Sends one text through the page's own path (cw_send; the Tab5 must be on the CW
page) and prints every change of cw_tx for --secs. The radio's own account is in
the serial capture: `cat: CW send -> KY ...`, `RX<- KY?;` and the `RX<- TB...;`
lines, which cat.c prints only when the reply CHANGES.

    python tools/cw_first_ky_test.py --text "VVV VVV DE OZ1LAV OZ1LAV"
"""
import argparse, json, time, urllib.request


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.210")
    ap.add_argument("--text", required=True)
    ap.add_argument("--secs", type=float, default=45)
    a = ap.parse_args()

    def st():
        return json.loads(urllib.request.urlopen(f"http://{a.host}/api/status", timeout=4).read())

    s = st()
    c = s["cw_tx"]
    print(f"before: fw {s.get('tab5_fw')} dial {s['freq_hz']} mode {s['mode']} screen {s['screen']} "
          f"keying {c['keying']} queued {c['queued']} last_sent {c['last_sent']!r}")
    if s["screen"] != "cw" or not s["freq_hz"]:
        print("not on the CW page, or the dial is unknown - nothing sent"); return 2
    if c["last_sent"]:
        print("a CW text was already sent since boot - this would not be the first KY; nothing sent"); return 2
    req = urllib.request.Request(f"http://{a.host}/api/cmd",
                                 data=json.dumps({"action": "cw_send", "text": a.text}).encode(),
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    print("send:", urllib.request.urlopen(req, timeout=5).read().decode()[:80])
    last, fails = None, 0
    while time.time() - t0 < a.secs:
        try:
            c = st()["cw_tx"]; cur = (c["keying"], c["queued"], c["last_sent"])
            if cur != last:
                print("%5.1f s  keying=%s queued=%s last_sent=%r" % (time.time() - t0, *cur)); last = cur
        except Exception as e:
            fails += 1
            print("%5.1f s  status failed: %s" % (time.time() - t0, type(e).__name__))
            if fails >= 5: print("the Tab5 stopped answering"); return 1
        time.sleep(1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
