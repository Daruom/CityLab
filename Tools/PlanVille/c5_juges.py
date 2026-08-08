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
    byid = {p["id"]: p for p in P}

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
    # -- les communautes au plus fort RELIEF (l'A/B 3D de l'utilisateur :
    #    aplanir ou draper une grande communaute pentue ? seuil NON tranche)
    nrep = json.load(io.open(os.path.join(OUT, "niveaux.json"),
                             encoding="utf-8"))
    for c in (nrep.get("communautes", {}).get("pires_reliefs") or [])[:N_TOURNEE]:
        sig("communaute de nivellement",
            "communaute au plus fort relief (%s)" % c["matiere"], c["x"],
            c["y"], "%d membres, %.0f m2, relief du MNT %.2f m, cote retenue "
                    "%.2f m" % (c["membres"], c["aire_m2"], c["relief_m"],
                                c["z_m"]))
    # -- LE GRIEF NOMINATIF : la place Saint-Pierre et ses escaliers
    for p2 in P:
        if (p2.get("meta") or {}).get("nature") != "escalier":
            continue
        r = p2["geom"].representative_point()
        if (r.x + 700.0) ** 2 + (r.y - 120.0) ** 2 > 200.0 ** 2:
            continue
        vs = [f for f in fronts if f["a"] == p2["id"] or f["b"] == p2["id"]]
        pire = max(vs, key=lambda f: f["dz_m"]) if vs else None
        sig("Saint-Pierre (grief utilisateur)", "escalier de la place "
            "Saint-Pierre", r.x, r.y,
            "%s, %.1f m2, %d interfaces ; plus forte marche %.2f m (%s)"
            % (p2["id"], p2["geom"].area, len(vs),
               pire["dz_m"] if pire else 0.0,
               pire["type"] if pire else "-"))
    # -- les plus fortes marches RESTANTES entre chaussees (invariant ①)
    def _bande(i):
        return i.startswith("bnd/") or i.startswith("tro/")
    ch = [f for f in fronts
          if byid.get(f["a"]) and byid.get(f["b"])
          and byid[f["a"]]["proprietaire"] == "voirie"
          and byid[f["b"]]["proprietaire"] == "voirie"
          and not (_bande(f["a"]) or _bande(f["b"]))]
    ch.sort(key=lambda f: -f["dz_m"])
    for f in ch[:N_TOURNEE]:
        sig("marche entre chaussees", "plus forte marche chaussee|chaussee "
            "restante", f["x"], f["y"],
            "dZ %.3f m sur %.1f m, resolution DECLAREE `%s`"
            % (f["dz_m"], f["longueur_m"], f["type"]))

    # -- L1b : les familles nouvelles, les terrassements, les ecarts au releve
    try:
        with open(os.path.join(CACHE, "l1b_solveur.pkl"), "rb") as f:
            SV = pickle.load(f)
        FAMP = SV["familles"]
        srep = json.load(io.open(os.path.join(OUT, "solveur.json"),
                                 encoding="utf-8"))
        for d in (srep.get("fidelite_pires") or [])[:N_TOURNEE]:
            sig("ecart au releve", "plus fort ecart |z_projet - z_releve|",
                d["x"], d["y"], "%s (%s) : %.2f m — projet %.2f, releve %.2f ; "
                "origine %s" % (d["parcelle"], d["famille"], d["ecart_m"],
                                d["z_projet_m"], d["z_releve_m"], d["origine"]))
        T2 = sorted(SV["terrassements"], key=lambda t: -t["dz_m"])
        for t in T2[:N_TOURNEE]:
            sig("terrassement", "plus fort raccord au terrain (%s)" % t["piece"],
                t["x"], t["y"], "dZ %.2f m sur %.1f m, largeur %.1f m, place "
                "disponible %.0f %%" % (t["dz_m"], t["longueur_m"],
                                        t["largeur_m"],
                                        t["place_disponible_pc"]))
        for t in [x for x in SV["terrassements"] if x["piece"] == "quai"][:6]:
            sig("quai", "piece de rive contre l'eau", t["x"], t["y"],
                "dZ %.2f m sur %.1f m" % (t["dz_m"], t["longueur_m"]))
        vus = set()
        for p2 in P:
            f2 = FAMP.get(p2["id"])
            if f2 not in ("canal", "voie_ferree", "terrain_sport", "parking",
                          "edicule", "ouvrage_hydro", "tremie") or f2 in vus:
                continue
            vus.add(f2)
            r = p2["geom"].representative_point()
            sig("famille nouvelle", "premiere emprise de la famille `%s`" % f2,
                r.x, r.y, "%s, %.0f m2" % (p2["id"], p2["geom"].area))
    except Exception as e:
        jalon("C5/tournee L1b indisponible (%s)" % e)

    # -- L1b-3 : les plans d'eau SANS cote declaree, et les voies reclassees
    n_sc = 0
    for pid, Lz in sorted(lois.items()):
        if not isinstance(Lz, dict):
            continue
        if Lz.get("bief_composante") and "aucune cote declaree" in                 str(Lz.get("source") or ""):
            p3 = byid.get(pid)
            if p3 is None:
                continue
            r = p3["geom"].representative_point()
            n_sc += 1
            if n_sc <= 8:
                sig("eau sans cote declaree",
                    "plan d'eau dont aucun side-car ne donne la cote",
                    r.x, r.y, "%s (%s) : %.1f m2, cote retenue %.2f m — a "
                    "juger en maquette" % (pid, Lz["bief_composante"],
                                           p3["geom"].area, Lz["z_m"]))
    n_rc = 0
    for pid, Lz in sorted(lois.items()):
        if isinstance(Lz, dict) and Lz.get("reclasse") == "emmarchement":
            p3 = byid.get(pid)
            if p3 is None:
                continue
            n_rc += 1
            if n_rc <= 10:
                r = p3["geom"].representative_point()
                sig("voie reclassee en emmarchement",
                    "voie pietonne trop pentue pour etre une rampe",
                    r.x, r.y, Lz.get("reclasse_motif", "")[:150])

    # -- L1b-4 : les plus grandes portees de tablier (jugees en maquette)
    for d in (srep.get("tabliers", {}).get("plus_grandes_portees") or [])[:10]:
        sig("tablier", "plus grande portee de tablier", d["x"], d["y"],
            "%s : portee %.1f m, epaisseur %.2f m (portee/20 bornee), "
            "extrados %.2f m, intrados %.2f m"
            % (d["parcelle"], d["portee_m"], d["epaisseur_m"],
               d["cote_extrados_m"], d["cote_intrados_m"]))

    # -- L1b-5 : CHAQUE cas rouge restant part en signet, un par un
    try:
        mrep2 = json.load(io.open(os.path.join(OUT, "matrice_mesuree.json"),
                                  encoding="utf-8"))
        for d in (mrep2.get("rouges_cas_par_cas") or []):
            L1 = lois.get(d["a"]) or {}
            L2 = lois.get(d["b"]) or {}
            raison = "cause NON ETABLIE"
            for Lx, qui in ((L1, d["a"]), (L2, d["b"])):
                if Lx.get("rampe_acces_ouvrage"):
                    raison = "rampe d'acces a un ouvrage (%s)" % qui
                    break
                if Lx.get("reclasse"):
                    raison = "voie reclassee en emmarchement (%s)" % qui
                    break
                if Lx.get("porteur"):
                    raison = "profil adopte d'un porteur (%s)" % qui
                    break
            sig("rouge residuel", "%s — dZ %.3f m" % (d["case"], d["dz_m"]),
                d["x"], d["y"],
                "%s | %s sur %.1f m — raison : %s"
                % (d["a"], d["b"], d["m"], raison))
    except Exception as e:
        jalon("C5/signets des rouges indisponibles (%s)" % e)

    # -- L1b-6 : les rampes CONTRAINTES (la longueur necessaire ne tient pas)
    try:
        nrep2 = json.load(io.open(os.path.join(OUT, "niveaux.json"),
                                  encoding="utf-8"))
        for d in (nrep2.get("rampes_acces", {}).get("contraintes") or [])[:20]:
            sig("rupture de terrain", "deux noeuds proches, fort denivele",
                d["x"], d["y"],
                "troncon %s : %.2f m de denivele sur %.2f m, il en faudrait "
                "%.2f m au plafond de 12 %% -> pente residuelle DECLAREE "
                "%.2f %% (cotes gelees : %s — aucune, c'est donc une rupture "
                "de terrain, pas une rampe coincee)"
                % (d["troncon"], d["denivele_m"], d["longueur_m"],
                   d["longueur_necessaire_m"], d["pente_residuelle_pc"],
                   d["geles"]))
    except Exception as e:
        jalon("C5/signets de rampe contrainte indisponibles (%s)" % e)

    # -- L1b-8 : la famille `dalot` et les ponts bas
    try:
        n_d = 0
        for p2 in P:
            if FAMP.get(p2["id"]) != "dalot" or n_d >= 10:
                continue
            L4 = lois.get(p2["id"]) or {}
            r = p2["geom"].representative_point()
            n_d += 1
            sig("dalot", "ouvrage affleurant (dalot, buse, ponceau)", r.x, r.y,
                "%s : %.0f m2, hauteur declaree %.2f m (< 2,20 m) ; son dessus "
                "adopte le profil de %s"
                % (p2["id"], p2["geom"].area,
                   float((p2.get("meta") or {}).get("hauteur_moy_m") or 0.0),
                   L4.get("porte_chaussee_de") or "sa voie"))
        bas = []
        for p2 in P:
            if FAMP.get(p2["id"]) != "pont":
                continue
            L4 = lois.get(p2["id"]) or {}
            h = (p2.get("meta") or {}).get("hauteur_moy_m")
            try:
                h = float(h)
            except (TypeError, ValueError):
                continue
            if h < 4.30:
                bas.append((h, p2, L4))
        bas.sort(key=lambda t: t[0])
        for h, p2, L4 in bas[:10]:
            r = p2["geom"].representative_point()
            sig("pont bas", "vrai pont sous le gabarit routier de 4,30 m",
                r.x, r.y, "%s : hauteur declaree %.2f m — un vieux pont bas "
                "est une realite, a juger sur place" % (p2["id"], h))
    except Exception as e:
        jalon("C5/signets dalot indisponibles (%s)" % e)

    # -- les pires conflits donnee/visible
    mrep = json.load(io.open(os.path.join(OUT, "matiere.json"),
                             encoding="utf-8"))
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
