"""Draws icon.png (48x48), banner.png (256x128) and banner.wav for Canvas 3DS.
Standard library only. Usage: python make_art.py <project dir>
"""
import math, struct, sys, wave, zlib

ROOT = sys.argv[1] if len(sys.argv) > 1 else ".."
SS = 4


class Canvas:
    def __init__(self, w, h, bg):
        self.w, self.h = w * SS, h * SS
        self.px = [list(bg) for _ in range(self.w * self.h)]

    def blend(self, x, y, c, a=1.0):
        if 0 <= x < self.w and 0 <= y < self.h:
            p = self.px[y * self.w + x]
            for i in range(3):
                p[i] = p[i] * (1 - a) + c[i] * a

    def poly(self, pts, c, a=1.0):
        pts = [(x * SS, y * SS) for x, y in pts]
        y0, y1 = int(min(p[1] for p in pts)), int(max(p[1] for p in pts)) + 1
        for y in range(max(0, y0), min(self.h, y1)):
            yc = y + 0.5
            xs = []
            for i in range(len(pts)):
                (xa, ya), (xb, yb) = pts[i], pts[(i + 1) % len(pts)]
                if (ya <= yc < yb) or (yb <= yc < ya):
                    xs.append(xa + (yc - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for k in range(0, len(xs) - 1, 2):
                for x in range(int(xs[k] + 0.5), int(xs[k + 1] + 0.5)):
                    self.blend(x, y, c, a)

    def rect(self, x, y, w, h, c, a=1.0):
        self.poly([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], c, a)

    def rrect(self, x, y, w, h, r, top, bot):
        for yy in range(int(y * SS), int((y + h) * SS)):
            t = (yy / SS - y) / h
            col = [top[i] * (1 - t) + bot[i] * t for i in range(3)]
            for xx in range(int(x * SS), int((x + w) * SS)):
                fx, fy = xx / SS, yy / SS
                cx = min(max(fx, x + r), x + w - r)
                cy = min(max(fy, y + r), y + h - r)
                if (fx - cx) ** 2 + (fy - cy) ** 2 <= r * r:
                    self.blend(xx, yy, col)

    def save_png(self, path):
        w, h = self.w // SS, self.h // SS
        raw = bytearray()
        for y in range(h):
            raw.append(0)
            for x in range(w):
                acc = [0, 0, 0]
                for sy in range(SS):
                    for sx in range(SS):
                        p = self.px[(y * SS + sy) * self.w + x * SS + sx]
                        for i in range(3):
                            acc[i] += p[i]
                raw += bytes(min(255, int(v / (SS * SS))) for v in acc) + b"\xff"

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
        open(path, "wb").write(png)


def book(cv, cx, cy, s):
    """Open book seen from the front, centered at (cx, cy), width ~2s."""
    white, page, line = (255, 255, 255), (236, 238, 242), (190, 196, 206)
    # left and right pages
    cv.poly([(cx - s, cy - 0.55 * s), (cx - 0.05 * s, cy - 0.4 * s), (cx - 0.05 * s, cy + 0.7 * s), (cx - s, cy + 0.55 * s)], white)
    cv.poly([(cx + s, cy - 0.55 * s), (cx + 0.05 * s, cy - 0.4 * s), (cx + 0.05 * s, cy + 0.7 * s), (cx + s, cy + 0.55 * s)], page)
    # text lines
    for i in range(4):
        y = cy - 0.3 * s + i * 0.22 * s
        cv.poly([(cx - 0.85 * s, y - 0.1 * s + 0.03 * i * s), (cx - 0.2 * s, y), (cx - 0.2 * s, y + 0.06 * s), (cx - 0.85 * s, y - 0.04 * s + 0.03 * i * s)], line)
        cv.poly([(cx + 0.85 * s, y - 0.1 * s + 0.03 * i * s), (cx + 0.2 * s, y), (cx + 0.2 * s, y + 0.06 * s), (cx + 0.85 * s, y - 0.04 * s + 0.03 * i * s)], line)
    # bookmark
    cv.poly([(cx + 0.45 * s, cy - 0.48 * s), (cx + 0.6 * s, cy - 0.5 * s), (cx + 0.6 * s, cy + 0.05 * s), (cx + 0.52 * s, cy - 0.05 * s), (cx + 0.45 * s, cy + 0.07 * s)], (214, 69, 40))


FONT = {
    "C": ["0111", "1000", "1000", "1000", "1000", "1000", "0111"],
    "A": ["0110", "1001", "1001", "1111", "1001", "1001", "1001"],
    "N": ["1001", "1101", "1101", "1011", "1011", "1001", "1001"],
    "V": ["1001", "1001", "1001", "1001", "1001", "0110", "0110"],
    "S": ["0111", "1000", "1000", "0110", "0001", "0001", "1110"],
    "3": ["1110", "0001", "0001", "0110", "0001", "0001", "1110"],
    "D": ["1110", "1001", "1001", "1001", "1001", "1001", "1110"],
}


def text(cv, s, x, y, px, col):
    for ch in s:
        g = FONT[ch]
        for r, row in enumerate(g):
            for c, bit in enumerate(row):
                if bit == "1":
                    cv.rect(x + c * px, y + r * px, px, px, col)
        x += (len(g[0]) + 1) * px


def main():
    icon = Canvas(48, 48, (0, 0, 0))
    icon.rrect(1, 1, 46, 46, 9, (232, 96, 54), (190, 52, 30))
    book(icon, 24, 24, 17)
    icon.save_png(f"{ROOT}/icon.png")

    ban = Canvas(256, 128, (0, 0, 0))
    ban.rrect(0, 0, 256, 128, 1, (44, 60, 74), (24, 34, 44))
    ban.rrect(160, 20, 84, 84, 14, (232, 96, 54), (190, 52, 30))
    book(ban, 202, 62, 30)
    text(ban, "CANVAS", 14, 36, 4, (255, 255, 255))
    text(ban, "3DS", 14, 76, 4, (240, 120, 70))
    ban.save_png(f"{ROOT}/banner.png")

    rate, out = 22050, bytearray()
    for i in range(int(rate * 1.4)):
        t = i / rate
        v = 0.0
        for start, f in ((0.0, 659.3), (0.12, 784.0), (0.24, 1046.5)):
            if t >= start:
                tt = t - start
                v += 0.25 * math.sin(2 * math.pi * f * tt) * math.exp(-tt * 3.5)
        out += struct.pack("<h", int(max(-1, min(1, v)) * 32767))
    with wave.open(f"{ROOT}/banner.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(out))
    print("ok")


main()
