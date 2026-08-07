# -*- coding: utf-8 -*-
"""PLAN DE VILLE / etape C — LA COUCHE ② NIVEAUX **v2 : LA SOLIDARITE**.

v1 attribuait sa cote a chaque parcelle INDEPENDAMMENT : d'ou les marches en
travers des chaussees, le patchwork de plateaux et la tranchee du quai que
l'utilisateur a vus en vol. v2 garde les TROIS formes (anti-nappe, Playbook
§13.1 — le Z ne s'interpole jamais en champ libre) mais les fait DEPENDRE les
unes des autres, dans un ordre national ou chaque etage s'ancre sur le
precedent :

  ① OUVRAGES        — les cotes DECLAREES par les side-cars sont des ancres
                      fixes (ponts : axes 3D BD TOPO ; murs : profil
                      x,y,z_crete,z_pied ; eau : z_min NGF).
  ② NOEUDS          — UNE cote par noeud du graphe viaire. Un noeud a portee
                      d'une ancre prend la cote de l'ancre, sinon le MNT.
  ③ TRONCONS        — profil en long ANCRE : le releve du MNT est rebase pour
                      passer exactement par les cotes de ses noeuds, avec un
                      PLATEAU DE CARREFOUR autour de chaque noeud (rayon derive
                      des largeurs incidentes) puis regularisation
                      Douglas-Peucker a extremites FIXEES.
  ④ SOLIDARITE VOIRIE — deux troncons dont les emprises se touchent sur une
                      longueur significative sans se rejoindre a un noeud sont
                      SOLIDAIRES : le moins large ADOPTE le profil du PORTEUR
                      (meme axe, meme profil) — dZ nul par construction.
  ⑤ BANDES          — OFFSET du profil du voisin dur PORTEUR : un trottoir SUIT
                      la pente de sa rue (chaussee + bordure nominale), il n'est
                      pas un plateau.
  ⑥ COMMUNAUTES     — les parcelles minerales pietonnes CONTIGUES forment UNE
                      communaute de nivellement -> UNE cote (le p50 de la
                      communaute), plus jamais un patchwork de plateaux.
  ⑦ ANCRAGE         — une parcelle a CONSTANTE dont le contact avec un ouvrage
                      ancre serait de plain-pied prend la cote de l'ouvrage.
  ⑧ ORGANIQUE       — drapage, inchange : le plan n'ecrit aucun Z.

⚠️ LE COTEAU N'EST PAS TOUCHE : la regle d'eligibilite au relief est un
arbitrage utilisateur DIFFERE ; les hors-catalogue de la cellule (2,-2) restent
tels quels.
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
from shapely.geometry import LineString, Point, Polygon
from shapely.strtree import STRtree

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (CACHE, CELL_M, OUT, SRC, alt_capitole_m, charge_json,
                      chrono, ecrire_json, jalon, sol_rendu)
from c1_qui import valide

# --- constantes de NORME et de REGLE, nommees et commentees ------------------
PAS_PROFIL_M = 5.0            # pas d'echantillonnage du profil en long
TOL_PROFIL_M = 0.15           # tolerance de regularisation (Douglas-Peucker)
PENTE_MAX_VOIRIE_PC = 12.0    # voirie urbaine (CEREMA)
PENTE_MAX_CHEMINEMENT_PC = 5.0   # cheminement accessible (arrete 15/01/2007)
RESSAUT_MAX_M = 0.02          # ressaut tolere sans chanfrein (meme arrete)
BORDURE_NOMINALE_M = 0.14     # vue nominale d'une bordure T2/CC1 (CEREMA)
PAS_GRILLE_M = 2.0            # pas d'echantillonnage d'une parcelle a constante

SNAP_NOEUD_M = 0.10           # deux extremites a moins de 10 cm = UN noeud
MARGE_CARREFOUR_M = 0.50      # marge du plateau au-dela de la demi-largeur
L_SOLIDAIRE_M = 2.00          # contact mini pour rendre deux troncons solidaires
K_SOLIDAIRE = 3.0             # ... ET au moins 3 LARGEURS de voie : c'est ce qui
#                               distingue deux troncons qui partagent la MEME
#                               surface (contact long et parallele) d'un simple
#                               carrefour (contact court). Mesure : au seuil de
#                               3 largeurs il reste 823 contacts dont 52
#                               seulement partagent un noeud ; a 2 m secs il en
#                               restait 8 054 et l'union transitive avalait tout
#                               le reseau (longueur mediane de troncon 954 m).
MIN_SEG_M = 2.00              # longueur minimale d un segment de profil
PART_PLATEAU_MAX = 0.25       # un plateau ne mange jamais plus d'un quart du
#                               troncon a chaque bout
L_ANCRE_M = 1.00              # contact mini pour ancrer une parcelle a un ouvrage
PLAIN_PIED_MAX_M = 0.20       # au-dela, le contact n'est plus de plain-pied
D_ANCRE_NOEUD_M = 3.00        # portee d'une ancre d'ouvrage sur un noeud


def echantillons(g, sol, pas=PAS_GRILLE_M):
    xa, ya, xb, yb = g.bounds
    nx = max(1, int((xb - xa) / pas))
    ny = max(1, int((yb - ya) / pas))
    if nx * ny <= 400000:
        X = xa + (np.arange(nx) + 0.5) * (xb - xa) / nx
        Y = ya + (np.arange(ny) + 0.5) * (yb - ya) / ny
        GX, GY = np.meshgrid(X, Y)
        GX, GY = GX.ravel(), GY.ravel()
        m = shapely.contains_xy(g, GX, GY)
        if m.any():
            return sol.z(GX[m], GY[m])
    p = g.representative_point()
    return sol.z(np.array([p.x]), np.array([p.y]))


def dp_profil(S, Z, tol):
    """Douglas-Peucker sur le profil (s, z) : les EXTREMITES sont conservees,
    donc les cotes de noeud restent exactes."""
    n = len(S)
    if n <= 2:
        return list(range(n))
    garde = [False] * n
    garde[0] = garde[n - 1] = True
    pile = [(0, n - 1)]
    while pile:
        a, b = pile.pop()
        if b <= a + 1:
            continue
        s0, z0, s1, z1 = S[a], Z[a], S[b], Z[b]
        ds = s1 - s0
        if abs(ds) < 1e-9:
            continue
        pred = z0 + (S[a:b + 1] - s0) * (z1 - z0) / ds
        d = np.abs(Z[a:b + 1] - pred)
        k = int(np.argmax(d))
        if d[k] > tol:
            garde[a + k] = True
            pile.append((a, a + k))
            pile.append((a + k, b))
    return [i for i in range(n) if garde[i]]


# ======================================================= ① LES ANCRES ========
def ancres(sol):
    """Les cotes DECLAREES par les side-cars. Ce sont des MESURES, pas des
    calculs : elles entrent dans le plan telles quelles."""
    t0 = time.time()
    pts_xy, pts_z, pts_src = [], [], []
    # -- ponts : l'axe est 3D (z_source = bdtopo3d)
    d = os.path.join(SRC, "Ponts")
    n_p = 0
    for f in sorted(os.listdir(d)):
        if not f.endswith(".json"):
            continue
        for it in (charge_json(os.path.join(d, f)).get("ponts") or []):
            for q in (it.get("pts") or []):
                if len(q) >= 3 and q[2] is not None and q[0] is not None:
                    pts_xy.append((float(q[0]), float(q[1])))
                    pts_z.append(float(q[2]))
                    pts_src.append("pont")
            n_p += 1
    # -- murs : profil = [x, y, z_crete, z_pied] ; la CRETE est le niveau ville
    d = os.path.join(SRC, "Murs")
    n_m = 0
    for f in sorted(os.listdir(d)):
        if not f.endswith(".json") or f == "index.json":
            continue
        for it in (charge_json(os.path.join(d, f)).get("murs") or []):
            pr = it.get("profil") or []
            for q in pr:
                if len(q) >= 3 and q[2] is not None and q[0] is not None:
                    pts_xy.append((float(q[0]), float(q[1])))
                    pts_z.append(float(q[2]))
                    pts_src.append("mur_crete")
            n_m += 1
    A = np.asarray(pts_xy) if pts_xy else np.zeros((0, 2))
    Z = np.asarray(pts_z) if pts_z else np.zeros(0)
    T = STRtree([Point(x, y) for x, y in A]) if len(A) else None
    chrono("C3/ancres", time.time() - t0, "%d points" % len(A))
    jalon("C3/① ANCRES D'OUVRAGE : %d points de cote DECLAREE (%d ponts a axe "
          "3D -> %d points, %d murs a profil -> %d points de crete) ; les "
          "escaliers et gradins n'en declarent aucune (ils garderont une cote "
          "calculee, et leurs interfaces restent des emmarchements declares)"
          % (len(A), n_p, sum(1 for s in pts_src if s == "pont"), n_m,
             sum(1 for s in pts_src if s == "mur_crete")))
    return A, Z, T, pts_src


# ======================================================== ② LES NOEUDS =======
def noeuds_du_graphe(P, sol, A, Z, T):
    """Les noeuds viennent du module PARTAGE `cn_reseau` — le meme dont C1 a
    tire les PARCELLES DE CARREFOUR. Une seule verite : sinon une chaussee ne
    rejoindrait pas son carrefour. On n'ajoute ici que l'ANCRAGE des cotes de
    noeud sur les cotes declarees des ouvrages."""
    import cn_reseau as RES
    axes = RES.axes_du_graphe()
    NX, NY, RN, INC, st, cles, LS, REFF, RDISQ, SUR = \
        RES.reseau(axes, sol)
    ZN = np.asarray(sol.z(NX, NY), dtype=float)
    n_anc = 0
    if T is not None and len(NX):
        for i in range(len(NX)):
            j = T.nearest(Point(NX[i], NY[i]))
            if j is None:
                continue
            j = int(j)
            if math.dist((NX[i], NY[i]), (A[j][0], A[j][1])) <= D_ANCRE_NOEUD_M:
                ZN[i] = Z[j]
                n_anc += 1
    jalon("C3/2 NOEUDS : %d noeuds du reseau NOUE (module partage avec C1), "
          "%d vrais carrefours ; %d cotes ancrees sur une cote declaree "
          "d'ouvrage, %d sur le MNT ; de %.2f a %.2f m NGF"
          % (len(NX), st["carrefours"], n_anc, len(NX) - n_anc,
             float(ZN.min()) if len(ZN) else 0.0,
             float(ZN.max()) if len(ZN) else 0.0))
    return NX, NY, ZN, n_anc, RN, st, REFF, SUR


def profil_ancre(axe, sol, TN, NX, NY, ZN, RN, reff=None):
    """Le profil en long ANCRE d'un troncon.

    (a) on releve le MNT le long de l'axe ;
    (b) on RE-BASE le releve pour qu'il passe exactement par les cotes des
        noeuds rencontres (translation affine entre noeuds consecutifs : c'est
        un profil 1D declare, pas une nappe 2D) ;
    (c) on aplatit le PLATEAU DE CARREFOUR autour de chaque noeud (rayon derive
        des largeurs incidentes) — un carrefour est plat, c'est ce qui fait
        que deux troncons incidents s'accordent EXACTEMENT ;
    (d) on regularise par Douglas-Peucker a extremites fixees.
    """
    P = np.asarray(axe, dtype=float)
    d = np.hypot(np.diff(P[:, 0]), np.diff(P[:, 1]))
    S = np.concatenate(([0.0], np.cumsum(d)))
    L = float(S[-1])
    if L < 1e-6:
        z = float(sol.z(np.array([P[0, 0]]), np.array([P[0, 1]]))[0])
        return {"L_m": 0.0, "pts": [[0.0, round(z, 3)]], "pente_max_pc": 0.0,
                "noeuds": 0}
    # --- (a) les noeuds rencontres par cet axe, avec leur abscisse -----------
    ligne = LineString([(float(a), float(b)) for a, b in zip(P[:, 0], P[:, 1])])
    anc = []
    for j in TN.query(ligne.buffer(SNAP_NOEUD_M)):
        j = int(j)
        pt = Point(NX[j], NY[j])
        if ligne.distance(pt) > SNAP_NOEUD_M:
            continue
        r = float(RN[j]) if reff is None else float(reff.get(j, 0.0))
        anc.append((float(ligne.project(pt)), float(ZN[j]), r))
    anc.sort()
    # ⚠️ Les BORDS DE PLATEAU sont des abscisses EXACTES, inserees dans
    # l'echantillonnage : sinon le plateau est quantifie au pas de 5 m et la
    # rampe repart a l'interieur du disque du carrefour.
    n = max(3, int(math.ceil(L / PAS_PROFIL_M)) + 1)
    sup = [0.0, L]
    for s0, _z0, r0 in anc:
        for b in (s0 - r0, s0, s0 + r0):
            if 0.0 <= b <= L:
                sup.append(float(b))
    ss = np.unique(np.round(np.concatenate([np.linspace(0.0, L, n),
                                            np.asarray(sup)]), 6))
    xs = np.interp(ss, S, P[:, 0])
    ys = np.interp(ss, S, P[:, 1])
    zs = np.asarray(sol.z(xs, ys), dtype=float)
    # les extremites sont toujours des noeuds
    if not anc or anc[0][0] > 1e-6:
        anc.insert(0, (0.0, float(zs[0]), 0.0))
    if anc[-1][0] < L - 1e-6:
        anc.append((L, float(zs[-1]), 0.0))
    sa = np.array([a[0] for a in anc])
    za = np.array([a[1] for a in anc])
    zm = np.interp(sa, ss, zs)                  # le MNT aux memes abscisses
    zs = zs + np.interp(ss, sa, za - zm)        # translation affine par morceau

    # --- (c) les plateaux de carrefour ---------------------------------------
    for k, (s0, z0, r0) in enumerate(anc):
        if r0 <= 0.0:
            continue
        # r0 est DEJA le rayon effectif ecrete par le module de reseau : on ne
        # le rabote plus ici, sinon le bord de la parcelle de carrefour
        # ressortirait du plateau (defaut mesure : 49,6 % de marches
        # carrefour|chaussee).
        r = r0
        if r <= 0.0:
            continue
        zs[np.abs(ss - s0) <= r] = z0
    idx = dp_profil(ss, zs, TOL_PROFIL_M)
    # noeuds ET bords de plateau : points de rupture OBLIGATOIRES (les
    # abscisses existent exactement dans `ss`, l'arrondi ne les deplace pas)
    forces = set(idx)
    for s0, z0, r0 in anc:
        for b in (s0 - r0, s0, s0 + r0):
            if 0.0 <= b <= L:
                k = int(np.argmin(np.abs(ss - b)))
                if abs(ss[k] - b) <= 1e-6:
                    forces.add(k)
    idx = sorted(forces)
    # ⚠️ Garde-fou : deux points de rupture trop proches font un segment
    # quasi vertical (mesure sans garde : pente max 11 643 %). On ne garde
    # qu'un point par intervalle de MIN_SEG_M, l'extremite etant prioritaire.
    net = [idx[0]]
    for k in idx[1:]:
        if float(ss[k]) - float(ss[net[-1]]) >= MIN_SEG_M:
            net.append(k)
    if net[-1] != idx[-1]:
        if float(ss[idx[-1]]) - float(ss[net[-1]]) < MIN_SEG_M and len(net) > 1:
            net[-1] = idx[-1]
        else:
            net.append(idx[-1])
    idx = net
    pts = [[round(float(ss[i]), 3), round(float(zs[i]), 3)] for i in idx]
    pm = 0.0
    for k in range(len(pts) - 1):
        ds = pts[k + 1][0] - pts[k][0]
        if ds > 1e-6:
            pm = max(pm, abs(pts[k + 1][1] - pts[k][1]) / ds * 100.0)
    return {"L_m": round(L, 3), "pts": pts, "pente_max_pc": round(pm, 3),
            "noeuds": len(anc)}


# ============================================================ CONTRE-PREUVE ===
def contre_preuve(sol):
    t0 = time.time()
    carte = charge_json(os.path.join(SRC, "Partition", "carte_v2.json"))
    fr = carte["frontieres"]
    Z0 = alt_capitole_m()
    XS, YS, ZR = [], [], []
    n_lignes = 0
    with io.open(os.path.join(SRC, "Partition", "profil_z_v1.json"),
                 encoding="utf-8") as f:
        for k, ligne in enumerate(f):
            ligne = ligne.strip()
            if not ligne or k == 0:
                continue
            d = json.loads(ligne)
            if d.get("g") != "f":
                continue
            i = int(d["i"])
            if i >= len(fr):
                continue
            co = fr[i]["polyligne"]
            z = d.get("z") or []
            pts = []
            for a in range(len(co) - 1):
                (x0, y0), (x1, y1) = co[a], co[a + 1]
                ln = math.hypot(x1 - x0, y1 - y0)
                kk = max(1, int(math.ceil(ln / 1.0)))
                for t in range(kk):
                    u = t / float(kk)
                    pts.append((x0 + (x1 - x0) * u, y0 + (y1 - y0) * u))
            pts.append((co[-1][0], co[-1][1]))
            if len(pts) != len(z):
                continue
            n_lignes += 1
            for (x, y), zv in zip(pts, z):
                XS.append(float(x))
                YS.append(float(y))
                ZR.append(float(zv) / 1000.0 + Z0)
    XS, YS, ZR = np.asarray(XS), np.asarray(YS), np.asarray(ZR)
    ZP = np.asarray(sol.z(XS, YS), dtype=float)
    d = np.abs(ZP - ZR)
    bon = d < 50.0
    q = np.percentile(d[bon], [50, 95, 99]) if bon.any() else [0, 0, 0]
    rep = {"points": int(XS.size), "runs": n_lignes,
           "hors_bornes_n": int((~bon).sum()),
           "ecart_median_m": round(float(q[0]), 6),
           "ecart_p95_m": round(float(q[1]), 6),
           "ecart_p99_m": round(float(q[2]), 6),
           "ecart_max_m": round(float(d[bon].max()) if bon.any() else 0.0, 6),
           "ecart_moyen_m": round(float(d[bon].mean()) if bon.any() else 0.0, 6),
           "sous_1cm_pc": round(100.0 * float((d[bon] < 0.01).mean()), 3),
           "sous_5cm_pc": round(100.0 * float((d[bon] < 0.05).mean()), 3),
           "note": "la lecture Python est celle du MNT nu ; le drapage de "
                   "l'organique n'est pas touche par NIVEAUX v2, cet ecart "
                   "doit donc etre IDENTIQUE a celui de v1"}
    chrono("C3/contre-preuve", time.time() - t0, "%d points" % XS.size)
    jalon("C3/⭐ CONTRE-PREUVE Z (inchangee par construction : v2 ne touche pas "
          "au drapage) : %d points, ecart median %.6f m, %.3f %% sous 1 cm, "
          "max %.6f m" % (rep["points"], rep["ecart_median_m"],
                          rep["sous_1cm_pc"], rep["ecart_max_m"]))
    return rep


def main():
    t0 = time.time()
    sol = sol_rendu()
    with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as f:
        D = pickle.load(f)
    P = D["parcelles"]
    for p in P:
        p["geom"] = shapely.from_wkb(p["geom"])
    with open(os.path.join(CACHE, "matiere.pkl"), "rb") as f:
        MT = pickle.load(f)
    mat = MT["matiere"]
    biefs = [{"geom": shapely.from_wkb(b["geom"]),
              **{k: b[k] for k in b if k != "geom"}} for b in MT["biefs"]]
    TB = STRtree([b["geom"] for b in biefs]) if biefs else None
    byid = {p["id"]: p for p in P}

    A, ZA, TA, SRCA = ancres(sol)
    NX, NY, ZN, n_anc, RN, STRES, REFF, SUR = noeuds_du_graphe(
        P, sol, A, ZA, TA)
    TN = STRtree([Point(x, y) for x, y in zip(NX, NY)])

    jalon("C3/3 PLATEAUX DE CARREFOUR : rayon rendu par le module de reseau "
          "(demi-largeur maximale incidente + marge) ; median %.2f m, max "
          "%.2f m ; un plateau ne mange jamais plus de %.0f %% du troncon"
          % (float(np.median(RN[RN > 0])) if (RN > 0).any() else 0.0,
             float(RN.max()), 100 * PART_PLATEAU_MAX))

    # --- ③ les profils de troncon -------------------------------------------
    t1 = time.time()
    lois = {}
    profils = {}
    for p in P:
        if p["proprietaire"] != "voirie" or p["id"].startswith("bnd/") \
                or p["id"].startswith("tro/"):
            continue
        a = (p.get("meta") or {}).get("axe")
        if not a or len(a) < 2:
            continue
        i = p["meta"]["i"]
        if i not in profils:
            reff = {j: REFF.get((i, j), 0.0) for _s, j in SUR.get(i, [])}
            profils[i] = (profil_ancre(a, sol, TN, NX, NY, ZN, RN, reff), a)
    chrono("C3/profils", time.time() - t1, "%d troncons" % len(profils))
    jalon("C3/③ PROFILS ANCRES : %d troncons ; le releve est rebase sur les "
          "cotes de ses noeuds puis aplati sur les plateaux, extremites FIXEES"
          % len(profils))

    # --- ④ solidarite entre troncons voisins --------------------------------
    t2 = time.time()
    VOI = [p for p in P if p["proprietaire"] == "voirie"
           and (p.get("meta") or {}).get("axe")]
    GV = [p["geom"] for p in VOI]
    TV = STRtree(GV)
    Aq, Bq = TV.query(GV, predicate="intersects")
    m = Aq < Bq
    Aq, Bq = Aq[m], Bq[m]
    # rang du porteur : le plus LARGE, puis le plus LONG, puis l'index
    rang = {}
    for k, p in enumerate(VOI):
        me = p["meta"]
        rang[k] = (-float(me.get("w_m") or 6.0),
                   -float(profils.get(me["i"], ({"L_m": 0.0}, None))[0]["L_m"]),
                   me["i"])
    parent = {k: k for k in range(len(VOI))}

    def racine(k):
        while parent[k] != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k

    n_sol = 0
    for i, j in zip(Aq, Bq):
        i, j = int(i), int(j)
        ii, jj = VOI[i]["meta"]["i"], VOI[j]["meta"]["i"]
        if ii == jj:
            continue
        try:
            lg = GV[i].boundary.intersection(GV[j].boundary).length
        except Exception:
            continue
        wmax = max(float(VOI[i]["meta"].get("w_m") or 6.0),
                   float(VOI[j]["meta"].get("w_m") or 6.0))
        if lg < max(L_SOLIDAIRE_M, K_SOLIDAIRE * wmax):
            continue
        ri, rj = racine(i), racine(j)
        if ri == rj:
            continue
        if rang[ri] <= rang[rj]:
            parent[rj] = ri
        else:
            parent[ri] = rj
        n_sol += 1
    porteur = {}
    for k, p in enumerate(VOI):
        r = racine(k)
        if r != k:
            porteur[p["id"]] = VOI[r]
    chrono("C3/solidarite", time.time() - t2, "%d fusions" % n_sol)
    jalon("C3/④ SOLIDARITE VOIRIE : %d fusions (contact >= %.1f m ET >= %.0f "
          "largeurs de voie — le critere qui distingue deux troncons de MEME "
          "surface d'un simple carrefour) -> %d parcelles ADOPTENT le profil "
          "de leur porteur (le plus large, puis le plus long) ; dZ nul par "
          "construction sur ces interfaces"
          % (n_sol, L_SOLIDAIRE_M, K_SOLIDAIRE, len(porteur)))

    # --- les lois de voirie --------------------------------------------------
    cpt = {"constante": 0, "profil_troncon": 0, "drapage": 0}
    pentes, hors_pente = [], []
    for p in VOI:
        src = porteur.get(p["id"], p)
        pr, ax = profils.get(src["meta"]["i"], (None, None))
        if pr is None:
            pr, ax = profils.get(p["meta"]["i"], (None, None))
            src = p
        if pr is None:
            z = float(np.percentile(echantillons(p["geom"], sol), 50))
            lois[p["id"]] = {"loi": "constante", "z_m": round(z, 3),
                             "source": "p50 (troncon sans axe exploitable)"}
            cpt["constante"] += 1
            continue
        L = {"loi": "profil_troncon", "profil": pr, "axe": ax,
             "source": "profil ANCRE sur les cotes de noeud (pas %.1f m, "
                       "regularise a %.2f m, plateaux de carrefour)"
                       % (PAS_PROFIL_M, TOL_PROFIL_M)}
        if src is not p:
            L["porteur"] = src["id"]
            L["source"] = "profil du porteur solidaire (contact > %.1f m)" \
                          % L_SOLIDAIRE_M
        lois[p["id"]] = L
        cpt["profil_troncon"] += 1
        pentes.append(pr["pente_max_pc"])
        if pr["pente_max_pc"] > PENTE_MAX_VOIRIE_PC:
            hors_pente.append((pr["pente_max_pc"], p["id"], pr["L_m"]))

    # --- LES PARCELLES DE CARREFOUR : la cote de leur noeud ----------------
    t25 = time.time()
    TNN = STRtree([Point(x, y) for x, y in zip(NX, NY)])
    n_carre = 0
    for p in P:
        me = p.get("meta") or {}
        if not me.get("carrefour"):
            continue
        nx, ny = float(me["noeud"][0]), float(me["noeud"][1])
        jn = TNN.nearest(Point(nx, ny))
        z = float(ZN[int(jn)]) if jn is not None else float(
            sol.z(np.array([nx]), np.array([ny]))[0])
        lois[p["id"]] = {"loi": "constante", "z_m": round(z, 3),
                         "carrefour": True, "noeud": me["noeud"],
                         "source": "cote du NOEUD du reseau noue : un carrefour "
                                   "EST un plateau, et il separe desormais les "
                                   "chaussees incidentes"}
        cpt["constante"] += 1
        n_carre += 1
    chrono("C3/carrefours", time.time() - t25, "%d carrefours" % n_carre)
    jalon("C3/3bis CARREFOURS : %d parcelles de carrefour recoivent la cote de "
          "leur noeud ; les chaussees incidentes, aplaties sur le meme plateau, "
          "les rejoignent a dZ nul par construction" % n_carre)

    # --- ⑦/⑧ les autres proprietaires ---------------------------------------
    t3 = time.time()
    for p in P:
        pid, prop, g = p["id"], p["proprietaire"], p["geom"]
        if pid in lois or pid.startswith("bnd/") or pid.startswith("tro/"):
            continue
        m = mat[pid][0]
        if m == "eau":
            z, s = None, None
            if TB is not None:
                best, ba = None, 0.0
                for j in TB.query(g):
                    j = int(j)
                    try:
                        a = biefs[j]["geom"].intersection(g).area
                    except Exception:
                        a = 0.0
                    if a > ba:
                        ba, best = a, j
                if best is not None and biefs[best].get("z_min_ngf_m") is not None:
                    z = float(biefs[best]["z_min_ngf_m"])
                    s = "side-car Eau (z_min_ngf_m, MESURE)"
            if z is None:
                z = float(np.percentile(echantillons(g, sol), 50))
                s = "p50 du MNT (le side-car ne porte pas de cote ici)"
            lois[pid] = {"loi": "constante", "z_m": round(z, 3), "source": s,
                         "bief": True}
            cpt["constante"] += 1
        elif prop == "organique":
            lois[pid] = {"loi": "drapage",
                         "source": "le plan n'ecrit aucun Z : le moteur drape"}
            cpt["drapage"] += 1
        else:
            E = echantillons(g, sol)
            z = float(np.percentile(E, 50))
            lois[pid] = {"loi": "constante", "z_m": round(z, 3),
                         "relief_m": round(float(E.max() - E.min()), 3),
                         "source": "p50 du MNT sous la parcelle (regle du plan "
                                   "dominant, validee au cratere)"}
            cpt["constante"] += 1
    chrono("C3/constantes", time.time() - t3, "")

    # --- ⑤ les BANDES : offset du porteur -----------------------------------
    t4 = time.time()
    hote = [p for p in P if not (p["id"].startswith("bnd/")
                                 or p["id"].startswith("tro/"))]
    TH = {}
    for k in ("ouvrage", "voirie", "batiment", "zone"):
        sub = [p for p in hote if p["proprietaire"] == k]
        TH[k] = (STRtree([p["geom"] for p in sub]), sub) if sub else (None, [])
    n_h = 0
    n_off = 0
    for p in P:
        if not (p["id"].startswith("bnd/") or p["id"].startswith("tro/")):
            continue
        k = p["proprietaire"]
        T, sub = TH.get(k, (None, []))
        ref = None
        if T is not None:
            cand = [int(j) for j in T.query(p["geom"].buffer(1.0))]
            if cand:
                # le PORTEUR est le voisin dur au plus long contact
                best, bl = None, -1.0
                for j in cand:
                    try:
                        lg = sub[j]["geom"].boundary.intersection(
                            p["geom"].boundary).length
                    except Exception:
                        lg = 0.0
                    if lg > bl:
                        bl, best = lg, j
                ref = best if bl > 0 else min(
                    cand, key=lambda j: sub[j]["geom"].distance(p["geom"]))
        if ref is None:
            z = float(np.percentile(echantillons(p["geom"], sol), 50))
            lois[p["id"]] = {"loi": "constante", "z_m": round(z, 3),
                             "source": "p50 (aucune parcelle hote trouvee)"}
            cpt["constante"] += 1
            continue
        base = dict(lois[sub[ref]["id"]])
        base["loi_heritee_de"] = sub[ref]["id"]
        if k == "voirie" and base["loi"] == "profil_troncon":
            base["offset_m"] = BORDURE_NOMINALE_M
            base["source"] = ("OFFSET du profil de la chaussee porteuse "
                              "(+%.2f m, vue nominale de bordure) : le "
                              "trottoir SUIT la pente de sa rue"
                              % BORDURE_NOMINALE_M)
            n_off += 1
        else:
            base["offset_m"] = 0.0
            base["source"] = "loi du voisin dur porteur, de plain-pied"
        lois[p["id"]] = base
        cpt[base["loi"]] = cpt.get(base["loi"], 0) + 1
        n_h += 1
    chrono("C3/bandes", time.time() - t4, "%d bandes" % n_h)
    jalon("C3/⑤ BANDES : %d bandes prennent la loi de leur voisin dur PORTEUR "
          "(au plus long contact), dont %d en OFFSET +%.2f m sur un profil de "
          "chaussee — un trottoir suit la pente de sa rue"
          % (n_h, n_off, BORDURE_NOMINALE_M))

    # --- ⑥ COMMUNAUTES DE NIVELLEMENT ---------------------------------------
    t5 = time.time()
    # Arbitrage coordinateur D : les communautes couvrent AUSSI le vegetal —
    # une pelouse continue est UNE surface. Deux membres ne se regroupent que
    # s'ils sont de MEME matiere (on ne mele pas une pelouse et un parvis).
    memb = [p for p in P
            if mat[p["id"]][0] in ("mineral", "vegetal")
            and lois[p["id"]]["loi"] == "constante"
            and p["proprietaire"] in ("zone",)
            and not lois[p["id"]].get("bief")
            and not (p.get("meta") or {}).get("carrefour")]
    GM = [p["geom"] for p in memb]
    n_comm = n_membres = 0
    COMM = []
    if GM:
        TM = STRtree(GM)
        Am, Bm = TM.query(GM, predicate="intersects")
        mm = Am < Bm
        Am, Bm = Am[mm], Bm[mm]
        par = {k: k for k in range(len(memb))}

        def rac(k):
            while par[k] != k:
                par[k] = par[par[k]]
                k = par[k]
            return k

        for i, j in zip(Am, Bm):
            i, j = int(i), int(j)
            try:
                lg = GM[i].boundary.intersection(GM[j].boundary).length
            except Exception:
                lg = 0.0
            if lg < RESSAUT_MAX_M:
                continue
            if mat[memb[i]["id"]][0] != mat[memb[j]["id"]][0]:
                continue
            ri, rj = rac(i), rac(j)
            if ri != rj:
                par[rj] = ri
        grp = {}
        for k in range(len(memb)):
            grp.setdefault(rac(k), []).append(k)
        for r, ks in sorted(grp.items()):
            if len(ks) < 2:
                continue
            E = np.concatenate([echantillons(GM[k], sol) for k in ks])
            z = round(float(np.percentile(E, 50)), 3)
            rel = round(float(E.max() - E.min()), 3)
            aire = float(sum(GM[k].area for k in ks))
            cen = GM[ks[0]].representative_point()
            for k in ks:
                lois[memb[k]["id"]] = {
                    "loi": "constante", "z_m": z,
                    "communaute": "comm/%d" % r, "membres": len(ks),
                    "relief_m": rel, "matiere": mat[memb[k]["id"]][0],
                    "source": "p50 de la COMMUNAUTE de nivellement (parcelles "
                              "contigues de meme matiere) : un seul plateau"}
            COMM.append({"id": "comm/%d" % r, "membres": len(ks),
                         "matiere": mat[memb[ks[0]]["id"]][0],
                         "relief_m": rel, "aire_m2": round(aire, 1),
                         "z_m": z, "x": round(cen.x, 1), "y": round(cen.y, 1)})
            n_comm += 1
            n_membres += len(ks)
    chrono("C3/communautes", time.time() - t5, "%d communautes" % n_comm)
    jalon("C3/⑥ COMMUNAUTES DE NIVELLEMENT : %d membres candidats (mineraux, "
          "pietons, contigus) -> %d communautes de 2 membres ou plus couvrant "
          "%d parcelles ; chacune recoit UNE cote (le p50 de la communaute)"
          % (len(memb), n_comm, n_membres))

    # --- ⑦ ANCRAGE AUX OUVRAGES ---------------------------------------------
    t6 = time.time()
    OUV = [p for p in P if p["proprietaire"] == "ouvrage"]
    n_ancre = 0
    cote_ouv = {}
    if OUV and len(A):
        # ⚠️ Une ancre n'est valable que pour un ouvrage DE SA NATURE : un
        # tablier de pont (axe 3D) ne cote pas un bloc de berge, et une crete
        # de mur ne cote pas un pont. Defaut mesure au premier essai : en
        # prenant la mediane de TOUTES les ancres sous l'emprise, les blocs de
        # berge montaient a la crete des murs (~142 m) alors qu'ils bordent une
        # eau a ~127 m -> 9 frontieres hors catalogue INVENTEES (dZ 12 a 15 m).
        NATURE_ANCRE = {"pont": "pont", "bloc_berge": "mur_crete"}
        AP = [Point(x, y) for x, y in A]
        TAP = STRtree(AP)
        for p in OUV:
            src_ok = NATURE_ANCRE.get((p.get("meta") or {}).get("nature"))
            if src_ok is None:
                continue
            if mat[p["id"]][0] == "eau":
                continue          # un ouvrage noye garde la cote de son bief
            js = [int(j) for j in TAP.query(p["geom"])
                  if p["geom"].intersects(AP[int(j)]) and SRCA[int(j)] == src_ok]
            if not js:
                continue
            cote_ouv[p["id"]] = float(np.median([ZA[j] for j in js]))
            lois[p["id"]] = {
                "loi": "constante", "z_m": round(cote_ouv[p["id"]], 3),
                "ancre": True, "ancres_n": len(js), "ancre_source": src_ok,
                "source": "cote DECLAREE par le side-car de l'ouvrage "
                          "(mediane des points de cote de sa NATURE sous "
                          "l'emprise)"}
        GO = [byid[i]["geom"] for i in cote_ouv]
        IO = list(cote_ouv)
        if GO:
            TO = STRtree(GO)
            for p in P:
                if p["proprietaire"] == "ouvrage":
                    continue
                L = lois.get(p["id"]) or {}
                if L.get("loi") != "constante" or L.get("bief"):
                    continue
                best, bl = None, 0.0
                for j in TO.query(p["geom"].buffer(0.05)):
                    j = int(j)
                    try:
                        lg = GO[j].boundary.intersection(
                            p["geom"].boundary).length
                    except Exception:
                        lg = 0.0
                    if lg > bl:
                        bl, best = lg, j
                if best is None or bl < L_ANCRE_M:
                    continue
                zc = cote_ouv[IO[best]]
                if abs(zc - float(L["z_m"])) > PLAIN_PIED_MAX_M:
                    continue
                L2 = dict(L)
                L2["z_m"] = round(zc, 3)
                L2["ancre_sur"] = IO[best]
                L2["source"] = ("cote d'ancrage de l'ouvrage voisin (contact "
                                "de plain-pied de %.1f m)" % bl)
                lois[p["id"]] = L2
                n_ancre += 1
    chrono("C3/ancrage", time.time() - t6, "%d parcelles ancrees" % n_ancre)
    jalon("C3/⑦ ANCRAGE AUX OUVRAGES : %d ouvrages recoivent leur cote "
          "DECLAREE ; %d parcelles a constante en contact de plain-pied "
          "(> %.1f m, ecart <= %.2f m) prennent la cote de l'ouvrage voisin"
          % (sum(1 for p in OUV if (lois.get(p["id"]) or {}).get("ancre")),
             n_ancre, L_ANCRE_M, PLAIN_PIED_MAX_M))

    hors_pente.sort(key=lambda t: -t[0])
    jalon("C3/⭐ NIVEAUX v2 : constante %d | profil_troncon %d | drapage %d ; "
          "pente de voirie mediane %.2f %%, p95 %.2f %%, max %.2f %% ; %d "
          "troncons au-dessus du plafond voirie %.0f %%"
          % (cpt["constante"], cpt["profil_troncon"], cpt["drapage"],
             float(np.percentile(pentes, 50)) if pentes else 0.0,
             float(np.percentile(pentes, 95)) if pentes else 0.0,
             max(pentes) if pentes else 0.0, len(hors_pente),
             PENTE_MAX_VOIRIE_PC))

    CP = contre_preuve(sol)
    with open(os.path.join(CACHE, "niveaux.pkl"), "wb") as f:
        pickle.dump({"lois": lois}, f, protocol=4)
    rep = {"couche": "NIVEAUX",
           "version": "v2 — solidarite",
           "formes": ["constante", "profil_troncon", "drapage"],
           "ordre_national": ["ouvrages (ancres declarees)", "noeuds",
                              "troncons ancres", "solidarite voirie",
                              "bandes en offset", "communautes",
                              "ancrage aux ouvrages", "organique drape"],
           "constantes_de_norme": {
               "PAS_PROFIL_M": PAS_PROFIL_M, "TOL_PROFIL_M": TOL_PROFIL_M,
               "PENTE_MAX_VOIRIE_PC": PENTE_MAX_VOIRIE_PC,
               "PENTE_MAX_CHEMINEMENT_PC": PENTE_MAX_CHEMINEMENT_PC,
               "RESSAUT_MAX_M": RESSAUT_MAX_M,
               "BORDURE_NOMINALE_M": BORDURE_NOMINALE_M,
               "SNAP_NOEUD_M": SNAP_NOEUD_M,
               "MARGE_CARREFOUR_M": MARGE_CARREFOUR_M,
               "L_SOLIDAIRE_M": L_SOLIDAIRE_M, "K_SOLIDAIRE": K_SOLIDAIRE,
               "PART_PLATEAU_MAX": PART_PLATEAU_MAX, "L_ANCRE_M": L_ANCRE_M,
               "PLAIN_PIED_MAX_M": PLAIN_PIED_MAX_M,
               "D_ANCRE_NOEUD_M": D_ANCRE_NOEUD_M},
           "noeuds": {"n": int(len(NX)), "ancres_sur_ouvrage": int(n_anc),
                      "rayon_plateau_median_m": round(float(np.median(RN)), 3),
                      "rayon_plateau_max_m": round(float(RN.max()), 3)},
           "solidarite_voirie": {"fusions": n_sol, "parcelles_adoptantes":
                                 len(porteur)},
           "bandes": {"n": n_h, "en_offset_bordure": n_off},
           "communautes": {"membres_candidats": len(memb),
                           "communautes_n": n_comm,
                           "parcelles_couvertes": n_membres,
                           "relief": {
                               "median_m": round(float(np.median(
                                   [c["relief_m"] for c in COMM])), 3)
                               if COMM else 0.0,
                               "p95_m": round(float(np.percentile(
                                   [c["relief_m"] for c in COMM], 95)), 3)
                               if COMM else 0.0,
                               "max_m": round(max(c["relief_m"] for c in COMM),
                                              3) if COMM else 0.0},
                           "pires_reliefs": sorted(
                               COMM, key=lambda c: -c["relief_m"])[:25]},
           "reseau_noue": STRES,
           "carrefours": {"parcelles": n_carre},
           "ancrage_ouvrages": {"ouvrages_cotes": sum(
               1 for p in OUV if (lois.get(p["id"]) or {}).get("ancre")),
               "parcelles_ancrees": n_ancre},
           "parcelles_par_loi": cpt,
           "pente_voirie": {
               "mediane_pc": round(float(np.percentile(pentes, 50)), 3)
               if pentes else 0.0,
               "p95_pc": round(float(np.percentile(pentes, 95)), 3)
               if pentes else 0.0,
               "max_pc": round(max(pentes), 3) if pentes else 0.0,
               "hors_plafond_voirie_n": len(hors_pente),
               "hors_plafond_cheminement_n": sum(
                   1 for v in pentes if v > PENTE_MAX_CHEMINEMENT_PC),
               "pires": [{"id": h[1], "pente_pc": h[0], "L_m": h[2]}
                         for h in hors_pente[:20]]},
           "contre_preuve_z": CP,
           "lecteur": "work/BERGES/b_lib.py::SolRendu (bilineaire "
                      "FTerrainSampler + enveloppe des deux triangulations)"}
    ecrire_json(os.path.join(OUT, "niveaux.json"), rep)
    chrono("C3 TOTAL", time.time() - t0, "")
    jalon("C3 v2 terminee (%.1f s)" % (time.time() - t0))


if __name__ == "__main__":
    main()
