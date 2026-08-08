# -*- coding: utf-8 -*-
"""mq_sonde_onglet.py — SONDE DE MESURE (lecture seule) : la geometrie des
bandes de talus, AVANT correctif.

Deux defauts possibles, et il faut savoir lequel pese :
  (a) DANS une polyligne : le code decale tous les sommets par UNE normale
      moyenne prise sur la CORDE (mq_build.normale_moyenne n'utilise que le
      premier et le dernier point). Des que la ligne tourne, la bande n'est
      plus parallele au mur : elle s'elargit, se retrecit et se croise.
  (b) ENTRE deux polylignes qui partagent une extremite (l'angle du batiment) :
      chacune se decale selon SA normale -> recouvrement d'un cote, lacune de
      l'autre.

N'ECRIT RIEN dans plan_ville.
"""
import io
import json
import os
import sys
from collections import Counter, defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq_lib import Contrat  # noqa: E402

CELL = sys.argv[1] if len(sys.argv) > 1 else "0_0"


def norm_moy(xy):
    d = xy[-1] - xy[0]
    L = float(np.hypot(d[0], d[1]))
    if L < 1e-9:
        return np.array([0.0, 1.0])
    return np.array([-d[1] / L, d[0] / L])


def main():
    ct = Contrat(strict=False)
    q = ct.qui(CELL)
    it = ct.itf(CELL)
    tr = ct.terrassements(CELL, qui=q, itf=it)

    # couples de terrassement `talus` a largeur, avec leur largeur
    couples = {}
    for t in tr:
        if (t.get("piece") or "talus") != "talus":
            continue
        w = float(t.get("largeur_m") or 0.0)
        if w > 0.01:
            couples[(t["regle"], t["terrain"])] = w

    # les polylignes de ces couples (index global, comme mq_build)
    lignes = defaultdict(list)
    for c in ct.cells:
        for i in ct.itf(c)["interfaces"]:
            for k in ((i["a"], i["b"]), (i["b"], i["a"])):
                if k in couples:
                    lignes[k].extend(i.get("polylignes") or [])

    # ---- (a) ECART entre la normale de CORDE et les normales de SEGMENT -----
    ecarts = []          # degres, par segment
    pires = []
    n_pl = 0
    for k, pls in lignes.items():
        for pl in pls:
            xy = np.asarray(pl, dtype=np.float64)
            if len(xy) < 3:
                continue
            n_pl += 1
            nm = norm_moy(xy)
            d = np.diff(xy, axis=0)
            L = np.hypot(d[:, 0], d[:, 1])
            ok = L > 1e-9
            if not ok.any():
                continue
            ns = np.column_stack([-d[ok, 1] / L[ok], d[ok, 0] / L[ok]])
            cs = np.clip(ns @ nm, -1.0, 1.0)
            a = np.degrees(np.arccos(np.abs(cs)))
            ecarts.append(a)
            pires.append((float(a.max()), k[0] + "|" + k[1], len(xy)))
    E = np.concatenate(ecarts) if ecarts else np.zeros(0)
    pires.sort(reverse=True)

    # ---- (b) EXTREMITES PARTAGEES entre polylignes ---------------------------
    bouts = defaultdict(list)
    for k, pls in lignes.items():
        for j, pl in enumerate(pls):
            xy = np.asarray(pl, dtype=np.float64)
            if len(xy) < 2:
                continue
            for e in (xy[0], xy[-1]):
                bouts[(round(float(e[0]), 2), round(float(e[1]), 2))].append(k)
    partages = {p: v for p, v in bouts.items() if len(v) >= 2}
    meme_couple = sum(1 for v in partages.values() if len(set(v)) == 1)
    largeurs_diff = 0
    for v in partages.values():
        w = {round(couples[c], 3) for c in set(v)}
        if len(w) > 1:
            largeurs_diff += 1

    out = {
        "cellule": CELL,
        "couples_talus": len(couples),
        "polylignes_examinees": n_pl,
        "(a) ecart normale de CORDE vs normale de SEGMENT (degres)": {
            "segments": int(E.size),
            "median": round(float(np.median(E)), 2) if E.size else None,
            "p90": round(float(np.percentile(E, 90)), 2) if E.size else None,
            "max": round(float(E.max()), 2) if E.size else None,
            "pc_au_dela_de_5deg": round(float(100 * (E > 5).mean()), 1)
            if E.size else None,
            "pc_au_dela_de_20deg": round(float(100 * (E > 20).mean()), 1)
            if E.size else None,
            "pc_au_dela_de_45deg": round(float(100 * (E > 45).mean()), 1)
            if E.size else None,
        },
        "pires_polylignes": [{"ecart_max_deg": round(p[0], 1), "couple": p[1],
                              "sommets": p[2]} for p in pires[:5]],
        "(b) extremites partagees entre polylignes": {
            "total": len(partages),
            "meme_couple_donc_meme_largeur": meme_couple,
            "couples_differents": len(partages) - meme_couple,
            "dont_largeurs_differentes": largeurs_diff,
        },
        "largeur_m": {"min": round(min(couples.values()), 3),
                      "med": round(float(np.median(list(couples.values()))), 3),
                      "max": round(max(couples.values()), 3)} if couples else None,
    }
    io.open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "mq_sonde_onglet.json"), "w",
            encoding="utf-8").write(json.dumps(out, ensure_ascii=False,
                                               indent=1))
    print(json.dumps(out, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
