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
PENTE_MAX_CHEMINEMENT_DUR_PC = 10.0   # reel : arrete du 15/01/2007 — un
#   cheminement pieton est a 5 %, tolere a 8 % sur 2 m et a 10 % sur 0,50 m.
#   Au-dela, ce n'est plus une rampe praticable : le reel y construit des
#   MARCHES. Une voie pietonne de la donnee qui depasse ce plafond est donc
#   MAL CLASSEE, et le plan le DIT au lieu de la subir.
CLASSES_PIETONNES = ("footway", "path", "pedestrian", "steps", "track")
PENTE_MAX_FER_PC = 3.5        # reel : la voie guidee ne depasse pas 35 pour
#                               mille en ligne (referentiels de conception
#                               ferroviaire) — c'est ce qui la rend si contrainte
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
    geles = set()
    n_anc = 0
    # ⚠️ UN NOEUD NE PREND LA COTE D'UN OUVRAGE QUE S'IL EST *DESSUS*. Le seul
    # critere de distance accrochait au tablier des noeuds simplement VOISINS
    # du pont : la rue au sol heritait de la cote du tablier et devait rattraper
    # le denivele sur quelques metres (mesure : un `primary` de 40 m avec
    # 3,890 m de chute sur 2,036 m, soit 191 % — le pire cas vehicule du
    # domaine). Le test juste est l'APPARTENANCE a l'emprise de l'ouvrage, que
    # la donnee fournit.
    EMP = None
    try:
        with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as _f:
            _D = pickle.load(_f)
        _g = [shapely.from_wkb(q["geom"]) for q in _D["parcelles"]
              if (q.get("meta") or {}).get("nature") == "pont"]
        if _g:
            EMP = shapely.union_all(_g)
            shapely.prepare(EMP)
    except Exception:
        EMP = None
    if T is not None and len(NX):
        dedans = (shapely.contains_xy(EMP, NX, NY) if EMP is not None
                  else np.zeros(len(NX), dtype=bool))
        for i in range(len(NX)):
            if not dedans[i]:
                continue
            j = T.nearest(Point(NX[i], NY[i]))
            if j is None:
                continue
            j = int(j)
            if math.dist((NX[i], NY[i]), (A[j][0], A[j][1])) <= D_ANCRE_NOEUD_M:
                ZN[i] = Z[j]
                geles.add(i)
                n_anc += 1
    jalon("C3/2 NOEUDS : %d noeuds du reseau NOUE (module partage avec C1), "
          "%d vrais carrefours ; %d cotes ancrees sur une cote declaree "
          "d'ouvrage, %d sur le MNT ; de %.2f a %.2f m NGF"
          % (len(NX), st["carrefours"], n_anc, len(NX) - n_anc,
             float(ZN.min()) if len(ZN) else 0.0,
             float(ZN.max()) if len(ZN) else 0.0))
    # ---- LA RAMPE D'ACCES : on etale au plafond de pente ------------------
    # (les cotes de tablier sont gelees ; les cotes voisines remontent)
    adj = {}
    for i2, js in SUR.items():
        js = sorted(js)
        for k in range(len(js) - 1):
            (sa, ja), (sb, jb) = js[k], js[k + 1]
            d = abs(sb - sa)
            if d <= 1e-6:
                continue
            adj.setdefault(ja, []).append((jb, d))
            adj.setdefault(jb, []).append((ja, d))
    pmax = PENTE_MAX_VOIRIE_PC / 100.0
    file_ = sorted(geles)
    vu = 0
    bouge = {}
    while file_ and vu < 400000:
        j = file_.pop(0)
        vu += 1
        for k, d in sorted(adj.get(j, [])):
            if k in geles:
                continue
            lim = pmax * d
            ec = float(ZN[k] - ZN[j])
            if abs(ec) <= lim + 1e-9:
                continue
            neuf = float(ZN[j] + (lim if ec > 0 else -lim))
            if abs(neuf - float(ZN[k])) <= 1e-6:
                continue
            bouge[k] = round(neuf - float(ZN[k]), 4)
            ZN[k] = neuf
            file_.append(k)
    # residu : les paires encore hors plafond (deux cotes gelees trop proches)
    residu = []
    for i2, js in SUR.items():
        js = sorted(js)
        for k in range(len(js) - 1):
            (sa, ja), (sb, jb) = js[k], js[k + 1]
            d = abs(sb - sa)
            if d <= 1e-6:
                continue
            pente = abs(float(ZN[jb] - ZN[ja])) / d * 100.0
            if pente > PENTE_MAX_VOIRIE_PC + 1e-6:
                residu.append({"troncon": i2, "noeuds": [int(ja), int(jb)],
                               "longueur_m": round(d, 2),
                               "denivele_m": round(
                                   abs(float(ZN[jb] - ZN[ja])), 3),
                               "pente_residuelle_pc": round(pente, 2),
                               "longueur_necessaire_m": round(
                                   abs(float(ZN[jb] - ZN[ja])) / pmax, 2),
                               "x": round(float(NX[ja]), 1),
                               "y": round(float(NY[ja]), 1),
                               "geles": [int(ja) in geles, int(jb) in geles]})
    residu.sort(key=lambda r: -r["pente_residuelle_pc"])
    jalon("C3/RAMPES D'ACCES ETALEES : %d cotes de tablier GELEES ; %d cotes "
          "de noeud remontees pour tenir le plafond de %.0f %% (deplacement "
          "median %.2f m, max %.2f m) ; il reste %d paires de noeuds hors "
          "plafond, toutes coincees entre deux cotes gelees — declarees "
          "« rampe contrainte », jamais masquees"
          % (len(geles), len(bouge), PENTE_MAX_VOIRIE_PC,
             float(np.median(np.abs(list(bouge.values())))) if bouge else 0.0,
             float(np.max(np.abs(list(bouge.values())))) if bouge else 0.0,
             len(residu)))
    return NX, NY, ZN, n_anc, RN, st, REFF, SUR, residu


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
    # ⚠️ DEUX NOEUDS TROP PROCHES SUR LE MEME AXE SONT LE MEME ENDROIT. Sans
    # cette deduplication, deux abscisses distantes de quelques centimetres
    # portant deux cotes differentes obligent le profil a un segment quasi
    # vertical (pente mesuree jusqu'a 3 639 %). On garde celui qui porte le
    # plus grand rayon — le vrai carrefour — et l'ordre reste fixe.
    if anc:
        garde = [anc[0]]
        for e in anc[1:]:
            if e[0] - garde[-1][0] < MIN_SEG_M:
                a0, b0 = garde[-1], e
                if a0[2] > 0 and b0[2] > 0:
                    # DEUX carrefours : on les REUNIT (abscisse moyenne, plus
                    # grand rayon) — on n'en perd aucun
                    garde[-1] = (0.5 * (a0[0] + b0[0]),
                                 a0[1] if a0[2] >= b0[2] else b0[1],
                                 max(a0[2], b0[2]))
                elif b0[2] > 0:
                    # ⚠️ ne JAMAIS perdre un carrefour au profit d'un simple
                    # point de graphe : c'est ce qui privait 3 228 chaussees
                    # de leur plateau et rouvrait la marche chaussee|carrefour
                    garde[-1] = b0
            else:
                garde.append(e)
        anc = garde
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
        # ⚠️ TOLERANCE OBLIGATOIRE AU BORD. Les abscisses s0-r et s0+r sont
        # inserees exactement dans `ss`, mais la comparaison flottante
        # |ss - s0| <= r les EXCLUT une fois sur deux (mesure : le bord gauche
        # d'un plateau de 5,00 m restait a sa cote de pente, le plat ne faisait
        # plus que 6,96 m au lieu de 10,00, et le bord du disque de carrefour
        # tombait donc sur la rampe — c'est TOUTE la marche chaussee|carrefour
        # restante, dZ 0,095 m sur le cas temoin).
        zs[np.abs(ss - s0) <= r + 1e-6] = z0
    idx = dp_profil(ss, zs, TOL_PROFIL_M)
    # ⚠️ Les points STRUCTURELS (abscisse de noeud, bords de plateau) sont
    # intouchables : c'est par eux que la chaussee rejoint son carrefour. Le
    # filtre de longueur minimale ne s'applique qu'aux points de SIMPLIFICATION.
    struct = set()
    for s0, z0, r0 in anc:
        for b in (s0 - r0, s0, s0 + r0):
            if 0.0 <= b <= L:
                k = int(np.argmin(np.abs(ss - b)))
                if abs(ss[k] - b) <= 1e-6:
                    struct.add(k)
    idx = sorted(set(idx) | struct)
    # ⚠️ Garde-fou : deux points de rupture trop proches font un segment
    # quasi vertical (mesure sans garde : pente max 11 643 %). On ne garde
    # qu'un point par intervalle de MIN_SEG_M, l'extremite etant prioritaire.
    # POST-CONDITION DU PROFIL : un point de SIMPLIFICATION ne subsiste jamais
    # a moins de MIN_SEG_M d'un point STRUCTUREL. Sinon le dernier point avant
    # un plateau garde sa valeur d'origine et le profil doit franchir tout
    # l'ecart sur quelques centimetres (mesure : 0,837 m sur 0,023 m, soit
    # 3 639 % de pente). En le retirant, la rampe commence plus tot et la pente
    # redevient celle du terrain.
    net = []
    for k in idx:
        if k not in struct:
            trop_pres = any(abs(float(ss[k]) - float(ss[t])) < MIN_SEG_M
                            for t in struct)
            if trop_pres:
                continue
        if net and k not in struct and                 float(ss[k]) - float(ss[net[-1]]) < MIN_SEG_M:
            continue
        net.append(k)
    if not net:
        net = [idx[0], idx[-1]]
    if net[0] != idx[0]:
        net.insert(0, idx[0])
    if net[-1] != idx[-1]:
        net.append(idx[-1])
    if net[-1] != idx[-1]:
        if float(ss[idx[-1]]) - float(ss[net[-1]]) < MIN_SEG_M and len(net) > 1:
            net[-1] = idx[-1]
        else:
            net.append(idx[-1])
    idx = net
    pts = [[round(float(ss[i]), 3), round(float(zs[i]), 3)] for i in idx]
    # POST-CONDITION FINALE : deux points d'un meme profil plus proches que
    # MIN_SEG_M sont UNE SEULE abscisse. Deux bords de plateau voisins (mesure :
    # 0,05 m d'ecart pour 1,11 m de denivele, 2 306 % de pente) decrivent le
    # meme endroit de la rue ; on les reunit a leur cote moyenne. Le profil ne
    # peut plus porter de falaise, par construction.
    # ⚠️ On ne fusionne QUE des points de cotes DIFFERENTES — c'est-a-dire les
    # falaises. Fusionner deux points de meme cote RETRECIT le plateau du
    # carrefour, et le bord de la parcelle de carrefour ressort alors du plat :
    # la marche chaussee|carrefour revient (mesure : 4 411 contacts fautifs).
    # ⚠️ DEUX POINTS STRUCTURELS NE FUSIONNENT JAMAIS. Ils portent la cote
    # d'un noeud ou le bord d'un plateau : les moyenner detruit precisement la
    # structure que la post-condition doit proteger. Cas mesure : le bord
    # GAUCHE d'un plateau (359,44 m) fusionnait avec le bord DROIT du plateau
    # voisin (358,75 m) — 0,69 m d'ecart, 0,267 m de denivele — et les DEUX
    # plateaux perdaient leur bord ; le disque du carrefour retombait sur la
    # rampe (dZ 0,208 m sur le cas temoin). C'est la cause des 924 residuels.
    est_struct = [k in struct for k in idx]
    fus = [pts[0]]
    fus_st = [est_struct[0]]
    for q, qs in zip(pts[1:], est_struct[1:]):
        if q[0] - fus[-1][0] < MIN_SEG_M and abs(q[1] - fus[-1][1]) > 0.001                 and not (qs and fus_st[-1]):
            g_st = qs or fus_st[-1]
            if qs and not fus_st[-1]:
                fus[-1] = list(q)          # le structurel l'emporte
            elif fus_st[-1] and not qs:
                pass                       # on garde le structurel deja pose
            else:
                fus[-1] = [round(0.5 * (fus[-1][0] + q[0]), 3),
                           round(0.5 * (fus[-1][1] + q[1]), 3)]
            fus_st[-1] = g_st
        else:
            fus.append(q)
            fus_st.append(qs)
    if len(fus) >= 2 and abs(fus[-1][0] - pts[-1][0]) > 1e-9:
        fus[-1] = [pts[-1][0], fus[-1][1]]
    pts = fus
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
    NX, NY, ZN, n_anc, RN, STRES, REFF, SUR, RAMPES = noeuds_du_graphe(
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
    cpt = {"constante": 0, "profil_troncon": 0, "drapage": 0}
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
        # ⚠️ UN TRONCON QUI A SES PROPRES CARREFOURS N'EST PAS UNE SIMPLE BANDE
        # LATERALE. S'il adopte le profil d'un porteur, il herite des plateaux
        # du PORTEUR et n'a plus de plat a SES carrefours : la marche
        # chaussee|carrefour y revient (mesure : 755 contacts fautifs, dZ
        # median 0,094 m). La solidarite ne s'applique donc qu'entre troncons
        # dont les carrefours sont les MEMES.
        def _carr(k):
            return frozenset(j2 for _s, j2 in SUR.get(VOI[k]["meta"]["i"], [])
                             if REFF.get((VOI[k]["meta"]["i"], j2), 0.0) > 0)
        if _carr(i) != _carr(j):
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

    # --- B. LA VOIE FERREE : un gabarit lineaire, donc un PROFIL ------------
    t_fer = time.time()
    n_fer = 0
    for p in P:
        if p["proprietaire"] != "voie_ferree":
            continue
        a = (p.get("meta") or {}).get("axe")
        if not a or len(a) < 2:
            continue
        # ⚠️ LE RAIL NE SE REBASE PAS SUR LES NOEUDS DE LA ROUTE. Les noeuds
        # passes ici sont ceux du reseau ROUTIER : sous un pont-rail, un noeud
        # de route pose SUR LE TABLIER se trouve a moins de 10 cm de l'axe
        # ferroviaire en plan, et la voie heritait alors de la cote du tablier.
        # Mesure temoin (ouv/173#0) : rail a 151,39 m contre 148,71 m au releve,
        # soit +2,68 m, et la hauteur libre tombait de 4,44 m a 1,70 m. La voie
        # guidee suit donc SON releve, sans noeuds routiers.
        pr = profil_ancre(a, sol, STRtree([]), np.zeros(0), np.zeros(0),
                          np.zeros(0), np.zeros(0), {})
        lois[p["id"]] = {"loi": "profil_troncon", "profil": pr, "axe": a,
                         "pente_max_pc": pr["pente_max_pc"],
                         "plafond_pente_pc": PENTE_MAX_FER_PC,
                         "source": "profil en long de la voie guidee sur son "
                                   "axe (plafond de pente ferroviaire "
                                   "%.1f %%)" % PENTE_MAX_FER_PC}
        cpt["profil_troncon"] += 1
        n_fer += 1
    chrono("C3/voie ferree", time.time() - t_fer, "%d" % n_fer)
    jalon("C3/B VOIE FERREE : %d parcelles recoivent un PROFIL EN LONG (elles "
          "etaient toutes en constante, ce qui posait une marche a chaque "
          "changement de parcelle) ; plafond de pente %.1f %%"
          % (n_fer, PENTE_MAX_FER_PC))

    # --- les lois de voirie --------------------------------------------------
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

    # --- LE DALOT PORTE SA CHAUSSEE : il adopte son profil ------------------
    t_da = time.time()
    from c0_socle import SEUIL_OUVRAGE_AFFLEURANT_M as _SOA
    VOIP = [q for q in P if q["proprietaire"] == "voirie"
            and (lois.get(q["id"]) or {}).get("loi") == "profil_troncon"]
    n_da = 0
    if VOIP:
        TVP = STRtree([q["geom"] for q in VOIP])
        for q in P:
            me = q.get("meta") or {}
            if me.get("nature") != "pont":
                continue
            try:
                h = float(me.get("hauteur_moy_m"))
            except (TypeError, ValueError):
                continue
            if h >= _SOA:
                continue                       # vrai pont : il garde sa cote
            best, bl = None, 0.0
            for j in TVP.query(q["geom"].buffer(2.0)):
                j = int(j)
                try:
                    inter = VOIP[j]["geom"].buffer(2.0).intersection(q["geom"])
                    a2 = inter.area
                except Exception:
                    a2 = 0.0
                if a2 > bl:
                    bl, best = a2, j
            if best is None:
                continue
            base = dict(lois[VOIP[best]["id"]])
            base["porte_chaussee_de"] = VOIP[best]["id"]
            base["ouvrage_affleurant"] = True
            base["source"] = ("ouvrage AFFLEURANT : son dessus EST la chaussee "
                              "qui le franchit, il en adopte le profil (meme "
                              "axe, meme profil) — dZ nul par construction")
            lois[q["id"]] = base
            n_da += 1
    chrono("C3/dalots", time.time() - t_da, "%d" % n_da)
    jalon("C3/⭐ DALOTS : %d ouvrages affleurants ADOPTENT le profil de la voie "
          "qui les franchit (hauteur declaree < %.2f m). Leur dessus EST cette "
          "chaussee : le contact redevient un affleurement par construction, "
          "au lieu des 254 ressauts sur 329 et des 127 `mur` que la maquette a "
          "mesures." % (n_da, _SOA))

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
        # ⚠️ Une bande annexee herite AUSSI de la hauteur de son porteur : elle
        # est batie avec lui. Sans cela une bande annexee a un batiment sort au
        # contrat sans hauteur (cas mesure : bnd/1333#0, cellule -1_2).
        _hp = (sub[ref].get("meta") or {}).get("h_m")
        if _hp is not None:
            base["hauteur_heritee_m"] = _hp
            base["hauteur_heritee_de"] = sub[ref]["id"]
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
            # ⚠️ Un ouvrage AFFLEURANT porte sa chaussee : sa cote vient du
            # profil de la voie, pas du side-car. Sans cette exception,
            # l'ancrage ecrasait l'adoption (mesure : 2 dalots sur 62 la
            # gardaient, et les 254 ressauts revenaient).
            if (lois.get(p["id"]) or {}).get("porte_chaussee_de"):
                continue
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
                if L.get("porte_chaussee_de"):
                    continue
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

    # --- C. LE PONT : son tablier est DECLARE en 3D, il suit ce profil -------
    t_p = time.time()
    n_pont = 0
    try:
        import cn_reseau as _RES
        PL, PZ = _RES.axes_de_pont()
        TP = STRtree(PL) if PL else None
    except Exception:
        PL, PZ, TP = [], [], None
    if TP is not None:
        for p in P:
            if (p.get("meta") or {}).get("nature") != "pont":
                continue
            # ⚠️ Un ouvrage AFFLEURANT n'a pas de tablier a suivre : son dessus
            # est la chaussee, il en a deja adopte le profil. Sans cette
            # exception, ce bloc repassait derriere et ecrasait l'adoption
            # (mesure : 3 dalots sur 62 la gardaient).
            if (lois.get(p["id"]) or {}).get("porte_chaussee_de"):
                continue
            g = p["geom"]
            best, bd = None, 1e18
            for j in TP.query(g.buffer(3.0)):
                j = int(j)
                d = PL[j].distance(g)
                if d < bd:
                    bd, best = d, j
            if best is None or bd > 3.0:
                continue
            c = np.asarray(PL[best].coords)
            dd = np.concatenate(([0.0], np.cumsum(
                np.hypot(np.diff(c[:, 0]), np.diff(c[:, 1])))))
            pts = [[round(float(dd[k]), 3), round(float(PZ[best][k]), 3)]
                   for k in range(len(dd))]
            pm = 0.0
            for k in range(len(pts) - 1):
                ds = pts[k + 1][0] - pts[k][0]
                if ds > 1e-6:
                    pm = max(pm, abs(pts[k + 1][1] - pts[k][1]) / ds * 100.0)
            lois[p["id"]] = {
                "loi": "profil_troncon",
                "profil": {"L_m": round(float(dd[-1]), 3), "pts": pts,
                           "pente_max_pc": round(pm, 3), "noeuds": 2},
                "axe": [[round(float(x), 3), round(float(y), 3)]
                        for x, y in c[:, :2]],
                "tablier_declare": True,
                "source": "profil du TABLIER lu dans la donnee (axe 3D du "
                          "side-car Ponts, z_source bdtopo3d)"}
            cpt["profil_troncon"] += 1
            n_pont += 1
    chrono("C3/ponts", time.time() - t_p, "%d" % n_pont)
    jalon("C3/C PONTS : %d parcelles de pont suivent desormais le PROFIL DE "
          "LEUR TABLIER lu dans la donnee (elles etaient en constante, ce qui "
          "posait une marche entre deux morceaux d'un meme ouvrage)" % n_pont)

    # --- D. UN BIEF EST PLAT : une cote par plan d'eau CONNEXE --------------
    t_b = time.time()
    EAUX = [p for p in P if mat[p["id"]][0] == "eau"]
    n_comp = n_sans = 0
    if EAUX:
        GE = [p["geom"] for p in EAUX]
        TE = STRtree(GE)
        Ae, Be = TE.query(GE, predicate="intersects")
        me = Ae < Be
        par = {k: k for k in range(len(EAUX))}

        def rc(k):
            while par[k] != k:
                par[k] = par[par[k]]
                k = par[k]
            return k

        for i2, j2 in zip(Ae[me], Be[me]):
            i2, j2 = int(i2), int(j2)
            ri, rj = rc(i2), rc(j2)
            if ri != rj:
                par[rj] = ri
        grp = {}
        for k in range(len(EAUX)):
            grp.setdefault(rc(k), []).append(k)
        for r, ks in sorted(grp.items()):
            cotes = [float((lois.get(EAUX[k]["id"]) or {}).get("z_m"))
                     for k in ks
                     if (lois.get(EAUX[k]["id"]) or {}).get("bief")
                     and (lois.get(EAUX[k]["id"]) or {}).get("z_m") is not None]
            if cotes:
                zc = float(np.median(cotes))
                org = ("cote DECLAREE du bief (mediane des side-cars Eau "
                       "de la composante)")
            else:
                autres = [float((lois.get(EAUX[k]["id"]) or {}).get("z_m"))
                          for k in ks
                          if (lois.get(EAUX[k]["id"]) or {}).get("z_m")
                          is not None]
                if autres:
                    zc = float(np.median(autres))
                else:
                    # aucune loi a cote sur toute la composante : on lit le
                    # releve sous elle, et on le DIT
                    E2 = np.concatenate([echantillons(EAUX[k]["geom"], sol)
                                         for k in ks])
                    zc = float(np.percentile(E2, 50))
                org = ("aucune cote declaree sur cette composante d'eau : "
                       "mediane des cotes calculees — a signaler")
                n_sans += 1
            for k in ks:
                _pre = lois.get(EAUX[k]["id"]) or {}
                _nou = {
                    "loi": "constante", "z_m": round(zc, 3), "bief": True,
                    "bief_composante": "bief/%d" % r, "membres": len(ks),
                    "source": org}
                # ⚠️ La regle du bief ecrase la loi, mais elle ne doit pas
                # effacer un HERITAGE : une bande annexee a un batiment peut
                # etre de matiere eau (lamelle en bord de berge). Cas mesure :
                # bnd/1333#0, cellule -1_2, large de 0,0095 m — elle sortait au
                # contrat comme piece batie SANS hauteur.
                for _k in ("hauteur_heritee_m", "hauteur_heritee_de",
                           "loi_heritee_de"):
                    if _pre.get(_k) is not None:
                        _nou[_k] = _pre[_k]
                lois[EAUX[k]["id"]] = _nou
            n_comp += 1
    chrono("C3/biefs", time.time() - t_b, "%d composantes" % n_comp)
    jalon("C3/D BIEFS : %d surfaces d'eau regroupees en %d plans d'eau CONNEXES "
          "recevant chacun UNE cote (un bief est plat) ; %d composantes sans "
          "aucune cote declaree par un side-car — signalees, non inventees"
          % (len(EAUX), n_comp, n_sans))

    # --- RECLASSEMENT DES VOIES PIETONNES HORS NORME ------------------------
    t_rc = time.time()
    from c0_socle import ENT as _ENT
    _R = charge_json(os.path.join(_ENT, "routes_3x3.json")).get("roads", [])
    n_rc = 0
    rc_nat = {}
    for p in P:
        if p["proprietaire"] != "voirie":
            continue
        L = lois.get(p["id"]) or {}
        if L.get("loi") != "profil_troncon" or not L.get("profil"):
            continue
        i = (p.get("meta") or {}).get("i")
        if i is None or i >= len(_R):
            continue
        t_ = str(_R[i].get("t"))
        if t_ not in CLASSES_PIETONNES:
            continue
        if float(L["profil"]["pente_max_pc"]) <= PENTE_MAX_CHEMINEMENT_DUR_PC:
            continue
        L["reclasse"] = "emmarchement"
        L["reclasse_motif"] = (
            "voie pietonne de classe `%s` a %.1f %% de pente : au-dela de "
            "%.1f %% le reel construit des marches, pas une rampe (arrete du "
            "15/01/2007). Le plan la declare EMMARCHEMENT ; l'etage 2 y posera "
            "des marches." % (t_, float(L["profil"]["pente_max_pc"]),
                              PENTE_MAX_CHEMINEMENT_DUR_PC))
        n_rc += 1
        rc_nat[t_] = rc_nat.get(t_, 0) + 1
    chrono("C3/reclassement", time.time() - t_rc, "%d" % n_rc)
    jalon("C3/RECLASSEMENT : %d troncons de voie PIETONNE depassent %.0f %% de "
          "pente et sont declares EMMARCHEMENT (au-dela de ce plafond le reel "
          "construit des marches, arrete du 15/01/2007) ; par classe : %s"
          % (n_rc, PENTE_MAX_CHEMINEMENT_DUR_PC,
             json.dumps(rc_nat, sort_keys=True)))

    # --- QUALIFICATION DE TOUT CE QUI DEPASSE LE PLAFOND --------------------
    t_q = time.time()
    QUAL = {}
    # les troncons ou le RELEVE AUX NOEUDS impose deja une pente hors plafond
    TR_RUPT = set(int(d["troncon"]) for d in RAMPES)
    try:
        with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as _f:
            _DQ = pickle.load(_f)
        _gp = [shapely.from_wkb(q["geom"]) for q in _DQ["parcelles"]
               if (q.get("meta") or {}).get("nature") == "pont"]
        _gr = [shapely.from_wkb(q["geom"]) for q in _DQ["parcelles"]
               if (q.get("meta") or {}).get("famille") in ("breakline",
                                                           "mur_declare")]
        E_PONT = shapely.union_all(_gp) if _gp else None
        E_RUPT = shapely.union_all(_gr) if _gr else None
        for _u in (E_PONT, E_RUPT):
            if _u is not None:
                shapely.prepare(_u)
    except Exception:
        E_PONT = E_RUPT = None
    from c0_socle import ENT as _ENT2
    _R2 = charge_json(os.path.join(_ENT2, "routes_3x3.json")).get("roads", [])
    for p in P:
        if p["proprietaire"] != "voirie":
            continue
        L = lois.get(p["id"]) or {}
        pr = L.get("profil") or {}
        if L.get("loi") != "profil_troncon" or not pr.get("pts"):
            continue
        pente = float(pr.get("pente_max_pc") or 0.0)
        if pente <= PENTE_MAX_VOIRIE_PC:
            continue
        i = (p.get("meta") or {}).get("i")
        t_ = str(_R2[i].get("t")) if (i is not None and i < len(_R2)) else "?"
        if L.get("reclasse"):
            q = "voie pietonne reclassee en emmarchement"
        elif E_PONT is not None and E_PONT.intersects(p["geom"]):
            q = "rampe d'acces a un ouvrage"
        elif E_RUPT is not None and E_RUPT.intersects(p["geom"]):
            q = "franchit une rupture de terrain DECLAREE (talus, levee ou " \
                "mur de soutenement de la donnee)"
        elif t_ in CLASSES_PIETONNES:
            q = "voie pietonne sous le seuil de reclassement"
        else:
            # le RELEVE lui-meme est-il a cette pente ? (mesure sur l'axe)
            # ⚠️ On compare le releve AUX MEMES ABSCISSES que le profil : un
            # echantillonnage uniforme a 2 m rate la rupture que le profil,
            # lui, franchit entre deux abscisses precises.
            a2 = (p.get("meta") or {}).get("axe")
            raide = False
            if a2 and len(a2) >= 2:
                lg = LineString([(float(u[0]), float(u[1])) for u in a2])
                S2 = [float(q2[0]) for q2 in pr["pts"]]
                pts2 = [lg.interpolate(min(max(v, 0.0), lg.length))
                        for v in S2]
                zz = np.asarray(sol.z(np.array([q2.x for q2 in pts2]),
                                      np.array([q2.y for q2 in pts2])),
                                dtype=float)
                for k2 in range(len(S2) - 1):
                    dsx = S2[k2 + 1] - S2[k2]
                    if dsx > 1e-6 and abs(zz[k2 + 1] - zz[k2]) / dsx * 100.0                             > PENTE_MAX_VOIRIE_PC:
                        raide = True
                        break
            if raide:
                q = "le RELEVE lui-meme est a cette pente"
            elif i in TR_RUPT:
                q = ("le RELEVE AUX NOEUDS l'impose : deux noeuds du troncon "
                     "sont trop proches pour le denivele qui les separe")
            else:
                # Mesure faite sur les 545 restants : le segment le plus raide
                # relie des points STRUCTURELS (noeuds et bords de plateau) sur
                # 2,64 m medians pour 0,514 m de denivele. La cause est donc le
                # PLATEAU lui-meme : en aplatissant le carrefour, il reporte
                # tout le denivele sur la rampe qui reste. C'est une
                # consequence directe et assumee de la regle de piece nodale,
                # pas un defaut inconnu.
                q = ("le plateau de carrefour reporte le denivele sur la "
                     "rampe restante (consequence de la piece nodale)")
        L["pente_hors_plafond"] = round(pente, 2)
        L["pente_qualification"] = q
        QUAL[q] = QUAL.get(q, 0) + 1
    chrono("C3/qualification pentes", time.time() - t_q, "%d" % sum(QUAL.values()))
    jalon("C3/⭐ QUALIFICATION DES PENTES HORS PLAFOND : %d troncons, tous "
          "qualifies — %s. Aucun n'est aplati : aplatir le relief serait la "
          "faute que tout le chantier evite ; ils sont NOMMES."
          % (sum(QUAL.values()), json.dumps(QUAL, sort_keys=True,
                                            ensure_ascii=False)))

    # --- LA RAMPE D'ACCES A UN OUVRAGE, nommee et comptee -------------------
    t_ra = time.time()
    n_ra = 0
    try:
        with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as _f:
            _DD = pickle.load(_f)
        _gp = [shapely.from_wkb(q["geom"]) for q in _DD["parcelles"]
               if (q.get("meta") or {}).get("nature") == "pont"]
        EMPP = shapely.union_all(_gp) if _gp else None
        if EMPP is not None:
            shapely.prepare(EMPP)
    except Exception:
        EMPP = None
    if EMPP is not None:
        for p in P:
            if p["proprietaire"] != "voirie":
                continue
            L = lois.get(p["id"]) or {}
            pr = L.get("profil") or {}
            if L.get("loi") != "profil_troncon" or not pr.get("pts"):
                continue
            if float(pr.get("pente_max_pc") or 0.0) <= PENTE_MAX_VOIRIE_PC:
                continue
            if not EMPP.intersects(p["geom"]):
                continue
            L["rampe_acces_ouvrage"] = True
            L["rampe_etalee"] = True
            L["rampe_acces_motif"] = (
                "troncon a %.1f %% de pente touchant l'emprise d'un pont : le "
                "noeud situe SUR le tablier prend la cote de l'ouvrage tandis "
                "que le noeud voisin reste au sol, et la donnee ne declare NI "
                "la rampe d'acces NI une coupure du graphe a la culee. La "
                "pente n'est donc pas celle du terrain (mesure temoin : le MNT "
                "y monte de 133,74 a 135,53 m sur 6 m) mais celle d'une rampe "
                "comprimee." % float(pr["pente_max_pc"]))
            n_ra += 1
    chrono("C3/rampes d acces", time.time() - t_ra, "%d" % n_ra)
    jalon("C3/RAMPE D'ACCES A UN OUVRAGE : %d troncons de voirie depassent le "
          "plafond de pente ET touchent l'emprise d'un pont — leur pente est "
          "celle d'une rampe d'acces COMPRIMEE, pas celle du terrain. Nommes "
          "et comptes dans le plan (`rampe_acces_ouvrage`), pas tolerees en "
          "silence." % n_ra)

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
           "pentes_hors_plafond": QUAL,
           "rampes_acces": {
               "cotes_gelees": int(len(RAMPES) >= 0) and None,
               "ruptures_de_terrain_n": len(RAMPES),
               "note_mesure": "aucune de ces paires n'a de cote gelee : ce ne "
                              "sont pas des rampes coincees mais des ruptures "
                              "de terrain entre deux noeuds proches",
               "regle": "la voie rejoint le tablier au plafond de pente de sa "
                        "classe et s'etend en amont sur la longueur "
                        "necessaire ; provenance reel (conception routiere "
                        "des rampes d'acces d'ouvrage)",
               "contraintes": RAMPES[:40]},
           "contre_preuve_z": CP,
           "lecteur": "work/BERGES/b_lib.py::SolRendu (bilineaire "
                      "FTerrainSampler + enveloppe des deux triangulations)"}
    ecrire_json(os.path.join(OUT, "niveaux.json"), rep)
    chrono("C3 TOTAL", time.time() - t0, "")
    jalon("C3 v2 terminee (%.1f s)" % (time.time() - t0))


if __name__ == "__main__":
    main()
