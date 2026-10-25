#!/usr/bin/env python3
"""Convert a video into the .vbg stream the VideoBG plugin plays.

Layout: 20-byte header (u32 little endian: magic "VBG1", width, height, fps,
frames) followed by `frames` raw RGB565 frames. Frames are 480x272, or 240x136
with --half (the plugin draws each pixel 2x2). A half-size clip is 4x smaller,
which is what lets the plugin keep it in RAM and leave the memory stick idle.

    python3 convert.py input.mp4 videobg.vbg [--fps 15] [--seconds 10] [--half] [--crossfade 1]

By default the last second fades into the first, so the loop has no visible cut
(the clip gets that much shorter). --crossfade 0 turns it off.

Copy the result to ms0:/seplugins/videobg.vbg.
"""
import argparse
import os
import struct
import subprocess
import sys

W, H = 480, 272
MAGIC = 0x31474256


def probe_duration(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                         capture_output=True, text=True)
    try:
        return float(out.stdout.strip())
    except ValueError:
        sys.exit("could not read the input duration")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--seconds", type=float, default=0, help="keep only the first N seconds (0 = all)")
    ap.add_argument("--half", action="store_true", help="store 240x136 (4x smaller), drawn 2x by the plugin")
    ap.add_argument("--crossfade", type=float, default=1.0, help="seconds of end-to-start blend for a seamless loop (0 = off)")
    args = ap.parse_args()

    if not 1 <= args.fps <= 60:
        sys.exit("fps must be 1..60")

    w, h = (W // 2, H // 2) if args.half else (W, H)
    base = f"fps={args.fps},scale={w}:{h}:force_original_aspect_ratio=increase,crop={w}:{h}"
    cmd = ["ffmpeg", "-v", "error", "-y", "-i", args.input]
    if args.seconds > 0:
        cmd += ["-t", str(args.seconds)]

    x = args.crossfade
    dur = probe_duration(args.input)
    if args.seconds > 0:
        dur = min(dur, args.seconds)
    if x > 0 and dur < 3 * x:
        print(f"clip too short for a {x}s crossfade, loop is left as is", file=sys.stderr)
        x = 0
    if x > 0:
        # tail fades into head, then the middle plays; the loop wraps from the
        # middle's end straight into the tail, which is continuous
        graph = (f"[0:v]{base},split=3[a][b][c];"
                 f"[a]trim=start={dur - x:.3f},setpts=PTS-STARTPTS[t];"
                 f"[b]trim=end={x:.3f},setpts=PTS-STARTPTS[h];"
                 f"[c]trim=start={x:.3f}:end={dur - x:.3f},setpts=PTS-STARTPTS[m];"
                 f"[t][h]xfade=transition=fade:duration={x:.3f}:offset=0[f];"
                 f"[f][m]concat=n=2:v=1:a=0[v]")
        cmd += ["-an", "-filter_complex", graph, "-map", "[v]"]
    else:
        cmd += ["-an", "-vf", base]
    cmd += ["-f", "rawvideo", "-pix_fmt", "rgb565le", "-"]

    frame_bytes = w * h * 2
    frames = 0
    with open(args.output, "wb") as out:
        out.write(b"\0" * 20)
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        while True:
            data = proc.stdout.read(frame_bytes)
            if len(data) < frame_bytes:
                break
            out.write(data)
            frames += 1
        if proc.wait() != 0:
            sys.exit("ffmpeg failed")
        if frames == 0:
            sys.exit("no frames decoded")
        out.seek(0)
        out.write(struct.pack("<5I", MAGIC, w, h, args.fps, frames))

    size = os.path.getsize(args.output)
    print(f"{w}x{h}, {frames} frames, {frames / args.fps:.1f} s, {size / 1e6:.1f} MB "
          f"(needs about {size / 1e6 + 3:.1f} MB of free RAM to play from memory)")


if __name__ == "__main__":
    main()
