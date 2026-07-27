# j3b_ajoute_cours.py -- ETAPE 2 du chantier COURS : attache les cours interieures
# (SourceData/GrandFetch/cours.json, produit par Fetch-GF-Cours.ps1) au JSON batiments
# SourceData/toulouse10_bati.json (qui contient DEJA rognage + teintes + toits).
#
# Nouvelle cle par batiment concerne : "holes":[[[x,y],...],...] (une liste par cour),
# en coordonnees LOCALES 2 dec, ORIENTEES CW (l'exterieur "pts" est CCW). C'est ce que
# bpypolyskel.polygonize attend et ce que le C++ lit verbatim (contour ++ trous ++ sv).
#
# GARANTIES :
#   - matching cours<->batiment par CONTENANCE d'emprise (rognage <1,4 m : on prend le
#     batiment dont l'emprise rognee CONTIENT le trou, pas juste le plus proche) ;
#   - un trou hors de l'emprise rognee est ECARTE et journalise (le squelette exige des
#     trous STRICTEMENT interieurs) ;
#   - tout le reste (h, u, tint, roof, et les batiments SANS cour) est preserve OCTET
#     pour OCTET : re-serialisation canonique verifiee identique par round-trip.
#   - sauvegarde toulouse10_bati.avant_cours.json (jamais ecrasee).
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "pylib"))
from shapely.geometry import Polygon, Point  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SourceData")
BATI_PATH = os.path.join(SRC, "toulouse10_bati.json")
COURS_PATH = os.path.join(SRC, "GrandFetch", "cours.json")
BAK_PATH = os.path.join(SRC, "toulouse10_bati.avant_cours.json")

GRID = 60.0  # cellule de l'index spatial des centroides (m)


def area2(ring):
    s = 0.0
    for i in range(len(ring)):
        x1, y1 = ring[i]
        x2, y2 = ring[(i + 1) % len(ring)]
        s += x1 * y2 - x2 * y1
    return s


def centroid(pts):
    return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))


def poly(pts):
    p = Polygon([(float(x), float(y)) for x, y in pts])
    if not p.is_valid:
        p = p.buffer(0)
    return p


def dumps_bati(root):
    """Serialisation CANONIQUE (identique au round-trip verifie octet-a-octet)."""
    return ('{"source":%s,"origin":%s,"sizeM":%s,"buildings":[%s]}') % (
        json.dumps(root["source"], ensure_ascii=False),
        json.dumps(root.get("origin"), separators=(",", ":")),
        json.dumps(root.get("sizeM"), separators=(",", ":")),
        ",".join(json.dumps(b, separators=(",", ":"), ensure_ascii=False)
                 for b in root["buildings"]))


def main():
    with open(BATI_PATH, encoding="utf-8") as f:
        root = json.load(f)
    buildings = root["buildings"]
    with open(COURS_PATH, encoding="utf-8") as f:
        cours = json.load(f)["buildings"]
    print("batiments : %d, batiments a cour (fetch) : %d" % (len(buildings), len(cours)))

    # Sauvegarde AVANT cours (une seule fois).
    if not os.path.exists(BAK_PATH):
        with open(BAK_PATH, "w", encoding="utf-8") as f:
            f.write(dumps_bati(root))
        print("sauvegarde -> %s" % BAK_PATH)
    else:
        print("sauvegarde deja presente (conservee) : %s" % BAK_PATH)

    # Index spatial des centroides des batiments cibles.
    cent = [centroid(b["pts"]) for b in buildings]
    grid = {}
    for i, (cx, cy) in enumerate(cent):
        grid.setdefault((int(cx // GRID), int(cy // GRID)), []).append(i)

    def candidates(cx, cy):
        gx, gy = int(cx // GRID), int(cy // GRID)
        out = []
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                out += grid.get((gx + dx, gy + dy), [])
        out.sort(key=lambda i: (cent[i][0] - cx) ** 2 + (cent[i][1] - cy) ** 2)
        return out

    n_matched = n_holes_added = n_holes_out = n_unmatched = 0
    poly_cache = {}

    def bpoly(i):
        if i not in poly_cache:
            poly_cache[i] = poly(buildings[i]["pts"])
        return poly_cache[i]

    for cb in cours:
        cx, cy = centroid(cb["pts"])
        holes = cb["holes"]
        # Le bon batiment = celui dont l'emprise rognee CONTIENT le plus de trous.
        best = None  # (i, kept, dropped)
        for i in candidates(cx, cy)[:12]:
            d2 = (cent[i][0] - cx) ** 2 + (cent[i][1] - cy) ** 2
            if d2 > 20.0 ** 2:  # au-dela de 20 m ce n'est pas le meme batiment
                break
            bp = bpoly(i)
            kept, dropped = [], 0
            for h in holes:
                hp = poly(h)
                if bp.contains(hp):
                    kept.append(h)
                else:
                    dropped += 1
            if kept and (best is None or len(kept) > len(best[1])):
                best = (i, kept, dropped)
        if best is None:
            n_unmatched += 1
            print("  cours NON rattachees : centroide (%.1f, %.1f), %d trou(s) (aucun "
                  "batiment rognee ne les contient)" % (cx, cy, len(holes)))
            continue
        i, kept, dropped = best
        # Normalisation CW + arrondi 2 dec (le contour "pts" est CCW).
        norm = []
        for h in kept:
            r = [[round(float(x), 2), round(float(y), 2)] for x, y in h]
            if area2(r) > 0:            # trou EXIGE CW (aire signee < 0 en local)
                r = r[::-1]
            norm.append(r)
        b = buildings[i]
        if "holes" in b:               # deux enregistrements cours -> meme batiment
            b["holes"].extend(norm)
        else:
            b["holes"] = norm
        n_matched += 1
        n_holes_added += len(norm)
        n_holes_out += dropped
        if dropped:
            print("  batiment #%d : %d cour(s) attachee(s), %d ecartee(s) (hors emprise "
                  "rognee)" % (i, len(norm), dropped))

    with open(BATI_PATH, "w", encoding="utf-8") as f:
        f.write(dumps_bati(root))

    print("FIN : %d batiments ont recu des holes, %d cours attachees, %d ecartees "
          "(hors emprise), %d batiments a cour non rattaches"
          % (n_matched, n_holes_added, n_holes_out, n_unmatched))


if __name__ == "__main__":
    main()
