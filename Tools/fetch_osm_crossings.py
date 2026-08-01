# fetch_osm_crossings.py — LOT FINITION_SOL : les PASSAGES PIETONS reels (OSM).
#
# POURQUOI : la couche « passage pieton » du sol a ete COUPEE le 2026-07-30
# (CROSSINGS_ON = False) parce que ses sites etaient DEVINES — « un axe pieton OSM
# croise la chaussee, donc il y avait probablement un passage la » — et la mesure
# lui a donne tort (375 des 383 sites du km2 proto debordaient de la chaussee,
# 32 mordaient un batiment). La doctrine est restee : « mieux vaut pas de marquage
# qu'un marquage invente ».
# OSM porte la VRAIE donnee : le noeud `highway=crossing` est pose SUR l'axe de la
# chaussee, a l'endroit exact du passage, et 66 % d'entre eux disent eux-memes s'ils
# sont zebres (`crossing:markings`). C'est la seule source nationale (mesure de
# l'analyse : 10 236 noeuds sur le carre 10 km, dont 6 727 marquages explicites).
#
# DECISION UTILISATEUR VERROUILLEE : on ne peint QUE la ou OSM tague un passage ;
# `crossing:markings=no` -> PAS de peinture ; la sous-couverture des petites
# communes est acceptee (degradation silencieuse et sure, jamais un passage
# invente).
#
# CACHE DISQUE OBLIGATOIRE (Playbook §6 : zero fetch reseau en lot). Le brut
# Overpass est ecrit dans SourceData/Reseau/_cache/ ; toute execution ulterieure le
# relit. `--refetch` force le reseau.
#
# SORTIE : SourceData/Reseau/osm_crossings_<nom>.json
#   { "source":..., "fetch":..., "bbox_local":[...], "crs":...,
#     "noeuds":[ {"id":1, "p":[x,y], "markings":"zebra|yes|no|null",
#                 "crossing":"marked|uncontrolled|traffic_signals|...",
#                 "peint": true|false } ],
#     "ways":[ {"id":1, "pts":[[x,y],...], "markings":..., "peint":...} ] }
#   pts/p en REPERE LOCAL CityLab (x = est, y = SUD, metres) — convention gelee.
#
# LICENCE : OSM = ODbL (attribution + share-alike sur la base derivee). Le projet
# consomme deja OSM massivement (toulouse10.json) — pas une nouvelle dependance.
#
# Usage :
#   python fetch_osm_crossings.py --half 5000 --nom carre10   # le carre 10 km
#   python fetch_osm_crossings.py --half 700  --nom proto     # controle proto
#   python fetch_osm_crossings.py --selftest                  # verrous du filtre
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
CACHE_DIR = os.path.join(OUT_DIR, "_cache")
LOG_PATH = os.path.join(OUT_DIR, "osm_crossings.progress.log")

MIRRORS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
]

# --- LE FILTRE, et rien d'autre.
# `crossing:markings` dit explicitement si le passage porte un marquage au sol.
# On ne peint QUE si la donnee l'affirme, et on refuse quand elle le NIE.
MARKINGS_NON = {"no", "none", "surface"}          # explicitement NON marque
# valeurs de `crossing` qui, a elles seules, affirment un marquage (vieux tagging)
CROSSING_MARQUE = {"marked", "zebra", "traffic_signals"}
# `crossing` qui NIE le marquage (passage sans zebra, simple abaissement)
CROSSING_NON = {"unmarked", "informal", "no"}


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def est_peint(tags):
    """VRAI si OSM affirme un marquage au sol. Regle unique, sans exception :
      1. `crossing:markings` present et hors {no, none, surface} -> PEINT ;
      2. `crossing:markings` dans {no, none, surface}            -> NON PEINT ;
      3. absent : on retombe sur `crossing` (tagging historique) — `marked`,
         `zebra`, `traffic_signals` affirment le zebra ; `unmarked`, `informal`
         nient ; tout le reste (`uncontrolled`, absent, inconnu) est INDECIS.
      4. INDECIS -> NON PEINT. « Mieux vaut pas de marquage qu'un marquage invente. »
    Retourne (peint, motif)."""
    mk = (tags.get("crossing:markings") or "").strip().lower()
    if mk:
        if mk in MARKINGS_NON:
            return False, "markings=%s" % mk
        return True, "markings=%s" % mk
    cr = (tags.get("crossing") or "").strip().lower()
    if cr in CROSSING_MARQUE:
        return True, "crossing=%s" % cr
    if cr in CROSSING_NON:
        return False, "crossing=%s" % cr
    return False, ("crossing=%s" % cr) if cr else "aucun tag de marquage"


def requete(half_m):
    lon0, lat0 = G.local_to_wgs84(-half_m, half_m)     # coin SW (y sud max = lat min)
    lon1, lat1 = G.local_to_wgs84(half_m, -half_m)     # coin NE
    b = "%f,%f,%f,%f" % (lat0, lon0, lat1, lon1)
    return ("[out:json][timeout:180];("
            "node[\"highway\"=\"crossing\"](%s);"
            "way[\"footway\"=\"crossing\"](%s);"
            ");out tags geom;" % (b, b)), b


def fetch(half_m, nom, refetch=False):
    os.makedirs(CACHE_DIR, exist_ok=True)
    cache = os.path.join(CACHE_DIR, "overpass_crossings_%s.json" % nom)
    if os.path.exists(cache) and not refetch:
        log("CACHE : %s (%.2f Mo) — aucun acces reseau"
            % (cache, os.path.getsize(cache) / 1048576.0))
        with open(cache, encoding="utf-8") as f:
            return json.load(f), cache
    q, b = requete(half_m)
    data = None
    for url in MIRRORS:
        try:
            log("Overpass %s (bbox %s)" % (url, b))
            req = urllib.request.Request(
                url, data=urllib.parse.urlencode({"data": q}).encode("ascii"),
                headers={"User-Agent": "CityLab-FinitionSol/1.0"})
            with urllib.request.urlopen(req, timeout=300) as r:
                raw = r.read()
            if len(raw) < 400 and b"<html" in raw.lower():
                log("  reponse HTML suspecte (%d octets), miroir suivant" % len(raw))
                continue
            data = json.loads(raw.decode("utf-8"))
            break
        except Exception as ex:  # noqa: BLE001
            log("  echec : %s" % ex)
            time.sleep(2.0)
    if data is None:
        raise SystemExit("tous les miroirs Overpass ont echoue (et pas de cache)")
    with open(cache, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
    log("cache ecrit : %s (%.2f Mo)" % (cache, os.path.getsize(cache) / 1048576.0))
    return data, cache


def convertir(data, half_m):
    noeuds, ways = [], []
    stats = {"noeuds": 0, "noeuds_peints": 0, "noeuds_non": 0, "ways": 0, "ways_peints": 0}
    motifs = {}
    for el in data.get("elements", []):
        tags = el.get("tags") or {}
        if el.get("type") == "node":
            if tags.get("highway") != "crossing":
                continue
            if "lat" not in el or "lon" not in el:
                continue
            x, y = G.wgs84_to_local(el["lon"], el["lat"])
            if not (-half_m <= x <= half_m and -half_m <= y <= half_m):
                continue
            peint, motif = est_peint(tags)
            stats["noeuds"] += 1
            stats["noeuds_peints" if peint else "noeuds_non"] += 1
            motifs[motif] = motifs.get(motif, 0) + 1
            n = {"id": el["id"], "p": [round(x, 2), round(y, 2)], "peint": peint,
                 "motif": motif}
            if tags.get("crossing:markings"):
                n["markings"] = tags["crossing:markings"]
            if tags.get("crossing"):
                n["crossing"] = tags["crossing"]
            noeuds.append(n)
        elif el.get("type") == "way":
            if tags.get("footway") != "crossing":
                continue
            geom = el.get("geometry") or []
            if len(geom) < 2:
                continue
            pts = []
            for g in geom:
                x, y = G.wgs84_to_local(g["lon"], g["lat"])
                pts.append([round(x, 2), round(y, 2)])
            peint, _m = est_peint(tags)
            stats["ways"] += 1
            if peint:
                stats["ways_peints"] += 1
            w = {"id": el["id"], "pts": pts, "peint": peint}
            if tags.get("crossing:markings"):
                w["markings"] = tags["crossing:markings"]
            if tags.get("crossing"):
                w["crossing"] = tags["crossing"]
            ways.append(w)
    log("noeuds highway=crossing : %d (dont %d PEINTS, %d refuses) ; "
        "ways footway=crossing : %d (dont %d peints)"
        % (stats["noeuds"], stats["noeuds_peints"], stats["noeuds_non"],
           stats["ways"], stats["ways_peints"]))
    for m, n in sorted(motifs.items(), key=lambda kv: -kv[1])[:12]:
        log("   motif %-28s %5d" % (m, n))
    return noeuds, ways, stats


def selftest():
    ok = True

    def chk(nom, tags, att):
        nonlocal ok
        got, motif = est_peint(tags)
        bon = got == att
        ok = ok and bon
        print("selftest %-42s attendu %-5s obtenu %-5s (%-24s) %s"
              % (nom, att, got, motif, "OK" if bon else "*** FAIL ***"))

    chk("markings=zebra -> peint", {"crossing:markings": "zebra"}, True)
    chk("markings=yes -> peint", {"crossing:markings": "yes"}, True)
    chk("markings=lines -> peint", {"crossing:markings": "lines"}, True)
    chk("markings=zebra;yes -> peint", {"crossing:markings": "zebra;yes"}, True)
    chk("markings=NO -> REFUSE (decision verrouillee)", {"crossing:markings": "no"}, False)
    chk("markings=surface -> REFUSE", {"crossing:markings": "surface"}, False)
    chk("markings=no gagne sur crossing=marked",
        {"crossing:markings": "no", "crossing": "marked"}, False)
    chk("crossing=marked seul -> peint", {"crossing": "marked"}, True)
    chk("crossing=zebra seul -> peint", {"crossing": "zebra"}, True)
    chk("crossing=traffic_signals seul -> peint", {"crossing": "traffic_signals"}, True)
    chk("crossing=unmarked -> REFUSE", {"crossing": "unmarked"}, False)
    chk("crossing=informal -> REFUSE", {"crossing": "informal"}, False)
    chk("crossing=uncontrolled (indecis) -> REFUSE", {"crossing": "uncontrolled"}, False)
    chk("aucun tag (indecis) -> REFUSE", {}, False)
    chk("tag inconnu (indecis) -> REFUSE", {"crossing": "bidule"}, False)
    print("SELFTEST fetch_osm_crossings : " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--half", type=float, default=5000.0)
    ap.add_argument("--nom", default="carre10")
    ap.add_argument("--refetch", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    t0 = time.time()
    data, cache = fetch(args.half, args.nom, args.refetch)
    noeuds, ways, stats = convertir(data, args.half)
    out = {
        "source": "OSM via Overpass (ODbL — attribution + share-alike)",
        "requete": "node[highway=crossing] + way[footway=crossing] bbox",
        "cache": os.path.basename(cache),
        "fetch": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "bbox_local": [-args.half, -args.half, args.half, args.half],
        "crs": "local_citylab_equirect_capitole (x=est, y=SUD, m)",
        "regle": "peint = OSM AFFIRME un marquage (crossing:markings hors {no,none,"
                 "surface}, sinon crossing dans {marked,zebra,traffic_signals}) ; "
                 "indecis = NON PEINT",
        "stats": stats,
        "noeuds": noeuds,
        "ways": ways,
    }
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, "osm_crossings_%s.json" % args.nom)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    log("ecrit %s : %d noeuds (%d peints), %d ways, %.2f Mo, %.1f s"
        % (path, len(noeuds), stats["noeuds_peints"], len(ways),
           os.path.getsize(path) / 1048576.0, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
