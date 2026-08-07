# -*- coding: utf-8 -*-
"""PLAN DE VILLE / etape E (1/2) — LES JUGES FINAUX + LA TOURNEE DES PIRES CAS.

  * INTEGRATION (les lecons du 07/08 gravees) : 0 objet mineral sur une
    frontiere vegetal|vegetal (verifie en D) ; 0 instance du semis sur du dur
    du plan — verifie SUR LES PIXELS des masques (R >= 128 = herbe), pas sur
    une vectorisation.
  * BIEFS : l'eau ne remonte pas — chaque bief est PLAT (une constante), on
    mesure l'amplitude z_max - z_min que porte la donnee.
  * TOURNEE : les N cas les plus EXTREMES de CHAQUE regle, en signets du
    visualiseur. On juge une regle sur SES PIRES CAS.
  * COMPTEURS de revue par regle (« appliquee N fois, revue sur M cas »).

Le semis part du BRUT (`FINITION_SOL/entrees_3x3/veg_3x3.json`, 1 222 412
positions candidates = de la DONNEE) et c'est LE PLAN qui decide qui vit.
"""
import io
import json
import os
import pickle
import re
import sys
import time

import numpy as np
import shapely
from shapely.strtree import STRtree

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (CACHE, CELL_M, ENT, OUT, chrono, ecrire_json, jalon)
from c2_matiere import MASQUE_PX, PX_M, SEUIL_R, charge_masques

N_TOURNEE = 12          # cas extremes retenus par regle


def lit_semis():
    """Le semis BRUT, par le lecteur UNIQUE du socle (c0_socle.lit_semis_brut).
    La version precedente lisait par tranches de 16 Mo avec un report de
    200 octets et comptait DEUX FOIS les instances a cheval sur deux tranches
    (1 222 434 au lieu de 1 222 412) — defaut trouve en confrontant les juges
    a l'export du contrat machine."""
    from c0_socle import lit_semis_brut
    t0 = time.time()
    rec, X, Y = lit_semis_brut()
    K = [r[5].decode("ascii") for r in rec]
    chrono("C5/semis lecture", time.time() - t0, "%d instances" % X.size)
    return X, Y, K


def main():
    t0 = time.time()
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
    with open(os.path.join(CACHE, "niveaux.pkl"), "rb") as f:
        lois = pickle.load(f)["lois"]
    with open(os.path.join(CACHE, "interfaces.pkl"), "rb") as f:
        fronts = pickle.load(f)["fronts"]

    # ================================================= ① LE SEMIS, SUR PIXELS =
    X, Y, K = lit_semis()
    M = charge_masques()
    t1 = time.time()
    # (a) le pixel de masque sous chaque instance
    veg_px = np.zeros(X.size, dtype=bool)
    couvert = np.zeros(X.size, dtype=bool)
    cx = np.floor(X / CELL_M).astype(np.int64)
    cy = np.floor(Y / CELL_M).astype(np.int64)
    for (kx, ky), R in M.items():
        m = (cx == kx) & (cy == ky)
        if not m.any():
            continue
        ii = np.clip(((X[m] - kx * CELL_M) / PX_M).astype(np.int64), 0,
                     MASQUE_PX - 1)
        jj = np.clip(((Y[m] - ky * CELL_M) / PX_M).astype(np.int64), 0,
                     MASQUE_PX - 1)
        veg_px[m] = R[jj, ii] >= SEUIL_R
        couvert[m] = True
    # (b) le DUR du plan : toute parcelle de matiere minerale
    dur = shapely.union_all([p["geom"] for p in P
                             if mat[p["id"]][0] == "mineral"])
    shapely.prepare(dur)
    sur_dur = shapely.contains_xy(dur, X, Y)
    # (c) LA REGLE DU PLAN : une instance vit si le pixel dit herbe ET si elle
    #     n'est pas sur du dur du plan.
    vit = veg_px & (~sur_dur)
    rej_px = int((~veg_px).sum())
    rej_dur = int((veg_px & sur_dur).sum())
    reste_sur_dur = int((vit & sur_dur).sum())
    chrono("C5/semis", time.time() - t1, "%d instances" % X.size)
    jalon("C5/⭐ SEMIS (BRUT -> PLAN) : %d instances candidates ; %d hors "
          "emprise des masques ; REJETEES : %d parce que le pixel de masque "
          "n'est pas de l'herbe (R < %d), %d parce qu'elles tombent sur du DUR "
          "du plan ; RETENUES %d (%.2f %%). INVARIANT D'INTEGRATION : %d "
          "instance(s) retenue(s) sur du dur (cible 0)"
          % (X.size, int((~couvert).sum()), rej_px, SEUIL_R, rej_dur,
             int(vit.sum()), 100.0 * vit.mean(), reste_sur_dur))

    # ==================================================== ② LES BIEFS =========
    t2 = time.time()
    amp = []
    for b in biefs:
        zi, za = b.get("z_min_ngf_m"), b.get("z_max_ngf_m")
        if zi is None or za is None:
            continue
        amp.append((float(za) - float(zi), b.get("nature"), b.get("cleabs")))
    amp.sort(key=lambda t: -t[0])
    # coherence : un bief est PLAT ; le plan lui donne une CONSTANTE (z_min)
    n_eau = sum(1 for p in P if mat[p["id"]][0] == "eau")
    z_eau = [lois[p["id"]]["z_m"] for p in P
             if mat[p["id"]][0] == "eau" and lois[p["id"]]["loi"] == "constante"]
    jalon("C5/⭐ BIEFS : %d parcelles d'eau, toutes a loi CONSTANTE (l'eau ne "
          "remonte pas dans une parcelle) ; cotes de %.2f a %.2f m NGF. "
          "Amplitude z_max - z_min portee par la donnee des %d biefs : "
          "mediane %.2f m, max %.2f m (%s)"
          % (n_eau, min(z_eau) if z_eau else 0.0, max(z_eau) if z_eau else 0.0,
             len(amp), float(np.median([a[0] for a in amp])) if amp else 0.0,
             amp[0][0] if amp else 0.0, amp[0][1] if amp else "-"))
    chrono("C5/biefs", time.time() - t2, "")

    # ==================================================== ③ LA TOURNEE ========
    t3 = time.time()
    signets = []

    def sig(regle, titre, x, y, detail):
        signets.append({"regle": regle, "titre": titre,
                        "x": round(float(x), 2), "y": round(float(y), 2),
                        "detail": detail})

    # -- la plus grande bande annexee, la plus fine
    bnd = [p for p in P if p["id"].startswith("bnd/")]
    bnd.sort(key=lambda p: -p["geom"].area)
    for p in bnd[:N_TOURNEE // 2]:
        c = p["geom"].representative_point()
        sig("annexion E0-bis", "plus grande bande annexee (%s)"
            % p["proprietaire"], c.x, c.y,
            "%.1f m2, largeur 4A/P %.3f m" % (p["geom"].area,
                                              p["meta"]["largeur_m"]))
    bnd.sort(key=lambda p: p["meta"].get("largeur_m", 0.0))
    for p in bnd[:N_TOURNEE // 2]:
        c = p["geom"].representative_point()
        sig("annexion E0-bis", "bande la plus fine (%s)" % p["proprietaire"],
            c.x, c.y, "%.3f m2, largeur %.4f m" % (p["geom"].area,
                                                   p["meta"]["largeur_m"]))
    # -- la parcelle a constante au plus fort relief (le pire cas du p50)
    cst = [(lois[p["id"]].get("relief_m") or 0.0, p) for p in P
           if lois[p["id"]]["loi"] == "constante"
           and lois[p["id"]].get("relief_m") is not None]
    cst.sort(key=lambda t: -t[0])
    for r, p in cst[:N_TOURNEE]:
        c = p["geom"].representative_point()
        sig("constante p50", "zone aplanie au plus fort relief", c.x, c.y,
            "%s, %.0f m2, relief du MNT sous la parcelle %.2f m, cote retenue "
            "%.2f m" % (p["proprietaire"], p["geom"].area, r,
                        lois[p["id"]]["z_m"]))
    # -- le profil le plus pentu
    prof = [(lois[p["id"]]["profil"]["pente_max_pc"], p) for p in P
            if lois[p["id"]]["loi"] == "profil_troncon"
            and lois[p["id"]].get("profil")]
    prof.sort(key=lambda t: -t[0])
    for v, p in prof[:N_TOURNEE]:
        c = p["geom"].representative_point()
        sig("profil par troncon", "troncon le plus pentu", c.x, c.y,
            "pente max %.2f %% sur %.1f m (plafond voirie 12 %%)"
            % (v, lois[p["id"]]["profil"]["L_m"]))
    # -- la plus longue interface de CHAQUE type
    par_type = {}
    for f in fronts:
        par_type.setdefault(f["type"], []).append(f)
    for t, L in sorted(par_type.items(), key=lambda kv: str(kv[0])):
        L.sort(key=lambda f: -f["longueur_m"])
        for f in L[:3]:
            sig("catalogue: %s" % t, "plus longue interface `%s`" % t,
                f["x"], f["y"], "%.1f m, dZ %.3f m, %s|%s"
                % (f["longueur_m"], f["dz_m"], f["mat"][0], f["mat"][1]))
        L2 = sorted(L, key=lambda f: -f["dz_m"])
        for f in L2[:2]:
            sig("catalogue: %s" % t, "plus fort dZ pour `%s`" % t, f["x"],
                f["y"], "dZ %.3f m sur %.1f m" % (f["dz_m"], f["longueur_m"]))
    # -- les pires conflits donnee/visible
    mrep = json.load(io.open(os.path.join(OUT, "matiere.json"),
                             encoding="utf-8"))
    byid = {p["id"]: p for p in P}
    for k, lib in (("dur_peint_vegetal", "du dur peint en herbe"),
                   ("zone_verte_minerale", "une zone OCS GE sans herbe "
                                           "visible")):
        for c in mrep["conflits_donnee_visible"][k]["pires"][:N_TOURNEE // 2]:
            p = byid.get(c["id"])
            if p is None:
                continue
            r = p["geom"].representative_point()
            sig("conflit donnee/visible", lib, r.x, r.y,
                "%s : %.0f m2, %.1f %% de pixels herbe" % (c["id"], c["m2"],
                                                           c["vegetal_pc"]))
    # -- les arbitrages hors catalogue
    irep = json.load(io.open(os.path.join(OUT, "interfaces.json"),
                             encoding="utf-8"))
    for a in irep["arbitrage"]["cas"][:N_TOURNEE]:
        sig("HORS CATALOGUE", "arbitrage demande", a["x"], a["y"],
            "dZ %.2f m sur %.1f m, %s|%s" % (a["dz_m"], a["longueur_m"],
                                             a["mat"][0], a["mat"][1]))
    chrono("C5/tournee", time.time() - t3, "%d signets" % len(signets))

    # ==================================================== ④ LES COMPTEURS =====
    regles = {}

    def cp(nom, applique, revue):
        regles[nom] = {"appliquee_n": applique, "revue_n": revue,
                       "couverture_revue_pc": round(
                           100.0 * revue / max(applique, 1), 4)}

    nb = {}
    for s in signets:
        nb[s["regle"]] = nb.get(s["regle"], 0) + 1
    cp("annexion E0-bis", len(bnd), nb.get("annexion E0-bis", 0))
    cp("constante p50", sum(1 for p in P
                            if lois[p["id"]]["loi"] == "constante"),
       nb.get("constante p50", 0))
    cp("profil par troncon", len(prof), nb.get("profil par troncon", 0))
    cp("drapage", sum(1 for p in P if lois[p["id"]]["loi"] == "drapage"), 0)
    for t, L in par_type.items():
        cp("catalogue: %s" % t, len(L), nb.get("catalogue: %s" % t, 0))
    cp("conflit donnee/visible",
       mrep["conflits_donnee_visible"]["dur_peint_vegetal"]["parcelles"]
       + mrep["conflits_donnee_visible"]["zone_verte_minerale"]["parcelles"],
       nb.get("conflit donnee/visible", 0))
    cp("HORS CATALOGUE", irep["arbitrage"]["n"], nb.get("HORS CATALOGUE", 0))
    massives = [k for k, v in regles.items()
                if v["appliquee_n"] > 1000 and v["couverture_revue_pc"] < 0.5]
    jalon("C5/⭐ COMPTEURS DE REVUE : %s ; regles massives peu revues "
          "(> 1000 applications, < 0,5 %% revue) : %s"
          % (" | ".join("%s %d/%d" % (k, v["revue_n"], v["appliquee_n"])
                        for k, v in sorted(regles.items())),
             ", ".join(massives) or "aucune"))

    rep = {"couche": "JUGES",
           "semis": {"brut_n": int(X.size),
                     "hors_masque_n": int((~couvert).sum()),
                     "rejet_pixel_non_herbe_n": rej_px,
                     "rejet_sur_dur_du_plan_n": rej_dur,
                     "retenues_n": int(vit.sum()),
                     "retenues_pc": round(100.0 * float(vit.mean()), 3),
                     "invariant_retenues_sur_dur_n": reste_sur_dur,
                     "regle": "une instance vit si son PIXEL de masque est "
                              "herbe (R >= %d) ET si elle n'est pas sur une "
                              "parcelle minerale du plan" % SEUIL_R,
                     "verification": "sur les PIXELS des masques, pas sur une "
                                     "vectorisation"},
           "biefs": {"parcelles_eau_n": n_eau,
                     "z_min_m": round(min(z_eau), 3) if z_eau else None,
                     "z_max_m": round(max(z_eau), 3) if z_eau else None,
                     "amplitude_mediane_m": round(
                         float(np.median([a[0] for a in amp])), 3) if amp else 0,
                     "amplitude_max_m": round(amp[0][0], 3) if amp else 0,
                     "pires": [{"amplitude_m": round(a[0], 3), "nature": a[1]}
                               for a in amp[:10]]},
           "compteurs_par_regle": regles,
           "regles_massives_peu_revues": massives,
           "tournee": signets}
    ecrire_json(os.path.join(OUT, "juges.json"), rep)
    with open(os.path.join(CACHE, "semis.pkl"), "wb") as f:
        pickle.dump({"x": X[vit], "y": Y[vit], "n_brut": int(X.size)}, f,
                    protocol=4)
    chrono("C5 TOTAL", time.time() - t0, "")
    jalon("C5 terminee (%.1f s) — plan_ville/v1/juges.json" % (time.time() - t0))


if __name__ == "__main__":
    main()
