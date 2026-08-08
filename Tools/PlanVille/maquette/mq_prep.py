# -*- coding: utf-8 -*-
"""mq_prep.py — INSTANTANES de lecture seule des couches que le contrat exporte
PAS ENCORE (defaut de contrat remonte au coordinateur) :

  * la FAMILLE de matrice par parcelle  (cache/l1b_solveur.pkl : `familles`)
  * les ASSIETTES  (cote + provenance)  (cache/l1b_solveur.pkl : `assiettes`)
  * les TERRASSEMENTS (piece/dz/largeur) (cache/l1b_solveur.pkl : `terrassements`)
  * la HAUTEUR de batiment (entrees_3x3/bati_3x3.json, source ATTESTEE par
    l'empreinte `empreintes_sources.bati_3x3_json` du plan — verifiee ici)

Aucune ecriture hors de work/PLAN/maquette. Quand le contrat re-exporte porte
ces champs, mq_build.py les prend DANS LE CONTRAT et ignore ces instantanes.
"""
import hashlib
import io
import json
import os
import pickle
import sys
import time

MQ = r"C:\LidarPoC\work\PLAN\maquette"
CACHE = r"C:\LidarPoC\work\PLAN\cache"
V1 = r"C:\LidarPoC\work\PLAN\plan_ville\v1"
BATI = r"C:\LidarPoC\work\FINITION_SOL\entrees_3x3\bati_3x3.json"


def md5f(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    t0 = time.time()
    plan = json.load(io.open(os.path.join(V1, "plan.json"), encoding="utf-8"))
    att = plan["empreintes_sources"]["bati_3x3_json"]
    got = md5f(BATI)
    if got != att:
        print("REFUS: bati_3x3.json (%s) ne porte pas l'empreinte attestee par "
              "le plan (%s)" % (got, att))
        return 2
    print("bati_3x3.json ATTESTE par le plan : md5 %s" % got)

    sv = pickle.load(open(os.path.join(CACHE, "l1b_solveur.pkl"), "rb"))
    fam = sv["familles"]
    ass = {a["parcelle"]: a for a in sv["assiettes"]}
    ter = sv["terrassements"]
    print("familles %d | assiettes %d | terrassements %d"
          % (len(fam), len(ass), len(ter)))

    b = json.load(io.open(BATI, encoding="utf-8-sig"))
    blds = b["buildings"]
    haut = {}
    for i, e in enumerate(blds):
        h = e.get("h")
        if h is not None:
            haut["bat/%d" % i] = round(float(h), 3)
    print("hauteurs de batiment : %d / %d" % (len(haut), len(blds)))

    out = {
        "produit_par": "work/PLAN/maquette/mq_prep.py",
        "note": "INSTANTANE des couches absentes du contrat exporte ; "
                "a supprimer des que le contrat les porte",
        "empreintes": {
            "bati_3x3_json": got,
            "l1b_solveur_pkl": md5f(os.path.join(CACHE, "l1b_solveur.pkl")),
        },
        "familles": fam,
        "assiettes": {k: {"z_m": v["z_m"], "cote": v["cote"],
                          "famille": v["famille"],
                          "provenance": v["provenance"]}
                      for k, v in ass.items()},
        "terrassements": ter,
        "hauteur_batiment_m": haut,
    }
    p = os.path.join(MQ, "mq_snapshot.json")
    with io.open(p, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"),
                  sort_keys=True)
    print("ecrit %s (%.1f Mo) en %.1f s"
          % (p, os.path.getsize(p) / 1e6, time.time() - t0))
    from collections import Counter
    print("familles :", json.dumps(dict(Counter(fam.values())),
                                   ensure_ascii=False))
    print("pieces de terrassement :",
          json.dumps(dict(Counter(t["piece"] for t in ter)), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
