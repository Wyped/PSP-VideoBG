#include <pspkernel.h>
#include "gedump.h"
#include "klog.h"

#define MAX_STEPS 20000
#define CALL_DEPTH 4

volatile u32 gedump_wall_addr;
volatile u32 gedump_wall_stride;
volatile u32 gedump_wall_hits;
volatile u32 gedump_cand_count;
static u32 last_cand;
volatile u64 gedump_wall_last_us;

/* GE state survives across the chunks a list is extended with. */
static u32 base_reg;
static u32 cur_tbp, cur_tbw, cur_tpsm;
static int tex512;
static u32 cur_vtype, cur_tmode, cur_tfilter;
static u32 lock_sig;
static u32 tbp_at, tbw_at;
static u32 ok_phys;
static u64 ok_us;
volatile u32 gedump_tex_phys;
volatile u32 gedump_tex_stride;
volatile u32 gedump_patched;
#define RELOCK_STALE_US 1000000
#define FP_WORDS 256
#define FP_STEP 509
static u32 fp_ref[FP_WORDS];

/* Samples the untouched wallpaper once; a texture at a new address is accepted
   only if its pixels match, so a game's PIC1 background can never pass. */
static void fp_sample(u32 phys, u32 *out)
{
    volatile u32 *t = (volatile u32 *)(phys | 0xA0000000);
    for (u32 i = 0; i < FP_WORDS; i++)
        out[i] = t[i * FP_STEP];
}

static u32 fp_score(u32 phys)
{
    u32 cur[FP_WORDS], same = 0;
    fp_sample(phys, cur);
    for (u32 i = 0; i < FP_WORDS; i++)
        same += cur[i] == fp_ref[i];
    return same;
}

static int valid_addr(u32 a)
{
    u32 p = a & 0x1FFFFFFF;
    return p >= 0x04000000 && p < 0x0C000000;
}

/* Read through the uncached mirror: the CPU may still hold list words in cache. */
static u32 rd(u32 addr)
{
    return *(volatile u32 *)((addr & 0x1FFFFFFF) | 0xA0000000);
}

static void log_cmd(u32 at, u32 cmd, u32 arg)
{
    switch (cmd) {
    case 0x04: klog("%08x PRIM type=%u count=%u", at, (arg >> 16) & 7, arg & 0xFFFF); break;
    case 0x05: klog("%08x BEZIER %06x", at, arg); break;
    case 0x06: klog("%08x SPLINE %06x", at, arg); break;
    case 0x9C: klog("%08x FBP %06x", at, arg); break;
    case 0xA0: klog("%08x TBP0 %06x", at, arg); break;
    case 0xA8: klog("%08x TBW0 %06x", at, arg); break;
    case 0xB8: klog("%08x TSIZE0 %ux%u", at, 1u << (arg & 0xF), 1u << ((arg >> 8) & 0xF)); break;
    case 0xC3: klog("%08x TPSM %u", at, arg & 0xF); break;
    default: break;
    }
}

static void patch_word(u32 at, u32 expect_cmd, u32 value)
{
    if ((rd(at) >> 24) != expect_cmd)
        return;
    /* flush first so the XMB's cached copy of the line cannot overwrite the patch later */
    sceKernelDcacheWritebackInvalidateRange((void *)(at & ~63u), 64);
    *(volatile u32 *)((at & 0x1FFFFFFF) | 0xA0000000) = (expect_cmd << 24) | value;
}

/* Called for a BEZIER drawn with a 512x512 8888 texture. Only a texture whose
   pixels match the wallpaper seen at start-up is redirected to our own RAM
   texture; nothing is ever written to the XMB's VRAM. */
static void latch_wallpaper(u32 bez)
{
    u32 addr = ((cur_tbw & 0xFF0000) << 8) | (cur_tbp & 0xFFFFFF);
    u32 stride = cur_tbw & 0xFFFF;
    u32 phys = addr & 0x1FFFFFFF;
    u64 now = sceKernelGetSystemTimeWide();

    if (gedump_tex_phys && phys == gedump_tex_phys) {
        gedump_wall_hits++;
        gedump_wall_last_us = now;
        return;
    }

    /* the top 272 rows of the texture must lie inside VRAM */
    if (phys < 0x04000000 || phys + stride * 4 * 272 > 0x04200000 || stride < 480 || stride > 512)
        return;

    u32 sig = cur_vtype * 31u + bez * 17u + cur_tmode * 7u + cur_tfilter;

    if (!gedump_wall_addr) {
        fp_sample(phys, fp_ref);
        u32 distinct = 0;
        for (u32 i = 1; i < FP_WORDS; i++)
            distinct += fp_ref[i] != fp_ref[0];
        if (distinct < 16)
            return; /* texture not uploaded yet */
        klog("WALLPAPER addr=%08x stride=%u vtype=%06x bez=%06x tmode=%06x tfilter=%06x", phys, stride, cur_vtype,
             bez, cur_tmode, cur_tfilter);
        gedump_wall_addr = phys;
        gedump_wall_stride = stride;
        lock_sig = sig;
        ok_phys = phys;
        ok_us = now;
    } else {
        int ok = 0;
        if (phys == ok_phys && now - ok_us < 500000) {
            ok = 1;
        } else if (sig == lock_sig) {
            u32 score = fp_score(phys);
            if (score >= FP_WORDS * 9 / 10) {
                ok = 1;
                ok_phys = phys;
                ok_us = now;
                if (phys != gedump_wall_addr)
                    klog("WALLPAPER at %08x (pixels match %u/%u)", phys, score, FP_WORDS);
                gedump_wall_addr = phys;
            }
        }
        if (!ok) {
            if (phys != last_cand) {
                last_cand = phys;
                gedump_cand_count++;
                klog("CANDIDATE ignored addr=%08x vtype=%06x bez=%06x tmode=%06x tfilter=%06x (locked %08x)", phys,
                     cur_vtype, bez, cur_tmode, cur_tfilter, gedump_wall_addr);
            }
            return;
        }
    }

    gedump_wall_hits++;
    gedump_wall_last_us = now;
    u32 tex = gedump_tex_phys;
    /* the RAM texture was laid out for one stride; only redirect draws that use it */
    if (tex && tbp_at && tbw_at && (cur_tbw & 0xFFFF) == gedump_tex_stride) {
        patch_word(tbp_at, 0xA0, tex & 0xFFFFFF);
        patch_word(tbw_at, 0xA8, (cur_tbw & 0xFFFF) | ((tex >> 8) & 0xFF0000));
        gedump_patched++;
    }
}

void gedump_scan(const void *from, const void *to, int verbose)
{
    u32 p = (u32)from;
    u32 end = (u32)to;
    u32 stack[CALL_DEPTH];
    int depth = 0;
    int jumped = 0;

    for (int steps = 0; steps < MAX_STEPS; steps++) {
        if (!valid_addr(p))
            return;
        if (end && !jumped && p >= end)
            return;

        u32 w = rd(p);
        u32 cmd = w >> 24;
        u32 arg = w & 0xFFFFFF;

        switch (cmd) {
        case 0x10: base_reg = (arg & 0x0F0000) << 8; break;
        case 0xA0: cur_tbp = arg; tbp_at = p; break;
        case 0xA8: cur_tbw = arg; tbw_at = p; break;
        case 0xC3: cur_tpsm = arg & 0xF; break;
        case 0x12: cur_vtype = arg; break;
        case 0xC2: cur_tmode = arg; break;
        case 0xC6: cur_tfilter = arg; break;
        case 0xB8: tex512 = (arg == 0x0909); break;
        case 0x05:
            if (tex512 && cur_tpsm == 3)
                latch_wallpaper(arg);
            break;
        default: break;
        }
        if (verbose)
            log_cmd(p, cmd, arg);

        if (cmd == 0x0C || cmd == 0x0F) {
            if (depth == 0)
                return;
        } else if (cmd == 0x08) {
            p = (base_reg | arg) & ~3u;
            jumped = 1;
            continue;
        } else if (cmd == 0x0A) {
            if (depth < CALL_DEPTH)
                stack[depth++] = p + 4;
            p = (base_reg | arg) & ~3u;
            jumped = 1;
            continue;
        } else if (cmd == 0x0B) {
            if (depth == 0)
                return;
            p = stack[--depth];
            continue;
        }
        p += 4;
    }
}
