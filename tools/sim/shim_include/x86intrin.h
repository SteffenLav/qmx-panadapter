/* Tab5 simulator shim. esp-dsp's dsp_common.h takes its "Linux target" branch
 * on any non-Xtensa, non-RISC-V build and includes <x86intrin.h> for __rdtsc.
 * WebAssembly has neither; the cycle counter is only used for benchmarks. */
#pragma once
static inline unsigned long long __rdtsc(void) { return 0; }
