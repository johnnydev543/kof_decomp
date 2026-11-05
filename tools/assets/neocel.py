"""neocel.py - decoder for the sprite frames ("cels") of SNK's Neo Geo fighting-game engine (KOF '97).

Table addresses come from games/<GAME>/game.json ("sprites"); the format was decoded from sprite_setup_frame
(0x5da0) and the tile writers dispatched by D_006030, and checked pixel by pixel against screenshots.

Data (all P2 addresses are 68000 addresses in the 0x200000 window):
  bank 1 (p2 file offset 0x100000):
    0x200002 + id*4 : long -> cel list of object type `id` (6-byte entries: x.w, y.w, frame.w)
    0x250000 + id*4 : long -> frame table of `id` (longs -> frame headers, index = frame & 0x3ff)
  bank 2 (p2 file offset 0x200000):
    0x200002 + id*4 : long -> animation script table (by variant)
    0x2cfff0 + n*32 : ROM palette n (word 0 unused, words 1..15 = colours 1..15)
Frame header: pal.b fmt.b w.b h.b, then format-specific data (see decode_frame).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import game  # noqa: E402  (games/<GAME>/game.json: sprite table addresses)

ROOT = os.path.join(game.ROOT, 'build/roms')
SP = game.CFG['sprites']
CEL_LISTS, FRAME_TABLES, ROM_PALETTES = (game.hexval(SP[k]) for k in ('cel_lists', 'frame_tables', 'rom_palettes'))
PLAYER_PALETTES, EFFECT_PALETTES, NSETS = game.hexval(SP['player_palettes']), game.hexval(SP['effect_palettes']), SP['sets']

class Rom:
    def __init__(self, root=ROOT):
        self.p2 = open(os.path.join(root, 'p2.be'), 'rb').read()
        self.c = open(os.path.join(root, 'c.bin'), 'rb').read()
        self.p1 = open(os.path.join(root, 'p1.be'), 'rb').read()
        self._tiles = {}

    def off(self, addr, bank):
        assert 0x200000 <= addr < 0x300000, hex(addr)
        return addr - 0x200000 + bank * 0x100000

    def b(self, a, bank=1): return self.p2[self.off(a, bank)]
    def w(self, a, bank=1): return struct.unpack_from('>H', self.p2, self.off(a, bank))[0]
    def sw(self, a, bank=1): return struct.unpack_from('>h', self.p2, self.off(a, bank))[0]
    def l(self, a, bank=1): return struct.unpack_from('>I', self.p2, self.off(a, bank))[0]

    # --- tables ---------------------------------------------------------
    def frame_table(self, oid): return self.l(FRAME_TABLES + oid * 4)
    def cel_list(self, oid): return self.l(CEL_LISTS + oid * 4)

    def cels(self, oid):
        """6-byte cel entries of object type oid: (index, addr, xoff, yoff, frameword)."""
        start = self.cel_list(oid)
        nxt = sorted(a for a in (self.cel_list(i) for i in range(NSETS)) if a > start)
        end = nxt[0] if nxt else start + 6 * 4096
        out = []
        for i, a in enumerate(range(start, end, 6)):
            out.append((i, a, self.sw(a), self.sw(a + 2), self.w(a + 4)))
        return out

    def frame_count(self, oid):
        ft = self.frame_table(oid)
        n = 0
        while True:
            v = self.l(ft + n * 4)
            if not (0x250000 <= v < 0x300000) or v == ft: break
            # stop at the start of another object's frame table
            n += 1
            if ft + n * 4 in self._ft_starts(): break
        return n

    def _ft_starts(self):
        if not hasattr(self, '_fts'):
            self._fts = set(self.frame_table(i) for i in range(NSETS))
        return self._fts

    def header(self, oid, idx): return self.l(self.frame_table(oid) + (idx & 0x3ff) * 4)

    # --- palettes -------------------------------------------------------
    def rom_palette(self, n):
        a = ROM_PALETTES + n * 32
        return [self.w(a + 2 * i, 2) for i in range(16)]

    def player_palette_base(self, cid, colour=0, orochi=False):
        """ROM palette number for palette slot +0x30+0x10 (player_build_palettes 0x47fa)."""
        d0 = cid
        if cid != 0 and orochi: d0 += 0xb
        idx = d0 * 8 + colour * 2
        return struct.unpack_from('>H', self.p1, PLAYER_PALETTES + idx)[0]

    def effect_palette_base(self, cid, orochi=False):
        """ROM palette number for slots +0x30+0x20..0x2f (D_004956, loaded by 0x4742)."""
        d0 = cid + (0xb if (cid != 0 and orochi) else 0)
        return struct.unpack_from('>H', self.p1, EFFECT_PALETTES + d0 * 2)[0]

    def palette_for(self, cid, pal_byte, colour=0, orochi=False):
        """ROM palette number a player-owned object (obj+0x30 = 0x00/0x20) shows for a
        frame whose header palette byte is pal_byte; None if not in the player's sets."""
        if 0x10 <= pal_byte < 0x20: return self.player_palette_base(cid, colour, orochi) + pal_byte - 0x10
        if 0x20 <= pal_byte < 0x30: return self.effect_palette_base(cid, orochi) + pal_byte - 0x20
        return None

    # --- tiles ----------------------------------------------------------
    def tile(self, code):
        t = self._tiles.get(code)
        if t is None:
            src = self.c[(code * 0x80) % len(self.c):][:0x80]
            t = []
            for y in range(16):
                row = []
                for half in (0x40, 0x00):
                    for x in range(8):
                        row.append((((src[half | 3 | (y << 2)] >> x) & 1) << 3) |
                                   (((src[half | 1 | (y << 2)] >> x) & 1) << 2) |
                                   (((src[half | 2 | (y << 2)] >> x) & 1) << 1) |
                                   (((src[half | 0 | (y << 2)] >> x) & 1)))
                t.append(row)
            self._tiles[code] = t
        return t

    # --- frame decode (sprite_setup_frame 0x5da0 + formats at D_006030) --
    def decode_frame(self, h):
        """Return (pal, fmt, w, h, tiles) where tiles = {(col,row): (code, attrlow)}.
        attrlow = extra SCB1 attribute low-byte bits OR'd per tile (bits 0-3)."""
        pal, fmt, W, H = self.b(h), self.b(h + 1), self.b(h + 2), self.b(h + 3)
        tiles = {}
        if W == 0 and H == 0:
            return pal, fmt, 0, 0, tiles
        a = h + 4
        rd_b = lambda x: self.b(x)
        rd_w = lambda x: self.w(x)

        def masks(start, wide):
            m = []
            for c in range(W):
                if wide: m.append(rd_w(start + 2 * c))
                else: m.append(rd_b(start + c) << 8)
            return m

        def rows_of(mask):
            return [r for r in range(H) if mask & (0x8000 >> r)]

        if fmt in (0, 4, 1):
            hi = rd_w(a) & 0x0f; lo = rd_w(a + 2)
            code = (hi << 16) | lo
            ms = masks(a + 4, fmt != 1)
            for c in range(W):
                for r in rows_of(ms[c]):
                    tiles[(c, r)] = (code, 0); code += 1
        elif fmt == 2:
            p = a; k = 0
            for c in range(W):
                for r in range(H):
                    if k & 1 == 0: t = rd_w(p); at = rd_b(p + 2); p += 3
                    else: at = rd_b(p); t = rd_w(p + 1); p += 3
                    tiles[(c, r)] = (((at >> 4) << 16) | t, at & 0x0f); k += 1
        elif fmt == 3:
            x = rd_w(a); p = a + 2
            for c in range(W):
                for r in range(H):
                    tiles[(c, r)] = ((((x >> 4) & 0xf) << 16) | rd_w(p), x & 0x0f); p += 2
        elif fmt in (5, 6):
            ms = masks(a, fmt == 5)
            p = a + (2 * W if fmt == 5 else (W + 1) & ~1)
            k = 0
            for c in range(W):
                for r in rows_of(ms[c]):
                    if k & 1 == 0: t = rd_w(p); at = rd_b(p + 2)
                    else: at = rd_b(p); t = rd_w(p + 1)
                    p += 3; k += 1
                    tiles[(c, r)] = (((at >> 4) << 16) | t, at & 0x0f)
        elif fmt in (7, 8):
            x = rd_w(a)
            ms = masks(a + 2, fmt == 7)
            p = a + 2 + (2 * W if fmt == 7 else (W + 1) & ~1)
            for c in range(W):
                for r in rows_of(ms[c]):
                    tiles[(c, r)] = ((((x >> 4) & 0xf) << 16) | rd_w(p), x & 0x0f); p += 2
        elif fmt in (9, 10):
            x = rd_w(a)
            ms = masks(a + 2, fmt == 9)
            p = a + 2 + (2 * W if fmt == 9 else W)
            for c in range(W):
                for r in rows_of(ms[c]):
                    tiles[(c, r)] = ((((x >> 4) & 0xf) << 16) | (x & 0xff00) | rd_b(p), x & 0x0f); p += 1
        else:
            raise ValueError('format %d' % fmt)
        return pal, fmt, W, H, tiles

    def render_frame(self, h, palette, hflip=False, vflip=False):
        """RGBA rows (list of lists of (r,g,b,a)) for frame header h with a 16-colour palette
        (list of Neo Geo colour words, or None for raw indices)."""
        pal, fmt, W, H, tiles = self.decode_frame(h)
        img = [[(0, 0, 0, 0)] * (W * 16) for _ in range(H * 16)]
        for (c, r), (code, at) in tiles.items():
            t = self.tile(code)
            fh = hflip ^ bool(at & 1); fv = vflip ^ bool(at & 2)
            cc = W - 1 - c if hflip else c
            rr = H - 1 - r if vflip else r
            for y in range(16):
                sy = 15 - y if fv else y
                row = t[sy]
                out = img[rr * 16 + y]
                for x in range(16):
                    v = row[15 - x if fh else x]
                    if v:
                        out[cc * 16 + x] = ng_rgb(palette[v]) + (255,) if palette else (v * 17, v * 17, v * 17, 255)
        return img


def ng_rgb(d):
    dark = d >> 15
    r = ((d >> 14) & 1) | ((d >> 7) & 0x1e)
    g = ((d >> 13) & 1) | ((d >> 3) & 0x1e)
    b = ((d >> 12) & 1) | ((d << 1) & 0x1e)
    r = (r << 1) | (not dark); g = (g << 1) | (not dark); b = (b << 1) | (not dark)
    return (r * 255 // 63, g * 255 // 63, b * 255 // 63)


def save_png(img, path):
    from PIL import Image
    H = len(img); W = len(img[0]) if H else 0
    im = Image.new('RGBA', (max(W, 1), max(H, 1)))
    if W and H:
        im.putdata([p for row in img for p in row])
    im.save(path)
