// T18 probe: per-second level change, VAD and per-band change of the page NR on one WAV.
//   node tools/web_nr_probe.mjs <file.wav>   (12 kHz 16-bit mono, 44-byte header)
import { readFileSync } from "node:fs"; import { gunzipSync } from "node:zlib";
const html = readFileSync("main/net/www/index.html","utf8");
const NR = new Function(html.slice(html.indexOf("/* NR-BEGIN"), html.indexOf("/* NR-END")) + "\nreturn {nrCreate,nrProcess,NR_LATENCY_12K};")();
const wasm = gunzipSync(readFileSync("main/net/www/rnnoise.wasm.gz"));
const rd = p => { const b = readFileSync(p); const d = b.subarray(44); const x = new Float32Array(d.length/2); for (let i=0;i<x.length;i++) x[i]=d.readInt16LE(i*2)/32768; return x; };
const x = rd(process.argv[2]);
const nr = await NR.nrCreate(wasm); const y = new Float32Array(x.length); const vads=[];
for (let i=0;i<x.length;i+=1200){ y.set(NR.nrProcess(nr,x.subarray(i,i+1200),1),i); vads.push(nr.vad); }
const L=NR.NR_LATENCY_12K;
// per-second level change
let s=""; for (let t=0;t+1<x.length/12000;t++){ let a=0,b=0; for(let i=t*12000;i<(t+1)*12000-L;i++){a+=x[i]**2;b+=y[i+L]**2;} s+=(10*Math.log10(b/a)).toFixed(0)+" "; }
console.log("per-second dB:", s);
console.log("vad mean", (vads.reduce((a,b)=>a+b)/vads.length).toFixed(2), "vad>0.5 frac", (vads.filter(v=>v>0.5).length/vads.length).toFixed(2));
// per band via DFT on 1024 blocks
const bands=[[200,600],[600,1200],[1200,2000],[2000,2800],[2800,4000],[4000,6000]];
const pw=(z,off)=>{const out=bands.map(()=>0); for(let s0=12000*2+off;s0+1024<z.length;s0+=1024){ for(const [bi,[lo,hi]] of bands.entries()){ for(let f=Math.round(lo/12000*1024);f<hi/12000*1024;f+=2){ let re=0,im=0; for(let i=0;i<1024;i+=1){const w=0.5-0.5*Math.cos(2*Math.PI*i/1023); re+=z[s0+i]*w*Math.cos(2*Math.PI*f*i/1024); im+=z[s0+i]*w*Math.sin(2*Math.PI*f*i/1024);} out[bi]+=re*re+im*im; } } } return out;};
const px=pw(x.subarray(0,x.length-L),0), py=pw(y.subarray(L),0);
console.log(bands.map((b,i)=>`${b[0]}-${b[1]}: ${(10*Math.log10(py[i]/px[i])).toFixed(1)}`).join("  "));
