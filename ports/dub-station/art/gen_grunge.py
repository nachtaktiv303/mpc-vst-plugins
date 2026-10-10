#!/usr/bin/env python3
"""Procedural weathering overlay for Dub Station (RE-201 vibe).

Generates a transparent RGBA PNG (1280x628) that is alpha-composited over the
chassis + panels by the browser renderer (`art file=art/grunge.png ...`).
Dark marks darken, light marks lighten whatever is underneath, so one pass
works over the silver strips, the green faceplate and the black fields alike.

    python3 gen_grunge.py [amount]     amount 0..1 (default 0.6), -> grunge.png

Deterministic (fixed seed) so rebuilds are stable.
"""
import sys, math
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

W, H = 1280, 628
AMT = float(sys.argv[1]) if len(sys.argv) > 1 else 0.6
rng = np.random.default_rng(20261007)

# accumulate light (adds) and dark (subtracts) in a signed float buffer, plus alpha coverage
light = np.zeros((H, W), np.float32)   # bright scuffs / dust highlights
dark  = np.zeros((H, W), np.float32)   # grime / stains / scratches / shadow

def blurred_noise(scale, lo=0.0, hi=1.0):
    """Low-frequency blotches: coarse random grid, upscaled + blurred."""
    gh, gw = max(2, H // scale), max(2, W // scale)
    g = rng.random((gh, gw)).astype(np.float32)
    im = Image.fromarray((g * 255).astype(np.uint8)).resize((W, H), Image.BICUBIC)
    im = im.filter(ImageFilter.GaussianBlur(scale * 0.4))
    a = np.asarray(im, np.float32) / 255.0
    return lo + (hi - lo) * a

# --- 1. patina / uneven grime: large soft dark blotches, a few lighter rubbed areas
blotch = blurred_noise(90)
dark += np.clip(blotch - 0.55, 0, 1) * 0.9        # darker patches where blotch is high
light += np.clip(0.35 - blotch, 0, 1) * 0.35      # faint rubbed-clean lighter patches

# --- 2. fine dust / speckle across everything
spk = rng.random((H, W)).astype(np.float32)
dark  += (spk > 0.985) * (spk - 0.985) * 40.0 * 0.5
light += (spk < 0.012) * (0.012 - spk) * 40.0 * 0.4

# --- 3. scratches (thin lines; mostly horizontal on the silver strips, varied elsewhere)
ld = Image.new("L", (W, H), 0); dd = Image.new("L", (W, H), 0)
lD = ImageDraw.Draw(ld); dD = ImageDraw.Draw(dd)
def scratch(draw, n, hmax, length, wmin, wmax, val, horiz_bias):
    for _ in range(n):
        x0 = rng.integers(0, W); y0 = rng.integers(0, hmax if hmax else H)
        if hmax and hmax < H and rng.random() < 0.5:   # bottom strip too
            y0 = rng.integers(H - 70, H)
        ang = rng.normal(0, 0.12) if rng.random() < horiz_bias else rng.uniform(-math.pi, math.pi)
        L = rng.integers(length[0], length[1])
        x1 = x0 + math.cos(ang) * L; y1 = y0 + math.sin(ang) * L
        draw.line([(x0, y0), (x1, y1)], fill=int(val), width=int(rng.integers(wmin, wmax)))

# brushed horizontal scuffs on the top silver strip (y 0..72) and bottom (y 558..628)
# very sparse + faint -- the strips had far too many visible lines
scratch(lD, 22, 72, (60, 300), 1, 2, 38, 0.99)    # bright brush lines
scratch(dD, 12, 72, (40, 220), 1, 2, 28, 0.99)    # dark brush lines
# general scratches over the whole faceplate -- a bit more visible (more + stronger + longer)
scratch(dD, 26, 0, (40, 230), 1, 2, 44, 0.7)
scratch(lD, 16, 0, (30, 180), 1, 2, 34, 0.7)
# blur a touch more so lines soften into scuffs rather than crisp strokes
light += np.asarray(ld.filter(ImageFilter.GaussianBlur(0.9)), np.float32) / 255.0
dark  += np.asarray(dd.filter(ImageFilter.GaussianBlur(0.9)), np.float32) / 255.0

# --- 4. edge wear + vignette (darker toward the borders and corners)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
nx = (xx - W / 2) / (W / 2); ny = (yy - H / 2) / (H / 2)
rad = np.sqrt(nx * nx + ny * ny)
dark += np.clip(rad - 0.72, 0, 1) * 0.6            # vignette
edge = np.minimum.reduce([xx, W - 1 - xx, yy, H - 1 - yy])
dark += np.clip(1.0 - edge / 10.0, 0, 1) * 0.5     # grimy outer rim

# a couple of worn corners (brass-rub feel): warm light smudge
for cx, cy in [(18, 18), (W - 18, 18), (18, H - 18), (W - 18, H - 18)]:
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    light += np.clip(1.0 - d / 90.0, 0, 1) * 0.3

# --- compose to RGBA. Light -> whitish, dark -> blackish; alpha = how much it shows.
light = np.clip(light, 0, 1); dark = np.clip(dark, 0, 1)
# give the dark grime a faintly warm/brown tint, highlights a cool silver tint
out = np.zeros((H, W, 4), np.float32)
a = np.clip((light * 0.9 + dark) * AMT, 0, 0.85)
# colour: blend between a warm-dark and a cool-light depending on which dominates
dom = dark / (dark + light + 1e-6)
col_dark = np.array([34, 28, 20], np.float32)      # warm umber grime
col_lite = np.array([235, 238, 233], np.float32)   # cool dust/scuff
for c in range(3):
    out[:, :, c] = col_dark[c] * dom + col_lite[c] * (1 - dom)
out[:, :, 3] = a * 255.0

Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save("grunge.png")
print("grunge.png written (amount=%.2f, max alpha=%.0f)" % (AMT, a.max() * 255))

# --- light grime for the black inset panels: mostly bright dust + faint scratches, no
# vignette (panels are small, stretched into each field). Dark marks drop out on black
# anyway, so this layer is light-weighted.
PW, PH = 560, 500
r2 = np.random.default_rng(770411)
pl = np.zeros((PH, PW), np.float32)
sp = r2.random((PH, PW)).astype(np.float32)
pl += (sp < 0.020) * (0.020 - sp) * 30.0           # dust specks (light)
pd = Image.new("L", (PW, PH), 0); pD = ImageDraw.Draw(pd)
for _ in range(90):                                 # faint light scratches
    x0 = r2.integers(0, PW); y0 = r2.integers(0, PH)
    ang = r2.uniform(-math.pi, math.pi); L = r2.integers(20, 160)
    pD.line([(x0, y0), (x0 + math.cos(ang) * L, y0 + math.sin(ang) * L)],
            fill=int(r2.integers(40, 90)), width=1)
pl += np.asarray(pd.filter(ImageFilter.GaussianBlur(0.5)), np.float32) / 255.0
# a couple of soft smudges (very low alpha, slightly light)
gh, gw = PH // 70, PW // 70
g = r2.random((max(2, gh), max(2, gw))).astype(np.float32)
sm = np.asarray(Image.fromarray((g * 255).astype(np.uint8)).resize((PW, PH), Image.BICUBIC)
                .filter(ImageFilter.GaussianBlur(28)), np.float32) / 255.0
pl += np.clip(sm - 0.6, 0, 1) * 0.25
pa = np.clip(pl * AMT * 0.55, 0, 0.4)               # keep the panels subtle
pout = np.zeros((PH, PW, 4), np.float32)
pout[:, :, 0] = 225; pout[:, :, 1] = 228; pout[:, :, 2] = 223
pout[:, :, 3] = pa * 255.0
Image.fromarray(np.clip(pout, 0, 255).astype(np.uint8)).save("grime_panel.png")
print("grime_panel.png written (max alpha=%.0f)" % (pa.max() * 255))
