# fetch_osm_berges.py — LOT QUAIS : le COMPLEMENT OSM des deux passes du lot,
# en UNE seule requete Overpass mise en cache sur disque.
#
# POURQUOI DEUX COUCHES DANS UN SEUL FETCH : Overpass fait payer la latence par
# requete, pas par couche ; et les deux couches du lot QUAIS sont voisines
# geographiquement (les berges) et fonctionnellement (l'escalier RELIE la
# promenade basse au quai haut).
#
#   1. `highway=steps`    -> V2, complement des troncons BD TOPO `nature=Escalier`
#   2. `highway=footway|path|pedestrian` + `area:highway=footway`
#                         -> V3, complement de la promenade basse
#
# MESURE QUI MOTIVE LE MOT « COMPLEMENT » (q0b_sentiers.py, 02/08) : contrairement
# a ce qu'annoncait le brief, BD TOPO PORTE DEJA la promenade basse — 3 209 m de
# `nature=Sentier|Chemin` passent a moins de 25 m d'un mur de classe quai sur
# l'emprise 3x3, dont 1 492 m MESURES du cote bas ; la « Promenade Henri Martin »
# court sur ~750 m du Pont Saint-Pierre a la Daurade. La source NATIONALE reste
# donc BD TOPO ; OSM ne fait que combler ce qu'elle ne code pas.
#
# La regle « zero fetch reseau en lot » (Playbook S6) reste vraie : ce script
# tourne UNE FOIS, les passes aval ne lisent que le fichier.
#
# SORTIE : SourceData/Reseau/osm_berges_<nom>.json
#   { "source", "fetch", "bbox_local", "couches": {"steps": [...], "pieton": [...]} }
#   pts en REPERE LOCAL CityLab (x = est, y = SUD, metres).
#
# Usage :
#   python fetch_osm_berges.py --half 5000 --nom carre10     (defaut)
import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_local as G

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "SourceData", "Reseau")
LOG_PATH = os.path.join(OUT_DIR, "osm_berges.progress.log")

MIRRORS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
]

# Les attributs OSM qui portent une information GEOMETRIQUE utile a la passe
# BuildStairs : nombre de marches, hauteur/giron, largeur, sens de montee.
TAGS_UTILES = ("step_count", "width", "est_width", "incline", "handrail",
               "ramp", "surface", "name", "conveying")


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
    lon0, lat0 = G.local_to_wgs84(-half_m, half_m)     # coin SW (y sud max = lat min)
    lon1, lat1 = G.local_to_wgs84(half_m, -half_m)     # coin NE
    s, w, n, e = lat0, lon0, lat1, lon1
    bbox = "(%f,%f,%f,%f)" % (s, w, n, e)
    query = (
        "[out:json][timeout:180];("
        'way["highway"="steps"]' + bbox + ";"
        'way["highway"~"^(footway|path|pedestrian)$"]' + bbox + ";"
        ");out geom;")
    data = None
    for url in MIRRORS:
        try:
            log("Overpass %s  bbox %s" % (url, bbox))
            req = urllib.request.Request(
                url, data=("data=" + urllib.parse.quote(query)).encode("ascii"),
                headers={"User-Agent": "CityLab-LotQUAIS/1.0"})
            with urllib.request.urlopen(req, timeout=300) as r:
                raw = r.read()
            log("  %d octets recus" % len(raw))
            # Piege paye sur data.geopf.fr et transposable : une page d'erreur HTML
            # courte se deserialise mal et passerait pour « zero resultat ».
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
    couches = {"steps": [], "pieton": []}
    for el in data.get("elements", []):
        if el.get("type") != "way":
            continue
        geom = el.get("geometry") or []
        if len(geom) < 2:
            continue
        tags = el.get("tags") or {}
        hw = tags.get("highway")
        cible = "steps" if hw == "steps" else "pieton"
        pts = []
        for g in geom:
            x, y = G.wgs84_to_local(g["lon"], g["lat"])
            pts.append([round(x, 2), round(y, 2)])
        w = {"id": el["id"], "highway": hw, "pts": pts}
        for t in TAGS_UTILES:
            if t in tags:
                w[t] = tags[t]
        if len(pts) >= 4 and pts[0] == pts[-1]:
            w["ferme"] = True
        if tags.get("area") == "yes":
            w["aire"] = True
        couches[cible].append(w)
    log("steps  : %d ways" % len(couches["steps"]))
    log("pieton : %d ways" % len(couches["pieton"]))
    nsc = sum(1 for w in couches["steps"] if "step_count" in w)
    nw = sum(1 for w in couches["steps"] if "width" in w)
    log("  dont step_count renseigne : %d ; width renseigne : %d" % (nsc, nw))
    return couches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--half", type=float, default=5000.0)
    ap.add_argument("--nom", default="carre10")
    args = ap.parse_args()
    dest = os.path.join(OUT_DIR, "osm_berges_%s.json" % args.nom)
    if os.path.exists(dest) and "--force" not in sys.argv:
        log("CACHE deja present : %s (%d ko) — rien a faire (--force pour refaire)"
            % (dest, os.path.getsize(dest) // 1024))
        return 0
    t0 = time.time()
    data = fetch(args.half)
    couches = convertir(data)
    out = {
        "source": "OSM via Overpass (ODbL — attribution + share-alike)",
        "requete": "way[highway=steps] + way[highway=footway|path|pedestrian] bbox",
        "fetch": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "bbox_local": [-args.half, -args.half, args.half, args.half],
        "crs": "local_citylab_equirect_capitole (x=est, y=SUD, m)",
        "couches": couches,
    }
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(dest, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    log("ECRIT %s (%d ko) en %.1f s" % (dest, os.path.getsize(dest) // 1024, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
