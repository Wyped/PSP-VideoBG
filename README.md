VideoBG - video wallpaper for the PSP XMB
=========================================

VideoBG is a kernel plugin that plays a looping video behind the XMB. It targets
ARK custom firmware and has been tested on ARK-4 and ARK-5 (6.61) on a PSP-1000,
a PSP-2000 and a PSP Go.

WARNING: VideoBG is considered incompatible with the CXMB plugin. Both replace
the XMB wallpaper, so running them together can give unexpected results (wrong
picture, flicker or a crash). The combination has not been tested. Disable CXMB
in `PLUGINS.TXT` before using VideoBG.

DISCLAIMER: The goal of this project was to assess AI agent usefulness in PSP kernel prx development. Code has been reviewed and tested by a human. 

Install
-------
1. Copy the `seplugins` and `PICTURE` folders from the zip to the root of the
   memory stick. The result is:
   - `ms0:/seplugins/videobg.prx`            the plugin
   - `ms0:/seplugins/videobg.vbg`            the video
   - `ms0:/PICTURE/videobg_wallpaper.jpg`    a placeholder wallpaper
2. Add this line to `ms0:/seplugins/PLUGINS.TXT` (create the file if it does
   not exist):

       vsh, ms0:/seplugins/videobg.prx, on

3. Reboot, then select the placeholder as the wallpaper:
   `Settings > Theme Settings > Wallpaper > Custom Wallpaper > videobg_wallpaper`.
   The video shows up a few seconds after the XMB appears.

Why a placeholder wallpaper is needed? 
The plugin does not draw its own
background. It replaces the picture the XMB is already drawing, so a picture
wallpaper must be selected. The default colour waves do not work (tested). The
built-in picture wallpapers do, for example the PSP-2000 "Tasty Treat" one, and
so does any custom photo. The image in the release zip is a plain placeholder so that
users do not need to supply one.

To remove the plugin, delete the line from `PLUGINS.TXT` (or change `on` to
`off`) and select another wallpaper.

Using your own video
--------------------
Python 3 and ffmpeg are required on the PC.

    python3 tools/convert.py clip.mp4 videobg.vbg --half --fps 12

- `--half` stores the video at 240x136 and the plugin scales it up to 480x272.
  This keeps the file small enough to load into RAM. Without it the file is 4
  times larger.
- `--fps 12` is the recommended value. 15 works on a Go, but a PSP-2000 starts
  dropping frames.
- `--seconds N` keeps only the first N seconds.
- The last second of the clip is faded into the first so the loop has no
  visible jump. `--crossfade 0` turns that off.

How big can the video be?
The plugin loads the whole clip into RAM when it fits. This is always preferred
over reading from the memory stick: the card stays idle and playback is
smoothest. The clip is streamed from the card only when it is too big.

- A 12 fps clip uses about 0.78 MB of RAM per second (65 KB a frame). The plugin
  leaves 3 MB free for the XMB.
- PSP-2000, 3000 and Go: about 23.5 MB is free (measured on a PSP-2000; the
  other two models are assumed to be the same). The largest clip that is cached
  is around 19 MB, about 25 seconds. Staying under 15 MB (about 19 seconds)
  leaves some margin, as free memory varies. `convert.py` prints how much RAM a
  clip needs.
- PSP-1000: only about 3.4 MB is free, so no practical clip fits and playback
  always streams from the card. This works, so the same file can be used on
  every model.

When something goes wrong
-------------------------
The XMB is never left in a broken state. At worst the normal wallpaper is shown.

| Problem | What happens |
|---|---|
| `videobg.vbg` is missing or is not a valid file | The plugin does nothing |
| The video was cut short (bad copy) | It plays the frames that are complete |
| Not enough RAM for the clip | It streams from the card |
| Card removed or busy while streaming | The last frame stays and it retries every second |
| The firmware does not have a function the plugin needs | The plugin does nothing |
| No picture wallpaper selected | Nothing to replace, the XMB looks normal |
| Game section, PIC1 backgrounds | The video pauses and the XMB is left alone |

For troubleshooting, create an empty file named `ms0:/seplugins/videobg.debug`
and reboot. The plugin then writes a log to `ms0:/seplugins/videobg.log`. Delete
the marker file afterwards: without it the plugin never writes to the card.

Known limits
------------
- No audio.
- After a theme or wallpaper change, the video returns once the XMB redraws the
  wallpaper. If it does not, reboot.
- When the console is busy (music player, large folders) the video drops to every
  2nd or 3rd frame and returns to normal when the load stops.
- On the PSP-1000 the clip is streamed, so playback is slightly less smooth under
  load than on the other models. After leaving USB mode the video pauses for a
  few seconds while the plugin reopens the file, then resumes automatically.

Building from source
--------------------
The open [pspdev](https://pspdev.github.io/) toolchain is required, plus Python 3
and ffmpeg for the tools.

    make                      # builds videobg.prx
    python3 tools/package.py --video clip.mp4 [--wallpaper my.jpg] [--fps 12]

`package.py` builds the plugin, converts the clip and writes
`release/VideoBG-<version>.zip`.

In short: the plugin hooks the GE list calls and the display call, detects the
draw that paints the XMB wallpaper, and changes its texture address so the GE
reads a texture in RAM instead. A worker thread writes the video frames into that
texture. Nothing is written into VRAM. The source is in `src/`; a detailed
description is in `docs/TECHNICAL.md`.

Licence
-------
MIT, see `LICENSE`. The wallpaper and video in the zip are copyright free.
