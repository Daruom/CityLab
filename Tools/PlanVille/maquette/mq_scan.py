# -*- coding: utf-8 -*-
"""mq_scan.py — releve EXHAUSTIF du schema du contrat plan_ville/v1.
LECTURE SEULE. Sortie : maquette/scan_contrat.json + resume stdout.
"""
import json, os, sys, glob, time
from collections import Counter, defaultdict

DATA = r"C:\LidarPoC\work\PLAN\plan_ville\v1\data"
OUT = r"C:\LidarPoC\work\PLAN\maquette"


def jload(p):
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    t0 = time.time()
    idx = jload(os.path.join(DATA, "plan_index.json"))
    cells = list(idx["par_cellule"].keys())

    par_keys = Counter()
    par_lawkeys = Counter()
    par_forme = Counter()
    par_prop = Counter()
    par_mat = Counter()
    prop_x_forme = Counter()
    itf_keys = Counter()
    itf_res = Counter()
    itf_extra = defaultdict(Counter)
    sem_keys = Counter()
    sem_esp = Counter()
    law_by_forme = defaultdict(Counter)   # forme -> keys present
    law_example = {}
    prop_example = {}
    res_example = {}
    sem_example = {}
    n_par = n_itf = n_sem = 0
    ring_stats = Counter()
    zmin, zmax = 1e18, -1e18

    for c in cells:
        q = jload(os.path.join(DATA, "plan_qui_%s.json" % c))
        for p in q["parcelles"]:
            n_par += 1
            for k in p:
                par_keys[k] += 1
            loi = p.get("loi") or {}
            f = loi.get("forme")
            par_forme[f] += 1
            for k in loi:
                par_lawkeys[k] += 1
                law_by_forme[f][k] += 1
            if f not in law_example:
                law_example[f] = p
            pr = p.get("proprietaire")
            par_prop[pr] += 1
            prop_x_forme[(pr, f)] += 1
            if pr not in prop_example:
                prop_example[pr] = p
            par_mat[p.get("matiere")] += 1
            an = p.get("anneaux") or []
            ring_stats["anneaux_total"] += len(an)
            if len(an) > 1:
                ring_stats["parcelles_multi_anneaux"] += 1
            for r in an:
                ring_stats["sommets_total"] += len(r)
            if f == "constante":
                z = loi.get("z_m")
                if z is not None:
                    zmin = min(zmin, z); zmax = max(zmax, z)

        it = jload(os.path.join(DATA, "plan_interfaces_%s.json" % c))
        for i in it["interfaces"]:
            n_itf += 1
            for k in i:
                itf_keys[k] += 1
            r = i.get("resolution")
            itf_res[r] += 1
            if r not in res_example:
                res_example[r] = i
            for k in i:
                if k not in ("a", "b", "polylignes", "matieres"):
                    pass
        cat = it.get("catalogue")

        s = jload(os.path.join(DATA, "plan_semis_%s.json" % c))
        for e in s["instances"]:
            n_sem += 1
            for k in e:
                sem_keys[k] += 1
            sem_esp[e.get("espece") or e.get("type") or e.get("classe")] += 1
            if not sem_example:
                sem_example["ex"] = e

    out = {
        "cellules_n": len(cells),
        "parcelles_pieces": n_par,
        "interfaces_pieces": n_itf,
        "instances": n_sem,
        "parcelle_champs": dict(par_keys),
        "loi_champs": dict(par_lawkeys),
        "loi_formes": dict(par_forme),
        "loi_champs_par_forme": {str(k): dict(v) for k, v in law_by_forme.items()},
        "loi_exemple_par_forme": {str(k): v for k, v in law_example.items()},
        "proprietaires": dict(par_prop),
        "proprietaire_exemple": prop_example,
        "matieres": dict(par_mat),
        "proprietaire_x_forme": {"%s|%s" % (a, b): n for (a, b), n in prop_x_forme.items()},
        "interface_champs": dict(itf_keys),
        "interface_resolutions": dict(itf_res),
        "interface_exemple_par_resolution": {str(k): v for k, v in res_example.items()},
        "catalogue": cat,
        "semis_champs": dict(sem_keys),
        "semis_especes": dict(sem_esp),
        "semis_exemple": sem_example,
        "geometrie": dict(ring_stats),
        "z_constante_min_max": [zmin, zmax],
        "secondes": round(time.time() - t0, 1),
    }
    with open(os.path.join(OUT, "scan_contrat.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1, sort_keys=True)

    def show(t, d):
        print("--", t, json.dumps(d, ensure_ascii=False)[:1400])
    print("cellules", len(cells), "parcelles", n_par, "interfaces", n_itf, "instances", n_sem)
    show("champs parcelle", dict(par_keys))
    show("formes de loi", dict(par_forme))
    show("champs par forme", {str(k): sorted(v) for k, v in law_by_forme.items()})
    show("proprietaires", dict(par_prop))
    show("matieres", dict(par_mat))
    show("champs interface", dict(itf_keys))
    show("resolutions", dict(itf_res))
    show("catalogue", cat)
    show("champs semis", dict(sem_keys))
    show("especes semis", dict(sem_esp))
    show("geometrie", dict(ring_stats))
    print("z constante", zmin, zmax, "en", round(time.time() - t0, 1), "s")


if __name__ == "__main__":
    main()
