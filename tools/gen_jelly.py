"""Turns digit masks (tools\\export_masks.ps1) into 'jelly' sprites: glossy translucent gel digits.
Each sprite pixel stores 4 bytes: A coverage, T tone (0 = dark saturated rim .. 255 = light body),
H white highlight, G soft glow outside the shape. The firmware colours them per digit position.
Writes main\\jelly.h and a preview PNG.   Usage: python gen_jelly.py [masks.bin] [DH] [DW]"""
import math, os, struct, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
MASKS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "assets", "masks.bin")
DH = int(sys.argv[2]) if len(sys.argv) > 2 else 100      # digit height in screen pixels
DW = int(sys.argv[3]) if len(sys.argv) > 3 else 39       # widest digit in screen pixels
GAP, CD, CGAP, PAD = 3, 12, 4, 7                        # gap between digits, colon dot diameter, colon extra width, sprite padding
SS = 4                                                   # supersampling
R_EDGE = 6.5                                             # pillow radius in screen pixels
DIL = float(os.environ.get("JELLY_DIL", "3.0"))          # how much the strokes are thickened on each side, in screen pixels
SPARKLES = int(os.environ.get("JELLY_SPARKLES", "2"))    # 4-point glints per digit
rng = np.random.default_rng(7)

# ---------------------------------------------------------------- small image helpers
def blur1d(a, sigma, axis):
    r = max(1, int(3 * sigma + 1)); k = np.exp(-(np.arange(-r, r + 1) ** 2) / (2.0 * sigma * sigma)); k /= k.sum()
    pad = [(0, 0), (0, 0)]; pad[axis] = (r, r)
    ap = np.pad(a, pad, mode="edge"); out = np.zeros_like(a, dtype=np.float64); n = a.shape[axis]
    for i, w in enumerate(k):
        sl = [slice(None), slice(None)]; sl[axis] = slice(i, i + n); out += w * ap[tuple(sl)]
    return out

def blur(a, sigma): return blur1d(blur1d(a.astype(np.float64), sigma, 0), sigma, 1)

def resize(a, h2, w2):
    ys = np.linspace(0, a.shape[0] - 1, h2); xs = np.linspace(0, a.shape[1] - 1, w2)
    y0 = np.floor(ys).astype(int); y1 = np.minimum(y0 + 1, a.shape[0] - 1); fy = (ys - y0)[:, None]
    x0 = np.floor(xs).astype(int); x1 = np.minimum(x0 + 1, a.shape[1] - 1); fx = (xs - x0)[None, :]
    t = a[y0][:, x0] * (1 - fx) + a[y0][:, x1] * fx
    b = a[y1][:, x0] * (1 - fx) + a[y1][:, x1] * fx
    return t * (1 - fy) + b * fy

def inside_distance(mask):                                # exact distance from each inside pixel to the nearest outside pixel
    inside = mask > 0.5
    edge = inside & ~(np.roll(inside, 1, 0) & np.roll(inside, -1, 0) & np.roll(inside, 1, 1) & np.roll(inside, -1, 1))
    outside_edge = ~inside & (np.roll(inside, 1, 0) | np.roll(inside, -1, 0) | np.roll(inside, 1, 1) | np.roll(inside, -1, 1))
    by, bx = np.nonzero(outside_edge)
    iy, ix = np.nonzero(inside)
    d = np.zeros(mask.shape)
    for s in range(0, len(iy), 1500):
        yy = iy[s:s + 1500, None] - by[None, :]; xx = ix[s:s + 1500, None] - bx[None, :]
        d[iy[s:s + 1500], ix[s:s + 1500]] = np.sqrt((yy * yy + xx * xx).min(axis=1).astype(np.float64))
    return d

def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t)

# ---------------------------------------------------------------- the jelly shader
def dilate(mask_ss, r):                                                   # grow a shape by r supersampled pixels (round)
    inside = mask_ss > 0.5
    dist = inside_distance(~inside)                                       # for background pixels: distance to the shape
    return (inside | ((~inside) & (dist <= r))).astype(np.float64)

def jelly(mask_ss, bubbles=7):
    """mask_ss: float 0..1 at SS x supersampling (padded). Returns A, T, H at screen resolution and the raw coverage."""
    m = dilate(mask_ss, DIL * SS)                                         # puff the strokes up
    m = (blur(m, 1.2 * SS) > 0.5).astype(np.float64)                      # rounder corners than the font has
    m = blur(m, 0.5 * SS)                                                 # soft edge for anti-aliasing
    d = inside_distance(m) / SS                                           # distance from the edge in screen pixels
    t = np.clip(d / R_EDGE, 0, 1)
    z = R_EDGE * np.sqrt(1 - (1 - t) ** 2)                                # circular "pillow" height
    z = blur(z, 0.6 * SS)
    zy, zx = np.gradient(z); zx *= SS; zy *= SS                           # slope in screen-pixel units
    nz = 1.0 / np.sqrt(zx * zx + zy * zy + 1.0); nx = -zx * nz; ny = -zy * nz
    L = np.array([-0.45, -0.6, 0.66]); L /= np.linalg.norm(L)
    diffuse = np.clip(nx * L[0] + ny * L[1] + nz * L[2], 0, 1)
    nxy = np.sqrt(nx * nx + ny * ny) + 1e-9
    ux, uy = nx / nxy, ny / nxy                                           # which way the surface tilts (screen xy)
    ul = np.clip(ux * -0.6 + uy * -0.8, 0, 1) ** 1.4                      # tilts toward the upper left
    lr = np.clip(ux * 0.6 + uy * 0.8, 0, 1) ** 1.4                        # tilts toward the lower right
    inner = (d > 0.3)
    # tone: dark saturated rim, lighter body, light leaking in along lower inner edges
    rim = 1.0 - smoothstep(0.0, 2.4, d)
    glow_low = np.clip(ny, 0, 1) ** 1.5 * (1 - t) * 0.5 + lr * 0.12 * (1 - smoothstep(0, 5, d))
    inset_line = np.exp(-((d - 2.9) / 0.6) ** 2)                          # thin darker line just inside the edge (double outline)
    T = np.clip(0.62 + 0.28 * diffuse - 0.55 * rim - 0.22 * inset_line + glow_low, 0, 1)
    # highlights: a white strip just inside the upper-left edge, a weaker one lower right, a broad sheen, bubbles
    band = np.exp(-((d - 3.2) / 1.45) ** 2)
    H = band * ul * 0.95 + np.exp(-((d - 3.3) / 0.9) ** 2) * lr * 0.35
    H += np.clip(nx * (L[0] + 0) + ny * L[1] + nz * (L[2] + 1), 0, 2) / 2.0 ** 0   # blinn-phong half vector part, below
    hv = np.array([L[0], L[1], L[2] + 1.0]); hv /= np.linalg.norm(hv)
    spec = np.clip(nx * hv[0] + ny * hv[1] + nz * hv[2], 0, 1) ** 36
    H = np.clip(band * ul * 1.15 + np.exp(-((d - 3.3) / 0.9) ** 2) * lr * 0.4 + spec * 0.6, 0, 1)
    ys, xs = np.nonzero(d > 4.5 * 1.0)
    if len(ys):
        for _ in range(bubbles):
            k = rng.integers(len(ys)); cy, cx = ys[k], xs[k]; rad = rng.uniform(0.55, 1.15) * SS
            yy, xx = np.ogrid[:d.shape[0], :d.shape[1]]
            dist = np.sqrt((yy - cy) ** 2 + (xx - cx) ** 2)
            H = np.maximum(H, np.clip(1.4 - dist / rad, 0, 1) * 0.9)
    # 4-point sparkle glints, like the stars on the reference digits
    ys2, xs2 = np.nonzero(d > 4.0)
    if len(ys2):
        yy, xx = np.ogrid[:d.shape[0], :d.shape[1]]
        for _ in range(SPARKLES):
            k = rng.integers(len(ys2)); cy, cx = ys2[k], xs2[k]
            dy, dx = (yy - cy) / SS, (xx - cx) / SS                       # screen pixels from the glint centre
            L = rng.uniform(3.0, 4.6)
            arm_h = np.exp(-(dy / 0.42) ** 2) * np.clip(1 - np.abs(dx) / L, 0, 1)
            arm_v = np.exp(-(dx / 0.42) ** 2) * np.clip(1 - np.abs(dy) / L, 0, 1)
            core = np.exp(-(dx * dx + dy * dy) / 1.2)
            H = np.maximum(H, np.clip(np.maximum(arm_h, arm_v) * 1.1 + core * 0.8, 0, 1))
    H *= inner
    return m, T, H

def downsample(a, w=None):
    h2, w2 = a.shape[0] // SS, a.shape[1] // SS
    a = a[:h2 * SS, :w2 * SS].reshape(h2, SS, w2, SS)
    return a.mean(axis=(1, 3))

def make_sprite(mask_ss):
    m, T, H = jelly(mask_ss)
    A = downsample(m)
    Tn = downsample(T * m); Hn = downsample(H * m)
    safe = np.where(A > 1e-3, A, 1.0)
    Tn = np.where(A > 1e-3, Tn / safe, 0); Hn = np.where(A > 1e-3, Hn / safe, 0)
    G = np.clip(blur(np.pad(A, ((1, 0), (0, 0)))[:-1], 2.6) * 1.15, 0, 1)         # soft glow, shifted 1 px down
    q = lambda v: np.clip(np.rint(v * 255), 0, 255).astype(np.uint8)
    return np.stack([q(A), q(Tn), q(Hn), q(G)], axis=-1)

# ---------------------------------------------------------------- read masks, scale to the digit box
data = open(MASKS, "rb").read(); o = 2; glyphs = []
for _ in range(struct.unpack_from("<H", data, 0)[0]):
    x, y, w, h = struct.unpack_from("<hhHH", data, o); o += 8
    glyphs.append((x, y, w, h, np.frombuffer(data, np.uint8, w * h, o).reshape(h, w) / 255.0)); o += w * h
top = min(g[1] for g in glyphs); bot = max(g[1] + g[3] for g in glyphs)
wmax = max(g[2] for g in glyphs)
sy = (DH - 2 * DIL) * SS / (bot - top); sx = (DW - 2 * DIL) * SS / wmax   # the dilation adds DIL on every side
cell_w = DW + GAP
sprites = []
for (x, y, w, h, a) in glyphs:
    gh, gw = int(round(h * sy)), int(round(w * sx))
    scaled = resize(a, gh, gw)
    cw, ch = (DW + 2 * PAD) * SS, (DH + 2 * PAD) * SS
    canvas = np.zeros((ch, cw)); oy = int(round((y - top) * sy + (PAD + DIL) * SS)); ox = (cw - gw) // 2
    canvas[oy:oy + gh, ox:ox + gw] = scaled
    sprites.append(make_sprite(canvas))
# colon: two round dots
cw, ch = (CD + 2 * PAD) * SS, (DH + 2 * PAD) * SS
canvas = np.zeros((ch, cw)); yy, xx = np.mgrid[:ch, :cw]
for cy in (PAD + DH * 0.30, PAD + DH * 0.70):
    canvas[np.sqrt((yy - cy * SS) ** 2 + (xx - cw / 2) ** 2) <= CD * SS / 2] = 1.0
sprites.append(make_sprite(canvas))

# ---------------------------------------------------------------- header
colon_cell = CD + CGAP
total = 6 * cell_w + 2 * colon_cell
out = ["// Generated by tools/gen_jelly.py. Do not edit by hand.", "#pragma once", "#include <stdint.h>", "",
       "#define JELLY_H       %d      // sprite height (digit height plus padding)" % sprites[0].shape[0],
       "#define JELLY_PAD     %d" % PAD, "#define DIGIT_H       %d" % DH,
       "#define DIGIT_CELL_W  %d" % cell_w, "#define COLON_CELL_W  %d" % colon_cell, "#define CLOCK_W       %d" % total, "",
       "typedef struct { uint32_t off; uint8_t w, h; int8_t xoff; } jglyph_t;   // xoff: sprite's left edge relative to the cell's left edge", "",
       "// 4 bytes per pixel: coverage, tone, highlight, glow"]
px = bytearray(); tab = []
for i, s in enumerate(sprites):
    cell = colon_cell if i == 10 else cell_w
    tab.append("    {%d, %d, %d, %d}, /* '%s' */" % (len(px), s.shape[1], s.shape[0], (cell - s.shape[1]) // 2, ":" if i == 10 else i))
    px += s.tobytes()
out.append("static const uint8_t jelly_px[%d] = {" % len(px))
for i in range(0, len(px), 32):
    out.append("    " + ",".join(str(b) for b in px[i:i + 32]) + ",")
out.append("};")
out.append("static const jglyph_t jelly_glyphs[11] = {   // '0'..'9', then ':'")
out += tab; out.append("};")
open(os.path.join(HERE, "..", "main", "jelly.h"), "w", newline="\n").write("\n".join(out) + "\n")
print("digit sprites %dx%d, colon %dx%d, clock %d px wide, %d bytes of sprite data" % (sprites[0].shape[1], sprites[0].shape[0], sprites[10].shape[1], sprites[10].shape[0], total, len(px)))

# ---------------------------------------------------------------- preview (same colour maths as the firmware)
PAL = [((255, 150, 215), (190, 15, 125)), ((255, 170, 135), (210, 40, 40)), ((255, 215, 90), (225, 125, 10)),
       ((225, 245, 90), (130, 180, 10)), ((150, 235, 110), (30, 145, 45)), ((120, 235, 215), (10, 135, 150))]
COLON = [((255, 190, 110), (220, 90, 20)), ((190, 240, 100), (90, 170, 30))]
W, H = 320, 172
img = np.zeros((H, W, 3))
yy = np.arange(H)[:, None] / (H - 1.0)
img[...] = np.stack([9 + 14 * yy + 0 * np.arange(W)[None, :], 12 + 0 * yy + 0 * np.arange(W)[None, :], 36 + 12 * yy + 0 * np.arange(W)[None, :]], -1)
def blend_sprite(s, x0, y0, light, dark, shadow_pass):
    h, w = s.shape[:2]
    for j in range(h):
        y = y0 + j
        if not 0 <= y < H: continue
        for i in range(w):
            x = x0 + i
            if not 0 <= x < W: continue
            a, t, hh, g = s[j, i] / 255.0
            if shadow_pass:
                if g > 0: img[y, x] += (np.array(dark) * 0.9 - img[y, x]) * g * 0.45
            else:
                if a <= 0: continue
                c = np.array(dark) + (np.array(light) - np.array(dark)) * t
                c = c + (255 - c) * hh
                img[y, x] += (c - img[y, x]) * a
text = "123456"
order = [(0, 0), (1, 1), None, (2, 2), (3, 3), None, (4, 4), (5, 5)]
digs = [int(c) for c in "123456"]
for shadow in (True, False):
    x = (W - total) // 2; di = 0; ci = 0
    for slot in "DD:DD:DD":
        if slot == ":":
            s = sprites[10]; light, dark = COLON[ci]; ci += 1; cell = colon_cell; gi = 10
        else:
            s = sprites[digs[di]]; light, dark = PAL[di]; di += 1; cell = cell_w
        blend_sprite(s, x + (cell - s.shape[1]) // 2, (H - DH) // 2 - PAD, light, dark, shadow)
        x += cell
png = np.clip(img + 0.5, 0, 255).astype(np.uint8)
K = 3
big = np.repeat(np.repeat(png, K, 0), K, 1)
raw = b"".join(b"\x00" + big[r].tobytes() for r in range(big.shape[0]))
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
prev = os.path.join(HERE, "..", "assets", "jelly_preview.png")
open(prev, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W * K, H * K, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
print("preview:", os.path.abspath(prev))
