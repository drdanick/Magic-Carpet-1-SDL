#!/usr/bin/env python3
"""Replay a user recording (or any input script) with a fresh C: drive and look at it.

Creates a temporary copy of <data>/CARPET.CD exactly like the replay tests do (tools/replay_test.py),
runs remc1 headless on the script, dumps every Nth frame, prints the tail of the log (crashes,
watchdog stacks, allert_error lines) and builds a contact sheet of the last frames.

  python tools/run_recording.py C:/mc1data/level_1_playthrough.txt --out /tmp/run --every 20
  python tools/run_recording.py rec.txt --out /tmp/run --from 1200 --to 1500 --every 5 -- --no-sound

Arguments after "--" are passed to remc1 (e.g. --no-sound for recordings made with sound off).
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_test  # noqa: E402  (make_c_drive)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--out", required=True, help="output folder (frames, log, sheet.png); recreated")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "win-x86", "RelWithDebInfo", "remc1.exe"))
    ap.add_argument("--data", default=os.environ.get("REMC1_DATA_DIR", "C:/mc1data"))
    ap.add_argument("--every", type=int, default=20, help="dump every Nth frame (0 = none)")
    ap.add_argument("--from", dest="first", type=int, default=0, help="contact sheet: first frame")
    ap.add_argument("--to", dest="last", type=int, default=10**9, help="contact sheet / run: last frame")
    ap.add_argument("--cols", type=int, default=8)
    ap.add_argument("--width", type=int, default=200)
    a, extra = ap.parse_known_args()
    a.extra = [x for x in extra if x != "--"]  # everything unknown goes to remc1

    shutil.rmtree(a.out, ignore_errors=True)
    frames = os.path.join(a.out, "frames")
    cdrive = os.path.join(a.out, "cdrive")
    os.makedirs(frames)
    os.makedirs(cdrive)
    replay_test.make_c_drive(a.data, cdrive)
    cmd = [os.path.abspath(a.exe), "--headless", "--data", a.data, "--c-drive", cdrive,
           "--input-script", os.path.abspath(a.script)]
    if a.every:
        cmd += ["--dump-frames", frames, "--dump-every", str(a.every)]
    if a.last < 10**9:
        cmd += ["--exit-after-frames", str(a.last)]
    cmd += a.extra
    log_path = os.path.join(a.out, "log.txt")
    with open(log_path, "wb") as log:
        rc = subprocess.run(cmd, cwd=os.path.dirname(os.path.abspath(a.exe)), stdout=log,
                            stderr=subprocess.STDOUT, env=dict(os.environ, SDL_AUDIODRIVER="dummy")).returncode
    text = open(log_path, "rb").read().decode("latin-1", "replace").splitlines()
    interesting = [l for l in text if any(k in l for k in ("WATCHDOG", "  #", "allert_error", "exit", "exhausted", "quit"))]
    print(f"exit code {rc} ({rc & 0xFFFFFFFF:#x}); log: {log_path}")
    for l in (interesting or text)[-25:]:
        print("  " + l)
    for f in os.listdir(frames):
        if f.startswith("presented"):
            os.remove(os.path.join(frames, f))
    keep = [f for f in sorted(os.listdir(frames)) if f.startswith("frame") and a.first <= int(f[5:11]) <= a.last]
    if keep:
        sel = os.path.join(a.out, "sel")
        os.makedirs(sel)
        for f in keep[-80:]:
            shutil.copy(os.path.join(frames, f), sel)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "contact_sheet.py"), sel,
                        "--width", str(a.width), "--cols", str(a.cols)], check=False)
        print("frames dumped:", len(keep), "sheet:", os.path.join(sel, "sheet.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
