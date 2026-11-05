#!/usr/bin/env python3
"""export_sprites.py - export every sprite frame of the game as a PNG (for redrawing, e.g. with ComfyUI).

usage: export_sprites.py [--sets 0-34] [--out build/assets/<GAME>/sprites] [--jobs 8] [--colour 0-3] [--orochi]
                         [--sheet] [--composites]

Writes OUT/NN_<name>/ per object set (players by character, then shared / effects / hud) and OUT/index.json
(every frame key -> set, frame, size, png). The key of a frame is the address of its frame header (6 hex
digits, the last part of each file name); a replacement image saved as mods/<GAME>/sprites/<key>.png is drawn
by the native renderer instead of the frame's tiles (any size: it is scaled to the frame's box).
The PNGs are derived from the ROM: keep them local (build/, mods/), never publish them.

Per set (the original single-set tool, still callable as export_set()):

OID = +0x70 anim id of the object (players: character id 0..33; 32/33/34 = shared
effect / HUD sets).  Writes
  OUTDIR/fNNN_HHHHHH.png  frame NNN (index into the frame table), header at 68k addr HHHHHH
                          (bank 1), unflipped, (W*16)x(H*16), transparent background
  OUTDIR/manifest.json    frames (header, format, size, palette) and the cel list
                          (x/y offsets, flips -> frame index)
  OUTDIR/sheet.png        contact sheet (with --sheet)
Palette: header byte 0x10..0x1f -> character colour set (D_00483e), 0x20..0x2f ->
character effect set (D_004956); anything else (or OID >= 32) is written with a grey
ramp and flagged "palette": null.
"""
import argparse
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import neocel  # noqa: E402
from neocel import Rom, save_png  # noqa: E402
game = neocel.game


def frame_count(R, oid):
    ft = R.frame_table(oid)
    n = 0
    while True:
        v = R.l(ft + 4 * n)
        if not (0x250000 <= v < ft):   # headers precede their table
            return n
        n += 1


def place(R, h, xoff, yoff, hf, vf):
    pal, fmt, W, H, tiles = R.decode_frame(h)
    left = -xoff - 16 * W if hf else xoff
    top = -yoff + 1 - 16 * H if vf else yoff + 1
    return left, top, W, H


def write_composites(R, oid, a, cels, frames):
    """Cel entries with bit 13 set continue with the next entry (drawn in the same
    sprite_draw_requests_sub_005d68 loop).  Compose each chain relative to the object origin
    (obj.sx, obj.sy-16) with facing 0; origin stored in manifest as cel['origin']."""
    from PIL import Image
    j = 0
    while j < len(cels):
        chain = [cels[j]]
        while R.w(chain[-1]['addr'] + 4) & 0x2000 and chain[-1]['index'] + 1 < len(cels):
            chain.append(cels[chain[-1]['index'] + 1])
        if len(chain) > 1 and all(c['valid'] for c in chain):
            parts = []
            for c in chain:
                fr = frames[c['frame']]
                if not fr['png']: continue
                l, t, W, H = place(R, fr['header'], c['xoff'], c['yoff'], c['hflip'], c['vflip'])
                im = Image.open(os.path.join(a.out, fr['png']))
                if c['hflip']: im = im.transpose(Image.FLIP_LEFT_RIGHT)
                if c['vflip']: im = im.transpose(Image.FLIP_TOP_BOTTOM)
                parts.append((l, t, im))
            if parts:
                x0 = min(p[0] for p in parts); y0 = min(p[1] for p in parts)
                x1 = max(p[0] + p[2].width for p in parts); y1 = max(p[1] + p[2].height for p in parts)
                out = Image.new('RGBA', (x1 - x0, y1 - y0))
                for l, t, im in parts:  # later entries get later sprite slots = drawn on top
                    out.alpha_composite(im, (l - x0, t - y0))
                out.save(os.path.join(a.out, 'c%04d.png' % chain[0]['index']))
                chain[0]['composite'] = dict(png='c%04d.png' % chain[0]['index'], origin=[-x0, -y0],
                                             parts=[c['index'] for c in chain])
        j += len(chain)


def export_set(job):
    oid, out, colour, orochi, sheet, composites = job
    a = argparse.Namespace(out=out, colour=colour, orochi=orochi, sheet=sheet, composites=composites)
    R = Rom()
    os.makedirs(a.out, exist_ok=True)
    ft = R.frame_table(oid)
    nf = frame_count(R, oid)
    base = R.player_palette_base(oid, a.colour, a.orochi) if oid < 32 else None
    frames = []
    pngs = []
    for i in range(nf):
        h = R.l(ft + 4 * i)
        pal, fmt, W, H, tiles = R.decode_frame(h)
        rec = dict(index=i, header=h, p2_offset=R.off(h, 1), format=fmt, w=W, h=H,
                   pal_byte=pal, tiles=len(tiles), palette=None, png=None)
        if W and H and tiles:
            palette = None
            if oid < 32:
                rec['palette'] = R.palette_for(oid, pal, a.colour, a.orochi)
                if rec['palette'] is not None:
                    palette = R.rom_palette(rec['palette'])
            img = R.render_frame(h, palette)
            name = 'f%03d_%06x.png' % (i, h & 0xffffff)
            save_png(img, os.path.join(a.out, name))
            rec['png'] = name
            pngs.append(name)
        frames.append(rec)
    cels = []
    cl = R.cel_list(oid)
    starts = sorted(set(R.cel_list(k) for k in range(35)))
    nxt = [s for s in starts if s > cl]
    end = nxt[0] if nxt else cl + 6 * 1200
    for j, ca in enumerate(range(cl, end, 6)):
        fw = R.w(ca + 4)
        idx = fw & 0x3ff
        cels.append(dict(index=j, addr=ca, xoff=R.sw(ca), yoff=R.sw(ca + 2), frame=idx,
                         hflip=bool(fw & 0x8000), vflip=bool(fw & 0x4000),
                         valid=idx < nf))
    if a.composites:
        write_composites(R, oid, a, cels, frames)
    man = dict(oid=oid, frame_table=ft, cel_list=cl, colour=a.colour, palette_base=base,
               screen_rule=('unflipped: left = obj.sx(+0x24) + xoff, top = obj.sy(+0x26) + yoff + 1 - 16; '
                            'hflip (facing^cel bit15): left = sx - xoff - 16*w; '
                            'vflip (cel bit14): top = sy - yoff + 1 - 16*h - 16'),
               frames=frames, cels=cels)
    json.dump(man, open(os.path.join(a.out, 'manifest.json'), 'w'), indent=1)
    if a.sheet and pngs:
        from PIL import Image
        ims = [Image.open(os.path.join(a.out, p)) for p in pngs]
        cw = max(i.width for i in ims); ch = max(i.height for i in ims)
        cols = 16
        rows = (len(ims) + cols - 1) // cols
        sh = Image.new('RGBA', (cols * cw, rows * ch), (40, 40, 60, 255))
        for k, im in enumerate(ims):
            sh.paste(im, ((k % cols) * cw, (k // cols) * ch), im)
        sh.save(os.path.join(a.out, 'sheet.png'))
    return oid, [dict(key='%06x' % (f['header'] & 0xffffff), set=oid, frame=f['index'], w=f['w'], h=f['h'],
                      png=os.path.join(os.path.basename(a.out), f['png'])) for f in frames if f['png']]


def set_name(oid):
    names = game.CFG['characters']['names']
    return game.CFG['sprites']['set_names'].get(str(oid)) or (names[oid] if oid < len(names) else 'set%d' % oid)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sets', default='0-%d' % (neocel.NSETS - 1), help='object sets, e.g. 0-34 or 0,9,33')
    ap.add_argument('--out', default=os.path.join(game.ROOT, 'build/assets', game.NAME, 'sprites'))
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--colour', type=int, default=0)
    ap.add_argument('--orochi', action='store_true')
    ap.add_argument('--sheet', action='store_true')
    ap.add_argument('--composites', action='store_true', help='also write cNNNN.png for chained (multi-frame) cels')
    a = ap.parse_args()
    sets = []
    for part in a.sets.split(','):
        lo, _, hi = part.partition('-')
        sets += list(range(int(lo), int(hi or lo) + 1))
    jobs = [(oid, os.path.join(a.out, '%02d_%s' % (oid, set_name(oid))), a.colour, a.orochi, a.sheet, a.composites)
            for oid in sets]
    index = {}
    with ProcessPoolExecutor(a.jobs) as ex:
        for oid, frames in ex.map(export_set, jobs):
            print('set %2d %-14s %5d images' % (oid, set_name(oid), len(frames)), flush=True)
            for f in frames:
                index.setdefault(f['key'], f)
    os.makedirs(a.out, exist_ok=True)
    json.dump(dict(game=game.NAME, replace_dir='mods/%s/sprites' % game.NAME, frames=index),
              open(os.path.join(a.out, 'index.json'), 'w'), indent=0)
    print('%d frames -> %s' % (len(index), a.out))


if __name__ == '__main__':
    sys.exit(main())
