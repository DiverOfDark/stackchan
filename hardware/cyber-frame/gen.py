#!/usr/bin/env python3
"""Cyber-frame v0 for the M5Stack StackChan (K151): generates a SolidCraft command script.

Usage:  python3 gen.py            # writes build/cyber-frame.json (+ ref/*.stl on first run)
        solidcraft-cli run build/export.json && solidcraft-cli run build/coupon.json && python3 make_3mf.py

Frame: X right (robot's left is -X), Y back (head front face = 0), Z up (desk = 0).
All numbers in mm. Sources: M5Stack Model_Size.pdf and the official STLs
(github.com/m5stack/M5_Hardware, Products/K151_StackChan/Structures, MIT).
"""
import json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.environ.get("STACKCHAN_STL_DIR", "/tmp/sc-ref/stl")

# ---- measured / published stock dimensions --------------------------------
HEAD_W   = 54.0     # head width and height (drawing)
CORE_D   = 15.0     # CoreS3 depth in front of the MainBody rim (owner's caliper)
HEAD_D   = CORE_D + 44.7   # head depth without the 2 mm rear lip (STL); + lip = 61.7 vs 61.5 on the drawing
BASE_W, BASE_D = 48.0, 55.96   # base footprint (STL)
BASE_Y0  = 1.5      # base front edge behind head front (scaled off the drawing)
HEAD_Z0  = 16.5     # head underside above desk on the stock robot (70.5 - 54)
BASE_BLOCK_TOP = 9.6  # top of the base's outer block above desk (STL z levels)
HOLE_Y   = [CORE_D + y for y in (11.8, 19.8, 27.8)]   # three side holes, from head front (STL)
HOLE_Z   = 44.0     # above head underside (STL; drawing: 10 below top)
HOLE_D   = 4.86     # Technic pin hole: bore diameter (STL)
HOLE_CB  = (6.1, 1.0)   # counterbore at the outer face: diameter, depth (STL)
HOLE_L   = 6.9      # outer face to the inner end of the bore (STL)
SLOT_Y1  = CORE_D + 9.0   # rear edge of the light-guide slot region (STL)
CHAMFER  = ((CORE_D + 34.5, 3.0), (HEAD_D, 12.5))   # MainBody rear-bottom cut, (Y, Z above underside) (STL)

# ---- design parameters ------------------------------------------------------
FLOOR    = 2.0      # plinth floor under the stock base (lifts the robot by this much)
GAP      = 0.15     # panel-to-head air gap
FIT      = 0.3      # pocket clearance per side
PLINTH_WALL = 6.5   # pocket wall height above the floor
T        = 2.4      # panel thickness
PIN_D, PIN_LIP = 4.75, 5.1   # snap pin: shaft and lip diameter (lip picked with the fit coupon)
POD_D, POD_H = 26.0, 7.0
POD_Y, POD_Z = CORE_D + 27.2, 25.0
ZH = HEAD_Z0 + FLOOR            # head underside in this model
XI = HEAD_W / 2 + GAP           # panel inner face
XO = XI + T                     # panel outer face

GRAPH="#23252b"; MAG="#ff2bd6"; CYAN="#00e5ff"; YEL="#ffd400"; WHITE="#e9e9ec"; BLACK="#050608"
BODY, ACCENT = GRAPH, YEL   # the two filaments; every printed body is one or the other
INLAY = 0.8                 # depth of accent inlays (4 layers at 0.2 mm)

class S:
    def __init__(s): s.c=[]; s.col={}; s.n=0
    def cmd(s,c,**p): s.c.append({"command":c,"params":p})
    def paint(s,name,color,metal=0.0,rough=0.5): s.col.setdefault((color,metal,rough),[]).append(name)
    def box(s,name,lo,hi,color=None,**k):
        size=[hi[i]-lo[i] for i in range(3)]; cen=[(hi[i]+lo[i])/2 for i in range(3)]
        s.cmd("solid.box",length=size[0],width=size[1],height=size[2],center=cen,operation="new",body_name=name)
        if color: s.paint(name,color,**k)
    def cyl(s,name,d,h,base,axis,color=None,**k):
        s.cmd("solid.cylinder",diameter=d,height=h,base=base,axis=axis,operation="new",body_name=name)
        if color: s.paint(name,color,**k)
    def prism(s,name,plane,off,pts,thick,color=None,**k):
        pl={"front":{"origin":[0,off,0],"x_dir":[1,0,0],"y_dir":[0,0,1]},
            "side":{"origin":[off,0,0],"x_dir":[0,1,0],"y_dir":[0,0,1]},
            "top":{"origin":[0,0,off],"x_dir":[1,0,0],"y_dir":[0,1,0]}}[plane]
        s.n+=1
        s.cmd("sketch.create",plane=pl,name=f"sk{s.n}")
        s.cmd("sketch.line",points=[list(p) for p in pts],closed=True,infer=False)
        s.cmd("sketch.finish")
        s.cmd("solid.extrude",sketch=f"sk{s.n}",distance=thick,direction="symmetric",operation="new",body_name=name)
        if color: s.paint(name,color,**k)
    def comb(s,target,tools,op): s.cmd("solid.combine",target=target,tools=tools,operation=op)
    def finish(s):
        for (color,m,r),names in s.col.items():
            s.cmd("appearance.assign",bodies=names,color=color,pbr={"metallic":m,"roughness":r,"dielectric_ior":1.5})

def pin(s, name, at, axis, sign, lip, gap=GAP):
    """Split snap pin for a Technic hole, growing from `at` (a point on the part face) along sign*axis.
    Collar seats in the counterbore, lip springs out behind the bore. Returns the body name."""
    i = "XYZ".index(axis); e = HOLE_L + gap
    segs = [(-0.3, gap + HOLE_CB[1] - 0.1, HOLE_CB[0] - 0.2), (0.5, e + 0.1, PIN_D),
            (e + 0.05, e + 0.25, (PIN_D + lip) / 2), (e + 0.25, e + 0.65, lip),
            (e + 0.65, e + 0.95, lip - 0.35), (e + 0.95, e + 1.25, lip - 0.7)]
    for k,(a,b,d) in enumerate(segs):
        base = list(at); base[i] += sign * (a if sign > 0 else b)
        s.cyl(f"{name}_{k}", d, b - a, base, axis)
    s.comb(f"{name}_0", [f"{name}_{k}" for k in range(1, len(segs))], "join")
    j = (i + 1) % 3 if axis != "X" else 2          # slot normal: Z for side pins, Y for upright pins
    lo = [c - 3.5 for c in at]; hi = [c + 3.5 for c in at]
    lo[j], hi[j] = at[j] - 0.6, at[j] + 0.6
    lo[i], hi[i] = sorted((at[i] + sign * (gap + 0.4), at[i] + sign * (e + 1.6)))
    s.box(f"{name}_slot", lo, hi); s.comb(f"{name}_0", [f"{name}_slot"], "cut")
    return f"{name}_0"

def hz(pts): return [(y, z + ZH) for y, z in pts]   # head-local (Y, Z) -> model

# ---- parts ------------------------------------------------------------------
def plinth(s):
    top = FLOOR + PLINTH_WALL                     # kept well below the base block top (FLOOR + 9.6) for head-tilt clearance
    yc = BASE_Y0 + BASE_D / 2
    px, py = BASE_W / 2 + FIT, BASE_D / 2 + FIT   # pocket half sizes
    wx, wy = px + 2.7, py + 2.7                   # plateau half sizes
    ox, oy = wx + 13, wy + 12                     # outer half sizes
    s.prism("plinth","front",yc,[(-ox,0),(ox,0),(ox,4),(wx,top),(-wx,top),(-ox,4)],300,BODY)
    s.prism("plinth_s","side",0,[(yc-oy,0),(yc+oy,0),(yc+oy,4),(yc+wy,top),(yc-wy,top),(yc-oy,4)],300)
    s.comb("plinth",["plinth_s"],"intersect")
    for sg,n in ((-1,"l"),(1,"r")):                # foot rails
        s.box(f"rail_{n}",(sg*ox-2.5 if sg>0 else sg*ox-2.5, yc-oy-1.5, 0),(sg*ox+2.5, yc+oy+1.5, 4))
    s.comb("plinth",["rail_l","rail_r"],"join")
    s.box("pocket",(-px,yc-py,FLOOR),(px,yc+py,top+5)); s.comb("plinth",["pocket"],"cut")
    s.box("usb",(-7.5,yc+py-0.5,FLOOR),(7.5,yc+oy+5,top+5)); s.comb("plinth",["usb"],"cut")      # rear USB-C cable slot
    s.box("notch",(-4,yc-oy-5,5),(4,yc-py+0.5,top+5)); s.comb("plinth",["notch"],"cut")          # front notch of the base
    s.box("groove",(-20,yc-oy-1,0.8),(20,yc-oy+INLAY,3.2)); s.comb("plinth",["groove"],"cut")   # hazard inlay seat
    k=0
    for sx in (-16,16):                            # base hole pattern 32 x 48 (drawing)
        for dy in (-24,24):
            s.cyl(f"scr{k}",3.4,FLOOR+2,[sx,yc+dy,-1],"Z"); s.comb("plinth",[f"scr{k}"],"cut"); k+=1
    s.box("hazard",(-20,yc-oy,0.8),(20,yc-oy+INLAY,3.2),ACCENT)   # fills the groove exactly: printed in place
    return yc, oy

def panel(s, sg, n):
    x = sg * (XI + T / 2)
    (cy0, cz0), (cy1, cz1) = CHAMFER
    prof = [(10.2,54),(10.2,48.5),(SLOT_Y1-1.8,48.5),(SLOT_Y1+0.5,46),(SLOT_Y1+0.5,8),(SLOT_Y1+5.2,3),
            (cy0,cz0),(cy1,cz1),(HEAD_D,47),(HEAD_D-5.5,54)]
    s.prism(f"panel_{n}","side",x,hz(prof),T,BODY)
    xin = sg*XI
    pins = [pin(s,f"peg_{n}{i}",[xin,y,ZH+HOLE_Z],"X",-sg,PIN_LIP) for i,y in enumerate(HOLE_Y)]   # snap pins into the three side holes
    s.comb(f"panel_{n}",pins,"join")
    s.cyl(f"podhole_{n}",8.2,T+2,[sg*XI-1 if sg>0 else -XO-1,POD_Y,ZH+POD_Z],"X"); s.comb(f"panel_{n}",[f"podhole_{n}"],"cut")
    for j,z in enumerate((6.0,9.5)):               # cheek stripes: accent inlays in the outer face
        lo,hi = sorted((sg*(XO-INLAY), sg*(XO+1)))
        s.box(f"slot_{n}{j}",(lo,SLOT_Y1+7,ZH+z),(hi,SLOT_Y1+21,ZH+z+1.6)); s.comb(f"panel_{n}",[f"slot_{n}{j}"],"cut")
        lo,hi = sorted((sg*(XO-INLAY), sg*XO))
        s.box(f"cheek_{n}{j}",(lo,SLOT_Y1+7,ZH+z),(hi,SLOT_Y1+21,ZH+z+1.6),ACCENT)
    lo,hi = sorted((sg*(XI-0.5), sg*(XI+T/2)))     # lap seat for the brow tongue
    s.box(f"lap_{n}",(lo,10.0,ZH+48.3),(hi,16.2,ZH+54.5)); s.comb(f"panel_{n}",[f"lap_{n}"],"cut")
    if sg>0:                                       # antenna, coplanar with the plate so it prints flat
        s.box("antenna",(XI,HEAD_D-9.5,ZH+53.5),(XO,HEAD_D-6.5,ZH+76)); s.comb(f"panel_{n}",["antenna"],"join")
        s.box("tip",(XI,HEAD_D-9.5,ZH+76),(XO,HEAD_D-6.5,ZH+80),ACCENT)
    # pod + ring
    xo = sg*XO
    s.cyl(f"pod_{n}",POD_D,POD_H,[xo if sg>0 else xo-POD_H,POD_Y,ZH+POD_Z],"X",BODY)
    s.cyl(f"boss_{n}",8.0,T-0.2+0.3,[sg*XI+0.2 if sg>0 else xo-0.3,POD_Y,ZH+POD_Z],"X"); s.comb(f"pod_{n}",[f"boss_{n}"],"join")
    face = xo + sg*POD_H
    gb = face-1.2 if sg>0 else face
    s.cyl(f"g1_{n}",18.3,1.2,[gb,POD_Y,ZH+POD_Z],"X"); s.cyl(f"g2_{n}",11.7,1.2,[gb,POD_Y,ZH+POD_Z],"X")
    s.comb(f"g1_{n}",[f"g2_{n}"],"cut"); s.comb(f"pod_{n}",[f"g1_{n}"],"cut")
    s.cyl(f"ring_{n}",18.3,1.2,[gb,POD_Y,ZH+POD_Z],"X",ACCENT); s.cyl(f"r2_{n}",11.7,1.2,[gb,POD_Y,ZH+POD_Z],"X")
    s.comb(f"ring_{n}",[f"r2_{n}"],"cut")

def brow(s):
    prof = [(-2.7,51),(-2.7,54.5),(0.5,57),(10,57),(10,54.2),(-0.3,54.2),(-0.3,51)]
    s.prism("brow","side",0,hz(prof),2*XO,ACCENT)
    tools=[]
    for sg,n in ((-1,"l"),(1,"r")):
        lo,hi = sorted((sg*XI, sg*XO))
        s.box(f"tab_{n}",(lo,-0.3,ZH+50.5),(hi,10,ZH+54.3)); tools.append(f"tab_{n}")
        lo,hi = sorted((sg*XI, sg*(XI+T/2-0.1)))
        s.box(f"tongue_{n}",(lo,9.9,ZH+48.6),(hi,16.0,ZH+54.0)); tools.append(f"tongue_{n}")
    s.comb("brow",tools,"join")

# ---- stock robot stand-in (reference only, not printed) ----------------------
def shift_stl(src, dst, f):
    d = open(src,'rb').read(); n = struct.unpack('<I', d[80:84])[0]; out = bytearray(d[:84])
    for i in range(n):
        o = 84 + i*50; v = list(struct.unpack('<12f', d[o:o+48]))
        for j in range(3): v[3+3*j:6+3*j] = f(*v[3+3*j:6+3*j])
        out += struct.pack('<12f', *v) + d[o+48:o+50]
    open(dst,'wb').write(out)

def stock(s):
    ref = os.path.join(HERE, "ref"); os.makedirs(ref, exist_ok=True)
    dz = 34.2 + FLOOR                              # STL z -> model (base underside -32.6 -> 1.6 + floor)
    maps = {
      "Base":      lambda x,y,z: (x-106.73, y+352.55+BASE_Y0, z+dz),
      "MainBody":  lambda x,y,z: (x-475.73, y+352.01+CORE_D, z+dz),
      "ServoBody": lambda x,y,z: (x-229.73, y+320.56+BASE_Y0+31.75, z+dz),
    }
    for p,f in maps.items():
        dst = os.path.join(ref, f"{p}.stl")
        if not os.path.exists(dst): shift_stl(os.path.join(SRC, f"StackChan-{p}.stl"), dst, f)
        s.cmd("file.insert_mesh", path=dst, name=f"stock_{p}")
    s.box("cores3",(-27,0.1,ZH),(27,CORE_D-0.1,ZH+54),"#d8d8dc")
    s.box("screen",(-20.4,-0.2,ZH+15),(20.4,0.2,ZH+45.6),BLACK,rough=0.1)
    s.box("eye_l",(-13,-0.5,ZH+31),(-7,-0.1,ZH+37),CYAN); s.box("eye_r",(7,-0.5,ZH+31),(13,-0.1,ZH+37),CYAN)
    s.box("mouth",(-6,-0.5,ZH+21),(6,-0.1,ZH+22.6),CYAN)
    for sg,n in ((-1,"l"),(1,"r")):                # the stock light guides (lit by the body's LEDs)
        lo,hi = sorted((sg*24.5, sg*26.4))             # in the upper side corner, y 5..34.5 behind the rim (STL)
        s.box(f"lightguide_{n}",(lo,CORE_D+5,ZH+51.5),(hi,CORE_D+34.5,ZH+53.4),MAG)

# ---- print plate: one job, two filaments -------------------------------------
# piece -> (bodies [body filament first, accent after], rotation (axis, degrees) about the origin, translation)
def plate_layout():
    return {
      "plinth":  (["plinth","hazard"],                      None,       [0, 0, 0]),
      "panel_r": (["panel_r","tip","cheek_r0","cheek_r1"],  ("Y", 90),  [30, 0, XO]),
      "panel_l": (["panel_l","cheek_l0","cheek_l1"],        ("Y", -90), [-30, 0, XO]),
      "pod_r":   (["pod_r","ring_r"],                       ("Y", 90),  [21, -60, XO + POD_H]),
      "pod_l":   (["pod_l","ring_l"],                       ("Y", -90), [-21, -60, XO + POD_H]),
      "brow":    (["brow"],                                 ("X", 180), [0, -30, ZH + 57]),
    }
ACCENT_BODIES = {"hazard","tip","cheek_r0","cheek_r1","cheek_l0","cheek_l1","ring_r","ring_l","brow"}

def build(with_stock=True, export=None):
    s = S()
    if with_stock: stock(s)
    plinth(s)
    for sg,n in ((-1,"l"),(1,"r")): panel(s,sg,n)
    brow(s)
    s.finish()
    if export:
        os.makedirs(export, exist_ok=True)
        allb = [b for bodies,_,_ in plate_layout().values() for b in bodies]
        s.cmd("file.export_step", path=os.path.join(HERE, "out", "cyber-frame.step"), bodies=allb)   # as assembled
        for bodies,rot,tr in plate_layout().values():
            p = dict(bodies=bodies, translate=tr)
            if rot: p.update(axis={"X":[1,0,0],"Y":[0,1,0]}[rot[0]], angle=f"{rot[1]} deg", origin=[0,0,0])
            s.cmd("solid.move", **p)
        for b in allb:
            s.cmd("file.export_stl", path=os.path.join(export, f"{b}.stl"), bodies=[b], quality="fine")
    return s.c

# ---- fit coupon: 5-minute print to pick the snap-pin lip for this printer -------
# Three single-pin tabs, told apart by length (shortest = smallest lip), and one
# strip with three pins at the real 8 mm hole spacing.
COUPON_TABS = ((5.0, 14), (5.1, 18), (5.2, 22))   # (lip diameter, tab length)
def coupon_layout():
    lay = {f"tab_{int(d*10)}": ([f"tab_{int(d*10)}"], None, None) for d,_ in COUPON_TABS}
    lay["strip"] = (["strip"], None, None)
    return lay

def build_coupon(export):
    s = S(); x = 0.0
    for d,l in COUPON_TABS:
        n = f"tab_{int(d*10)}"
        s.box(n,(x,0,0),(x+10,l,T))
        s.comb(n,[pin(s,n+"_p",[x+5,5,T],"Z",1,d)],"join")
        x += 16
    span = HOLE_Y[-1]-HOLE_Y[0]
    s.box("strip",(x,0,0),(x+10,span+10,T))
    s.comb("strip",[pin(s,f"sp{i}",[x+5,5+y-HOLE_Y[0],T],"Z",1,PIN_LIP) for i,y in enumerate(HOLE_Y)],"join")
    os.makedirs(export, exist_ok=True)
    for b in coupon_layout():
        s.cmd("file.export_stl", path=os.path.join(export, f"{b}.stl"), bodies=[b], quality="fine")
    return s.c

if __name__ == "__main__":
    b = os.path.join(HERE, "build"); os.makedirs(b, exist_ok=True)
    json.dump(build(True), open(os.path.join(b, "cyber-frame.json"), "w"))
    json.dump(build(False, os.path.join(b, "plate")), open(os.path.join(b, "export.json"), "w"))
    json.dump(build_coupon(os.path.join(b, "coupon")), open(os.path.join(b, "coupon.json"), "w"))
    print("wrote", b)
