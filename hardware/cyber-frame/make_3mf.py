#!/usr/bin/env python3
"""Pack build/plate/*.stl into one Bambu Studio 3MF: six objects, each with a
body part on filament 1 and accent parts on filament 2.

Usage: python3 make_3mf.py   (after gen.py and `solidcraft-cli run build/export.json`)
"""
import os, struct, uuid, zipfile
from gen import plate_layout, coupon_layout, ACCENT_BODIES, HERE

def read_stl(path):
    d = open(path, 'rb').read(); n = struct.unpack('<I', d[80:84])[0]
    idx = {}; verts = []; tris = []
    for i in range(n):
        v = struct.unpack('<9f', d[84 + i*50 + 12: 84 + i*50 + 48]); t = []
        for j in range(3):
            p = tuple(round(c, 4) + 0.0 for c in v[3*j:3*j+3])
            if p not in idx: idx[p] = len(verts); verts.append(p)
            t.append(idx[p])
        if len(set(t)) == 3: tris.append(t)
    return verts, tris

NS = ('xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
      'xmlns:BambuStudio="http://schemas.bambulab.com/package/2021" '
      'xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p"')
HEAD = '<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" ' + NS + '>\n'
IDENT = "1 0 0 0 1 0 0 0 1 0 0 0"

def pack(layout, sub_dir, out_name, plate_name, here=HERE, accents=ACCENT_BODIES):
    # Same package layout Bambu Studio 2.x writes: one sub-model per piece holding its part meshes,
    # a root model whose objects reference them as components, and model_settings.config with the filaments.
    src = os.path.join(here, "build", sub_dir); out = os.path.join(here, "out", out_name)
    files = {}; root = []; build = []; rels = []; cfg = ['<?xml version="1.0" encoding="UTF-8"?>', '<config>']
    oid = 0
    for n, (piece, (bodies, _, _)) in enumerate(layout.items(), 1):
        sub = f"/3D/Objects/object_{n}.model"; meshes = []; parts = []
        for b in bodies:
            oid += 1; verts, tris = read_stl(os.path.join(src, b + ".stl"))
            meshes.append(f'  <object id="{oid}" p:UUID="{uuid.uuid4()}" type="model">\n   <mesh>\n    <vertices>\n'
                          + ''.join(f'     <vertex x="{x:.4f}" y="{y:.4f}" z="{z:.4f}"/>\n' for x, y, z in verts)
                          + '    </vertices>\n    <triangles>\n'
                          + ''.join(f'     <triangle v1="{a}" v2="{b_}" v3="{c}"/>\n' for a, b_, c in tris)
                          + '    </triangles>\n   </mesh>\n  </object>')
            parts.append((oid, b, len(tris)))
        files[sub.lstrip('/')] = (HEAD + ' <metadata name="BambuStudio:3mfVersion">1</metadata>\n <resources>\n'
                                  + '\n'.join(meshes) + '\n </resources>\n <build/>\n</model>\n')
        rels.append(f' <Relationship Target="{sub}" Id="rel-{n}" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>')
        oid += 1
        root.append(f'  <object id="{oid}" p:UUID="{uuid.uuid4()}" type="model">\n   <components>\n'
                    + ''.join(f'    <component p:path="{sub}" objectid="{p}" p:UUID="{uuid.uuid4()}" transform="{IDENT}"/>\n' for p, _, _ in parts)
                    + '   </components>\n  </object>')
        build.append(f'  <item objectid="{oid}" p:UUID="{uuid.uuid4()}" transform="{IDENT}" printable="1"/>')
        cfg.append(f'  <object id="{oid}">\n    <metadata key="name" value="{piece}"/>\n    <metadata key="extruder" value="1"/>')
        for p, b, nt in parts:
            ex = 2 if b in accents else 1
            cfg.append(f'    <part id="{p}" subtype="normal_part">\n      <metadata key="name" value="{b}"/>\n'
                       f'      <metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/>\n'
                       f'      <metadata key="extruder" value="{ex}"/>\n'
                       f'      <mesh_stat face_count="{nt}" edges_fixed="0" degenerate_facets="0" facets_removed="0" facets_reversed="0" backwards_edges="0"/>\n    </part>')
        cfg.append('  </object>')
    cfg += ['  <plate>', '    <metadata key="plater_id" value="1"/>', '    <metadata key="plater_name" value="' + plate_name + '"/>',
            '    <metadata key="locked" value="false"/>', '  </plate>', '</config>']
    files['3D/3dmodel.model'] = (HEAD + ' <metadata name="Application">BambuStudio-02.09.01.70</metadata>\n'
        ' <metadata name="BambuStudio:3mfVersion">1</metadata>\n <resources>\n' + '\n'.join(root)
        + f'\n </resources>\n <build p:UUID="{uuid.uuid4()}">\n' + '\n'.join(build) + '\n </build>\n</model>\n')
    files['3D/_rels/3dmodel.model.rels'] = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n'
        + '\n'.join(rels) + '\n</Relationships>')
    files['Metadata/model_settings.config'] = '\n'.join(cfg) + '\n'
    files['[Content_Types].xml'] = ('<?xml version="1.0" encoding="UTF-8"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">\n'
        ' <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>\n'
        ' <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>\n</Types>')
    files['_rels/.rels'] = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n'
        ' <Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>\n</Relationships>')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in ('[Content_Types].xml', '_rels/.rels'): z.writestr(name, files.pop(name))
        for name, data in files.items(): z.writestr(name, data)
    print("wrote", out, os.path.getsize(out), "bytes")

if __name__ == "__main__":
    pack(plate_layout(), "plate", "cyber-frame-plate.3mf", "cyber-frame")
    pack(coupon_layout(), "coupon", "fit-coupon.3mf", "fit-coupon")
