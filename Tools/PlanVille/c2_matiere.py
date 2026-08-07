# -*- coding: utf-8 -*-
"""PLAN DE VILLE / etape B — LA COUCHE ③ MATIERE : mineral / vegetal / eau.

C'est LE champ dont l'absence a cause la regression du 07/08 (pierre cousue
entre deux herbes, arbres sur rubans). Il se decide par des REGLES NATIONALES,
sans aucun identifiant ni coordonnee, depuis deux autorites :

  * la DONNEE de propriete (qui possede la parcelle) ;
  * le VISIBLE : les masques de sol `Sols/mask_<x>_<y>.png`, canal R < 128 =
    vegetal, 1024 px pour 500 m = 0,48828 m/px. Tolerance raster->vecteur
    DECLAREE au contrat : 1 px.

REGLES (fermees, dans cet ordre) :
  M0  eau       : >= 50 % de l'aire de la parcelle dans une surface d'eau
                  (BD TOPO `toulouse10_surfaces.json` + side-cars `Eau/`).
  M1  mineral   : parcelle d'ouvrage, de voirie ou de batiment — le dur est du
                  dur ; la donnee d'autorite prime la peinture de rendu.
  M2  vegetal   : parcelle de zone (OCS GE) ou organique dont >= 50 % des
                  pixels de masque sont vegetal.
  M3  mineral   : parcelle de zone ou organique sinon.
  M4  (fallback) hors emprise des masques : la classe de la DONNEE seule
                  (zone -> vegetal, organique -> mineral), `masque_absent`.

Les CONFLITS donnee/visible ne sont jamais resolus en silence : ils sont
comptes, mesures en m2, et les pires partent dans la tournee du visualiseur.
"""
import io
import json
import math
import os
import pickle
import sys
import time

import numpy as np
import shapely
from shapely.geometry import Polygon, box
from shapely.ops import unary_union
from shapely.strtree import STRtree

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (CACHE, CELL_M, ENT, OUT, SRC, charge_json, chrono,
                      ecrire_json, jalon, lire_png_rgba, ecrire_png)
from c1_qui import valide, eclate

MASQUE_PX = 1024
PX_M = CELL_M / MASQUE_PX          # 0,48828 m/px
# ⚠️ PREMISSE DU BRIEF FALSIFIEE PAR LA MESURE. Le brief annonce « canal
# R < 128 = vegetal ». C'est L'INVERSE : R >= 128 = HERBE. Deux preuves
# independantes :
#   * mesure : sur la cellule 0_0, les parcelles OCS GE (`zone`) ont R moyen
#     232,1 et seulement 2,1 % de leurs pixels sous 128 ; les parcelles de
#     `voirie` ont R moyen 10,9 (et G moyen 114,1 : G = chaussee) ; sous les
#     batiments les 3 canaux valent ~1 (le sol n'y est pas peint) ;
#   * code producteur : `work/BLOC/m1_masque.py:119` -> `herbe = (sub[:,:,0]
#     >= 128)`.
# Avec la lettre du brief, 41 % du domaine devenait « conflit donnee/visible »
# (les emprises de batiment, non peintes, passaient toutes pour du vegetal).
SEUIL_R = 128                      # canal R >= 128 = vegetal (herbe)
SEUIL_VEG = 0.50                   # part de pixels vegetal qui fait la matiere
SEUIL_EAU = 0.50                   # part d'aire dans l'eau qui fait la matiere
TOL_RASTER_M = PX_M                # tolerance raster -> vecteur, DECLAREE


# ================================================================== L'EAU =====
def surfaces_eau():
    """Les surfaces d'eau : BD TOPO (`toulouse10_surfaces.json`) et les
    side-cars `Eau/` qui portent en plus les COTES (z_min/z_max NGF)."""
    gs, biefs = [], []
    S = charge_json(os.path.join(SRC, "toulouse10_surfaces.json"))
    for i, w in enumerate(S.get("water", [])):
        pts = w.get("pts") or []
        if len(pts) < 3:
            continue
        g = valide(Polygon([(float(p[0]), float(p[1])) for p in pts]))
        if not g.is_empty:
            gs.append(g)
    d = os.path.join(SRC, "Eau")
    for f in sorted(os.listdir(d)):
        if not f.endswith(".json"):
            continue
        j = charge_json(os.path.join(d, f))
        for e in (j.get("eau") or []):
            pts = e.get("pts") or []
            if len(pts) < 3:
                continue
            g = valide(Polygon([(float(p[0]), float(p[1])) for p in pts]))
            if g.is_empty:
                continue
            gs.append(g)
            biefs.append({"geom": g, "nature": e.get("nature"),
                          "z_min_ngf_m": e.get("z_min_ngf_m"),
                          "z_max_ngf_m": e.get("z_max_ngf_m"),
                          "aire_m2": e.get("aire_m2"),
                          "cleabs": e.get("cleabs")})
    EAU = valide(unary_union(gs)) if gs else Polygon()
    jalon("C2/EAU : %d surfaces (BD TOPO + side-cars), %.0f m2 ; %d biefs "
          "portent une cote (z_min/z_max NGF)" % (len(gs), EAU.area, len(biefs)))
    return EAU, biefs


# ============================================================== LES MASQUES ===
def charge_masques():
    """Le canal R de chaque masque de cellule, en uint8 (ligne 0 = y minimal
    de la cellule : convention `index.json`, origin = coin (x0, y0))."""
    idx = charge_json(os.path.join(SRC, "Sols", "index.json"))
    cache = os.path.join(CACHE, "masques_R.npz")
    t0 = time.time()
    if os.path.exists(cache):
        z = np.load(cache)
        M = {tuple(int(v) for v in k.split("_")): z[k] for k in z.files}
        chrono("C2/masques (cache)", time.time() - t0, "%d cellules" % len(M))
        return M
    M = {}
    for c in idx.get("cells", []):
        cx, cy = int(c["cell"][0]), int(c["cell"][1])
        p = os.path.join(SRC, "Sols", "mask_%d_%d.png" % (cx, cy))
        if not os.path.exists(p):
            continue
        M[(cx, cy)] = lire_png_rgba(p)[:, :, 0]
    np.savez_compressed(cache, **{"%d_%d" % k: v for k, v in M.items()})
    chrono("C2/masques", time.time() - t0, "%d cellules" % len(M))
    jalon("C2/MASQUES : %d cellules lues (%d px de cote, %.5f m/px, tolerance "
          "raster->vecteur DECLAREE = 1 px = %.3f m) ; le domaine du plan en "
          "compte %d — l'ecart d'emprise est chiffre, pas resolu"
          % (len(M), MASQUE_PX, PX_M, TOL_RASTER_M, 49))
    return M


def fraction_vegetal(g, M):
    """Part des pixels de masque VEGETAL (R < 128) sous la parcelle, mesuree
    sur la grille du masque (pas de vectorisation : la mesure est faite SUR LES
    PIXELS). Rend (fraction, n_pixels)."""
    xa, ya, xb, yb = g.bounds
    n_veg = n_tot = 0
    shapely.prepare(g)
    for cx in range(int(math.floor(xa / CELL_M)), int(math.floor(xb / CELL_M)) + 1):
        for cy in range(int(math.floor(ya / CELL_M)),
                        int(math.floor(yb / CELL_M)) + 1):
            R = M.get((cx, cy))
            if R is None:
                continue
            x0, y0 = cx * CELL_M, cy * CELL_M
            i0 = max(0, int((xa - x0) / PX_M))
            i1 = min(MASQUE_PX - 1, int((xb - x0) / PX_M))
            j0 = max(0, int((ya - y0) / PX_M))
            j1 = min(MASQUE_PX - 1, int((yb - y0) / PX_M))
            if i1 < i0 or j1 < j0:
                continue
            ii = np.arange(i0, i1 + 1)
            jj = np.arange(j0, j1 + 1)
            X = x0 + (ii + 0.5) * PX_M
            Y = y0 + (jj + 0.5) * PX_M
            GX, GY = np.meshgrid(X, Y)
            dedans = shapely.contains_xy(g, GX.ravel(), GY.ravel())
            if not dedans.any():
                continue
            sub = R[np.ix_(jj, ii)].ravel()[dedans]
            n_tot += int(sub.size)
            n_veg += int((sub >= SEUIL_R).sum())
    if n_tot == 0:
        return None, 0
    return n_veg / float(n_tot), n_tot


# ================================================================= LA REGLE ===
def matiere(p, EAU, M):
    g = p["geom"]
    prop = p["proprietaire"]
    out = {"regle": None, "masque_absent": False, "px": 0, "veg_pc": None}
    # M0 — l'eau prime tout
    try:
        a_eau = EAU.intersection(g).area if EAU.intersects(g) else 0.0
    except Exception:
        a_eau = 0.0
    out["eau_pc"] = round(100.0 * a_eau / max(g.area, 1e-9), 3)
    if a_eau >= SEUIL_EAU * g.area:
        out["regle"] = "M0"
        return "eau", out
    f, n = fraction_vegetal(g, M)
    out["px"] = n
    out["veg_pc"] = None if f is None else round(100.0 * f, 3)
    if prop in ("ouvrage", "voirie", "batiment"):
        out["regle"] = "M1"
        return "mineral", out
    if f is None:
        out["masque_absent"] = True
        out["regle"] = "M4"
        return ("vegetal" if prop == "zone" else "mineral"), out
    if f >= SEUIL_VEG:
        out["regle"] = "M2"
        return "vegetal", out
    out["regle"] = "M3"
    return "mineral", out


def main():
    t0 = time.time()
    with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as f:
        D = pickle.load(f)
    P = D["parcelles"]
    for p in P:
        p["geom"] = shapely.from_wkb(p["geom"])
    EAU, biefs = surfaces_eau()
    shapely.prepare(EAU)
    M = charge_masques()

    t1 = time.time()
    cpt = {"mineral": 0, "vegetal": 0, "eau": 0}
    aire = {"mineral": 0.0, "vegetal": 0.0, "eau": 0.0}
    regles = {}
    conflits = {"dur_peint_vegetal": [], "zone_verte_minerale": [],
                "organique_vegetal": 0}
    a_conf = {"dur_peint_vegetal": 0.0, "zone_verte_minerale": 0.0}
    n_sans_masque = 0
    a_sans_masque = 0.0
    for i, p in enumerate(P):
        m, info = matiere(p, EAU, M)
        p["matiere"] = m
        p["matiere_info"] = info
        cpt[m] += 1
        aire[m] += p["geom"].area
        regles[info["regle"]] = regles.get(info["regle"], 0) + 1
        if info["masque_absent"]:
            n_sans_masque += 1
            a_sans_masque += p["geom"].area
        # CONFLITS donnee / visible — comptes, jamais resolus en silence
        if info["veg_pc"] is not None:
            if p["proprietaire"] in ("voirie", "batiment", "ouvrage") \
                    and info["veg_pc"] >= 100.0 * SEUIL_VEG:
                conflits["dur_peint_vegetal"].append(
                    (p["geom"].area, p["id"], info["veg_pc"]))
                a_conf["dur_peint_vegetal"] += p["geom"].area
            if p["proprietaire"] == "zone" and info["veg_pc"] < 100.0 * SEUIL_VEG:
                conflits["zone_verte_minerale"].append(
                    (p["geom"].area, p["id"], info["veg_pc"]))
                a_conf["zone_verte_minerale"] += p["geom"].area
            if p["proprietaire"] == "organique" and m == "vegetal":
                conflits["organique_vegetal"] += 1
        if (i + 1) % 10000 == 0:
            jalon("C2/  matiere : %d / %d parcelles (%.0f s)"
                  % (i + 1, len(P), time.time() - t1))
    chrono("C2/matiere", time.time() - t1, "%d parcelles" % len(P))
    tot = sum(aire.values())
    jalon("C2/⭐ MATIERE : mineral %d parcelles / %.0f m2 (%.2f %%) | vegetal "
          "%d / %.0f m2 (%.2f %%) | eau %d / %.0f m2 (%.2f %%) ; regles "
          "appliquees %s ; %d parcelles hors emprise des masques (%.0f m2, "
          "regle M4)"
          % (cpt["mineral"], aire["mineral"], 100.0 * aire["mineral"] / tot,
             cpt["vegetal"], aire["vegetal"], 100.0 * aire["vegetal"] / tot,
             cpt["eau"], aire["eau"], 100.0 * aire["eau"] / tot,
             json.dumps(regles, sort_keys=True), n_sans_masque, a_sans_masque))
    for k in ("dur_peint_vegetal", "zone_verte_minerale"):
        conflits[k].sort(key=lambda t: -t[0])
        jalon("C2/⚠️ CONFLIT DONNEE/VISIBLE `%s` : %d parcelles, %.0f m2 "
              "(%.3f %% du domaine) ; pire cas %s"
              % (k, len(conflits[k]), a_conf[k],
                 100.0 * a_conf[k] / 12250000.0,
                 ", ".join("%s %.0f m2 (%.1f %% vegetal)" % (c[1], c[0], c[2])
                           for c in conflits[k][:3])))

    # ---- rendu PNG de l'etape ---------------------------------------------
    t2 = time.time()
    W = 1400
    xa, ya, xb, yb = -2000.0, -1500.0, 2000.0, 2000.0
    H = int(W * (yb - ya) / (xb - xa))
    gx = xa + (np.arange(W) + 0.5) * (xb - xa) / W
    gy = ya + (np.arange(H) + 0.5) * (yb - ya) / H
    GX, GY = np.meshgrid(gx, gy)
    GX, GY = GX.ravel(), GY.ravel()
    img = np.full((H * W, 3), 18, dtype=np.uint8)
    COUL = {"mineral": (150, 146, 140), "vegetal": (86, 152, 76),
            "eau": (58, 110, 178)}
    for m in ("mineral", "vegetal", "eau"):
        gs = [p["geom"] for p in P if p["matiere"] == m]
        if not gs:
            continue
        U = valide(unary_union(gs))
        shapely.prepare(U)
        img[shapely.contains_xy(U, GX, GY)] = COUL[m]
    ecrire_png(os.path.join(r"C:\LidarPoC\work\PLAN", "_PLAN_B.png"),
               img.reshape(H, W, 3)[::-1])
    chrono("C2/rendu", time.time() - t2, "_PLAN_B.png %dx%d" % (W, H))

    with open(os.path.join(CACHE, "matiere.pkl"), "wb") as f:
        pickle.dump({"matiere": {p["id"]: (p["matiere"], p["matiere_info"])
                                 for p in P},
                     "eau_wkb": shapely.to_wkb(EAU),
                     "biefs": [{"geom": shapely.to_wkb(b["geom"]),
                                "nature": b["nature"],
                                "z_min_ngf_m": b["z_min_ngf_m"],
                                "z_max_ngf_m": b["z_max_ngf_m"],
                                "cleabs": b["cleabs"]} for b in biefs]},
                    f, protocol=4)
    rep = {"couche": "MATIERE",
           "regles": {"M0": "eau : >= %.0f %% de l'aire dans une surface d'eau"
                            % (100 * SEUIL_EAU),
                      "M1": "mineral : ouvrage / voirie / batiment",
                      "M2": "vegetal : zone ou organique, >= %.0f %% de pixels "
                            "de masque R < %d" % (100 * SEUIL_VEG, SEUIL_R),
                      "M3": "mineral : zone ou organique sinon",
                      "M4": "hors emprise des masques : classe de la donnee"},
           "tolerance_raster_vecteur_m": round(TOL_RASTER_M, 5),
           "parcelles": {k: cpt[k] for k in cpt},
           "aires_m2": {k: round(aire[k], 1) for k in aire},
           "regles_appliquees": regles,
           "hors_masque": {"parcelles": n_sans_masque,
                           "m2": round(a_sans_masque, 1),
                           "cellules_masque": len(M), "cellules_plan": 49},
           "conflits_donnee_visible": {
               k: {"parcelles": len(conflits[k]), "m2": round(a_conf[k], 1),
                   "pires": [{"id": c[1], "m2": round(c[0], 1),
                              "vegetal_pc": c[2]} for c in conflits[k][:20]]}
               for k in ("dur_peint_vegetal", "zone_verte_minerale")},
           "biefs_n": len(biefs)}
    ecrire_json(os.path.join(OUT, "matiere.json"), rep)
    chrono("C2 TOTAL", time.time() - t0, "")
    jalon("C2 terminee (%.1f s) — _PLAN_B.png + plan_ville/v1/matiere.json"
          % (time.time() - t0))


if __name__ == "__main__":
    main()
