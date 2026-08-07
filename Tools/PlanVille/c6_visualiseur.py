# -*- coding: utf-8 -*-
"""PLAN DE VILLE / etape E (2/2) — L'EXPORT DU VISUALISEUR WEB LOCAL.

Le plan reste a PLEINE PRECISION dans `plan_ville/v1` ; ce qui part au
visualiseur est SIMPLIFIE A L'AFFICHAGE SEULEMENT (tolerance declaree), et les
donnees sont embarquees en fichiers `.js` (`var ... = [...]`) — jamais en JSON
charge par `fetch`, que le mode `file://` interdit (CORS).

Coordonnees : le repere local (x est, y sud) est converti en WGS84 par la
georeference du MNT (equirectangulaire locale). Le fond de plan est
l'orthophoto IGN servie en tuiles par data.geopf.fr.
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

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (CACHE, CELL_M, OUT, PLAN, chrono, jalon, wgs84)

VISU = os.path.join(PLAN, "visualiseur")
DATA = os.path.join(VISU, "data")
SIMPLIF_PARC_M = 0.20      # simplification D'AFFICHAGE des parcelles
SIMPLIF_FRONT_M = 0.50     # simplification D'AFFICHAGE des frontieres
AIRE_MIN_AFF_M2 = 0.5      # sous cette aire, la parcelle n'est pas dessinee
LONG_MIN_AFF_M = 1.0       # sous cette longueur, la frontiere n'est pas dessinee
PAQUET = 4000              # parcelles par fichier .js

PROPS = ["ouvrage", "voirie", "batiment", "zone", "organique"]
MATS = ["mineral", "vegetal", "eau"]
LOIS = ["constante", "profil_troncon", "drapage"]
TYPES = ["rien", "talus", "emmarchement", "affleurement", "bordure", "mur",
         "HORS CATALOGUE"]


def anneaux_wgs84(g, tol):
    """Les anneaux d'un polygone, simplifies puis convertis en WGS84, en
    tableaux plats [lon, lat, lon, lat, ...] arrondis a 6 decimales."""
    q = g.simplify(tol, preserve_topology=False)
    if q.is_empty:
        q = g
    out = []
    geoms = q.geoms if hasattr(q, "geoms") else [q]
    for h in geoms:
        if h.geom_type != "Polygon":
            continue
        for ring in [h.exterior] + list(h.interiors):
            c = np.asarray(ring.coords)
            if len(c) < 4:
                continue
            lon, lat = wgs84(c[:, 0], c[:, 1])
            flat = np.empty(2 * len(lon))
            flat[0::2] = np.round(lon, 6)
            flat[1::2] = np.round(lat, 6)
            out.append([float(v) for v in flat])
    return out


def ligne_wgs84(g, tol):
    q = g.simplify(tol, preserve_topology=False)
    if q.is_empty:
        q = g
    out = []
    geoms = q.geoms if hasattr(q, "geoms") else [q]
    for h in geoms:
        if h.geom_type != "LineString":
            continue
        c = np.asarray(h.coords)
        if len(c) < 2:
            continue
        lon, lat = wgs84(c[:, 0], c[:, 1])
        flat = np.empty(2 * len(lon))
        flat[0::2] = np.round(lon, 6)
        flat[1::2] = np.round(lat, 6)
        out.append([float(v) for v in flat])
    return out


def ecrire_js(path, nom, obj):
    s = "var %s = %s;\n" % (nom, json.dumps(obj, separators=(",", ":"),
                                            ensure_ascii=False))
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)
    return os.path.getsize(path)


def main():
    t0 = time.time()
    for d in (VISU, DATA):
        if not os.path.isdir(d):
            os.makedirs(d)
    with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as f:
        D = pickle.load(f)
    P = D["parcelles"]
    for p in P:
        p["geom"] = shapely.from_wkb(p["geom"])
    with open(os.path.join(CACHE, "matiere.pkl"), "rb") as f:
        mat = pickle.load(f)["matiere"]
    with open(os.path.join(CACHE, "niveaux.pkl"), "rb") as f:
        lois = pickle.load(f)["lois"]
    with open(os.path.join(CACHE, "interfaces.pkl"), "rb") as f:
        fronts = pickle.load(f)["fronts"]

    # ---------------------------------------------------------- PARCELLES ----
    t1 = time.time()
    lots, cur = [], []
    n_aff = n_saute = 0
    octets = 0
    for p in P:
        g = p["geom"]
        if g.area < AIRE_MIN_AFF_M2:
            n_saute += 1
            continue
        L = lois.get(p["id"]) or {}
        m, info = mat[p["id"]]
        rings = anneaux_wgs84(g, SIMPLIF_PARC_M)
        if not rings:
            n_saute += 1
            continue
        cur.append([p["id"], PROPS.index(p["proprietaire"]), MATS.index(m),
                    (LOIS.index(L["loi"]) if L.get("loi") in LOIS else -1),
                    (round(L["z_m"], 2) if L.get("z_m") is not None else None),
                    round(g.area, 1),
                    (info.get("veg_pc") if info else None),
                    (1 if (p.get("meta") or {}).get("heritee") else 0),
                    rings])
        n_aff += 1
        if len(cur) >= PAQUET:
            lots.append(cur)
            cur = []
    if cur:
        lots.append(cur)
    for i, L in enumerate(lots):
        octets += ecrire_js(os.path.join(DATA, "parc_%03d.js" % i),
                            "PARC_%03d" % i, L)
    chrono("C6/parcelles", time.time() - t1,
           "%d affichees, %d lots, %.1f Mo" % (n_aff, len(lots), octets / 1e6))

    # --------------------------------------------------------- FRONTIERES ----
    t2 = time.time()
    byid = {p["id"]: p["geom"] for p in P}
    F, nf = [], 0
    for f in fronts:
        if f["longueur_m"] < LONG_MIN_AFF_M:
            continue
        a, b = byid.get(f["a"]), byid.get(f["b"])
        if a is None or b is None:
            continue
        try:
            it = a.boundary.intersection(b.boundary)
        except Exception:
            continue
        lg = ligne_wgs84(it, SIMPLIF_FRONT_M)
        if not lg:
            continue
        t = f["type"] if f["type"] in TYPES else "HORS CATALOGUE"
        # classe de MARCHE (carte des marches, anti-recidive) :
        # 0 <= 2 cm | 1 <= 20 cm | 2 <= 1 m | 3 > 1 m
        dz = f["dz_m"]
        cm = 0 if dz <= 0.02 else (1 if dz <= 0.20 else (2 if dz <= 1.0 else 3))
        F.append([TYPES.index(t), f["dz_m"], round(f["longueur_m"], 1),
                  MATS.index(f["mat"][0]), MATS.index(f["mat"][1]),
                  f["a"], f["b"], lg, cm])
        nf += 1
    of = 0
    for i in range(0, len(F), PAQUET):
        of += ecrire_js(os.path.join(DATA, "front_%03d.js" % (i // PAQUET)),
                        "FRONT_%03d" % (i // PAQUET), F[i:i + PAQUET])
    chrono("C6/frontieres", time.time() - t2,
           "%d affichees, %.1f Mo" % (nf, of / 1e6))

    # -------------------------------------------------------------- SEMIS ----
    t3 = time.time()
    ns = 0
    os_ = 0
    try:
        with open(os.path.join(CACHE, "semis.pkl"), "rb") as f:
            S = pickle.load(f)
        lon, lat = wgs84(S["x"], S["y"])
        pts = np.empty(2 * len(lon))
        pts[0::2] = np.round(lon, 6)
        pts[1::2] = np.round(lat, 6)
        ns = len(lon)
        os_ = ecrire_js(os.path.join(DATA, "semis.js"), "SEMIS",
                        [float(v) for v in pts])
    except Exception as e:
        jalon("C6/semis indisponible (%s)" % e)
    chrono("C6/semis", time.time() - t3, "%d instances, %.1f Mo" % (ns,
                                                                    os_ / 1e6))

    # --------------------------------------------------------------- META ----
    meta = {}
    for f in ("qui", "matiere", "niveaux", "interfaces", "juges"):
        p = os.path.join(OUT, "%s.json" % f)
        if os.path.exists(p):
            meta[f] = json.load(io.open(p, encoding="utf-8"))
    tour = []
    for s in (meta.get("juges", {}).get("tournee") or []):
        lon, lat = wgs84(np.array([s["x"]]), np.array([s["y"]]))
        tour.append({"regle": s["regle"], "titre": s["titre"],
                     "detail": s["detail"], "lon": round(float(lon[0]), 6),
                     "lat": round(float(lat[0]), 6),
                     "x": s["x"], "y": s["y"]})
    cx = [c for c in meta.get("qui", {}).get("domaine_cellules", [])]
    lon0, lat0 = wgs84(np.array([0.0]), np.array([0.0]))
    M = {"lots_parcelles": len(lots),
         "lots_frontieres": (len(F) + PAQUET - 1) // PAQUET,
         "props": PROPS, "mats": MATS, "lois": LOIS, "types": TYPES,
         "marches": ["<= 2 cm (affleurement)", "<= 20 cm (bordure)",
                     "<= 1 m", "> 1 m"],
         "parcelles_affichees": n_aff, "parcelles_non_affichees": n_saute,
         "frontieres_affichees": nf,
         "semis_n": ns,
         "simplification_affichage_m": {"parcelles": SIMPLIF_PARC_M,
                                        "frontieres": SIMPLIF_FRONT_M},
         "centre": [float(lon0[0]), float(lat0[0])],
         "cellules": cx,
         "tournee": tour,
         "compteurs": meta.get("juges", {}).get("compteurs_par_regle", {}),
         "resume": {
             "qui": meta.get("qui", {}),
             "matiere": {k: v for k, v in meta.get("matiere", {}).items()
                         if k != "conflits_donnee_visible"},
             "conflits": meta.get("matiere", {}).get("conflits_donnee_visible",
                                                     {}),
             "niveaux": meta.get("niveaux", {}),
             "interfaces": {k: v for k, v in meta.get("interfaces", {}).items()
                            if k != "arbitrage"},
             "arbitrage": meta.get("interfaces", {}).get("arbitrage", {}),
             "juges": {k: v for k, v in meta.get("juges", {}).items()
                       if k != "tournee"}}}
    om = ecrire_js(os.path.join(DATA, "meta.js"), "META", M)
    total = octets + of + os_ + om
    jalon("C6/⭐ VISUALISEUR : %d parcelles dessinees (%d sautees sous %.1f m2), "
          "%d frontieres, %d instances de semis retenues ; %d fichiers .js, "
          "%.1f Mo au total (simplification D'AFFICHAGE %.2f m / %.2f m ; le "
          "plan reste a pleine precision)"
          % (n_aff, n_saute, AIRE_MIN_AFF_M2, nf, ns,
             len(lots) + (len(F) + PAQUET - 1) // PAQUET + 2, total / 1e6,
             SIMPLIF_PARC_M, SIMPLIF_FRONT_M))
    chrono("C6 TOTAL", time.time() - t0, "%.1f Mo" % (total / 1e6))
    return M


if __name__ == "__main__":
    main()
