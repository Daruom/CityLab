# fetch_osm_bleachers.py — LOT FINITION QUAIS : les EMPRISES DE GRADINS, en UNE
# seule requete Overpass mise en cache sur disque, puis un side-car par cellule.
#
# CE QUE CETTE COUCHE EST, ET CE QU'ELLE N'EST PAS.
# `leisure=bleachers` est le tag OSM des EMMARCHEMENTS / tribunes. Il donne une
# EMPRISE fiable — un polygone ferme — et RIEN de la geometrie des marches : ni
# nombre de gradins, ni giron, ni contremarche. C'est exactement ce qu'il faut :
# la geometrie reste produite par la regle C++ (`bQuayTiers`), calee sur le
# denivele MESURE sur la surface rendue ; la donnee ne fait que dire OU.
#
# POURQUOI C'EST UN VOCABULAIRE NATIONAL, MESURE (lot DATA, mission B) : une
# requete groupee sur 5 agglomerations (Toulouse, Lyon, Bordeaux, Paris, Nantes)
# rend 99 elements dont 92 polygones fermes, aires p10 14,8 m2 / p50 42,8 m2 /
# p90 391 m2. Ce n'est pas un hapax toulousain.
#
# REGLE AVAL (dans le C++, pas ici) : le mecanisme de gradins ne s'applique qu'a
# la PORTION d'un mur de classe `quai` qui tombe dans un de ces polygones. Hors
# emprise, le mur reste lisse. AUCUN identifiant OSM ne circule dans la regle :
# la verite locale vit dans le verrou nominatif (work/FINQUAIS/f_verrou_gradins.py).
#
# La regle « zero fetch reseau en lot » (Playbook S6) reste vraie : ce script
# tourne UNE FOIS, les passes aval ne lisent que les fichiers.
#
# SORTIES
#   1. cache brut          SourceData/Reseau/osm_bleachers_<nom>.json
#   2. side-car par cellule SourceData/Gradins/gradins_<cx>_<cy>.json
#      { cell, cellSizeM, origin, crs, axes, source, recette, stats,
#        gradins: [ { id, aire_m2, pts: [[x,y], ...] } ] }
#      Meme contrat que Ponts/ et Murs/ : dossier absent ou cellule sans fichier
#      = AUCUN gradin, sans erreur.
#
# Usage :
#   python fetch_osm_bleachers.py --half 5000 --nom carre10      # le fetch
#   python fetch_osm_bleachers.py --sidecar --half 1500          # le side-car 3x3
#   python fetch_osm_bleachers.py --selftest
import argparse
import json
import math
import os
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "pylib"))
import geo_local as G                                                 # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "SourceData", "Reseau")
SIDECAR_DIR = os.path.join(ROOT, "SourceData", "Gradins")
LOG_PATH = os.path.join(OUT_DIR, "osm_bleachers.progress.log")
CELL_M = 500.0
AIRE_MIN_M2 = 5.0          # plancher de bon sens : sous 5 m2 ce n'est pas une emprise

MIRRORS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
]


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def aire(pts):
    """Aire d'un anneau ferme (valeur absolue), en m2."""
    s = 0.0
    for i in range(len(pts) - 1):
        s += pts[i][0] * pts[i + 1][1] - pts[i + 1][0] * pts[i][1]
    return abs(s) * 0.5


def fetch(half_m):
    lon0, lat0 = G.local_to_wgs84(-half_m, half_m)
    lon1, lat1 = G.local_to_wgs84(half_m, -half_m)
    bbox = "(%f,%f,%f,%f)" % (lat0, lon0, lat1, lon1)
    query = ("[out:json][timeout:180];("
             'way["leisure"="bleachers"]' + bbox + ";"
             'relation["leisure"="bleachers"]' + bbox + ";"
             ");out geom;")
    data = None
    for url in MIRRORS:
        try:
            log("Overpass %s  bbox %s" % (url, bbox))
            req = urllib.request.Request(
                url, data=("data=" + urllib.parse.quote(query)).encode("ascii"),
                headers={"User-Agent": "CityLab-LotFINQUAIS/1.0"})
            with urllib.request.urlopen(req, timeout=300) as r:
                raw = r.read()
            log("  %d octets recus" % len(raw))
            if len(raw) < 400 and b"<" in raw[:20]:
                log("  reponse suspecte (%d octets), miroir suivant" % len(raw))
                continue
            data = json.loads(raw.decode("utf-8"))
            break
        except Exception as ex:  # noqa: BLE001
            log("  echec : %s" % ex)
            time.sleep(3.0)
    if data is None:
        raise SystemExit("tous les miroirs Overpass ont echoue")
    return data


def convertir(data):
    """Elements Overpass -> polygones FERMES en repere local. Les ways ouverts
    sont ECARTES et comptes : une emprise est une surface."""
    out, rejets = [], {"ouvert": 0, "trop_petit": 0, "sans_geom": 0}
    for el in data.get("elements", []):
        geom = el.get("geometry") or []
        if len(geom) < 4:
            rejets["sans_geom"] += 1
            continue
        pts = []
        for g in geom:
            x, y = G.wgs84_to_local(g["lon"], g["lat"])
            pts.append([round(x, 2), round(y, 2)])
        if pts[0] != pts[-1]:
            rejets["ouvert"] += 1
            continue
        a = aire(pts)
        if a < AIRE_MIN_M2:
            rejets["trop_petit"] += 1
            continue
        rec = {"id": "%s/%s" % (el.get("type"), el.get("id")), "aire_m2": round(a, 1),
               "pts": pts}
        tags = el.get("tags") or {}
        for t in ("surface", "name", "material"):
            if t in tags:
                rec[t] = tags[t]
        out.append(rec)
    log("emprises fermees retenues : %d (ecartees : %s)" % (len(out), rejets))
    return out, rejets


def ecrire_sidecar(emprises, half_m):
    """Un fichier par cellule de 500 m, comme Ponts/ et Murs/. Une emprise est
    rangee dans la cellule de son CENTROIDE (elle n'est PAS decoupee : le C++
    teste l'appartenance de points, un polygone tronque fausserait le test) ;
    elle est donc aussi copiee dans les cellules VOISINES qu'elle touche."""
    os.makedirs(SIDECAR_DIR, exist_ok=True)
    n = int(math.ceil(half_m / CELL_M))
    par_cell = {}
    for e in emprises:
        xs = [p[0] for p in e["pts"]]
        ys = [p[1] for p in e["pts"]]
        cx0, cx1 = int(math.floor(min(xs) / CELL_M)), int(math.floor(max(xs) / CELL_M))
        cy0, cy1 = int(math.floor(min(ys) / CELL_M)), int(math.floor(max(ys) / CELL_M))
        for cx in range(cx0, cx1 + 1):
            for cy in range(cy0, cy1 + 1):
                par_cell.setdefault((cx, cy), []).append(e)
    ecrits = 0
    for cx in range(-n, n):
        for cy in range(-n, n):
            liste = par_cell.get((cx, cy), [])
            d = {
                "cell": [cx, cy], "cellSizeM": CELL_M,
                "origin": [cx * CELL_M, cy * CELL_M],
                "crs": "local_citylab_equirect_capitole",
                "axes": "x = est (m), y = sud (m) ; NORD = -Y",
                "source": "OpenStreetMap, leisure=bleachers (ODbL : attribution + partage a l'identique)",
                "recette": {
                    "regle": "EMPRISE seule : le polygone dit OU des gradins existent ; "
                             "le nombre de gradins, le giron et la contremarche restent "
                             "produits par la regle C++ sur le denivele MESURE",
                    "aire_min_m2": AIRE_MIN_M2,
                    "polygones_fermes_seulement": True,
                    "note": "une emprise qui deborde est ecrite dans CHAQUE cellule qu'elle "
                            "touche, entiere (jamais decoupee) : le test d'appartenance "
                            "d'un point exige le polygone complet",
                },
                "stats": {"gradins": len(liste),
                          "aire_m2": round(sum(g["aire_m2"] for g in liste), 1)},
                "gradins": liste,
            }
            with open(os.path.join(SIDECAR_DIR, "gradins_%d_%d.json" % (cx, cy)),
                      "w", encoding="utf-8") as f:
                json.dump(d, f, ensure_ascii=False, separators=(",", ":"))
            ecrits += 1
    nb = sum(len(v) for v in par_cell.values())
    log("side-car : %d fichiers ecrits dans %s ; %d emprises placees (%d references "
        "cellule)" % (ecrits, SIDECAR_DIR, len(emprises), nb))
    return ecrits


def selftest():
    ok = True

    def check(nom, a, b, tol=0.0):
        nonlocal ok
        bon = abs(a - b) <= tol
        ok = ok and bon
        log("selftest %-52s attendu %10.3f obtenu %10.3f  %s"
            % (nom, b, a, "OK" if bon else "ECHEC"))

    carre = [[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]
    check("aire d'un carre de 10 m", aire(carre), 100.0, 1e-6)
    check("aire invariante au sens de parcours", aire(carre[::-1]), 100.0, 1e-6)
    # Un way OUVERT n'est pas une emprise, un polygone minuscule non plus.
    faux = {"elements": [
        {"type": "way", "id": 1, "geometry": [{"lon": 1.44, "lat": 43.60},
                                              {"lon": 1.441, "lat": 43.60},
                                              {"lon": 1.441, "lat": 43.601}]},
    ]}
    out, rej = convertir(faux)
    check("un way ouvert de 3 points est ecarte", float(len(out)), 0.0)
    log("SELFTEST : %s" % ("PASS" if ok else "ECHEC"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--half", type=float, default=5000.0)
    ap.add_argument("--nom", default="carre10")
    ap.add_argument("--sidecar", action="store_true",
                    help="ecrit seulement le side-car depuis le cache")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    dest = os.path.join(OUT_DIR, "osm_bleachers_%s.json" % args.nom)
    if args.sidecar:
        if not os.path.exists(dest):
            raise SystemExit("cache absent : %s (lance d'abord le fetch)" % dest)
        cache = json.load(open(dest, encoding="utf-8"))
        emprises = cache["emprises"]
        log("cache lu : %d emprises" % len(emprises))
        ecrire_sidecar([e for e in emprises
                        if abs(e["pts"][0][0]) <= args.half + 1000.0
                        and abs(e["pts"][0][1]) <= args.half + 1000.0], args.half)
        return 0
    if os.path.exists(dest) and not args.force:
        log("CACHE deja present : %s (%d ko) — rien a faire (--force pour refaire)"
            % (dest, os.path.getsize(dest) // 1024))
        return 0
    t0 = time.time()
    data = fetch(args.half)
    emprises, rejets = convertir(data)
    out = {
        "source": "OSM via Overpass (ODbL — attribution + partage a l'identique)",
        "requete": "way|relation[leisure=bleachers] bbox",
        "fetch": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "bbox_local": [-args.half, -args.half, args.half, args.half],
        "crs": "local_citylab_equirect_capitole (x=est, y=SUD, m)",
        "rejets": rejets,
        "emprises": emprises,
    }
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(dest, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    log("ECRIT %s (%d ko) en %.1f s" % (dest, os.path.getsize(dest) // 1024,
                                        time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
