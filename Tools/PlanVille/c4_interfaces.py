# -*- coding: utf-8 -*-
"""PLAN DE VILLE / etape D — LA COUCHE ④ INTERFACES : le catalogue ferme.

CHAQUE frontiere entre deux parcelles recoit une resolution prise dans un
CATALOGUE FERME NATIONAL, calculee depuis (matiere A, matiere B, dZ le long) :

    rien          vegetal|vegetal, dZ <= 30 cm (l'herbe absorbe)
    talus         vegetal|vegetal, dZ >  30 cm
    emmarchement  une des deux parcelles est un escalier (side-car BD TOPO)
    affleurement  dZ <= 2 cm
    bordure       2 cm < dZ <= 20 cm (vue nominale 14 cm, CEREMA T2/CC1)
    mur           20 cm < dZ <= 12 m (hauteur = dZ)

Tout ce qui n'entre pas dans ces bornes part dans la LISTE D'ARBITRAGE,
chiffree et bornee : c'est un LIVRABLE, pas un echec.

Le dZ n'est pas devine : les deux parcelles portent une LOI de Z (etape C) et
on l'EVALUE le long de la frontiere (constante -> sa cote ; profil -> le profil
projete sur l'axe ; drapage -> le MNT lu comme le moteur).
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
from shapely.geometry import LineString, Point
from shapely.strtree import STRtree

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (CACHE, OUT, chrono, ecrire_json, jalon, sol_rendu)

# --- LE CATALOGUE : ses bornes, nommees --------------------------------------
AFFLEUREMENT_M = 0.02     # ressaut tolere (arrete accessibilite du 15/01/2007)
BORDURE_MAX_M = 0.20      # vue maximale d'une bordure de trottoir (CEREMA)
BORDURE_NOMINALE_M = 0.14 # vue nominale T2/CC1
TALUS_MIN_M = 0.30        # en dessous, l'herbe absorbe : `rien`
MUR_MAX_M = 12.0          # au-dela, hors catalogue -> arbitrage
N_SONDES = 5              # sondes de dZ le long d'une frontiere
LONG_MIN_M = 0.01         # sous cette longueur, un contact n'est pas une frontiere

CATALOGUE = ["rien", "talus", "emmarchement", "affleurement", "bordure", "mur"]


class Cote(object):
    """Evaluateur de la loi de Z d'une parcelle en un point."""

    def __init__(self, parcelles, lois, sol):
        self.sol = sol
        self.loi = lois
        self.axe = {}
        for p in parcelles:
            a = (p.get("meta") or {}).get("axe")
            if a:
                self.axe[p["id"]] = LineString([(float(q[0]), float(q[1]))
                                                for q in a])

    def z(self, pid, xs, ys):
        L = self.loi.get(pid)
        if L is None:
            return None
        if L["loi"] == "constante":
            return np.full(len(xs), float(L["z_m"]))
        if L["loi"] == "drapage":
            return np.asarray(self.sol.z(np.asarray(xs), np.asarray(ys)),
                              dtype=float)
        if L["loi"] == "profil_troncon":
            ax = self.axe.get(L.get("loi_heritee_de") or pid) or self.axe.get(pid)
            pr = L.get("profil")
            if ax is None or not pr or not pr["pts"]:
                return np.full(len(xs), float(pr["pts"][0][1]) if pr
                               and pr["pts"] else 0.0)
            S = np.array([q[0] for q in pr["pts"]], dtype=float)
            Z = np.array([q[1] for q in pr["pts"]], dtype=float)
            ss = np.array([ax.project(Point(float(x), float(y)))
                           for x, y in zip(xs, ys)], dtype=float)
            return np.interp(ss, S, Z)
        return None


def sondes(g, n=N_SONDES):
    """n points repartis le long d'une frontiere (geometrie lineaire)."""
    if g.geom_type == "LineString":
        lines = [g]
    elif hasattr(g, "geoms"):
        lines = [q for q in g.geoms if q.geom_type == "LineString"]
    else:
        return [], []
    lines = [q for q in lines if q.length > 0]
    if not lines:
        # contact ponctuel : on sonde le point lui-meme
        c = g.representative_point()
        return [c.x], [c.y]
    L = max(lines, key=lambda q: q.length)
    xs, ys = [], []
    for k in range(n):
        p = L.interpolate((k + 0.5) / n, normalized=True)
        xs.append(p.x)
        ys.append(p.y)
    return xs, ys


def resolution(mA, mB, dz, escalier):
    """LA REGLE. Rend (type, motif) ou (None, motif) si hors catalogue."""
    if escalier:
        return "emmarchement", "une des deux parcelles est un escalier"
    if mA == "vegetal" and mB == "vegetal":
        if dz <= TALUS_MIN_M:
            return "rien", "vegetal|vegetal, dZ <= %.2f m : l'herbe absorbe" \
                % TALUS_MIN_M
        return "talus", "vegetal|vegetal, dZ > %.2f m" % TALUS_MIN_M
    if dz <= AFFLEUREMENT_M:
        return "affleurement", "dZ <= %.2f m" % AFFLEUREMENT_M
    if dz <= BORDURE_MAX_M:
        return "bordure", "%.2f m < dZ <= %.2f m" % (AFFLEUREMENT_M,
                                                     BORDURE_MAX_M)
    if dz <= MUR_MAX_M:
        return "mur", "%.2f m < dZ <= %.1f m, hauteur = dZ" % (BORDURE_MAX_M,
                                                              MUR_MAX_M)
    return None, "dZ = %.2f m au-dessus du plafond de mur (%.1f m)" % (dz,
                                                                       MUR_MAX_M)


def main():
    t0 = time.time()
    sol = sol_rendu()
    with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as f:
        D = pickle.load(f)
    P = D["parcelles"]
    for p in P:
        p["geom"] = shapely.from_wkb(p["geom"])
    with open(os.path.join(CACHE, "matiere.pkl"), "rb") as f:
        mat = pickle.load(f)["matiere"]
    with open(os.path.join(CACHE, "niveaux.pkl"), "rb") as f:
        lois = pickle.load(f)["lois"]
    C = Cote(P, lois, sol)
    esc = set(p["id"] for p in P
              if (p.get("meta") or {}).get("nature") == "escalier")
    jalon("C4/ENTREES : %d parcelles, %d escaliers a emmarchement" % (len(P),
                                                                      len(esc)))

    t1 = time.time()
    G = [p["geom"] for p in P]
    T = STRtree(G)
    A, B = T.query(G, predicate="intersects")
    m = A < B
    A, B = A[m], B[m]
    chrono("C4/paires", time.time() - t1, "%d paires candidates" % len(A))
    jalon("C4/PAIRES : %d couples de parcelles en contact (requete groupee)"
          % len(A))

    t2 = time.time()
    fronts = []
    cpt = {k: 0 for k in CATALOGUE}
    lng = {k: 0.0 for k in CATALOGUE}
    arb = []
    l_tot = 0.0
    for n, (i, j) in enumerate(zip(A, B)):
        i, j = int(i), int(j)
        # ⚠️ La frontiere se prend sur les BORDS (`boundary ∩ boundary`), pas
        # sur `A ∩ B`. Defaut MESURE a la passe d'export : sur 546 contacts,
        # GEOS rend `A ∩ B` sous la forme d'un polygone DEGENERE (aire nulle,
        # MultiPolygon ou GeometryCollection) ; `.length` y vaut alors le
        # PERIMETRE — environ le DOUBLE de la longueur de contact — et aucune
        # polyligne n'en sort pour le contrat machine. L'intersection des bords
        # est lineaire par definition.
        try:
            it = G[i].boundary.intersection(G[j].boundary)
        except Exception:
            continue
        if it.is_empty:
            continue
        Lg = it.length
        if Lg < LONG_MIN_M:
            continue
        xs, ys = sondes(it)
        if not xs:
            continue
        pa, pb = P[i]["id"], P[j]["id"]
        za, zb = C.z(pa, xs, ys), C.z(pb, xs, ys)
        if za is None or zb is None:
            continue
        d = np.abs(za - zb)
        dz = float(np.median(d))
        dzmax = float(d.max())
        mA, mB = mat[pa][0], mat[pb][0]
        typ, motif = resolution(mA, mB, dz, (pa in esc) or (pb in esc))
        c = it.representative_point()
        rec = {"a": pa, "b": pb, "mat": [mA, mB], "type": typ,
               "dz_m": round(dz, 4), "dz_max_m": round(dzmax, 4),
               "longueur_m": round(Lg, 3),
               "x": round(c.x, 2), "y": round(c.y, 2), "motif": motif}
        if typ is None:
            arb.append(rec)
        else:
            cpt[typ] += 1
            lng[typ] += Lg
            if typ == "bordure":
                rec["h_m"] = round(max(dz, 0.0), 4)
                rec["h_nominale_m"] = BORDURE_NOMINALE_M
            elif typ == "mur":
                rec["h_m"] = round(dz, 4)
        fronts.append(rec)
        l_tot += Lg
        if (n + 1) % 40000 == 0:
            jalon("C4/  interfaces : %d / %d paires (%.0f s)"
                  % (n + 1, len(A), time.time() - t2))
    chrono("C4/interfaces", time.time() - t2, "%d frontieres" % len(fronts))
    jalon("C4/⭐ TABLE DES INTERFACES : %d frontieres, %.0f m de lineaire ; "
          "%s ; HORS CATALOGUE : %d frontieres (%.0f m)"
          % (len(fronts), l_tot,
             " | ".join("%s %d / %.0f m" % (k, cpt[k], lng[k])
                        for k in CATALOGUE),
             len(arb), sum(a["longueur_m"] for a in arb)))

    # --- INVARIANTS de la couche -------------------------------------------
    # « sans resolution » = ni une entree du catalogue, ni un cas d'arbitrage
    # declare. Les hors-catalogue SONT resolus : ils partent a l'arbitrage,
    # qui est un livrable.
    ids_arb = set((a["a"], a["b"]) for a in arb)
    sans = sum(1 for f in fronts
               if f["type"] is None and (f["a"], f["b"]) not in ids_arb)
    mineral_sur_veg = [f for f in fronts
                       if f["mat"] == ["vegetal", "vegetal"]
                       and f["type"] in ("bordure", "mur", "emmarchement")]
    hors_borne = []
    EPS = 1e-4                 # l'arrondi d'ecriture du dZ (4 decimales)
    for f in fronts:
        t, dz = f["type"], f["dz_m"]
        if t == "affleurement" and dz > AFFLEUREMENT_M + EPS:
            hors_borne.append(f)
        elif t == "bordure" and not (AFFLEUREMENT_M - EPS < dz
                                     <= BORDURE_MAX_M + EPS):
            hors_borne.append(f)
        elif t == "mur" and not (BORDURE_MAX_M - EPS < dz <= MUR_MAX_M + EPS):
            hors_borne.append(f)
        elif t == "rien" and dz > TALUS_MIN_M + EPS:
            hors_borne.append(f)
    paires = {}
    for f in fronts:
        if f["type"] == "mur":
            k = "|".join(sorted([f["a"].split("/")[0], f["b"].split("/")[0]]))
            paires[k] = paires.get(k, 0) + 1
    jalon("C4/⭐ INVARIANTS : frontieres sans resolution NI arbitrage %d "
          "(cible 0) | objets mineraux sur frontiere vegetal|vegetal %d "
          "(cible 0) | dZ hors des bornes de sa resolution %d (cible 0) ; "
          "repartition des `mur` par couple de proprietaires : %s"
          % (sans, len(mineral_sur_veg), len(hors_borne),
             json.dumps(dict(sorted(paires.items(), key=lambda kv: -kv[1])[:8]),
                        sort_keys=False)))

    arb.sort(key=lambda a: -a["dz_m"])
    with open(os.path.join(CACHE, "interfaces.pkl"), "wb") as f:
        pickle.dump({"fronts": fronts}, f, protocol=4)
    rep = {"couche": "INTERFACES",
           "catalogue": CATALOGUE,
           "bornes_m": {"AFFLEUREMENT_M": AFFLEUREMENT_M,
                        "BORDURE_MAX_M": BORDURE_MAX_M,
                        "BORDURE_NOMINALE_M": BORDURE_NOMINALE_M,
                        "TALUS_MIN_M": TALUS_MIN_M, "MUR_MAX_M": MUR_MAX_M},
           "frontieres_n": len(fronts),
           "lineaire_m": round(l_tot, 1),
           "par_type": {k: {"n": cpt[k], "m": round(lng[k], 1)}
                        for k in CATALOGUE},
           "invariants": {"sans_resolution_ni_arbitrage_n": sans,
                          "mineral_sur_vegetal_vegetal_n": len(mineral_sur_veg),
                          "dz_hors_bornes_n": len(hors_borne)},
           "murs_par_couple_de_proprietaires": paires,
           "arbitrage": {"n": len(arb),
                         "m": round(sum(a["longueur_m"] for a in arb), 1),
                         "cas": arb[:200]}}
    ecrire_json(os.path.join(OUT, "interfaces.json"), rep)
    chrono("C4 TOTAL", time.time() - t0, "")
    jalon("C4 terminee (%.1f s) — plan_ville/v1/interfaces.json"
          % (time.time() - t0))


if __name__ == "__main__":
    main()
