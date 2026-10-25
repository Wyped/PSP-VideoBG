# VideoBG: technical description

VideoBG is a kernel plugin for PSP custom firmware that plays a looping video as the XMB wallpaper. It was developed
on ARK-4 and ARK-5 (firmware 6.61) and tested on a PSP-1000, a PSP-2000 (ARK-5) and a PSP Go (ARK-4). The figures
below come from the plugin's own debug logs on those consoles and are measurements, not estimates. Statements that
are assumptions are marked as such.

VideoBG is considered incompatible with the CXMB plugin. Both replace the wallpaper, so behaviour with both loaded is
unpredictable. The combination has not been tested.

## 1. Goals

- The video plays on the XMB without breaking anything else: USB mode, the Game section, the music and video players
  and the memory stick all have to keep working.
- It has to work on the smallest console. The PSP-1000 has 32 MB of RAM and its log showed only about 3.4 MB of the
  user partition free, so caching a full-size clip (about 4x the memory of a half-size one) was ruled out. When the
  clip does not fit in RAM, the plugin streams it from the card.
- When something goes wrong, the stock XMB stays as it was.

## 2. Overview

The XMB draws its wallpaper as a textured patch (a BEZIER primitive) from a 512x512 8888 texture in VRAM. The plugin
never writes into that texture. Instead it:

1. hooks the Graphics Engine (GE) list calls,
2. scans each display list for the wallpaper draw,
3. rewrites the texture-address words in the list so the GE reads a private texture in main RAM,
4. runs a worker thread that puts the current video frame into that RAM texture.

```
 .vbg file -> RAM cache -> worker thread -> upscale -> RAM texture (512 wide, 272 rows)
                                                              ^
 XMB display list --[hook scans, patches TBP0/TBW0]-----------+  the GE reads the RAM texture
```

## 3. Hooks

`install_hooks()` looks up four functions with `sctrlHENFindFunction` and replaces them with
`sctrlHENPatchSyscall`. They sit at different addresses on a 2000 and a Go, so no address is hard-coded.

| Function | NID | Used for |
|---|---|---|
| `sceGeListEnQueue` | `0xAB49E76A` | scan a new list before it runs |
| `sceGeListEnQueueHead` | `0x1C0D95A6` | same, for lists queued at the head |
| `sceGeListUpdateStallAddr` | `0xE0D68148` | scan the part of a list that was just released |
| `sceDisplaySetFrameBuf` | `0x289D82FE` | timing, and noticing when the wallpaper is still being drawn |

Installation is all or nothing: if one of the four cannot be found, nothing is patched and an error goes to the log. The hooks are
only installed after the clip has been checked, so a bad clip can never leave the system half hooked.

The scan has to run before the original function is called, because the GE can start executing the words straight
away.

## 4. Finding the wallpaper (`src/gedump.c`)

`gedump_scan()` walks a display list, following jumps, calls and returns up to a step limit. It tracks the GE state
that matters: texture base (TBP0, `0xA0`), buffer width (TBW0, `0xA8`), pixel format (`0xC3`), size (`0xB8`), vertex
type, texture mode and filter. A BEZIER (`0x05`) drawn with a 512x512 (`0x0909`) 8888 texture is a wallpaper
candidate.

That is not sufficient on its own, because the Game section paints PIC1 and game backgrounds with the same kind of draw. So
there is a second check:

- The first time the wallpaper is seen, the plugin samples 256 words of the texture (every 509th word) as a
  fingerprint. At least 16 different words are required; otherwise the texture is assumed not to be uploaded yet.
- Later draws are accepted if their signature (`vtype*31 + bez*17 + tmode*7 + tfilter`) matches the first one and at
  least 90% of the sampled words are the same. The XMB moves the wallpaper around in VRAM, so the
  address cannot be trusted but the pixels can.
- Anything else is logged as `CANDIDATE ignored` and left alone.

As a consequence, the wallpaper needs some detail (a flat colour fails the 16-word test) and must be a picture the
XMB draws as a texture. The default colour-wave wallpaper is not drawn that way and does not work (tested on
hardware). The built-in picture wallpapers and custom photos work.

## 5. Redirecting instead of drawing into VRAM

The first versions wrote the video frames straight into the wallpaper texture in VRAM. This crashed as soon as the
XMB entered the Game section: the XMB reuses VRAM and moves textures between sections, and game backgrounds are
another 512x512 8888 texture, so the plugin ended up overwriting memory that was no longer the wallpaper. Gating on
the address, locking the address and relocking by fingerprint all still crashed.

The fix (build 15) was to stop writing to VRAM completely. The plugin owns a RAM texture of about 1 MB (stride
484 words, 512 rows) and changes the TBP0 and TBW0 command words of the matching draw so that the GE reads it.

- The words are changed through the uncached mirror, after writing back and invalidating the data-cache line, so a
  stale cached copy cannot overwrite the patch.
- Only draws whose stride matches the RAM texture are redirected.
- If the plugin stops patching (the wallpaper is no longer drawn), the GE simply reads the original texture, so the
  stock look comes back straight away.
- Each finished frame is written back with `sceKernelDcacheWritebackRange` before the GE can read it.

## 6. The `.vbg` format and the converter

`tools/convert.py` calls ffmpeg. A file is five little-endian u32 values (magic `0x31474256` "VBG1", width, height,
fps, frame count) followed by raw RGB565 frames.

- `--half` stores 240x136 and the plugin scales it up 2x. That halves memory and I/O.
- `--fps` sets the frame rate. 12 is recommended; 15 is borderline on a PSP-2000.
- `--crossfade` (1 s by default) fades the end of the clip into the start with ffmpeg's `xfade`, so the loop has no
  visible jump. Clips shorter than three times the fade skip it.

`play_clip()` checks the header, that the frame count is neither zero nor above `MAX_FRAMES`, and that the file is
complete. A truncated file plays its complete frames and logs a warning. A file it cannot use leaves the XMB alone.

## 7. Playback

**RAM cache.** If the whole clip plus a 3 MB reserve fits in the user partition, it is read once (5.5 MB in 528 ms
on the PSP-2000) and the file is closed. The card then stays idle, which keeps USB and the XMB's own file access
fast. If it does not fit, the clip is streamed, and read errors are retried with a 1 s back-off.

**Worker thread.** It runs at priority `0x48`. At `0x30`, with the long blit of that
build, it starved USB. `0x60` and `0x48` were both fine in tests.

**Upscale.** `blit_half` does a separable bilinear 2x upscale: a horizontal pass into a buffer, then a vertical pass
that writes each 128-byte texture block in one go (the GE reads the 8888 texture in 16-byte by 8-row blocks). With
that layout and `-O2` the CPU time per frame went from 48 ms to about 33 ms on the PSP-2000, and it is about 22 ms on
the Go.

**Pause and resume.** When the wallpaper stops being drawn (Game section, some menus) the worker pauses, and it
picks up at the right frame when the wallpaper is drawn again.

**Frame skipping.** The plugin keeps a smoothed wall time per frame. When it exceeds 1.4x the frame period, the
plugin advances 2 or 3 frames per draw instead of 1, and returns to 1 when it drops below 1.2x. That keeps the video at
the right speed under load (MP3 player, memory-stick access) at the price of a lower frame rate. On the PSP-2000
under heavy load the step flips between 1 and 2 quite often; this was accepted.

**Streaming on the PSP-1000.** Leaving USB mode (or a card change) invalidates the open file handle. The read then
fails with `0x80020328`. Build 22 reopens the file after two failed reads, and again after every five failures. On the
1000 the log shows `READ failed`, `REOPEN`, then `READ recovered after 3 failures`, and the video resumes. Build 20
retried but never reopened, so it stayed stopped until a reboot.

## 8. Error handling

Every failure leaves the stock XMB alone. The reason goes to the log when debug is on.

| Condition | Result |
|---|---|
| clip missing or unreadable | `ERROR: no usable video`, no hooks installed |
| bad magic, bad size, 0 or too many frames | same |
| truncated clip | plays the complete frames, logs a warning |
| not enough RAM for the cache | streams from the card |
| a hook function is not found | nothing is patched |
| the RAM texture cannot be allocated | the wallpaper draw is not redirected |
| no picture wallpaper selected | nothing is recognised, the XMB looks normal |

## 9. Debug log

The log is off by default. If `ms0:/seplugins/videobg.debug` exists, the worker enables a log at
`ms0:/seplugins/videobg.log`. The file is opened, appended to and closed for every line, so it survives a freeze. It
contains the build number, resolved function addresses, memory figures, wallpaper detection, step changes and periodic
statistics.
With the marker present, holding L+R+Square starts a verbose dump of the GE commands.

## 10. Measurements

| | PSP-2000 (ARK-5) | PSP Go (ARK-4) |
|---|---|---|
| CPU per frame (240x136 to 480x272) | about 33 ms | about 22 ms |
| wall time per frame | 55 to 100+ ms under load | 29 to 43 ms |
| clip load speed | about 7.8 MB/s | about 12 MB/s |
| free user memory at start | about 23.5 MB | not recorded |

At 12 fps the period is 83 ms, which is comfortable on both models. A 10 s half-size clip at 12 fps takes about 7.8 MB, plus
1 MB for the texture.

**PSP-1000** (a 108-frame 240x136 clip at 12 fps): 3.4 MB of user memory free, so the 7.05 MB clip plus the 3 MB reserve
does not fit and it streams. CPU per frame is about 33 ms, the same as the 2000. Wall time is 47 to 98 ms and a read
averages 9 to 18 ms. Under load the step went up to 3 (one spike of about 400 ms) and came back to 1. The pause and
resume, the redirect and the Game section all worked. The XMB moved the wallpaper texture from `0411fa00` to
`0411bc00` and the fingerprint relock accepted it with 256 of 256 words matching.

## 11. Limitations and open issues

- The XMB needs a picture wallpaper (section 4).
- Not compatible with CXMB (see the introduction).
- On the PSP-1000 the plugin works, but the clip is streamed because only about 3.4 MB is free. Playback is slightly
  less smooth than on the other models under load, and depends on memory-stick speed and on what else the XMB is
  doing. It was observed to be less smooth in the XMB settings screens than elsewhere. The log has no marker for which
  screen is showing, so the cause is unknown. The assumed explanation is that the XMB's own card access competes with
  the streaming reads.
- Read-ahead was tried on the PSP-1000: build 23 read 6 frames per card access and build 24 read 3, reducing card
  accesses from 12 a second to about 2 or 4. Both were equally smooth in every XMB screen, but felt slightly less
  smooth overall than build 22 (one frame per read), which is the released version. This is a subjective assessment
  from one tester, one run each. The assumed (unmeasured) reason is that a batched read takes longer than one frame
  period, so the frame that triggers it arrives late (about 55 ms for 6 frames at the observed card speed).
- The RAM cache needs the clip plus a 3 MB reserve to fit in the free user memory. On the 1000, with 3.41 MB free,
  at most about 257 KB (4 frames) would fit, so the cache is effectively never used there.
- A larger cache for 64 MB models was considered and dropped. It would need memory outside the user partition, and a
  build that probed the partition table crashed at boot (build 21, reverted).
- The reason the Go is faster than the 2000 is not established.
- No audio.
- The code only uses open pspsdk and ARK APIs.
