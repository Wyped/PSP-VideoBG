#ifndef GEDUMP_H
#define GEDUMP_H

#include <psptypes.h>

/* Walks a GE command range [from, to) following JUMP/CALL/RET. It tracks the
   texture registers and latches the wallpaper: the 512x512 8888 texture that a
   BEZIER draws. With verbose set it also logs the interesting commands. */
void gedump_scan(const void *from, const void *to, int verbose);

/* VRAM address (physical, 0x04......) and pixel stride of the wallpaper texture
   found by the last successful scan; 0 until one is found. */
extern volatile u32 gedump_wall_addr;
extern volatile u32 gedump_wall_stride;
extern volatile u32 gedump_wall_hits;
extern volatile u32 gedump_cand_count;
extern volatile u64 gedump_wall_last_us;

/* Physical address of the RAM texture the wallpaper draw is redirected to; set by the player once it has a frame. */
extern volatile u32 gedump_tex_phys;
extern volatile u32 gedump_tex_stride;
extern volatile u32 gedump_patched;

#endif
