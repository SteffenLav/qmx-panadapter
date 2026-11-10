// T184: the page's spectral NR on REAL CW captures (12 kHz, 16-bit mono, CW mode).
//
//   node tools/web_snr_cw_real.mjs OUT_DIR clip.wav [clip.wav ...]
//
// No clean reference exists. The note itself is the reference:
//  - the note's frequency is the strongest line of the clip's average spectrum
//  - 20 ms blocks are sorted by the INPUT's power at that frequency:
//    key-down = within 6 dB of the 95th percentile, key-up = within 3 dB of the 10th
//  - a note is PRESENT only if the 95th percentile stands > 20 dB over the 10th.
//    Noise through the CW filter gives 14.5 dB (exponential power: ln 20 / ln(1/0.9)),
//    measured 15.0 on an empty frequency. The first version asked for a line 10 dB
//    over the median of 200-1500 Hz and called the empty frequency a note: that
//    measured the CW filter's shape.
//  - "long gaps" are key-up blocks with a key-up block either side (>= 60 ms of
//    gap), split into the note's own frequency and the rest of the band
//  - per level, on the SAME blocks of the delay-aligned output:
//      note   = change of the power at the note's frequency in key-down blocks
//      gaps   = change of the whole-band power in key-up blocks
//      gain   = note - gaps: how much further the note stands over the gaps
// A clip with no note is reported as noise: level change only.
// Writes cw_ab_<clip>.wav: 10 s, Off -> Low -> Med -> High, 0.5 s apart.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
const html = readFileSync("main/net/www/index.html", "utf8");
const NR = new Function(html.slice(html.indexOf("/* NR-BEGIN"), html.indexOf("/* NR-END")) +
  "\nreturn {NR_LEVELS,SN_LATENCY_12K,snCreate,snProcess};")();
const FS = 12000, L = NR.SN_LATENCY_12K, B = 240;
const rd = p => { const b = readFileSync(p).subarray(44); const x = new Float32Array(b.length >> 1); for (let i = 0; i < x.length; i++) x[i] = b.readInt16LE(i * 2) / 32768; return x; };
const wr = (p, x) => { const b = Buffer.alloc(44 + x.length * 2); b.write("RIFF", 0); b.writeUInt32LE(36 + x.length * 2, 4); b.write("WAVEfmt ", 8); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(FS, 24); b.writeUInt32LE(FS * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write("data", 36); b.writeUInt32LE(x.length * 2, 40); for (let i = 0; i < x.length; i++) b.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(x[i] * 32767))), 44 + i * 2); writeFileSync(p, b); };
const db = v => 10 * Math.log10(v + 1e-20);
// Hann-windowed power at f over [from, from+len)
function gz(x, f, from, len) { const c = 2 * Math.cos(2 * Math.PI * f / FS); let s1 = 0, s2 = 0;
  for (let i = 0; i < len; i++) { const s0 = x[from + i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / len)) + c * s1 - s2; s2 = s1; s1 = s0; }
  return (s1 * s1 + s2 * s2 - c * s1 * s2) / (len * len); }
const pw = (x, a, n) => { let s = 0; for (let i = a; i < a + n; i++) s += x[i] * x[i]; return s / n; };
const sn = (x, floor) => { const s = NR.snCreate(), y = new Float32Array(x.length);
  for (let i = 0; i < x.length; i += 1200) y.set(NR.snProcess(s, x.subarray(i, Math.min(i + 1200, x.length)), floor), i); return { y, patched: s.plcPatched }; };
const outDir = process.argv[2]; mkdirSync(outDir, { recursive: true });
for (const f of process.argv.slice(3)) {
  const x = rd(f), name = f.split(/[\\/]/).pop().replace(".wav", "");
  // average spectrum 200..1500 Hz in 10 Hz steps, 1 s slices
  const spec = []; for (let hz = 200; hz <= 1500; hz += 10) { let p = 0, n = 0; for (let t = 0; t + FS <= x.length; t += FS) { p += gz(x, hz, t, FS); n++; } spec.push([hz, p / n]); }
  const med = spec.map(s => s[1]).sort((a, b) => a - b)[spec.length >> 1];
  const [f0, pk] = spec.reduce((a, b) => b[1] > a[1] ? b : a);
  const nb = Math.floor((x.length - L) / B);
  console.log(`\n${name}: ${(x.length / FS).toFixed(0)} s, rms ${(db(pw(x, 0, x.length))).toFixed(1)} dBFS, strongest line ${f0} Hz, ${(db(pk) - db(med)).toFixed(1)} dB over the median of 200-1500 Hz`);
  const outs = NR.NR_LEVELS.map(lv => sn(x, lv.floor));
  console.log(`  holes concealed: ${outs[1].patched}`);
  let zr = 0; for (let i = 0, run = 0; i <= x.length; i++) { if (i < x.length && x[i] === 0) run++; else { if (run >= 100) zr++; run = 0; } }
  console.log(`  runs of >= 100 exact zeros in the capture: ${zr}`);
  const tone = []; for (let b = 0; b < nb; b++) tone.push(gz(x, f0, b * B, B));
  const sorted = tone.slice().sort((a, b) => a - b), p95 = sorted[Math.floor(0.95 * nb)], p10 = sorted[Math.floor(0.10 * nb)];
  if (db(p95) - db(p10) < 20) {
    console.log(`  NO NOTE (p95 stands ${(db(p95) - db(p10)).toFixed(1)} dB over p10; noise gives 14.5): noise only. Level change Low/Med/High: ` +
      outs.slice(1).map(o => (db(pw(o.y, FS * 2 + L, x.length - FS * 2 - L)) - db(pw(x, FS * 2, x.length - FS * 2 - L))).toFixed(1)).join(" / ") + " dB");
    continue;
  }
  const down = [], up = [];
  for (let b = Math.ceil(FS * 2 / B); b < nb; b++) { if (tone[b] >= p95 / 4) down.push(b); else if (tone[b] <= p10 * 2) up.push(b); }
  const noteIn = down.reduce((s, b) => s + tone[b], 0) / down.length, gapIn = up.reduce((s, b) => s + pw(x, b * B, B), 0) / up.length;
  console.log(`  key-down ${down.length} blocks, key-up ${up.length} of ${nb}; at the note's frequency key-down stands ${(db(p95) - db(p10)).toFixed(1)} dB over key-up`);
  console.log("  level   note dB / gaps dB / gain dB");
  NR.NR_LEVELS.forEach((lv, i) => { if (!i) return; const y = outs[i].y;
    const note = db(down.reduce((s, b) => s + gz(y, f0, b * B + L, B), 0) / down.length) - db(noteIn);
    const gaps = db(up.reduce((s, b) => s + pw(y, b * B + L, B), 0) / up.length) - db(gapIn);
    console.log(`  ${lv.name.padEnd(5)}  ${note.toFixed(1).padStart(5)} / ${gaps.toFixed(1).padStart(5)} / ${(note - gaps).toFixed(1).padStart(5)}`); });
  { const ups = new Set(up), lg = up.filter(b => ups.has(b - 1) && ups.has(b + 1));
    // the rest of the band = whole-band power minus the power within +-60 Hz of the note
    const nb3 = (y, off) => { let a = 0, t = 0; for (const b of lg) { const w = pw(y, b * B + off, B), n = 1.5 * gz(y, f0, b * B + off, B) * 2; t += n; a += Math.max(0, w - n); } return [t / lg.length, a / lg.length]; };
    const i0 = nb3(x, 0);
    console.log(`  long gaps: ${lg.length} blocks; in them the note's frequency holds ${(db(i0[0]) - db(i0[0] + i0[1])).toFixed(1)} dB of the power`);
    NR.NR_LEVELS.forEach((lv, i) => { if (!i) return; const o = nb3(outs[i].y, L);
      console.log(`  ${lv.name.padEnd(5)}  long gaps: at the note ${(db(o[0]) - db(i0[0])).toFixed(1)} dB, rest of the band ${(db(o[1]) - db(i0[1])).toFixed(1)} dB`); }); }
  // A/B: the 10 s with the most key-down blocks
  const per = FS * 10 / B; let best = 0, bestN = -1;
  for (let s0 = 0; s0 + per <= nb; s0 += 50) { const n = down.filter(b => b >= s0 && b < s0 + per).length; if (n > bestN) { bestN = n; best = s0; } }
  const seg = FS * 10, gap = FS / 2, ab = new Float32Array(4 * seg + 3 * gap);
  outs.forEach((o, i) => ab.set(o.y.subarray(best * B + L, best * B + L + seg), i * (seg + gap)));
  wr(`${outDir}/cw_ab_${name}.wav`, ab);
}
