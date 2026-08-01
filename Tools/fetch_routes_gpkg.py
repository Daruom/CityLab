# fetch_routes_gpkg.py -- LE FETCH ROUTES, DIRECTEMENT DANS LE GPKG BD TOPO.
#
# CE QU'IL REMPLACE
# -----------------
# 1. SourceData/GrandFetch/Fetch-GF-RoutesBDTopo.ps1 : WFS Geoplateforme, 1000
#    objets par requete a ~1,2 s d'intervalle, 18 attributs sur 88, PAS de cleabs
#    (la liste noire $GFPropBlacklist de GF-Common.ps1 l. 235 l'exclut), pas
#    d'amenagement cyclable, pas d'id_ban_odonyme. Sur les 137 000 troncons de la
#    metropole, ce chemin demanderait ~3 minutes de reseau dans le meilleur cas et
#    depend de la disponibilite du service.
# 2. C:/LidarPoC/work/E2SOL1/fetch_routes_bdtopo.py : meme WFS, puis map_type()
#    (l. 54-69) ECRASE 12 attributs sur 14 pour ne garder qu'un type OSM-like.
#
# AUCUN DES DEUX N'EST SUPPRIME. Ils restent utilisables (ils ne demandent pas les
# 2,8 Gio du GPKG). Ce script est le chemin PRIVILEGIE : meme donnee IGN, meme
# repere local, meme format de sortie, plus rien d'appauvri, et 2 s au lieu de 3 min.
#
# DEUX SORTIES, TOUJOURS ECRITES ENSEMBLE
# ---------------------------------------
#   (a) COMPATIBLE : le format que le pipeline consomme AUJOURD'HUI, cle pour cle
#       (Tools/j3c_sols_corridor.py lit "troncons"[].{pts,nature,importance,
#       largeur_de_chaussee,...}). Zero rupture : on peut ecraser
#       SourceData/GrandFetch/routes_bdtopo.json avec.
#   (b) RICHE : toutes les colonnes metier du GPKG, cleabs COMPRIS. C'est la source
#       de repli de Tools/build_reseau.py sur une machine qui n'a pas le GPKG.
#
# Usage :
#   python fetch_routes_gpkg.py --selftest
#   python fetch_routes_gpkg.py --half 5000                  # carre 10 km
#   python fetch_routes_gpkg.py --half 450 --nom proto       # emprise proto
#   python fetch_routes_gpkg.py --bbox -2000 -1000 3000 4000 --nom quartier
#   python fetch_routes_gpkg.py --controle                   # non-regression vs WFS
import argparse
import json
import math
import os
import sqlite3
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_local as G

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GF = os.path.join(ROOT, "SourceData", "GrandFetch")
OUT_DIR = os.path.join(ROOT, "SourceData", "Reseau")
LOG_PATH = os.path.join(OUT_DIR, "fetch_routes_gpkg.progress.log")
REF_WFS = os.path.join(GF, "routes_bdtopo.json")

GPKG = os.path.join(ROOT, "SourceData", "Agglo", "BDTOPO",
                    "BDTOPO_3-5_TOUSTHEMES_GPKG_LAMB93_D031_2026-06-15", "BDTOPO",
                    "1_DONNEES_LIVRAISON_2026-06-00418",
                    "BDT_3-5_GPKG_LAMB93_D031_ED2026-06-15",
                    "BDT_3-5_GPKG_LAMB93_D031-ED2026-06-15.gpkg")
MILLESIME = "BD TOPO 3.5 GPKG LAMB93 D031 ED2026-06-15 (IGN, Licence Ouverte 2.0)"

# (a) Les 18 cles de Fetch-GF-RoutesBDTopo.ps1 l. 20-23, DANS LEUR ORDRE.
KEYS_COMPAT = ["nature", "importance", "largeur_de_chaussee", "nombre_de_voies",
               "sens_de_circulation", "position_par_rapport_au_sol", "fictif",
               "prive", "urbain", "etat_de_l_objet", "acces_vehicule_leger",
               "acces_pieton", "reserve_aux_bus", "vitesse_moyenne_vl",
               "cpx_classement_administratif", "cpx_numero", "nom_voie_ban_gauche",
               "nom_collaboratif_gauche"]

# (b) Colonnes ECARTEES de la sortie riche : metadonnees d'acquisition, sans usage
# pour le rendu ni pour le trafic. TOUT LE RESTE PART, cleabs en tete.
EXCLU_RICHE = {"fid", "geometrie", "date_creation", "date_modification",
               "date_d_apparition", "date_de_confirmation", "sources",
               "identifiants_sources", "methode_d_acquisition_planimetrique",
               "methode_d_acquisition_altimetrique", "precision_planimetrique",
               "precision_altimetrique"}

SEUIL_AMINCI_M = 1.0        # meme seuil que GFThinLine (GF-Common.ps1 l. 191)


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def colonnes_gpkg(con, table="troncon_de_route"):
    return [r[1] for r in con.execute("PRAGMA table_info(%s)" % table).fetchall()]


def amincir(pts):
    """Reproduit GFThinLine : arrondi CENTIMETRIQUE puis seuil de MANHATTAN a 1 m,
    dernier point TOUJOURS conserve (c'est lui qui porte la connectivite)."""
    r = [(round(p[0], 2), round(p[1], 2)) for p in pts]
    out = []
    prev = None
    for p in r:
        if prev is not None and abs(p[0] - prev[0]) + abs(p[1] - prev[1]) < SEUIL_AMINCI_M:
            continue
        out.append(p)
        prev = p
    if len(r) >= 2 and out:
        last = r[-1]
        if abs(last[0] - out[-1][0]) + abs(last[1] - out[-1][1]) > 0.001:
            out.append(last)
    return out


def _clip_t(p, q, lo, hi, i):
    """Parametres d'entree/sortie d'un segment dans une bande [lo,hi] sur l'axe i."""
    d = q[i] - p[i]
    if abs(d) < 1e-12:
        return (0.0, 1.0) if lo <= p[i] <= hi else None
    t0 = (lo - p[i]) / d
    t1 = (hi - p[i]) / d
    return (min(t0, t1), max(t0, t1))


def touche_rect(pts, x0, y0, x1, y1):
    """La polyligne intersecte-t-elle le rectangle ferme ? (semantique BBOX du WFS :
    on garde le troncon ENTIER des qu'il mord la fenetre.)"""
    for i in range(len(pts) - 1):
        p, q = pts[i], pts[i + 1]
        tx = _clip_t(p, q, x0, x1, 0)
        if tx is None:
            continue
        ty = _clip_t(p, q, y0, y1, 1)
        if ty is None:
            continue
        a = max(0.0, tx[0], ty[0])
        b = min(1.0, tx[1], ty[1])
        if a <= b:
            return True
    return False


def brut(v):
    """Valeur JSON : les booleens SQLite (0/1) redeviennent des booleens ; les
    chaines vides deviennent nulles (comme GFAppendPropsCount qui les saute)."""
    if v is None:
        return None
    if isinstance(v, str):
        v = v.strip()
        return v if v else None
    if isinstance(v, float):
        return round(v, 3)
    return v


def extraire(bbox, cols_riche):
    """Troncons dont la geometrie MORD la fenetre locale. -> [(props, pts_locaux)]"""
    x0, y0, x1, y1 = bbox
    bx0, by0, bx1, by1 = G.local_bbox_to_lamb93(x0, y0, x1, y1, 200.0)
    con = sqlite3.connect("file:%s?mode=ro" % GPKG.replace("\\", "/"), uri=True)
    dispo = colonnes_gpkg(con)
    cols = [c for c in cols_riche if c in dispo]
    sql = ("SELECT t.geometrie, " + ", ".join("t." + c for c in cols) +
           " FROM troncon_de_route t"
           " JOIN rtree_troncon_de_route_geometrie r ON r.id = t.fid"
           " WHERE r.maxx >= ? AND r.minx <= ? AND r.maxy >= ? AND r.miny <= ?")
    lus = 0
    for row in con.execute(sql, (bx0, bx1, by0, by1)):
        lus += 1
        props = {c: brut(v) for c, v in zip(cols, row[1:])}
        for ligne in G.gpkg_lines(row[0]):
            loc = [G.lamb93_to_local(x, y) for (x, y, _z) in ligne]
            if not touche_rect(loc, x0, y0, x1, y1):
                continue
            yield props, loc
    con.close()
    log("  fenetre Lambert 93 [%.0f %.0f -> %.0f %.0f] : %d lignes candidates lues"
        % (bx0, by0, bx1, by1, lus))


def fetch(bbox, nom):
    x0, y0, x1, y1 = bbox
    con = sqlite3.connect("file:%s?mode=ro" % GPKG.replace("\\", "/"), uri=True)
    cols_riche = [c for c in colonnes_gpkg(con) if c not in EXCLU_RICHE]
    con.close()
    log("sortie riche : %d colonnes conservees sur 88 (%d ecartees : metadonnees "
        "d'acquisition)" % (len(cols_riche), 88 - len(cols_riche)))

    compat, riche = [], []
    fill_c, fill_r = Counter(), Counter()
    nat = Counter()
    long_tot = 0.0
    n_geo = 0
    t0 = time.time()
    for props, loc in extraire(bbox, cols_riche):
        pts = amincir(loc)
        if len(pts) < 2:
            continue
        n_geo += 1
        nat[props.get("nature")] += 1
        long_tot += sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
                        for i in range(len(pts) - 1))
        oc = {"pts": [[p[0], p[1]] for p in pts]}
        for k in KEYS_COMPAT:
            v = props.get(k)
            if v is None:
                continue
            oc[k] = v
            fill_c[k] += 1
        compat.append(oc)
        orr = {"pts": [[p[0], p[1]] for p in pts]}
        for k in cols_riche:
            v = props.get(k)
            if v is None:
                continue
            orr[k] = v
            fill_r[k] += 1
        riche.append(orr)
    log("%d polylignes retenues, %.1f km, %.1f s" % (n_geo, long_tot / 1000.0, time.time() - t0))

    sx, sy = x1 - x0, y1 - y0
    entete = {"source": "GPKG " + MILLESIME,
              "origin": {"lat": G.LAT0, "lon": G.LON0},
              "sizeM": {"x": round(sx, 1), "y": round(sy, 1)},
              "bboxLocal": [x0, y0, x1, y1]}
    os.makedirs(OUT_DIR, exist_ok=True)
    pa = os.path.join(OUT_DIR, "routes_bdtopo_gpkg_%s.json" % nom)
    pb = os.path.join(OUT_DIR, "routes_riche_%s.json" % nom)
    with open(pa, "w", encoding="utf-8") as f:
        d = dict(entete)
        d["format"] = "compatible SourceData/GrandFetch/routes_bdtopo.json (18 cles)"
        d["troncons"] = compat
        json.dump(d, f, ensure_ascii=False, separators=(",", ":"))
    with open(pb, "w", encoding="utf-8") as f:
        d = dict(entete)
        d["format"] = "riche : toutes les colonnes metier du GPKG, cleabs compris"
        d["colonnes"] = cols_riche
        d["troncons"] = riche
        json.dump(d, f, ensure_ascii=False, separators=(",", ":"))
    log("ECRIT (a) %s  (%.2f Mo)" % (pa, os.path.getsize(pa) / 1048576.0))
    log("ECRIT (b) %s  (%.2f Mo)" % (pb, os.path.getsize(pb) / 1048576.0))
    return {"n": n_geo, "km": long_tot / 1000.0, "natures": dict(nat.most_common()),
            "fill_compat": dict(fill_c), "fill_riche": dict(fill_r.most_common()),
            "path_compat": pa, "path_riche": pb}


# --- controle de non-regression ------------------------------------------------
def controle(nom="carre10"):
    pa = os.path.join(OUT_DIR, "routes_bdtopo_gpkg_%s.json" % nom)
    if not os.path.exists(pa):
        raise SystemExit("lance d'abord : fetch_routes_gpkg.py --half 5000")
    neuf = json.load(open(pa, encoding="utf-8"))["troncons"]
    vieux = json.load(open(REF_WFS, encoding="utf-8"))["troncons"]

    def mesure(tr):
        nat = Counter()
        fill = Counter()
        L = 0.0
        larg = []
        for t in tr:
            nat[t.get("nature")] += 1
            for k in KEYS_COMPAT:
                if t.get(k) is not None:
                    fill[k] += 1
            p = t["pts"]
            L += sum(math.hypot(p[i + 1][0] - p[i][0], p[i + 1][1] - p[i][1])
                     for i in range(len(p) - 1))
            if isinstance(t.get("largeur_de_chaussee"), (int, float)):
                larg.append(t["largeur_de_chaussee"])
        return {"n": len(tr), "km": L / 1000.0, "natures": nat, "fill": fill,
                "larg_n": len(larg), "larg_moy": (sum(larg) / len(larg)) if larg else 0.0}

    a, b = mesure(neuf), mesure(vieux)
    lignes = []
    lignes.append("GPKG %s  : %d troncons, %.1f km" % (nom, a["n"], a["km"]))
    lignes.append("WFS (ref): %d troncons, %.1f km" % (b["n"], b["km"]))
    lignes.append("ecart     : %+d troncons (%+.2f %%), %+.1f km (%+.2f %%)"
                  % (a["n"] - b["n"], 100.0 * (a["n"] - b["n"]) / b["n"],
                     a["km"] - b["km"], 100.0 * (a["km"] - b["km"]) / b["km"]))
    lignes.append("largeur_de_chaussee : GPKG %d remplies (moy %.2f m) / WFS %d (moy %.2f m)"
                  % (a["larg_n"], a["larg_moy"], b["larg_n"], b["larg_moy"]))
    lignes.append("")
    lignes.append("%-26s %8s %8s %8s" % ("nature", "GPKG", "WFS", "ecart"))
    for k in sorted(set(a["natures"]) | set(b["natures"]),
                    key=lambda k: -b["natures"].get(k, 0)):
        lignes.append("%-26s %8d %8d %+8d"
                      % (k, a["natures"].get(k, 0), b["natures"].get(k, 0),
                         a["natures"].get(k, 0) - b["natures"].get(k, 0)))
    lignes.append("")
    lignes.append("%-32s %8s %8s" % ("remplissage par cle (%)", "GPKG", "WFS"))
    for k in KEYS_COMPAT:
        lignes.append("%-32s %8.1f %8.1f"
                      % (k, 100.0 * a["fill"].get(k, 0) / max(1, a["n"]),
                         100.0 * b["fill"].get(k, 0) / max(1, b["n"])))
    txt = "\n".join(lignes)
    print(txt)
    p = os.path.join(OUT_DIR, "controle_non_regression_%s.txt" % nom)
    with open(p, "w", encoding="utf-8") as f:
        f.write(txt + "\n")
    log("controle ecrit : %s" % p)
    return txt


def selftest():
    ok = True

    def chk(nom, cond, detail=""):
        nonlocal ok
        print("  %s %s %s" % ("OK " if cond else "KO ", nom, detail))
        if not cond:
            ok = False

    # amincissement : seuil de Manhattan, dernier point conserve
    pts = [(0.0, 0.0), (0.2, 0.2), (0.9, 0.0), (5.0, 0.0), (5.1, 0.1)]
    a = amincir(pts)
    chk("amincir garde le dernier point", a[-1] == (5.1, 0.1), str(a))
    chk("amincir jette les points a moins d'1 m (Manhattan)", len(a) == 3, str(a))
    chk("amincir conserve le premier point", a[0] == (0.0, 0.0))

    # intersection polyligne / rectangle
    chk("touche_rect : segment traversant",
        touche_rect([(-10.0, 5.0), (10.0, 5.0)], 0.0, 0.0, 6.0, 6.0))
    chk("touche_rect : segment exterieur",
        not touche_rect([(-10.0, 50.0), (10.0, 50.0)], 0.0, 0.0, 6.0, 6.0))
    chk("touche_rect : segment interieur",
        touche_rect([(1.0, 1.0), (2.0, 2.0)], 0.0, 0.0, 6.0, 6.0))
    chk("touche_rect : segment tangent au bord",
        touche_rect([(-5.0, 0.0), (5.0, 0.0)], 0.0, 0.0, 6.0, 6.0))
    chk("touche_rect : segment qui contourne le coin",
        not touche_rect([(7.0, -1.0), (7.0, 9.0)], 0.0, 0.0, 6.0, 6.0))

    # valeurs brutes
    chk("brut('') -> None", brut("") is None)
    chk("brut(' Libre ') -> 'Libre'", brut(" Libre ") == "Libre")
    chk("brut(0) -> 0 (et pas None)", brut(0) == 0)

    # les 18 cles compatibles existent bien dans le GPKG
    if os.path.exists(GPKG):
        con = sqlite3.connect("file:%s?mode=ro" % GPKG.replace("\\", "/"), uri=True)
        cols = set(colonnes_gpkg(con))
        con.close()
        manque = [k for k in KEYS_COMPAT if k not in cols]
        chk("les 18 cles du format compatible existent dans le GPKG", not manque, str(manque))
    print("SELFTEST fetch_routes_gpkg :", "PASS" if ok else "ECHEC")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--half", type=float, default=5000.0)
    ap.add_argument("--bbox", nargs=4, type=float)
    ap.add_argument("--nom")
    ap.add_argument("--controle", action="store_true",
                    help="controle de non-regression APRES le fetch")
    ap.add_argument("--controle-seul", action="store_true",
                    help="controle seul, sur une sortie deja ecrite")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.controle_seul:
        controle(args.nom or "carre10")
        return 0
    if args.bbox:
        bbox = tuple(args.bbox)
        nom = args.nom or "bbox"
    else:
        h = args.half
        bbox = (-h, -h, h, h)
        nom = args.nom or ("carre%d" % int(round(2 * h / 1000.0)))
    log("FETCH %s : bbox locale [%.0f %.0f -> %.0f %.0f] m" % ((nom,) + bbox))
    fetch(bbox, nom)
    if args.controle:
        controle(nom)
    return 0


if __name__ == "__main__":
    sys.exit(main())
