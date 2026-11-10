// T18: the page's NR block on REAL SSB captures. No clean reference exists, so:
//  - blocks of 100 ms are sorted by RNNoise's own VAD (High run): speech > 0.6, pause < 0.1
//  - per level: level change in the pause blocks and in the speech blocks
//  - "contrast gain" = speech change - pause change = how much further the voice
//    stands above the noise between words. Speech blocks still contain noise, so
//    the speech figure is a mix: read it as an upper bound on speech loss.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs"; import { gunzipSync } from "node:zlib";
const html = readFileSync("main/net/www/index.html", "utf8");
const NR = new Function(html.slice(html.indexOf("/* NR-BEGIN"), html.indexOf("/* NR-END")) +
  "\nreturn {NR_LEVELS,nrCreate,nrProcess,nrDestroy,NR_LATENCY_12K};")();
const wasm = gunzipSync(readFileSync("main/net/www/rnnoise.wasm.gz"));
const FS = 12000, L = NR.NR_LATENCY_12K;
const rd = p => { const b = readFileSync(p).subarray(44); const x = new Float32Array(b.length >> 1); for (let i = 0; i < x.length; i++) x[i] = b.readInt16LE(i * 2) / 32768; return x; };
const wr = (p, x) => { const b = Buffer.alloc(44 + x.length * 2); b.write("RIFF", 0); b.writeUInt32LE(36 + x.length * 2, 4); b.write("WAVEfmt ", 8); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(FS, 24); b.writeUInt32LE(FS * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write("data", 36); b.writeUInt32LE(x.length * 2, 40); for (let i = 0; i < x.length; i++) b.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(x[i] * 32767))), 44 + i * 2); writeFileSync(p, b); };
const run = async (x, wet, wantVad) => { const nr = await NR.nrCreate(wasm); const y = new Float32Array(x.length), v = [];
  for (let i = 0; i < x.length; i += 120) { y.set(NR.nrProcess(nr, x.subarray(i, Math.min(i + 120, x.length)), wet), i); v.push(nr.vad); }
  const patched = nr.plcPatched; NR.nrDestroy(nr); return { y, v, patched }; };
const pw = (x, a, b) => { let s = 0; for (let i = a; i < b; i++) s += x[i] * x[i]; return s; };
const db = v => 10 * Math.log10(v + 1e-20);
const outDir = process.argv[2]; mkdirSync(outDir, { recursive: true });
console.log("clip                      dur  holes  speech/pause blocks  in contrast |  level: pause dB / speech dB / contrast gain dB");
for (const f of process.argv.slice(3)) {
  const x = rd(f), name = f.split("/").pop().replace(".wav", "");
  const hi = await run(x, 1, true);
  // VAD frame k was computed when input sample k*120 went in; the model saw input ~1040 samples earlier (PLC hold + FIR)
  const nb = Math.floor((x.length - L) / 1200), sp = [], pa = [];
  for (let b = 2; b < nb; b++) { let m = 0; for (let k = 0; k < 10; k++) m += hi.v[Math.min(hi.v.length - 1, b * 10 + k + 9)] / 10; if (m > 0.6) sp.push(b); else if (m < 0.1) pa.push(b); }
  const lvl = (y, blocks, off) => { let s = 0; for (const b of blocks) s += pw(y, b * 1200 + off, (b + 1) * 1200 + off); return s / Math.max(1, blocks.length); };
  const inS = lvl(x, sp, 0), inP = lvl(x, pa, 0);
  let line = `${name.padEnd(24)} ${(x.length / FS).toFixed(0).padStart(4)}s ${String(hi.patched).padStart(5)}  ${String(sp.length).padStart(4)}/${String(pa.length).padEnd(4)}          ${(db(inS) - db(inP)).toFixed(1).padStart(5)} dB |`;
  const outs = [];
  for (const lv of NR.NR_LEVELS) {
    const r = lv.wet === 0 ? { y: (await run(x, 0)).y } : await run(x, lv.wet);
    outs.push(r.y);
    if (lv.wet === 0) continue;
    const dP = db(lvl(r.y, pa, L)) - db(inP), dS = db(lvl(r.y, sp, L)) - db(inS);
    line += `  ${lv.name}: ${dP.toFixed(1)} / ${dS.toFixed(1)} / ${(dS - dP).toFixed(1)}`;
  }
  console.log(line);
  // A/B file: the 10 s with the most speech, Off -> Low -> Med -> High, 0.5 s of silence between
  let best = 0, bestN = -1; for (let s0 = 0; s0 + 100 <= nb; s0 += 10) { const n = sp.filter(b => b >= s0 && b < s0 + 100).length; if (n > bestN) { bestN = n; best = s0; } }
  const seg = Math.min(100, nb) * 1200, gap = FS / 2, ab = new Float32Array(4 * seg + 3 * gap);
  outs.forEach((y, i) => ab.set(y.subarray(best * 1200 + L, best * 1200 + L + seg), i * (seg + gap)));
  wr(`${outDir}/nr_ab_${name}.wav`, ab);
}
