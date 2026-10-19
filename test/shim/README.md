# Host shim — run firmware `.c` files on the PC

Enough of ESP-IDF to compile and run a real `main/` source file under gcc on
this machine. It exists so a defect can be reproduced and a fix proven without
flashing, and without the QMX needing a power cycle.

**It links the real firmware source.** A harness that reimplements the logic
tests the reimplementation. Everything here is a stand-in for the *platform*,
never for the code under test.

## What is in it

| header | stands in for | notes |
|---|---|---|
| `esp_log.h` | `ESP_LOGE/W/I/D` | keeps `shim_log_errors`, so "it must say so" is testable |
| `esp_heap_caps.h` | `heap_caps_*` | caps ignored; `shim_heap_fail_after` starves an allocation on purpose |
| `esp_spiffs.h` | mount + info | "SPIFFS" is a scratch directory |
| `esp_err.h` | `esp_err_t` | |
| `freertos/` | mutex | single-threaded; `shim_lock_depth` catches a take/give imbalance |
| `settings.h` | the three upload cursors | **not** the real multi-kilobyte struct, on purpose — a copy would rot against it |
| `sd_archive.h`, `ui.h` | no SD, toast to stdout | |
| `shim_paths.h` | path redirect + MinGW gaps | see below |

## `shim_paths.h` is force-included, and only into the code under test

The firmware opens absolute paths like `/spiffs/qso.adi`. On the PC that is the
drive root. `shim_paths.h` macro-redirects `fopen` / `remove` / `rename` /
`stat` / `opendir` to rewrite a leading `/spiffs`, and fills in `gmtime_r` and
`fsync`, which MinGW does not have.

⛔ It must **not** reach `shim.c`, which calls the real `fopen` itself. So the
build compiles them in separate steps:

```sh
gcc $CFLAGS -c test/shim/shim.c -o build_host/shim.o
gcc $CFLAGS -include test/shim/shim_paths.h -c main/adif/adif_log.c -o build_host/adif_log.o
```

⛔ The firmware source is **not** changed to suit the harness. Making a path a
build option so the test could set it would put a test-only knob in shipping
code, and would stop the harness exercising the path handling as written.

## Users

- `test/adif_worked_cache_harness.c` — the worked-call cache
  (`sh test/build_adif_worked_cache.sh`), mutation-tested by
  `test/mutate_adif_worked_cache.py`.

Output goes to `build_host/`, which is git-ignored.
