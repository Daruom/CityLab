# -*- coding: utf-8 -*-
"""PLAN DE VILLE — LE RESEAU VIAIRE NOUE : source UNIQUE des noeuds.

Importe par c1 (qui en tire les PARCELLES DE CARREFOUR) et par c3 (qui en tire
les COTES DE NOEUD) : les deux DOIVENT voir exactement le meme graphe, sinon
une chaussee ne rejoindrait pas son carrefour.

⚠️ NOUAGE : le graphe des sources n'est pas noue (mesure : 14 562 paires d'axes
se croisent, dont 7 298 sans extremite commune). On noue donc extremites UNION
croisements.

⚠️⚠️ EXCLUSION DES CROISEMENTS DENIVELES (directive coordinateur) : un
franchissement (pont, tremie) ne doit JAMAIS partager de cote avec la voie
qu'il enjambe — aplatir un pont dans sa rue inferieure serait une catastrophe
silencieuse. Critere NATIONAL, sans identifiant :
  * l'un des deux axes est porte par un OUVRAGE DECLARE (side-car Ponts, ou
    attribut `bridge` du graphe, ou `layer` non nul), OU
  * l'ecart entre les cotes PORTEUSES des deux axes au point de croisement
    depasse SEUIL_DENIVELE_M (la cote porteuse d'un axe de pont est sa cote
    DECLAREE ; celle d'un axe ordinaire est le MNT).
"""
import math
import os
import sys

import numpy as np
import shapely
from shapely.geometry import LineString, Point
from shapely.ops import unary_union
from shapely.strtree import STRtree

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import SRC, charge_json, jalon

SNAP_NOEUD_M = 0.10           # deux points a moins de 10 cm = UN noeud
MARGE_CARREFOUR_M = 0.50      # marge du plateau au-dela de la demi-largeur
SEUIL_DENIVELE_M = 2.50       # au-dela, le croisement est un FRANCHISSEMENT
PART_PLATEAU_MAX = 0.25       # un plateau ne mange jamais plus d'un quart
#                               du troncon a chaque bout
TOL_PONT_M = 3.00             # distance a un axe de pont declare


def axes_de_pont():
    """Les axes 3D declares par les side-cars Ponts (z_source = bdtopo3d)."""
    L, Z = [], []
    d = os.path.join(SRC, "Ponts")
    for f in sorted(os.listdir(d)):
        if not f.endswith(".json"):
            continue
        for it in (charge_json(os.path.join(d, f)).get("ponts") or []):
            pts = [q for q in (it.get("pts") or [])
                   if len(q) >= 3 and q[0] is not None and q[2] is not None]
            if len(pts) < 2:
                continue
            L.append(LineString([(float(q[0]), float(q[1])) for q in pts]))
            Z.append(np.array([float(q[2]) for q in pts]))
    return L, Z


def axes_du_graphe():
    """LA source unique du graphe viaire : `routes_3x3.json`, telle que C1 la
    lit pour construire les emprises de voirie. C1 et C3 DOIVENT partir d'ici,
    sinon leurs noeuds divergent."""
    import os as _os
    from c0_socle import ENT as _ENT
    R = charge_json(_os.path.join(_ENT, "routes_3x3.json"))
    axes = {}
    for i, r in enumerate(R.get("roads", [])):
        pts = r.get("pts") or []
        if len(pts) < 2:
            continue
        axes[i] = {"axe": [[round(float(q[0]), 3), round(float(q[1]), 3)]
                           for q in pts],
                   "w": float(r.get("w") or 6.0),
                   "bridge": bool(r.get("bridge")),
                   "layer": int(r.get("layer") or 0)}
    return axes


def reseau(axes, sol, journal=True):
    """`axes` = {i: {"axe": [[x,y],...], "w": largeur, "bridge": bool,
    "layer": int}}. Rend (NX, NY, RN, INC, stats)."""
    cles = sorted(axes)
    LS = [LineString([(float(q[0]), float(q[1])) for q in axes[i]["axe"]])
          for i in cles]
    PL, PZ = axes_de_pont()
    TP = STRtree(PL) if PL else None

    def porte_ouvrage(k):
        a = axes[cles[k]]
        if a.get("bridge") in (True, "True") or int(a.get("layer") or 0) != 0:
            return True
        if TP is None:
            return False
        for j in TP.query(LS[k].buffer(TOL_PONT_M)):
            if PL[int(j)].distance(LS[k]) <= TOL_PONT_M:
                return True
        return False

    porte = [porte_ouvrage(k) for k in range(len(LS))]

    def cote_porteuse(k, x, y):
        """La cote que porte cet axe en (x, y) : la cote DECLAREE s'il est un
        pont, le MNT sinon."""
        if porte[k] and TP is not None:
            best, bd = None, 1e18
            for j in TP.query(Point(x, y).buffer(TOL_PONT_M)):
                j = int(j)
                dd = PL[j].distance(Point(x, y))
                if dd < bd:
                    bd, best = dd, j
            if best is not None and bd <= TOL_PONT_M:
                s = PL[best].project(Point(x, y))
                c = np.asarray(PL[best].coords)
                dd = np.concatenate(([0.0], np.cumsum(
                    np.hypot(np.diff(c[:, 0]), np.diff(c[:, 1])))))
                return float(np.interp(s, dd, PZ[best]))
        return float(sol.z(np.array([x]), np.array([y]))[0])

    # ---- les points candidats : extremites + croisements -------------------
    brut = []
    for k, l in enumerate(LS):
        brut.append((l.coords[0][0], l.coords[0][1]))
        brut.append((l.coords[-1][0], l.coords[-1][1]))
    n_ext = len(brut)
    T = STRtree(LS)
    A, B = T.query(LS, predicate="intersects")
    m = A < B
    n_cx = n_den = n_den_ouv = n_den_z = 0
    for i, j in zip(A[m], B[m]):
        i, j = int(i), int(j)
        try:
            it = LS[i].intersection(LS[j])
        except Exception:
            continue
        pts = []
        for g in (it.geoms if hasattr(it, "geoms") else [it]):
            if g.geom_type == "Point":
                pts.append((g.x, g.y))
            elif g.geom_type == "LineString" and g.length > 0:
                pts.append(g.coords[0])
                pts.append(g.coords[-1])
        for (x, y) in pts:
            n_cx += 1
            ouvr = porte[i] != porte[j]
            zi = cote_porteuse(i, x, y)
            zj = cote_porteuse(j, x, y)
            den_z = abs(zi - zj) > SEUIL_DENIVELE_M
            if ouvr or den_z:
                n_den += 1
                n_den_ouv += 1 if ouvr else 0
                n_den_z += 1 if (den_z and not ouvr) else 0
                continue          # ⛔ franchissement : PAS de noeud commun
            brut.append((x, y))
    # ---- fusion --------------------------------------------------------------
    cle = {}
    for x, y in brut:
        k = (round(float(x) / SNAP_NOEUD_M), round(float(y) / SNAP_NOEUD_M))
        cle.setdefault(k, []).append((float(x), float(y)))
    NX, NY = [], []
    for k in sorted(cle):
        L2 = cle[k]
        NX.append(float(np.mean([q[0] for q in L2])))
        NY.append(float(np.mean([q[1] for q in L2])))
    NX, NY = np.asarray(NX), np.asarray(NY)

    # ---- rayon de plateau et troncons incidents ------------------------------
    TN = STRtree([Point(x, y) for x, y in zip(NX, NY)])
    RN = np.zeros(len(NX))
    INC = {}
    for k, l in enumerate(LS):
        w = float(axes[cles[k]].get("w") or 6.0)
        for j in TN.query(l.buffer(SNAP_NOEUD_M)):
            j = int(j)
            if l.distance(Point(NX[j], NY[j])) > SNAP_NOEUD_M:
                continue
            INC.setdefault(j, set()).add(cles[k])
            RN[j] = max(RN[j], 0.5 * w + MARGE_CARREFOUR_M)
    n_vrai = 0
    for j in range(len(RN)):
        if len(INC.get(j, ())) < 2:
            RN[j] = 0.0           # cul-de-sac ou bord : pas de carrefour
        else:
            n_vrai += 1

    # ---- LE RAYON EFFECTIF, partage entre C1 et C3 --------------------------
    # r_eff(troncon, noeud) : le rayon que le PROFIL pourra reellement aplatir,
    # ecrete par la longueur du troncon et par la distance aux noeuds voisins
    # du MEME troncon. Le disque du carrefour prendra le MINIMUM sur les
    # troncons incidents : il tient alors dans TOUS les plateaux.
    REFF = {}
    RDISQUE = np.zeros(len(NX))
    sur = {}
    for k, l in enumerate(LS):
        js = []
        for j in TN.query(l.buffer(SNAP_NOEUD_M)):
            j = int(j)
            if l.distance(Point(NX[j], NY[j])) > SNAP_NOEUD_M:
                continue
            js.append((float(l.project(Point(NX[j], NY[j]))), j))
        js.sort()
        sur[cles[k]] = js
        Lg = float(l.length)
        for n, (s0, j) in enumerate(js):
            if RN[j] <= 0.0:
                continue
            r = min(float(RN[j]), PART_PLATEAU_MAX * Lg)
            if n > 0:
                r = min(r, 0.5 * (s0 - js[n - 1][0]))
            if n < len(js) - 1:
                r = min(r, 0.5 * (js[n + 1][0] - s0))
            r = max(r, 0.0)
            REFF[(cles[k], j)] = r
    for j in range(len(NX)):
        if RN[j] <= 0.0:
            continue
        rs = [REFF.get((i, j), 0.0) for i in INC.get(j, ())]
        RDISQUE[j] = min(rs) if rs else 0.0
    st = {"axes": len(LS), "extremites": n_ext, "croisements": n_cx,
          "rayon_disque_median_m": round(float(np.median(
              RDISQUE[RDISQUE > 0])), 3) if (RDISQUE > 0).any() else 0.0,
          "croisements_deniveles": n_den,
          "deniveles_par_ouvrage": n_den_ouv,
          "deniveles_par_ecart_z": n_den_z,
          "noeuds": int(len(NX)), "carrefours": n_vrai,
          "axes_portes_par_un_ouvrage": int(sum(porte)),
          "SEUIL_DENIVELE_M": SEUIL_DENIVELE_M,
          "SNAP_NOEUD_M": SNAP_NOEUD_M,
          "MARGE_CARREFOUR_M": MARGE_CARREFOUR_M}
    if journal:
        jalon("RESEAU/NOUAGE : %d axes (%d portes par un ouvrage declare) ; "
              "%d extremites + %d croisements geometriques ; ⛔ %d croisements "
              "DENIVELES exclus du nouage (%d parce qu'un seul des deux axes "
              "est porte par un ouvrage, %d parce que l'ecart des cotes "
              "porteuses depasse %.2f m) -> %d noeuds apres fusion a %.2f m, "
              "dont %d VRAIS carrefours (2 troncons incidents ou plus)"
              % (st["axes"], st["axes_portes_par_un_ouvrage"], n_ext, n_cx,
                 n_den, n_den_ouv, n_den_z, SEUIL_DENIVELE_M, len(NX),
                 SNAP_NOEUD_M, n_vrai))
    return NX, NY, RN, INC, st, cles, LS, REFF, RDISQUE, sur


def emprises_carrefour(NX, NY, RDISQUE, INC, axes, cles, LS, bout=2):
    """La PARCELLE DE CARREFOUR : le disque du plateau INTERSECTE l'emprise des
    voies incidentes — jamais au-dela, pour ne rien voler aux batiments ni a
    l'organique. C'est elle qui portera la cote du noeud, et c'est elle qui
    empeche desormais deux chaussees de se toucher directement."""
    idx = {c: k for k, c in enumerate(cles)}
    out = []
    for j in range(len(NX)):
        if RDISQUE[j] <= 0.0:
            continue
        inc = INC.get(j, ())
        if len(inc) < 2:
            continue
        bufs = []
        for i in inc:
            k = idx[i]
            w = float(axes[i].get("w") or 6.0)
            bufs.append(LS[k].buffer(max(0.5, 0.5 * w), cap_style=bout))
        try:
            g = Point(NX[j], NY[j]).buffer(float(RDISQUE[j])).intersection(
                unary_union(bufs))
        except Exception:
            continue
        if g.is_empty or g.area <= 1e-6:
            continue
        out.append((g, {"src": "carrefour du reseau noue", "carrefour": True,
                        "noeud": [round(float(NX[j]), 3),
                                  round(float(NY[j]), 3)],
                        "rayon_m": round(float(RDISQUE[j]), 3),
                        "troncons": sorted(inc)}))
    return out
