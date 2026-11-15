#!/usr/bin/env python3
"""Cyber-shell v1 "armour": replacement head shell and base for the M5Stack StackChan (K151).

Unlike ../cyber-frame (clip-on, robot stays assembled), these parts REPLACE the stock
MainBody and Base. The inside is M5Stack's own mesh, untouched; the armour is separate
solids that overlap it, and the slicer fuses the parts of one object.

Stock features the design is built around (all measured on the MainBody STL, y behind
the front rim, z above the head underside):
  LED slit     upper side corner, y 5..34.5, on the corner fillet (x 24.6..26.3, z 51.6..53.3)
  Technic holes  3 per side, y 11.8/19.8/27.8, z 44 -> left bare; the optic clips into them
  side recess  y 1.5..7.6, z 6.8..47, 0.3 deep -> filled with an accent strip
  top          kept bare behind the visor so the head-pat touch sensor is not buried

Usage (in this folder):
    python3 shell.py
    solidcraft-cli run build/export.json
    python3 pack.py                          # -> out/cyber-shell-plate.3mf
"""
import json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cyber-frame"))
import gen as F
from gen import S, CORE_D, ZH, FLOOR, BASE_Y0, BODY, ACCENT, INLAY, HOLE_Y, HOLE_Z, XI, T, PIN_LIP, pin

REF = os.path.join(F.HERE, "ref")
ZT = ZH + 54.0                  # head top
ZB = 1.6 + FLOOR                # base underside in the shared model frame
BASE_BLOCK = 8.0                # height of the base's outer block (STL)
LED_Y = (4.0, 35.5)             # LED channel rails, a little longer than the 5..34.5 slit
def hy(pts): return [(CORE_D + y, ZH + z) for y, z in pts]   # MainBody-local -> model

# ---- head add-ons -------------------------------------------------------------
TONGUES = ((7.0, 15.0), (-15.0, -7.0))     # visor dovetail tongues (x ranges); the centre stays clear of the rim notch
DV, DH, DC = 0.8, 2.0, 0.12                # dovetail flare, height, clearance per side
RAIL_L = 12.0
PLATE_T = 2.5
ARMOUR = [(9.5, 6), (32, 6), (42.5, 15.5), (42.5, 33), (37, 38.5), (9.5, 38.5)]   # clear of recess, holes and rear chamfer
ACC = set()
def accent(n): ACC.add(n); return n

def head(s):
    names = []
    k = 0
    for a, b in TONGUES:                    # rails on the top front: the visor slides in from the front
        for pts in ([(b+DV+DC, ZT-0.4), (b+3.0, ZT-0.4), (b+3.0, ZT+DH), (b+DC, ZT+DH), (b+DV+DC, ZT)],
                    [(a-DV-DC, ZT-0.4), (a-3.0, ZT-0.4), (a-3.0, ZT+DH), (a-DC, ZT+DH), (a-DV-DC, ZT)]):
            s.prism(f"rail{k}", "front", CORE_D + RAIL_L/2, pts, RAIL_L, BODY); names.append(f"rail{k}"); k += 1
    xo = 27 + PLATE_T
    for sg, n in ((-1, "l"), (1, "r")):
        # armour plate, 45 degree ramp on its front (bed) edge
        s.prism(f"armour_{n}", "side", sg*(27 + PLATE_T/2 - 0.25), hy(ARMOUR), PLATE_T + 0.5, BODY)
        y0, y1 = CORE_D + ARMOUR[0][0], CORE_D + 43
        s.prism(f"ramp_{n}", "top", ZH+25, [(sg*26.4, y0-0.6), (sg*xo, y0+PLATE_T), (sg*xo, y1+1), (sg*26.4, y1+1)], 80)
        s.comb(f"armour_{n}", [f"ramp_{n}"], "intersect")
        for j, (y, z) in enumerate(((14, 9), (14, 35.5), (40, 17.5), (40, 31))):
            s.cyl(f"bolt_{n}{j}", 3, 1.1, [xo-0.3 if sg > 0 else -xo-0.8, CORE_D+y, ZH+z], "X")
        s.comb(f"armour_{n}", [f"bolt_{n}{j}" for j in range(4)], "join")
        for i in range(4):                   # slanted vents: accent inlays in the plate
            pts = hy([(17, 10+i*6), (31, 14.5+i*6), (31, 16.7+i*6), (17, 12.2+i*6)])
            s.prism(f"vcut_{n}{i}", "side", sg*(xo-0.2), pts, 1.2); s.comb(f"armour_{n}", [f"vcut_{n}{i}"], "cut")
            s.prism(accent(f"vent_{n}{i}"), "side", sg*(xo-INLAY/2), pts, INLAY, ACCENT)
        # LED channel: a rail above and a rail below the corner slit, so the light sits in a trough
        lo, hi = sorted((sg*20.2, sg*21.8)); s.box(f"ledtop_{n}", (lo, CORE_D+LED_Y[0], ZT-0.3), (hi, CORE_D+LED_Y[1], ZT+1.6), BODY)
        lo, hi = sorted((sg*26.7, sg*28.2)); s.box(f"ledside_{n}", (lo, CORE_D+LED_Y[0], ZH+48.4), (hi, CORE_D+LED_Y[1], ZH+50.2), BODY)
        # accent strip in the stock side recess next to the face
        lo, hi = sorted((sg*26.5, sg*27.5)); s.box(accent(f"strip_{n}"), (lo, CORE_D+2.1, ZH+7.2), (hi, CORE_D+7.3, ZH+46.6), ACCENT)
        names += [f"armour_{n}", f"ledtop_{n}", f"ledside_{n}"]
    # roof marking around the head-pat zone: thin flush outline, nothing thick over the touch sensor
    x0, x1, ya, yb, w = -13, 13, CORE_D+17, CORE_D+39, 1.2
    for i, (lo, hi) in enumerate((((x0, ya), (x1, ya+w)), ((x0, yb-w), (x1, yb)), ((x0, ya+w), (x0+w, yb-w)), ((x1-w, ya+w), (x1, yb-w)))):
        s.box(accent(f"pat{i}"), (lo[0], lo[1], ZT-0.6), (hi[0], hi[1], ZT+0.3), ACCENT)
    return names

def visor(s):
    """Wedge visor over the CoreS3 top. Stops at the screen's top edge (no drop in front of the camera)
    and its hood covers the rails; held by two dovetail tongues."""
    yr = CORE_D - 0.2
    s.prism("visor", "side", 0, [(-6, ZT+0.2), (-6, ZT+3.0), (-4.5, ZT+4.5), (yr, ZT+4.5), (yr, ZT+0.2)], 54, BODY)
    s.prism("hood", "side", 0, [(yr-0.3, ZT+DH+0.15), (yr-0.3, ZT+4.5), (CORE_D+10.2, ZT+4.5), (CORE_D+12.5, ZT+DH+0.2), (CORE_D+12.5, ZT+DH+0.15)], 40)
    names = ["hood"]
    for i, (a, b) in enumerate(TONGUES):
        s.prism(f"tongue{i}", "front", CORE_D+5.6, [(a-DV, ZT+0.05), (b+DV, ZT+0.05), (b, ZT+DH+0.3), (a, ZT+DH+0.3)], 11.8)
        names.append(f"tongue{i}")
    s.comb("visor", names, "join")
    s.box("edge_cut", (-25, -6.5, ZT+0.9), (25, -6+INLAY, ZT+2.4)); s.comb("visor", ["edge_cut"], "cut")
    s.box(accent("visor_edge"), (-25, -6, ZT+0.9), (25, -6+INLAY, ZT+2.4), ACCENT)

def optic(s):
    """Clip-on side optic: snaps into the three Technic holes (same pins as the cyber-frame strip),
    so it comes off and the holes stay usable. Fits either side: flip it over for the left."""
    ya, yb = HOLE_Y[0]-5, HOLE_Y[-1]+5; zc = ZH + HOLE_Z; xp = XI + T
    s.box("optic", (XI, ya, zc-4.4), (xp, yb, zc+4.3), BODY)
    s.prism("optic_h", "front", (ya+yb)/2 - 2, [(xp-0.2, zc-4.4), (xp-0.2, zc+4.3), (xp+6.5, zc+2.6), (xp+6.5, zc-2.6)], yb-ya+4)
    s.comb("optic", ["optic_h"] + [pin(s, f"op{i}", [XI, y, zc], "X", -1, PIN_LIP) for i, y in enumerate(HOLE_Y)], "join")
    yf = ya - 4
    s.box("lens_cut", (xp+1.2, yf-1, zc-2.4), (xp+5.3, yf+INLAY, zc+2.4)); s.comb("optic", ["lens_cut"], "cut")
    s.box(accent("lens"), (xp+1.2, yf, zc-2.4), (xp+5.3, yf+INLAY, zc+2.4), ACCENT)

def antenna(s, sg, n):
    """Clip-on blade antenna for the three Technic holes on the side the optic leaves free.
    A finned radio box over the holes and a swept-back blade rising 26 mm above the head top.
    The blade stands 4.5 mm outboard of the wall, so the LED slit below it stays open. One per side."""
    zc = ZH + HOLE_Z; xp = XI + T; xf = xp + 4.5; b = f"ant_{n}"
    lo, hi = sorted((sg*XI, sg*xp))
    s.box(b, (lo, HOLE_Y[0]-5, zc-4.3), (hi, HOLE_Y[-1]+5, zc+4.3), BODY)
    s.prism(b+"_h", "side", sg*(xp+xf-0.2)/2, hy([(9.8, 39.7), (32.8, 39.7), (32.8, 48.3), (6.8, 48.3), (6.8, 42.7)]), xf-xp+0.2)
    s.prism(b+"_b", "side", sg*(xf-1.2), hy([(19, 47), (32.8, 47), (42, 80), (38.5, 80)]), 2.4)
    s.comb(b, [b+"_h", b+"_b"] + [pin(s, f"ap{n}{i}", [sg*XI, y, zc], "X", -sg, PIN_LIP) for i, y in enumerate(HOLE_Y)], "join")
    for i, y in enumerate((9.5, 12.5, 15.5)):            # cooling grooves on the box
        lo, hi = sorted((sg*(xf-0.8), sg*(xf+1)))
        s.box(f"{b}_g{i}", (lo, CORE_D+y, zc-3), (hi, CORE_D+y+1.2, zc+3)); s.comb(b, [f"{b}_g{i}"], "cut")
    st = hy([(30.2, 50), (32.4, 50), (40.0, 77), (38.4, 77)])   # accent stripe, flush in the blade's outer face
    s.prism(b+"_sc", "side", sg*(xf-0.3), st, 1.0); s.comb(b, [b+"_sc"], "cut")
    s.prism(accent(b+"_stripe"), "side", sg*(xf-0.4), st, INLAY, ACCENT)

# ---- base skirt ------------------------------------------------------------------
def stl_verts(path):
    d = open(path, 'rb').read(); n = struct.unpack('<I', d[80:84])[0]; V = set()
    for i in range(n):
        o = 84 + i*50 + 12
        for k in range(3): V.add(struct.unpack('<3f', d[o+k*12:o+k*12+12]))
    return V

def hull(P):
    P = sorted(set(P))
    def cr(o, a, b): return (a[0]-o[0])*(b[1]-o[1]) - (a[1]-o[1])*(b[0]-o[0])
    out = []
    for seq in (P, P[::-1]):
        h = []
        for p in seq:
            while len(h) >= 2 and cr(h[-2], h[-1], p) <= 1e-6: h.pop()
            h.append(p)
        out += h[:-1]
    return out

def base(s):
    y0, y1 = BASE_Y0, BASE_Y0 + 55.97; yc = (y0+y1)/2; top = ZB + BASE_BLOCK; band = ZB + 3.8; fl = 6
    s.prism("skirt", "front", yc, [(-24-fl, ZB), (24+fl, ZB), (24+fl, band), (25.4, top), (-25.4, top), (-24-fl, band)], 300, BODY)
    s.prism("skirt_s", "side", 0, [(y0-fl, ZB), (y1+fl, ZB), (y1+fl, band), (y1+1.4, top), (y0-1.4, top), (y0-fl, band)], 300)
    s.prism("skirt_o", "top", ZB+4, [(-20.2, y0-fl), (20.2, y0-fl), (24+fl, y0+3.8), (24+fl, y1-11), (24, y1+fl), (-24, y1+fl), (-24-fl, y1-11), (-24-fl, y0+3.8)], 40)
    s.comb("skirt", ["skirt_s", "skirt_o"], "intersect")
    # hole for the stock base: its outline at the block top, shrunk 0.3 so skirt and base overlap and fuse
    out = hull([(round(x, 2), round(y, 2)) for x, y, z in stl_verts(os.path.join(REF, "Base.stl")) if ZB+7.5 <= z <= ZB+8.2])
    k = 1 - 0.3/24
    s.prism("skirt_in", "top", ZB+4, [(x*k, yc + (y-yc)*k) for x, y in out], 40); s.comb("skirt", ["skirt_in"], "cut")
    s.box("usb", (-7.5, y1-3, ZB-1), (7.5, y1+10, top+1)); s.comb("skirt", ["usb"], "cut")        # rear USB-C
    s.box("notch", (-4, y0-10, ZB+4.2), (4, y0+1, top+1)); s.comb("skirt", ["notch"], "cut")      # front cable notch
    names = []
    for i, x in enumerate(range(-17, 14, 6)):   # hazard chevrons: accent inlays in the front band
        pts = [(x, ZB+0.7), (x+3, ZB+0.7), (x+5, ZB+3.1), (x+2, ZB+3.1)]
        s.prism(f"hzcut{i}", "front", y0-fl, pts, 2*INLAY); s.comb("skirt", [f"hzcut{i}"], "cut")
        s.prism(accent(f"hz{i}"), "front", y0-fl+INLAY/2, pts, INLAY, ACCENT); names.append(f"hz{i}")
    return ["skirt"] + names

# ---- print plate ----------------------------------------------------------------------
# piece -> (stock mesh or None, CAD bodies [body colour first], rotation (axis, degrees) about the origin, translation)
def ant_plate():                            # outer face on the bed, pins up; print the one for your free side
    xf = XI + T + 4.5
    return {"antenna-left":  (None, ["ant_l", "ant_l_stripe"], ("Y", -90), [ZH+HOLE_Z+40, -70, xf]),
            "antenna-right": (None, ["ant_r", "ant_r_stripe"], ("Y", 90),  [-ZH-HOLE_Z+60, -70, xf])}

def plate(hb, bb):
    acc_head = sorted(n for n in ACC if n.startswith(("vent_", "strip_", "pat")))
    return {
        "head":  ("MainBody", hb + acc_head,         ("X", 90),  [-45, ZH+80, -CORE_D]),          # front rim on the bed
        "base":  ("Base",     bb,                    None,       [45, -BASE_Y0+10, -ZB]),
        "visor": (None,       ["visor", "visor_edge"], ("X", 180), [45, -15, ZT+4.5]),            # top face on the bed
        "optic": (None,       ["optic", "lens"],     ("Y", 90),  [-ZH-HOLE_Z+100, -70, XI+T+6.5]),  # outer face on the bed, pins up
    }

def place_stock(export, P):
    for m, _, rot, t in P.values():
        if not m: continue
        f = (lambda x, y, z, t=t: (x+t[0], -z+t[1], y+t[2])) if rot else (lambda x, y, z, t=t: (x+t[0], y+t[1], z+t[2]))
        F.shift_stl(os.path.join(REF, m+".stl"), os.path.join(export, f"stock_{m}.stl"), f)

def build(preview, export=None):
    s = S(); ACC.clear()
    if preview: F.stock(s)
    hb = head(s); visor(s); optic(s); antenna(s, -1, 'l'); antenna(s, 1, 'r'); bb = base(s); s.finish()
    P = plate(hb, bb); P.update(ant_plate())
    if export:
        os.makedirs(export, exist_ok=True); place_stock(export, P)
        allb = [b for _, bodies, _, _ in P.values() for b in bodies]
        s.cmd("file.export_step", path=os.path.join(HERE, "out", "cyber-shell-addons.step"), bodies=allb)
        for _, bodies, rot, t in P.values():
            p = dict(bodies=bodies, translate=t)
            if rot: p.update(axis={"X": [1, 0, 0], "Y": [0, 1, 0]}[rot[0]], angle=f"{rot[1]} deg", origin=[0, 0, 0])
            s.cmd("solid.move", **p)
        for b in allb:
            s.cmd("file.export_stl", path=os.path.join(export, f"{b}.stl"), bodies=[b], quality="fine")
    return s.c, P

def layout():
    _, P = build(False)
    return {p: ((["stock_"+m] if m else []) + b, None, None) for p, (m, b, _, _) in P.items()}
def accents():
    build(False); return set(ACC)

if __name__ == "__main__":
    b = os.path.join(HERE, "build"); os.makedirs(b, exist_ok=True); os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
    json.dump(build(True)[0], open(os.path.join(b, "cyber-shell.json"), "w"))
    json.dump(build(False, os.path.join(b, "plate"))[0], open(os.path.join(b, "export.json"), "w"))
    print("wrote", b)
