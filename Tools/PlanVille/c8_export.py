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


def main(cellules_filtre=None):
    """`cellules_filtre` = liste de noms de cellule (« -1_0 ») : mode ITERATION.
    On n'ecrit alors QUE ces cellules et un index PARTIEL ; ni plan_index.json
    ni plan.json ne sont touches, pour qu'une iteration ne puisse jamais se
    faire passer pour une passe complete."""
    t0 = time.time()
    if not os.path.isdir(DATA):
        os.makedirs(DATA)
    with open(os.path.join(CACHE, "parcelles.pkl"), "rb") as f:
        D = pickle.load(f)
    P = D["parcelles"]
    EMP_OBJ = D.get("emprises_objet") or {}
    for p in P:
        p["geom"] = shapely.from_wkb(p["geom"])
    cells = [tuple(c) for c in D["cells"]]
    if cellules_filtre:
        cells = [c for c in cells if "%d_%d" % c in set(cellules_filtre)]
        jalon("C8/MODE ITERATION : export limite a %d cellule(s) %s ; la "
              "porteuse est calculee sur ces seules cellules et l'index est "
              "PARTIEL — ni plan_index.json ni plan.json ne sont touches"
              % (len(cells), sorted(cellules_filtre)))
    with open(os.path.join(CACHE, "matiere.pkl"), "rb") as f:
        mat = pickle.load(f)["matiere"]
    with open(os.path.join(CACHE, "niveaux.pkl"), "rb") as f:
        lois = pickle.load(f)["lois"]
    with open(os.path.join(CACHE, "interfaces.pkl"), "rb") as f:
        fronts = pickle.load(f)["fronts"]
    # L1b-4 : le contrat doit se suffire a lui-meme — familles de matrice,
    # hauteur du bati, assiettes et terrassements sortent desormais du cache.
    FAM, ASSIETTES, TERR = {}, [], []
    try:
        with open(os.path.join(CACHE, "l1b_solveur.pkl"), "rb") as f:
            _SV = pickle.load(f)
        FAM = _SV.get("familles") or {}
        ASSIETTES = _SV.get("assiettes") or []
        TERR = _SV.get("terrassements") or []
    except Exception:
        pass
    A_PAR = {a["parcelle"]: a for a in ASSIETTES}
    jalon("C8/ENTREES : %d parcelles, %d frontieres, %d cellules"
          % (len(P), len(fronts), len(cells)))

    rec, XS, YS = lit_semis_complet()
    vit = retenues(rec, XS, YS, P, mat)

    # index spatial
    byid = {p["id"]: p for p in P}
    G = [p["geom"] for p in P]
    T = STRtree(G)
    # ---- PASSE A : ou chaque parcelle a-t-elle REELLEMENT une piece ? -------
    # (les memes filtres qu'a l'ecriture : aire nulle, intersection vide, et
    # surtout la deduplication a 1 mm qui fait disparaitre les lamelles)
    tA = time.time()
    aires = {}
    for cx, cy in cells:
        nom = "%d_%d" % (cx, cy)
        bx = box(cx * CELL_M, cy * CELL_M, (cx + 1) * CELL_M,
                 (cy + 1) * CELL_M)
        for j in T.query(bx):
            p = P[int(j)]
            if p["geom"].area <= AIRE_NULLE_M2:
                continue
            try:
                q = valide(p["geom"].intersection(bx))
            except Exception:
                continue
            if q.is_empty or q.area <= 1e-9:
                continue
            if not coords(q):
                continue          # lamelle : rien a ecrire dans cette cellule
            aires.setdefault(p["id"], {})[nom] = q.area
    SANS_PIECE = set(p["id"] for p in P if p["id"] not in aires)
    porteuse = {}
    for pid, d in aires.items():
        porteuse[pid] = sorted(d.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
    sans_piece = sorted(p["id"] for p in P if p["id"] not in aires)
    chrono("C8/porteuse", time.time() - tA,
           "%d parcelles situees, %d sans piece" % (len(porteuse),
                                                    len(sans_piece)))
    if cellules_filtre:
        jalon("C8/PORTEUSE (iteration) : %d parcelles situees dans la ou les "
              "cellules demandees ; le compte des parcelles « sans piece » n'a "
              "PAS de sens ici (il vaudrait « hors cellule ») et n'est donc pas "
              "produit — seule la passe complete le mesure." % len(porteuse))
    else:
        # ⚠️ le set doit etre hisse : dans la comprehension il etait reconstruit
        # a chaque tour (quadratique des que la liste grossit).
        _aires_sp = [P[i]["geom"].area for i in range(len(P))
                     if P[i]["id"] in SANS_PIECE] or [0.0]
        jalon("C8/PORTEUSE : la cellule porteuse est desormais celle ou la "
              "piece a la PLUS GRANDE AIRE (a egalite, le plus petit nom de "
              "cellule) — plus le point representatif. %d parcelles situees ; "
              "%d parcelles n'ont AUCUNE piece emise (lamelles que la "
              "deduplication a %.0f mm reduit a moins de 4 sommets ; aires de "
              "%.2e a %.2e m2) et sont exclues du contrat, comptees ici."
              % (len(porteuse), len(sans_piece), 1.0,
                 min(_aires_sp), max(_aires_sp)))
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
    n_vers = [0]
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
                # ⚠️ L'axe vit dans la LOI pour les ponts (profil du tablier
                # declare) et dans la META pour la voirie. Le lire seulement
                # dans la meta laissait 73 parcelles d'ouvrage avec une loi de
                # profil SANS axe — les 190 polylignes sans Z de la maquette.
                axe = loi.get("axe") \
                    or (byid.get(src, {}).get("meta") or {}).get("axe")
                lo["profil"] = pr.get("pts")
                lo["L_m"] = pr.get("L_m")
                lo["pente_max_pc"] = pr.get("pente_max_pc")
                lo["axe"] = axe
            if loi.get("loi_heritee_de"):
                lo["loi_heritee_de"] = loi["loi_heritee_de"]
            r = {"id": p["id"], "proprietaire": p["proprietaire"],
                 "famille": FAM.get(p["id"]),
                 "matiere": mat[p["id"]][0], "loi": lo,
                 "aire_m2": round(q.area, 3),
                 "aire_totale_m2": round(g.area, 3),
                 "cellule_porteuse": porteuse.get(p["id"], nom),
                 "entiere": bool(abs(q.area - g.area) < 1e-6),
                 "anneaux": an}
            me = p.get("meta") or {}
            if me.get("h_m") is not None:
                r["hauteur_m"] = me["h_m"]
            elif loi.get("hauteur_heritee_m") is not None:
                r["hauteur_m"] = loi["hauteur_heritee_m"]
                r["hauteur_heritee_de"] = loi.get("hauteur_heritee_de")
            # ⚠️ TOUTE EMPRISE D'OUVRAGE DECLARE SON INTRADOS — une cote si
            # elle porte un tablier, `sans_objet` motive sinon. Le contrat ne
            # se tait jamais : la maquette doit pouvoir lire un champ, pas
            # deviner une absence.
            if p["proprietaire"] == "ouvrage":
                # l'emprise COMPLETE declaree par le side-car, non rognee par
                # la preseance : le bloc de berge absorbe la part de sol des
                # tabliers (ouv/107#0 : 66 m2 gagnes pour 238,6 m de portee),
                # mais la forme que la donnee declare, elle, est entiere.
                try:
                    _i = int(p["id"].split("/")[1].split("#")[0])
                    _w = EMP_OBJ.get(_i)
                except Exception:
                    _w = None
                _an = None
                if _w is not None:
                    _g = shapely.from_wkb(_w)
                    _an = coords(_g)
                if _an:
                    r["emprise_objet"] = _an
                    r["emprise_objet_m2"] = round(_g.area, 3)
                    r["emprise_objet_note"] = (
                        "emprise COMPLETE declaree par le side-car, non "
                        "rognee par la preseance ; `anneaux` reste la part "
                        "de sol que l'ouvrage a gagnee dans la partition. "
                        "Champ porte par l'OBJET : quand la partition le "
                        "fragmente (ouv/220#1 et #2), chaque fragment porte "
                        "la meme emprise — ne pas sommer emprise_objet_m2, "
                        "regrouper par identifiant avant le #")
                else:
                    # bande annexee ou bouchon de trou : du sol attribue a
                    # l'ouvrage, sans objet declare derriere. On le DIT.
                    r["emprise_objet"] = "sans_objet"
                    r["emprise_objet_note"] = (
                        "parcelle de sol attribuee a l'ouvrage par la "
                        "partition (bande annexee ou bouchon de trou) : "
                        "aucun objet n'est declare pour elle en side-car")
                if loi.get("cote_intrados_m") is not None:
                    r["intrados"] = loi["cote_intrados_m"]
                    r["intrados_nature"] = "cote d'intrados de tablier"
                elif FAM.get(p["id"]) == "dalot":
                    r["intrados"] = "sans_objet"
                    r["intrados_motif"] = (
                        "ouvrage AFFLEURANT (dalot, buse, ponceau) : hauteur "
                        "declaree inferieure au plus petit gabarit de passage "
                        "du reel (2,20 m). La route passe dessus, rien ne "
                        "passe dessous — aucune hauteur libre a exiger.")
                else:
                    r["intrados"] = "sans_objet"
                    r["intrados_motif"] = (
                        "cet ouvrage ne porte pas de tablier : il n'a pas "
                        "d'intrados a declarer (famille %s)"
                        % (FAM.get(p["id"]) or "ouvrage"))
            if loi.get("profil_intrados"):
                r["profil_intrados"] = loi["profil_intrados"]
                r["intrados_note"] = loi.get("intrados_note")
            for k2 in ("epaisseur_tablier_m", "cote_intrados_m", "portee_m",
                       "portee_hypothese", "epaisseur_provenance",
                       "reclasse", "reclasse_motif"):
                if loi.get(k2) is not None:
                    r[k2] = loi[k2]
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
            vers = [x for x in (f["a"], f["b"]) if x in SANS_PIECE]
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
            if vers:
                # la parcelle d'en face n'a AUCUNE piece emise (lamelle
                # reduite a moins de 4 sommets par la deduplication au mm) :
                # la frontiere est tracable, elle n'est pas silencieuse
                r["vers_parcelle_non_emise"] = vers
                n_vers[0] += 1
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

        # ---- ASSIETTES et TERRASSEMENTS de la cellule ---------------------
        ids_c = set(r["id"] for r in L)
        AS = sorted([a for a in ASSIETTES if a["parcelle"] in ids_c],
                    key=lambda a: a["parcelle"])
        d = dict(ent)
        d["regle"] = ("emprise d'appui d'un objet declare ; le nivellement la "
                      "regle a la cote de l'objet")
        d["assiettes"] = AS
        fichiers["plan_assiettes_%s.json" % nom] = ecrire(
            os.path.join(DATA, "plan_assiettes_%s.json" % nom), d)
        TE = sorted([t for t in TERR
                     if t["regle"] in ids_c or t["terrain"] in ids_c],
                    key=lambda t: (t["regle"], t["terrain"]))
        d = dict(ent)
        d["regle"] = ("emprise de raccordement entre un element regle et le "
                      "terrain : talus si la place existe, soutenement sinon, "
                      "quai contre l'eau")
        d["terrassements"] = TE
        fichiers["plan_terrassements_%s.json" % nom] = ecrire(
            os.path.join(DATA, "plan_terrassements_%s.json" % nom), d)
        par_cell[nom] = {"parcelles": len(L), "interfaces": len(I),
                         "instances": len(S), "assiettes": len(AS),
                         "terrassements": len(TE)}
        if (n + 1) % 10 == 0:
            jalon("C8/  export : %d / %d cellules (%.0f s)"
                  % (n + 1, len(cells), time.time() - t2))
    chrono("C8/export", time.time() - t2, "%d fichiers" % len(fichiers))

    if cellules_filtre:
        tot = sum(v["octets"] for v in fichiers.values())
        with io.open(os.path.join(DATA, "plan_index_partiel.json"), "w",
                     encoding="utf-8", newline="\n") as f:
            f.write(json.dumps({
                "mode": "iteration",
                "avertissement": "index PARTIEL : la cellule porteuse et les "
                                 "comptes ne valent que pour les cellules "
                                 "listees ; seule une passe complete fait foi",
                "cellules": sorted(cellules_filtre),
                "fichiers": fichiers}, indent=1, sort_keys=True))
        jalon("C8/ITERATION terminee : %d fichiers, %.2f Mo, index PARTIEL "
              "ecrit (plan_index.json inchange)" % (len(fichiers), tot / 1e6))
        chrono("C8 iteration", time.time() - t0, "%d cellules" % len(cells))
        return fichiers, None

    # ---- LE REGISTRE DES REGLES ENTRE AU CONTRAT ---------------------------
    # Une mesure de conformite doit etre rejouable depuis le SEUL contrat :
    # sans le registre, le lecteur connait les valeurs mais pas les regles.
    try:
        _reg = json.load(io.open(os.path.join(CACHE, "l1a_matrice.json"),
                                 encoding="utf-8"))
        _rj = {"version": "registre/v1",
               "note": "les regles que le plan applique, avec leur enonce, "
                       "leur provenance, leur reference, leur invariant et la "
                       "mesure qui le verifie. Le contrat se suffit ainsi a "
                       "lui-meme : une conformite se rejoue sans le "
                       "compilateur.",
               "contrats": _reg.get("contrats"),
               "familles": _reg.get("familles"),
               "regles": _reg.get("registre"),
               "compteurs_provenance": _reg.get("compteurs_provenance")}
        fichiers["registre.json"] = ecrire(
            os.path.join(DATA, "registre.json"), _rj)
        n_reg = len(_rj["regles"] or [])
    except Exception as _e:
        n_reg = 0
        jalon("C8/⚠️ registre non exporte (%s)" % _e)

    # ---- LE JUGE : 0 piece batie sans hauteur ------------------------------
    sans_h = []
    for cx, cy in cells:
        nom2 = "%d_%d" % (cx, cy)
        try:
            d2 = json.load(io.open(os.path.join(
                DATA, "plan_qui_%s.json" % nom2), encoding="utf-8"))
        except Exception:
            continue
        for r2 in d2["parcelles"]:
            if r2.get("famille") == "batiment" and r2.get("hauteur_m") is None:
                sans_h.append(r2["id"])
    jalon("C8/⭐ JUGE DES HAUTEURS : %d piece(s) batie(s) sans hauteur "
          "(cible 0)%s ; registre exporte au contrat (%d regles)"
          % (len(sans_h), (" : %s" % sans_h[:5]) if sans_h else "", n_reg))

    # ---- LE JUGE de la convention de decoupe -------------------------------
    porteuse_orpheline = sorted(pid for pid, c in porteuse.items()
                                if c not in aires.get(pid, {}))
    reconstitue = len(set(porteuse.values() and porteuse.keys()))
    juges = {
        "porteuse_sans_piece_n": len(porteuse_orpheline),
        "porteuse_sans_piece_ids": porteuse_orpheline[:20],
        "parcelles_du_plan": len(P),
        "parcelles_emises": len(ids_parc),
        "parcelles_sans_piece": len(sans_piece),
        "comptabilite_fermee": bool(len(P) == len(ids_parc) + len(sans_piece)),
        "porteuses_distinctes_reconstituees": reconstitue}
    jalon("C8/⭐ JUGE DE LA CONVENTION : %d parcelle(s) dont la porteuse ne "
          "porte aucune piece (cible 0) | comptabilite : %d parcelles du plan "
          "= %d emises + %d sans piece -> %s"
          % (len(porteuse_orpheline), len(P), len(ids_parc), len(sans_piece),
             "FERMEE" if len(P) == len(ids_parc) + len(sans_piece)
             else "OUVERTE"))

    # ---- l'index ----------------------------------------------------------
    fam = {}
    for k, v in fichiers.items():
        parts = k.split("_")
        f = parts[1] if len(parts) > 2 else "registre"
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
        "frontieres_vers_parcelle_non_emise": n_vers[0],
        "parcelles_sans_piece_exclues": {
            "n": len(sans_piece), "ids": sans_piece,
            "cause": "lamelle reduite a moins de 4 sommets par la "
                     "deduplication a 1 mm"},
        "champs_ajoutes_L1b9": [
            "emprise_objet (+ emprise_objet_m2) : la forme entiere que le "
            "side-car declare pour l'ouvrage, non rognee par la preseance ; "
            "vaut la chaine \"sans_objet\" pour les parcelles de sol "
            "attribuees a un ouvrage sans objet declare (bandes bnd/, "
            "bouchons tro/)",
            "assiettes : cote synchronisee avec la loi (une seule verite)"],
        "champs_ajoutes_L1b4": [
            "famille (famille de matrice, 19 peuplees)",
            "hauteur_m (bati, de la donnee attestee par empreintes_sources)",
            "epaisseur_tablier_m / cote_intrados_m / portee_m (ponts)",
            "reclasse (voie pietonne hors norme declaree emmarchement)"],
        "familles_peuplees": sorted(set(v for v in FAM.values() if v)),
        "regle_cellule_porteuse": "cellule ou la piece a la plus grande aire ; "
                                  "a egalite d'aire, le plus petit nom de "
                                  "cellule. Calculee sur les pieces REELLEMENT "
                                  "emises.",
        "juges": juges,
        "registre": {"fichier": "registre.json", "regles": n_reg},
        "pieces_batie_sans_hauteur": {"n": len(sans_h),
                                      "ids": sorted(sans_h)[:20]},
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
    import sys as _s
    # ⚠️ un nom de cellule commence souvent par un moins : ce sont des noms,
    # pas des options.
    _f = [a for a in _s.argv[1:] if a and "_" in a]
    main(_f or None)
