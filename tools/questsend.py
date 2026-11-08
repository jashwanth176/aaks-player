"""Send films to AaksVR's debug receiver on the Quest, straight into Movies.

The receiver is in debug builds only (app/src/debug, debugtools/TransferService). Start it:

    adb shell am start-foreground-service -n com.aaks.player.debug/com.limelight.debugtools.TransferService

then send:

    python tools/questsend.py QUEST_IP FILE [FILE...]

One plain TCP stream per file, so it runs at the Wi-Fi's speed rather than adb's. A transfer
that drops resumes from what is already on the headset when run again, and the md5 of the
bytes sent is checked against what the headset received.
"""
import hashlib
import os
import socket
import sys
import time
from urllib.parse import quote

PORT = 8765
BLOCK = 4 * 1024 * 1024


def ask(host, line):
    with socket.create_connection((host, PORT), timeout=30) as s:
        s.sendall((line + "\n").encode())
        return s.makefile().readline().strip()


def send(host, path):
    name = os.path.basename(path)
    size = os.path.getsize(path)
    have = ask(host, "STAT " + quote(name))
    if not have.startswith("LEN "):
        sys.exit("receiver said: " + have)
    offset = int(have.split()[1])
    if offset >= size:
        offset = 0 if offset > size else size
    if offset == size:
        print("%s: already there (%.2f GB)" % (name, size / 2**30))
        return
    print("%s: %.2f GB%s" % (name, size / 2**30,
                             ", resuming at %.2f GB" % (offset / 2**30) if offset else ""), flush=True)
    md5 = hashlib.md5()
    began = time.time()
    shown = began
    sent = 0
    with socket.create_connection((host, PORT), timeout=60) as s, open(path, "rb") as f:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 * 1024 * 1024)
        s.sendall(("SEND %s %d %d\n" % (quote(name), size, offset)).encode())
        f.seek(offset)
        while True:
            data = f.read(BLOCK)
            if not data:
                break
            md5.update(data)
            s.sendall(data)
            sent += len(data)
            now = time.time()
            if now - shown >= 10:
                shown = now
                rate = sent / (now - began) / 2**20
                left = (size - offset - sent) / 2**20 / max(rate, 0.1)
                print("  %.1f of %.1f GB, %.1f MB/s, %d min left" % (
                    (offset + sent) / 2**30, size / 2**30, rate, left / 60 + 0.5), flush=True)
        s.shutdown(socket.SHUT_WR)
        answer = s.makefile().readline().strip().split()
    took = time.time() - began
    ok = len(answer) == 3 and answer[0] == "OK" and int(answer[1]) == sent and answer[2] == md5.hexdigest()
    print("%s: %s, %.0f s, %.1f MB/s" % (name, "DONE, checksum matches" if ok else "FAILED: %s" % " ".join(answer),
                                         took, sent / max(took, 0.001) / 2**20), flush=True)
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    for p in sys.argv[2:]:
        send(sys.argv[1], p)
