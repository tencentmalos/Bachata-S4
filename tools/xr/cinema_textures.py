#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Original procedural PBR texture sets for the XR cinema scenes.

Every set is tileable and written as three PNGs (the Lite Engine decodes PNG
and KTX2 only): <name>_albedo.png (sRGB), <name>_orm.png (R occlusion,
G roughness, B metallic; linear) and <name>_normal.png (OpenGL/glTF tangent
space). Deterministic: a fixed seed per set, no downloaded imagery.
Output: build/xr-cinema/textures (consumed by blender_cinema_assets.py).
"""
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/xr-cinema/textures'
SHIP = 1024


def noise(n, scale, seed, aspect=1.0, power=2.0):
    """Tileable band-limited noise in [-1,1]: white noise filtered in frequency.
    scale is the feature size in pixels; aspect>1 stretches features along x."""
    rng = np.random.default_rng(seed)
    w = rng.standard_normal((n, n))
    fy = np.fft.fftfreq(n)[:, None]
    fx = np.fft.fftfreq(n)[None, :]
    r = np.sqrt((fx * aspect) ** 2 + fy ** 2) * scale
    f = np.real(np.fft.ifft2(np.fft.fft2(w) * np.exp(-r ** power)))
    f -= f.mean()
    return f / (np.abs(f).max() + 1e-9)


def fbm(n, scale, seed, octaves=4, aspect=1.0):
    total, amp, norm = 0, 1.0, 0
    for o in range(octaves):
        total = total + amp * noise(n, max(scale / 2 ** o, 1.0), seed + o * 101, aspect)
        norm += amp
        amp *= .5
    return total / norm


def normal_from_height(h, strength):
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * .5
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * .5
    # Image rows grow downward while glTF +V (OpenGL convention) grows up.
    nx, ny, nz = -dx * strength, dy * strength, np.ones_like(h)
    l = np.sqrt(nx * nx + ny * ny + nz * nz)
    return np.stack([nx / l, ny / l, nz / l], -1) * .5 + .5


def save(name, albedo, rough, metal, height, strength, ao=None):
    OUT.mkdir(parents=True, exist_ok=True)
    n = albedo.shape[0]
    ao = np.ones((n, n)) if ao is None else ao
    metal = np.broadcast_to(metal, (n, n))
    to8 = lambda a: (np.clip(a, 0, 1) * 255 + .5).astype(np.uint8)
    srgb = np.where(albedo <= .0031308, albedo * 12.92, 1.055 * np.clip(albedo, 0, 1) ** (1 / 2.4) - .055)
    # Authored at 1024 (the pattern scales assume it), shipped at SHIP px:
    # every GLB embeds its own copy, and the XR engine keeps RGBA8 + mips
    # (measured on Swan: 1024 px textures cost ~209 MiB in the lounge World).
    ship = lambda im: im.resize((SHIP, SHIP), Image.LANCZOS) if im.width > SHIP else im
    ship(Image.fromarray(to8(srgb))).save(OUT / f'{name}_albedo.png', optimize=True)
    ship(Image.fromarray(to8(np.stack([ao, rough, metal], -1)))).save(OUT / f'{name}_orm.png', optimize=True)
    ship(Image.fromarray(to8(normal_from_height(height, strength)))).save(OUT / f'{name}_normal.png', optimize=True)


def lerp(a, b, t):
    return np.asarray(a)[None, None, :] * (1 - t[..., None]) + np.asarray(b)[None, None, :] * t[..., None]


def wood_grain(n, seed, rings, aspect=24.0, warp_amp=.12):
    """Long-grain board: warped ring pattern along x plus pores."""
    y = np.linspace(0, 1, n, endpoint=False)[:, None] * np.ones((1, n))
    warp = fbm(n, n / 6, seed, 3, aspect=aspect / 4) * warp_amp + fbm(n, n / 24, seed + 7, 3, aspect=aspect) * .06
    ring = np.sin((y + warp) * rings * np.pi * 2)
    ring = (ring * .5 + .5) ** 2
    pores = np.clip(noise(n, 1.2, seed + 3, aspect=aspect * 2), -1, 1)
    return ring, pores


def planks(n, seed, board_px, length_px, base_a, base_b, rings, gap=2, rough=(.42, .62)):
    """Floor boards running along x; board_px wide, staggered random lengths."""
    rng = np.random.default_rng(seed)
    ring, pores = wood_grain(n, seed, rings)
    albedo = np.zeros((n, n, 3))
    height = np.zeros((n, n))
    rows = n // board_px
    tone = np.zeros((n, n))
    seam = np.ones((n, n))
    for r in range(rows):
        y0, y1 = r * board_px, (r + 1) * board_px
        offset = int(rng.integers(0, length_px))
        x = (np.arange(n) + offset) % length_px
        board_id = (np.arange(n) + offset) // length_px
        shade = rng.uniform(-1, 1, board_id.max() + 2)[board_id]
        tone[y0:y1] = shade[None, :]
        seam[y0:y0 + gap] = 0
        seam[y0:y1, (x < gap)] = 0
        # Each board takes its own slice of the grain.
        shift = int(rng.integers(0, n))
        ring[y0:y1] = np.roll(ring[y0:y1], shift, 1)
    t = np.clip(ring * .35 + tone * .30 + .35, 0, 1)
    albedo = lerp(base_a, base_b, t) * (1 + pores[..., None] * .05)
    albedo *= (.35 + .65 * seam)[..., None]
    height = ring * .25 + pores * .08 + seam * 1.0
    roughness = rough[0] + (rough[1] - rough[0]) * (1 - ring) * .6 + (1 - seam) * .3
    ao = .55 + .45 * seam
    return albedo, roughness, height, ao


def build():
    n = 1024
    # 1. Oak floor (texture covers 2.4 m): 0.19 m boards.
    a, r, h, ao = planks(n, 11, 81, 520, (.33, .22, .13), (.43, .29, .17), 70)
    save('oak_floor', a, r, 0, h, 3.0, ao)
    # 2. Walnut veneer (texture covers 1 m), satin finish.
    ring, pores = wood_grain(n, 23, 55, aspect=40, warp_amp=.035)
    t = np.clip(ring * .7 + fbm(n, 90, 29, 3, 8) * .2 + .2, 0, 1)
    a = lerp((.105, .060, .035), (.205, .125, .070), t) * (1 + pores[..., None] * .07)
    save('walnut', a, .38 + (1 - ring) * .14 + pores * .03, 0, ring * .15 + pores * .25, 1.6)
    # 3. Light ash (slats), texture covers 1 m.
    ring, pores = wood_grain(n, 31, 46)
    t = np.clip(ring * .6 + fbm(n, 80, 37, 3, 8) * .2 + .25, 0, 1)
    a = lerp((.42, .30, .19), (.58, .44, .30), t) * (1 + pores[..., None] * .05)
    save('ash', a, .5 + (1 - ring) * .12, 0, ring * .15 + pores * .2, 1.4)
    # 4. Warm limewash plaster (covers 2 m).
    big = fbm(n, 220, 41, 4)
    small = fbm(n, 6, 43, 3)
    a = lerp((.29, .27, .25), (.36, .335, .30), np.clip(big * .6 + .5, 0, 1)) * (1 + small[..., None] * .025)
    save('plaster', a, .86 + small * .05, 0, big * .4 + small * .25, 2.0)
    # 5. Acoustic fabric, charcoal blue (covers 0.5 m): plain weave.
    yy, xx = np.mgrid[0:n, 0:n]
    weave = np.sin(xx * np.pi * 2 / 8) * np.sin(yy * np.pi * 2 / 8)
    fuzz = noise(n, 1.5, 51)
    slub = fbm(n, 40, 53, 2, 6)
    a = lerp((.040, .048, .060), (.070, .080, .095), np.clip(weave * .3 + slub * .4 + .5, 0, 1)) * (1 + fuzz[..., None] * .08)
    save('fabric', a, .95 + fuzz * .03, 0, weave * .5 + fuzz * .3, 2.5, .85 + weave * .1)
    # 6. Wool rug (one rug, 2.4 m x 1.7 m UV 0..1): border + heathered field.
    tuft = noise(n, 1.3, 61)
    heather = fbm(n, 18, 63, 3)
    u = np.linspace(0, 1, n)[None, :] * np.ones((n, 1))
    v = np.linspace(0, 1, n)[:, None] * np.ones((1, n))
    edge = np.minimum(np.minimum(u, 1 - u) * 2.4, np.minimum(v, 1 - v) * 1.7)
    border = ((edge > .07) & (edge < .11)) | ((edge > .14) & (edge < .15))
    a = lerp((.15, .145, .135), (.21, .20, .185), np.clip(heather * .5 + .5, 0, 1))
    a[border] = a[border] * .45 + np.array([.075, .085, .10]) * .55
    a[edge < .025] *= .55
    a *= (1 + tuft[..., None] * .10)
    save('rug', a, .97 + tuft * .02, 0, tuft * .6 + heather * .2 - border * .3, 3.0, .8 + tuft * .15)
    # 7. Matte black plastic with fine texture (covers .25 m).
    grain = noise(n, 1.4, 71)
    a = np.full((n, n, 3), .028) * (1 + grain[..., None] * .06)[..., :1]
    save('plastic_matte', a, .62 + grain * .05, 0, grain * .5, .8)
    # 8. Dark brushed metal (covers .5 m), brushing along x.
    brush = noise(n, 1.2, 81, aspect=60)
    a = np.full((n, n, 3), .16) * (1 + brush[..., None] * .10)
    save('metal_brushed', a, .36 + brush * .06, 1, brush * .4, .6)
    # 9. Teak deck (covers 2.4 m), 0.14 m boards with dark gaps.
    a, r, h, ao = planks(n, 91, 60, 1024, (.24, .14, .08), (.38, .24, .14), 30, gap=5, rough=(.55, .75))
    save('teak_deck', a, r, 0, h, 3.5, ao)
    # 10. Board-formed concrete (covers 2 m).
    big = fbm(n, 160, 101, 4)
    pits = np.clip(noise(n, 1.5, 103) - .75, 0, 1) * 4
    a = lerp((.20, .20, .205), (.30, .295, .29), np.clip(big * .5 + .5, 0, 1)) * (1 - pits[..., None] * .3)
    save('concrete', a, .82 + big * .06, 0, big * .3 - pits * .5, 2.2)


def env_sets(n=1024):
    """Extra environment sets: dark plaster/smoked oak (dark room), sand and
    water (seaside), glossy void floor."""
    big = fbm(n, 220, 141, 4)
    small = fbm(n, 6, 143, 3)
    a = lerp((.040, .040, .045), (.060, .058, .060), np.clip(big * .6 + .5, 0, 1)) * (1 + small[..., None] * .04)
    save('plaster_dark', a, .80 + small * .05, 0, big * .4 + small * .25, 2.0)
    a, r, h, ao = planks(n, 151, 81, 520, (.075, .052, .036), (.115, .080, .055), 70)
    save('oak_dark', a, r * .9, 0, h, 3.0, ao)
    ripples = fbm(n, 30, 161, 3, aspect=1.6)
    grain = noise(n, 1.2, 163)
    a = lerp((.42, .35, .25), (.55, .47, .35), np.clip(ripples * .5 + .5, 0, 1)) * (1 + grain[..., None] * .12)
    save('sand', a, .92 + grain * .04, 0, ripples * .6 + grain * .5, 3.0)
    waves = fbm(n, 60, 171, 4, aspect=2.5) + .35 * fbm(n, 12, 173, 3, aspect=3)
    a = np.ones((n, n, 3)) * np.array([.025, .075, .105])
    save('water', a, .14 + waves * .03, 0, waves, 7.0)
    speck = noise(n, 1.0, 181)
    a = np.ones((n, n, 3)) * .03 * (1 + speck[..., None] * .2)
    save('void_floor', a, .22 + speck * .04, 0, speck * .2, .6)


def gradient_sky(name, stops, sun=None, seed=5, star_density=0.):
    w, h = 2048, 1024
    v = np.linspace(0, 1, h)[:, None]
    elev = (0.5 - v) * np.pi
    col = np.zeros((h, 1, 3))
    for (e0, c0), (e1, c1) in zip(stops, stops[1:]):
        m = (elev <= e0) & (elev > e1)
        t = ((e0 - elev) / (e0 - e1))[..., None]
        col = np.where(m[..., None], np.array(c0) * (1 - t) + np.array(c1) * t, col)
    col = np.repeat(col, w, 1)
    u = np.linspace(0, 1, w)[None, :]
    if sun:
        su, se, radius, colour, halo = sun
        du = ((u - su + .5) % 1) - .5
        d = np.sqrt((du * 2 * np.pi * np.cos(se)) ** 2 + (elev - se) ** 2)
        col = col + np.array(colour) * (np.exp(-(d / halo) ** 2) * .6 + (d < radius) * 3.0)[..., None]
    if star_density:
        rng = np.random.default_rng(seed)
        stars = (rng.random((h, w)) > 1 - star_density) & (elev > .3)
        col[stars] += rng.uniform(.02, .10, stars.sum())[:, None]
    srgb = np.where(col <= .0031308, col * 12.92, 1.055 * np.clip(col, 0, 1) ** (1 / 2.4) - .055)
    Image.fromarray((np.clip(srgb, 0, 1) * 255 + .5).astype(np.uint8)).save(OUT / f'{name}.png', optimize=True)


def skyline():
    """Distant dusk city silhouette strip (emissive windows), 4096x512, tiles in u."""
    w, h = 4096, 512
    rng = np.random.default_rng(7)
    img = np.zeros((h, w, 3))
    # Sky gradient behind (alpha keeps the sky sphere visible above).
    alpha = np.zeros((h, w))
    x = 0
    while x < w:
        bw = int(rng.integers(40, 170))
        bh = int(rng.integers(60, 400) * (1 if rng.random() < .85 else 1.25))
        bh = min(bh, h - 10)
        top = h - bh
        tone = rng.uniform(.010, .022)
        img[top:, x:x + bw] = (tone, tone * 1.05, tone * 1.35)
        alpha[top:, x:x + bw] = 1
        lit = rng.uniform(.03, .12)
        for wy in range(top + 8, h - 4, 11):
            for wx in range(x + 5, min(x + bw - 5, w - 4), 9):
                if rng.random() < lit:
                    warm = rng.random() < .8
                    c = (1.0, .66, .32) if warm else (.55, .75, 1.0)
                    k = rng.uniform(.12, .35)
                    img[wy:wy + 5, wx:wx + 4] = np.array(c) * k
        if False:  # rooftop beacons removed (too busy)
            img[top - 3:top, x + bw // 2:x + bw // 2 + 3] = (1, .12, .08)
            alpha[top - 3:top, x + bw // 2:x + bw // 2 + 3] = 1
        x += bw + int(rng.integers(0, 12))
    rgba = np.concatenate([img, alpha[..., None]], -1)
    srgb = np.where(rgba[..., :3] <= .0031308, rgba[..., :3] * 12.92, 1.055 * rgba[..., :3] ** (1 / 2.4) - .055)
    out = np.concatenate([srgb, rgba[..., 3:]], -1)
    Image.fromarray((np.clip(out, 0, 1) * 255 + .5).astype(np.uint8), 'RGBA').save(OUT / 'skyline.png', optimize=True)


def dusk_sky():
    """Equirectangular-ish vertical gradient for the sky dome (u around, v up->down)."""
    w, h = 2048, 1024
    v = np.linspace(0, 1, h)[:, None]  # 0 zenith .. 1 nadir
    elev = (0.5 - v) * np.pi  # +pi/2 .. -pi/2
    stops = [(np.pi / 2, (.010, .014, .040)), (.6, (.030, .045, .110)), (.22, (.12, .10, .20)),
             (.06, (.55, .28, .20)), (0.0, (.80, .42, .22)), (-.2, (.05, .04, .06)), (-np.pi / 2, (.01, .01, .015))]
    col = np.zeros((h, 1, 3))
    for (e0, c0), (e1, c1) in zip(stops, stops[1:]):
        m = (elev <= e0) & (elev > e1)
        t = ((e0 - elev) / (e0 - e1))[..., None]
        col = np.where(m[..., None], np.array(c0) * (1 - t) + np.array(c1) * t, col)
    col = np.repeat(col, w, 1)
    u = np.linspace(0, 1, w)[None, :]
    # Warmer glow towards -Z (behind the screen, u=.75 in Blender sphere UVs).
    glow = np.exp(-((((u - .75 + .5) % 1) - .5) / .18) ** 2) * np.exp(-np.abs(elev) / .25)
    col = col * (1 + glow[..., None] * .8)
    rng = np.random.default_rng(3)

    srgb = np.where(col <= .0031308, col * 12.92, 1.055 * np.clip(col, 0, 1) ** (1 / 2.4) - .055)
    Image.fromarray((np.clip(srgb, 0, 1) * 255 + .5).astype(np.uint8)).save(OUT / 'dusk_sky.png', optimize=True)


def glow():
    """Light-bar halo card: blue, alpha falls off across (gaussian) and at the ends."""
    w, h = 64, 512
    u = np.linspace(-1, 1, w)[None, :]
    v = np.linspace(-1, 1, h)[:, None]
    a = np.exp(-(u / .38) ** 2) * np.clip((1 - np.abs(v)) / .12, 0, 1) ** 1.5
    core = np.exp(-(u / .08) ** 2)
    rgb = np.stack([.10 + .55 * core, .42 + .45 * core, np.ones_like(core)], -1) * np.ones((h, 1, 1))
    rgba = np.concatenate([rgb, (a * .85)[..., None]], -1)
    Image.fromarray((np.clip(rgba, 0, 1) * 255 + .5).astype(np.uint8)).save(OUT / 'ps4_glow.png', optimize=True)


def covers():
    """Original placeholder PS4-style case covers (fictional titles): the
    blue PS4 band on top, abstract art below."""
    from PIL import ImageDraw, ImageFont
    font = lambda n, s: ImageFont.truetype('C:/Windows/Fonts/' + n, s)
    specs = {'a': ('NIGHT TIDE', (8, 18, 40), (40, 110, 160), 'moon'),
             'b': ('IRON BLOOM', (40, 12, 10), (190, 80, 40), 'sun'),
             'c': ('ECHO RUN', (10, 30, 20), (60, 170, 110), 'peaks'),
             'd': ('SKYFORGE', (30, 20, 45), (170, 120, 220), 'rings')}
    for tag, (title, c0, c1, motif) in specs.items():
        w, h = 512, 656
        im = Image.new('RGB', (w, h))
        d = ImageDraw.Draw(im)
        for y in range(h):
            t = y / h
            d.line((0, y, w, y), fill=tuple(int(a + (b - a) * t) for a, b in zip(c0, c1)))
        cx, cy = w // 2, int(h * .45)
        if motif == 'moon':
            d.ellipse((cx - 90, cy - 150, cx + 90, cy + 30), fill=(230, 235, 240))
            d.ellipse((cx - 50, cy - 170, cx + 130, cy + 10), fill=c0)
        elif motif == 'sun':
            d.ellipse((cx - 120, cy - 120, cx + 120, cy + 120), fill=(250, 170, 60))
            d.polygon([(0, h * .75), (w * .4, h * .5), (w * .7, h * .68), (w, h * .55), (w, h), (0, h)], fill=(25, 8, 6))
        elif motif == 'peaks':
            d.polygon([(0, h * .8), (w * .3, h * .35), (w * .55, h * .7), (w * .8, h * .45), (w, h * .8), (w, h), (0, h)],
                      fill=(8, 20, 14))
        else:
            for r in (160, 120, 80):
                d.ellipse((cx - r, cy - r * .45, cx + r, cy + r * .45), outline=(240, 220, 255), width=6)
        d.text((36, h - 150), title, font=font('segoeuib.ttf', 54), fill=(245, 245, 245))
        band = 64
        d.rectangle((0, 0, w, band), fill=(0, 55, 145))
        d.text((22, 8), 'PS4', font=font('segoeuib.ttf', 40), fill=(255, 255, 255))
        im.save(OUT / f'cover_{tag}.png', optimize=True)


if __name__ == '__main__':
    covers()
    glow()
    build()
    env_sets()
    # Sunset over the sea: the sun sits low behind the screen (-z is u=.75).
    gradient_sky('sea_sky', [(np.pi / 2, (.05, .09, .22)), (.5, (.20, .28, .45)), (.15, (.75, .55, .45)),
                             (.03, (1.2, .62, .30)), (0., (1.3, .70, .35)), (-.05, (.10, .14, .20)),
                             (-np.pi / 2, (.02, .03, .05))], sun=(.75, .045, .012, (1.6, .9, .45), .09))
    gradient_sky('void_sky', [(np.pi / 2, (.0, .0, .0)), (.4, (.004, .005, .010)), (.0, (.020, .024, .040)),
                              (-.3, (.002, .002, .004)), (-np.pi / 2, (0, 0, 0))], star_density=.0004)
    skyline()
    dusk_sky()
    print('textures ->', OUT)
