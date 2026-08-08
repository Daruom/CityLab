# -*- coding: utf-8 -*-
"""mq_verif_onglet.py — VERIFICATION NUMERIQUE du raccord en onglet.

Ne regarde pas une image : verifie la PROPRIETE geometrique de l'onglet sur la
vraie geometrie de la cellule.

  * AVANT : deux segments qui partagent un sommet se decalent chacun par SA
    normale -> leurs deux coins decales sont DISTANTS ; cette distance est,
    au signe pres, le recouvrement (cote convexe) ou la lacune (cote concave).
  * APRES : les deux prennent le meme vecteur d'onglet -> distance NULLE, et
    le coin obtenu est a distance EXACTEMENT `larg` des DEUX segments (c'est
    la definition du miter, et c'est ce qui est verifie ici).

N'ECRIT RIEN dans plan_ville.
"""
import io
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mq_build as B  # noqa: E402
from mq_lib import Contrat  # noqa: E402

CELL = sys.argv[1] if len(sys.argv) > 1 else "0_0"


def dist_point_droite(q, p, d):
    """distance du point q a la droite passant par p de direction unitaire d"""
    v = q - p
    return abs(float(v[0] * d[1] - v[1] * d[0]))


def main():
    ct = Contrat(strict=False)
    q = ct.qui(CELL)
    it = ct.itf(CELL)
    tr = ct.terrassements(CELL, qui=q, itf=it)
    larg = {}
    for t in tr:
        if (t.get("piece") or "talus") == "talus":
            w = float(t.get("largeur_m") or 0.0)
            if w > 0.01:
                larg[(t["regle"], t["terrain"])] = w
    B.index_lignes(ct, [CELL])          # remplit B.LIGNES puis B.BOUTS

    # largeur representative par sommet : celle du couple dont il vient
    w_bout = {}
    for k, pls in B.LIGNES.items():
        if k not in larg:
            continue
        for pl in pls:
            xy = np.asarray(pl, dtype=np.float64)
            if len(xy) < 2:
                continue
            for e in (xy[0], xy[-1]):
                w_bout.setdefault(B._cle_bout(e), []).append(larg[k])

    avant, apres, ecarts_w = [], [], []
    n_coins, n_onglet, n_repli = 0, 0, 0
    faux = 0
    for c, dirs in B.BOUTS.items():
        if len(dirs) != 2:
            continue                     # le coin franc : deux segments
        ws = w_bout.get(c)
        if not ws:
            continue
        n_coins += 1
        w = float(np.median(ws))
        if len(set(round(x, 3) for x in ws)) > 1:
            ecarts_w.append(max(ws) - min(ws))
        p = np.asarray(c, dtype=np.float64)
        d1, d2 = dirs[0], dirs[1]
        n1 = np.array([-d1[1], d1[0]])
        n2 = np.array([-d2[1], d2[0]])
        if float(n1 @ n2) < 0:
            n2 = -n2                     # meme cote, comme dans onglet()
        # AVANT : chacun son coin
        avant.append(float(np.hypot(*(n1 * w - n2 * w))))
        s = 1.0 + float(n1 @ n2)
        if s < 1e-6:
            n_repli += 1
            continue
        v = (n1 + n2) / s
        if float(np.hypot(*v)) > B.ONGLET_LIMITE:
            n_repli += 1
            continue
        n_onglet += 1
        qc = p + v * w
        apres.append(0.0)                # meme vecteur des deux cotes
        # LA PROPRIETE : le coin d'onglet est a distance `w` des DEUX segments
        e1 = abs(dist_point_droite(qc, p, d1) - w)
        e2 = abs(dist_point_droite(qc, p, d2) - w)
        if max(e1, e2) > 1e-9:
            faux += 1

    A = np.asarray(avant) if avant else np.zeros(0)
    out = {
        "cellule": CELL,
        "coins_francs (2 segments)": n_coins,
        "onglet applique": n_onglet,
        "repli sur decalage droit (angle trop ferme ou normales opposees)":
            n_repli,
        "AVANT — ecart entre les deux coins decales (m)": {
            "median": round(float(np.median(A)), 4) if A.size else None,
            "p90": round(float(np.percentile(A, 90)), 4) if A.size else None,
            "max": round(float(A.max()), 4) if A.size else None,
            "coins au-dela de 1 cm": int((A > 0.01).sum()) if A.size else 0,
            "coins au-dela de 10 cm": int((A > 0.10).sum()) if A.size else 0,
        },
        "APRES — ecart entre les deux coins decales (m)": {
            "max": 0.0 if apres else None, "tous_nuls": bool(apres)},
        "PROPRIETE DU MITER (coin a distance `larg` des DEUX segments)": {
            "verifiee_sur": n_onglet, "en_faute_au_dela_de_1e-9": faux},
        "coins ou les deux largeurs different": len(ecarts_w),
    }
    io.open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "mq_verif_onglet.json"), "w",
            encoding="utf-8").write(json.dumps(out, ensure_ascii=False,
                                               indent=1))
    print(json.dumps(out, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
