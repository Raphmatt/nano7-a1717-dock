#!/usr/bin/env python3
"""Upload build/a1717_patch.bin into iPod nano 7G RAM and run it, over ipod_sun's SCSI channel.

Needs ipod_sun (untethered) on the nano, which provides the vendor SCSI commands
  c6 96 01 <addr BE>   write 512 bytes      c6 96 02 <addr BE>   read 512 bytes
  c6 96 03 <addr BE>   call addr (|1 = Thumb)

Linux:  sudo python3 tools/load.py --dev /dev/sdX            (uses sg_raw from sg3_utils)
macOS:  python3 tools/load.py --sg /path/to/ipod_sg_raw     (sg_raw-compatible tool; DEV is ignored)

Run it once after every reboot of the nano. It refuses to write unless the firmware is 1.1.2
(39A10023), the hook site has the original bytes, and the target RAM is unused.
"""
import argparse
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile
import time

LOAD_ADDR = 0x0867F8E4          # must match the Makefile
VERSION_AT = 0x08129D78         # "39A10023" in osos 1.1.2
ID_WRITER = 0x084DF4C4          # hooked function
ID_WRITER_ORIG = bytes.fromhex("10b54178")
CHUNK = 512


class Dev:
    def __init__(self, sg, dev):
        self.sg, self.dev, self.tmp = sg, dev, tempfile.mkdtemp()

    def _run(self, args):
        r = subprocess.run([self.sg, *args], check=False, capture_output=True)
        if r.returncode:
            sys.exit(f"{self.sg} failed: {r.stderr.decode().strip() or r.returncode}")

    @staticmethod
    def _a(addr):
        return [f"{b:02x}" for b in addr.to_bytes(4, "big")]

    def read(self, addr, n):
        out, a = b"", addr
        while len(out) < n:
            base = a & ~(CHUNK - 1)
            f = os.path.join(self.tmp, "r.bin")
            self._run(["-o", f, "-r", str(CHUNK), self.dev, "c6", "96", "02", *self._a(base)])
            blk = pathlib.Path(f).read_bytes()
            take = blk[a - base:a - base + n - len(out)]
            out += take
            a += len(take)
        return out

    def write(self, addr, data):
        assert len(data) == CHUNK
        f = os.path.join(self.tmp, "w.bin")
        pathlib.Path(f).write_bytes(data)
        self._run(["-s", str(CHUNK), "-i", f, self.dev, "c6", "96", "01", *self._a(addr)])

    def call(self, addr):
        self._run(["-o", "/dev/null", "-r", str(CHUNK), self.dev, "c6", "96", "03", *self._a(addr | 1)])


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=os.path.join(here, "..", "build", "a1717_patch.bin"))
    ap.add_argument("--sg", help="sg_raw (Linux) or ipod_sg_raw (macOS); default: sg_raw / ipod_sg_raw from PATH")
    ap.add_argument("--dev", default="/dev/null", help="SCSI device on Linux, e.g. /dev/sdb (ignored on macOS)")
    a = ap.parse_args()

    sg = a.sg or shutil.which("ipod_sg_raw" if platform.system() == "Darwin" else "sg_raw")
    if not sg:
        sys.exit("no sg_raw / ipod_sg_raw found; pass --sg")
    if platform.system() == "Linux" and a.dev == "/dev/null":
        sys.exit("pass --dev /dev/sdX (the iPod's disk)")
    img = pathlib.Path(a.bin).read_bytes()
    d = Dev(sg, a.dev)

    ver = d.read(VERSION_AT, 8)
    if ver != b"39A10023":
        sys.exit(f"firmware is not 1.1.2 / 39A10023 (found {ver!r}); addresses would be wrong")
    site = d.read(ID_WRITER, 4)
    if site != ID_WRITER_ORIG:
        sys.exit(f"hook site {ID_WRITER:#x} = {site.hex()}, expected {ID_WRITER_ORIG.hex()}; "
                 "already patched this boot? reboot the nano first")
    cur = d.read(LOAD_ADDR, len(img))
    if any(cur) and cur != img:
        sys.exit(f"RAM at {LOAD_ADDR:#x} is not empty (another payload loaded?); reboot the nano first")

    # Writes are whole 512 B chunks: pad the last one with what the device already holds.
    pad = -len(img) % CHUNK
    data = img + (d.read(LOAD_ADDR + len(img), pad) if pad else b"")
    for off in range(0, len(data), CHUNK):
        d.write(LOAD_ADDR + off, data[off:off + CHUNK])
    if d.read(LOAD_ADDR, len(img)) != img:
        sys.exit("upload verify failed; not running it")

    d.call(LOAD_ADDR)
    time.sleep(0.5)
    site = d.read(ID_WRITER, 4)
    if site == ID_WRITER_ORIG:
        sys.exit("payload ran but the hook is not in place")
    print(f"patched (hook at {ID_WRITER:#x} -> {site.hex()}). Dock the nano now; "
          "for charging, plug the charger into the dock before docking.")


if __name__ == "__main__":
    main()
