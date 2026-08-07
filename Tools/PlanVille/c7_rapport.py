# -*- coding: utf-8 -*-
"""PLAN DE VILLE / cloture — LES RENDUS C et D + L'INDEX DE CONTRAT.

`plan_ville/v1/plan.json` est LE contrat que le C++ lira a l'etage 2 : il porte
la liste des couches, leur DOUBLE empreinte (md5 des octets + md5 du contenu
logique LF, la garde du piege CRLF), le resultat des juges et des invariants,
les ecarts chiffres, et la liste d'arbitrage. Un plan incomplet ou a l'empreinte
fausse doit faire REFUSER le build — jamais deviner.
"""
import io
import json
import os
import pickle
import sys
import time

import numpy as np
import shapely
from shapely.ops import unary_union

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (CACHE, OUT, PLAN, chrono, ecrire_png, jalon, md5_logique,
                      md5_octets)
from c1_qui import valide

W = 1400
XA, YA, XB, YB = -2000.0, -1500.0, 2000.0, 2000.0
H = int(W * (YB - YA) / (XB - XA))


def grille():
    gx = XA + (np.arange(W) + 0.5) * (XB - XA) / W
    gy = YA + (np.arange(H) + 0.5) * (YB - YA) / H
    GX, GY = np.meshgrid(gx, gy)
    return GX.ravel(), GY.ravel()


def main():
    t0 = time.time()
    with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as f:
        P = pickle.load(f)["parcelles"]
    for p in P:
        p["geom"] = shapely.from_wkb(p["geom"])
    with open(os.path.join(CACHE, "niveaux.pkl"), "rb") as f:
        lois = pickle.load(f)["lois"]
    with open(os.path.join(CACHE, "interfaces.pkl"), "rb") as f:
        fronts = pickle.load(f)["fronts"]
    GX, GY = grille()

    # ---- _PLAN_C.png : la loi de Z ----------------------------------------
    t1 = time.time()
    img = np.full((H * W, 3), 18, dtype=np.uint8)
    COUL = {"constante": (224, 183, 74), "profil_troncon": (224, 112, 58),
            "drapage": (74, 144, 192)}
    for k, c in COUL.items():
        gs = [p["geom"] for p in P if (lois.get(p["id"]) or {}).get("loi") == k]
        if not gs:
            continue
        U = valide(unary_union(gs))
        shapely.prepare(U)
        img[shapely.contains_xy(U, GX, GY)] = c
    ecrire_png(os.path.join(PLAN, "_PLAN_C.png"), img.reshape(H, W, 3)[::-1])
    chrono("C7/rendu C", time.time() - t1, "_PLAN_C.png")

    # ---- _PLAN_D.png : les interfaces --------------------------------------
    t2 = time.time()
    im2 = np.full((H, W, 3), 16, dtype=np.uint8)
    CT = {"rien": (76, 175, 80), "talus": (139, 195, 74),
          "emmarchement": (255, 152, 0), "affleurement": (110, 110, 116),
          "bordure": (33, 150, 243), "mur": (244, 67, 54),
          None: (255, 0, 255)}
    byid = {p["id"]: p["geom"] for p in P}
    ordre = ["affleurement", "bordure", "mur", "talus", "rien", "emmarchement",
             None]
    par = {}
    for f in fronts:
        par.setdefault(f["type"], []).append(f)
    sx = W / (XB - XA)
    sy = H / (YB - YA)
    for t in ordre:
        for f in par.get(t, []):
            a, b = byid.get(f["a"]), byid.get(f["b"])
            if a is None or b is None:
                continue
            try:
                it = a.intersection(b)
            except Exception:
                continue
            if it.is_empty:
                continue
            lines = it.geoms if hasattr(it, "geoms") else [it]
            for L in lines:
                if L.geom_type != "LineString" or L.length <= 0:
                    continue
                n = max(2, int(L.length / 1.2))
                for q in range(n + 1):
                    p2 = L.interpolate(q / float(n), normalized=True)
                    i = int((p2.x - XA) * sx)
                    j = int((p2.y - YA) * sy)
                    if 0 <= i < W and 0 <= j < H:
                        im2[j, i] = CT[t]
    ecrire_png(os.path.join(PLAN, "_PLAN_D.png"), im2[::-1])
    chrono("C7/rendu D", time.time() - t2, "_PLAN_D.png")

    # ---- l'index de contrat ------------------------------------------------
    couches = {}
    for f in sorted(os.listdir(OUT)):
        if not f.endswith(".json") or f == "plan.json":
            continue
        p = os.path.join(OUT, f)
        s = io.open(p, encoding="utf-8").read()
        couches[f] = {"octets": os.path.getsize(p),
                      "md5_octets": md5_octets(p),
                      "md5_logique": md5_logique(s)}
    e1 = json.load(io.open(os.path.join(CACHE, "empreintes_passe1.json"),
                           encoding="utf-8"))
    e2 = json.load(io.open(os.path.join(CACHE, "empreintes_passe2.json"),
                           encoding="utf-8"))
    idem = all(e1.get(k, {}).get("md5_octets") == e2.get(k, {}).get("md5_octets")
               and e1.get(k, {}).get("md5_logique")
               == e2.get(k, {}).get("md5_logique")
               for k in set(list(e1) + list(e2)))
    L = {f: json.load(io.open(os.path.join(OUT, f), encoding="utf-8"))
         for f in couches}
    plan = {
        "version": "plan_ville/v1",
        "produit_par": "work/PLAN/c1..c7 (Python pur, zero Unreal)",
        "domaine": {"cellules": L["qui.json"]["domaine_cellules"],
                    "m2": L["qui.json"]["domaine_m2"],
                    "source": L["qui.json"]["domaine_source"]},
        "couches": couches,
        "empreintes_sources": L["qui.json"]["empreintes_sources"],
        "invariants": {
            "couverture_pc": L["qui.json"]["juges"]["couverture_pc"],
            "interstices_n": L["qui.json"]["juges"]["interstices_n"],
            "interstices_m2": L["qui.json"]["juges"]["interstices_m2"],
            "recouvrements_n": L["qui.json"]["juges"]["recouvrements_n"],
            "frontieres_sans_resolution_ni_arbitrage_n":
                L["interfaces.json"]["invariants"][
                    "sans_resolution_ni_arbitrage_n"],
            "dz_hors_bornes_n":
                L["interfaces.json"]["invariants"]["dz_hors_bornes_n"],
            "mineral_sur_frontiere_vegetal_vegetal_n":
                L["interfaces.json"]["invariants"][
                    "mineral_sur_vegetal_vegetal_n"],
            "semis_retenu_sur_dur_n":
                L["juges.json"]["semis"]["invariant_retenues_sur_dur_n"],
            "idempotence_2_compilations_bit_identiques": bool(idem)},
        "ecarts_chiffres": {
            "domaine_disque_hors_carte":
                L["qui.json"]["domaine_ecart_disque"],
            "emprise_des_masques":
                L["matiere.json"]["hors_masque"],
            "contre_preuve_z": L["niveaux.json"]["contre_preuve_z"],
            "conflits_donnee_visible":
                {k: {"parcelles": v["parcelles"], "m2": v["m2"]}
                 for k, v in
                 L["matiere.json"]["conflits_donnee_visible"].items()}},
        "arbitrage": L["interfaces.json"]["arbitrage"],
        "visualiseur": "work/PLAN/visualiseur/index.html (double-clic)",
        "garde_etage_2": "un plan incomplet ou a l'empreinte fausse = build "
                         "REFUSE, jamais devine (double empreinte octets + "
                         "contenu logique LF)"}
    s = json.dumps(plan, indent=1, sort_keys=True, ensure_ascii=False)
    with io.open(os.path.join(OUT, "plan.json"), "w", encoding="utf-8",
                 newline="\n") as f:
        f.write(s)
    jalon("C7/⭐ CONTRAT plan_ville/v1 ecrit : %d couches + plan.json ; "
          "idempotence %s ; invariants %s"
          % (len(couches), "PASS (bit-identique)" if idem else "ECHEC",
             json.dumps(plan["invariants"], sort_keys=True)))
    chrono("C7 TOTAL", time.time() - t0, "")


if __name__ == "__main__":
    main()
