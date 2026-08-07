# -*- coding: utf-8 -*-
"""PLAN DE VILLE / etape C — LA COUCHE ② NIVEAUX : la loi de Z par parcelle.

TROIS FORMES SEULEMENT (anti-nappe, doctrine Playbook 13.1 : aucune nappe
lissee ne raccorde deux choses) :

  * `constante`      — une seule cote pour toute la parcelle (place, pelouse,
                       emprise de batiment, bief d'eau). Regle du plan dominant
                       p50, validee au cratere. Une constante n'est PAS une
                       nappe.
  * `profil_troncon` — la voirie : le MNT echantillonne SUR L'AXE du graphe,
                       puis REGULARISE (Douglas-Peucker sur le profil en long)
                       en segments a PENTE CONSTANTE. Les plafonds de norme
                       (CEREMA / arrete accessibilite) sont VERIFIES et les
                       depassements listes — jamais ecretes en silence.
  * `drapage`        — l'organique : LE PLAN N'ECRIT AUCUN Z. C'est le moteur
                       qui drapera, comme aujourd'hui : zero divergence par
                       construction.

Les BANDES annexees heritent de la loi de la parcelle qui les a annexees
(regle nationale : une bande n'a pas de niveau propre, elle suit son voisin) —
la reference est ecrite (`loi_heritee_de`), jamais implicite.

⭐ CONTRE-PREUVE obligatoire : sur les points des frontieres zone|organique,
l'echantillonnage Python (b_lib.SolRendu) est confronte a `profil_z_v1.json`
(le releve du monde RENDU, 276 191 pts). L'accord prouve la parite de lecture.
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
from shapely.geometry import LineString, Polygon
from shapely.strtree import STRtree

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (CACHE, CELL_M, OUT, SRC, alt_capitole_m, charge_json,
                      chrono, ecrire_json, jalon, sol_rendu, ecrire_png)
from c1_qui import valide

# --- constantes de NORME, nommees et commentees (aucune ne vient d'un lot) ---
PAS_PROFIL_M = 5.0        # pas d'echantillonnage du profil en long
TOL_PROFIL_M = 0.15       # tolerance de regularisation (Douglas-Peucker en Z)
PENTE_MAX_VOIRIE_PC = 12.0    # pente maximale admise en voirie urbaine (CEREMA,
#                               « Voirie urbaine — guide de conception »)
PENTE_MAX_CHEMINEMENT_PC = 5.0  # cheminement accessible (arrete du 15/01/2007)
RESSAUT_MAX_M = 0.02      # ressaut tolere sans chanfrein (meme arrete)
PAS_GRILLE_M = 2.0        # pas d'echantillonnage d'une parcelle a constante


def echantillons(g, sol, pas=PAS_GRILLE_M):
    """Points du MNT sous une parcelle : grille reguliere clippee, et au moins
    le point representatif (une parcelle de 1 m2 doit avoir une cote)."""
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
    """Douglas-Peucker sur le profil en long (s, z) : rend les indices des
    points de rupture. Chaque segment porte alors une PENTE CONSTANTE."""
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
        pred = z0 + (Z[a:b + 1] * 0.0) + (S[a:b + 1] - s0) * (z1 - z0) / ds
        d = np.abs(Z[a:b + 1] - pred)
        k = int(np.argmax(d))
        if d[k] > tol:
            garde[a + k] = True
            pile.append((a, a + k))
            pile.append((a + k, b))
    return [i for i in range(n) if garde[i]]


def profil_troncon(axe, sol):
    """Le profil en long REGULARISE d'un troncon : MNT sur l'axe, pas de 5 m,
    puis segments a pente constante."""
    P = np.asarray(axe, dtype=float)
    d = np.hypot(np.diff(P[:, 0]), np.diff(P[:, 1]))
    S = np.concatenate(([0.0], np.cumsum(d)))
    L = float(S[-1])
    if L < 1e-6:
        z = float(sol.z(np.array([P[0, 0]]), np.array([P[0, 1]]))[0])
        return {"L_m": 0.0, "pts": [[0.0, round(z, 3)]], "pente_max_pc": 0.0}
    n = max(2, int(math.ceil(L / PAS_PROFIL_M)) + 1)
    ss = np.linspace(0.0, L, n)
    xs = np.interp(ss, S, P[:, 0])
    ys = np.interp(ss, S, P[:, 1])
    zs = np.asarray(sol.z(xs, ys), dtype=float)
    idx = dp_profil(ss, zs, TOL_PROFIL_M)
    pts = [[round(float(ss[i]), 3), round(float(zs[i]), 3)] for i in idx]
    pm = 0.0
    for k in range(len(pts) - 1):
        ds = pts[k + 1][0] - pts[k][0]
        if ds > 1e-6:
            pm = max(pm, abs(pts[k + 1][1] - pts[k][1]) / ds * 100.0)
    return {"L_m": round(L, 3), "pts": pts, "pente_max_pc": round(pm, 3)}


# ============================================================ CONTRE-PREUVE ===
def contre_preuve(sol):
    """Echantillonnage Python vs `profil_z_v1.json` sur les points des
    frontieres zone|organique. `profil_z_v1` est en mm, repere monde Unreal
    (Capitole = 0) ; on le ramene en m NGF."""
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
            # `profil_z_v1` subdivise CHAQUE segment au pas de 1 m (les sommets
            # sont conserves) : on reconstruit la meme suite de points.
            pts = []
            for a in range(len(co) - 1):
                (x0, y0), (x1, y1) = co[a], co[a + 1]
                ln = math.hypot(x1 - x0, y1 - y0)
                k = max(1, int(math.ceil(ln / 1.0)))
                for t in range(k):
                    u = t / float(k)
                    pts.append((x0 + (x1 - x0) * u, y0 + (y1 - y0) * u))
            pts.append((co[-1][0], co[-1][1]))
            if len(pts) != len(z):
                continue
            n_lignes += 1
            for (x, y), zv in zip(pts, z):
                XS.append(float(x))
                YS.append(float(y))
                ZR.append(float(zv) / 1000.0 + Z0)
    XS = np.asarray(XS)
    YS = np.asarray(YS)
    ZR = np.asarray(ZR)
    ZP = np.asarray(sol.z(XS, YS), dtype=float)
    d = np.abs(ZP - ZR)
    bon = d < 50.0            # ecarte les sentinelles eventuelles
    q = np.percentile(d[bon], [50, 95, 99]) if bon.any() else [0, 0, 0]
    rep = {"points": int(XS.size), "runs": n_lignes,
           "hors_bornes_n": int((~bon).sum()),
           "ecart_median_m": round(float(q[0]), 6),
           "ecart_p95_m": round(float(q[1]), 6),
           "ecart_p99_m": round(float(q[2]), 6),
           "ecart_max_m": round(float(d[bon].max()) if bon.any() else 0.0, 6),
           "ecart_moyen_m": round(float(d[bon].mean()) if bon.any() else 0.0, 6),
           "sous_1cm_pc": round(100.0 * float((d[bon] < 0.01).mean()), 3),
           "sous_5cm_pc": round(100.0 * float((d[bon] < 0.05).mean()), 3)}
    # CARACTERISATION du desaccord : `profil_z_v1` est le releve du monde
    # RENDU (avec ses ouvrages), la lecture Python est celle du MNT nu. On
    # mesure donc OU tombent les points en desaccord, au lieu de supposer.
    ko = bon & (d >= 0.05)
    if ko.any():
        try:
            with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as fp:
                DD = pickle.load(fp)
            U = {}
            for k in ("ouvrage", "batiment", "voirie", "zone", "organique"):
                gs = [shapely.from_wkb(p["geom"]) for p in DD["parcelles"]
                      if p["proprietaire"] == k]
                u = shapely.union_all(gs) if gs else None
                if u is not None:
                    shapely.prepare(u)
                U[k] = u
            part = {}
            for k, u in U.items():
                if u is None:
                    continue
                part[k] = round(100.0 * float(shapely.contains_xy(
                    u, XS[ko], YS[ko]).mean()), 2)
            rep_ko = part
        except Exception as e:
            rep_ko = {"erreur": str(e)}
    else:
        rep_ko = {}
    rep["desaccord_5cm_repartition_pc"] = rep_ko
    chrono("C3/contre-preuve", time.time() - t0, "%d points" % XS.size)
    jalon("C3/⭐ CONTRE-PREUVE Z (Python b_lib.SolRendu vs profil_z_v1, %d "
          "points sur %d runs de frontiere zone|organique) : ecart median "
          "%.6f m, moyen %.6f m, p95 %.6f m, p99 %.6f m, max %.6f m ; "
          "%.3f %% sous 1 cm, %.3f %% sous 5 cm ; %d points hors bornes"
          % (rep["points"], n_lignes, rep["ecart_median_m"],
             rep["ecart_moyen_m"], rep["ecart_p95_m"], rep["ecart_p99_m"],
             rep["ecart_max_m"], rep["sous_1cm_pc"], rep["sous_5cm_pc"],
             rep["hors_bornes_n"]))
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
    biefs = [{"geom": shapely.from_wkb(b["geom"]), **{k: b[k] for k in b
                                                      if k != "geom"}}
             for b in MT["biefs"]]
    TB = STRtree([b["geom"] for b in biefs]) if biefs else None

    t1 = time.time()
    lois = {}
    cpt = {"constante": 0, "profil_troncon": 0, "drapage": 0}
    pentes = []
    hors_pente = []
    for i, p in enumerate(P):
        pid, prop, g = p["id"], p["proprietaire"], p["geom"]
        m = mat[pid][0]
        if pid.startswith("bnd/") or pid.startswith("tro/"):
            lois[pid] = {"loi": "_bande", "proprietaire": prop}
            continue
        if m == "eau":
            z = None
            src = None
            if TB is not None:
                cand = [int(j) for j in TB.query(g)]
                best, ba = None, 0.0
                for j in cand:
                    try:
                        a = biefs[j]["geom"].intersection(g).area
                    except Exception:
                        a = 0.0
                    if a > ba:
                        ba, best = a, j
                if best is not None and biefs[best].get("z_min_ngf_m") is not None:
                    z = float(biefs[best]["z_min_ngf_m"])
                    src = "side-car Eau (z_min_ngf_m, MESURE)"
            if z is None:
                z = float(np.percentile(echantillons(g, sol), 50))
                src = "p50 du MNT (le side-car ne porte pas de cote ici)"
            lois[pid] = {"loi": "constante", "z_m": round(z, 3),
                         "source": src, "bief": True}
            cpt["constante"] += 1
        elif prop == "organique":
            lois[pid] = {"loi": "drapage",
                         "source": "le plan n'ecrit aucun Z : le moteur drape"}
            cpt["drapage"] += 1
        elif prop == "voirie":
            axe = p["meta"].get("axe")
            if not axe or len(axe) < 2:
                z = float(np.percentile(echantillons(g, sol), 50))
                lois[pid] = {"loi": "constante", "z_m": round(z, 3),
                             "source": "p50 (troncon sans axe exploitable)"}
                cpt["constante"] += 1
            else:
                pr = profil_troncon(axe, sol)
                lois[pid] = {"loi": "profil_troncon", "profil": pr,
                             "source": "MNT sur l'axe du graphe, pas %.1f m, "
                                       "regularise a %.2f m"
                                       % (PAS_PROFIL_M, TOL_PROFIL_M)}
                cpt["profil_troncon"] += 1
                pentes.append(pr["pente_max_pc"])
                if pr["pente_max_pc"] > PENTE_MAX_VOIRIE_PC:
                    hors_pente.append((pr["pente_max_pc"], pid, pr["L_m"]))
        else:
            E = echantillons(g, sol)
            z = float(np.percentile(E, 50))
            lois[pid] = {"loi": "constante", "z_m": round(z, 3),
                         "relief_m": round(float(E.max() - E.min()), 3),
                         "source": "p50 du MNT sous la parcelle (regle du plan "
                                   "dominant, validee au cratere)"}
            cpt["constante"] += 1
        if (i + 1) % 10000 == 0:
            jalon("C3/  niveaux : %d / %d parcelles (%.0f s)"
                  % (i + 1, len(P), time.time() - t1))

    # ---- les BANDES heritent de la loi de leur annexant --------------------
    hote = [p for p in P if not (p["id"].startswith("bnd/")
                                 or p["id"].startswith("tro/"))]
    TH = {}
    for k in ("ouvrage", "voirie", "batiment", "zone"):
        sub = [p for p in hote if p["proprietaire"] == k]
        TH[k] = (STRtree([p["geom"] for p in sub]), sub) if sub else (None, [])
    n_h = 0
    for p in P:
        if not (p["id"].startswith("bnd/") or p["id"].startswith("tro/")):
            continue
        k = p["proprietaire"]
        T, sub = TH.get(k, (None, []))
        ref = None
        if T is not None:
            cand = [int(j) for j in T.query(p["geom"].buffer(1.0))]
            if cand:
                ref = min(cand, key=lambda j: sub[j]["geom"].distance(p["geom"]))
        if ref is None:
            z = float(np.percentile(echantillons(p["geom"], sol), 50))
            lois[p["id"]] = {"loi": "constante", "z_m": round(z, 3),
                             "source": "p50 (aucune parcelle hote trouvee)"}
            cpt["constante"] += 1
            continue
        base = dict(lois[sub[ref]["id"]])
        base["loi_heritee_de"] = sub[ref]["id"]
        base["source"] = "loi de la parcelle qui a annexe la bande"
        lois[p["id"]] = base
        cpt[base["loi"]] = cpt.get(base["loi"], 0) + 1
        n_h += 1
    chrono("C3/niveaux", time.time() - t1, "%d parcelles" % len(P))
    hors_pente.sort(key=lambda t: -t[0])
    jalon("C3/⭐ NIVEAUX : constante %d | profil_troncon %d | drapage %d ; "
          "%d bandes heritent de la loi de leur annexant. Pente de voirie : "
          "mediane %.2f %%, p95 %.2f %%, max %.2f %% ; %d troncons au-dessus du "
          "plafond voirie %.0f %% (CEREMA) et %d au-dessus du plafond "
          "cheminement accessible %.0f %%"
          % (cpt["constante"], cpt["profil_troncon"], cpt["drapage"], n_h,
             float(np.percentile(pentes, 50)) if pentes else 0.0,
             float(np.percentile(pentes, 95)) if pentes else 0.0,
             max(pentes) if pentes else 0.0, len(hors_pente),
             PENTE_MAX_VOIRIE_PC,
             sum(1 for v in pentes if v > PENTE_MAX_CHEMINEMENT_PC),
             PENTE_MAX_CHEMINEMENT_PC))

    CP = contre_preuve(sol)

    with open(os.path.join(CACHE, "niveaux.pkl"), "wb") as f:
        pickle.dump({"lois": lois}, f, protocol=4)
    rep = {"couche": "NIVEAUX",
           "formes": ["constante", "profil_troncon", "drapage"],
           "constantes_de_norme": {
               "PAS_PROFIL_M": PAS_PROFIL_M, "TOL_PROFIL_M": TOL_PROFIL_M,
               "PENTE_MAX_VOIRIE_PC": PENTE_MAX_VOIRIE_PC,
               "PENTE_MAX_CHEMINEMENT_PC": PENTE_MAX_CHEMINEMENT_PC,
               "RESSAUT_MAX_M": RESSAUT_MAX_M},
           "parcelles_par_loi": cpt,
           "bandes_heritees": n_h,
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
    jalon("C3 terminee (%.1f s) — plan_ville/v1/niveaux.json" % (time.time() - t0))


if __name__ == "__main__":
    main()
