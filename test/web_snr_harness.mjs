// T184 - host harness for the browser's spectral noise reduction and auto-notch.
//
//   node test/web_snr_harness.mjs            # the gate, synthetic only
//   node test/web_snr_harness.mjs --tune     # also prints the CW table per level
//
// It runs the page's OWN code: the block between NR-BEGIN and NR-END in
// main/net/www/index.html is extracted and evaluated. A copy would test the copy.
//
// Checks (spectral NR):
//   1. floor = 1 is the input delayed by exactly SN_LATENCY_12K
//   2. latency of the processed path, by cross-correlation, is the same number
//   3. stationary noise comes down to the level's floor: 6 / 12 / 20 dB
//   4. a keyed CW note at 12 and 35 WPM gains SNR: >= 6 dB at Med when it is
//      clear, >= 1 dB when it is weak (-10 dB in 2.4 kHz)
//   5. a voiced test signal keeps its level at High
//   6. rx_audio.c gap-frame holes do not defeat it; a squelch stays silent
//   7. a reset instance equals a new one
//   8. a carrier that never keys is taken down (documented behaviour)
// Checks (auto-notch):
//   9. a steady carrier in noise is taken down >= 30 dB once found (0.85 s)
//  10. noise alone passes BIT-EXACT; a voiced signal alone within 1 dB
//  11. it lets go: 1 s after the carrier stops the output is the input, bit-exact
//  12. notch ahead of the NR does not hide the holes from the concealment
// MUTATIONS: gain forced to 1, notch frozen, concealment off - each must FAIL.
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const FS = 12000;
const html = readFileSync(join(ROOT, "main/net/www/index.html"), "utf8");
const a = html.indexOf("/* NR-BEGIN"), b = html.indexOf("/* NR-END");
if (a < 0 || b < 0) { console.error("NR-BEGIN/NR-END not found in index.html"); process.exit(1); }
const NR = new Function(html.slice(a, b) +
  "\nreturn { NR_LEVELS, SN_LATENCY_12K, snCreate, snReset, snProcess, anCreate, anReset, anProcess };")();

let fails = 0, quiet = false;
const check = (ok, msg) => { if (!quiet) console.log(`  ${ok ? "ok  " : "FAIL"} ${msg}`); if (!ok) fails++; return ok; };
const log = (s) => { if (!quiet) console.log(s); };

// ---- deterministic signals ----------------------------------------------------
function rng(seed) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }
function gauss(r) { return Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r()); }
function rms(x, from = 0, to = x.length) { let s = 0; for (let i = from; i < to; i++) s += x[i] * x[i]; return Math.sqrt(s / Math.max(1, to - from)); }
const db = (v) => 20 * Math.log10(v + 1e-12);
function scaleTo(x, rmsDb) { const g = 10 ** (rmsDb / 20) / rms(x); for (let i = 0; i < x.length; i++) x[i] *= g; return x; }
function add(...xs) { const z = new Float32Array(xs[0].length); for (const x of xs) for (let i = 0; i < z.length; i++) z[i] += x[i]; return z; }
// Band noise like the captures: white through a 2-pole band-pass, 300-2700 Hz.
function ssbNoise(n, rmsDb, seed) {
  const r = rng(seed), y = new Float32Array(n);
  const R = Math.exp(-Math.PI * 2400 / FS), th = 2 * Math.PI * 1500 / FS;
  const a1 = 2 * R * Math.cos(th), a2 = -R * R; let y1 = 0, y2 = 0;
  for (let i = 0; i < n; i++) { const v = gauss(r) + a1 * y1 + a2 * y2; y2 = y1; y1 = v; y[i] = v; }
  return scaleTo(y, rmsDb);
}
function voiced(n, rmsDb) {
  const y = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / FS; let v = 0;
    // pitch glides 110-140 Hz: a real voice never holds one pitch for a second
    const ph = 2 * Math.PI * (125 * t - 15 / (2 * Math.PI * 0.9) * Math.cos(2 * Math.PI * 0.9 * t));
    for (let k = 1; k * 125 < 3000; k++) {
      const f = k * 125;
      v += (1 / (1 + ((f - 700) / 150) ** 2) + 0.6 / (1 + ((f - 1200) / 200) ** 2)) * Math.sin(k * ph);
    }
    y[i] = (t % 0.35) < 0.25 ? v * Math.sin(Math.PI * ((t % 0.35) / 0.25)) : 0;
  }
  return scaleTo(y, rmsDb);
}
// Keyed CW: "PARIS " repeated, 5 ms raised-cosine edges. peak = key-down amplitude.
function cw(n, wpm, hz, peak) {
  const dit = Math.round(1.2 / wpm * FS), edge = Math.round(0.005 * FS);
  const M = { P: ".--.", A: ".-", R: ".-.", I: "..", S: "..." };
  const env = new Float32Array(n); let t = Math.round(0.3 * FS);
  outer: for (;;) for (const ch of "PARIS ") {
    if (ch === " ") { t += 4 * dit; continue; }
    for (const el of M[ch]) {
      const len = (el === "." ? 1 : 3) * dit;
      if (t + len >= n) break outer;
      for (let i = 0; i < len; i++) {
        const r = Math.min(1, i / edge, (len - i) / edge);
        env[t + i] = 0.5 - 0.5 * Math.cos(Math.PI * r);
      }
      t += len + dit;
    }
    t += 2 * dit;
  }
  const y = new Float32Array(n);
  for (let i = 0; i < n; i++) y[i] = peak * env[i] * Math.sin(2 * Math.PI * hz * i / FS);
  return { y, env };
}
function withHoles(x, perSec, seed) {
  const r = rng(seed), y = x.slice(); let t = 6000;
  while (t + 400 < y.length) {
    for (let k = 0; k < 120; k++) y[t + k] *= 1 - k / 120;
    y.fill(0, t + 120, t + 256);
    for (let k = 0; k < 120; k++) y[t + 256 + k] *= k / 120;
    t += Math.round(FS / perSec * (0.5 + r()));
  }
  return y;
}
function bestLag(x, y, lo, hi) {
  let best = -1, bestC = -Infinity;
  for (let L = lo; L <= hi; L++) {
    let c = 0, ex = 0, ey = 0;
    for (let i = 0; i + L < y.length; i++) { c += x[i] * y[i + L]; ex += x[i] * x[i]; ey += y[i + L] * y[i + L]; }
    c /= Math.sqrt(ex * ey) + 1e-12;
    if (c > bestC) { bestC = c; best = L; }
  }
  return { lag: best, corr: bestC };
}
// Project y (delayed by L) on the clean signal: gain of the clean part, and SDR.
function proj(clean, y, L, from) {
  let cc = 0, cy = 0; const n = clean.length - L;
  for (let i = from; i < n; i++) { cc += clean[i] * clean[i]; cy += clean[i] * y[i + L]; }
  const g = cy / cc; let e = 0, s = 0;
  for (let i = from; i < n; i++) { const t = g * clean[i]; s += t * t; e += (y[i + L] - t) ** 2; }
  return { sdr: 10 * Math.log10(s / (e + 1e-30)), gainDb: db(Math.abs(g)) };
}
// Amplitude of a tone at hz in x[from..to), by projection on sin and cos.
function toneAmp(x, hz, from, to) {
  let s = 0, c = 0; for (let i = from; i < to; i++) { const p = 2 * Math.PI * hz * i / FS; s += x[i] * Math.sin(p); c += x[i] * Math.cos(p); }
  return 2 * Math.hypot(s, c) / (to - from);
}

const L = NR.SN_LATENCY_12K;
const FLOORS = NR.NR_LEVELS.map(l => l.floor);
let mutate = null;            // (sn, an) => void, applied to every new state
function runSn(x, floor, useAn, keep) {
  const sn = NR.snCreate(), an = useAn ? NR.anCreate() : null;
  if (mutate) mutate(sn, an);
  const out = new Float32Array(x.length);
  for (let i = 0; i < x.length; i += 1200) out.set(NR.snProcess(sn, x.subarray(i, Math.min(i + 1200, x.length)), floor, an), i);
  if (keep) keep.sn = sn;
  return out;
}
function runAn(x) {
  const an = NR.anCreate(); if (mutate) mutate(null, an);
  const y = x.slice();
  for (let i = 0; i < y.length; i += 1200) NR.anProcess(an, y.subarray(i, Math.min(i + 1200, y.length)));
  return y;
}

function gate(label, tune) {
  const before = fails;
  log(`\n[${label}]`);
  const N = FS * 8, noise = ssbNoise(N, -26, 7), voice = voiced(N, -16);

  check(FLOORS.length === 4 && FLOORS[0] === 1 && FLOORS[3] < FLOORS[2] && FLOORS[2] < FLOORS[1],
    `levels carry a floor each: ${FLOORS.join(" / ")}`);

  // 1. floor 1 = pure delay
  { const y = runSn(noise, 1); let worst = 0;
    for (let i = L; i < N; i++) worst = Math.max(worst, Math.abs(y[i] - noise[i - L]));
    check(worst < 1e-5, `floor 1 = input delayed by ${L} (max err ${worst.toExponential(1)})`); }

  // 2. latency of the processed path, on a strong CW note
  { const c = cw(N, 20, 700, 0.2).y, y = runSn(add(c, noise), FLOORS[3]);
    const { lag, corr } = bestLag(c.subarray(FS), y.subarray(FS), L - 200, L + 200);
    check(lag === L && corr > 0.5, `processed latency ${lag} (want ${L}), corr ${corr.toFixed(2)}`); }

  // 3. stationary noise per level
  { const got = FLOORS.map(f => db(rms(runSn(noise, f), FS * 3)) - db(rms(noise, FS * 3)));
    log(`  noise change Off/Low/Med/High: ${got.map(v => v.toFixed(1)).join(" / ")} dB`);
    for (let i = 1; i < 4; i++) { const want = db(FLOORS[i]);
      check(got[i] <= want + 2.5 && got[i] >= want - 0.5, `${NR.NR_LEVELS[i].name}: noise ${got[i].toFixed(1)} dB (floor ${want.toFixed(0)} dB)`); } }

  // 4. keyed CW in noise. SNR is key-down tone power over the noise in the whole
  //    2.4 kHz band, so 0 dB here is about +17 dB in a 50 Hz filter: a clear signal.
  //    -10 dB is a weak one.
  for (const wpm of [12, 35]) for (const snr of [10, 0, -10]) {
    const peak = 10 ** ((-26 + snr) / 20) * Math.SQRT2;
    const c = cw(N, wpm, 700, peak).y, mix = add(c, noise);
    const i0 = proj(c, mix, 0, FS * 2);
    const rows = [];
    for (let i = 1; i < 4; i++) { const r = proj(c, runSn(mix, FLOORS[i]), L, FS * 2); rows.push(r); }
    if (tune) log(`  CW ${wpm} WPM, ${String(snr).padStart(3)} dB: SDR in ${i0.sdr.toFixed(1)} -> ` +
      rows.map((r, i) => `${NR.NR_LEVELS[i + 1].name} ${r.sdr.toFixed(1)} (note ${r.gainDb.toFixed(1)})`).join("  "));
    const med = rows[1], high = rows[2];
    // A weak note is turned down too - a Wiener gain does that - but it must
    // come down LESS than the noise does, at every level, or NR is a loss.
    check(high.gainDb > (snr >= 0 ? -2.5 : db(FLOORS[3]) + 4), `CW ${wpm} WPM at ${snr} dB: note level ${high.gainDb.toFixed(1)} dB at High`);
    check(snr >= 0 ? med.sdr - i0.sdr >= 6 && high.sdr - i0.sdr >= 8 : med.sdr - i0.sdr >= 1 && high.sdr - i0.sdr >= 3,
      `CW ${wpm} WPM at ${snr} dB: SNR gain Med ${(med.sdr - i0.sdr).toFixed(1)} dB, High ${(high.sdr - i0.sdr).toFixed(1)} dB`);
  }

  // 5. clean voiced signal keeps its level at High
  { const y = runSn(voice, FLOORS[3]);
    const loss = db(rms(voice, FS, N - L)) - db(rms(y, FS + L, N));
    check(loss <= 3, `voiced signal loses <= 3 dB at High (${loss.toFixed(1)})`); }

  // 6. holes and squelch
  { const keep = {}, y = runSn(withHoles(noise, 2.2, 11), FLOORS[3], false, keep);
    const d = db(rms(y, FS * 3)) - db(rms(noise, FS * 3));
    check(keep.sn.plcPatched > 10 && d <= db(FLOORS[3]) + 4, `holes do not defeat it (${d.toFixed(1)} dB, ${keep.sn.plcPatched} patched)`);
    const sq = noise.slice(); sq.fill(0, FS * 4, FS * 4 + 6000);
    const z = runSn(sq, FLOORS[3]);
    const inside = db(rms(z, FS * 4 + L + 300, FS * 4 + L + 5700));
    const after = db(rms(z, FS * 5 + L, FS * 7)) - db(rms(noise, FS * 5, FS * 7 - L));
    check(inside < -100, `a 500 ms zero run (squelch) stays silent (${inside.toFixed(0)} dBFS)`);
    check(after <= db(FLOORS[3]) + 3, `and the floor is still known after it (${after.toFixed(1)} dB)`); }

  // 7. reset == new
  { const want = runSn(voice, FLOORS[2]);
    const sn = NR.snCreate(); if (mutate) mutate(sn, null);
    NR.snProcess(sn, noise, FLOORS[2]); NR.snReset(sn); if (mutate) mutate(sn, null);
    const got = new Float32Array(N);
    for (let i = 0; i < N; i += 1200) got.set(NR.snProcess(sn, voice.subarray(i, i + 1200), FLOORS[2]), i);
    let worst = 0; for (let i = 0; i < N; i++) worst = Math.max(worst, Math.abs(got[i] - want[i]));
    check(worst === 0, `a reset instance equals a new one (max diff ${worst})`); }

  // 8. a carrier that never keys counts as noise
  { const car = new Float32Array(N); for (let i = 0; i < N; i++) car[i] = 0.1 * Math.sin(2 * Math.PI * 1107 * i / FS);
    const y = runSn(add(car, noise), FLOORS[3]);
    const d = db(toneAmp(y, 1107, FS * 5 + L, FS * 7 + L)) - db(0.1);
    check(d <= -15, `a steady carrier is taken down by the NR alone (${d.toFixed(1)} dB at High after 5 s)`); }

  // ---- auto-notch
  { const mk = (amp, hz, until) => { const t = new Float32Array(N); for (let i = 0; i < until; i++) t[i] = amp * Math.sin(2 * Math.PI * hz * i / FS); return t; };
    const maxDiff = (p, q, from, to) => { let w = 0; for (let i = from; i < to; i++) w = Math.max(w, Math.abs(p[i] - q[i])); return w; };
    const tone = mk(0.15, 1107, FS * 5), y = runAn(add(tone, noise));
    const d1 = db(toneAmp(y, 1107, FS * 1.5, FS * 2.5)) - db(0.15);
    const d4 = db(toneAmp(y, 1107, FS * 3, FS * 5)) - db(0.15);
    check(d1 <= -30 && d4 <= -30, `notch: 1107 Hz carrier ${d1.toFixed(1)} dB at 1.5-2.5 s, ${d4.toFixed(1)} dB at 3-5 s`);
    const rel = maxDiff(y, noise, FS * 6, N);
    check(rel === 0, `notch lets go: 1 s after the carrier stops the output IS the input (max diff ${rel})`);
    const nz = maxDiff(runAn(noise), noise, 0, N);
    check(nz === 0, `notch on noise alone: output IS the input (max diff ${nz})`);
    const vd = maxDiff(runAn(voice), voice, 0, N);
    check(vd === 0, `notch on a voiced signal alone never engages (max diff ${vd})`);
    // a transmitter drifting 1 Hz a second stays notched
    { const dr = new Float32Array(N); let ph = 0; for (let i = 0; i < N; i++) { ph += 2 * Math.PI * (1000 + i / FS) / FS; dr[i] = 0.1 * Math.sin(ph); }
      const yd = runAn(add(dr, noise)); const dd = db(rms(yd, FS * 3, N)) - db(rms(noise, FS * 3, N));
      check(dd <= 1, `notch follows a carrier drifting 1 Hz/s: what is left is ${dd.toFixed(2)} dB over the noise`); }
    // a carrier 13 dB under the noise in the whole band is still 11 dB over it in 9 Hz: audible
    const weak = mk(0.015, 1107, N), yw = runAn(add(weak, noise));
    const dw = db(toneAmp(yw, 1107, FS * 3, N)) - db(0.015);
    check(dw <= -15, `notch: a carrier 13 dB under the band noise, ${dw.toFixed(1)} dB`);
    const y2 = runAn(add(mk(0.1, 600, N), mk(0.1, 2100.4, N), noise));
    const da = db(toneAmp(y2, 600, FS * 3, N)) - db(0.1), dbb = db(toneAmp(y2, 2100.4, FS * 3, N)) - db(0.1);
    check(da <= -30 && dbb <= -30, `notch: two carriers at once, ${da.toFixed(1)} / ${dbb.toFixed(1)} dB`);
    // the noise around a notched carrier is left alone: total change = the carrier's power, no more
    const yn = runAn(add(tone, noise)), resid = db(rms(yn, FS * 3, FS * 5)) - db(rms(noise, FS * 3, FS * 5));
    check(Math.abs(resid) <= 0.5, `notch: noise beside the notched carrier changes ${resid.toFixed(2)} dB`);
    // 12. notch ahead of the NR, with holes: the concealment must still find them
    const keep = {}; const yh = runSn(withHoles(add(tone, noise), 2.2, 11), FLOORS[2], true, keep);
    const dh = db(toneAmp(yh, 1107, FS * 3 + L, FS * 5 + L)) - db(0.15);
    check(keep.sn.plcPatched > 10 && dh <= -30, `notch + NR + holes: ${keep.sn.plcPatched} holes patched, carrier ${dh.toFixed(1)} dB`); }

  return fails - before;
}

const tune = process.argv.includes("--tune");
gate("spectral NR + auto-notch", tune);

// CPU
{ const x = ssbNoise(FS * 10, -26, 3);
  let t0 = performance.now(); runSn(x, 0.25); const a1 = performance.now() - t0;
  t0 = performance.now(); runAn(x); const a2 = performance.now() - t0;
  console.log(`  cpu per second of audio: NR ${(a1 / 10).toFixed(1)} ms, notch ${(a2 / 10).toFixed(1)} ms`); }

for (const [name, m] of [
  ["gain forced to 1", (sn) => { if (sn) sn.identity = true; }],
  ["notch frozen", (sn, an) => { if (an) an.frozen = true; }],
  ["concealment off", (sn) => { if (sn) sn.plcDisabled = true; }],
]) {
  const saved = fails; quiet = true; mutate = m;
  const caught = gate("mutation: " + name, false);
  quiet = false; mutate = null; fails = saved;
  check(caught > 0, `mutation (${name}) is refused by ${caught} check(s)`);
}

console.log(fails ? `\nweb_snr_harness: ${fails} FAILED` : "\nweb_snr_harness: all checks passed");
process.exit(fails ? 1 : 0);
