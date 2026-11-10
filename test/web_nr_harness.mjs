// T18 - host harness for the browser noise reduction (RNNoise WASM).
//
//   node test/web_nr_harness.mjs                       # the gate, synthetic only
//   node test/web_nr_harness.mjs --wav-dir D [--out-dir O]
//        D holds real captures (tools/web_audio_capture.py) and a clean speech
//        file named speech*.wav / tts*.wav; reports per-level numbers and, with
//        --out-dir, writes A/B WAVs to listen to. Report only - the gate is the
//        synthetic part, which needs nothing outside the repo.
//
// It runs the page's OWN code: the block between NR-BEGIN and NR-END in
// main/net/www/index.html is extracted and evaluated, and the WASM is the
// committed main/net/www/rnnoise.wasm.gz. A copy of either would test the copy.
//
// Checks:
//   1. the module has no imports (the page instantiates it with {})
//   2. dry path (wet=0) is the input delayed by exactly NR_LATENCY_12K
//   3. wet path latency, by cross-correlation, equals NR_LATENCY_12K
//   4. stationary noise is attenuated at High by at least NOISE_MIN_DB
//   5. voiced test signal keeps its level within VOICE_MAX_LOSS_DB at High
//   6. Low/Med noise floor sits where the wet/dry mix says (-6 / -14 dB)
//   7. CPU: seconds of compute per second of audio, reported
//   8. MUTATION: the same checks against an identity "model" must FAIL
import { readFileSync, writeFileSync, readdirSync, mkdirSync } from "node:fs";
import { gunzipSync } from "node:zlib";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const FS = 12000;
const NOISE_MIN_DB = 10;      // High must take stationary noise down >= 10 dB
const VOICE_MAX_LOSS_DB = 3;  // and a clean voiced signal down <= 3 dB

// ---- load the page's block -------------------------------------------------
const html = readFileSync(join(ROOT, "main/net/www/index.html"), "utf8");
const a = html.indexOf("/* NR-BEGIN"), b = html.indexOf("/* NR-END");
if (a < 0 || b < 0) { console.error("NR-BEGIN/NR-END not found in index.html"); process.exit(1); }
const NR = new Function(html.slice(a, b) +
  "\nreturn { NR_LEVELS, NR_LATENCY_12K, nrCreate, nrDestroy, nrProcess, nrReset };")();
const wasm = gunzipSync(readFileSync(join(ROOT, "main/net/www/rnnoise.wasm.gz")));

let fails = 0;
const check = (ok, msg) => { console.log(`  ${ok ? "ok  " : "FAIL"} ${msg}`); if (!ok) fails++; return ok; };

// ---- deterministic signals ---------------------------------------------------
function rng(seed) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }
function gauss(r) { return Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r()); }
// Band-limited noise like the captures (300-2700 Hz after the mode filter),
// made by a simple 2-pole band-pass over white noise.
function ssbNoise(n, rmsDb, seed) {
  const r = rng(seed), y = new Float32Array(n);
  const f0 = 1500, bw = 2400, R = Math.exp(-Math.PI * bw / FS), th = 2 * Math.PI * f0 / FS;
  const a1 = 2 * R * Math.cos(th), a2 = -R * R;
  let y1 = 0, y2 = 0;
  for (let i = 0; i < n; i++) { const v = gauss(r) + a1 * y1 + a2 * y2; y2 = y1; y1 = v; y[i] = v; }
  return scaleTo(y, rmsDb);
}
// A voiced test signal: 120 Hz glottal-like harmonics through two formants,
// gated in 250 ms syllables. Not speech, but what RNNoise keys its voice
// detection on (pitch + spectral envelope), so it should pass.
function voiced(n, rmsDb) {
  const y = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / FS, f0 = 120 * (1 + 0.08 * Math.sin(2 * Math.PI * 0.7 * t));
    let v = 0;
    for (let k = 1; k * 120 < 3000; k++) {
      const f = k * f0;
      const g = 1 / (1 + ((f - 700) / 150) ** 2) + 0.6 / (1 + ((f - 1200) / 200) ** 2);
      v += g * Math.sin(2 * Math.PI * k * 120 * t + 0.08 * k * 120 / 0.7 * (1 - Math.cos(2 * Math.PI * 0.7 * t)) / (2 * Math.PI) * 0) ;
    }
    const syl = Math.sin(Math.PI * ((t % 0.35) / 0.25));
    y[i] = (t % 0.35) < 0.25 ? v * syl : 0;
  }
  return scaleTo(y, rmsDb);
}
// Insert rx_audio.c-shaped gap holes at `perSec` a second (deterministic).
function withHoles(x, perSec, seed) {
  const r = rng(seed), y = x.slice(); let t = 6000;
  while (t + 400 < y.length) {
    for (let k = 0; k < 120; k++) y[t + k] *= 1 - k / 120;          // fade out
    y.fill(0, t + 120, t + 256);                                    // 136 zeros
    for (let k = 0; k < 120; k++) y[t + 256 + k] *= k / 120;         // fade in
    t += Math.round(FS / perSec * (0.5 + r()));
  }
  return y;
}
function rms(x, from = 0, to = x.length) { let s = 0; for (let i = from; i < to; i++) s += x[i] * x[i]; return Math.sqrt(s / Math.max(1, to - from)); }
const db = (v) => 20 * Math.log10(v + 1e-12);
function scaleTo(x, rmsDb) { const g = 10 ** (rmsDb / 20) / rms(x); for (let i = 0; i < x.length; i++) x[i] *= g; return x; }
function add(x, y) { const z = new Float32Array(x.length); for (let i = 0; i < x.length; i++) z[i] = x[i] + y[i]; return z; }

// Run through in 100 ms frames, like audioFrame() does.
function run(nr, x, wet) {
  const out = new Float32Array(x.length);
  for (let i = 0; i < x.length; i += 1200) out.set(NR.nrProcess(nr, x.subarray(i, Math.min(i + 1200, x.length)), wet), i);
  return out;
}
// Best lag of y against x in [0, maxLag].
function bestLag(x, y, maxLag) {
  let best = -1, bestC = -Infinity;
  for (let L = 0; L <= maxLag; L++) {
    let c = 0, ex = 0, ey = 0;
    for (let i = 0; i + L < y.length; i++) { c += x[i] * y[i + L]; ex += x[i] * x[i]; ey += y[i + L] * y[i + L]; }
    c /= Math.sqrt(ex * ey) + 1e-12;
    if (c > bestC) { bestC = c; best = L; }
  }
  return { lag: best, corr: bestC };
}

// ---- the gate ----------------------------------------------------------------
async function gate(wasmBytes, label, quiet) {
  const before = fails;
  const log = quiet ? () => {} : console.log;
  if (!quiet) console.log(`\n[${label}]`);
  const _check = quiet ? (ok) => { if (!ok) fails++; return ok; } : check;

  const mod = new WebAssembly.Module(wasmBytes);
  _check(WebAssembly.Module.imports(mod).length === 0, `no imports (${WebAssembly.Module.imports(mod).length})`);

  const L = NR.NR_LATENCY_12K, N = FS * 6;
  const noise = ssbNoise(N, -22, 7), voice = voiced(N, -16);

  // 2. dry alignment
  { const nr = await NR.nrCreate(wasmBytes); const y = run(nr, noise, 0); NR.nrDestroy(nr);
    let worst = 0; for (let i = L; i < N; i++) worst = Math.max(worst, Math.abs(y[i] - noise[i - L]));
    _check(worst === 0, `dry path = input delayed by ${L} exactly (max err ${worst})`); }

  // 3. wet latency on the voiced signal
  { const nr = await NR.nrCreate(wasmBytes); const y = run(nr, voice, 1); NR.nrDestroy(nr);
    const { lag, corr } = bestLag(voice.subarray(FS), y.subarray(FS), L + 200);
    _check(lag === L && corr > 0.5, `wet latency ${lag} (want ${L}), corr ${corr.toFixed(2)}`); }

  // 4./6. noise floor per level (skip the first second: the model settles)
  const noiseDb = [];
  for (const lv of NR.NR_LEVELS) {
    const nr = await NR.nrCreate(wasmBytes); const y = run(nr, noise, lv.wet); NR.nrDestroy(nr);
    noiseDb.push(db(rms(y, FS * 2)) - db(rms(noise, FS * 2)));
  }
  log(`  noise change Off/Low/Med/High: ${noiseDb.map(v => v.toFixed(1)).join(" / ")} dB`);
  _check(noiseDb[3] <= -NOISE_MIN_DB, `High takes stationary noise down >= ${NOISE_MIN_DB} dB (${noiseDb[3].toFixed(1)})`);
  // With the wet path near zero, Low leaves 0.5 of the dry (-6 dB) and Med 0.2 (-14 dB).
  _check(Math.abs(noiseDb[1] - (-6.0)) < 1.5, `Low floor near -6 dB (${noiseDb[1].toFixed(1)})`);
  _check(Math.abs(noiseDb[2] - (-14.0)) < 2.5, `Med floor near -14 dB (${noiseDb[2].toFixed(1)})`);

  // 4b. rx_audio.c gap-frame holes: 10 ms fade, 136 exact zeros, 10 ms fade-in,
  //     ~2 a second as measured on bench1. With concealment the noise must come
  //     down nearly as far as without holes; a long zero run (a closed squelch)
  //     must be left as silence.
  { const holed = withHoles(noise, 2.2, 11);
    const nr = await NR.nrCreate(wasmBytes); const y = run(nr, holed, 1); NR.nrDestroy(nr);
    const d = db(rms(y, FS * 2)) - db(rms(noise, FS * 2));
    log(`  noise with gap-frame holes, High: ${d.toFixed(1)} dB, ${nr.plcPatched} holes patched`);
    _check(d <= noiseDb[3] + 10 && d <= -NOISE_MIN_DB, `holes do not defeat NR (${d.toFixed(1)} dB vs ${noiseDb[3].toFixed(1)} without)`);
    const sq = noise.slice(); sq.fill(0, FS * 3, FS * 3 + 2400);
    const nr2 = await NR.nrCreate(wasmBytes); const z = run(nr2, sq, 0); NR.nrDestroy(nr2);
    let nz = 0; for (let i = FS * 3 + 1; i < FS * 3 + 2399; i++) if (z[i + L] !== 0) nz++;
    _check(nz === 0, `a 200 ms zero run (squelch) stays silent (${nz} non-zero)`); }

  // 5. clean voiced signal keeps its level
  { const nr = await NR.nrCreate(wasmBytes); const y = run(nr, voice, 1); NR.nrDestroy(nr);
    const loss = db(rms(voice, FS, N - L)) - db(rms(y, FS + L, N));
    _check(loss <= VOICE_MAX_LOSS_DB, `voiced signal loses <= ${VOICE_MAX_LOSS_DB} dB at High (${loss.toFixed(1)})`); }

  // 5b. nrReset(): the page resets the instance every time NR starts or the
  //     mode changes. A reset instance must be a NEW one, sample for sample -
  //     otherwise the previous signal's noise estimate leaks into the next.
  { const fresh = await NR.nrCreate(wasmBytes); const want = run(fresh, voice, 1); NR.nrDestroy(fresh);
    const nr = await NR.nrCreate(wasmBytes); run(nr, noise, 1); NR.nrReset(nr);
    const got = run(nr, voice, 1); NR.nrDestroy(nr);
    let worst = 0; for (let i = 0; i < N; i++) worst = Math.max(worst, Math.abs(got[i] - want[i]));
    _check(worst === 0, `a reset instance equals a new one (max diff ${worst})`); }

  // 7. CPU
  if (!quiet) {
    const nr = await NR.nrCreate(wasmBytes); const t0 = performance.now(); run(nr, noise, 1);
    const ms = performance.now() - t0; NR.nrDestroy(nr);
    log(`  cpu: ${(ms / 6).toFixed(1)} ms per second of audio (${(ms / 60).toFixed(2)} %)`);
  }
  return fails - before;
}

// ---- main --------------------------------------------------------------------
const args = process.argv.slice(2);
const opt = (k) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : null; };

console.log(`rnnoise.wasm: ${wasm.length} bytes, gz ${readFileSync(join(ROOT, "main/net/www/rnnoise.wasm.gz")).length}`);
await gate(wasm, "RNNoise", false);

// 8. Mutation: replace rnnoise_process_frame with a copy (no denoising).
{
  const real = NR.nrCreate;
  NR.nrCreate = async (bytes) => {
    const nr = await real(bytes);
    const ex = nr.ex;
    nr.ex = { ...ex, rnnoise_process_frame: (st, o, i) => {
      const h = new Float32Array(ex.memory.buffer); h.copyWithin(o >> 2, i >> 2, (i >> 2) + 480); return 0; } };
    return nr;
  };
  const saved = fails;
  const caught = await gate(wasm, "mutation: identity model", true);
  fails = saved;
  NR.nrCreate = real;
  check(caught > 0, `mutation (identity model) is refused by ${caught} check(s)`);
}
{
  const real = NR.nrCreate;
  NR.nrCreate = async (bytes) => { const nr = await real(bytes); nr.plcDisabled = true; return nr; };
  const saved = fails;
  const caught = await gate(wasm, "mutation: no hole concealment", true);
  fails = saved; NR.nrCreate = real;
  check(caught > 0, `mutation (concealment off) is refused by ${caught} check(s)`);
}
{
  const real = NR.nrReset;
  NR.nrReset = (nr) => nr;
  const saved = fails;
  const caught = await gate(wasm, "mutation: reset does nothing", true);
  fails = saved; NR.nrReset = real;
  check(caught > 0, `mutation (reset does nothing) is refused by ${caught} check(s)`);
}

// ---- optional report on real captures -----------------------------------------
const dir = opt("--wav-dir"), outDir = opt("--out-dir");
if (dir) {
  const readWav = (p) => { const b = readFileSync(p); let o = 12; let fmt, data;
    while (o < b.length) { const id = b.toString("ascii", o, o + 4), n = b.readUInt32LE(o + 4);
      if (id === "fmt ") fmt = { rate: b.readUInt32LE(o + 12), bits: b.readUInt16LE(o + 22), ch: b.readUInt16LE(o + 10) };
      if (id === "data") data = b.subarray(o + 8, o + 8 + n); o += 8 + n + (n & 1); }
    if (fmt.bits !== 16 || fmt.ch !== 1) throw new Error(p + ": want 16-bit mono");
    const x = new Float32Array(data.length / 2); for (let i = 0; i < x.length; i++) x[i] = data.readInt16LE(i * 2) / 32768;
    return { rate: fmt.rate, x }; };
  const writeWav = (p, x) => { const b = Buffer.alloc(44 + x.length * 2);
    b.write("RIFF", 0); b.writeUInt32LE(36 + x.length * 2, 4); b.write("WAVEfmt ", 8); b.writeUInt32LE(16, 16);
    b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(FS, 24); b.writeUInt32LE(FS * 2, 28);
    b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write("data", 36); b.writeUInt32LE(x.length * 2, 40);
    for (let i = 0; i < x.length; i++) b.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(x[i] * 32767))), 44 + i * 2);
    writeFileSync(p, b); };
  if (outDir) mkdirSync(outDir, { recursive: true });
  const files = readdirSync(dir).filter(f => f.endsWith(".wav")).sort();
  const speechF = files.filter(f => /^(speech|tts)/.test(f));
  const noiseF = files.filter(f => !/^(speech|tts)/.test(f));
  const L = NR.NR_LATENCY_12K;

  console.log(`\n[real noise captures: change in level per level Off/Low/Med/High]`);
  for (const f of noiseF) {
    const { rate, x } = readWav(join(dir, f)); if (rate !== FS) continue;
    const r = [];
    for (const lv of NR.NR_LEVELS) { const nr = await NR.nrCreate(wasm); const y = run(nr, x, lv.wet); NR.nrDestroy(nr);
      r.push(db(rms(y, FS * 2)) - db(rms(x, FS * 2))); }
    console.log(`  ${f.padEnd(24)} ${r.map(v => v.toFixed(1).padStart(6)).join(" ")} dB`);
  }

  // Speech + real noise at a known SNR. SDR: project the output on the
  // (delayed) clean speech; what is left is noise + distortion.
  const sdr = (clean, y) => { let cc = 0, cy = 0; const n = clean.length - L;
    for (let i = FS; i < n; i++) { cc += clean[i] * clean[i]; cy += clean[i] * y[i + L]; }
    const g = cy / cc; let e = 0, s = 0;
    for (let i = FS; i < n; i++) { const t = g * clean[i]; s += t * t; e += (y[i + L] - t) ** 2; }
    return { sdr: 10 * Math.log10(s / e), gainDb: db(Math.abs(g)) }; };
  const noiseCap = noiseF.map(f => readWav(join(dir, f)).x);
  for (const sf of speechF) {
    let { x: sp } = readWav(join(dir, sf));
    console.log(`\n[${sf} + real QMX noise: SDR in -> out per level, and speech level change]`);
    for (const snr of [0, 6, 12, 20]) {
      // tile the real noise to the speech length (captures are 20-40 s)
      const nz = new Float32Array(sp.length); for (let i = 0; i < nz.length; i++) nz[i] = noiseCap[0][i % noiseCap[0].length];
      // SNR over the speech-active samples (where the clean is above -45 dBFS in 20 ms)
      const act = []; for (let i = 0; i + 240 <= sp.length; i += 240) if (db(rms(sp, i, i + 240)) > -45) act.push(i);
      let ps = 0, pn = 0; for (const i of act) { for (let k = 0; k < 240; k++) { ps += sp[i + k] ** 2; pn += nz[i + k] ** 2; } }
      const g = Math.sqrt(ps / pn) / 10 ** (snr / 20); for (let i = 0; i < nz.length; i++) nz[i] *= g;
      const mix = add(sp, nz);
      // normalise the mix to the captures' level, as the Tab5's AGC would
      const k = 10 ** (-20 / 20) / Math.max(...[rms(mix)]); for (let i = 0; i < mix.length; i++) mix[i] *= k;
      const clean = sp.map(v => v * k);
      const row = [];
      for (const lv of NR.NR_LEVELS) { const nr = await NR.nrCreate(wasm); const y = run(nr, mix, lv.wet); NR.nrDestroy(nr);
        const r = sdr(clean, y); row.push(`${lv.name} ${r.sdr.toFixed(1)}/${r.gainDb.toFixed(1)}`);
        if (outDir) writeFileSync && writeWav(join(outDir, `${sf.replace(".wav", "")}_snr${snr}_${lv.name}.wav`), y); }
      console.log(`  SNR ${String(snr).padStart(2)} dB: ${row.join("   ")}   (SDR dB / speech gain dB)`);
    }
  }
}

console.log(fails ? `\nweb_nr_harness: ${fails} FAILED` : "\nweb_nr_harness: all checks passed");
process.exit(fails ? 1 : 0);
