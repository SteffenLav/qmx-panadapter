#!/usr/bin/env python3
"""Tab5 simulator, step 0: measure the platform surface the firmware needs.

Compiles the APPLICATION sources (main/, components/ft8_lib, LVGL, esp-dsp's
portable C) to wasm objects with emcc, using the exact -I/-D flags from
build/compile_commands.json, then lists every symbol they use and none of
them define. That list is what a browser build has to supply (shim, port or
compile out). It answers "how big is the port" with a list instead of a guess.

Not a build of the simulator: nothing is linked, nothing runs.

FreeRTOS: the RISC-V port headers (inline asm) are swapped for IDF's own
FreeRTOS Linux-port headers, which is what a browser build would also use.

Usage (from the repo root, after a normal `idf.py build`):
    python tools/sim/step0_surface.py [--emsdk C:/dev/emsdk] [--jobs 12]
Writes build-sim/step0/report.md and build-sim/step0/undefined.txt.
"""
import argparse, collections, json, os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "build-sim" / "step0"

APP_PATTERNS = [
    r"/main/",
    r"/components/ft8_lib/",
    r"/managed_components/lvgl__lvgl/",
    r"/managed_components/espressif__esp-dsp/.*_ansi\.c$",
    r"/managed_components/espressif__esp-dsp/modules/common/",
]
DROP_FLAG_PREFIXES = ("-march", "-mabi", "-mlongcalls", "-mtune", "-mno-", "-fstrict-volatile-bitfields",
                      "-fno-shrink-wrap", "-fno-tree-switch-conversion", "-specs", "-nostartfiles",
                      "-Werror", "-fdiagnostics-color", "-fmacro-prefix-map", "-ffile-prefix-map",
                      "-fno-jump-tables", "-fno-builtin-memcpy", "-mdisable-hardware-atomics")


def norm(p):
    return p.replace("\\", "/")


def emcc_cmd(entry, obj, emcc):
    cmd = entry.get("command")
    args = shlex.split(cmd, posix=False) if cmd else list(entry["arguments"])
    keep = []
    i = 1
    while i < len(args):
        a = args[i].strip('"')
        if a in ("-o", "-c"):
            i += 2 if a == "-o" else 1
            continue
        if a.startswith(("-I", "-D", "-include")):
            if a == "-include":
                keep += [a, args[i + 1]]
                i += 2
                continue
            if norm(a).rstrip("/").endswith("build/config"):
                # The simulator gets its own sdkconfig.h: the Linux FreeRTOS port
                # is single-core, and the Tab5's CONFIG_FREERTOS_NUMBER_OF_CORES=2
                # made 87 files fail on "portGET_CORE_ID() must be defined".
                a = "-I" + norm(str(OUT / "config"))
            a = (a.replace("freertos/config/riscv/include", "freertos/config/linux/include")
                  .replace("FreeRTOS-Kernel/portable/riscv/include", "FreeRTOS-Kernel/portable/linux/include"))
            if "newlib/platform_include" in a:   # shadows Emscripten's libc headers
                i += 1
                continue
            keep.append(a)
        elif a.startswith("-std="):
            keep.append(a)
        i += 1
    shim = norm(str(REPO / "tools" / "sim" / "shim_include"))
    # -Diovec=iovec: lwIP's sockets.h defines struct iovec unless the NAME is a
    # macro; Emscripten already has the struct, so this keeps one definition.
    return [emcc, "-c", "-O0", "-w", "-Wno-error", "-ferror-limit=3", "-I" + shim, "-Diovec=iovec",
            "-Wno-implicit-function-declaration", "-Wno-int-conversion",
            "-Wno-incompatible-pointer-types", *keep, entry["file"], "-o", str(obj)]


def first_error(text):
    for line in text.splitlines():
        if " error:" in line or "fatal error:" in line:
            return line.strip()[:240]
    return text.strip().splitlines()[-1][:240] if text.strip() else "?"


def group_of(sym):
    m = re.match(r"(esp_[a-z0-9]+|x[A-Z][a-z]+|v[A-Z][a-z]+|ul[A-Z][a-z]+|ux[A-Z][a-z]+|pv[A-Z][a-z]+|pc[A-Z][a-z]+|"
                 r"heap_caps|nvs|usb_host|uac_host|cdc_acm|hid_host|sdmmc|gpio|i2c|i2s|bsp|httpd|mdns|lwip|"
                 r"[a-z]+_)", sym)
    return m.group(1) if m else sym


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emsdk", default="C:/dev/emsdk")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    a = ap.parse_args()
    emsdk = Path(a.emsdk)
    emcc = str(emsdk / "upstream" / "emscripten" / ("emcc.bat" if os.name == "nt" else "emcc"))
    nm = str(emsdk / "upstream" / "bin" / ("llvm-nm.exe" if os.name == "nt" else "llvm-nm"))
    cc = json.load(open(REPO / "build" / "compile_commands.json"))
    app = [e for e in cc if e["file"].endswith(".c") and any(re.search(p, norm(e["file"])) for p in APP_PATTERNS)]
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "obj").mkdir(exist_ok=True)
    (OUT / "config").mkdir(exist_ok=True)
    sdk = (REPO / "build" / "config" / "sdkconfig.h").read_text(encoding="utf-8")
    sdk = re.sub(r"#define CONFIG_FREERTOS_NUMBER_OF_CORES \d+", "#define CONFIG_FREERTOS_NUMBER_OF_CORES 1", sdk)
    sdk += "\n#define CONFIG_FREERTOS_UNICORE 1\n"
    (OUT / "config" / "sdkconfig.h").write_text(sdk, encoding="utf-8")

    def build(e):
        rel = norm(os.path.relpath(e["file"], REPO))
        obj = OUT / "obj" / (re.sub(r"[^A-Za-z0-9_.]", "_", rel) + ".o")
        cmd = emcc_cmd(e, obj, emcc)
        # Windows caps a command line at 8191 chars and main/'s -I list is longer:
        # every main/ file failed with "The command line is too long" on the
        # first run. Pass the arguments in a response file instead.
        rsp = obj.with_suffix(".rsp")
        def rsp_arg(x):
            # -DIDF_VER=\"v5.4.4\" arrives shell-escaped; un-escape it, then
            # quote for clang's GNU response-file syntax.
            x = x.replace('\\"', '"') if x.startswith("-D") else norm(x)
            return '"' + x.replace("\\", "\\\\").replace('"', '\\"') + '"'
        rsp.write_text("\n".join(rsp_arg(x) for x in cmd[1:]), encoding="utf-8")
        r = subprocess.run([cmd[0], "@" + str(rsp)], capture_output=True, text=True, cwd=e.get("directory", REPO))
        return rel, obj, r.returncode == 0 and obj.exists(), first_error(r.stderr) if r.returncode else ""

    with ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(build, app))

    ok = [r for r in results if r[2]]
    bad = [r for r in results if not r[2]]
    defined, undefined = set(), collections.defaultdict(set)
    for rel, obj, _, _ in ok:
        out = subprocess.run([nm, str(obj)], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "U":
                undefined[parts[1]].add(rel)
            elif len(parts) == 3 and parts[1] in "TDBRCW" + "tdbrc":
                defined.add(parts[2])
    need = {s: f for s, f in undefined.items() if s not in defined}
    libc = {"memcpy", "memset", "memmove", "memcmp", "strlen", "strcmp", "strncmp", "strcpy", "strncpy", "strchr",
            "strrchr", "strstr", "strtol", "strtoul", "strtod", "strtof", "atoi", "atof", "snprintf", "sprintf",
            "vsnprintf", "printf", "fprintf", "malloc", "calloc", "realloc", "free", "qsort", "abs", "fabs", "fabsf",
            "sqrt", "sqrtf", "sin", "sinf", "cos", "cosf", "atan2", "atan2f", "exp", "expf", "log", "logf", "log10",
            "log10f", "pow", "powf", "floor", "floorf", "ceil", "ceilf", "round", "roundf", "lroundf", "lround",
            "fopen", "fclose", "fread", "fwrite", "fseek", "ftell", "fgets", "fputs", "fflush", "remove", "rename",
            "time", "gmtime_r", "localtime_r", "mktime", "strftime", "gettimeofday", "isalpha", "isdigit", "isspace",
            "toupper", "tolower", "strtok_r", "sscanf", "strcasecmp", "strncasecmp", "strdup", "rand", "srand"}
    platform = {s: f for s, f in need.items() if s not in libc and not s.startswith(("__", "emscripten"))}

    by_group = collections.defaultdict(list)
    for s in platform:
        by_group[group_of(s)].append(s)
    lines = [f"# Tab5 simulator step 0 - platform surface",
             f"",
             f"Application sources: {len(app)}; compiled to wasm: {len(ok)}; failed: {len(bad)}.",
             f"Undefined after removing what the application itself defines: {len(need)}; "
             f"of those libc/compiler: {len(need) - len(platform)}; **platform: {len(platform)}**.",
             f"", "## Platform symbols by group (count, then symbols)", ""]
    for g, syms in sorted(by_group.items(), key=lambda kv: -len(kv[1])):
        users = set().union(*(platform[s] for s in syms))
        lines.append(f"- **{g}** {len(syms)} (used in {len(users)} files): " + ", ".join(sorted(syms)[:40])
                     + (" ..." if len(syms) > 40 else ""))
    lines += ["", "## Files that did not compile (first error)", ""]
    fail_dirs = collections.Counter(re.sub(r"/[^/]+$", "", r[0]) for r in bad)
    lines += [f"- {d}: {n}" for d, n in fail_dirs.most_common()]
    lines += [""] + [f"- `{r[0]}`: {r[3]}" for r in sorted(bad)]
    (OUT / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (OUT / "undefined.txt").write_text(
        "\n".join(f"{s}\t{len(f)}\t{','.join(sorted(f))[:300]}" for s, f in sorted(platform.items())) + "\n",
        encoding="utf-8")
    print(f"sources {len(app)}  compiled {len(ok)}  failed {len(bad)}  platform symbols {len(platform)}")
    print(f"report: {OUT / 'report.md'}")


if __name__ == "__main__":
    sys.exit(main())
