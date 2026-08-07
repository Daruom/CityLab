# -*- coding: utf-8 -*-
"""PLAN DE VILLE / passe 2 — L'EXPORT DU CONTRAT MACHINE, PAR CELLULE.

Le manifeste `plan_ville/v1/*.json` dit CE QUE VAUT le plan ; ces side-cars
disent CE QU'IL EST, dans le patron que le C++ consomme deja (un fichier par
cellule, `cell` / `cellSizeM` / `origin` / `crs` / `axes` en tete).

⚠️ CONVENTION DE DECOUPE (documentee dans plan_index.json) : une parcelle a
cheval sur plusieurs cellules est **DECOUPEE PAR CELLULE** — chaque fichier ne
porte que la part de geometrie qui tombe dans SA cellule. Justification : le
lecteur C++ est district-first ; il doit pouvoir construire une cellule sans
ouvrir aucune autre. Le champ `cellule_porteuse` (cellule du point
representatif de la parcelle ENTIERE) et le drapeau `entiere` permettent de
reconstituer les comptes distincts du manifeste sans lire toutes les cellules.
Les valeurs NON geometriques (loi de Z, axe du troncon, matiere) sont
RECOPIEES entieres dans chaque cellule concernee : une cellule se lit seule.

⚠️ Aucune REGLE du plan ne change dans cette passe. Les 5 frontieres hors
catalogue sont exportees telles quelles, `resolution = "arbitrage_demande"`.

⛔ Rien n'est ecrit hors de work\\PLAN.
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
from shapely.geometry import box
from shapely.strtree import STRtree

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (CACHE, CELL_M, ENT, OUT, chrono, jalon, md5_logique,
                      md5_octets)
from c1_qui import valide, eclate
from c2_matiere import MASQUE_PX, PX_M, SEUIL_R, charge_masques

DATA = os.path.join(OUT, "data")
MM = 3                      # les geometries sont ecrites au MILLIMETRE
AIRE_NULLE_M2 = 1e-6        # DETTE c8 SOLDEE : une parcelle d'aire nulle
#                             n'entre pas dans le contrat (elle est comptee)
ENTETE = {"cellSizeM": CELL_M,
          "crs": "equirectangulaire locale (SourceData/toulouse10_mnt.json)",
          "axes": "x = est (m), y = SUD (m), z = altitude NGF (m)",
          "plan": "plan_ville/v1",
          "produit_par": "work/PLAN/c8_export.py"}


def ecrire(path, obj):
    """Ecriture DETERMINISTE : cles triees, separateurs fixes, LF."""
    s = json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=False)
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)
    return {"octets": os.path.getsize(path), "md5_octets": md5_octets(path),
            "md5_logique": md5_logique(s)}


def _dedup(pts, ferme):
    """DETTE c8 SOLDEE : deux sommets a moins d'un MILLIMETRE l'un de l'autre
    sont un seul sommet. Le contrat sort propre ; la fusion cote lecteur C++
    reste comme garde, elle n'a plus rien a rattraper."""
    out = []
    for q in pts:
        if out and abs(q[0] - out[-1][0]) < 1e-3 and abs(q[1] - out[-1][1]) < 1e-3:
            continue
        out.append(q)
    if ferme:
        while len(out) > 1 and abs(out[0][0] - out[-1][0]) < 1e-3 \
                and abs(out[0][1] - out[-1][1]) < 1e-3:
            out.pop()
        if len(out) >= 3:
            out.append([out[0][0], out[0][1]])
    return out


def coords(g):
    """Anneaux d'un polygone (ext puis trous), au mm, sommets dedupliques."""
    out = []
    for h in (g.geoms if hasattr(g, "geoms") else [g]):
        if h.geom_type != "Polygon":
            continue
        for ring in [h.exterior] + list(h.interiors):
            c = np.asarray(ring.coords)
            if len(c) < 4:
                continue
            r = _dedup([[round(float(x), MM), round(float(y), MM)]
                        for x, y in c], True)
            if len(r) >= 4:
                out.append(r)
    return out


def lignes(g):
    out = []
    for h in (g.geoms if hasattr(g, "geoms") else [g]):
        if h.geom_type != "LineString":
            continue
        c = np.asarray(h.coords)
        if len(c) < 2:
            continue
        out.append([[round(float(x), MM), round(float(y), MM)] for x, y in c])
    return out


# ================================================== LE SEMIS, RECORD COMPLET ==
def lit_semis_complet():
    """Le semis BRUT avec TOUS ses champs, par le lecteur UNIQUE du socle."""
    from c0_socle import lit_semis_brut
    t0 = time.time()
    rec, X, Y = lit_semis_brut()
    chrono("C8/semis lecture", time.time() - t0, "%d instances" % len(rec))
    return rec, X, Y


def retenues(rec, X, Y, P, mat):
    """LA REGLE DU PLAN, rejouee a l'identique (c5_juges.py) : une instance vit
    si son PIXEL de masque est de l'herbe (R >= 128) ET si elle n'est pas sur
    une parcelle minerale du plan."""
    t0 = time.time()
    M = charge_masques()
    veg = np.zeros(X.size, dtype=bool)
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
        veg[m] = R[jj, ii] >= SEUIL_R
    dur = shapely.union_all([p["geom"] for p in P
                             if mat[p["id"]][0] == "mineral"])
    shapely.prepare(dur)
    vit = veg & (~shapely.contains_xy(dur, X, Y))
    chrono("C8/semis regle", time.time() - t0, "%d retenues" % int(vit.sum()))
    return vit


def main():
    t0 = time.time()
    if not os.path.isdir(DATA):
        os.makedirs(DATA)
    with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as f:
        D = pickle.load(f)
    P = D["parcelles"]
    for p in P:
        p["geom"] = shapely.from_wkb(p["geom"])
    cells = [tuple(c) for c in D["cells"]]
    with open(os.path.join(CACHE, "matiere.pkl"), "rb") as f:
        mat = pickle.load(f)["matiere"]
    with open(os.path.join(CACHE, "niveaux.pkl"), "rb") as f:
        lois = pickle.load(f)["lois"]
    with open(os.path.join(CACHE, "interfaces.pkl"), "rb") as f:
        fronts = pickle.load(f)["fronts"]
    jalon("C8/ENTREES : %d parcelles, %d frontieres, %d cellules"
          % (len(P), len(fronts), len(cells)))

    rec, XS, YS = lit_semis_complet()
    vit = retenues(rec, XS, YS, P, mat)

    # index spatial
    byid = {p["id"]: p for p in P}
    G = [p["geom"] for p in P]
    T = STRtree(G)
    porteuse = {}
    for p in P:
        c = p["geom"].representative_point()
        porteuse[p["id"]] = "%d_%d" % (int(np.floor(c.x / CELL_M)),
                                       int(np.floor(c.y / CELL_M)))
    # geometries de frontiere, calculees UNE fois
    t1 = time.time()
    fgeom = []
    for f in fronts:
        a, b = byid.get(f["a"]), byid.get(f["b"])
        try:
            fgeom.append(a["geom"].boundary.intersection(
                b["geom"].boundary))
        except Exception:
            fgeom.append(None)
    TF = STRtree([g if g is not None else shapely.Point(0, 0) for g in fgeom])
    chrono("C8/geom frontieres", time.time() - t1, "%d" % len(fgeom))

    # semis par cellule
    scx = np.floor(XS / CELL_M).astype(np.int64)
    scy = np.floor(YS / CELL_M).astype(np.int64)

    fichiers = {}
    nulles = set()
    par_cell = {}
    n_parc_pieces = n_int_pieces = n_sem = 0
    ids_parc, ids_int = set(), set()
    t2 = time.time()
    for n, (cx, cy) in enumerate(cells):
        nom = "%d_%d" % (cx, cy)
        bx = box(cx * CELL_M, cy * CELL_M, (cx + 1) * CELL_M,
                 (cy + 1) * CELL_M)
        ent = dict(ENTETE)
        ent["cell"] = [cx, cy]
        ent["origin"] = [cx * CELL_M, cy * CELL_M]

        # ---- ① QUI ------------------------------------------------------
        L = []
        for j in sorted(int(k) for k in T.query(bx)):
            p = P[j]
            g = p["geom"]
            if g.area <= AIRE_NULLE_M2:
                nulles.add(p["id"])
                continue
            try:
                q = valide(g.intersection(bx))
            except Exception:
                continue
            if q.is_empty or q.area <= 1e-9:
                continue
            an = coords(q)
            if not an:
                continue
            loi = dict(lois.get(p["id"]) or {})
            forme = loi.pop("loi", None)
            lo = {"forme": forme}
            if forme == "constante":
                lo["z_m"] = loi.get("z_m")
            elif forme == "profil_troncon":
                pr = loi.get("profil") or {}
                src = loi.get("loi_heritee_de") or p["id"]
                axe = (byid.get(src, {}).get("meta") or {}).get("axe")
                lo["profil"] = pr.get("pts")
                lo["L_m"] = pr.get("L_m")
                lo["pente_max_pc"] = pr.get("pente_max_pc")
                lo["axe"] = axe
            if loi.get("loi_heritee_de"):
                lo["loi_heritee_de"] = loi["loi_heritee_de"]
            r = {"id": p["id"], "proprietaire": p["proprietaire"],
                 "matiere": mat[p["id"]][0], "loi": lo,
                 "aire_m2": round(q.area, 3),
                 "aire_totale_m2": round(g.area, 3),
                 "cellule_porteuse": porteuse[p["id"]],
                 "entiere": bool(abs(q.area - g.area) < 1e-6),
                 "anneaux": an}
            me = p.get("meta") or {}
            if me.get("heritee"):
                r["heritee"] = True
                r["provenance"] = me.get("provenance")
            if me.get("bande"):
                r["bande"] = True
                r["largeur_m"] = me.get("largeur_m")
            if me.get("trou_comble"):
                r["trou_comble"] = True
            L.append(r)
            ids_parc.add(p["id"])
        L.sort(key=lambda r: r["id"])
        n_parc_pieces += len(L)
        d = dict(ent)
        d["parcelles"] = L
        d["convention_decoupe"] = "parcelle DECOUPEE par cellule ; " \
                                  "`cellule_porteuse` designe la cellule du " \
                                  "point representatif de la parcelle entiere"
        fichiers["plan_qui_%s.json" % nom] = ecrire(
            os.path.join(DATA, "plan_qui_%s.json" % nom), d)

        # ---- ④ INTERFACES ------------------------------------------------
        I = []
        for j in sorted(int(k) for k in TF.query(bx)):
            g = fgeom[j]
            if g is None or g.is_empty:
                continue
            f = fronts[j]
            try:
                q = g.intersection(bx)
            except Exception:
                continue
            pl = lignes(q)
            if not pl:
                continue
            res = f["type"] or "arbitrage_demande"
            r = {"a": f["a"], "b": f["b"], "resolution": res,
                 "matieres": f["mat"], "dz_m": f["dz_m"],
                 "dz_max_m": f["dz_max_m"],
                 "longueur_m": round(q.length, 3),
                 "longueur_totale_m": f["longueur_m"],
                 "polylignes": pl}
            if res in ("bordure", "mur"):
                r["h_m"] = f.get("h_m", f["dz_m"])
            if res == "arbitrage_demande":
                r["motif"] = f["motif"]
            I.append(r)
            ids_int.add((f["a"], f["b"]))
        I.sort(key=lambda r: (r["a"], r["b"]))
        n_int_pieces += len(I)
        d = dict(ent)
        d["catalogue"] = ["rien", "talus", "emmarchement", "affleurement",
                          "bordure", "mur", "arbitrage_demande"]
        d["interfaces"] = I
        fichiers["plan_interfaces_%s.json" % nom] = ecrire(
            os.path.join(DATA, "plan_interfaces_%s.json" % nom), d)

        # ---- SEMIS --------------------------------------------------------
        m = vit & (scx == cx) & (scy == cy)
        idx = np.nonzero(m)[0]
        S = []
        for k in idx:
            r = rec[int(k)]
            S.append({"mesh": r[0].decode("utf-8"), "x": float(r[1]),
                      "y": float(r[2]), "scale": float(r[3]),
                      "yaw": float(r[4]), "kind": r[5].decode("utf-8")})
        n_sem += len(S)
        d = dict(ent)
        d["regle"] = "une instance vit si son PIXEL de masque est de l'herbe " \
                     "(R >= %d) ET si elle n'est pas sur une parcelle " \
                     "minerale du plan" % SEUIL_R
        d["instances"] = S
        fichiers["plan_semis_%s.json" % nom] = ecrire(
            os.path.join(DATA, "plan_semis_%s.json" % nom), d)

        par_cell[nom] = {"parcelles": len(L), "interfaces": len(I),
                         "instances": len(S)}
        if (n + 1) % 10 == 0:
            jalon("C8/  export : %d / %d cellules (%.0f s)"
                  % (n + 1, len(cells), time.time() - t2))
    chrono("C8/export", time.time() - t2, "%d fichiers" % len(fichiers))

    # ---- l'index ----------------------------------------------------------
    fam = {}
    for k, v in fichiers.items():
        f = k.split("_")[1]
        fam.setdefault(f, {"fichiers": 0, "octets": 0})
        fam[f]["fichiers"] += 1
        fam[f]["octets"] += v["octets"]
    index = {
        "version": "plan_ville/v1 — donnees",
        "produit_par": "work/PLAN/c8_export.py",
        "domaine": {"cellules": ["%d_%d" % c for c in cells],
                    "cellule_m": CELL_M,
                    "m2": round(len(cells) * CELL_M * CELL_M, 1)},
        "convention_decoupe": {
            "choix": "parcelle et frontiere DECOUPEES PAR CELLULE",
            "justification": "le lecteur C++ est district-first : il doit "
                             "pouvoir construire une cellule sans ouvrir "
                             "aucune autre",
            "champs": {"cellule_porteuse": "cellule du point representatif de "
                                           "la parcelle ENTIERE — sert a "
                                           "reconstituer les comptes distincts",
                       "entiere": "true si la parcelle tient tout entiere dans "
                                  "cette cellule",
                       "aire_totale_m2": "aire de la parcelle ENTIERE",
                       "longueur_totale_m": "longueur de la frontiere ENTIERE"},
            "non_geometrique": "loi de Z, axe de troncon et matiere sont "
                               "RECOPIES entiers dans chaque cellule concernee"},
        "parcelles_aire_nulle_exclues": {"n": len(nulles),
                                         "ids": sorted(nulles)[:50]},
        "deduplication_sommets_mm": 1.0,
        "totaux": {"parcelles_distinctes": len(ids_parc),
                   "parcelles_pieces": n_parc_pieces,
                   "interfaces_distinctes": len(ids_int),
                   "interfaces_pieces": n_int_pieces,
                   "instances": n_sem},
        "unites": {"geometries": "metres, arrondies au millimetre (3 decimales)",
                   "z": "altitude NGF en metres"},
        "arbitrage_differe": "les frontieres hors catalogue sont exportees "
                             "telles quelles (`arbitrage_demande`) ; aucune "
                             "regle du plan n'a change dans cette passe",
        "par_cellule": par_cell,
        "tailles_par_famille": {k: {"fichiers": v["fichiers"],
                                    "Mo": round(v["octets"] / 1e6, 3)}
                                for k, v in sorted(fam.items())},
        "fichiers": fichiers}
    emp = ecrire(os.path.join(DATA, "plan_index.json"), index)

    # ---- le manifeste pointe vers l'index ---------------------------------
    pm = os.path.join(OUT, "plan.json")
    plan = json.load(io.open(pm, encoding="utf-8"))
    plan["index_donnees"] = {"fichier": "data/plan_index.json",
                             "octets": emp["octets"],
                             "md5_octets": emp["md5_octets"],
                             "md5_logique": emp["md5_logique"],
                             "totaux": index["totaux"],
                             "convention_decoupe":
                                 index["convention_decoupe"]["choix"]}
    s = json.dumps(plan, indent=1, sort_keys=True, ensure_ascii=False)
    with io.open(pm, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)

    tot = sum(v["octets"] for v in fichiers.values()) + emp["octets"]
    jalon("C8/⭐ CONTRAT MACHINE : %d fichiers dans plan_ville/v1/data/ "
          "(%.1f Mo) ; %s ; TOTAUX : %d parcelles distinctes (%d pieces de "
          "cellule), %d interfaces distinctes (%d pieces), %d instances ; "
          "index md5 octets %s / logique %s"
          % (len(fichiers) + 1, tot / 1e6,
             " | ".join("%s %d fichiers %.1f Mo" % (k, v["fichiers"],
                                                    v["octets"] / 1e6)
                        for k, v in sorted(fam.items())),
             len(ids_parc), n_parc_pieces, len(ids_int), n_int_pieces, n_sem,
             emp["md5_octets"], emp["md5_logique"]))
    chrono("C8 TOTAL", time.time() - t0, "%.1f Mo" % (tot / 1e6))
    return fichiers, emp


if __name__ == "__main__":
    main()
