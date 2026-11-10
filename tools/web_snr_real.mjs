// T184: the page's spectral NR and auto-notch on REAL captures (12 kHz, 16-bit mono).
//
//   node tools/web_snr_real.mjs OUT_DIR clip.wav [clip.wav ...]
//
// No clean reference exists, so, as tools/web_nr_real.mjs does:
//  - 100 ms blocks are sorted by RNNoise's own VAD: speech > 0.6, pause < 0.1
//  - per level: level change in the pause blocks and in the speech blocks, for
//    the spectral NR and for RNNoise on the SAME blocks
//  - notch: how long it was engaged on the clip as recorded (should be never,
//    unless the clip holds a carrier), what it changed, and how far it takes a
//    1107 Hz carrier added 10 dB under the clip's own level
// Writes nr_ab_spectral_<clip>.wav: the 10 s with the most speech, Off -> Low
// -> Med -> High (spectral), then RNNoise Med, 0.5 s apart.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs"; import { gunzipSync } from "node:zlib";
const html = readFileSync("main/net/www/index.html", "utf8");
const NR = new Function(html.slice(html.indexOf("/* NR-BEGIN"), html.indexOf("/* NR-END")) +
  "\nreturn {NR_LEVELS,nrCreate,nrProcess,nrDestroy,NR_LATENCY_12K,SN_LATENCY_12K,snCreate,snProcess,anCreate,anProcess};")();
const wasm = gunzipSync(readFileSync("main/net/www/rnnoise.wasm.gz"));
const FS = 12000, L = NR.NR_LATENCY_12K, LS = NR.SN_LATENCY_12K;
const rd = p => { const b = readFileSync(p).subarray(44); const x = new Float32Array(b.length >> 1); for (let i = 0; i < x.length; i++) x[i] = b.readInt16LE(i * 2) / 32768; return x; };
const wr = (p, x) => { const b = Buffer.alloc(44 + x.length * 2); b.write("RIFF", 0); b.writeUInt32LE(36 + x.length * 2, 4); b.write("WAVEfmt ", 8); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(FS, 24); b.writeUInt32LE(FS * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write("data", 36); b.writeUInt32LE(x.length * 2, 40); for (let i = 0; i < x.length; i++) b.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(x[i] * 32767))), 44 + i * 2); writeFileSync(p, b); };
const rn = async (x, wet) => { const nr = await NR.nrCreate(wasm); const y = new Float32Array(x.length), v = [];
  for (let i = 0; i < x.length; i += 120) { y.set(NR.nrProcess(nr, x.subarray(i, Math.min(i + 120, x.length)), wet), i); v.push(nr.vad); }
  NR.nrDestroy(nr); return { y, v }; };
const sn = (x, floor) => { const s = NR.snCreate(), y = new Float32Array(x.length);
  for (let i = 0; i < x.length; i += 1200) y.set(NR.snProcess(s, x.subarray(i, Math.min(i + 1200, x.length)), floor), i); return y; };
const an = (x) => { const a = NR.anCreate(), y = x.slice(); let onFrames = 0, frames = 0; const seen = new Set();
  for (let i = 0; i < y.length; i += 512) { NR.anProcess(a, y.subarray(i, Math.min(i + 512, y.length))); frames++;
    const on = a.tr.filter(t => t.on); if (on.length) onFrames++; for (const t of on) seen.add(Math.round(t.f / 10) * 10); }
  return { y, pct: 100 * onFrames / frames, seen: [...seen] }; };
const pw = (x, a, b) => { let s = 0; for (let i = a; i < b; i++) s += x[i] * x[i]; return s; };
const db = v => 10 * Math.log10(v + 1e-20);
const tone = (x, hz, from, to) => { let s = 0, c = 0; for (let i = from; i < to; i++) { const p = 2 * Math.PI * hz * i / FS; s += x[i] * Math.sin(p); c += x[i] * Math.cos(p); } return 2 * Math.hypot(s, c) / (to - from); };
const outDir = process.argv[2]; mkdirSync(outDir, { recursive: true });
for (const f of process.argv.slice(3)) {
  const x = rd(f), name = f.split(/[\\/]/).pop().replace(".wav", "");
  const hi = await rn(x, 1);
  const nb = Math.floor((x.length - L) / 1200), sp = [], pa = [];
  for (let b = 2; b < nb; b++) { let m = 0; for (let k = 0; k < 10; k++) m += hi.v[Math.min(hi.v.length - 1, b * 10 + k + 9)] / 10; if (m > 0.6) sp.push(b); else if (m < 0.1) pa.push(b); }
  const lvl = (y, blocks, off) => { let s = 0; for (const b of blocks) s += pw(y, b * 1200 + off, (b + 1) * 1200 + off); return s / Math.max(1, blocks.length); };
  const inS = lvl(x, sp, 0), inP = lvl(x, pa, 0);
  console.log(`\n${name}: ${(x.length / FS).toFixed(0)} s, ${sp.length} speech / ${pa.length} pause blocks, speech stands ${(db(inS) - db(inP)).toFixed(1)} dB over the pauses`);
  console.log("  level     pause dB / speech dB / contrast gain dB     spectral | RNNoise");
  const outs = [];
  for (const lv of NR.NR_LEVELS) {
    const ys = sn(x, lv.floor); outs.push(ys);
    if (lv.wet === 0) continue;
    const yr = (await rn(x, lv.wet)).y;
    const a = [db(lvl(ys, pa, LS)) - db(inP), db(lvl(ys, sp, LS)) - db(inS)], b = [db(lvl(yr, pa, L)) - db(inP), db(lvl(yr, sp, L)) - db(inS)];
    console.log(`  ${lv.name.padEnd(5)}  ${a[0].toFixed(1).padStart(6)} / ${a[1].toFixed(1).padStart(5)} / ${(a[1] - a[0]).toFixed(1).padStart(5)}   |  ${b[0].toFixed(1).padStart(6)} / ${b[1].toFixed(1).padStart(5)} / ${(b[1] - b[0]).toFixed(1).padStart(5)}`);
    if (lv.name === "Med") outs.rnMed = yr;
  }
  // notch on the clip as recorded
  const n0 = an(x); let e = 0, s = 0; for (let i = 0; i < x.length; i++) { e += (n0.y[i] - x[i]) ** 2; s += x[i] * x[i]; }
  console.log(`  notch, clip as recorded: engaged ${n0.pct.toFixed(1)} % of the time${n0.seen.length ? " at " + n0.seen.join(", ") + " Hz" : ""}; changed the audio by ${e === 0 ? "nothing (bit-exact)" : (db(e) - db(s)).toFixed(1) + " dB re the input"}`);
  // with a carrier 10 dB under the clip
  let r = 0; for (let i = 0; i < x.length; i++) r += x[i] * x[i]; r = Math.sqrt(r / x.length);
  const amp = r * 0.316 * Math.SQRT2, xt = x.map((v, i) => v + amp * Math.sin(2 * Math.PI * 1107 * i / FS)), n1 = an(xt);
  console.log(`  notch, 1107 Hz carrier added 10 dB under the clip: carrier ${(20 * Math.log10(tone(n1.y, 1107, FS * 2, x.length) / amp)).toFixed(1)} dB (from 2 s on), engaged ${n1.pct.toFixed(1)} %`);
  // A/B
  let best = 0, bestN = -1; for (let s0 = 0; s0 + 100 <= nb; s0 += 10) { const n = sp.filter(b => b >= s0 && b < s0 + 100).length; if (n > bestN) { bestN = n; best = s0; } }
  const seg = Math.min(100, nb) * 1200, gap = FS / 2, parts = [...outs.map(y => [y, LS]), [outs.rnMed, L]], ab = new Float32Array(parts.length * seg + (parts.length - 1) * gap);
  parts.forEach(([y, lat], i) => ab.set(y.subarray(best * 1200 + lat, best * 1200 + lat + seg), i * (seg + gap)));
  wr(`${outDir}/nr_ab_spectral_${name}.wav`, ab);
  // and the carrier case: as received, then notched
  const nbv = new Float32Array(2 * seg + gap); nbv.set(xt.subarray(best * 1200, best * 1200 + seg), 0); nbv.set(n1.y.subarray(best * 1200, best * 1200 + seg), seg + gap);
  wr(`${outDir}/notch_ab_${name}.wav`, nbv);
}
