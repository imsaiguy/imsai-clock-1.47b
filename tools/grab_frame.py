"""Ask the board for its frame buffer and save it as a PNG (3x enlarged).
Usage: python grab_frame.py COM9 out.png"""
import struct, sys, time, zlib
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
out = sys.argv[2] if len(sys.argv) > 2 else "frame.png"
W, H, K = 320, 172, 3
with serial.Serial() as s:
    s.port, s.baudrate, s.timeout = port, 115200, 1
    s.dtr = False
    s.rts = False
    s.open()
    s.reset_input_buffer()
    s.write(b"D\n")
    rows, begun, t0 = {}, False, time.time()
    buf = b""
    while time.time() - t0 < 20:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if line == b"FBBEGIN":
                begun = True
            elif line == b"FBEND":
                t0 = 0
                break
            elif begun and line.startswith(b"R") and len(line) == 4 + W * 4:
                rows[int(line[1:4])] = bytes.fromhex(line[4:].decode())
        else:
            continue
        break
if len(rows) != H:
    sys.exit("got %d of %d rows" % (len(rows), H))

def px(row, x):   # the buffer holds each pixel high byte first, so the bytes read as plain big-endian RGB565
    v = (row[2 * x] << 8) | row[2 * x + 1]
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))

raw = bytearray()
for y in range(H):
    line = b"".join(px(rows[y], x) * K for x in range(W))
    raw += (b"\x00" + line) * K
def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W * K, H * K, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
print("saved", out)
