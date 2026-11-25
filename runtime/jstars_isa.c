// JStarsReborn ISA emulator: MOVBE + BMI1 (+ LZCNT/TZCNT decode) for pre-Haswell hosts.
// See jstars_isa.h for the contract. GPL-2.0-or-later.
#define _GNU_SOURCE
#include "jstars_isa.h"
#include "jstars_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#include <cpuid.h>
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/ucontext.h>
#include <ucontext.h>
#endif

void rt_logf(const char *fmt, ...);

JsiCpuWords jsi_cpu_words(void) {
    JsiCpuWords w = {0, 0, 0};
#if defined(__GNUC__)
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) w.leaf1_ecx = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) w.leaf7_ebx = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) w.ext1_ecx = c;
#else
    // MSVC fallback (the runtime exe itself builds with mingw GCC; this is for completeness).
    // Without <cpuid.h> we report nothing and the startup check warns instead of stopping.
#endif
    return w;
}

typedef struct { const char *name; int word; uint32_t bit; unsigned set; } JsiSet;
enum { W1 = 0, W7, WX };
static const JsiSet fatal_sets[] = {
    {"SSE4.2", W1, 1u << 20, 0}, {"POPCNT", W1, 1u << 23, 0}, {"AVX", W1, 1u << 28, 0},
};
static const JsiSet rewritable_sets[] = {
    {"MOVBE", W1, 1u << 22, JSI_SET_MOVBE}, {"F16C", W1, 1u << 29, JSI_SET_F16C},
    {"BMI1", W7, 1u << 3, JSI_SET_BMI1}, {"LZCNT", WX, 1u << 5, JSI_SET_LZCNT},
};

static int collect(const JsiSet *sets, size_t n, JsiCpuWords w, char *out, size_t size, unsigned *mask) {
    const uint32_t v[3] = {w.leaf1_ecx, w.leaf7_ebx, w.ext1_ecx};
    int miss = 0;
    size_t used = 0;
    *mask = 0;
    if (out && size) out[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        if (v[sets[i].word] & sets[i].bit) continue;
        if (out && used < size) {
            int k = snprintf(out + used, size - used, "%s%s", miss ? ", " : "", sets[i].name);
            if (k < 0) break;
            used += (size_t)k;
            if (used >= size) { used = size - 1; break; }
        }
        *mask |= sets[i].set;
        ++miss;
    }
    return miss;
}

unsigned jsi_missing_rewritable(JsiCpuWords w, char *out, size_t n) {
    unsigned m = 0;
    collect(rewritable_sets, sizeof rewritable_sets / sizeof *rewritable_sets, w, out, n, &m);
    return m;
}

int jsi_missing_fatal(JsiCpuWords w, char *out, size_t n) {
    unsigned m = 0;
    return collect(fatal_sets, sizeof fatal_sets / sizeof *fatal_sets, w, out, n, &m);
}

void jsi_log_startup(void) {
    JsiCpuWords w = jsi_cpu_words();
    char fatal[128] = {0}, rw[128] = {0};
    int nf = jsi_missing_fatal(w, fatal, sizeof fatal);
    unsigned rwmask = jsi_missing_rewritable(w, rw, sizeof rw);
    if (nf) {
        rt_logf("CPU check: FATAL missing %s (needs SSE4.2/POPCNT/AVX: Sandy Bridge 2011+ or newer)", fatal);
    } else if (rwmask) {
        rt_logf("CPU check: missing %s; MOVBE/BMI1 faults will be emulated; LZCNT/TZCNT sites are rewritten at load (see jsi_patch_code)", rw);
    } else {
        rt_logf("CPU check: complete (MOVBE BMI1 LZCNT F16C present)");
    }
}

// ---- decoder/emulator -------------------------------------------------------

enum { NOREG = 0xFF };
enum { K_MOVBE_LOAD = 1, K_MOVBE_STORE, K_LZCNT, K_TZCNT, K_ANDN, K_BEXTR, K_BLSI, K_BLSMSK, K_BLSR };

static uint64_t width_mask(int w) { return w == 64 ? ~0ull : (1ull << w) - 1; }

static uint64_t zf_sf(int width, uint64_t r) {
    return (r == 0 ? (uint64_t)JSI_ZF : 0) | ((r >> (width - 1)) & 1 ? (uint64_t)JSI_SF : 0);
}

static uint64_t bswap_for(int width, uint64_t v) {
    if (width == 16) return (uint64_t)__builtin_bswap16((uint16_t)v);
    if (width == 32) return (uint64_t)__builtin_bswap32((uint32_t)v);
    return __builtin_bswap64(v);
}

// Decoded memory operand.
typedef struct {
    int is_mem;      // 0 = register direct (no emulation needed here)
    int rip_rel;     // RIP-relative: addr = rip + len + disp
    int base;        // 0..15 or NOREG
    int index;       // 0..15 or NOREG
    int scale;       // 1/2/4/8
    int64_t disp;
    int addr32;      // 0x67 addressing: use low 32 bits of base/index
    size_t len;      // total instruction length once disp is known
} MemDec;

// Decode ModRM (+SIB+disp) at bytes[modrm] with R/X/B extensions.
// start = instruction start index (for length), rip = fault RIP (for RIP-relative).
// Returns 0 on truncation/invalid, else 1 with *m filled (is_mem=0 for mod==3).
static int decode_modrm(const uint8_t *b, size_t avail, size_t modrm, int R, int X, int B,
                        int addr32, uintptr_t rip, size_t start, MemDec *m, int *reg_out) {
    (void)rip; // only mem_addr needs it; decoding stores disp for later
    if (modrm >= avail) return 0;
    uint8_t modrmb = b[modrm];
    int mod = modrmb >> 6, reg = ((modrmb >> 3) & 7) | (R << 3), rm = (modrmb & 7) | (B << 3);
    if (reg_out) *reg_out = reg;
    m->is_mem = (mod != 3);
    m->rip_rel = 0; m->base = NOREG; m->index = NOREG; m->scale = 1; m->disp = 0; m->addr32 = addr32;
    if (!m->is_mem) { m->len = modrm + 1 - start; return 1; }
    size_t p = modrm + 1;
    int base = -1, index = -1, scale = 1;
    if ((modrmb & 7) == 4) { // SIB
        if (p >= avail) return 0;
        uint8_t sib = b[p++];
        scale = 1 << (sib >> 6);
        index = ((sib >> 3) & 7) | (X << 3);
        base = (sib & 7) | (B << 3);
        if (base == 5 && mod == 0) base = -1; // SIB base 101 (RBP/R13 without B): disp32, no base
        if ((sib >> 3 & 7) == 4 && X == 0) index = -1; // ESP/RSP cannot be index (without X)
        // With X==1, index 4|8=RSP/R12 is a valid index register.
    } else {
        base = rm;
    }
    size_t disp_len = 0;
    if (rm == 5 && mod == 0 && (modrmb & 7) != 4) {
        // mod==00, r/m==101 (RBP/RIP without B): RIP-relative. With B==1 rm==13 ([R13]+disp32).
        m->rip_rel = 1; disp_len = 4;
    } else if ((modrmb & 7) == 5 && mod == 0) {
        disp_len = 4; // REX.B: [R13]+disp32 (base already holds 13, not RIP-relative)
    } else if (mod == 1) {
        disp_len = 1;
    } else if (mod == 2) {
        disp_len = 4;
    } else if (mod == 0 && base == -1) {
        disp_len = 4; // SIB with no base
    }
    if (p + disp_len > avail) return 0;
    int64_t disp = 0;
    if (disp_len == 1) disp = (int8_t)b[p];
    else if (disp_len == 4) disp = (int32_t)((uint32_t)b[p] | (uint32_t)b[p+1] << 8 | (uint32_t)b[p+2] << 16 | (uint32_t)b[p+3] << 24);
    m->len = p + disp_len - start;
    if (m->rip_rel) {
        m->disp = disp; // resolved later as rip + len + disp
        return 1;
    }
    if (base >= 0) m->base = base;
    if (index >= 0) m->index = index;
    m->scale = scale;
    m->disp = disp;
    return 1;
}

static uintptr_t mem_addr(const MemDec *m, const JsiRegs *r, uintptr_t rip) {
    if (m->rip_rel) return rip + m->len + (uint64_t)(int64_t)m->disp;
    uint64_t a = (uint64_t)(uint64_t)m->disp;
    if (m->base != NOREG) {
        uint64_t bv = r->gpr[m->base];
        if (m->addr32) bv = (uint32_t)bv;
        a += bv;
    }
    if (m->index != NOREG) {
        uint64_t iv = r->gpr[m->index];
        if (m->addr32) iv = (uint32_t)iv;
        a += iv * (uint64_t)m->scale;
    }
    if (m->addr32) a = (uint32_t)a;
    return (uintptr_t)a;
}

static uint64_t read_reg(const JsiRegs *r, int reg, int width) {
    if (reg == NOREG) return 0;
    return r->gpr[reg] & width_mask(width);
}

static void write_dst(JsiRegs *r, int dst, int width, uint64_t v, int is_mem_dst, uintptr_t addr) {
    if (is_mem_dst) {
        if (width == 16) { uint16_t t = (uint16_t)v; os_safe_write((void *)addr, &t, 2); }
        else if (width == 32) { uint32_t t = (uint32_t)v; os_safe_write((void *)addr, &t, 4); }
        else os_safe_write((void *)addr, &v, 8);
        return;
    }
    if (width == 64) r->gpr[dst] = v;
    else if (width == 32) r->gpr[dst] = (uint32_t)v;
    else r->gpr[dst] = (r->gpr[dst] & ~0xffffull) | (v & 0xffffull);
}

static uint64_t read_mem(const MemDec *m, const JsiRegs *r, uintptr_t rip, int width, int *ok) {
    uintptr_t a = mem_addr(m, r, rip);
    uint64_t v = 0;
    size_t want = width == 16 ? 2 : width == 32 ? 4 : 8;
    *ok = os_safe_read(&v, (const void *)a, want) == want;
    // os_safe_read writes the first bytes; mask off anything beyond want on short reads.
    if (want == 2) v &= 0xffffull;
    else if (want == 4) v &= 0xffffffffull;
    return v;
}

static void set_flags(JsiRegs *r, uint64_t mask, uint64_t v) {
    r->rflags = (r->rflags & ~mask) | (v & mask);
}

static void mn_copy(char *out, size_t cap, const char *s) {
    if (!out || !cap) return;
    snprintf(out, cap, "%s", s);
}

int jsi_emulate(const uint8_t *ip, size_t avail, JsiRegs *regs, size_t *out_len,
                char *mn_out, size_t mn_cap) {
    if (avail == 0) return 0;
    extern uintptr_t jsi_fault_rip_tls;
    uintptr_t rip = jsi_fault_rip_tls;
    // ---- VEX path (BMI1/BMI2/F16C) ----
    if (ip[0] == 0xC4) {
        if (avail < 5) return 0; // C4 + P1 + P2 + op + ModRM: 5 bytes minimum (register form)
        uint8_t p1 = ip[1], p2 = ip[2], op = ip[3];
        int m = p1 & 0x1F;
        int R = ((p1 >> 7) & 1) ^ 1, X = ((p1 >> 6) & 1) ^ 1, B = ((p1 >> 5) & 1) ^ 1;
        int W = (p2 >> 7) & 1, vvvv = ((~p2) >> 3) & 0xF, L = (p2 >> 2) & 1, pp = p2 & 3;
        if (m == 3) { mn_copy(mn_out, mn_cap, "F16C"); return -1; } // 0F3A: VCVTPH2PS/VCVTPS2PH
        if (m != 2) return 0; // not 0F38: some other VEX (AVX etc.), not ours
        if (L != 0 || pp != 0) return 0;
        if (op == 0xF5 || op == 0xF6) { mn_copy(mn_out, mn_cap, op == 0xF5 ? "PDEP" : "PEXT"); return -1; }
        if (op == 0xF2) { // ANDN dest, src1, src2
            int dest = 0;
            MemDec mem = {0};
            if (!decode_modrm(ip, avail, 4, R, X, B, 0, rip, 0, &mem, &dest)) return 0;
            int width = W ? 64 : 32;
            uint64_t s1 = read_reg(regs, vvvv, width), s2 = 0;
            if (!mem.is_mem) s2 = read_reg(regs, (ip[4] & 7) | (B << 3), width);
            else { int ok = 0; s2 = read_mem(&mem, regs, rip, width, &ok); if (!ok) return 0; }
            uint64_t res = (~s1 & s2) & width_mask(width);
            if (!mem.is_mem) {
                // dest is a register; mem.is_mem==0 means r/m was a register, dest=reg field.
                write_dst(regs, dest, width, res, 0, 0);
            } else {
                write_dst(regs, dest, width, res, 0, 0);
            }
            set_flags(regs, (uint64_t)(JSI_SF | JSI_ZF | JSI_CF | JSI_OF), zf_sf(width, res));
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, "ANDN");
            return 1;
        }
        if (op == 0xF7) { // BEXTR dest, src, control
            int dest = 0;
            MemDec mem = {0};
            if (!decode_modrm(ip, avail, 4, R, X, B, 0, rip, 0, &mem, &dest)) return 0;
            int width = W ? 64 : 32;
            uint64_t src = 0;
            if (!mem.is_mem) src = read_reg(regs, (ip[4] & 7) | (B << 3), width);
            else { int ok = 0; src = read_mem(&mem, regs, rip, width, &ok); if (!ok) return 0; }
            uint64_t ctrl = read_reg(regs, vvvv, width);
            uint64_t start = ctrl & 0xFF, len = (ctrl >> 8) & 0xFF, res = 0;
            if (start < (uint64_t)width && len != 0) {
                if (len > (uint64_t)width - start) len = (uint64_t)width - start;
                res = (src >> start) & (len == 64 ? ~0ull : ((len == 64 ? ~0ull : (1ull << len) - 1)));
            }
            write_dst(regs, dest, width, res, 0, 0);
            set_flags(regs, (uint64_t)(JSI_ZF | JSI_CF | JSI_OF), res == 0 ? (uint64_t)JSI_ZF : 0);
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, "BEXTR");
            return 1;
        }
        if (op == 0xF3) { // Group 17: dest=vvvv, src=r/m
            if (avail < 5) return 0;
            int ext = (ip[4] >> 3) & 7;
            const char *mn = ext == 1 ? "BLSR" : ext == 2 ? "BLSMSK" : ext == 3 ? "BLSI" : NULL;
            if (!mn) return 0;
            if (R != 0) return 0; // reg field is the fixed /1 /2 /3 extension
            int dest = vvvv;
            MemDec mem = {0};
            int dummy = 0;
            if (!decode_modrm(ip, avail, 4, 0, X, B, 0, rip, 0, &mem, &dummy)) return 0;
            int width = W ? 64 : 32;
            uint64_t src = 0;
            if (!mem.is_mem) src = read_reg(regs, (ip[4] & 7) | (B << 3), width);
            else { int ok = 0; src = read_mem(&mem, regs, rip, width, &ok); if (!ok) return 0; }
            src &= width_mask(width);
            uint64_t res = 0, fl = 0;
            if (ext == 3) { res = (src & (0 - src)) & width_mask(width); fl = zf_sf(width, res) | (src != 0 ? (uint64_t)JSI_CF : 0); }
            else if (ext == 2) { res = (src ^ (src - 1)) & width_mask(width); fl = (zf_sf(width, res) & (uint64_t)JSI_SF) | (src == 0 ? (uint64_t)JSI_CF : 0); }
            else { res = (src & (src - 1)) & width_mask(width); fl = zf_sf(width, res) | (src == 0 ? (uint64_t)JSI_CF : 0); }
            write_dst(regs, dest, width, res, 0, 0);
            set_flags(regs, (uint64_t)(JSI_SF | JSI_ZF | JSI_CF | JSI_OF), fl);
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, mn);
            return 1;
        }
        return 0;
    }
    if (ip[0] == 0xC5) return 0; // 2-byte VEX only encodes 0F map: AVX, never BMI1/MOVBE.

    // ---- legacy path: prefixes, then 0F 38 F0/F1 (MOVBE) or F3 0F BC/BD (TZCNT/LZCNT) ----
    {
        size_t p = 0;
        int has66 = 0, addr32 = 0, rep = 0, lock = 0, seg = 0;
        for (int k = 0; k < 5 && p < avail; ++k) {
            uint8_t c = ip[p];
            if (c == 0x66) { has66 = 1; ++p; }
            else if (c == 0x67) { addr32 = 1; ++p; }
            else if (c == 0xF2 || c == 0xF3) { rep = c; ++p; }
            else if (c == 0xF0) { lock = 1; ++p; }
            else if (c == 0x2E || c == 0x3E || c == 0x26 || c == 0x36 || c == 0x64 || c == 0x65) { seg = 1; ++p; }
            else break;
        }
        int rex = 0, W = 0, R = 0, X = 0, B = 0;
        if (p < avail && (ip[p] & 0xF0) == 0x40) {
            rex = ip[p++];
            W = (rex >> 3) & 1; R = (rex >> 2) & 1; X = (rex >> 1) & 1; B = rex & 1;
        }
        if (p + 1 >= avail) return 0;
        if (ip[p] != 0x0F) return 0;
        // MOVBE: 0F 38 F0/F1
        if (p + 2 < avail && ip[p+1] == 0x38 && (ip[p+2] == 0xF0 || ip[p+2] == 0xF1)) {
            if (lock || seg) { mn_copy(mn_out, mn_cap, "MOVBE"); return -1; }
            if (rep) return 0;
            int is_load = ip[p+2] == 0xF0;
            MemDec mem = {0};
            int reg = 0;
            (void)rex;
            if (!decode_modrm(ip, avail, p + 3, R, X, B, addr32, rip, 0, &mem, &reg)) return 0;
            if (!mem.is_mem) { mn_copy(mn_out, mn_cap, "MOVBE"); return -1; } // mod==3 is #UD
            int width = has66 ? 16 : (W ? 64 : 32);
            if (is_load) {
                int ok = 0;
                uint64_t v = read_mem(&mem, regs, rip, width, &ok);
                if (!ok) return 0;
                write_dst(regs, reg, width, bswap_for(width, v), 0, 0);
            } else {
                uint64_t v = read_reg(regs, reg, width);
                uintptr_t a = mem_addr(&mem, regs, rip);
                if (width == 16) { uint16_t t = (uint16_t)bswap_for(16, v); if (os_safe_write((void *)a, &t, 2) != 2) return 0; }
                else if (width == 32) { uint32_t t = (uint32_t)bswap_for(32, v); if (os_safe_write((void *)a, &t, 4) != 4) return 0; }
                else { uint64_t t = bswap_for(64, v); if (os_safe_write((void *)a, &t, 8) != 8) return 0; }
            }
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, "MOVBE");
            return 1;
        }
        // LZCNT/TZCNT: F3 0F BD/BC (2 opcode bytes: ModRM sits at p+2, unlike MOVBE's p+3)
        if (p + 2 < avail && rep == 0xF3 && ip[p] == 0x0F && (ip[p+1] == 0xBC || ip[p+1] == 0xBD)) {
            int is_lz = ip[p+1] == 0xBD;
            if (lock || seg) { mn_copy(mn_out, mn_cap, is_lz ? "LZCNT" : "TZCNT"); return -1; }
            MemDec mem = {0};
            int dest = 0;
            if (!decode_modrm(ip, avail, p + 2, R, X, B, addr32, rip, 0, &mem, &dest)) return 0;
            int width = has66 ? 16 : (W ? 64 : 32);
            uint64_t src = 0;
            if (!mem.is_mem) src = read_reg(regs, (ip[p+2] & 7) | (B << 3), width);
            else { int ok = 0; src = read_mem(&mem, regs, rip, width, &ok); if (!ok) return 0; }
            src &= width_mask(width);
            uint64_t res;
            if (src == 0) res = (uint64_t)width;
            else if (is_lz) res = (uint64_t)__builtin_clzll(src) - (64 - (uint64_t)width);
            else res = (uint64_t)__builtin_ctzll(src);
            write_dst(regs, dest, width, res, 0, 0);
            set_flags(regs, (uint64_t)(JSI_CF | JSI_ZF),
                      (src == 0 ? (uint64_t)JSI_CF : 0) | (res == 0 ? (uint64_t)JSI_ZF : 0));
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, is_lz ? "LZCNT" : "TZCNT");
            return 1;
        }
        return 0;
    }
}

// Decode-only for the fault log: no register or memory effects.
int jsi_decode(const uint8_t *ip, size_t avail, char *mn_out, size_t mn_cap, size_t *out_len) {
    if (avail == 0) return 0;
    if (ip[0] == 0xC4) {
        if (avail < 5) return 0;
        uint8_t p1 = ip[1], p2 = ip[2], op = ip[3];
        int m = p1 & 0x1F, L = (p2 >> 2) & 1, pp = p2 & 3;
        if (m == 3) {
            MemDec mem = {0};
            int R = ((p1 >> 7) & 1) ^ 1, X = ((p1 >> 6) & 1) ^ 1, B = ((p1 >> 5) & 1) ^ 1;
            int dummy = 0;
            if (!decode_modrm(ip, avail, 4, R, X, B, 0, 0, 0, &mem, &dummy)) return 0;
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, "F16C");
            return 1;
        }
        if (m != 2 || L != 0 || pp != 0) return 0;
        if (op == 0xF5) { MemDec mem = {0}; int R = ((p1 >> 7) & 1) ^ 1, X = ((p1 >> 6) & 1) ^ 1, B = ((p1 >> 5) & 1) ^ 1, d = 0;
            if (!decode_modrm(ip, avail, 4, R, X, B, 0, 0, 0, &mem, &d)) return 0;
            if (out_len) *out_len = mem.len; mn_copy(mn_out, mn_cap, "PDEP"); return 1; }
        if (op == 0xF6) { MemDec mem = {0}; int R = ((p1 >> 7) & 1) ^ 1, X = ((p1 >> 6) & 1) ^ 1, B = ((p1 >> 5) & 1) ^ 1, d = 0;
            if (!decode_modrm(ip, avail, 4, R, X, B, 0, 0, 0, &mem, &d)) return 0;
            if (out_len) *out_len = mem.len; mn_copy(mn_out, mn_cap, "PEXT"); return 1; }
        if (op == 0xF2 || op == 0xF7) {
            MemDec mem = {0};
            int R = ((p1 >> 7) & 1) ^ 1, X = ((p1 >> 6) & 1) ^ 1, B = ((p1 >> 5) & 1) ^ 1, d = 0;
            if (!decode_modrm(ip, avail, 4, R, X, B, 0, 0, 0, &mem, &d)) return 0;
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, op == 0xF2 ? "ANDN" : "BEXTR");
            return 1;
        }
        if (op == 0xF3) {
            if (avail < 5) return 0;
            int ext = (ip[4] >> 3) & 7;
            const char *mn = ext == 1 ? "BLSR" : ext == 2 ? "BLSMSK" : ext == 3 ? "BLSI" : NULL;
            if (!mn) return 0;
            MemDec mem = {0};
            int X = ((p1 >> 6) & 1) ^ 1, B = ((p1 >> 5) & 1) ^ 1, d = 0;
            if (!decode_modrm(ip, avail, 4, 0, X, B, 0, 0, 0, &mem, &d)) return 0;
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, mn);
            return 1;
        }
        return 0;
    }
    if (ip[0] == 0xC5) return 0;
    {
        size_t p = 0;
        int has66 = 0, addr32 = 0, rep = 0;
        for (int k = 0; k < 5 && p < avail; ++k) {
            uint8_t c = ip[p];
            if (c == 0x66) { has66 = 1; ++p; }
            else if (c == 0x67) { addr32 = 1; ++p; }
            else if (c == 0xF2 || c == 0xF3) { rep = c; ++p; }
            else if (c == 0xF0 || c == 0x2E || c == 0x3E || c == 0x26 || c == 0x36 || c == 0x64 || c == 0x65) { ++p; }
            else break;
        }
        int W = 0, R = 0, X = 0, B = 0;
        if (p < avail && (ip[p] & 0xF0) == 0x40) {
            uint8_t rex = ip[p++];
            W = (rex >> 3) & 1; R = (rex >> 2) & 1; X = (rex >> 1) & 1; B = rex & 1;
        }
        (void)has66;
        if (p + 2 >= avail || ip[p] != 0x0F) return 0;
        if (ip[p+1] == 0x38 && (ip[p+2] == 0xF0 || ip[p+2] == 0xF1)) {
            MemDec mem = {0};
            int d = 0;
            if (!decode_modrm(ip, avail, p + 3, R, X, B, addr32, 0, 0, &mem, &d)) return 0;
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, "MOVBE");
            return 1;
        }
        if (rep == 0xF3 && (ip[p+1] == 0xBC || ip[p+1] == 0xBD)) {
            MemDec mem = {0};
            int d = 0;
            if (!decode_modrm(ip, avail, p + 2, R, X, B, addr32, 0, 0, &mem, &d)) return 0;
            if (out_len) *out_len = mem.len;
            mn_copy(mn_out, mn_cap, ip[p+1] == 0xBD ? "LZCNT" : "TZCNT");
            return 1;
        }
        (void)W;
        return 0;
    }
}

// ---- LZCNT/TZCNT load-time rewrite ------------------------------------------------------------
// Without LZCNT (and BMI1 for TZCNT) the CPU runs F3 0F BD / F3 0F BC as BSR / BSF: no fault, but a
// different result (BSR returns the index of the top set bit, LZCNT the count of leading zeros; for a
// zero input both BSR and BSF leave the destination undefined). A fault handler can never see them,
// so at load time every such instruction in an executable segment is overwritten with UD2 (0F 0B).
// The original bytes are kept in a table keyed by the instruction address; jsi_handle_fault() looks the
// faulting RIP up there and emulates the saved bytes with jsi_emulate(), then resumes after them.
typedef struct { uintptr_t addr; uint8_t orig[15]; } JsiSite;
enum { JSI_MAX_SITES = 1024 };
static JsiSite g_sites[JSI_MAX_SITES];
static size_t g_nsites;

static const JsiSite *jsi_find_site(uintptr_t rip) {
    size_t lo = 0, hi = g_nsites;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_sites[mid].addr < rip) lo = mid + 1; else hi = mid;
    }
    return (lo < g_nsites && g_sites[lo].addr == rip) ? &g_sites[lo] : NULL;
}

static int jsi_add_site(uintptr_t addr, const uint8_t *orig15) {
    if (g_nsites >= JSI_MAX_SITES) return 0;
    size_t pos = 0;
    while (pos < g_nsites && g_sites[pos].addr < addr) ++pos;
    memmove(&g_sites[pos + 1], &g_sites[pos], (g_nsites - pos) * sizeof g_sites[0]);
    g_sites[pos].addr = addr;
    memcpy(g_sites[pos].orig, orig15, 15);
    ++g_nsites;
    return 1;
}

static int jsi_is_prefix_byte(uint8_t c) {
    return c == 0x66 || c == 0x67 || c == 0xF0 || c == 0xF2 || c == 0xF3 ||
           c == 0x2E || c == 0x36 || c == 0x3E || c == 0x26 || c == 0x64 || c == 0x65;
}

size_t jsi_patch_code_ex(uint8_t *code, size_t size, int need_lz, int need_tz) {
    size_t nlz = 0, ntz = 0, skipped = 0;
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (code[i] != 0xF3) continue;
        size_t j = i + 1;
        if ((code[j] & 0xF0) == 0x40) ++j;       // optional REX
        if (j + 2 >= size || code[j] != 0x0F) continue;
        int is_lz = code[j + 1] == 0xBD, is_tz = code[j + 1] == 0xBC;
        if (!((is_lz && need_lz) || (is_tz && need_tz))) continue;
        uint8_t buf[15];
        size_t n = size - i < sizeof buf ? size - i : sizeof buf;
        memset(buf, 0x90, sizeof buf);
        memcpy(buf, code + i, n);
        char mn[16] = {0};
        size_t len = 0;
        if (!jsi_decode(buf, n, mn, sizeof mn, &len) || len < 4 || len > 15) { ++skipped; continue; }
        // A byte before F3 that looks like a legacy prefix may be a real prefix (then the instruction starts
        // there and the fault RIP is that earlier byte) or just the tail of the previous instruction (then the
        // fault RIP is the F3 itself). jsi_handle_fault() resolves both, so every site is patched.
        if (!jsi_add_site((uintptr_t)(code + i), buf)) {
            rt_logf("ISA patch: site table full (%u); remaining LZCNT/TZCNT left untouched", (unsigned)JSI_MAX_SITES);
            ++skipped;
            break;
        }
        code[i] = 0x0F;
        code[i + 1] = 0x0B;                       // UD2
        if (is_lz) ++nlz; else ++ntz;
        i += len - 1;
    }
    if (nlz || ntz || skipped)
        rt_logf("ISA patch: %u LZCNT + %u TZCNT rewritten to UD2 in [%p, +0x%x), %u skipped",
                (unsigned)nlz, (unsigned)ntz, (void *)code, (unsigned)size, (unsigned)skipped);
    return nlz + ntz;
}

size_t jsi_patch_code(uint8_t *code, size_t size) {
    if (getenv("JSTARS_NO_LZCNT_PATCH")) return 0;   // A/B switch for diagnosis
    JsiCpuWords w = jsi_cpu_words();
    int need_lz = !(w.ext1_ecx & (1u << 5));          // CPUID 80000001h:ECX.LZCNT
    int need_tz = !(w.leaf7_ebx & (1u << 3));         // CPUID 7:EBX.BMI1 (TZCNT)
    if (!need_lz && !need_tz) return 0;
    return jsi_patch_code_ex(code, size, need_lz, need_tz);
}

// ---- BMI1 fast path: ANDN / BEXTR / BLSR / BLSMSK / BLSI without a fault -------------------------------
// Emulating these through exceptions costs thousands of cycles per instruction and the hot loops of the game
// execute hundreds of thousands per second. For the sites listed in k_hot_sites (observed in real runs of
// J-Stars Victory VS+ US 01.02) the instruction is replaced at load time by "jmp rel32" to a short block that
// computes the same result and the same defined flags with ordinary instructions, then jumps back. Memory
// operands are first loaded into the destination register with the original ModRM/SIB/disp bytes.
// Forms that are not handled (RIP-relative, rsp as a register operand, register aliasing that needs a scratch
// we cannot spare) are left alone: they keep going through the fault handler.
typedef struct { uint8_t *p; uint8_t *end; int err; } JsiAsm;

static void a8(JsiAsm *a, unsigned v) { if (a->p >= a->end) { a->err = 1; return; } *a->p++ = (uint8_t)v; }
static void a32(JsiAsm *a, uint32_t v) { for (int i = 0; i < 4; ++i) a8(a, (v >> (8 * i)) & 0xFF); }
static void a_rex(JsiAsm *a, int w, int r, int x, int b) { a8(a, 0x40 | (w ? 8 : 0) | ((r & 1) << 2) | ((x & 1) << 1) | (b & 1)); }
// "op r/m, reg": 89 mov, 21 and, 31 xor, 85 test
static void a_op_rr(JsiAsm *a, int w, unsigned opc, int rm, int reg) {
    a_rex(a, w, reg >> 3, 0, rm >> 3); a8(a, opc); a8(a, 0xC0 | ((reg & 7) << 3) | (rm & 7));
}
// "op r/m" with an opcode extension: F7 /2 not, F7 /3 neg, FF /1 dec, D3 /4 shl cl, D3 /5 shr cl
static void a_grp(JsiAsm *a, int w, unsigned opc, int ext, int rm) {
    a_rex(a, w, 0, 0, rm >> 3); a8(a, opc); a8(a, 0xC0 | (ext << 3) | (rm & 7));
}
// "op r/m, imm8": C1 /5 shr, 83 /7 cmp, 83 /1 or
static void a_grp_i8(JsiAsm *a, int w, unsigned opc, int ext, int rm, unsigned imm) {
    a_grp(a, w, opc, ext, rm); a8(a, imm);
}
static void a_lea_dec(JsiAsm *a, int w, int dst, int src) {           // lea dst,[src-1]
    a_rex(a, w, dst >> 3, 0, src >> 3); a8(a, 0x8D);
    if ((src & 7) == 4) { a8(a, 0x44 | ((dst & 7) << 3)); a8(a, 0x24); }
    else a8(a, 0x40 | ((dst & 7) << 3) | (src & 7));
    a8(a, 0xFF);
}
static void a_rsp_adj(JsiAsm *a, int32_t d) { a8(a, 0x48); a8(a, 0x8D); a8(a, 0xA4); a8(a, 0x24); a32(a, (uint32_t)d); } // lea rsp,[rsp+d]: leaves flags alone
static void a_push(JsiAsm *a, int r) { if (r >= 8) a8(a, 0x41); a8(a, 0x50 + (r & 7)); }
static void a_pop(JsiAsm *a, int r) { if (r >= 8) a8(a, 0x41); a8(a, 0x58 + (r & 7)); }
static void a_mov_imm32(JsiAsm *a, int r, uint32_t v) { if (r >= 8) a8(a, 0x41); a8(a, 0xB8 + (r & 7)); a32(a, v); }
static void a_movzx8(JsiAsm *a, int dst, int src) {                   // movzx dst32, src8 (REX always present: sil/dil/r8b.. are intended)
    a_rex(a, 0, dst >> 3, 0, src >> 3); a8(a, 0x0F); a8(a, 0xB6); a8(a, 0xC0 | ((dst & 7) << 3) | (src & 7));
}
static uint8_t *a_j8(JsiAsm *a, unsigned opc) { a8(a, opc); a8(a, 0); return a->err ? NULL : a->p - 1; }
static void a_bind(JsiAsm *a, uint8_t *rel) {
    if (!rel || a->err) return;
    long d = (long)(a->p - (rel + 1));
    if (d > 127) { a->err = 1; return; }
    *rel = (uint8_t)d;
}

typedef struct {
    int op;                  // 0 ANDN, 1 BEXTR, 2 BLSR, 3 BLSMSK, 4 BLSI
    int w;                   // 1 = 64-bit operands
    int dst, a;              // ANDN: a = first source (inverted); BEXTR: a = control; BLS*: dst is VEX.vvvv, a unused
    int rm;                  // register operand, or -1 for a memory operand
    int vx, vb;              // VEX.X / VEX.B as REX bits (memory form)
    uint8_t mem[6];          // ModRM, SIB, displacement of the memory operand (verbatim)
    size_t memlen, len;
} JsiBmi;

static int jsi_bmi_parse(const uint8_t *p, size_t avail, JsiBmi *o) {
    if (avail < 5 || p[0] != 0xC4) return 0;
    unsigned b1 = p[1], b2 = p[2], opc = p[3], modrm = p[4];
    if ((b1 & 0x1F) != 2 || (b2 & 7) != 0) return 0;      // 0F38 map, L=0, pp=none
    int R = !(b1 & 0x80), X = !(b1 & 0x40), B = !(b1 & 0x20);
    int vvvv = (~b2 >> 3) & 15;
    int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    memset(o, 0, sizeof *o);
    o->w = (b2 >> 7) & 1;
    switch (opc) {
        case 0xF2: o->op = 0; o->dst = reg | (R << 3); o->a = vvvv; break;
        case 0xF7: o->op = 1; o->dst = reg | (R << 3); o->a = vvvv; break;
        case 0xF3:
            if (reg < 1 || reg > 3) return 0;
            o->op = 1 + reg; o->dst = vvvv; o->a = -1; break;
        default: return 0;
    }
    size_t n = 5;
    if (mod == 3) { o->rm = rm | (B << 3); }
    else {
        if (mod == 0 && rm == 5) return 0;                // RIP-relative: not handled
        size_t extra = 0;
        if (rm == 4) {
            if (avail < 6) return 0;
            extra += 1;
            if (mod == 0 && (p[5] & 7) == 5) extra += 4;  // SIB without base: disp32
        }
        if (mod == 1) extra += 1; else if (mod == 2) extra += 4;
        n = 5 + extra;
        if (avail < n || 1 + extra > sizeof o->mem) return 0;
        o->rm = -1; o->vx = X; o->vb = B;
        memcpy(o->mem, p + 4, 1 + extra);
        o->memlen = 1 + extra;
    }
    o->len = n;
    return 1;
}

static int jsi_pick_scratch(unsigned used) {
    static const int order[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 5};
    for (size_t i = 0; i < sizeof order / sizeof *order; ++i)
        if (!(used & (1u << order[i]))) return order[i];
    return -1;
}

// Builds the replacement block for the BMI1 instruction at `insn` (placed at site_addr) into out/cap, to run at
// out_addr. Returns the block length, or 0 when the form is not handled. *len_out = length of the original insn.
size_t jsi_fast_build(uint8_t *out, size_t cap, uint64_t out_addr, const uint8_t *insn, size_t avail,
                      uint64_t site_addr, size_t *len_out) {
    JsiBmi d;
    if (!jsi_bmi_parse(insn, avail, &d)) return 0;
    JsiAsm A = {out, out + cap, 0}, *a = &A;
    int w = d.w, D = d.dst, S;
#define BIT(r) (1u << (r))
    if (D == 4) return 0;
    if (d.rm >= 0) {
        S = d.rm;
        if (S == 4 || (d.op < 2 && d.a == 4)) return 0;
    } else {
        if (d.op < 2 && (d.a == D || d.a == 4)) return 0;  // the load would destroy a source
        a_rex(a, w, D >> 3, d.vx, d.vb); a8(a, 0x8B);       // mov D,[mem] with the original addressing bytes
        a8(a, (d.mem[0] & 0xC7) | ((D & 7) << 3));
        for (size_t i = 1; i < d.memlen; ++i) a8(a, d.mem[i]);
        S = D;
    }
    if (d.op == 0) {                                         // ANDN: D = ~A & S
        int Aa = d.a;
        if (Aa == S) a_op_rr(a, 0, 0x31, D, D);
        else if (D == Aa) { a_grp(a, w, 0xF7, 2, D); a_op_rr(a, w, 0x21, D, S); }
        else if (D == S) { a_grp(a, 1, 0xF7, 2, Aa); a_op_rr(a, w, 0x21, D, Aa); a_grp(a, 1, 0xF7, 2, Aa); }
        else { a_op_rr(a, w, 0x89, D, Aa); a_grp(a, w, 0xF7, 2, D); a_op_rr(a, w, 0x21, D, S); }
    } else if (d.op == 1) {                                  // BEXTR: D = (S >> ctrl[7:0]) & mask(ctrl[15:8])
        int C = d.a, W = w ? 64 : 32;
        unsigned used = BIT(D) | BIT(S) | BIT(C) | BIT(1) | BIT(4);
        int T = jsi_pick_scratch(used), U = T < 0 ? -1 : jsi_pick_scratch(used | BIT(T));
        if (T < 0 || U < 0) return 0;
        a_rsp_adj(a, -136);                                  // step over the red zone
        if (D != 1) a_push(a, 1);
        a_push(a, T); a_push(a, U);
        a_op_rr(a, 1, 0x89, T, S);
        a_op_rr(a, 1, 0x89, U, C);
        a_movzx8(a, 1, U);                                   // ecx = start
        a_grp_i8(a, 0, 0x83, 7, 1, W);                       // cmp ecx,W
        uint8_t *jzero = a_j8(a, 0x73);                      // jae -> 0
        a_grp(a, w, 0xD3, 5, T);                             // shr T,cl
        a_grp_i8(a, 1, 0xC1, 5, U, 8);
        a_movzx8(a, 1, U);                                   // ecx = len
        a_grp_i8(a, 0, 0x83, 7, 1, W);
        uint8_t *jall = a_j8(a, 0x73);                       // len >= W: no mask
        a_mov_imm32(a, U, 1);
        a_grp(a, w, 0xD3, 4, U);                             // shl U,cl
        a_grp(a, w, 0xFF, 1, U);                             // dec U
        a_op_rr(a, w, 0x21, T, U);                           // and T,U   (flags: ZF, CF=OF=0)
        uint8_t *jd1 = a_j8(a, 0xEB);
        a_bind(a, jall);
        a_op_rr(a, w, 0x85, T, T);                           // test T,T
        uint8_t *jd2 = a_j8(a, 0xEB);
        a_bind(a, jzero);
        a_op_rr(a, 0, 0x31, T, T);                           // T = 0, ZF=1
        a_bind(a, jd1); a_bind(a, jd2);
        a_op_rr(a, w, 0x89, D, T);
        a_pop(a, U); a_pop(a, T); if (D != 1) a_pop(a, 1);
        a_rsp_adj(a, 136);
    } else {                                                 // BLSR / BLSMSK / BLSI: D = f(S)
        int T = jsi_pick_scratch(BIT(D) | BIT(S) | BIT(4));
        if (T < 0) return 0;
        a_rsp_adj(a, -136);
        a_push(a, T);
        if (d.op == 2) {                                     // BLSR: S & (S-1); CF = (S == 0)
            a_lea_dec(a, w, T, S);
            a_op_rr(a, w, 0x21, T, S);
            uint8_t *jok = a_j8(a, 0x75);                    // result != 0: flags are exact
            a_op_rr(a, w, 0x85, S, S);
            uint8_t *jpow = a_j8(a, 0x75);                   // S is a power of two: CF = 0
            a8(a, 0xF9);                                     // S == 0: stc
            uint8_t *jend = a_j8(a, 0xEB);
            a_bind(a, jpow);
            a_op_rr(a, 0, 0x31, T, T);                       // T = 0 with ZF=1, SF=0, CF=OF=0
            a_bind(a, jok); a_bind(a, jend);
        } else if (d.op == 3) {                              // BLSMSK: S ^ (S-1); CF = (S == 0)
            a_lea_dec(a, w, T, S);
            a_op_rr(a, w, 0x85, S, S);
            uint8_t *jnz = a_j8(a, 0x75);
            a_grp_i8(a, w, 0x83, 1, T, 0xFF);                // or T,-1  (ZF=0, SF=1, CF=OF=0)
            a8(a, 0xF9);                                     // stc
            uint8_t *jend = a_j8(a, 0xEB);
            a_bind(a, jnz);
            a_op_rr(a, w, 0x31, T, S);                       // xor T,S
            a_bind(a, jend);
        } else {                                             // BLSI: S & -S; CF = (S != 0)
            a_op_rr(a, w, 0x89, T, S);
            a_grp(a, w, 0xF7, 3, T);                         // neg T
            a_op_rr(a, w, 0x21, T, S);
            uint8_t *jz = a_j8(a, 0x74);
            a8(a, 0xF9);                                     // stc
            a_bind(a, jz);
        }
        a_op_rr(a, w, 0x89, D, T);
        a_pop(a, T);
        a_rsp_adj(a, 136);
    }
    // jmp site+len
    a8(a, 0xE9);
    int64_t rel = (int64_t)(site_addr + d.len) - (int64_t)(out_addr + (uint64_t)(a->p - out) + 4);
    if (rel > INT32_MAX || rel < INT32_MIN) return 0;
    a32(a, (uint32_t)(int32_t)rel);
#undef BIT
    if (a->err) return 0;
    if (len_out) *len_out = d.len;
    return (size_t)(a->p - out);
}

static const uint32_t k_hot_sites[] = {
    0x0222cb, 0x04c06b, 0x05082f, 0x05083d, 0x05084b, 0x050859, 0x05ac8c, 0x0607f3,
    0x0622ed, 0x06b70f, 0x07f1fb, 0x093a1c, 0x0d53de, 0x1f92ff, 0x1fa97d, 0x1fe727,
    0x203050, 0x20305e, 0x20306c, 0x20307a, 0x203642, 0x203650, 0x20365e, 0x244985,
    0x2453c4, 0x248854, 0x248e98, 0x273ce4, 0x321f69, 0x329114, 0x32afe0, 0x32b7a9,
    0x3324cf, 0x349e76, 0x34d6e4, 0x34d792, 0x34d941, 0x34f637, 0x34f9a8, 0x34f9b0,
    0x373882, 0x373d63, 0x37878d, 0x37882f, 0x378c46, 0x378d42, 0x3860a9, 0x3862c6,
    0x405a24, 0x406e68, 0x40796f, 0x407fc8, 0x422488, 0x428920, 0x456f54, 0x4570fd,
    0x45712d, 0x45715d, 0x45718d, 0x4571bd, 0x4573e3, 0x4573f9, 0x45740b, 0x45c21b,
    0x45c4ad, 0x45e1e5, 0x4723e5, 0x4727d5, 0x4727e8, 0x4727f2, 0x4727fc, 0x472f94,
    0x472f99, 0x472f9e, 0x472fa3, 0x472fad, 0x472fb2, 0x472fb7, 0x474a75, 0x475205,
    0x475265, 0x475292, 0x4752dc, 0x4752e6, 0x4752f6, 0x475300, 0x47530a, 0x47b14c,
    0x47b229, 0x47b76d, 0x47d656, 0x47e700, 0x47e70f, 0x4bf284, 0x4bf2db, 0x4c2837,
    0x4d9972, 0x4d99b8, 0x4e7a16, 0x4fc0e9, 0x51ba63, 0x52a5ab, 0x52bc5e, 0x52c011,
    0x52cb84, 0x52cd15, 0x52cd38, 0x52cdbc, 0x52cf14, 0x52cf32, 0x52cf50, 0x52ea78,
    0x85f988, 0x85f9db, 0x86912d, 0x86913c,
};

// Patches every listed site (image offsets) that still holds a handled BMI1 form. `image_bytes` bounds the offsets.
size_t jsi_fast_patch(uint8_t *image, size_t image_bytes, uint8_t *jit, size_t jit_cap,
                      const uint32_t *sites, size_t nsites) {
    size_t used = 0, patched = 0, rejected = 0;
    for (size_t i = 0; i < nsites; ++i) {
        if ((size_t)sites[i] + 15 > image_bytes) { ++rejected; continue; }
        uint8_t *site = image + sites[i];
        used = (used + 15) & ~(size_t)15;
        if (used >= jit_cap) { rt_logf("BMI1 fast path: code area full"); break; }
        size_t len = 0;
        size_t n = jsi_fast_build(jit + used, jit_cap - used, (uint64_t)(uintptr_t)(jit + used), site, 15,
                                  (uint64_t)(uintptr_t)site, &len);
        int64_t rel = (int64_t)(uintptr_t)(jit + used) - (int64_t)(uintptr_t)(site + 5);
        if (!n || len < 5 || rel > INT32_MAX || rel < INT32_MIN) { ++rejected; continue; }
        site[0] = 0xE9;
        int32_t r32 = (int32_t)rel;
        memcpy(site + 1, &r32, 4);
        memset(site + 5, 0xCC, len - 5);
        used += n;
        ++patched;
    }
    rt_logf("BMI1 fast path: %u of %u sites rewritten to inline code (%u left to the fault handler), %u bytes of code",
            (unsigned)patched, (unsigned)nsites, (unsigned)rejected, (unsigned)used);
    return patched;
}

size_t jsi_fast_patch_hot(uint8_t *image, size_t image_bytes, uint8_t *jit, size_t jit_cap) {
    if (getenv("JSTARS_NO_BMI_FASTPATH")) return 0;         // A/B switch for diagnosis
    JsiCpuWords w = jsi_cpu_words();
    if (w.leaf7_ebx & (1u << 3)) return 0;                   // the CPU has BMI1: nothing to do
    return jsi_fast_patch(image, image_bytes, jit, jit_cap, k_hot_sites, sizeof k_hot_sites / sizeof *k_hot_sites);
}

// Thread-local fault RIP for RIP-relative resolution inside jsi_emulate.
uintptr_t jsi_fault_rip_tls = 0;

int jsi_handle_fault(const struct OsFault *f) {
    if (!f || !f->native) return 0;
#ifdef _WIN32
    if (f->signal != 0xC000001D /* EXCEPTION_ILLEGAL_INSTRUCTION */) return 0;
#else
    if (f->signal != 4 /* SIGILL */) return 0;
#endif
    uint8_t bytes[15];
    size_t got;
    const JsiSite *site = jsi_find_site(f->rip);
    uint8_t pre[3] = {0, 0, 0};
    size_t npre = 0;
    if (!site) {
        // The instruction may start with legacy prefix bytes (66, 67, segment, ...) that precede the patched F3:
        // the CPU then reports the fault at the first prefix byte. Only prefix bytes may lie in between.
        uint8_t look[4];
        size_t g = os_safe_read(look, (const void *)f->rip, sizeof look);
        for (size_t k = 1; k <= 3 && k < g; ++k) {
            if (!jsi_is_prefix_byte(look[k - 1])) break;
            const JsiSite *s = jsi_find_site(f->rip + k);
            if (s) { site = s; npre = k; memcpy(pre, look, k); break; }
        }
    }
    if (site) {
        // The code now holds UD2; emulate what was there before (prefix bytes + saved original bytes).
        memcpy(bytes, pre, npre);
        memcpy(bytes + npre, site->orig, sizeof bytes - npre);
        got = sizeof bytes;
    } else {
        got = os_safe_read(bytes, (const void *)f->rip, sizeof bytes);
        if (got < 4) return 0;
    }
    JsiRegs r;
    r.rflags = f->rflags;
    for (int i = 0; i < 16; ++i) r.gpr[i] = f->gpr[i];
    jsi_fault_rip_tls = f->rip;
    size_t len = 0;
    char mn[16] = {0};
    int rc = jsi_emulate(bytes, got, &r, &len, mn, sizeof mn);
    jsi_fault_rip_tls = 0;
    if (rc != 1 || len == 0 || len > 15) return 0;
    static unsigned long hits = 0;
    // Throttled log: first 8 sites, then every 1024th, so a hot strlen loop does not spam.
    if (hits < 8 || (hits & 1023) == 0) {
        rt_logf("ISA emulate: %s at %p (+%u) (%lu emulated so far)",
                mn[0] ? mn : "?", (void *)f->rip, (unsigned)len, hits + 1);
    }
    ++hits;
#ifdef _WIN32
    {
        EXCEPTION_POINTERS *e = (EXCEPTION_POINTERS *)f->native;
        CONTEXT *x = e->ContextRecord;
        x->Rax = r.gpr[0]; x->Rcx = r.gpr[1]; x->Rdx = r.gpr[2]; x->Rbx = r.gpr[3];
        x->Rsp = r.gpr[4]; x->Rbp = r.gpr[5]; x->Rsi = r.gpr[6]; x->Rdi = r.gpr[7];
        x->R8 = r.gpr[8]; x->R9 = r.gpr[9]; x->R10 = r.gpr[10]; x->R11 = r.gpr[11];
        x->R12 = r.gpr[12]; x->R13 = r.gpr[13]; x->R14 = r.gpr[14]; x->R15 = r.gpr[15];
        x->EFlags = (DWORD)((x->EFlags & 0xFFFFF000u) | (r.rflags & 0xFFFu) | 2u);
        x->Rip = (DWORD64)(f->rip + len);
    }
#else
    {
        ucontext_t *uc = (ucontext_t *)f->native;
        static const int idx[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                    REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11,
                                    REG_R12, REG_R13, REG_R14, REG_R15};
        for (int i = 0; i < 16; ++i) uc->uc_mcontext.gregs[idx[i]] = (long long)r.gpr[i];
        long long efl = uc->uc_mcontext.gregs[REG_EFL];
        efl = (efl & ~0xFFFll) | (long long)(r.rflags & 0xFFFu) | 2ll;
        uc->uc_mcontext.gregs[REG_EFL] = efl;
        uc->uc_mcontext.gregs[REG_RIP] = (long long)(f->rip + len);
    }
#endif
    return 1;
}

