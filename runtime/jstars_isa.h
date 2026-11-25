// JStarsReborn ISA compatibility: CPUs without the PS4 Jaguar's MOVBE/BMI1/LZCNT/F16C
// (Sandy Bridge, Ivy Bridge) stop with STATUS_ILLEGAL_INSTRUCTION inside the game's own
// libc.prx. See docs/ADR-005 ("ISA rewriting ... still open") and issue #7.
//
// What this file does:
//  - CPUID check at startup (missing sets are logged; see jsi_missing_*).
//  - A vectored-exception / SIGILL prehook that emulates the faulting MOVBE and BMI1
//    instructions (ANDN, BEXTR, BLSI, BLSMSK, BLSR) and resumes the game.
//  - LZCNT/TZCNT cannot be caught by a fault: without the set the CPU executes them as BSR/BSF
//    with no fault (silent wrong result). jsi_patch_code() therefore rewrites each one in the
//    executable segments to UD2 at load time and the fault handler emulates it from a side table.
//
// Flag semantics follow Intel SDM as validated against native execution in
// FelipePrado0/SpartanPort (GPL-2.0) src/isa_emulate.c: ANDN/BLSI/BLSMSK/BLSR define
// SF/ZF/CF/OF, BEXTR defines ZF/CF/OF, LZCNT/TZCNT define CF/ZF, MOVBE defines none.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t leaf1_ecx, leaf7_ebx, ext1_ecx; } JsiCpuWords;
JsiCpuWords jsi_cpu_words(void);
enum { JSI_SET_MOVBE = 1u << 0, JSI_SET_BMI1 = 1u << 1, JSI_SET_LZCNT = 1u << 2, JSI_SET_F16C = 1u << 3 };
// Missing rewritable sets as a JSI_SET_* mask; human-readable names in out (may be NULL).
unsigned jsi_missing_rewritable(JsiCpuWords w, char *out, size_t n);
// Missing baseline sets the game needs even with emulation (SSE4.2, POPCNT, AVX).
int jsi_missing_fatal(JsiCpuWords w, char *out, size_t n);
void jsi_log_startup(void);

typedef struct { uint64_t rflags; uint64_t gpr[16]; } JsiRegs; // x86 numbering; gpr[4] = rsp
enum { JSI_CF = 1u << 0, JSI_ZF = 1u << 6, JSI_SF = 1u << 7, JSI_OF = 1u << 11 };

// Emulate one instruction at ip (up to avail bytes, at least 15 when possible).
// Returns 1 when emulated (*out_len = instruction length), 0 when it is not a
// MOVBE/BMI1/LZCNT/TZCNT instruction, -1 when it is one but uses an encoding this
// file does not emulate (mn_out names it for the fault log).
int jsi_emulate(const uint8_t *ip, size_t avail, JsiRegs *regs, size_t *out_len,
                char *mn_out, size_t mn_cap);
// Decode-only version for the fault log: no register or memory side effects.
// Returns 1 with mnemonic+len for MOVBE/BMI1/BMI2/LZCNT/TZCNT/F16C, 0 otherwise.
int jsi_decode(const uint8_t *ip, size_t avail, char *mn_out, size_t mn_cap, size_t *out_len);

struct OsFault;
// VEH/SIGILL prehook: 1 when the fault was emulated (caller resumes), 0 otherwise.
int jsi_handle_fault(const struct OsFault *f);

// Load-time rewrite of LZCNT/TZCNT (see jstars_isa.c). Call once per executable segment, after
// relocations and before the segment is made read-only. Does nothing on CPUs that have the sets,
// or when the environment variable JSTARS_NO_LZCNT_PATCH is set. Returns the number of sites patched.
size_t jsi_patch_code(uint8_t *code, size_t size);
size_t jsi_patch_code_ex(uint8_t *code, size_t size, int need_lz, int need_tz); // forced, for tests
// Test hook: 1 when jsi_patch_code[_ex] rewrote the instruction at addr (side-table hit).
int jsi_is_patched(const void *addr);

// BMI1 fast path (see jstars_isa.c): rewrites the known hot ANDN/BEXTR/BLSR/BLSMSK/BLSI sites of J-Stars Victory
// VS+ US 01.02 into jumps to inline code placed in `jit` (writable now, make it executable afterwards; it must be
// within +-2 GiB of the image). Call after jsi_patch_code() and before the image is made read-only.
// The caller must only use it for that exact build. No-op when the CPU has BMI1 or JSTARS_NO_BMI_FASTPATH is set.
size_t jsi_fast_patch_hot(uint8_t *image, size_t image_bytes, uint8_t *jit, size_t jit_cap);
// Lower level, used by the tests: builds one replacement block / patches a given list of image offsets.
size_t jsi_fast_build(uint8_t *out, size_t cap, uint64_t out_addr, const uint8_t *insn, size_t avail,
                      uint64_t site_addr, size_t *len_out);
size_t jsi_fast_patch(uint8_t *image, size_t image_bytes, uint8_t *jit, size_t jit_cap,
                      const uint32_t *sites, size_t nsites);
