"""Wayfarer Map Builder: world map pictures for Wayfarer's Journal, drawn from the land of your own load order.

Reads an OpenMW profile (openmw.cfg: the data folders and the content list), takes the land of every outside cell
from the content files (the last file to change a cell wins, as in the game) and draws it from above in the map
looks of Wayfarer's Journal: the parchment atlas (sepia relief on paper) and the native window (brown land, teal
water). Writes the pictures and an index of them next to the program:

  textures/wayfarers_journal/gen/<look>/t_<x>_<y>.png   16 x 16 cells each, 64 pixels a cell
  textures/wayfarers_journal/gen/<look>/o_<x>_<y>.png   64 x 64 cells each, for the map zoomed out
  scripts/wayfarers_journal/data/genmap.lua             which pictures there are
and of each picture, and of each picture Wayfarer Map Renderer drew (local, interior and map), a compressed copy
(.dds, DXT1) beside it: every picture the journal's map has shown stays in video memory until the game is quit, and
compressed it takes an eighth of the memory.

Usage: wayfarer_map_builder [--config openmw.cfg] [--out folder] [--looks atlas,native]
"""

import argparse
import os
import re
import struct
import sys
import time

import numpy as np
from PIL import Image

from wayfarer_data_builder import default_config, find_files, log, read_config, records, subrecords

VERSION = '0.9'

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
# The game's own top-down pictures of the cells (its local map): Wayfarer Map Renderer's, else Advanced World Map's.
# They show what the heights can't: towns, buildings, roads, docks, bridges. Each look recolours them into its style.

LOCAL_DIRS = ('textures/advanced_world_map/local', 'textures/wayfarers_journal/local')  # later wins
LOCAL_NAME = re.compile(r'^\((-?\d+),(-?\d+)\)\.png$', re.IGNORECASE)


def find_local_maps(data_dirs):
    found = {}
    for sub in LOCAL_DIRS:
        for d in data_dirs:
            p = os.path.join(d, *sub.split('/'))
            try:
                names = os.listdir(p)
            except OSError:
                continue
            for n in names:
                m = LOCAL_NAME.match(n)
                if m:
                    found[(int(m.group(1)), int(m.group(2)))] = os.path.join(p, n)
    return found


class LocalMaps:
    """Each cell's picture at PX x PX: luminance 0..1 and whether each pixel is open water (rows north first)."""

    def __init__(self, paths):
        self.paths = paths
        self.cache = {}

    def get(self, cell):
        if cell in self.cache:
            return self.cache[cell]
        v = None
        path = self.paths.get(cell)
        if path:
            try:
                img = Image.open(path).convert('RGB').resize((PX, PX), Image.Resampling.BOX)
                a = np.asarray(img, dtype=np.float32)
                if a.mean() > 3:  # an empty (black) picture is no picture
                    r, g, b = a[..., 0], a[..., 1], a[..., 2]
                    lum = (0.30 * r + 0.55 * g + 0.15 * b)
                    # open water (only used where the land is under the sea, to tell the water from what stands in
                    # it): bluish, as on Advanced World Map's pictures, or, on the Map Renderer's, what is water is judged against the picture's own water: the typical dark grey of it, when
                    # there is enough of it, and anything close to that or only a little lighter with the water's
                    # green-blue tint. Absolute colours alone can't tell pale shallows from a blue-grey roof (Vivec's
                    # canton domes with some texture packs), but against the picture's own water they can.
                    sat = a.max(axis=-1) - a.min(axis=-1)
                    water = (b - r >= 4) & (g - r >= 2) & (lum < 90)
                    tint = (g - r >= 1) & (b >= r - 12)
                    grey = (lum < 70) & (sat < 16)
                    if grey.mean() > 0.15:
                        cut = np.percentile(lum[grey], 60)
                        ref = np.median(a[grey & (lum <= cut)], axis=0)
                        ref_lum = 0.30 * ref[0] + 0.55 * ref[1] + 0.15 * ref[2]
                        water |= np.sqrt(((a - ref) ** 2).sum(axis=-1)) < 14
                        water |= tint & (lum <= ref_lum + 16)
                    else:
                        water |= tint & (lum < 45)
                    v = (lum.astype(np.float16), water)
            except OSError:
                v = None
        self.cache[cell] = v
        return v

    def layer(self, x0, y0, w, h):
        """Luminance (NaN where no picture) and water, over pixels [x0, x0 + w) x [y0, y0 + h) (row 0 the north)."""
        lum = np.full((h, w), np.nan, dtype=np.float32)
        water = np.zeros((h, w), dtype=bool)
        top = y0 + h - 1
        for cx in range(x0 // PX, (x0 + w - 1) // PX + 1):
            for cy in range(y0 // PX, (y0 + h - 1) // PX + 1):
                v = self.get((cx, cy))
                if v is None:
                    continue
                px, py = cx * PX, cy * PX
                ax0, ax1 = max(px, x0), min(px + PX, x0 + w)
                ay0, ay1 = max(py, y0), min(py + PX, y0 + h)
                if ax0 >= ax1 or ay0 >= ay1:
                    continue
                r0, r1 = py + PX - ay1, py + PX - ay0   # picture rows (north first) for y in [ay0, ay1)
                rows = slice(top - (ay1 - 1), top - ay0 + 1)
                cols = slice(ax0 - x0, ax1 - x0)
                lum[rows, cols] = v[0][r0:r1, ax0 - px:ax1 - px]
                water[rows, cols] = v[1][r0:r1, ax0 - px:ax1 - px]
        return lum, water


def stretch(lum):
    """The pictures are dark (lit as the game's map is): spread their brightness over 0..1."""
    return np.clip((lum - 12.0) / 85.0, 0, 1) ** 0.85


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



def _box_count(m, r):
    """How many pixels of a square of radius r around each pixel are set."""
    a = m.astype(np.int32)
    for axis in (0, 1):
        p = np.pad(a, [((r + 1, r) if ax == axis else (0, 0)) for ax in (0, 1)], mode='constant')
        c = np.cumsum(p, axis=axis)
        a = (c[2 * r + 1:] - c[:-2 * r - 1]) if axis == 0 else (c[:, 2 * r + 1:] - c[:, :-2 * r - 1])
    return a


HOLE_MAX = PX * PX // 2   # water enclosed by a structure, up to half a cell, is part of it
HOLE_WIDE = 6             # pixels; the gaps in a canton are narrower, the canals between cantons wider


def _label(m):
    """Connected parts (4-way) of a mask, numbered 0..n-1; pixels outside the mask get n. Runs along the rows,
    joined where they touch the row below (no scipy in the build)."""
    hgt, wid = m.shape
    flat = m.ravel()
    start = flat.copy()
    start[1:] &= ~flat[:-1]
    start[::wid] = flat[::wid]                    # a run starts where the mask starts, or at a row's first pixel
    run = np.cumsum(start) - 1                    # each mask pixel's run
    nruns = int(start.sum())
    down = (m[:-1] & m[1:]).ravel()
    pairs = np.stack([run[:-wid][down], run[wid:][down]], axis=1)
    pairs = np.unique(pairs, axis=0) if len(pairs) else pairs
    parent = list(range(nruns))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for p, q in pairs.tolist():
        rp, rq = root(p), root(q)
        if rp != rq:
            parent[max(rp, rq)] = min(rp, rq)
    roots = np.array([root(i) for i in range(nruns)], dtype=np.int64)
    _, comp = np.unique(roots, return_inverse=True)
    n = int(comp.max()) + 1 if nruns else 0
    lab = np.full(m.size, n, dtype=np.int64)
    lab[flat] = comp[run[flat]]
    return lab.reshape(m.shape)


def solid_structures(built, has, land):
    """What stands in the water (docks, bridges, Vivec's cantons), made whole: the dark gaps between a canton's
    terraces and its roofs come out as dark and grey as the water around it, and drawn as sea they left the cantons
    as hollow rings. Gaps of a pixel are closed, then water fully enclosed by a structure (up to half a cell) is
    taken as part of it."""
    sea = has & ~land
    b = built & sea
    b = (_box_count(_box_count(b, 1) > 0, 1) >= 9) & sea   # closing, 3 x 3
    lab = _label(~b)                              # the water, and anything else not drawn as structure
    n = int(lab.max()) + 1
    edge = np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))
    size = np.bincount(lab.ravel(), minlength=n)
    dry = np.bincount(lab.ravel(), weights=(~sea).ravel(), minlength=n)
    fill = (size <= HOLE_MAX) & (dry == 0)       # enclosed, small, and all of it sea
    fill[edge] = False
    fill[-1] = False                              # the structure itself
    cand = fill[lab] & ~b
    # a canal between cantons is open water too, closed in by bridges: water wider than HOLE_WIDE pixels stays water
    r = HOLE_WIDE // 2
    fill[np.unique(lab[_box_count(cand, r) == (2 * r + 1) ** 2])] = False
    return b | (fill[lab] & ~b)


def draw_atlas(h, x0, y0, local=None):
    """The parchment atlas: sepia relief on paper, the coast inked, the sea darkened a little along it; where the
    game's own picture of a cell is known, that picture in sepia ink (towns, roads, docks), lit by the relief."""
    land = h > 0
    shade = hillshade(h, 2.2)
    rise = np.clip(h / 14000.0, 0, 1) ** 0.6
    col = lerp((196, 168, 118), (92, 62, 32), rise)
    col *= (0.42 + 0.78 * shade)[..., None]
    built = np.zeros_like(land)
    if local is not None:
        lum, water = local
        has = ~np.isnan(lum)
        L = stretch(np.nan_to_num(lum))
        ink = lerp((64, 42, 22), (232, 212, 168), L) * (0.78 + 0.32 * shade)[..., None]
        col = np.where(has[..., None], ink * 0.8 + col * 0.2, col)
        built = solid_structures(has & ~water & ~land, has, land)  # docks, bridges, cantons standing in the water
        # what stands in the water (docks, bridges, Vivec's cantons) in ink only up to a mid tan: their light parts
        # (plazas, pale stone) drawn as light as the paper read as more sea, and the cantons looked hollow
        ink_flat = lerp((58, 38, 20), (178, 150, 106), L)
    paper = np.empty(h.shape + (3,), dtype=np.float32)
    paper[:] = SEA['atlas']   # plain: the journal lays its own paper under the map, and pictures meet without a seam
    glow = box_blur(land.astype(np.float32), 5)
    paper *= (1 - 0.16 * glow)[..., None]
    out = np.where(land[..., None], col, paper)
    edge = coast(land)
    out[edge] = (84, 56, 30)
    if built.any():
        out[built] = ink_flat[built]
    return out


def draw_native(h, x0, y0, local=None):
    """The native window: brown land lit from the north-west, teal water; where the game's own picture of a cell
    is known, that picture in the window's browns (towns, roads, docks), lit by the relief."""
    land = h > 0
    shade = hillshade(h, 2.0)
    rise = np.clip(h / 12000.0, 0, 1)
    col = lerp((120, 86, 60), (82, 58, 42), rise)
    col *= (0.45 + 0.65 * shade)[..., None]
    built = np.zeros_like(land)
    if local is not None:
        lum, water = local
        has = ~np.isnan(lum)
        L = stretch(np.nan_to_num(lum))
        brown = lerp((46, 32, 22), (196, 156, 112), L)
        col = np.where(has[..., None], brown * (0.80 + 0.30 * shade)[..., None] * 0.85 + col * 0.15, col)
        built = solid_structures(has & ~water & ~land, has, land)
    # the water one teal (its depth only shows as cell-sized steps where files give no sea bed), lighter by the shore
    water = np.empty(h.shape + (3,), dtype=np.float32)
    water[:] = SEA['native']
    glow = box_blur(land.astype(np.float32), 4)
    water *= (1 + 0.22 * glow)[..., None]
    out = np.where(land[..., None], col, water)
    edge = coast(land)
    out[edge] = out[edge] * 0.72
    if built.any():
        out[built] = brown[built]
    return out


LOOKS = {'atlas': draw_atlas, 'native': draw_native}
SEA = {'atlas': (234, 219, 178), 'native': (58, 96, 92)}


def save_png(rgb, path, colours=128):
    full = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    img = full.quantize(colours, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    img.save(path, optimize=True)
    save_dds(full, os.path.splitext(path)[0] + '.dds')


def save_dds(img, path):
    """A picture as the journal's map loads it best: DXT1 (DXT5 where it is see-through), a power of two across and
    down (the game shrinks any other size when it loads it, slowly and blurrily). Written whole or not at all."""
    w, h = img.size
    pw, ph = pow2_below(w), pow2_below(h)
    alpha = img.mode in ('RGBA', 'LA', 'PA') or (img.mode == 'P' and 'transparency' in img.info)
    img = img.convert('RGBA' if alpha else 'RGB')
    if alpha and img.getchannel('A').getextrema()[0] == 255:
        img, alpha = img.convert('RGB'), False
    if (pw, ph) != (w, h):
        img = img.resize((pw, ph), Image.Resampling.LANCZOS)
    tmp = path + '.part'
    img.save(tmp, 'DDS', pixel_format='DXT5' if alpha else 'DXT1')
    # the header's linear size as the format has it (the picture's bytes), whatever the writer put there
    with open(tmp, 'r+b') as f:
        f.seek(20)
        f.write(struct.pack('<I', ((pw + 3) // 4) * ((ph + 3) // 4) * (16 if alpha else 8)))
    os.replace(tmp, path)


def pow2_below(n):
    p = 4
    while p * 2 <= n:
        p *= 2
    return p


GAME_PICTURES = ('textures/wayfarers_journal/local', 'textures/wayfarers_journal/interior',
                 'textures/wayfarers_journal/map')


def compress_pictures(data_dirs, out_dir):
    """A compressed copy (.dds) in the output folder of each picture Wayfarer Map Renderer drew, wherever it is in
    the data folders (the last folder wins, as in the game). Copies already newer than their picture are kept, and
    copies whose picture is gone are removed. Returns how many were made and how many there are."""
    src = {}
    for d in data_dirs:
        for sub in GAME_PICTURES:
            root = os.path.join(d, *sub.split('/'))
            if not os.path.isdir(root):
                continue
            for dirpath, _, names in os.walk(root):
                rel_dir = os.path.relpath(dirpath, d)
                for n in names:
                    if n.lower().endswith('.png'):
                        src[os.path.normcase(os.path.join(rel_dir, n[:-4]))] = (os.path.join(rel_dir, n[:-4]),
                                                                                os.path.join(dirpath, n))
    made = 0
    t = time.time()
    for i, (rel, path) in enumerate(src.values()):
        dds = os.path.join(out_dir, rel + '.dds')
        try:
            if os.path.getmtime(dds) >= os.path.getmtime(path):
                continue
        except OSError:
            pass
        try:
            os.makedirs(os.path.dirname(dds), exist_ok=True)
            with Image.open(path) as img:
                img.load()
                save_dds(img, dds)
            made += 1
        except (OSError, ValueError) as e:
            log(f'  could not compress {path}: {e}')
        if made and made % 2000 == 0:
            log(f'  {made} compressed ({i + 1} of {len(src)} looked at, {time.time() - t:.0f} s)')
    for sub in GAME_PICTURES:
        root = os.path.join(out_dir, *sub.split('/'))
        for dirpath, _, names in os.walk(root):
            for n in names:
                if n.lower().endswith('.dds'):
                    rel = os.path.normcase(os.path.join(os.path.relpath(dirpath, out_dir), n[:-4]))
                    if rel not in src:
                        try:
                            os.remove(os.path.join(dirpath, n))
                        except OSError:
                            pass
    return made, len(src)


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
        f.write(f'    px = {PX}, tileCells = {TILE}, overCells = {OVER}, dds = true,\n')
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
    ap.add_argument('--no-pictures', action='store_true', help='draw the land only, without the local map pictures')
    ap.add_argument('--no-compress', action='store_true',
                    help='don\'t make the compressed (.dds) copies of the Map Renderer\'s pictures')
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
    data_dirs, _ = read_config(cfg, remaps)
    # also the output folder itself (where the Map Renderer puts its pictures), even if that mod is switched off
    local_paths = {} if args.no_pictures else find_local_maps(list(data_dirs) + [args.out])
    local = LocalMaps(local_paths) if local_paths else None
    if local:
        log(f'Using the game\'s own pictures of {len(local_paths)} cells for towns and roads')
    else:
        log('No local map pictures found (Wayfarer Map Renderer\'s or Advanced World Map\'s): drawing the land only')
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
        layer = local.layer(x0, y0, size + 2 * MARGIN, size + 2 * MARGIN) if local else None
        for k in looks:
            rgb = LOOKS[k](h, x0, y0, layer)[MARGIN:-MARGIN, MARGIN:-MARGIN]
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
    if not args.no_compress and not args.no_pictures:
        log('Compressing the Map Renderer\'s pictures for the journal (only new or changed ones)...')
        t2 = time.time()
        made, total = compress_pictures(list(data_dirs) + [args.out], args.out)
        log(f'  {made} compressed, {total - made} already were ({time.time() - t2:.0f} s)')
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
