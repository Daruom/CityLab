# -*- coding: utf-8 -*-
"""PLAN DE VILLE / etape B (socle) — LA COUCHE ① QUI : LES PARCELLES.

La carte v2.1 publie les BANDES et les FRONTIERES zone|organique, mais PAS les
polygones des 4 proprietaires (choix explicite de p31 : « le C++ a deja ses
sources »). Le PLAN, lui, doit les porter : on les RE-DERIVE ici des MEMES
sources que le C++, avec la preseance arbitree du lot partition :

        ouvrage > voirie > batiment > zone > organique (defaut)

Une PARCELLE = une piece connexe d'UN objet source apres preseance. Les objets
d'une meme classe qui se recouvrent (les tampons de voirie aux carrefours) sont
departages par ordre d'index : la partition est DISJOINTE par construction.

Completion des ouvrages (dette ③ du doc maitre, « 36 escaliers sans emprise ») :
l'emprise est derivee de l'axe du side-car et de sa LARGEUR — et cette largeur
est une DONNEE (`largeur_m`, BD TOPO), presente sur 36/36 escaliers et 98/98
ponts : aucune constante heritee n'est necessaire. Regle NATIONALE, sans
identifiant : emprise = axe tamponne de largeur_m/2, bouts francs.

Sortie : `cache/parcelles.pkl` (WKB) + `plan_ville/v1/qui.json` (resume + juges).
⛔ Rien n'est ecrit hors de work\\PLAN. SourceData est en lecture seule.
"""
import io
import json
import os
import pickle
import sys
import time

import numpy as np
import shapely
from shapely.geometry import LineString, Polygon, box
from shapely.ops import unary_union
from shapely.strtree import STRtree

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import (BLOC, CELL_M, ENT, OUT, PART, PLAN, PROPS, SRC, CACHE,
                      chrono, ecrire_json, jalon, md5_octets, charge_json)

AIRE_MIN_M2 = 1e-6          # une piece plus petite n'est pas une parcelle
BOUT = 2                    # cap_style=2 : bouts francs (comme p0_carte)

# --- constantes de l'ANNEXION E0-bis (work/PART/p5_snap.py, arbitrage
#     utilisateur du lot partition ; la carte v2.1 en porte le resultat) -------
BANDE_MAX_M = 3.0           # au-dela, une plage est du vrai terrain
COLLIER_M = 0.02            # epaisseur du collier de detection des voisins
SEUIL_INTERSTICE_M = 0.05   # le juge d'interstice
DUR = ["ouvrage", "voirie", "batiment"]


def largeur(g):
    """Largeur caracteristique 4A/P (p5_snap.py::largeur)."""
    p = g.length
    return (4.0 * g.area / p) if p > 1e-9 else 0.0


def valide(g):
    if g is None or g.is_empty:
        return Polygon()
    if not g.is_valid:
        g = g.buffer(0)
    return g


def eclate(g):
    if g is None or g.is_empty:
        return []
    if hasattr(g, "geoms"):
        return [q for q in g.geoms if q.geom_type == "Polygon" and q.area > 0]
    return [g] if g.geom_type == "Polygon" else []


# =============================================================== LES SOURCES ==
def sources():
    """Les objets sources, un par un, avec leur provenance."""
    emp, src = {}, {}

    # ① OUVRAGE — le bloc de berge + les ponts + les escaliers + les gradins
    p = os.path.join(BLOC, "bl2_emprise.wkt")
    emp["bl2_emprise_wkt"] = md5_octets(p)
    bloc = valide(shapely.from_wkt(io.open(p, encoding="utf-8").read()))
    ouv = []
    for i, g in enumerate(eclate(bloc)):
        ouv.append((g, {"src": "bl2_emprise.wkt", "nature": "bloc_berge"}))
    n_lg = {"pont": 0, "escalier": 0}
    for nom, cle, nat in (("Ponts", "ponts", "pont"),
                          ("Escaliers", "escaliers", "escalier"),
                          ("Gradins", "gradins", "gradin")):
        d = os.path.join(SRC, nom)
        for f in sorted(os.listdir(d)):
            if not f.endswith(".json"):
                continue
            j = charge_json(os.path.join(d, f))
            for it in (j.get(cle) or []):
                pts = [(float(q[0]), float(q[1])) for q in (it.get("pts") or [])]
                if len(pts) < 2:
                    continue
                lg = it.get("largeur_m")
                if nat == "gradin":
                    g = valide(Polygon(pts)) if len(pts) >= 3 else Polygon()
                    meta = {"src": nom, "nature": nat, "id": it.get("id"),
                            "emprise": "polygone du side-car"}
                elif lg:
                    g = valide(LineString(pts).buffer(0.5 * float(lg),
                                                      cap_style=BOUT))
                    n_lg[nat if nat in n_lg else "pont"] += 1
                    meta = {"src": nom, "nature": nat, "id": it.get("id"),
                            "largeur_m": float(lg),
                            "emprise": "axe tamponne de largeur_m/2 "
                                       "(regle nationale, largeur = donnee "
                                       "BD TOPO du side-car)"}
                else:
                    continue
                if not g.is_empty:
                    ouv.append((g, meta))
    src["ouvrage"] = ouv

    # ② VOIRIE — le graphe routier fait autorite (largeur `w` du graphe)
    p = os.path.join(ENT, "routes_3x3.json")
    emp["routes_3x3_json"] = md5_octets(p)
    R = charge_json(p)
    voi = []
    for i, r in enumerate(R.get("roads", [])):
        pts = r.get("pts") or []
        if len(pts) < 2:
            continue
        w = float(r.get("w") or 6.0)
        try:
            g = valide(LineString([(float(q[0]), float(q[1])) for q in pts])
                       .buffer(max(0.5, 0.5 * w), cap_style=BOUT))
        except Exception:
            continue
        if not g.is_empty:
            voi.append((g, {"src": "routes_3x3.json", "i": i, "w_m": w,
                            "t": r.get("t"), "surface": r.get("surface"),
                            "axe": [[round(float(q[0]), 3),
                                     round(float(q[1]), 3)] for q in pts]}))
    src["voirie"] = voi

    # ③ BATIMENT — les emprises
    p = os.path.join(ENT, "bati_3x3.json")
    emp["bati_3x3_json"] = md5_octets(p)
    B = charge_json(p)
    bat = []
    for i, b in enumerate(B.get("buildings", [])):
        pts = b.get("pts") or []
        if len(pts) < 3:
            continue
        try:
            g = valide(Polygon([(float(u[0]), float(u[1])) for u in pts]))
        except Exception:
            continue
        if not g.is_empty:
            bat.append((g, {"src": "bati_3x3.json", "i": i,
                            "h_m": b.get("h"), "u": b.get("u")}))
    src["batiment"] = bat

    # ④ ZONE — OCS GE (couverture du sol)
    p = os.path.join(SRC, "ocsge_verts.json")
    emp["ocsge_verts_json"] = md5_octets(p)
    Z = charge_json(p)
    zon = []
    for i, z in enumerate(Z.get("cs2", [])):
        pts = z.get("pts") or []
        if len(pts) < 4:
            continue
        try:
            g = valide(Polygon([(float(u[0]), float(u[1])) for u in pts],
                               [[(float(v[0]), float(v[1])) for v in h]
                                for h in (z.get("holes") or [])]))
        except Exception:
            continue
        if not g.is_empty:
            zon.append((g, {"src": "ocsge_verts.json", "i": i,
                            "cs": z.get("cs"), "us": z.get("us")}))
    src["zone"] = zon
    jalon("C1/SOURCES : %d ouvrages (dont %d ponts et %d escaliers a emprise "
          "derivee de l'axe x largeur_m), %d troncons de voirie, %d emprises de "
          "batiment, %d polygones OCS GE"
          % (len(ouv), n_lg["pont"], n_lg["escalier"], len(voi), len(bat),
             len(zon)))
    return src, emp


# ============================================================== LA PARTITION ==
def partition(src, DOM):
    """Preseance entre classes + depart d'ex-aequo intra-classe par index."""
    parcelles = []
    pris = []                       # unions cumulees des classes superieures
    PRIS = Polygon()
    stats = {}
    for k in PROPS[:-1]:
        t0 = time.time()
        objs = src[k]
        geoms = [g for g, _ in objs]
        T = STRtree(geoms) if geoms else None
        shapely.prepare(PRIS)
        n_ok = 0
        aire = 0.0
        for i, (g, meta) in enumerate(objs):
            g = valide(g.intersection(DOM))
            if g.is_empty or g.area <= AIRE_MIN_M2:
                continue
            if not PRIS.is_empty and PRIS.intersects(g):
                g = valide(g.difference(PRIS))
            # ex-aequo intra-classe : le plus petit index garde le sol
            vois = [int(j) for j in T.query(g)] if T is not None else []
            av = [geoms[j] for j in vois if j < i]
            if av:
                u = unary_union(av)
                if u.intersects(g):
                    g = valide(g.difference(u))
            if g.is_empty or g.area <= AIRE_MIN_M2:
                continue
            for n, q in enumerate(eclate(g)):
                if q.area <= AIRE_MIN_M2:
                    continue
                parcelles.append({"id": "%s/%d#%d" % (k[:3], i, n),
                                  "proprietaire": k, "geom": q, "meta": meta})
                aire += q.area
                n_ok += 1
        gl = valide(unary_union([g for g, _ in objs])) if objs else Polygon()
        gl = valide(gl.intersection(DOM))
        PRIS = valide(unary_union([PRIS, gl])) if not PRIS.is_empty else gl
        stats[k] = {"objets": len(objs), "parcelles": n_ok,
                    "aire_m2": round(aire, 1)}
        chrono("C1/" + k, time.time() - t0,
               "%d parcelles, %.0f m2" % (n_ok, aire))
        jalon("C1/PARTITION %-9s : %d objets -> %d parcelles, %.0f m2"
              % (k, len(objs), n_ok, aire))

    t0 = time.time()
    ORG = valide(DOM.difference(PRIS))
    plages = [q for q in eclate(ORG) if q.area > AIRE_MIN_M2]
    chrono("C1/organique", time.time() - t0, "%d plages" % len(plages))

    # ---- ANNEXION E0-bis : la regle arbitree du lot partition, rejouee -------
    t0 = time.time()
    arch = [p for p in parcelles]          # les parcelles architecturales
    T = STRtree([p["geom"] for p in arch])
    n_bande = {"ouvrage": 0, "voirie": 0, "batiment": 0, "zone": 0}
    m_bande = {"ouvrage": 0.0, "voirie": 0.0, "batiment": 0.0, "zone": 0.0}
    n_large = n_sansvoisin = 0
    aire_org = 0.0
    n_org = 0
    for n, q in enumerate(plages):
        L = largeur(q)
        cible = None
        if L <= BANDE_MAX_M:
            col = q.buffer(COLLIER_M).difference(q)
            vois, contact = set(), {}
            for j in T.query(col):
                j = int(j)
                g = arch[j]["geom"]
                if not g.intersects(col):
                    continue
                k = arch[j]["proprietaire"]
                vois.add(k)
                if k == "zone":
                    try:
                        contact[j] = contact.get(j, 0.0) + \
                            col.intersection(g).area
                    except Exception:
                        pass
            for k in DUR:
                if k in vois:
                    cible = k
                    motif = "voisin dur (preseance)"
                    break
            if cible is None and "zone" in vois:
                cible = "zone"
                motif = "aucun dur ; zone au plus long contact"
            if cible is None:
                n_sansvoisin += 1
        else:
            n_large += 1
        if cible is not None:
            n_bande[cible] += 1
            m_bande[cible] += q.area
            parcelles.append({
                "id": "bnd/%d#0" % n, "proprietaire": cible, "geom": q,
                "meta": {"src": "annexion E0-bis (p5_snap.py)",
                         "bande": True, "largeur_m": round(L, 4),
                         "motif": motif,
                         "heritee": True,
                         "provenance": "work/PART/p5_snap.py:36-39 "
                                       "(BANDE_MAX_M=3.0, COLLIER_M=0.02, "
                                       "preseance dur, voisin unique annexe)"}})
        else:
            parcelles.append({"id": "org/%d#0" % n, "proprietaire": "organique",
                              "geom": q, "meta": {"src": "complement du domaine",
                                                  "largeur_m": round(L, 4)}})
            aire_org += q.area
            n_org += 1
    stats["organique"] = {"objets": n_org, "parcelles": n_org,
                          "aire_m2": round(aire_org, 1)}
    for k in n_bande:
        stats[k]["bandes_annexees_n"] = n_bande[k]
        stats[k]["bandes_annexees_m2"] = round(m_bande[k], 1)
        stats[k]["parcelles"] += n_bande[k]
        stats[k]["aire_m2"] = round(stats[k]["aire_m2"] + m_bande[k], 1)
    chrono("C1/annexion", time.time() - t0,
           "%d bandes" % sum(n_bande.values()))
    jalon("C1/⭐ ANNEXION E0-bis rejouee (regle p5_snap.py, largeur 4A/P <= "
          "%.1f m -> voisin dur par preseance, sinon zone au plus long "
          "contact) : %d plages examinees -> %d BANDES annexees (%s), %d "
          "plages larges laissees organiques, %d sans voisin architectural ; "
          "il reste %d parcelles organiques (%.0f m2)"
          % (BANDE_MAX_M, len(plages), sum(n_bande.values()),
             ", ".join("%s %d/%.0f m2" % (k, n_bande[k], m_bande[k])
                       for k in n_bande if n_bande[k]),
             n_large, n_sansvoisin, n_org, aire_org))
    return parcelles, stats


# ================================================ DISJONCTION (reparation) ====
def disjoindre(parcelles):
    """Garde de DISJONCTION. La preseance nationale tranche : a recouvrement,
    c'est la parcelle de rang INFERIEUR qui cede (ouvrage > voirie > batiment >
    zone > organique ; a rang egal, le plus grand index cede).

    Necessaire parce que GEOS peut rendre un `difference` incomplet sur des
    tampons quasi-colineaires (cas mesure : voi/2906 et voi/4504 partagent leur
    premier sommet ; `difference` laisse 78,52 m2 en double propriete alors que
    le meme calcul refait isolement rend 0). On ne fait donc pas CONFIANCE au
    calcul : on VERIFIE et on repare."""
    t0 = time.time()
    rang = {k: i for i, k in enumerate(PROPS)}
    geoms = [p["geom"] for p in parcelles]
    T = STRtree(geoms)
    n_rep, a_rep = 0, 0.0
    for i, g in enumerate(geoms):
        for j in T.query(g):
            j = int(j)
            if j <= i:
                continue
            try:
                inter = geoms[i].intersection(geoms[j])
            except Exception:
                continue
            if inter.is_empty or inter.area <= 1e-9:
                continue
            ri, rj = rang[parcelles[i]["proprietaire"]], \
                rang[parcelles[j]["proprietaire"]]
            perd = j if (ri < rj or (ri == rj and i < j)) else i
            geoms[perd] = valide(geoms[perd].difference(inter))
            parcelles[perd]["geom"] = geoms[perd]
            parcelles[perd].setdefault("meta", {})["disjonction_reparee_m2"] = \
                round(parcelles[perd]["meta"].get("disjonction_reparee_m2", 0.0)
                      + inter.area, 6)
            n_rep += 1
            a_rep += inter.area
    parcelles[:] = [p for p in parcelles
                    if not p["geom"].is_empty and p["geom"].area > AIRE_MIN_M2]
    chrono("C1/disjonction", time.time() - t0, "%d reparations" % n_rep)
    jalon("C1/DISJONCTION : %d recouvrement(s) repare(s) par la preseance "
          "nationale, %.6f m2 rendus au proprietaire de rang superieur"
          % (n_rep, a_rep))
    return {"reparations_n": n_rep, "reparations_m2": round(a_rep, 6)}


# ================================================================ LES JUGES ===
def combler(parcelles, cells):
    """Garde de COUVERTURE. Meme motif que la garde de disjonction : un
    `difference` GEOS incomplet peut laisser un TROU (aucun proprietaire).
    On le mesure cellule par cellule et on le comble par la MEME regle
    d'annexion E0-bis — sans quoi la couverture n'est pas 100 %."""
    t0 = time.time()
    geoms = [p["geom"] for p in parcelles]
    T = STRtree(geoms)
    n, a = 0, 0.0
    ajouts = []
    for cx, cy in cells:
        bx = box(cx * CELL_M, cy * CELL_M, (cx + 1) * CELL_M, (cy + 1) * CELL_M)
        loc = [geoms[int(j)] for j in T.query(bx)]
        if not loc:
            continue
        R = valide(bx.difference(valide(unary_union(loc))))
        for q in eclate(R):
            if q.area <= AIRE_MIN_M2:
                continue
            ajouts.append(q)
            n += 1
            a += q.area
    ajouts.sort(key=lambda q: -q.area)
    for q in ajouts[:5]:
        c = q.representative_point()
        jalon("C1/  TROU (%.1f ; %.1f) cellule %d_%d : %.6f m2, largeur 4A/P "
              "%.4f m" % (c.x, c.y, c.x // CELL_M, c.y // CELL_M, q.area,
                          largeur(q)))
    for i, q in enumerate(ajouts):
        L = largeur(q)
        cible = None
        col = q.buffer(COLLIER_M).difference(q)
        vois, contact = set(), {}
        for j in T.query(col):
            j = int(j)
            if not geoms[j].intersects(col):
                continue
            k = parcelles[j]["proprietaire"]
            vois.add(k)
            if k == "zone":
                try:
                    contact[j] = contact.get(j, 0.0) + col.intersection(
                        geoms[j]).area
                except Exception:
                    pass
        if L <= BANDE_MAX_M:
            for k in DUR:
                if k in vois:
                    cible = k
                    break
            if cible is None and "zone" in vois:
                cible = "zone"
        parcelles.append({"id": "tro/%d#0" % i,
                          "proprietaire": cible or "organique", "geom": q,
                          "meta": {"src": "comblement d'un trou de calcul",
                                   "largeur_m": round(L, 4),
                                   "trou_comble": True}})
    chrono("C1/comblement", time.time() - t0, "%d trous" % n)
    jalon("C1/COMBLEMENT : %d trou(s) de calcul mesure(s) cellule par cellule, "
          "%.6f m2 au total, rendus a un proprietaire par la regle d'annexion"
          % (n, a))
    return {"trous_n": n, "trous_m2": round(a, 6)}


def juges(parcelles, DOM, seuil_interstice_m=SEUIL_INTERSTICE_M):
    t0 = time.time()
    aire = sum(p["geom"].area for p in parcelles)
    cov = 100.0 * aire / DOM.area
    # INTERSTICE (metrique E0) : une plage restee ORGANIQUE dont la largeur
    # caracteristique 4A/P est sous le seuil arbitre de 5 cm.
    n_int, a_int = 0, 0.0
    for p in parcelles:
        if p["proprietaire"] != "organique":
            continue
        if largeur(p["geom"]) <= seuil_interstice_m:
            n_int += 1
            a_int += p["geom"].area
    # recouvrement residuel entre parcelles (la partition doit etre disjointe)
    geoms = [p["geom"] for p in parcelles]
    T = STRtree(geoms)
    a_rec = 0.0
    n_rec = 0
    for i, g in enumerate(geoms):
        for j in T.query(g):
            j = int(j)
            if j <= i:
                continue
            try:
                inter = g.intersection(geoms[j])
            except Exception:
                continue
            if inter.is_empty or inter.area <= 1e-9:
                continue
            a_rec += inter.area
            n_rec += 1
    J = {"couverture_pc": round(cov, 6),
         "domaine_m2": round(DOM.area, 1),
         "attribue_m2": round(aire, 1),
         "interstices_n": n_int,
         "interstices_m2": round(a_int, 3),
         "interstices_seuil_m": seuil_interstice_m,
         "recouvrements_n": n_rec,
         "recouvrements_m2": round(a_rec, 6),
         "parcelles_n": len(parcelles)}
    chrono("C1/juges", time.time() - t0, "")
    jalon("C1/⭐ JUGES E0 REJOUES : couverture %.6f %% (%.0f / %.0f m2) | "
          "%d parcelles | interstices (< %.0f cm de large) : %d plages, "
          "%.3f m2 | recouvrements entre parcelles : %d paires, %.6f m2"
          % (cov, aire, DOM.area, len(parcelles), 100 * seuil_interstice_m,
             n_int, a_int, n_rec, a_rec))
    return J


def domaine():
    """LE DOMAINE = celui de la CARTE v2.1 (source officielle) : l'ensemble des
    cellules citees par ses bandes et ses frontieres. La liste des maillages
    `SM_Ground_*.bin` sur le disque (regle de p0_carte.py) sert de CONTRE-
    MESURE : elle a ete reecrite le 07/08 a 16:59, apres la carte (15:18), et
    porte 3 cellules de plus. Le plan ne s'y cale PAS ; l'ecart est chiffre."""
    carte = charge_json(os.path.join(SRC, "Partition", "carte_v2.json"))
    cc = set()
    for cle in ("bandes", "frontieres"):
        for it in carte.get(cle, []):
            cc.add((int(it["cellule"][0]), int(it["cellule"][1])))
    cells = sorted(cc)
    from c0_socle import cellules_domaine
    disque = sorted(set(cellules_domaine()))
    hors = sorted(set(disque) - cc)
    manq = sorted(cc - set(disque))
    return cells, carte, disque, hors, manq


def main():
    t0 = time.time()
    cells, carte, disque, hors, manq = domaine()
    DOM = valide(unary_union([box(cx * CELL_M, cy * CELL_M, (cx + 1) * CELL_M,
                                  (cy + 1) * CELL_M) for cx, cy in cells]))
    jalon("C1/DOMAINE = CELUI DE LA CARTE v2.1 : %d cellules de %.0f m, "
          "%.4f km2. Contre-mesure disque (SM_Ground_*.bin de BLOC/mesh_vis, "
          "reecrits le 07/08 16:59) : %d cellules, %d hors carte %s, %d "
          "manquantes %s — le plan se cale sur la CARTE, l'ecart part au "
          "rapport." % (len(cells), CELL_M, DOM.area / 1e6, len(disque),
                        len(hors), ["%d_%d" % c for c in hors], len(manq),
                        ["%d_%d" % c for c in manq]))
    src, emp = sources()
    parcelles, stats = partition(src, DOM)
    D = disjoindre(parcelles)
    C = combler(parcelles, cells)
    D2 = disjoindre(parcelles)
    J = juges(parcelles, DOM)
    J.update(D)
    J.update(C)
    J["reparations_2e_passe_n"] = D2["reparations_n"]

    # empreintes de la carte v2.1 : on VERIFIE qu'on lit les memes octets
    ref = carte.get("empreinte_sources", {})
    acc = {k: (ref.get(k), emp.get(k), ref.get(k) == emp.get(k))
           for k in sorted(set(list(ref.keys()) + list(emp.keys())))}
    ok = all(v[2] for v in acc.values() if v[0] is not None)
    jalon("C1/EMPREINTES SOURCES (md5 en toutes lettres, carte v2.1 -> lu par "
          "le plan) : %s"
          % (" ; ".join("%s carte=%s plan=%s %s"
                        % (k, v[0] or "ABSENT", v[1] or "ABSENT",
                           "IDENTIQUE" if v[2] else "DIVERGENT")
                        for k, v in acc.items())))

    with open(os.path.join(CACHE, "parcelles.pkl"), "wb") as f:
        pickle.dump({"cells": cells,
                     "dom": shapely.to_wkb(DOM),
                     "parcelles": [{"id": p["id"],
                                    "proprietaire": p["proprietaire"],
                                    "geom": shapely.to_wkb(p["geom"]),
                                    "meta": p["meta"]} for p in parcelles]},
                    f, protocol=4)
    rep = {"couche": "QUI",
           "domaine_cellules": ["%d_%d" % c for c in cells],
           "domaine_m2": round(DOM.area, 1),
           "domaine_source": "cellules citees par carte_v2.json (bandes + "
                             "frontieres) — la carte fait autorite",
           "domaine_ecart_disque": {
               "cellules_disque_n": len(disque),
               "hors_carte": ["%d_%d" % c for c in hors],
               "manquantes_sur_disque": ["%d_%d" % c for c in manq]},
           "preseance": PROPS,
           "annexion": {"regle": "E0-bis (work/PART/p5_snap.py)",
                        "BANDE_MAX_M": BANDE_MAX_M, "COLLIER_M": COLLIER_M,
                        "preseance_dur": DUR, "heritee": True},
           "par_proprietaire": stats,
           "juges": J,
           "empreintes_sources": {k: v[1] for k, v in acc.items()},
           "empreintes_sources_carte_v21": {k: v[0] for k, v in acc.items()},
           "empreintes_identiques": ok,
           "regle_emprise_ouvrage": "axe du side-car tamponne de largeur_m/2, "
                                    "bouts francs (largeur_m = donnee BD TOPO)"}
    ecrire_json(os.path.join(OUT, "qui.json"), rep)
    chrono("C1 TOTAL", time.time() - t0, "%d parcelles" % len(parcelles))
    jalon("C1 terminee (%.1f s) — cache/parcelles.pkl + plan_ville/v1/qui.json"
          % (time.time() - t0))


if __name__ == "__main__":
    main()
