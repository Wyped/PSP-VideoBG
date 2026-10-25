#!/usr/bin/env python3
"""Build a release folder and zip: the plugin, a default video and the
placeholder wallpaper, laid out like the memory stick root.

    python3 tools/package.py --video clip.mp4 [--wallpaper assets/wallpaper.jpg] [--fps 12] [--seconds 10]

Output: release/VideoBG-<version>/ and release/VideoBG-<version>.zip
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONVERT = os.path.join(ROOT, "tools", "convert.py")


def version():
    with open(os.path.join(ROOT, "src", "klog.h")) as f:
        return re.search(r'VERSION_STR\s+"([^"]+)"', f.read()).group(1)


def run(cmd, **kw):
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--video", required=True, help="source of the default video wallpaper")
    ap.add_argument("--wallpaper", default=os.path.join(ROOT, "assets", "wallpaper.jpg"),
                    help="480x272 JPEG the user sets as XMB wallpaper; it gives the XMB a texture to replace")
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--seconds", type=float, default=0)
    args = ap.parse_args()

    env = dict(os.environ)
    env.setdefault("PSPDEV", os.path.expanduser("~/pspdev"))
    env["PATH"] = os.path.join(env["PSPDEV"], "bin") + os.pathsep + env["PATH"]
    run(["make", "-C", ROOT, "clean"], env=env)
    run(["make", "-C", ROOT], env=env)

    name = f"VideoBG-{version()}"
    out = os.path.join(ROOT, "release", name)
    shutil.rmtree(out, ignore_errors=True)
    plug = os.path.join(out, "seplugins")
    os.makedirs(plug)

    shutil.copy(os.path.join(ROOT, "videobg.prx"), plug)
    conv = [sys.executable, CONVERT]
    cmd = conv + [args.video, os.path.join(plug, "videobg.vbg"), "--half", "--fps", str(args.fps)]
    if args.seconds > 0:
        cmd += ["--seconds", str(args.seconds)]
    run(cmd)
    os.makedirs(os.path.join(out, "PICTURE"))
    shutil.copy(args.wallpaper, os.path.join(out, "PICTURE", "videobg_wallpaper.jpg"))
    os.makedirs(os.path.join(out, "tools"))
    shutil.copy(CONVERT, os.path.join(out, "tools"))
    shutil.copy(os.path.join(ROOT, "README.md"), os.path.join(out, "README.txt"))

    archive = shutil.make_archive(out, "zip", os.path.join(ROOT, "release"), name)
    print("\nbuilt", archive)
    for d, _, files in os.walk(out):
        for f in sorted(files):
            p = os.path.join(d, f)
            print(f"  {os.path.relpath(p, out):28} {os.path.getsize(p):>10} bytes")


if __name__ == "__main__":
    main()
