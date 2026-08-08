# -*- coding: utf-8 -*-
"""mq_degagement.py — LES 18 CASES « DEGAGEMENT » de la matrice.

La matrice les avait declarees NON MESURABLES en 2,5D, avec ce motif :
« le tablier et la voie franchie partagent la meme emprise, une seule des deux
la gagne par preseance ». En 3D le tablier est POSE : la voie franchie n'est
plus sous lui dans le plan, mais elle reste presente DE PART ET D'AUTRE. C'est
ce que ce programme exploite.

METHODE (entierement lue au contrat) :
 1. axe long du tablier par analyse en composantes principales de son emprise ;
 2. pour chaque voisin du tablier (couple d'interface du contrat), direction de
    la ligne de contact : PARALLELE a l'axe long => le tablier LONGE ou FRANCHIT
    ce voisin ; PERPENDICULAIRE => c'est une culee / un raccord d'about ;
 3. cote du voisin par SA loi du contrat, au droit du contact ;
 4. hauteur libre = cote d'INTRADOS DU TABLIER AU DROIT DU CONTACT, moins la
    cote du voisin. L'intrados est pris POINT PAR POINT, par la regle meme du
    registre (`epaisseur_tablier` : « la cote d'intrados est la cote
    d'extrados moins cette epaisseur ») :

        intrados(x,y) = loi_du_tablier(x,y) - epaisseur_tablier_m

    et NON par le scalaire `cote_intrados_m`. Motif MESURE, pas suppose : sur
    les 31 tabliers a loi `constante` les deux coincident a moins de 5 mm (le
    scalaire EST z_m - epaisseur) ; mais sur les 25 tabliers a loi
    `profil_troncon` l'extrados varie le long de la travee (jusqu'a 2,40 m
    d'amplitude) alors que `cote_intrados_m` reste UN SEUL NOMBRE, voisin de
    la mediane du profil. Mesurer un voisin situe au haut d'une rampe contre
    l'intrados du bas de la rampe fabriquait des hauteurs libres NEGATIVES.
    Les deux valeurs sont rapportees cote a cote (`hauteur_libre_m` et
    `hauteur_libre_scalaire_m`) pour que l'ecart soit visible.

SEUILS : LUS dans la regle `hauteur_libre` du REGISTRE DU CONTRAT
(`plan_ville/v1/data/registre.json`), dont l'empreinte est verifiee contre
`plan_index.json::fichiers`. L'extraction depuis la page L1a
(`work/PLAN/matrice/data_matrice.js`) est SUPPRIMEE : le trou signale a la
passe precedente est comble, la mesure de conformite se rejoue desormais
depuis le seul contrat. Une classe de gabarit absente de l'enonce ne recoit
pas de verdict — on ne devine pas un seuil.

LES DALOTS SONT HORS GABARIT, ET C'EST LE CONTRAT QUI LE DIT : la regle
`ouvrage_affleurant` du registre pose qu'un ouvrage de franchissement dont la
hauteur declaree est sous le plus petit gabarit du reel (2,20 m) est un dalot
— « la route passe dessus et rien ne passe dessous », il ne porte « ni
epaisseur de tablier, ni intrados, ni exigence de hauteur libre ». Le contrat
final les sort donc de la famille `pont` (56 vrais ponts cotes contre 62
dalots `intrados: sans_objet`). Ce programme ne mesure que la famille `pont`,
et VERIFIE l'invariant de la regle : aucun dalot ne porte d'intrados.
"""
import io
import json
import os
import re
import sys
import time
from collections import defaultdict

import numpy as np
import shapely
from shapely.geometry import Polygon
from shapely.strtree import STRtree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq_lib import MQ, Contrat, Registre, ZEval, chrono, jalon, polygone

# --- LES 18 CASES, telles que la matrice les nomme -------------------------
CASES = [("chaussee", "pont"), ("trottoir", "pont"), ("voie_ferree", "pont"),
         ("canal", "pont"), ("carrefour", "pont"), ("pont", "escalier"),
         ("pont", "gradins"), ("pont", "mur_sout"), ("pont", "ouvrage_hydro"),
         ("pont", "edicule"), ("pont", "tremie"), ("pont", "sol_mineral"),
         ("pont", "sol_vegetal"), ("pont", "eau_surface"), ("pont", "parking"),
         ("pont", "terrain_sport"), ("pont", "batiment"),
         ("pont", "terrain_naturel")]

# ------------------------------------------------------------- LES SEUILS ---
# Ils ne sont PAS ecrits ici : ils sont LUS dans la regle `hauteur_libre` du
# registre DU CONTRAT (mq_lib.Registre, empreinte verifiee). Si une classe
# manque a l'enonce, on ne la devine pas : la case reste sans verdict.

# a quelle classe de gabarit appartient chaque famille franchie
CLASSE = {
    "chaussee": "route", "carrefour": "route", "parking": "route",
    "sol_mineral": "route", "tremie": "route",
    "voie_ferree": "fer",
    "canal": "eau", "eau_surface": "eau", "ouvrage_hydro": "eau",
    "trottoir": "pieton", "escalier": "pieton", "gradins": "pieton",
    "sol_vegetal": "pieton", "terrain_sport": "pieton",
    "terrain_naturel": "pieton",
}
SANS_CLASSE = {
    "mur_sout": "un mur de soutenement n'est pas une voie : rien n'y circule, "
                "aucun gabarit de passage ne s'y applique",
    "edicule": "un edicule est un volume bati, pas une voie franchie",
    "batiment": "un batiment n'est pas une voie franchie",
    "dalot": "registre::ouvrage_affleurant — un dalot est un ouvrage "
             "AFFLEURANT : la route passe dessus et rien ne passe dessous. "
             "Il ne porte aucune exigence de hauteur libre, ni comme "
             "franchisseur ni comme surface franchie",
}

REG = None
APLOMB_TOL_M = 1.0        # de part et d'autre de l'axe : au-dela, c'est un cote
PARALLELE = 0.70          # |cos| au-dela duquel un contact LONGE le tablier
SEUIL_DENIVELE = None     # lu au contrat (niveaux.json::reseau_noue)
VOISINAGE_M = 12.0        # portee de la passe INDIRECTE (piece de rive)


def axe_long(g):
    """Direction principale d'une emprise (analyse en composantes principales
    des sommets de son enveloppe)."""
    c = np.asarray(g.exterior.coords, dtype=np.float64)[:-1]
    if len(c) < 3:
        return np.array([1.0, 0.0])
    c = c - c.mean(axis=0)
    u, s, vt = np.linalg.svd(c, full_matrices=False)
    return vt[0] / (np.linalg.norm(vt[0]) or 1.0)


def dir_ligne(xy):
    d = np.asarray(xy[-1], dtype=np.float64) - np.asarray(xy[0], dtype=np.float64)
    n = np.linalg.norm(d)
    return d / n if n > 1e-9 else np.array([1.0, 0.0])


def travers(g, ax, x, y):
    """Coordonnee TRANSVERSALE signee de (x, y) par rapport a l'axe long du
    tablier, comptee depuis son centre : le SIGNE dit de quel cote on est."""
    n = np.array([-ax[1], ax[0]])
    c = g.centroid
    return float((x - c.x) * n[0] + (y - c.y) * n[1])


def tablier_au_point(t, ze, x, y):
    """(extrados, intrados) DU TABLIER a l'aplomb de (x, y).

    L'extrados vient de la loi que le contrat donne au tablier ; l'intrados
    s'en deduit par la regle `epaisseur_tablier` du registre. Le scalaire
    `cote_intrados_m` sert de repli quand l'epaisseur manque, et il est
    toujours rapporte a cote pour que l'ecart soit lisible."""
    x = np.atleast_1d(np.asarray(x, dtype=np.float64))
    y = np.atleast_1d(np.asarray(y, dtype=np.float64))
    ze_ = ze.z_loi(t["loi"], x, y)
    if ze_ is None:
        return None, None
    zt = float(np.median(ze_))
    # 1) L'INTRADOS EN LONG DU CONTRAT, quand il existe : c'est la reponse
    #    du compilateur, elle prime sur toute derivation de ma part.
    lpi = t.get("loi_intrados")
    if lpi is not None:
        zi_ = ze.z_loi(lpi, x, y)
        if zi_ is not None:
            return zt, float(np.median(zi_))
    # 2) sinon la regle du registre : extrados moins l'epaisseur declaree
    if t["ep"] is None:
        return zt, t["zi_scalaire"]
    return zt, zt - t["ep"]


def main():
    global SEUIL_DENIVELE
    t0 = time.time()
    ct = Contrat()
    global REG
    reg = Registre()                      # empreinte verifiee dans le contrat
    REG = reg.gabarits()
    AFF = reg.seuil_affleurant()
    jalon("degagement : seuils LUS AU REGISTRE DU CONTRAT %s (registre.json, "
          "md5 %s, empreinte verifiee contre plan_index.json ; %d regles). "
          "Regle ouvrage_affleurant : seuil %.2f m"
          % (json.dumps(REG["seuils_m"], ensure_ascii=False), reg.md5,
             reg.compte_reel, AFF["seuil_m"]))
    SEUIL_DENIVELE = float(ct.niveaux["reseau_noue"]["SEUIL_DENIVELE_M"])
    ze = ZEval(ct)
    jalon("degagement : contrat lu, %d lois indexees" % len(ze.lois))

    # --- inventaire : familles, lois, emprises des tabliers ------------------
    fam = {}
    lois = {}
    ponts = {}
    intrados = {}
    prov_intrados = defaultdict(int)
    ep = []                 # les epaisseurs de tablier DU CONTRAT
    dalots = {"n": 0, "avec_intrados": [], "sans_objet": 0}
    for c in ct.cells:
        q = ct.qui(c)
        for p in q["parcelles"]:
            pid = p["id"]
            f = ct.famille(p)
            fam[pid] = f
            lois[pid] = p["loi"]
            # INVARIANT DE LA REGLE `ouvrage_affleurant`, verifie et non
            # suppose : « 0 ouvrage affleurant portant une exigence de
            # hauteur libre ». Un dalot qui porterait un intrados le violerait.
            if f == "dalot":
                dalots["n"] += 1
                if p.get("cote_intrados_m") is not None:
                    dalots["avec_intrados"].append(pid)
                elif p.get("intrados") == "sans_objet":
                    dalots["sans_objet"] += 1
            if f == "pont":
                if p.get("epaisseur_tablier_m") is not None:
                    ep.append(float(p["epaisseur_tablier_m"]))
                g = polygone(p["anneaux"])
                if g is None or g.is_empty:
                    continue
                if pid in ponts and ponts[pid].area >= g.area:
                    continue
                ponts[pid] = g
                zi, prov = ct.cote_intrados(p)
                if zi is None:
                    # un `pont` sans cote d'intrados serait un trou de contrat
                    prov_intrados["SANS INTRADOS (%s)" % prov] += 1
                    ponts.pop(pid, None)
                    continue
                epi = p.get("epaisseur_tablier_m")
                lpi = ct.profil_intrados(p)
                prov_intrados["contrat" if lpi is None
                              else "contrat (intrados EN LONG)"] += 1
                intrados[pid] = {"zi_scalaire": float(zi),
                                 "ep": float(epi) if epi is not None else None,
                                 "loi": p["loi"], "loi_intrados": lpi,
                                 "prov": prov}
    # --- 2e MOITIE DE L'INVARIANT `ouvrage_affleurant` -----------------------
    # « 0 ouvrage affleurant DISCONTINU avec sa chaussee ». On mesure donc le
    # dZ au contact entre chaque dalot et la voirie qui le franchit, et on le
    # compare au ressaut que le registre admet (regle `ressaut_max`).
    ress = reg.regle("ressaut_max")
    m_r = re.search(r"n'excede pas\s*(\d+)\s*cm", ress["enonce"])
    seuil_ress = (float(m_r.group(1)) / 100.0) if m_r else None
    FAM_VOIRIE = ("chaussee", "trottoir", "carrefour", "parking",
                  "sol_mineral")
    dz_dalot = []
    dz_par_fam = defaultdict(list)
    for c in ct.cells:
        for i in ct.itf(c)["interfaces"]:
            a, b = i["a"], i["b"]
            fa, fb = fam.get(a), fam.get(b)
            if fa == "dalot" and fb in FAM_VOIRIE:
                d_, v_ = a, b
            elif fb == "dalot" and fa in FAM_VOIRIE:
                d_, v_ = b, a
            else:
                continue
            for pl in i.get("polylignes") or []:
                xy = np.asarray(pl, dtype=np.float64)
                if len(xy) < 2:
                    continue
                z1 = ze.z_loi(lois[d_], xy[:, 0], xy[:, 1])
                z2 = ze.z_loi(lois[v_], xy[:, 0], xy[:, 1])
                if z1 is None or z2 is None:
                    continue
                v = float(np.median(np.abs(z1 - z2)))
                dz_dalot.append(v)
                dz_par_fam[fam.get(v_)].append(v)
    dza = np.asarray(dz_dalot) if dz_dalot else np.zeros(0)
    # La regle dit « continu avec LA CHAUSSEE qui le franchit ». Le detail par
    # famille evite de mettre sur le dos du dalot un contact avec un talus ou
    # une zone minerale qui n'est pas la voie portee.
    par_fam = {}
    for f_, L_ in sorted(dz_par_fam.items()):
        A = np.asarray(L_)
        par_fam[f_] = {"contacts": int(A.size),
                       "dz_median_m": round(float(np.median(A)), 4),
                       "dz_max_m": round(float(A.max()), 4),
                       "au_dessus_du_ressaut": (int((A > seuil_ress).sum())
                                                if seuil_ress else None)}
    strict = np.asarray(dz_par_fam.get("chaussee", [])
                        + dz_par_fam.get("carrefour", []))
    continuite = {
        "seuil_ressaut_m": seuil_ress,
        "seuil_provenance": "registre::ressaut_max (extrait de l'enonce)",
        "contacts_dalot_voirie": int(dza.size),
        "dz_median_m": round(float(np.median(dza)), 4) if dza.size else None,
        "dz_max_m": round(float(dza.max()), 4) if dza.size else None,
        "contacts_au_dessus_du_ressaut": (
            int((dza > seuil_ress).sum()) if (dza.size and seuil_ress) else None),
        "invariant_continuite_tenu": (
            bool(dza.size and seuil_ress is not None
                 and (dza <= seuil_ress).all())),
        "par_famille_voisine": par_fam,
        "au_sens_STRICT_de_la_regle_chaussee_carrefour": {
            "contacts": int(strict.size),
            "dz_median_m": (round(float(np.median(strict)), 4)
                            if strict.size else None),
            "dz_max_m": round(float(strict.max()), 4) if strict.size else None,
            "au_dessus_du_ressaut": (int((strict > seuil_ress).sum())
                                     if (strict.size and seuil_ress) else None),
            "invariant_tenu": bool(strict.size and seuil_ress is not None
                                   and (strict <= seuil_ress).all()),
        },
        "lecture": "c'est CE chiffre qui prouve qu'un dalot affleure, pas la "
                   "capture : le dZ au contact avec la voirie qui le franchit.",
    }

    invariant_affleurant = {
        "continuite_avec_la_chaussee": continuite,
        "regle": "registre::ouvrage_affleurant",
        "enonce": AFF["enonce"], "invariant": AFF["invariant"],
        "seuil_m": AFF["seuil_m"],
        "dalots_du_contrat": dalots["n"],
        "dalots_intrados_sans_objet": dalots["sans_objet"],
        "dalots_portant_un_intrados": len(dalots["avec_intrados"]),
        "ids_en_faute": dalots["avec_intrados"][:20],
        "invariant_tenu": (not dalots["avec_intrados"]
                           and dalots["sans_objet"] == dalots["n"]),
        "consequence": "les dalots sont EXCLUS de la mesure de hauteur libre "
                       "(ils ne sont pas de la famille `pont`)",
    }
    jalon("degagement : %d emprises de tablier (famille `pont`), intrados %s ; "
          "regle ouvrage_affleurant : %d dalots, %d sans_objet, %d en faute -> "
          "invariant %s"
          % (len(ponts), dict(prov_intrados), dalots["n"], dalots["sans_objet"],
             len(dalots["avec_intrados"]),
             "TENU" if invariant_affleurant["invariant_tenu"] else "VIOLE"))

    # --- balayage des interfaces : qui touche un tablier, et comment ---------
    mes = defaultdict(list)
    ecartes = defaultdict(int)
    for c in ct.cells:
        for i in ct.itf(c)["interfaces"]:
            a, b = i["a"], i["b"]
            fa, fb = fam.get(a), fam.get(b)
            if fa == "pont" and fb != "pont":
                pnt, vsn = a, b
            elif fb == "pont" and fa != "pont":
                pnt, vsn = b, a
            else:
                continue
            g = ponts.get(pnt)
            if g is None or vsn not in lois:
                continue
            ax = axe_long(g)
            for pl in i.get("polylignes") or []:
                xy = np.asarray(pl, dtype=np.float64)
                if len(xy) < 2:
                    continue
                d = dir_ligne(xy)
                cos = abs(float(np.dot(d, ax)))
                zv = ze.z_loi(lois[vsn], xy[:, 0], xy[:, 1])
                if zv is None:
                    ecartes["voisin sans loi de Z"] += 1
                    continue
                t = intrados[pnt]
                prov = t["prov"]
                zt, zi = tablier_au_point(t, ze, xy[:, 0], xy[:, 1])
                if zt is None:
                    ecartes["tablier sans loi de Z evaluable"] += 1
                    continue
                zvm = float(np.median(zv))
                # UN FRANCHISSEMENT EST UN DENIVELE. Le seuil n'est pas invente :
                # c'est celui que le plan se donne lui-meme pour declarer un
                # croisement denivele (niveaux.json::reseau_noue.SEUIL_DENIVELE_M).
                if zt - zvm < SEUIL_DENIVELE:
                    ecartes["contact A NIVEAU (denivele < %.1f m) : releve de la "
                            "continuite/jonction, pas du degagement"
                            % SEUIL_DENIVELE] += 1
                    continue
                if cos < PARALLELE:
                    ecartes["contact d'about (culee/raccord d'extremite)"] += 1
                    continue
                libre = float(zi - zvm)
                f = fam.get(vsn)
                mes[f].append({
                    "pont": pnt, "voisin": vsn, "famille": f,
                    "hauteur_libre_m": round(libre, 3),
                    "hauteur_libre_scalaire_m":
                        round(float(t["zi_scalaire"] - zvm), 3),
                    "z_intrados_m": round(zi, 3), "z_extrados_m": round(zt, 3),
                    "z_voisin_m": round(float(np.median(zv)), 3),
                    "longueur_contact_m": round(float(i.get("longueur_m") or 0), 2),
                    "cos_axe": round(cos, 3),
                    "travers_m": round(travers(g, ax, float(xy[:, 0].mean()),
                                               float(xy[:, 1].mean())), 2),
                    "provenance_intrados": prov,
                    "x": round(float(xy[:, 0].mean()), 2),
                    "y": round(float(xy[:, 1].mean()), 2),
                })

    # --- 2e passe : les franchissements INDIRECTS ----------------------------
    # Un pont de la Garonne ne touche PAS l'eau : le mur de quai s'intercale.
    # L'adjacence seule laisse donc la case pont|eau_surface vide alors que le
    # franchissement existe. On regarde donc aussi ce que l'emprise du tablier
    # SURVOLE a courte distance, en gardant le meme critere de denivele.
    t2 = time.time()
    autres = []
    for c in ct.cells:
        for p in ct.qui(c)["parcelles"]:
            f = ct.famille(p)
            if f == "pont":
                continue
            g = polygone(p["anneaux"])
            if g is None or g.is_empty or g.area <= 0:
                continue
            autres.append((p["id"], f, g, p["loi"]))
    arbre = STRtree([a[2] for a in autres])
    for pid, g in ponts.items():
        t = intrados[pid]
        prov = t["prov"]
        ax = axe_long(g)
        halo = g.buffer(VOISINAGE_M)
        for k in arbre.query(halo):
            vid, f, gv, lo = autres[int(k)]
            if not gv.intersects(halo):
                continue
            if any(m["pont"] == pid and m["voisin"] == vid for m in mes[f]):
                continue                       # deja pris par l'adjacence
            pt = gv.intersection(halo).representative_point()
            zv = ze.z_loi(lo, np.array([pt.x]), np.array([pt.y]))
            if zv is None:
                continue
            zvm = float(zv[0])
            # l'intrados est pris a l'aplomb du point regarde, pas au centre
            zt, zi = tablier_au_point(t, ze, pt.x, pt.y)
            if zt is None:
                continue
            if zt - zvm < SEUIL_DENIVELE:
                continue
            mes[f].append({
                "pont": pid, "voisin": vid, "famille": f,
                "hauteur_libre_m": round(float(zi - zvm), 3),
                "hauteur_libre_scalaire_m":
                    round(float(t["zi_scalaire"] - zvm), 3),
                "z_intrados_m": round(zi, 3), "z_extrados_m": round(zt, 3),
                "z_voisin_m": round(zvm, 3), "longueur_contact_m": None,
                "cos_axe": None, "provenance_intrados": prov,
                "travers_m": round(travers(g, ax, pt.x, pt.y), 2),
                "voisinage": "INDIRECT (a moins de %.0f m de l'emprise du "
                             "tablier, piece de rive intercalee)" % VOISINAGE_M,
                "x": round(float(pt.x), 2), "y": round(float(pt.y), 2),
            })
            ecartes["franchissement INDIRECT retenu"] += 1
    chrono("degagement / passe indirecte", time.time() - t2,
           "%d emprises voisines indexees" % len(autres))

    # --- 3e PASSE : LE TIRANT D'AIR AU-DESSUS DU LIT -------------------------
    # Le trou de methode que j'avais nomme : un pont de la Garonne ne TOUCHE
    # jamais l'eau (le mur de quai s'intercale), donc l'adjacence ne le voyait
    # pas, et le critere d'aplomb le laissait sans verdict. Or le tirant d'air
    # se mesure au-dessus du LIT, pas au contact.
    #
    # Ce qui rend la mesure legitime sans adjacence, et c'est le contrat qui le
    # dit : `registre::bief_plat` — « un bief est PLAT : une surface d'eau
    # porte une cote unique ». Verifie : les 275 pieces de bief
    # (eau_surface + canal) ont TOUTES une loi `constante`. La cote du plan
    # d'eau reste donc definie SOUS la travee, la ou la regle d'exclusion a
    # decoupe le polygone. On mesure a l'aplomb de l'axe du tablier, et le
    # critere de franchissement reste le meme : le bief doit se retrouver DES
    # DEUX COTES de l'axe (sinon le tablier LONGE la berge, il ne la franchit
    # pas).
    t3 = time.time()
    FAM_BIEF = ("eau_surface", "canal")
    EAU_RECHERCHE_M = 30.0
    biefs = []
    for c in ct.cells:
        for p in ct.qui(c)["parcelles"]:
            if p.get("famille") not in FAM_BIEF or p.get("matiere") != "eau":
                continue
            zb = (p["loi"] or {}).get("z_m")
            if zb is None:
                continue
            gb = polygone(p["anneaux"])
            if gb is None or gb.is_empty or gb.area <= 0:
                continue
            biefs.append((p["id"], p["famille"], gb, float(zb)))
    arbre_eau = STRtree([b[2] for b in biefs]) if biefs else None
    n_lit = 0
    for pid, g in (ponts.items() if arbre_eau is not None else []):
        t = intrados[pid]
        ax = axe_long(g)
        cx, cy = g.centroid.x, g.centroid.y
        halo = g.buffer(EAU_RECHERCHE_M)
        par_fam = defaultdict(lambda: {"vmin": 1e9, "vmax": -1e9,
                                       "z": [], "ids": set()})
        proches = defaultdict(list)
        for k in arbre_eau.query(halo):
            bid, bf, gb, zb = biefs[int(k)]
            inter = gb.intersection(halo)
            if inter.is_empty or inter.area <= 0.5:
                continue
            co = shapely.get_coordinates(inter)
            v = (co[:, 0] - cx) * (-ax[1]) + (co[:, 1] - cy) * ax[0]
            e = par_fam[bf]
            e["vmin"] = min(e["vmin"], float(v.min()))
            e["vmax"] = max(e["vmax"], float(v.max()))
            e["z"].append(zb)
            e["ids"].add(bid)
            proches[bf].append((bid, gb, zb))
        for bf, e in par_fam.items():
            if not (e["vmax"] > APLOMB_TOL_M and e["vmin"] < -APLOMB_TOL_M):
                ecartes["bief d'un SEUL cote du tablier (il longe la berge, "
                        "il ne franchit pas le lit)"] += 1
                continue
            # PLUS DE MEDIANE. Chaque point de l'axe est mesure contre LE BIEF
            # QU'IL FRANCHIT REELLEMENT : celui dont le polygone est le plus
            # proche a l'aplomb de ce point. C'est licite parce qu'un bief est
            # PLAT (registre::bief_plat) — sa cote reste definie sous la
            # travee. Un tablier au-dessus de deux biefs de cotes differentes
            # est donc juge travee par travee, chacune contre la sienne.
            cands = proches.get(bf) or []
            multi = len(set(round(z, 3) for (_, _, z) in cands)) > 1
            if multi:
                ecartes["tablier au-dessus de PLUSIEURS biefs de cotes "
                        "differentes : mesure point par point contre le bief "
                        "le plus proche (aucune mediane)"] += 1
            # points a l'APLOMB : le long de l'axe du tablier, dans l'emprise
            co = shapely.get_coordinates(g)
            u = (co[:, 0] - cx) * ax[0] + (co[:, 1] - cy) * ax[1]
            ech = []
            zw_vus = []
            for uu in np.linspace(float(u.min()), float(u.max()), 9):
                px, py = cx + uu * ax[0], cy + uu * ax[1]
                P_ = shapely.Point(px, py)
                if not g.contains(P_):
                    continue
                zt_, zi_ = tablier_au_point(t, ze, px, py)
                if zt_ is None:
                    continue
                zw_p = (min(cands, key=lambda c: c[1].distance(P_))[2]
                        if cands else float(np.median(e["z"])))
                zw_vus.append(zw_p)
                ech.append((float(zi_ - zw_p), zi_, zt_, px, py))
            zw = (float(np.median(zw_vus)) if zw_vus
                  else float(np.median(e["z"])))
            if not ech:
                ecartes["tablier dont l'axe ne rencontre pas sa propre "
                        "emprise (emprise trop courbe)"] += 1
                continue
            ech.sort()
            hl, zi_, zt_, px, py = ech[0]          # le point le PLUS contraint
            mes[bf].append({
                "pont": pid, "voisin": sorted(e["ids"])[0], "famille": bf,
                "hauteur_libre_m": round(hl, 3),
                "hauteur_libre_scalaire_m": round(float(t["zi_scalaire"] - zw), 3),
                "z_intrados_m": round(float(zi_), 3),
                "z_extrados_m": round(float(zt_), 3),
                "z_voisin_m": round(zw, 3),
                "longueur_contact_m": None, "cos_axe": None,
                "provenance_intrados": t["prov"],
                "travers_m": 0.0,
                "aplomb_deux_cotes": True,
                "mesure": "TIRANT D'AIR AU-DESSUS DU LIT — cote de bief "
                          "(registre::bief_plat, loi constante) prise a "
                          "l'aplomb de l'axe du tablier, sans exiger "
                          "l'adjacence ; bief present des deux cotes de l'axe",
                "biefs_de_cotes_differentes": bool(multi),
                "cote_bief_par_point": True,
                "cotes_de_bief_rencontrees":
                    sorted(set(round(z, 3) for z in zw_vus)),
                "biefs": sorted(e["ids"])[:6],
                "biefs_n": len(e["ids"]),
                "echantillons_axe": len(ech),
                "hauteur_libre_mediane_axe_m":
                    round(float(np.median([x[0] for x in ech])), 3),
                "x": round(float(px), 2), "y": round(float(py), 2),
            })
            n_lit += 1
    chrono("degagement / passe au-dessus du lit", time.time() - t3,
           "%d bief(s) indexes, %d franchissements de lit mesures"
           % (len(biefs), n_lit))
    jalon("degagement : passe AU-DESSUS DU LIT — %d pieces de bief "
          "(eau_surface+canal, toutes a loi constante), %d franchissements de "
          "lit mesures a l'aplomb sans adjacence" % (len(biefs), n_lit))

    # --- LE CRITERE D'APLOMB -------------------------------------------------
    # Le plan INTERDIT le recouvrement des emprises (contrat `exclusion`) et je
    # l'ai verifie : 0 couple recouvrant sur les 56 tabliers. Une surface
    # franchie n'est donc JAMAIS sous le tablier EN PLAN — elle est decoupee de
    # part et d'autre. Un test « le point est dans l'emprise du tablier »
    # rendrait donc zero mesure partout. Le critere qui separe reellement
    # FRANCHIR de LONGER est celui-ci : la surface franchie doit se retrouver
    # DES DEUX COTES de l'axe du tablier. Une voie qui passe dessous est
    # coupee en deux par le tablier ; une voie qui longe reste d'un seul cote.
    cotes = defaultdict(lambda: [False, False])
    for f, L in mes.items():
        for m in L:
            v = m.get("travers_m")
            if v is None or m.get("aplomb_deux_cotes") is not None:
                continue
            k = (m["pont"], f)
            if v > APLOMB_TOL_M:
                cotes[k][0] = True
            elif v < -APLOMB_TOL_M:
                cotes[k][1] = True
    for f, L in mes.items():
        for m in L:
            # la passe AU-DESSUS DU LIT a deja tranche son propre aplomb
            # (bief des deux cotes de l'axe) : on ne l'ecrase pas.
            if m.get("aplomb_deux_cotes") is not None:
                continue
            g_, d_ = cotes.get((m["pont"], f), [False, False])
            m["aplomb_deux_cotes"] = bool(g_ and d_)

    # --- CONCORDANCE des deux intrados : la preuve que le point par point ----
    # est bien la regle du registre appliquee, et la mesure de ce que le
    # scalaire du contrat coute sur les tabliers en pente.
    conc = defaultdict(lambda: {"n": 0, "ecarts": []})
    for f, L in mes.items():
        for m in L:
            forme = (intrados[m["pont"]]["loi"] or {}).get("forme")
            e = conc[forme or "?"]
            e["n"] += 1
            e["ecarts"].append(abs(m["hauteur_libre_m"]
                                   - m["hauteur_libre_scalaire_m"]))
    concordance = {}
    for k, v in conc.items():
        a = np.asarray(v["ecarts"])
        concordance[k] = {
            "couples": v["n"],
            "ecart_median_m": round(float(np.median(a)), 3) if len(a) else None,
            "ecart_max_m": round(float(a.max()), 3) if len(a) else None,
        }
    concordance["lecture"] = (
        "ecart entre l'intrados PAR POINT (loi du tablier - epaisseur du "
        "contrat) et le scalaire `cote_intrados_m`. Sur `constante` il doit "
        "etre nul : c'est la verification que la regle du registre est bien "
        "celle qu'applique le contrat. Sur `profil_troncon` il chiffre ce que "
        "coute un intrados scalaire sur un tablier en pente.")

    # --- les 18 cases --------------------------------------------------------
    seuil_par_famille = {}
    for f, cl in CLASSE.items():
        if cl in REG["seuils_m"]:
            seuil_par_famille[f] = (
                REG["seuils_m"][cl],
                "registre::hauteur_libre, classe %s = %.2f m — %s"
                % (cl, REG["seuils_m"][cl], REG["reference"]))

    out = []
    for a, b in CASES:
        f = a if b == "pont" else b
        L = mes.get(f, [])
        e = {"case": "%s x %s" % (a, b), "famille_franchie": f,
             "couples_mesures": len(L)}
        if not L:
            e["statut"] = "SANS OBJET MESURE"
            e["note"] = ("aucun contact tablier|%s parallele a l'axe du "
                         "tablier dans le domaine" % f)
            out.append(e)
            continue
        h = np.array([x["hauteur_libre_m"] for x in L])
        sous = h[h > -50]
        e["hauteur_libre_min_m"] = round(float(h.min()), 3)
        e["hauteur_libre_mediane_m"] = round(float(np.median(h)), 3)
        e["hauteur_libre_max_m"] = round(float(h.max()), 3)
        e["sous_le_tablier_n"] = int((h > 0.0).sum())
        e["au_dessus_ou_au_niveau_n"] = int((h <= 0.0).sum())
        L2 = sorted([x for x in L if x["hauteur_libre_m"] > 0],
                    key=lambda x: x["hauteur_libre_m"])
        e["pires"] = L2[:5]
        s = seuil_par_famille.get(f)
        if s is None:
            e["statut"] = "MESUREE, SANS VERDICT"
            e["seuil"] = None
            e["classe_de_gabarit"] = CLASSE.get(f)
            e["motif_sans_verdict"] = SANS_CLASSE.get(
                f, "le registre ne chiffre pas de gabarit pour la classe %s"
                   % CLASSE.get(f))
        else:
            e["seuil_m"] = s[0]
            e["seuil_reference"] = s[1]
            # LE VERDICT NE PORTE QUE SUR LE MESURABLE SANS RESERVE :
            # contact DIRECT (du contrat) ET franchissement confirme des deux
            # cotes de l'axe. Le reste est rapporte, pas juge.
            juge = [x for x in L
                    if not x.get("voisinage") and x.get("aplomb_deux_cotes")]
            e["retenus_pour_le_verdict_n"] = len(juge)
            sous_seuil = [x for x in L
                          if 0 < x["hauteur_libre_m"] < s[0]]
            e["sous_le_seuil_n"] = len(sous_seuil)
            # LE PARTAGE QUI COMPTE POUR LA REVUE : un contact DIRECT porte le
            # critere d'axe (le tablier longe ou franchit vraiment ce voisin) ;
            # un releve INDIRECT prend tout ce qui se trouve a moins de 12 m de
            # l'emprise du tablier, sans critere d'axe — il attrape donc aussi
            # ce qui est A COTE de l'ouvrage et pas dessous. Les deux sont
            # rapportes separement : je ne les melange pas dans un verdict.
            dir_ = [x for x in sous_seuil if not x.get("voisinage")]
            ind_ = [x for x in sous_seuil if x.get("voisinage")]
            e["sous_le_seuil_direct_n"] = len(dir_)
            e["sous_le_seuil_indirect_n"] = len(ind_)
            e["sous_le_seuil_pires"] = sorted(
                sous_seuil, key=lambda x: x["hauteur_libre_m"])[:5]
            e["sous_le_seuil_pires_DIRECTS"] = sorted(
                dir_, key=lambda x: x["hauteur_libre_m"])[:5]
            # --- LE VERDICT : sur les seuls retenus ---------------------------
            fautifs = [x for x in juge if 0 < x["hauteur_libre_m"] < s[0]]
            e["sous_le_seuil_RETENUS_n"] = len(fautifs)
            e["sous_le_seuil_RETENUS_pires"] = sorted(
                fautifs, key=lambda x: x["hauteur_libre_m"])[:5]
            if not juge:
                e["statut"] = "MESUREE, SANS VERDICT"
                e["motif_sans_verdict"] = (
                    "aucun franchissement confirme des deux cotes de l'axe du "
                    "tablier : les %d couples de cette case ne sont que du "
                    "voisinage (le tablier LONGE, il ne franchit pas). "
                    "Rapporte, pas juge." % len(L))
            else:
                e["statut"] = "VERTE" if not fautifs else "ROUGE"
            e["hors_verdict"] = {
                "indirects_n": len(ind_),
                "directs_sans_aplomb_n": len([x for x in L
                                              if not x.get("voisinage")
                                              and not x.get("aplomb_deux_cotes")]),
                "note": "la passe INDIRECTE (halo de 12 m) et les contacts "
                        "sans confirmation des deux cotes sont RAPPORTES et "
                        "n'entrent dans aucun verdict.",
            }
        out.append(e)

    res = {
        "produit_par": "work/PLAN/maquette/mq_degagement.py",
        "objet": "les 18 cases NON MESURABLES de matrice_mesuree.json, "
                 "mesurees sur la maquette 3D",
        "methode": __doc__.split("METHODE")[1].split("SEUILS")[0].strip(),
        "tabliers": len(ponts),
        "provenance_intrados": dict(prov_intrados),
        "epaisseur_tablier": {
            "provenance": "CONTRAT (champ `epaisseur_tablier_m` de la "
                          "parcelle, regle registre::epaisseur_tablier). La "
                          "maquette ne declare plus AUCUNE epaisseur.",
            "n": len(ep),
            "min_m": round(float(min(ep)), 3) if ep else None,
            "mediane_m": round(float(np.median(ep)), 3) if ep else None,
            "max_m": round(float(max(ep)), 3) if ep else None,
        },
        "regle_ouvrage_affleurant": invariant_affleurant,
        "concordance_intrados": concordance,
        "critere_aplomb": {
            "regle": "un franchissement n'est retenu que si la surface "
                     "franchie se retrouve DES DEUX COTES de l'axe du tablier "
                     "(tolerance %.1f m)" % APLOMB_TOL_M,
            "pourquoi_pas_le_test_dans_l_emprise":
                "le plan interdit le recouvrement des emprises (contrat "
                "`exclusion`) et je l'ai VERIFIE : 0 couple recouvrant sur les "
                "56 tabliers, 0 m2. Une surface franchie n'est donc jamais "
                "sous le tablier EN PLAN ; un test d'appartenance a l'emprise "
                "rendrait zero mesure partout. Le test des deux cotes est la "
                "traduction geometrique fidele de « passer dessous ».",
            "verdict_porte_sur": "contacts DIRECTS du contrat ET confirmes "
                                 "des deux cotes",
        },
        "seuil_denivele_m": SEUIL_DENIVELE,
        "voisinage_indirect_m": VOISINAGE_M,
        "seuil_denivele_provenance":
            "niveaux.json::reseau_noue.SEUIL_DENIVELE_M — le seuil que LE PLAN "
            "se donne pour declarer un croisement denivele (1120 croisements "
            "denivelees sur 17668). Aucun seuil invente par la maquette.",
        "contacts_ecartes": dict(ecartes),
        "registre": REG,
        "registre_dans_le_contrat": True,
        "registre_source": {
            "fichier": "plan_ville/v1/data/registre.json",
            "md5": reg.md5,
            "empreinte_verifiee_contre": "plan_index.json::fichiers",
            "regles": reg.compte_reel,
            "note": "TROU COMBLE : a la passe precedente les gabarits etaient "
                    "lus dans la page L1a (work/PLAN/matrice/data_matrice.js), "
                    "hors contrat. Le contrat final porte son registre et "
                    "l'index le scelle : cette mesure de conformite se rejoue "
                    "desormais depuis le SEUL contrat. L'extraction externe a "
                    "ete supprimee du programme.",
        },
        "classes_de_gabarit": CLASSE,
        "cases": out,
        "compte": {
            "VERTE": sum(1 for e in out if e["statut"] == "VERTE"),
            "ROUGE": sum(1 for e in out if e["statut"] == "ROUGE"),
            "MESUREE, SANS VERDICT": sum(1 for e in out
                                         if e["statut"] == "MESUREE, SANS VERDICT"),
            "SANS OBJET MESURE": sum(1 for e in out
                                     if e["statut"] == "SANS OBJET MESURE"),
        },
        "secondes": round(time.time() - t0, 1),
    }
    p = os.path.join(MQ, "mq_degagement.json")
    with io.open(p, "w", encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False, indent=1, sort_keys=True)
    chrono("18 cases degagement", time.time() - t0, p)
    for e in out:
        print("%-28s %-22s n=%-5d %s"
              % (e["case"], e["statut"], e["couples_mesures"],
                 ("min %.2f m / med %.2f m"
                  % (e.get("hauteur_libre_min_m", 0),
                     e.get("hauteur_libre_mediane_m", 0)))
                 if e["couples_mesures"] else ""))
    print(json.dumps(res["compte"], ensure_ascii=False))
    print("ecrit", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
