# fetch_osm_pieton.py — LOT A (A3) : les ways OSM `highway=pedestrian` en repere local.
#
# POURQUOI : le critere BD TOPO strict (`acces_vehicule_leger = Physiquement
# impossible`) rate une partie du plateau pietonnier reel — MESURE : le millesime
# 2026-06 code `rue du Taur` et `rue Saint Rome` en acces `Libre`. Le critere
# pieton du LOT A est donc HYBRIDE : strict BD TOPO ∪ OSM `highway=pedestrian`.
# Ce script fetch la moitie OSM UNE FOIS et la met en cache sur disque : la regle
# « zero fetch reseau en lot » (Playbook §6) reste vraie pour toutes les passes
# aval (build_reseau lit le fichier, jamais le reseau).
#
# SORTIE : SourceData/Reseau/osm_pieton_<nom>.json
#   { "source": "OSM Overpass (ODbL)", "fetch": "...date...", "bbox_local": [...],
#     "ways": [ { "id": 123, "nom": "Rue du Taur", "ferme": true|absent,
#                 "aire": true|absent, "pts": [[x,y],...] } ] }
#   pts en REPERE LOCAL CityLab (x = est, y = SUD, metres) — meme convention que
#   le side-car reseau (geo_local.py, gelee).
#
# LICENCE : OSM = ODbL (attribution + share-alike sur la base derivee). Le projet
# consomme deja OSM massivement (toulouse10.json) — pas une nouvelle dependance,
# mais la couche pietonne hybride en herite (cf. analyse B.3, reserve 2).
#
# Usage :
#   python fetch_osm_pieton.py --half 5000 --nom carre10     # le carre 10 km (defaut)
#   python fetch_osm_pieton.py --half 700  --nom proto       # controle proto seul
import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_local as G

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "SourceData", "Reseau")
LOG_PATH = os.path.join(OUT_DIR, "osm_pieton.progress.log")

# Miroirs Overpass, essayes dans l'ordre. 1 requete au total en nominal.
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


def fetch(half_m):
    # bbox locale -> bbox WGS84 (l'equirectangulaire locale est monotone : les
    # coins suffisent). y = SUD : le lat max est a y = -half.
    lon0, lat0 = G.local_to_wgs84(-half_m, half_m)     # coin SW (y sud max = lat min)
    lon1, lat1 = G.local_to_wgs84(half_m, -half_m)     # coin NE
    s, w, n, e = lat0, lon0, lat1, lon1
    query = (
        "[out:json][timeout:120];"
        "way[\"highway\"=\"pedestrian\"](%f,%f,%f,%f);"
        "out geom;" % (s, w, n, e))
    data = None
    for url in MIRRORS:
        try:
            log("Overpass %s (bbox %.5f,%.5f,%.5f,%.5f)" % (url, s, w, n, e))
            req = urllib.request.Request(
                url, data=("data=" + urllib.request.quote(query)).encode("ascii"),
                headers={"User-Agent": "CityLab-LotA/1.0"})
            with urllib.request.urlopen(req, timeout=180) as r:
                raw = r.read()
            if len(raw) < 200 and b"<html" in raw.lower():
                log("  reponse HTML suspecte (%d octets), miroir suivant" % len(raw))
                continue
            data = json.loads(raw.decode("utf-8"))
            break
        except Exception as ex:  # noqa: BLE001
            log("  echec : %s" % ex)
            time.sleep(2.0)
    if data is None:
        raise SystemExit("tous les miroirs Overpass ont echoue")
    return data


def convertir(data, half_m):
    ways = []
    n_area = n_closed = 0
    for el in data.get("elements", []):
        if el.get("type") != "way":
            continue
        geom = el.get("geometry") or []
        if len(geom) < 2:
            continue
        pts = []
        for g in geom:
            x, y = G.wgs84_to_local(g["lon"], g["lat"])
            pts.append([round(x, 2), round(y, 2)])
        # clip grossier a la fenetre (Overpass rend le way ENTIER des qu'il touche
        # la bbox) : on garde tel quel, le matching aval est spatial de toute facon.
        tags = el.get("tags") or {}
        w = {"id": el["id"], "pts": pts}
        nom = tags.get("name")
        if nom:
            w["nom"] = nom
        ferme = len(pts) >= 4 and pts[0] == pts[-1]
        if ferme:
            w["ferme"] = True
            n_closed += 1
        if tags.get("area") == "yes":
            # les PLACES pietonnes OSM sont des polygones : on les garde marquees —
            # le matcher aval decide (un anneau de place matche les axes qui la
            # traversent, c'est voulu : une place pietonne EST pietonne).
            w["aire"] = True
            n_area += 1
        ways.append(w)
    log("ways highway=pedestrian : %d (dont %d fermes, %d area=yes)"
        % (len(ways), n_closed, n_area))
    return ways


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--half", type=float, default=5000.0)
    ap.add_argument("--nom", default="carre10")
    args = ap.parse_args()
    t0 = time.time()
    data = fetch(args.half)
    ways = convertir(data, args.half)
    out = {
        "source": "OSM via Overpass (ODbL — attribution + share-alike)",
        "requete": "way[highway=pedestrian] bbox",
        "fetch": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "bbox_local": [-args.half, -args.half, args.half, args.half],
        "crs": "local_citylab_equirect_capitole (x=est, y=SUD, m)",
        "ways": ways,
    }
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, "osm_pieton_%s.json" % args.nom)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    km = sum(
        sum(((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2) ** 0.5
            for p, q in zip(w["pts"], w["pts"][1:]))
        for w in ways) / 1000.0
    log("ecrit %s : %d ways, %.1f km d'axes, %.2f Mo, %.1f s"
        % (path, len(ways), km, os.path.getsize(path) / 1048576.0, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
