# -*- coding: utf-8 -*-
"""mq_build.py — LA MAQUETTE BLANCHE : le contrat plan_ville/v1 -> geometrie 3D.

Une commande, tout le domaine :
    C:\\LidarPoC\\venv\\Scripts\\python.exe mq_build.py
Une seule cellule (mise au point) :
    ... mq_build.py --cellules 0_0,0_1

Sorties (dans maquette\\web\\) : mq_index.js + cells\\mq_<cx>_<cy>.js
Les fichiers .js sont charges par balise <script> : la page marche en
DOUBLE-CLIC (file://), ou fetch() est interdit.

Couches produites, toutes depuis le contrat :
  sol   parcelles a leur loi resolue (constante / profil_troncon / drapage)
  eau   parcelles de matiere eau, aux cotes de bief
  ouvr  parcelles d'ouvrage en VOLUMES (tablier, mur, escalier, edicule...)
  bati  emprises de batiment extrudees sur leur assiette
  itf   faces de liaison du catalogue (bordure/mur/emmarchement/talus/...)
  terr  terrassements (talus en pente, soutenements et quais en face verticale)
  veg   semis en volumes ultra-simples, instancies
"""
import io
import json
import os
import sys
import time
from collections import Counter, defaultdict

import numpy as np
import shapely
from shapely.geometry import Polygon
from shapely.strtree import STRtree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq_lib import (CAT_ID, CATALOGUE, Contrat, FAM_ID, FAMILLES, MQ, PARAMS,
                    Registre, Tampon, ZEval, b64, chrono, jalon, polygone,
                    prisme, quad_strip, trianguler)

WEB = os.path.join(MQ, "web")
CELLS = os.path.join(WEB, "cells")

RESSAUT_M = 0.02          # registre::ressaut_max
BORDURE_MAX_M = 0.20      # registre::bordure_vue


def marches(xy, z_haut, z_bas, normale, h_max, g_min, blondel):
    """Une volee d'emmarchement a la geometrie de l'arrete : marches egales,
    h <= h_max, giron >= g_min, 2h + g dans les bornes de Blondel. Les marches
    s'etendent dans la direction `normale` (vers le cote BAS)."""
    dz = float(z_haut - z_bas)
    if dz <= 1e-6:
        return None, None, 0, 0.0, 0.0
    n = int(np.ceil(dz / h_max))
    h = dz / n
    g = max(g_min, blondel[0] - 2.0 * h)
    if 2.0 * h + g > blondel[1]:
        g = max(g_min, blondel[1] - 2.0 * h)
    V = []
    T = []
    base = 0
    m = len(xy)
    for k in range(n):
        z_t = z_haut - k * h            # nez de la marche k
        z_b = z_haut - (k + 1) * h
        off_a = normale * (k * g)
        off_b = normale * ((k + 1) * g)
        # contremarche (verticale) puis giron (horizontal)
        A = xy + off_a
        B = xy + off_b
        for (P0, zz0, P1, zz1) in ((A, z_t, A, z_b), (A, z_b, B, z_b)):
            V0 = np.empty((2 * m, 3))
            V0[0::2, :2] = P0
            V0[0::2, 2] = zz0
            V0[1::2, :2] = P1
            V0[1::2, 2] = zz1
            k2 = np.arange(m - 1)
            a = base + 2 * k2
            b = base + 2 * k2 + 1
            c = base + 2 * k2 + 2
            d = base + 2 * k2 + 3
            T.append(np.concatenate([np.stack([a, b, d], 1),
                                     np.stack([a, d, c], 1)]))
            V.append(V0)
            base += 2 * m
    return (np.concatenate(V), np.concatenate(T), n, h, g)


def cote_bas(gpoly, xy, normale):
    """La normale pointe-t-elle vers l'interieur de `gpoly` ? (choix du cote)"""
    if gpoly is None:
        return normale
    i = len(xy) // 2
    p = xy[i] + normale * 0.35
    try:
        if gpoly.contains(shapely.Point(p[0], p[1])):
            return normale
    except Exception:
        pass
    return -normale


def normale_moyenne(xy):
    d = xy[-1] - xy[0]
    L = float(np.hypot(d[0], d[1]))
    if L < 1e-9:
        return np.array([0.0, 1.0])
    return np.array([-d[1] / L, d[0] / L])


LIGNES = {}


def index_lignes(ct, cells):
    """Index GLOBAL couple -> polylignes de contact, pour les terrassements.
    Il doit etre global : la piece d'interface d'un couple peut vivre dans une
    AUTRE cellule que le point du terrassement (le contrat decoupe les deux
    par cellule, mais pas au meme endroit)."""
    t = time.time()
    besoin = set()
    for c in cells:
        for tr in ct.terrassements(c):
            besoin.add((tr["regle"], tr["terrain"]))
            besoin.add((tr["terrain"], tr["regle"]))
    n = 0
    for c in ct.cells:
        for i in ct.itf(c)["interfaces"]:
            for k in ((i["a"], i["b"]), (i["b"], i["a"])):
                if k in besoin:
                    LIGNES.setdefault(k, []).extend(i.get("polylignes") or [])
                    n += 1
    chrono("index des lignes de contact", time.time() - t,
           "%d couples demandes, %d pieces trouvees" % (len(besoin) // 2, n))
    return LIGNES


def construire_cellule(ct, ze, cle, stats):
    t_c = time.time()
    q = ct.qui(cle)
    it = ct.itf(cle)
    sm = ct.semis(cle)
    par = q["parcelles"]

    geoms = {}
    lois = {}
    fams = {}
    haut = ze.haut
    for p in par:
        geoms[p["id"]] = None
        lois[p["id"]] = p["loi"]
        fams[p["id"]] = ct.famille(p)
    assiettes = ct.assiettes(cle)

    table = {}
    T_sol = Tampon(table)
    T_eau = Tampon(table)
    T_ouv = Tampon(table)
    T_bat = Tampon(table)
    T_itf = Tampon(table)
    T_ter = Tampon(table)
    itf_dz = []
    itf_cat = []
    bat_h = []
    intrados = {}
    fiches = {}

    # ------------------------------------------------------- LE SOL ---------
    for p in par:
        pid = p["id"]
        fam = fams[pid]
        g = polygone(p["anneaux"])
        geoms[pid] = g
        v, tri = trianguler(g)
        if v is None:
            stats["parcelles_non_triangulables"] += 1
            ct.saut("parcelle non triangulable (emprise degeneree)", pid)
            continue
        z = ze.z_loi(p["loi"], v[:, 0], v[:, 1])
        if z is None:
            stats["parcelles_sans_z"] += 1
            ct.saut("parcelle dont la loi de Z ne s'evalue pas", pid)
            continue
        xyz = np.column_stack([v[:, 0], v[:, 1], z])
        fid = FAM_ID.get(fam, FAM_ID["sol_mineral"])
        if p["matiere"] == "eau":
            T_eau.ajouter(xyz, tri, fid, pid)
        else:
            T_sol.ajouter(xyz, tri, fid, pid)
        stats["parcelles"] += 1
        stats["fam_" + fam] += 1

        # --------------------------------------------- LES OUVRAGES --------
        if p["proprietaire"] == "ouvrage":
            # volume : de l'extrados declare a l'INTRADOS DU CONTRAT. Un
            # ouvrage sans tablier (`sans_objet` motive) n'a pas d'intrados :
            # il recoit une simple jupe pour exister en volume.
            zt = float(np.median(z))
            zi, prov = ct.cote_intrados(p)
            if prov == "contrat":
                stats["intrados_du_contrat"] += 1
            elif prov == "sans_objet":
                stats["intrados_sans_objet_motive"] += 1
                zi = zt - PARAMS["JUPE_OUVRAGE_MIN_M"]
            else:
                stats["intrados_MANQUANT"] += 1
                ct.saut("ouvrage muet sur l'intrados (ni cote, ni sans_objet)",
                        pid)
                zi = zt - PARAMS["JUPE_OUVRAGE_MIN_M"]
            # L'INTRADOS EN LONG DU CONTRAT (`profil_intrados`) quand il
            # existe : le dessous du tablier SUIT la travee au lieu d'etre un
            # plan. C'est le correctif du compilateur au defaut que j'avais
            # remonte (un scalaire sur une travee en pente coutait jusqu'a
            # 1,60 m). Repli sur la cote scalaire quand il n'y en a pas.
            lpi = ct.profil_intrados(p)
            if lpi is not None:
                stats["intrados_en_long_du_contrat"] += 1
                zi_f = (lambda xy, _l=lpi: ze.z_loi(_l, xy[:, 0], xy[:, 1]))
            else:
                zi_f = zi
            intrados[pid] = zi
            n0 = T_ouv.nt
            for anneau in p["anneaux"]:
                V, TT = prisme(anneau, zi_f, zt)
                if V is None:
                    ct.saut("anneau d'ouvrage non extrudable "
                            "(moins de 3 sommets distincts)", pid)
                    continue
                T_ouv.ajouter(V, TT, fid, pid)
            if T_ouv.nt == n0:
                ct.saut("ouvrage sans aucun volume produit", pid)
            stats["ouvrages"] += 1

        # ------------------------------------------- LES BATIMENTS ---------
        if p["proprietaire"] == "batiment":
            h = ct.hauteur_batiment(p, haut)
            if h is None:
                # LE SAUT QUI AVAIT ETE PAYE (bnd/1333#0) : il est desormais
                # COMPTE et NOMME, et il fait echouer --contrat-seul.
                stats["batiments_sans_hauteur"] += 1
                ct.saut("piece batie sans hauteur (ni propre, ni heritee)", pid)
                continue
            zb = float(np.median(z))
            n0 = T_bat.nt
            for anneau in p["anneaux"]:
                V, TT = prisme(anneau, zb, zb + h)
                if V is None:
                    ct.saut("anneau de batiment non extrudable "
                            "(moins de 3 sommets distincts)", pid)
                    continue
                T_bat.ajouter(V, TT, FAM_ID["batiment"], pid)
            if T_bat.nt > n0:
                bat_h.append(round(h, 2))
                stats["batiments"] += 1
            else:
                ct.saut("batiment sans aucun volume produit", pid)

    # ------------------------------------------- LES FACES D'INTERFACE ------
    P = PARAMS
    for i in it["interfaces"]:
        res = i.get("resolution") or "rien"
        a, b = i["a"], i["b"]
        cle_i = a + "|" + b
        if res == "rien":
            stats["itf_rien"] += 1
            ct.saut("interface resolue a `rien`", cle_i)
            continue
        cid = CAT_ID.get(res, CAT_ID["mur"])
        pls = i.get("polylignes") or []
        if not pls:
            ct.saut("interface sans polyligne de contact", cle_i)
            continue
        for pl in pls:
            xy = np.asarray(pl, dtype=np.float64)
            if len(xy) < 2:
                ct.saut("polyligne d'interface degeneree (moins de 2 points)",
                        cle_i)
                continue
            za = ze.z(a, xy[:, 0], xy[:, 1])
            zb = ze.z(b, xy[:, 0], xy[:, 1])
            if za is None or zb is None:
                stats["itf_sans_z"] += 1
                # DEUX CAUSES BIEN DIFFERENTES, et une seule est excusable :
                #  * le cote n'a emis aucune piece -> LE CONTRAT LE DIT
                #    desormais lui-meme, frontiere par frontiere, par le champ
                #    `vers_parcelle_non_emise` : c'est une LECTURE, plus une
                #    deduction de ma part. Je compte a part ce que je devrais
                #    encore deduire (cible : zero) ;
                #  * le cote existe mais sa loi ne s'evalue pas -> PERTE.
                declare = i.get("vers_parcelle_non_emise") or []
                absents = [s for s in (a, b) if s not in ze.lois]
                if declare or absents:
                    for s in (declare or absents):
                        ct.sans_piece_vus.add(s)
                    if declare:
                        ct.sans_piece_declares += 1
                    else:
                        ct.sans_piece_deduits += 1
                    ct.saut(ct.SAUT_SANS_PIECE, cle_i)
                else:
                    ct.saut("interface dont un cote a une loi de Z "
                            "INEVALUABLE", cle_i)
                continue
            dz = float(np.median(np.abs(za - zb)))
            if res == "emmarchement":
                haut = za if np.median(za) >= np.median(zb) else zb
                bas = zb if np.median(za) >= np.median(zb) else za
                gbas = geoms.get(b if np.median(za) >= np.median(zb) else a)
                nrm = cote_bas(gbas, xy, normale_moyenne(xy))
                V, TT, nm, hm, gm = marches(
                    xy, float(np.median(haut)), float(np.median(bas)), nrm,
                    P["MARCHE_H_MAX_M"], P["MARCHE_GIRON_MIN_M"],
                    P["MARCHE_BLONDEL_M"])
                if V is None:
                    # Aucune marche a generer. Le contrat porte-t-il LUI-MEME
                    # un denivele nul sur ce contact ? Si oui, il n'y a rien a
                    # batir et le saut est justifie. Si NON, mon evaluation du
                    # Z contredit le dZ declare : c'est une PERTE, et le
                    # verrou doit la crier.
                    dzc = i.get("dz_m")
                    if dzc is not None and abs(float(dzc)) < 1e-9:
                        ct.saut(ct.SAUT_EMM_PLAT, cle_i)
                    else:
                        ct.saut("emmarchement sans marche alors que le contrat "
                                "declare un denivele NON NUL (desaccord entre "
                                "ma lecture du Z et le dz du contrat)", cle_i)
                    continue
                T_itf.ajouter(V, TT, cid, cle_i)
                stats["marches_generees"] += nm
                stats["emmarchements"] += 1
            else:
                V, TT = quad_strip(xy, za, zb)
                if V is None:
                    ct.saut("face d'interface non construite (bande "
                            "degeneree)", cle_i)
                    continue
                # repere de l'arete HAUTE : sert a dresser la face en ailette
                # dans la carte des marches
                H = np.zeros(len(V), dtype=np.uint8)
                if np.median(za) >= np.median(zb):
                    H[0::2] = 1
                else:
                    H[1::2] = 1
                T_itf.ajouter(V, TT, cid, cle_i, haut=H)
            itf_dz.append(dz)
            itf_cat.append(cid)
            stats["itf_" + res] += 1

    # --------------------------------------------- LES TERRASSEMENTS --------
    lignes = LIGNES
    ci, cj = [int(v) for v in cle.split("_")]
    C = ct.cell_m
    ox, oy = ci * C, cj * C
    for tr in ct.terrassements(cle, qui=q, itf=it):
        cle_t = tr["regle"] + "|" + tr["terrain"]
        pls = lignes.get((tr["regle"], tr["terrain"]))
        if not pls:
            stats["terrassements_sans_ligne"] += 1
            ct.saut("terrassement sans ligne de contact dans tout le domaine",
                    cle_t)
            continue
        larg = float(tr.get("largeur_m") or 0.0)
        piece = tr.get("piece") or "talus"
        fid = FAM_ID["terrassement"]
        n0 = T_ter.nt
        sans_z = False
        for pl in pls:
            xy = np.asarray(pl, dtype=np.float64)
            if len(xy) < 2:
                ct.saut("polyligne de terrassement degeneree (moins de 2 "
                        "points)", cle_t)
                continue
            # LA COTE SE PREND LE LONG DE LA LIGNE, PAS EN UN SEUL POINT.
            # Avant, un unique echantillon pris en (tr.x, tr.y) etait etendu a
            # toute la polyligne : sur un quai qui monte, la face verticale
            # restait plate pendant que les parcelles voisines suivaient leur
            # loi point par point — et la fente s'ouvrait entre les deux.
            # Ecart ainsi commis, mesure sur 6000 polylignes : 80 % au-dela de
            # 2 cm cote terrain (median 7,2 cm, p95 40,6 cm, max 1,22 m).
            # C'est LA cause des « faces noires » (qui sont des TROUS).
            zr = ze.z(tr["regle"], xy[:, 0], xy[:, 1])
            zt = ze.z(tr["terrain"], xy[:, 0], xy[:, 1])
            if zr is None or zt is None:
                sans_z = True
                continue
            if piece == "talus" and larg > 0.01:
                nrm = cote_bas(geoms.get(tr["terrain"]), xy,
                               normale_moyenne(xy))
                xy2 = xy + nrm * larg
                # le PIED du talus suit la loi du terrain a SA position
                zt2 = ze.z(tr["terrain"], xy2[:, 0], xy2[:, 1])
                if zt2 is None:
                    zt2 = zt
                V = np.concatenate([
                    np.column_stack([xy[:, 0], xy[:, 1], zr]),
                    np.column_stack([xy2[:, 0], xy2[:, 1], zt2])])
                m = len(xy)
                k = np.arange(m - 1)
                TT = np.concatenate([
                    np.stack([k, k + m, k + m + 1], 1),
                    np.stack([k, k + m + 1, k + 1], 1)])
            else:
                V, TT = quad_strip(xy, zr, zt)
                if V is None:
                    ct.saut("face de terrassement non construite (bande "
                            "degeneree)", cle_t)
                    continue
            T_ter.ajouter(V, TT, fid, cle_t)
        if sans_z:
            stats["terrassements_sans_z"] += 1
            ct.saut("terrassement dont un cote n'a pas de loi de Z evaluable",
                    cle_t)
        if T_ter.nt == n0:
            ct.saut("terrassement sans aucune face produite", cle_t)
        stats["terr_" + piece] += 1

    # ------------------------------------------------- LA VEGETATION --------
    inst = sm.get("instances") or []
    veg = None
    if inst:
        X = np.array([e["x"] for e in inst], dtype=np.float64)
        Y = np.array([e["y"] for e in inst], dtype=np.float64)
        S = np.array([e.get("scale") or 1.0 for e in inst], dtype=np.float32)
        R = np.array([e.get("yaw") or 0.0 for e in inst], dtype=np.float32)
        Z = np.full(len(X), np.nan)
        ids = [p["id"] for p in par]
        gl = [geoms[p] for p in ids if geoms.get(p) is not None]
        il = [p for p in ids if geoms.get(p) is not None]
        if gl:
            tree = STRtree(gl)
            pts = shapely.points(X, Y)
            gi, pi = tree.query(pts, predicate="within")
            # gi = indice du point, pi = indice de la geometrie
            for kk in range(len(gi)):
                ip, ig = int(gi[kk]), int(pi[kk])
                if np.isnan(Z[ip]):
                    zz = ze.z_loi(lois[il[ig]], X[ip:ip + 1], Y[ip:ip + 1])
                    if zz is not None:
                        Z[ip] = zz[0]
        manq = np.isnan(Z)
        if manq.any():
            Z[manq] = ze._sol().z(X[manq], Y[manq])
            stats["veg_hors_parcelle"] += int(manq.sum())
            for _ in range(int(manq.sum())):
                ct.saut("cote de vegetation prise au lecteur de drapage")
        veg = {"n": int(len(X)),
               "m": b64(np.column_stack([X, Y, Z]).astype(np.float32)),
               "s": b64(S), "y": b64(R)}
        stats["instances"] += len(X)

    couches = {}
    for nom, tp in (("sol", T_sol), ("eau", T_eau), ("ouvr", T_ouv),
                    ("bati", T_bat), ("itf", T_itf), ("terr", T_ter)):
        pk = tp.paquet()
        if pk is not None:
            couches[nom] = pk
    if itf_dz:
        couches.setdefault("itf", {})
        couches["itf"]["dz"] = b64(np.asarray(itf_dz, dtype=np.float32))
        couches["itf"]["cat"] = b64(np.asarray(itf_cat, dtype=np.uint8))
    if veg:
        couches["veg"] = veg

    noms = [None] * len(table)
    for s, i in table.items():
        noms[i] = s
    # --- LES FICHES : ce que le clic doit rendre (loi + provenance) ---------
    for p in par:
        pid = p["id"]
        if pid not in table:
            continue
        lo = p["loi"]
        f = {"t": "parcelle", "fam": fams[pid], "pro": p["proprietaire"],
             "mat": p["matiere"], "forme": lo.get("forme"),
             "a": p.get("aire_m2"), "at": p.get("aire_totale_m2"),
             "cel": p.get("cellule_porteuse"), "ent": p.get("entiere")}
        if lo.get("forme") == "constante":
            f["z"] = lo.get("z_m")
        elif lo.get("forme") == "profil_troncon":
            f["L"] = lo.get("L_m")
            f["pente"] = lo.get("pente_max_pc")
            pr = lo.get("profil") or []
            if pr:
                f["z"] = round(0.5 * (pr[0][1] + pr[-1][1]), 3)
        else:
            f["z"] = None
        if lo.get("loi_heritee_de"):
            f["her"] = lo["loi_heritee_de"]
        if p.get("provenance"):
            f["prov"] = p["provenance"]
        if p.get("largeur_m") is not None:
            f["larg"] = p["largeur_m"]
        a = assiettes.get(pid)
        if a:
            f["ass"] = [a.get("z_m"), a.get("cote"), a.get("provenance")]
        fiches[pid] = f
    for i in it["interfaces"]:
        k = i["a"] + "|" + i["b"]
        if k in table and k not in fiches:
            fiches[k] = {"t": "interface", "res": i.get("resolution"),
                         "dz": i.get("dz_m"), "dzx": i.get("dz_max_m"),
                         "h": i.get("h_m"), "m": i.get("longueur_m"),
                         "mt": i.get("matieres"),
                         "a": i["a"], "b": i["b"]}
    for tr in ct.terrassements(cle, qui=q, itf=it):
        k = tr["regle"] + "|" + tr["terrain"]
        if k in table and fiches.get(k, {}).get("t") != "terrassement":
            fiches.setdefault(k, {})
            fiches[k] = {"t": "terrassement", "piece": tr.get("piece"),
                         "dz": tr.get("dz_m"), "larg": tr.get("largeur_m"),
                         "m": tr.get("longueur_m"),
                         "place": tr.get("place_disponible_pc"),
                         "a": tr["regle"], "b": tr["terrain"],
                         "fam": tr.get("famille")}
    charge = {"c": cle, "o": [ox, oy], "s": C, "ids": noms, "L": couches,
              "F": fiches,
              "intrados": {k: round(v, 3) for k, v in intrados.items()}}
    p = os.path.join(CELLS, "mq_%s.js" % cle)
    with io.open(p, "w", encoding="ascii") as f:
        f.write("MQ_CELLULE(")
        json.dump(charge, f, separators=(",", ":"))
        f.write(");\n")
    o = os.path.getsize(p)
    stats["octets"] += o
    stats["secondes_cellules"] += time.time() - t_c
    return {"cellule": cle, "octets": o,
            "triangles": sum(t.nt for t in (T_sol, T_eau, T_ouv, T_bat,
                                            T_itf, T_ter)),
            "sommets": sum(t.n for t in (T_sol, T_eau, T_ouv, T_bat,
                                         T_itf, T_ter)),
            "instances": int(veg["n"]) if veg else 0,
            "secondes": round(time.time() - t_c, 2)}


def main():
    t0 = time.time()
    for d in (WEB, CELLS):
        if not os.path.isdir(d):
            os.makedirs(d)
    args = sys.argv[1:]
    seules = None
    if "--cellules" in args:
        seules = args[args.index("--cellules") + 1].split(",")
    strict = "--contrat-seul" in args

    ct = Contrat(strict=strict)
    jalon("contrat charge (%.1f s) : %d cellules, mode %s"
          % (ct.secondes_chargement, len(ct.cells),
             "CONTRAT SEUL (aucun repli tolere)" if strict
             else ("repli sur instantane autorise" if ct.snap
                   else "sans instantane")))
    # LA GEOMETRIE DE MARCHE VIENT DU REGISTRE DU CONTRAT (plus de recopie)
    reg = Registre()
    mar = reg.marche()
    if mar:
        PARAMS["MARCHE_H_MAX_M"] = mar["h_max_m"]
        PARAMS["MARCHE_GIRON_MIN_M"] = mar["giron_min_m"]
        PARAMS["MARCHE_BLONDEL_M"] = mar["blondel_m"]
        PARAMS["MARCHE_note"] = (
            "LU dans registre::geometrie_marche DU CONTRAT (md5 %s) : %s"
            % (reg.md5, mar["reference"]))
    jalon("registre du contrat lu : %d regles, md5 %s (empreinte verifiee "
          "contre plan_index.json) ; geometrie_marche h<=%.2f giron>=%.2f "
          "Blondel %s" % (reg.compte_reel, reg.md5, PARAMS["MARCHE_H_MAX_M"],
                          PARAMS["MARCHE_GIRON_MIN_M"],
                          PARAMS["MARCHE_BLONDEL_M"]))
    ze = ZEval(ct)
    chrono("index des lois", ze.secondes_index, "%d parcelles" % len(ze.lois))

    cells = seules or ct.cells
    index_lignes(ct, cells)
    stats = Counter()
    fiches = []
    t1 = time.time()
    for k, c in enumerate(cells):
        r = construire_cellule(ct, ze, c, stats)
        fiches.append(r)
        if (k + 1) % 8 == 0 or k + 1 == len(cells):
            jalon("maquette %d/%d cellules — %.1f Mtri, %.1f Mo, %.0f s"
                  % (k + 1, len(cells),
                     sum(f["triangles"] for f in fiches) / 1e6,
                     stats["octets"] / 1e6, time.time() - t1))
    chrono("construction geometrie", time.time() - t1,
           "%d cellules" % len(cells))

    # ------------------------------------------------------- L'INDEX --------
    tour = ct.juges["tournee"]
    idx = {
        "plan": ct.plan["version"],
        "produit_par": "work/PLAN/maquette/mq_build.py",
        "registre": {"source": "CONTRAT data/registre.json", "md5": reg.md5,
                     "regles": reg.compte_reel,
                     "empreinte_verifiee_contre": "plan_index.json::fichiers"},
        "params_maquette": PARAMS,
        "familles": FAMILLES,
        "catalogue": CATALOGUE,
        "cellule_m": ct.cell_m,
        "cellules": [{"c": f["cellule"],
                      "o": [int(f["cellule"].split("_")[0]) * ct.cell_m,
                            int(f["cellule"].split("_")[1]) * ct.cell_m],
                      "t": f["triangles"], "v": f["sommets"],
                      "i": f["instances"], "ko": round(f["octets"] / 1024)}
                     for f in fiches],
        "signets": tour,
        "totaux": {
            "triangles": int(sum(f["triangles"] for f in fiches)),
            "sommets": int(sum(f["sommets"] for f in fiches)),
            "instances": int(sum(f["instances"] for f in fiches)),
            "octets": int(stats["octets"]),
            "parcelles": int(stats["parcelles"]),
            "batiments": int(stats["batiments"]),
            "ouvrages": int(stats["ouvrages"]),
            "marches_generees": int(stats["marches_generees"]),
        },
        "stats": dict(stats),
        "provenance": {"du_contrat": ct.du_contrat, "repli_instantane": ct.repli,
                       "contrat_complet": (not ct.repli)},
        "sauts": ct.bilan_sauts(),
        "solveur": ct.solveur,
        "matrice": {"compteurs": ct.matrice["compteurs_mesure"],
                    "non_mesurables": ct.matrice["non_mesurables"]},
    }
    p = os.path.join(WEB, "mq_index.js")
    with io.open(p, "w", encoding="ascii") as f:
        f.write("var MQ_INDEX=")
        json.dump(idx, f, separators=(",", ":"))
        f.write(";\n")
    with io.open(os.path.join(MQ, "mq_stats.json"), "w", encoding="utf-8") as f:
        json.dump({"totaux": idx["totaux"], "stats": dict(stats),
                   "cellules": fiches, "secondes": round(time.time() - t0, 1)},
                  f, ensure_ascii=False, indent=1, sort_keys=True)
    jalon("MAQUETTE ECRITE : %d triangles, %d instances, %.1f Mo sur %d cellules"
          " en %.0f s" % (idx["totaux"]["triangles"], idx["totaux"]["instances"],
                          stats["octets"] / 1e6, len(cells), time.time() - t0))
    chrono("mq_build total", time.time() - t0, "%d cellules" % len(cells))

    # ------------------------------------------------------- LE VERROU ------
    # Toute piece sautee, quelle qu'en soit la raison, est COMPTEE et LISTEE.
    # `--contrat-seul` n'accepte que le zero : soit rien n'a ete saute, soit
    # le saut est une REPONSE du contrat (`rien`, cote de drapage designee).
    bilan = ct.bilan_sauts()
    with io.open(os.path.join(MQ, "mq_sauts.json"), "w", encoding="utf-8") as f:
        json.dump({"produit_par": "work/PLAN/maquette/mq_build.py",
                   "objet": "toute piece que le lecteur n'a PAS construite",
                   "mode": "--contrat-seul" if strict else "repli autorise",
                   "cellules": len(cells), "bilan": bilan},
                  f, ensure_ascii=False, indent=1, sort_keys=True)
    jalon("SAUTS : %d pieces sautees dont %d PERDUES (%d motifs de perte) — "
          "detail dans mq_sauts.json"
          % (bilan["pieces_sautees_total"], bilan["pieces_perdues"],
             bilan["motifs_de_perte"]))
    print(json.dumps(dict(stats), ensure_ascii=False, sort_keys=True))
    perdus = ct.sauts_perdus()
    if perdus:
        for m, v in sorted(perdus.items(), key=lambda kv: -kv[1]["n"]):
            print("  PERDU x%-6d %s  ex: %s"
                  % (v["n"], m, ", ".join(v["ids"][:4]) or "-"))
    if strict and perdus:
        jalon("VERROU : REFUS — %d pieces PERDUES en mode --contrat-seul"
              % bilan["pieces_perdues"])
        print("REFUS : --contrat-seul exige zero piece perdue ; il en reste %d."
              % bilan["pieces_perdues"])
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
