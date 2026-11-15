#!/usr/bin/env python3
"""Pack build/plate/*.stl into out/*.3mf (stock mesh + add-ons per object, two filaments)."""
import shell as G
from make_3mf import pack
L = G.layout(); A = {k: L.pop(k) for k in list(L) if k.startswith("antenna")}
pack(L, "plate", "cyber-shell-plate.3mf", "cyber-shell", here=G.HERE, accents=G.accents())
pack(A, "plate", "antenna.3mf", "antenna", here=G.HERE, accents=G.accents())
