"""Wayfarer Map Builder: world map pictures for Wayfarer's Journal, drawn from the land of your own load order.

Reads an OpenMW profile (openmw.cfg: the data folders and the content list), takes the land of every outside cell
from the content files (the last file to change a cell wins, as in the game) and draws it from above in the map
looks of Wayfarer's Journal: the parchment atlas (sepia relief on paper) and the native window (brown land, teal
water). Writes the pictures and an index of them next to the program:

  textures/wayfarers_journal/gen/<look>/t_<x>_<y>.png   16 x 16 cells each, 64 pixels a cell
  textures/wayfarers_journal/gen/<look>/o_<x>_<y>.png   64 x 64 cells each, for the map zoomed out
  scripts/wayfarers_journal/data/genmap.lua             which pictures there are

Usage: wayfarer_map_builder [--config openmw.cfg] [--out folder] [--looks atlas,native]
"""

import argparse
import os
import struct
import sys
import time

import numpy as np
from PIL import Image

from wayfarer_data_builder import default_config, find_files, log, read_config, records, subrecords

VERSION = '0.1'

PX = 64                 # pixels a cell (a cell's land has 65 x 65 heights, 64 apart, the last shared with the next)
TILE = 16               # cells a picture
OVER = 64               # cells an overview picture
MARGIN = 16             # pixels worked out around a picture so shading and coasts match across its edges
SEA_FLOOR = -2500.0     # where no file gives land: open sea
UNITS = 8192 / 64       # game units between two heights


# --------------------------------------------------------------------------------------------------------------------
# The land

def read_land(path, lands):
    """Every LAND record of a file into lands[(x, y)]: 64 x 64 heights in game units (row 0 the south edge)."""
    n = 0
    for _, _, data in records(path, {b'LAND'}):
        grid, vhgt = None, None
        for name, value in subrecords(data):
            if name == b'INTV' and len(value) >= 8:
                grid = struct.unpack_from('<ii', value)
            elif name == b'VHGT' and len(value) >= 4 + 65 * 65:
                vhgt = value
        if grid is None:
            continue
        if vhgt is None:
            lands[grid] = None
            continue
        offset = struct.unpack_from('<f', vhgt)[0]
        d = np.frombuffer(vhgt, dtype=np.int8, count=65 * 65, offset=4).reshape(65, 65).astype(np.float32)
        rows = offset + np.cumsum(d[:, 0])
        d[:, 0] = rows
        h = np.cumsum(d, axis=1) * 8.0
        lands[grid] = h[:64, :64].astype(np.float32)
        n += 1
    return n


def heights(lands, by_tile, x0, y0, w, h):
    """Heights over pixels [x0, x0 + w) x [y0, y0 + h) of the world (y up), as an image (row 0 the north)."""
    out = np.full((h, w), SEA_FLOOR, dtype=np.float32)
    top = y0 + h - 1
    for cx in range(x0 // PX, (x0 + w - 1) // PX + 1):
        for cy in range(y0 // PX, (y0 + h - 1) // PX + 1):
            cell = lands.get((cx, cy))
            if cell is None:
                continue
            px, py = cx * PX, cy * PX
            ax0, ax1 = max(px, x0), min(px + PX, x0 + w)
            ay0, ay1 = max(py, y0), min(py + PX, y0 + h)
            if ax0 >= ax1 or ay0 >= ay1:
                continue
            block = cell[ay0 - py:ay1 - py, ax0 - px:ax1 - px][::-1]
            out[top - (ay1 - 1):top - ay0 + 1, ax0 - x0:ax1 - x0] = block
    return out


# --------------------------------------------------------------------------------------------------------------------
# Drawing

def box_blur(a, r):
    """A soft blur (three box blurs of radius r), edges clamped."""
    for _ in range(3):
        for axis in (0, 1):
            p = np.pad(a, [((r + 1, r) if ax == axis else (0, 0)) for ax in (0, 1)], mode='edge')
            c = np.cumsum(p, axis=axis, dtype=np.float64)
            if axis == 0:
                a = (c[2 * r + 1:] - c[:-2 * r - 1]) / (2 * r + 1)
            else:
                a = (c[:, 2 * r + 1:] - c[:, :-2 * r - 1]) / (2 * r + 1)
            a = a.astype(np.float32)
    return a


def hillshade(h, exaggerate):
    """Light from the north-west, high: 0 (in shadow) .. 1 (facing the light)."""
    gx = np.zeros_like(h)
    gy = np.zeros_like(h)
    gx[:, 1:-1] = (h[:, 2:] - h[:, :-2]) / (2 * UNITS)
    gy[1:-1, :] = (h[:-2, :] - h[2:, :]) / (2 * UNITS)   # row 0 is the north: up the image is +y
    nx, ny, nz = -gx * exaggerate, -gy * exaggerate, np.ones_like(h)
    norm = np.sqrt(nx * nx + ny * ny + 1)
    lx, ly, lz = -0.55, 0.55, 0.63
    return np.clip((nx * lx + ny * ly + nz * lz) / norm, 0, 1)


def lerp(a, b, t):
    a = np.asarray(a, dtype=np.float32)
    b = np.asarray(b, dtype=np.float32)
    return a + (b - a) * t[..., None]


def coast(land):
    """Land pixels with sea beside them."""
    e = np.zeros_like(land)
    e[1:-1, 1:-1] = land[1:-1, 1:-1] & ~(land[:-2, 1:-1] & land[2:, 1:-1] & land[1:-1, :-2] & land[1:-1, 2:])
    return e


def draw_atlas(h, x0, y0):
    """The parchment atlas: sepia relief on paper, the coast inked, the sea darkened a little along it."""
    land = h > 0
    shade = hillshade(h, 2.2)
    rise = np.clip(h / 14000.0, 0, 1) ** 0.6
    col = lerp((196, 168, 118), (92, 62, 32), rise)
    col *= (0.42 + 0.78 * shade)[..., None]
    paper = np.empty(h.shape + (3,), dtype=np.float32)
    paper[:] = SEA['atlas']   # plain: the journal lays its own paper under the map, and pictures meet without a seam
    glow = box_blur(land.astype(np.float32), 5)
    paper *= (1 - 0.16 * glow)[..., None]
    out = np.where(land[..., None], col, paper)
    edge = coast(land)
    out[edge] = (84, 56, 30)
    return out


def draw_native(h, x0, y0):
    """The native window: brown land lit from the north-west, teal water darker where deeper."""
    land = h > 0
    shade = hillshade(h, 2.0)
    rise = np.clip(h / 12000.0, 0, 1)
    col = lerp((120, 86, 60), (82, 58, 42), rise)
    col *= (0.45 + 0.65 * shade)[..., None]
    # the water one teal (its depth only shows as cell-sized steps where files give no sea bed), lighter by the shore
    water = np.empty(h.shape + (3,), dtype=np.float32)
    water[:] = SEA['native']
    glow = box_blur(land.astype(np.float32), 4)
    water *= (1 + 0.22 * glow)[..., None]
    out = np.where(land[..., None], col, water)
    edge = coast(land)
    out[edge] = out[edge] * 0.72
    return out


LOOKS = {'atlas': draw_atlas, 'native': draw_native}
SEA = {'atlas': (234, 219, 178), 'native': (58, 96, 92)}


def save_png(rgb, path, colours=128):
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), 'RGB')
    img = img.quantize(colours, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    img.save(path, optimize=True)


# --------------------------------------------------------------------------------------------------------------------
# The program

def content_order(cfg, remaps):
    data_dirs, content = read_config(cfg, remaps)
    plugins = [c for c in content if c.lower().endswith(('.esm', '.esp', '.omwaddon', '.omwgame'))]
    paths = find_files(data_dirs, plugins)
    missing = [c for c in plugins if c.lower() not in paths]
    if missing:
        # a folder Mod Organizer left out of openmw.cfg: look beside the data folders it did list
        parents = sorted({os.path.dirname(d.rstrip('/\\')) for d in data_dirs if d})
        siblings = []
        for par in parents:
            try:
                siblings.extend(os.path.join(par, e) for e in os.listdir(par))
            except OSError:
                pass
        extra = find_files([d for d in siblings if os.path.isdir(d)], missing)
        for c in missing:
            if extra.get(c.lower()):
                paths[c.lower()] = extra[c.lower()]
    order = [(c, paths[c.lower()]) for c in plugins if c.lower() in paths]
    missing = [c for c in plugins if c.lower() not in paths]
    return order, missing


def write_index(out_dir, looks, tiles, overs, bounds, nfiles):
    d = os.path.join(out_dir, 'scripts', 'wayfarers_journal', 'data')
    os.makedirs(d, exist_ok=True)

    def keys(ks):
        return ', '.join(f"['{x},{y}'] = true" for x, y in sorted(ks))

    with open(os.path.join(d, 'genmap.lua'), 'w', encoding='utf-8') as f:
        f.write('-- Made by Wayfarer Map Builder ' + VERSION + ': which world map pictures there are.\n')
        f.write('return {\n')
        f.write(f'    version = 1, time = {int(time.time())}, files = {nfiles},\n')
        f.write(f'    px = {PX}, tileCells = {TILE}, overCells = {OVER},\n')
        f.write('    looks = { ' + ', '.join(f'{k} = true' for k in looks) + ' },\n')
        f.write('    sea = { ' + ', '.join(f'{k} = {{ {SEA[k][0]}, {SEA[k][1]}, {SEA[k][2]} }}' for k in looks)
                + ' },\n')
        f.write(f'    bounds = {{ {bounds[0]}, {bounds[1]}, {bounds[2]}, {bounds[3]} }},\n')
        f.write('    tiles = { ' + keys(tiles) + ' },\n')
        f.write('    over = { ' + keys(overs) + ' },\n')
        f.write('}\n')


def main(argv=None):
    ap = argparse.ArgumentParser(description='World map pictures for Wayfarer\'s Journal.')
    ap.add_argument('--config', help='openmw.cfg of the profile (default: the one in Documents/My Games/OpenMW)')
    here = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.getcwd()
    ap.add_argument('--out', default=here, help='the folder to write into (default: the program\'s own folder)')
    ap.add_argument('--looks', default='atlas,native', help='which looks to draw (atlas, native)')
    ap.add_argument('--only', help='X,Y,X2,Y2: only the pictures over these cells (to try it out)')
    ap.add_argument('--remap', action='append', default=[], help='FROM=TO: read data folders under FROM at TO')
    ap.add_argument('--no-pause', action='store_true', help='don\'t wait for Enter at the end')
    args = ap.parse_args(argv)

    looks = [k for k in (s.strip() for s in args.looks.split(',')) if k in LOOKS]
    cfg = args.config or default_config()
    if not cfg or not os.path.isfile(cfg):
        log('Could not find openmw.cfg; give it with --config.')
        return 1
    remaps = [tuple(r.split('=', 1)) for r in args.remap if '=' in r]
    log(f'Wayfarer Map Builder {VERSION}')
    log(f'Profile: {cfg}')
    order, missing = content_order(cfg, remaps)
    if missing:
        log(f'{len(missing)} content files not found (skipped): ' + ', '.join(missing[:10])
            + (' ...' if len(missing) > 10 else ''))

    t0 = time.time()
    lands = {}
    for c, p in order:
        read_land(p, lands)
    lands = {k: v for k, v in lands.items() if v is not None}
    if not lands:
        log('No land found in the content files.')
        return 1
    log(f'Read the land of {len(lands)} cells from {len(order)} content files in {time.time() - t0:.0f} s')

    xs = [k[0] for k in lands]
    ys = [k[1] for k in lands]
    bounds = (min(xs), min(ys), max(xs), max(ys))
    want = {(cx // TILE, cy // TILE) for cx, cy in lands}
    if args.only:
        a = [int(v) for v in args.only.split(',')]
        want = {(tx, ty) for tx, ty in want
                if a[0] // TILE <= tx <= a[2] // TILE and a[1] // TILE <= ty <= a[3] // TILE}

    tex = os.path.join(args.out, 'textures', 'wayfarers_journal', 'gen')
    for k in looks:
        os.makedirs(os.path.join(tex, k), exist_ok=True)
    small = {k: {} for k in looks}           # tile -> a quarter-size copy, for the overview pictures
    t1 = time.time()
    size = TILE * PX
    for i, (tx, ty) in enumerate(sorted(want)):
        x0, y0 = tx * size - MARGIN, ty * size - MARGIN
        h = heights(lands, None, x0, y0, size + 2 * MARGIN, size + 2 * MARGIN)
        for k in looks:
            rgb = LOOKS[k](h, x0, y0)[MARGIN:-MARGIN, MARGIN:-MARGIN]
            save_png(rgb, os.path.join(tex, k, f't_{tx}_{ty}.png'))
            q = rgb.reshape(size // 4, 4, size // 4, 4, 3).mean(axis=(1, 3))
            small[k][(tx, ty)] = q
        if (i + 1) % 10 == 0 or i + 1 == len(want):
            log(f'  {i + 1} of {len(want)} pictures ({time.time() - t1:.0f} s)')

    per = OVER // TILE
    overs = {(tx // per, ty // per) for tx, ty in want}
    q = size // 4
    for k in looks:
        for ox, oy in overs:
            img = np.empty((per * q, per * q, 3), dtype=np.float32)
            img[:] = SEA[k]
            for (tx, ty), part in small[k].items():
                if tx // per == ox and ty // per == oy:
                    cx, cy = tx - ox * per, ty - oy * per
                    r = (per - 1 - cy) * q
                    img[r:r + q, cx * q:cx * q + q] = part
            save_png(img, os.path.join(tex, k, f'o_{ox}_{oy}.png'))
    write_index(args.out, looks, want, overs, bounds, len(order))
    log(f'Wrote {len(want)} pictures and {len(overs)} overview pictures for each look ({", ".join(looks)}) '
        f'to {os.path.abspath(args.out)} in {time.time() - t0:.0f} s')
    return 0


if __name__ == '__main__':
    code = 1
    try:
        code = main()
    except Exception:  # keep the window open to show what went wrong
        import traceback
        traceback.print_exc()
    if getattr(sys, 'frozen', False) and sys.stdin and sys.stdin.isatty() and '--no-pause' not in sys.argv:
        input('Press Enter to close.')
    sys.exit(code)
