# -*- coding: utf-8 -*-
"""mq_blender.py — LE SECOND SUPPORT : la MEME maquette, assemblee en .blend.

Il ne re-derive rien : il relit les MEMES paquets de geometrie que la page
(maquette\\web\\cells\\mq_*.js) et les monte en objets Blender. Les deux supports
montrent donc exactement la meme chose — c'est la condition pour que la
comparaison de fluidite ait un sens.

    "C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe" ^
        --background --factory-startup --python mq_blender.py -- [--cellules a,b]

Sortie : maquette\\mq_maquette.blend + maquette\\mq_blender.json (les chronos).
"""
import base64
import json
import os
import sys
import time

import numpy as np

MQ = os.path.dirname(os.path.abspath(__file__))
CELLS = os.path.join(MQ, "web", "cells")

# teintes plates de maquette, une par famille (memes valeurs que la page)
PAL = {
    "chaussee": (0.72, 0.72, 0.74), "trottoir": (0.82, 0.81, 0.79),
    "carrefour": (0.66, 0.67, 0.70), "voie_ferree": (0.60, 0.58, 0.56),
    "canal": (0.55, 0.70, 0.80), "pont": (0.86, 0.72, 0.52),
    # famille NEUVE du contrat final (registre::ouvrage_affleurant)
    "dalot": (0.80, 0.62, 0.44),
    "escalier": (0.88, 0.80, 0.62), "gradins": (0.86, 0.78, 0.60),
    "mur_sout": (0.78, 0.70, 0.62), "ouvrage_hydro": (0.62, 0.74, 0.82),
    "edicule": (0.84, 0.74, 0.66), "tremie": (0.70, 0.66, 0.62),
    "sol_mineral": (0.87, 0.86, 0.84), "sol_vegetal": (0.72, 0.80, 0.66),
    "eau_surface": (0.48, 0.64, 0.78), "parking": (0.78, 0.78, 0.77),
    "terrain_sport": (0.74, 0.82, 0.70), "batiment": (0.93, 0.92, 0.90),
    "terrassement": (0.80, 0.74, 0.64), "terrain_naturel": (0.76, 0.80, 0.70),
    "semis": (0.55, 0.70, 0.50),
}
COUCHES = ["sol", "eau", "ouvr", "bati", "itf", "terr"]


def unb64(s, t):
    return np.frombuffer(base64.b64decode(s), dtype=t)


def lire_cellule(p):
    with open(p, "r", encoding="ascii") as f:
        s = f.read()
    return json.loads(s[s.index("(") + 1:s.rindex(")")])


def main():
    import bmesh
    import bpy

    t0 = time.time()
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    seules = None
    if "--cellules" in argv:
        seules = argv[argv.index("--cellules") + 1].split(",")

    idx = os.path.join(MQ, "web", "mq_index.js")
    with open(idx, "r", encoding="ascii") as f:
        s = f.read()
    INDEX = json.loads(s[s.index("=") + 1:].rstrip().rstrip(";\n"))
    FAM = INDEX["familles"]

    bpy.ops.wm.read_factory_settings(use_empty=True)
    mats = {}
    for f, c in PAL.items():
        m = bpy.data.materials.new("M_" + f)
        m.use_nodes = False
        m.diffuse_color = (c[0], c[1], c[2], 1.0)
        mats[f] = m

    cols = {}
    for nom in COUCHES + ["veg"]:
        c = bpy.data.collections.new(nom)
        bpy.context.scene.collection.children.link(c)
        cols[nom] = c

    # LE GABARIT D'ARBRE, une seule fois : volume ultra-simple (cone a 4 pans)
    gm = bpy.data.meshes.new("SM_arbre_maquette")
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=4,
                          radius1=1.8, radius2=0.0, depth=5.0)
    bm.to_mesh(gm)
    bm.free()
    gm.materials.append(mats["semis"])
    gab = bpy.data.objects.new("SM_arbre_maquette", gm)

    fichiers = sorted(os.listdir(CELLS))
    n_tri = n_som = n_obj = n_ins = 0
    t_lect = t_mesh = 0.0
    for fn in fichiers:
        if not fn.startswith("mq_") or not fn.endswith(".js"):
            continue
        cle = fn[3:-3]
        if seules and cle not in seules:
            continue
        ta = time.time()
        d = lire_cellule(os.path.join(CELLS, fn))
        t_lect += time.time() - ta
        for nom in COUCHES:
            L = d["L"].get(nom)
            if not L:
                continue
            for ci, k in enumerate(L["k"]):
                tb = time.time()
                q = k["q"]
                lo = np.array(q[:3])
                sp = np.array(q[3:])
                P = unb64(k["p"], np.uint16).reshape(-1, 3).astype(np.float64)
                P = lo + P / 65535.0 * sp
                I = unb64(k["i"], np.uint16).astype(np.int32).reshape(-1, 3)
                me = bpy.data.meshes.new("%s_%s_%d" % (nom, cle, ci))
                me.from_pydata([tuple(v) for v in P], [],
                               [tuple(t) for t in I])
                me.validate(verbose=False)
                # une teinte par famille : on prend la famille MAJORITAIRE du
                # troncon (la maquette est plate, pas un rendu)
                rf = unb64(k["rf"], np.uint8)
                fam = FAM[int(np.bincount(rf).argmax())] if len(rf) else "sol_mineral"
                ob = bpy.data.objects.new(me.name, me)
                ob.data.materials.append(mats.get(fam, mats["sol_mineral"]))
                cols[nom].objects.link(ob)
                n_tri += len(I)
                n_som += len(P)
                n_obj += 1
                t_mesh += time.time() - tb
        V = d["L"].get("veg")
        if V and V.get("n"):
            # INSTANCIATION NATIVE : un semis de sommets porte le gabarit
            # d'arbre en enfant (`instance_type='VERTS'`). Construire un cone
            # par arbre coute ~30 min sur le domaine ; ceci coute ~0 s.
            M = unb64(V["m"], np.float32).reshape(-1, 3).astype(np.float64)
            n_ins += len(M)
            me = bpy.data.meshes.new("semis_" + cle)
            me.vertices.add(len(M))
            me.vertices.foreach_set("co", M.ravel())
            me.update()
            ob = bpy.data.objects.new(me.name, me)
            ob.instance_type = 'VERTS'
            cols["veg"].objects.link(ob)
            gab.parent = None
            g2 = gab.copy()
            g2.data = gab.data
            g2.parent = ob
            cols["veg"].objects.link(g2)
            n_obj += 2

    t_asm = time.time() - t0
    out = os.path.join(MQ, "mq_maquette.blend")
    tb = time.time()
    bpy.ops.wm.save_as_mainfile(filepath=out)
    t_save = time.time() - tb
    o = os.path.getsize(out)

    res = {
        "produit_par": "work/PLAN/maquette/mq_blender.py",
        "source": "les MEMES paquets que la page (web/cells/mq_*.js)",
        "objets": n_obj, "triangles": int(n_tri), "sommets": int(n_som),
        "instances_vegetation": int(n_ins),
        "blend": out, "octets": o, "Mo": round(o / 1e6, 1),
        "secondes_lecture_json": round(t_lect, 1),
        "secondes_construction_maillages": round(t_mesh, 1),
        "secondes_assemblage_total": round(t_asm, 1),
        "secondes_sauvegarde": round(t_save, 1),
        "blender": bpy.app.version_string,
    }
    with open(os.path.join(MQ, "mq_blender.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False, indent=1, sort_keys=True)
    print("BLENDER " + json.dumps(res, ensure_ascii=False))


if __name__ == "__main__":
    main()
