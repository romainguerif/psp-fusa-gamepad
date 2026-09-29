#!/usr/bin/env python3
"""PSP Bridge XMB art: ICON0.PNG (144x80) and PIC1.PNG (480x272).
A PSP linked to a laptop by a glowing bridge arc. Drawn at 4x, downscaled.
Run: /opt/homebrew/bin/python3 art/make_icons.py  (writes src/app/icon0.png, pic1.png)"""
import math, os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "src", "app")
CYAN = (40, 230, 255)
MAGENTA = (255, 40, 220)
BODY = (26, 28, 44)
EDGE = (120, 130, 170)
SCREEN_ON = (18, 60, 90)

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def gradient(w, h, top, bottom):
    g = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(g)
    for y in range(h):
        d.line([(0, y), (w, y)], fill=lerp(top, bottom, y / max(1, h - 1)))
    return g

def arc_points(p0, p1, lift, n=200):
    (x0, y0), (x1, y1) = p0, p1
    cx, cy = (x0 + x1) / 2, min(y0, y1) - lift
    pts = []
    for i in range(n + 1):
        t = i / n
        x = (1 - t) ** 2 * x0 + 2 * (1 - t) * t * cx + t * t * x1
        y = (1 - t) ** 2 * y0 + 2 * (1 - t) * t * cy + t * t * y1
        pts.append((x, y, t))
    return pts

def draw_arc(img, p0, p1, lift, width, glow):
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    pts = arc_points(p0, p1, lift)
    for (x, y, t), (x2, y2, _) in zip(pts, pts[1:]):
        c = lerp(CYAN, MAGENTA, t)
        d.line([(x, y), (x2, y2)], fill=c + (255,), width=width)
    halo = layer.filter(ImageFilter.GaussianBlur(glow))
    img.alpha_composite(halo)
    img.alpha_composite(halo)
    img.alpha_composite(layer)
    # signal dots riding the bridge
    d = ImageDraw.Draw(img)
    for t in (0.3, 0.5, 0.7):
        x, y, _ = pts[int(t * (len(pts) - 1))]
        r = width * 0.9
        d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, 255))

def draw_psp(img, cx, cy, w):
    d = ImageDraw.Draw(img)
    h = w * 0.44
    x0, y0, x1, y1 = cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2
    d.rounded_rectangle([x0, y0, x1, y1], radius=h * 0.48, fill=BODY, outline=EDGE, width=max(2, int(w * 0.015)))
    sw, sh = w * 0.52, h * 0.66
    d.rounded_rectangle([cx - sw / 2, cy - sh / 2, cx + sw / 2, cy + sh / 2], radius=w * 0.02, fill=SCREEN_ON)
    # tiny tracker rows on the screen
    for i in range(4):
        yy = cy - sh / 2 + sh * (0.2 + 0.2 * i)
        d.line([(cx - sw * 0.38, yy), (cx + sw * (0.1 + 0.08 * (i % 2)), yy)], fill=CYAN if i == 1 else (90, 150, 190), width=max(1, int(w * 0.018)))
    # d-pad
    px, py, s = x0 + w * 0.12, cy, w * 0.045
    d.rectangle([px - s, py - s * 3, px + s, py + s * 3], fill=EDGE)
    d.rectangle([px - s * 3, py - s, px + s * 3, py + s], fill=EDGE)
    # face buttons
    bx, by, r, g = x1 - w * 0.12, cy, w * 0.03, w * 0.065
    for dx, dy, col in ((0, -g, (80, 220, 170)), (g, 0, (255, 90, 110)), (0, g, (90, 150, 255)), (-g, 0, (230, 120, 230))):
        d.ellipse([bx + dx - r, by + dy - r, bx + dx + r, by + dy + r], fill=col)
    return (cx, y0)

def draw_laptop(img, cx, cy, w):
    d = ImageDraw.Draw(img)
    sh = w * 0.62
    sx0, sy0, sx1, sy1 = cx - w / 2, cy - sh, cx + w / 2, cy
    d.rounded_rectangle([sx0, sy0, sx1, sy1], radius=w * 0.05, fill=BODY, outline=EDGE, width=max(2, int(w * 0.02)))
    m = w * 0.07
    inner = [sx0 + m, sy0 + m, sx1 - m, sy1 - m]
    d.rectangle(inner, fill=SCREEN_ON)
    # the game, big: a sun over hills
    ix0, iy0, ix1, iy1 = inner
    iw, ih = ix1 - ix0, iy1 - iy0
    d.ellipse([ix0 + iw * 0.58, iy0 + ih * 0.15, ix0 + iw * 0.82, iy0 + ih * 0.15 + iw * 0.24], fill=MAGENTA)
    d.polygon([(ix0, iy1), (ix0 + iw * 0.3, iy0 + ih * 0.5), (ix0 + iw * 0.55, iy1)], fill=(40, 170, 200))
    d.polygon([(ix0 + iw * 0.35, iy1), (ix0 + iw * 0.7, iy0 + ih * 0.62), (ix1, iy1)], fill=CYAN)
    # base
    bw, bh = w * 1.22, w * 0.07
    d.rounded_rectangle([cx - bw / 2, sy1, cx + bw / 2, sy1 + bh], radius=bh / 2, fill=EDGE)
    return (cx, sy0)

def icon0():
    S = 4
    W, H = 144 * S, 80 * S
    bg = gradient(W, H, (14, 16, 40), (29, 10, 31)).convert("RGBA")
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, H - 1], radius=18 * S, fill=255)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    img.paste(bg, (0, 0), mask)
    top_psp = draw_psp(img, 40 * S, 58 * S, 64 * S)
    top_lap = draw_laptop(img, 108 * S, 62 * S, 44 * S)
    draw_arc(img, (top_psp[0], top_psp[1] + 2 * S), (top_lap[0], top_lap[1] + 1 * S), 26 * S, 3 * S, 5 * S)
    ImageDraw.Draw(img).rounded_rectangle([1, 1, W - 2, H - 2], radius=18 * S, outline=(255, 0, 221, 160), width=S)
    return img.resize((144, 80), Image.LANCZOS)

def pic1():
    S = 2
    W, H = 480 * S, 272 * S
    img = gradient(W, H, (10, 12, 30), (29, 10, 31)).convert("RGBA")
    d = ImageDraw.Draw(img)
    # faint perspective grid at the bottom
    hy = int(H * 0.62)
    for i in range(-12, 13):
        d.line([(W / 2 + i * 12 * S, hy), (W / 2 + i * 90 * S, H)], fill=(255, 0, 221, 40), width=S)
    for k in range(1, 9):
        y = hy + (H - hy) * (k / 8) ** 1.7
        d.line([(0, y), (W, y)], fill=(40, 230, 255, 35), width=S)
    # the bridge, large, on the right (the XMB writes the title top-left)
    top_psp = draw_psp(img, 250 * S, 200 * S, 130 * S)
    top_lap = draw_laptop(img, 400 * S, 206 * S, 90 * S)
    draw_arc(img, (top_psp[0], top_psp[1] + 3 * S), (top_lap[0], top_lap[1] + 2 * S), 70 * S, 4 * S, 10 * S)
    return img.resize((480, 272), Image.LANCZOS).convert("RGB")

if __name__ == "__main__":
    icon0().save(os.path.join(OUT, "icon0.png"))
    pic1().save(os.path.join(OUT, "pic1.png"))
    print("wrote", os.path.join(OUT, "icon0.png"), "and pic1.png")
