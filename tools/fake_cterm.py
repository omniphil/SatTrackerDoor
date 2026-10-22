#!/usr/bin/env python3
"""Fake CTerm terminal for the Satellite Tracker door's JPEG XL mode: answers the questions, decodes DrawJXLBlob onto a canvas,
answers ESC[6n, plays a script of keys/mouse and saves PNG shots.  usage: fake_cterm.py DOORDIR OUTDIR [W H]"""
import base64, os, pty, re, select, sys, time
try:
    import numpy as np, imagecodecs
    from PIL import Image
except ImportError:
    np = None
import pickle
blobs = []

door, outdir = sys.argv[1], sys.argv[2]
W, H = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (640, 400)
os.makedirs(outdir, exist_ok=True)
canvas = np.zeros((H, W, 3), np.uint8) if np else None
pid, fd = pty.fork()
if pid == 0:
    os.chdir("/tmp")
    os.execv(os.path.join(door, "satdoor"), ["satdoor"])

buf = b""; stats = {"frames": 0, "bytes": 0, "pics": 0}
def send(b): os.write(fd, b)
def pump(secs):
    global buf
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.01)
        if not r: continue
        try: d = os.read(fd, 1 << 20)
        except OSError: return False
        if not d: return False
        buf += d; stats["bytes"] += len(d)
        handle()
    return True
def handle():
    global buf
    while True:
        i = buf.find(b"\x1b")
        if i < 0: buf = b""; return
        buf = buf[i:]
        if buf.startswith(b"\x1b_"):
            j = buf.find(b"\x1b\\")
            if j < 0: return
            cmd = buf[2:j].decode("latin1"); buf = buf[j+2:]
            apc(cmd); continue
        m = re.match(rb"\x1b\[([0-9;?=<>]*)([A-Za-z])", buf)
        if not m:
            if len(buf) < 32 and re.match(rb"\x1b(\[[0-9;?=<>]*)?$", buf): return
            buf = buf[1:]; continue
        p, f = m.group(1), m.group(2); buf = buf[m.end():]
        if f == b"c" and p == b"": send(b"\x1b[=67;84;101;114;109;1;332c")
        elif f == b"n" and p == b"5": send(b"\x1b[0n")
        elif f == b"n" and p == b"6": stats["frames"] += 1; send(b"\x1b[1;1R")
        elif f == b"S" and p == b"?2;1": send(b"\x1b[?2;0;%d;%dS" % (W, H))
def apc(cmd):
    if cmd == "SyncTERM:Q;JXL": send(b"\x1b[=1;1-n"); return
    if cmd.startswith("SyncTERM:C;DrawJXLBlob;"):
        parts = cmd.split(";")
        opts = {k: int(v) for k, v in (x.split("=") for x in parts[2:-1])}
        data = base64.b64decode(parts[-1])
        if np is None:
            blobs.append((opts["DX"], opts["DY"], data)); stats["pics"] += 1; return
        img = imagecodecs.jpegxl_decode(data)
        if img.ndim == 2: img = np.stack([img]*3, -1)
        h, w = img.shape[:2]; x, y = opts["DX"], opts["DY"]
        canvas[y:y+h, x:x+w] = img[:, :, :3]; stats["pics"] += 1
def shot(name):
    if np is None:
        pickle.dump(blobs, open(os.path.join(outdir, name + ".pkl"), "wb")); print("shot", name, stats, flush=True); return
    Image.fromarray(canvas).save(os.path.join(outdir, name + ".png")); print("shot", name, stats, flush=True)

pump(2.5)           # TRACE query times out, CTerm questions, menu
send(b"2"); pump(0.5); send(b" "); pump(8); shot("01_map")
send(b"\x1b[B\x1b[B"); pump(1.5); shot("02_down2")
send(b"3"); pump(6); shot("03_group_weather")
# mouse: hover then click the second group chip (Brightest), row 3
send(b"\x1b[<35;18;3M"); pump(0.5); send(b"\x1b[<0;18;3M\x1b[<0;18;3m"); pump(6); shot("04_click_chip")
send(b"\x1b[<64;40;12M"); pump(1); shot("05_wheel")
# an old terminal's X10 mouse report (ESC [ M + 3 raw bytes): click the Ham radio chip
send(b"\x1b[M" + bytes([32, 32 + 38, 32 + 3]) + b"\x1b[M" + bytes([35, 32 + 38, 32 + 3])); pump(6); shot("05b_x10_click")
send(b"l"); pump(1); send(b"Portland, Maine\r"); pump(5); shot("06_picker_search")
send(b"\x1b"); pump(1.5); shot("07_picker_closed")
t0 = time.time(); f0 = stats["frames"]; b0 = stats["bytes"]; pump(10)
print("idle 10s: frames %d, KB/s %.1f" % (stats["frames"] - f0, (stats["bytes"] - b0) / 10 / 1024))
shot("08_after_idle")
send(b"q"); alive = pump(3)
print("door exited" if not alive else "door still running", stats)
import resource
try: os.waitpid(pid, 0)
except Exception: pass
ru = resource.getrusage(resource.RUSAGE_CHILDREN)
print("door cpu: user %.1fs sys %.1fs" % (ru.ru_utime, ru.ru_stime))
