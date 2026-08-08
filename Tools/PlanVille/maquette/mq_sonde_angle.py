# -*- coding: utf-8 -*-
"""mq_sonde_angle.py — SONDE DE MESURE (lecture seule) du lot `plinthes`.

Objet : departager, AVANT tout code, d'ou viennent les « patches jaune/marron »
en pied de batiment signales par l'utilisateur, et trouver un ANGLE de batiment
avec des pieces des deux cotes pour la capture avant/apres.

N'ECRIT RIEN dans plan_ville. Sortie : un JSON sur la sortie standard.
"""
import io
import json
import os
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq_lib import Contrat  # noqa: E402

CELL = sys.argv[1] if len(sys.argv) > 1 else "0_0"


def main():
    ct = Contrat(strict=False)
    q = ct.qui(CELL)
    it = ct.itf(CELL)
    fam = {p["id"]: ct.famille(p) for p in q["parcelles"]}

    # ---- 1. LES TERRASSEMENTS DE LA CELLULE : combien de `talus` a largeur ---
    tr = ct.terrassements(CELL, qui=q, itf=it)
    ct_piece = Counter()
    larg = []
    for t in tr:
        p = t.get("piece") or "talus"
        ct_piece[p] += 1
        if p == "talus":
            larg.append(float(t.get("largeur_m") or 0.0))
    larg = np.asarray(larg) if larg else np.zeros(0)

    # ---- 2. LES INTERFACES QUI TOUCHENT UN BATIMENT -------------------------
    res_bat = Counter()
    # sommets partages : un point du plan ou DEUX pieces d'interface se
    # rejoignent -> c'est la ou l'onglet doit se faire
    coins = {}
    for i in it["interfaces"]:
        a, b = i["a"], i["b"]
        r = i.get("resolution") or "rien"
        touche_bat = (fam.get(a) == "batiment") or (fam.get(b) == "batiment")
        if touche_bat:
            res_bat[r] += 1
        if r == "rien":
            continue
        for pl in (i.get("polylignes") or []):
            xy = np.asarray(pl, dtype=np.float64)
            if len(xy) < 2:
                continue
            for e in (xy[0], xy[-1]):
                k = (round(float(e[0]), 2), round(float(e[1]), 2))
                d = coins.setdefault(k, {"n": 0, "res": Counter(),
                                         "bat": False, "cles": []})
                d["n"] += 1
                d["res"][r] += 1
                d["bat"] = d["bat"] or touche_bat
                if len(d["cles"]) < 4:
                    d["cles"].append(a + "|" + b)

    # un ANGLE interessant : >= 2 pieces qui partagent l'extremite, au moins un
    # cote batiment, et des pieces qui ont une EPAISSEUR visible (mur/bordure)
    cands = []
    for k, d in coins.items():
        if d["n"] < 2 or not d["bat"]:
            continue
        if not (d["res"].get("mur") or d["res"].get("bordure")):
            continue
        cands.append({"x": k[0], "y": k[1], "n": d["n"],
                      "res": dict(d["res"]), "cles": d["cles"]})
    cands.sort(key=lambda c: (-c["n"], abs(c["x"] - 250) + abs(c["y"] - 250)))

    out = {
        "cellule": CELL,
        "terrassements": {"total": len(tr), "par_piece": dict(ct_piece),
                          "talus_largeur_m": ({
                              "n": int(larg.size),
                              "n_sup_1cm": int((larg > 0.01).sum()),
                              "min": round(float(larg.min()), 3),
                              "med": round(float(np.median(larg)), 3),
                              "max": round(float(larg.max()), 3)}
                              if larg.size else None)},
        "interfaces_touchant_un_batiment": dict(res_bat),
        "extremites_partagees": {
            "total": len(coins),
            "avec_2_pieces_ou_plus": sum(1 for d in coins.values()
                                         if d["n"] >= 2),
            "dont_un_cote_batiment": sum(1 for d in coins.values()
                                         if d["n"] >= 2 and d["bat"]),
        },
        "angles_candidats": cands[:12],
    }
    io.open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "mq_sonde_angle.json"), "w",
            encoding="utf-8").write(json.dumps(out, ensure_ascii=False,
                                               indent=1))
    print(json.dumps(out, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
