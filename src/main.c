#include <pspkernel.h>
#include <pspsdk.h>
#include <pspctrl.h>
#include <pspge.h>
#include <psploadcore.h>
#include <pspsysmem_kernel.h>
#include <systemctrl.h>
#include <stdio.h>
#include <string.h>
#include "klog.h"
#include "gedump.h"

PSP_MODULE_INFO("VideoBG", PSP_MODULE_KERNEL, 1, 0);

#define NID_GE_ENQUEUE       0xAB49E76A
#define NID_GE_ENQUEUE_HEAD  0x1C0D95A6
#define NID_GE_UPDATE_STALL  0xE0D68148
#define NID_DISP_SETFRAMEBUF 0x289D82FE

#define VIDEO_PATH "ms0:/seplugins/videobg.vbg"
#define DEBUG_MARKER "ms0:/seplugins/videobg.debug"
#define MAX_FRAMES 100000
#define VBG_MAGIC  0x31474256 /* "VBG1" */
#define VID_W 480 /* size drawn on screen */
#define VID_H 272
#define USER_PARTITION 2
/* Left free in the user partition when the clip is cached, for the XMB's own
   allocations (dialogs, music, photos) */
#define RAM_RESERVE (3 * 1024 * 1024)
#define LOAD_CHUNK (64 * 1024)
#define STATS_REPORTS 3 /* each report wakes the memory stick, so only a few */

#define ARM_COMBO (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SQUARE)
#define MAX_QUEUES 64

typedef int (*EnQueueFn)(const void *, void *, int, PspGeListArgs *);
typedef int (*UpdateStallFn)(int, void *);
typedef int (*SetFrameBufFn)(void *, int, int, int);

typedef struct {
    u32 magic, width, height, fps, frames;
} VbgHeader;

static EnQueueFn orig_enqueue, orig_enqueue_head;
static UpdateStallFn orig_update_stall;
static SetFrameBufFn orig_setfb;

static volatile int armed_frames;
static volatile int frame_no;

static struct { int qid; const void *stall; } queues[MAX_QUEUES];
static int queue_next;

static const void *queue_get(int qid)
{
    for (int i = 0; i < MAX_QUEUES; i++)
        if (queues[i].qid == qid)
            return queues[i].stall;
    return NULL;
}

static void queue_set(int qid, const void *stall)
{
    for (int i = 0; i < MAX_QUEUES; i++) {
        if (queues[i].qid == qid) {
            queues[i].stall = stall;
            return;
        }
    }
    queues[queue_next].qid = qid;
    queues[queue_next].stall = stall;
    queue_next = (queue_next + 1) % MAX_QUEUES;
}

/* Every list is scanned so the wallpaper address stays current and its liveness
   is known. */
static void scan_range(const void *from, void *stall)
{
    if (!from)
        return;
    if (armed_frames > 0)
        gedump_scan(from, stall, 1);
    else
        gedump_scan(from, stall, 0);
}

static int hook_enqueue(const void *list, void *stall, int cbid, PspGeListArgs *arg)
{
    scan_range(list, stall);
    int qid = orig_enqueue(list, stall, cbid, arg);
    queue_set(qid, stall ? stall : list);
    return qid;
}

static int hook_enqueue_head(const void *list, void *stall, int cbid, PspGeListArgs *arg)
{
    scan_range(list, stall);
    int qid = orig_enqueue_head(list, stall, cbid, arg);
    queue_set(qid, stall ? stall : list);
    return qid;
}

static int hook_update_stall(int qid, void *stall)
{
    const void *prev = queue_get(qid);
    if (prev && stall)
        scan_range(prev, stall); /* patch before the GE can run the new commands */
    int ret = orig_update_stall(qid, stall);
    if (stall)
        queue_set(qid, stall);
    return ret;
}

static int hook_setfb(void *top, int bw, int pf, int sync)
{
    int ret = orig_setfb(top, bw, pf, sync);
    frame_no++;
    if (armed_frames > 0)
        armed_frames--;
    return ret;
}

static u32 find(const char *mod, const char *lib, u32 nid)
{
    u32 a = sctrlHENFindFunction(mod, lib, nid);
    klog("FIND %s/%s %08x -> %08x", mod, lib, nid, a);
    return a;
}

/* Resolves all four functions first and patches only if every one was found,
   so a firmware that differs never ends up half hooked. */
static int install_hooks(void)
{
    u32 ge_enq = find("sceGE_Manager", "sceGe_user", NID_GE_ENQUEUE);
    u32 ge_head = find("sceGE_Manager", "sceGe_user", NID_GE_ENQUEUE_HEAD);
    u32 ge_stall = find("sceGE_Manager", "sceGe_user", NID_GE_UPDATE_STALL);
    u32 setfb = find("sceDisplay_Service", "sceDisplay", NID_DISP_SETFRAMEBUF);

    if (!ge_enq || !ge_head || !ge_stall || !setfb) {
        klog("ERROR: a required system function was not found; plugin stays inactive");
        return 0;
    }
    orig_enqueue = (EnQueueFn)ge_enq;
    orig_enqueue_head = (EnQueueFn)ge_head;
    orig_update_stall = (UpdateStallFn)ge_stall;
    orig_setfb = (SetFrameBufFn)setfb;
    sctrlHENPatchSyscall((void *)ge_enq, hook_enqueue);
    sctrlHENPatchSyscall((void *)ge_head, hook_enqueue_head);
    sctrlHENPatchSyscall((void *)ge_stall, hook_update_stall);
    sctrlHENPatchSyscall((void *)setfb, hook_setfb);
    return 1;
}

/* Converts one RGB565 frame into the wallpaper texture: swizzled 8888, 4x8
   pixel blocks of 128 bytes, `stride` pixels per texture row. Writes go through
   the uncached VRAM mirror; the XMB flushes the GE texture cache before it draws. */
static inline u32 px8888(u32 p)
{
    u32 r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return ((b << 3) | (b >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((r << 3) | (r >> 2));
}

/* (3a + b) / 4 on all four channels at once, two channels per 32-bit lane. */
static inline u32 mix31(u32 a, u32 b)
{
    u32 lo = 3 * (a & 0x00FF00FF) + (b & 0x00FF00FF) + 0x00020002;
    u32 hi = 3 * ((a >> 8) & 0x00FF00FF) + ((b >> 8) & 0x00FF00FF) + 0x00020002;
    return 0xFF000000 | ((lo >> 2) & 0x00FF00FF) | (((hi >> 2) & 0x00FF00FF) << 8);
}

#define HALF_W (VID_W / 2)
#define HALF_H (VID_H / 2)
/* Frame after the horizontal 2x pass: 480 pixels per row, one row per source row. */
static u32 hbuf[HALF_H][VID_W] __attribute__((aligned(64)));
static u32 lut_r[32], lut_g[64], lut_b[32];
static int lut_ready;

#define WALL_STALE_US 150000

static void init_lut(void)
{
    for (u32 i = 0; i < 32; i++) {
        lut_r[i] = px8888(i << 11) & 0xFF;
        lut_b[i] = px8888(i) & 0xFF0000;
    }
    for (u32 i = 0; i < 64; i++)
        lut_g[i] = px8888(i << 5) & 0xFF00;
    lut_ready = 1;
}

/* Half-size frame to texture. Separable bilinear 2x: a horizontal pass into
   hbuf, then a vertical pass that writes the swizzled blocks. */
static void blit_half(const u16 *src, u32 *dst_base, u32 stride)
{
    if (!lut_ready)
        init_lut();

    for (u32 y = 0; y < HALF_H; y++) {
        const u16 *s = src + y * HALF_W;
        u32 *out = hbuf[y];
        u32 prev = 0, cur = 0, next = 0;
        u32 pp = s[0];
        cur = lut_r[pp >> 11] | lut_g[(pp >> 5) & 63] | lut_b[pp & 31];
        prev = cur;
        for (u32 x = 0; x < HALF_W; x++) {
            u32 nx = x + 1 < HALF_W ? x + 1 : x;
            u32 p = s[nx];
            next = lut_r[p >> 11] | lut_g[(p >> 5) & 63] | lut_b[p & 31];
            out[2 * x] = mix31(cur, prev);
            out[2 * x + 1] = mix31(cur, next);
            prev = cur;
            cur = next;
        }
    }

    u32 blocks_per_row = stride / 4;
    for (u32 by = 0; by < VID_H / 8; by++) {
        u32 *blk = dst_base + by * blocks_per_row * 32;
        const u32 *rows0[8], *rows1[8];
        for (u32 y = 0; y < 8; y++) {
            u32 oy = by * 8 + y;
            s32 sy = oy >> 1, ny = (oy & 1) ? sy + 1 : sy - 1;
            if (ny < 0) ny = 0;
            if (ny >= HALF_H) ny = HALF_H - 1;
            rows0[y] = hbuf[sy];
            rows1[y] = hbuf[ny];
        }
        /* each 128-byte block is written in one contiguous run */
        for (u32 bx = 0; bx < VID_W / 4; bx++, blk += 32) {
            u32 o = bx * 4;
            for (u32 y = 0; y < 8; y++) {
                const u32 *r0 = rows0[y] + o, *r1 = rows1[y] + o;
                blk[y * 4 + 0] = mix31(r0[0], r1[0]);
                blk[y * 4 + 1] = mix31(r0[1], r1[1]);
                blk[y * 4 + 2] = mix31(r0[2], r1[2]);
                blk[y * 4 + 3] = mix31(r0[3], r1[3]);
            }
        }
    }
}

static void blit_frame(const u16 *src, u32 src_w, u32 shift, u32 *dst_base, u32 stride)
{
    u32 blocks_per_row = stride / 4;

    if (shift) {
        blit_half(src, dst_base, stride);
        return;
    }

    /* shift 1 = the frame is stored at half size and each pixel is drawn 2x2 */
    for (u32 by = 0; by < VID_H / 8; by++) {
        u32 *row = dst_base + by * blocks_per_row * 32;
        for (u32 bx = 0; bx < VID_W / 4; bx++) {
            u32 *blk = row + bx * 32;
            for (u32 y = 0; y < 8; y++) {
                const u16 *s = src + (((by * 8 + y) >> shift) * src_w);
                for (u32 x = 0; x < 4; x++) {
                    u32 p = s[(bx * 4 + x) >> shift];
                    u32 r = (p >> 11) & 31;
                    u32 g = (p >> 5) & 63;
                    u32 b = p & 31;
                    r = (r << 3) | (r >> 2);
                    g = (g << 2) | (g >> 4);
                    b = (b << 3) | (b >> 2);
                    blk[y * 4 + x] = 0xFF000000 | (b << 16) | (g << 8) | r;
                }
            }
        }
    }
}

static void *alloc_block(u32 bytes, SceUID *uid)
{
    SceUID blk = sceKernelAllocPartitionMemory(USER_PARTITION, "videobg", PSP_SMEM_Low, bytes + 64, NULL);
    klog("ALLOC %u bytes -> %08x", bytes, blk);
    if (blk < 0)
        return NULL;
    *uid = blk;
    return (void *)(((u32)sceKernelGetBlockHeadAddr(blk) + 63) & ~63u);
}

/* Reads the whole clip into RAM. Returns the base pointer, or NULL if it does
   not fit (a streaming fallback is then used) or a read fails. */
static u8 *load_clip(SceUID fd, u32 frames, u32 frame_bytes)
{
    u32 max_free = sceKernelPartitionMaxFreeMemSize(USER_PARTITION);
    u32 total_free = sceKernelPartitionTotalFreeMemSize(USER_PARTITION);
    u64 need = (u64)frames * frame_bytes;
    klog("MEM user partition max free=%u total free=%u, clip needs %u + reserve %u", max_free, total_free,
         (u32)need, RAM_RESERVE);

    if (need + RAM_RESERVE + 64 > max_free) {
        klog("CACHE off: clip does not fit; at most %u KB of clip would fit, streaming from the card",
             max_free > RAM_RESERVE + 64 ? (max_free - RAM_RESERVE - 64) / 1024 : 0);
        return NULL;
    }

    SceUID blk = -1;
    u8 *base = alloc_block((u32)need, &blk);
    if (!base)
        return NULL;

    u64 t = sceKernelGetSystemTimeWide();
    for (u32 done = 0; done < (u32)need;) {
        u32 n = (u32)need - done;
        if (n > LOAD_CHUNK)
            n = LOAD_CHUNK;
        int r = sceIoRead(fd, base + done, n);
        if (r != (int)n) {
            klog("CACHE load failed at %u: %08x", done, r);
            sceKernelFreePartitionMemory(blk);
            return NULL;
        }
        done += n;
    }
    klog("CACHE loaded %u bytes in %u ms", (u32)need, (u32)((sceKernelGetSystemTimeWide() - t) / 1000));
    return base;
}

/* Reads and validates the header. Returns the size shift (0 = 480x272 stored,
   1 = 240x136 stored and drawn 2x), or -1 if the file is not usable. */
static int hd_is_half(SceUID fd, VbgHeader *hd)
{
    memset(hd, 0, sizeof(*hd));
    if (sceIoRead(fd, hd, sizeof(*hd)) != sizeof(*hd) || hd->magic != VBG_MAGIC || hd->fps == 0 ||
        hd->fps > 60 || hd->frames == 0)
        return -1;
    if (hd->width == VID_W && hd->height == VID_H)
        return 0;
    if (hd->width == VID_W / 2 && hd->height == VID_H / 2)
        return 1;
    return -1;
}

/* Plays the clip. Returns 0 if it is unusable and nothing was changed. */
static int play_clip(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        klog("OPEN %s failed: %08x", path, fd);
        return 0;
    }
    VbgHeader hd;
    int half = hd_is_half(fd, &hd);
    if (half < 0) {
        klog("ERROR: %s has a bad header: magic=%08x %ux%u fps=%u frames=%u", path, hd.magic, hd.width, hd.height,
             hd.fps, hd.frames);
        sceIoClose(fd);
        return 0;
    }
    u32 shift = half;
    u32 frame_bytes = hd.width * hd.height * 2;

    /* a copy that was cut short (full card, interrupted USB transfer) plays what is there */
    SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    SceOff have = size > (SceOff)sizeof(hd) ? (size - sizeof(hd)) / frame_bytes : 0;
    if (have <= 0 || hd.frames > MAX_FRAMES) {
        klog("ERROR: %s holds no complete frame (size %u, frames %u)", path, (u32)size, hd.frames);
        sceIoClose(fd);
        return 0;
    }
    if (have < hd.frames) {
        klog("WARNING: %s is truncated, %u of %u frames", path, (u32)have, hd.frames);
        hd.frames = (u32)have;
    }
    sceIoLseek(fd, sizeof(hd), PSP_SEEK_SET);
    klog("VIDEO %s %ux%u fps=%u frames=%u (%s)", path, hd.width, hd.height, hd.fps, hd.frames,
         half ? "half size, drawn 2x" : "full size");

    u8 *cache = load_clip(fd, hd.frames, frame_bytes);
    SceUID unused;
    u8 *buf = NULL;
    if (cache) {
        sceIoClose(fd); /* the card stays idle from here on */
        fd = -1;
        klog("playing from RAM, memory stick closed");
    } else {
        sceIoLseek(fd, sizeof(hd), PSP_SEEK_SET);
        buf = alloc_block(frame_bytes, &unused);
        if (!buf) {
            klog("ERROR: no memory for a frame buffer");
            sceIoClose(fd);
            return 0;
        }
    }

    if (!install_hooks()) {
        if (fd >= 0)
            sceIoClose(fd);
        return 1;
    }
    klog_flush();

    u64 t0 = sceKernelGetSystemTimeWide();
    u32 file_next = 0; /* streaming only: frame index the file position is at */
    u32 shown = 0, short_reads = 0, reports = 0, read_fails = 0;
    u64 read_us = 0, conv_us = 0, cpu_us = 0;
    int was_held = 0;
    u32 last_cand_seen = 0;
    u32 *tex = NULL;
    u32 step = 1; /* frames advanced per draw; raised while the system is busy */
    u64 wall_ema = 0;
    u32 tex_stride = 0;
    int was_active = 0;

    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        int held = (pad.Buttons & ARM_COMBO) == ARM_COMBO;
        if (klog_enabled() && held && !was_held && armed_frames == 0) {
            klog("ARM verbose scan at frame %d", frame_no);
            armed_frames = 1;
        }
        was_held = held;

        /* Only draw while the XMB is still drawing the wallpaper texture; in other
           screens that VRAM may hold something else. */
        if (gedump_cand_count != last_cand_seen) {
            last_cand_seen = gedump_cand_count;
            klog_flush();
        }
        u64 tnow = sceKernelGetSystemTimeWide();
        int active = gedump_wall_addr && (tnow - gedump_wall_last_us) < WALL_STALE_US;
        if (active != was_active) {
            klog("wallpaper %s at frame %d, free=%u", active ? "drawn again, resuming" : "not drawn, paused",
                 frame_no, sceKernelPartitionTotalFreeMemSize(USER_PARTITION));
            klog_flush();
            was_active = active;
            if (active)
                t0 = tnow;
        }
        u32 wall = gedump_wall_addr, stride = gedump_wall_stride;
        if (!active) {
            sceKernelDelayThread(50 * 1000);
            continue;
        }
        if (!wall) {
            sceKernelDelayThread(100 * 1000);
            t0 = sceKernelGetSystemTimeWide();
            klog_flush();
            continue;
        }

        if (!tex) {
            /* The wallpaper draw is redirected to this RAM texture (512 rows),
               so the XMB's own VRAM texture is never written. */
            SceUID tex_uid;
            tex_stride = stride;
            gedump_tex_stride = stride;
            u32 tex_bytes = tex_stride * 4 * 512;
            tex = alloc_block(tex_bytes, &tex_uid);
            if (!tex) {
                klog("ERROR: no memory for the wallpaper texture; plugin stays inactive");
                return 1;
            }
            memset(tex, 0, tex_bytes);
            blit_frame((const u16 *)(cache ? cache : buf), hd.width, shift, tex, tex_stride);
            sceKernelDcacheWritebackRange(tex, tex_stride * 4 * 272);
            gedump_tex_phys = (u32)tex & 0x1FFFFFFF;
            klog("TEXTURE in RAM at %08x, stride %u; wallpaper draw is redirected", gedump_tex_phys, tex_stride);
            klog_flush();
        }

        u64 now = sceKernelGetSystemTimeWide();
        u32 want = (u32)(((now - t0) * hd.fps) / 1000000ULL);
        want -= want % step;
        u32 idx = want % hd.frames;

        const u8 *frame;
        u64 a = sceKernelGetSystemTimeWide();
        if (cache) {
            frame = cache + (u32)idx * frame_bytes;
        } else {
            if (idx != file_next)
                sceIoLseek(fd, sizeof(hd) + (SceOff)idx * frame_bytes, PSP_SEEK_SET);
            int n = sceIoRead(fd, buf, frame_bytes);
            if (n != (int)frame_bytes) {
                /* card removed, asleep or busy: retry, backing off to once a second */
                short_reads++;
                file_next = (u32)-1;
                if (read_fails == 0)
                    klog("READ failed: %08x (idx %u)", n, idx);
                /* USB mode or a card change invalidates the handle: reopen it */
                if (read_fails >= 2 && read_fails % 5 == 2) {
                    sceIoClose(fd);
                    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
                    klog("REOPEN %s -> %08x after %u failures", path, fd, read_fails);
                    if (fd < 0)
                        fd = -1;
                }
                sceKernelDelayThread(++read_fails < 20 ? 20 * 1000 : 1000 * 1000);
                continue;
            }
            if (read_fails >= 2)
                klog("READ recovered after %u failures", read_fails);
            read_fails = 0;
            file_next = (idx + 1) % hd.frames;
            if (file_next == 0)
                sceIoLseek(fd, sizeof(hd), PSP_SEEK_SET);
            frame = buf;
        }
        u64 b = sceKernelGetSystemTimeWide();
        SceKernelThreadRunStatus rs0, rs1;
        rs0.size = rs1.size = sizeof(rs0);
        sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &rs0);

        blit_frame((const u16 *)frame, hd.width, shift, tex, tex_stride);
        sceKernelDcacheWritebackRange(tex, tex_stride * 4 * 272);
        u64 c = sceKernelGetSystemTimeWide();
        sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &rs1);
        cpu_us += (u64)rs1.runClocks.low + ((u64)rs1.runClocks.hi << 32) - rs0.runClocks.low -
                  ((u64)rs0.runClocks.hi << 32);

        read_us += b - a;
        conv_us += c - b;
        shown++;
        if (shown % 150 == 0 && (!cache || reports < STATS_REPORTS)) {
            reports++;
            klog("STATS frame=%d shown=%u late=%u shortreads=%u avg read=%uus blit=%uus (cpu %uus) patched=%u", frame_no,
                 shown, want > shown ? want - shown : 0, short_reads, (u32)(read_us / 150), (u32)(conv_us / 150),
                 (u32)(cpu_us / 150), gedump_patched);
            read_us = conv_us = cpu_us = 0;
            klog_flush();
        }

        /* Smoothed wall time of a draw against the frame period. When other
           threads keep the worker waiting, draw every 2nd or 3rd frame at an even
           cadence instead of stuttering; go back once the load is gone. */
        u64 period = 1000000ULL / hd.fps;
        wall_ema = wall_ema ? (wall_ema * 3 + (c - b)) / 4 : (c - b);
        u32 new_step = step;
        if (wall_ema * 10 > period * 14 && step < 3)
            new_step = step + 1;
        else if (wall_ema * 10 < period * 12 && step > 1)
            new_step = step - 1;
        if (new_step != step) {
            klog("step %u -> %u (wall %uus, period %uus) at frame %d", step, new_step, (u32)wall_ema, (u32)period,
                 frame_no);
            step = new_step;
            wall_ema = 0;
        }

        u64 end = sceKernelGetSystemTimeWide();
        u32 cur = (u32)(((end - t0) * hd.fps) / 1000000ULL);
        u32 next_frame = (cur / step + 1) * step;
        u64 next_at = t0 + ((u64)next_frame * 1000000ULL) / hd.fps;
        /* always leave a gap so the system threads get the CPU */
        sceKernelDelayThread(next_at > end + 2000 ? (SceUInt)(next_at - end) : 2000);
    }
}

static int worker(SceSize args, void *argp)
{
    sceKernelDelayThread(3 * 1000 * 1000); /* wait for the memory stick to be mounted */

    SceIoStat st;
    klog_enable(sceIoGetstat(DEBUG_MARKER, &st) >= 0);
    klog("VideoBG %s build %d, devkit %08x, CFW %08x minor %08x", VERSION_STR, BUILD_NUM, sceKernelDevkitVersion(),
         sctrlHENGetVersion(), sctrlHENGetMinorVersion());

    if (!play_clip(VIDEO_PATH))
        klog("ERROR: no usable video; plugin inactive, the XMB is unchanged");
    klog("player stopped");
    klog_flush();
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceUID th = sceKernelCreateThread("VideoBGWorker", worker, 0x48, 0x4000, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
    return 0;
}
