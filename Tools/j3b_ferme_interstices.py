# J3b-bis : ferme les INTERSTICES FINS (< SEUIL) entre batiments adjacents.
#
# Les emprises BD TOPO laissent parfois un cheveu (quelques cm a ~0,6 m) entre deux
# batiments qui, dans la vraie ville, se touchent (mur mitoyen digitalise avec un
# ecart). Une fois le detail rendu, ca lit comme une fente vide "glitchee".
#
# Principe (FIABLE, ordre-independant, sans devinette) : chaque batiment s'etend de
# SEUIL/2 vers ses voisins UNIQUEMENT la ou deux emprises sont a moins de SEUIL l'une
# de l'autre (intersection de leurs tampons). Nulle part ailleurs -> les RUES (aucun
# voisin proche) et les VRAIES VENELLES (> SEUIL) ne bougent pas. Les COURS (holes)
# sont preservees (on n'etend que le contour exterieur, puis on re-attache les trous).
# Le toit des batiments modifies est RECALCULE (squelette droit sur le nouveau contour).
#
#   python j3b_ferme_interstices.py --selftest
#   python j3b_ferme_interstices.py            # applique EN PLACE a toulouse10_bati.json
#   python j3b_ferme_interstices.py --seuil 0.6
import argparse, json, math, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "pylib"))
from shapely.geometry import Polygon
from shapely.ops import unary_union
from shapely.strtree import STRtree

import j3b_prep_toits as P   # skeleton_faces, mat_of, clean_ring, _ring_area2 (helpers)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATI = os.path.join(ROOT, "SourceData", "toulouse10_bati.json")
SEUIL = 0.6          # gaps < SEUIL fermes ; >= SEUIL (venelles >=0.9 m) gardes
MIN_GAIN = 0.05      # m2 : en-deca on considere le contour inchange (bruit)

def log(m): print(time.strftime("%H:%M:%S ") + m, flush=True)

def ring_area2(r):
    s = 0.0
    for i in range(len(r)):
        x1, y1 = r[i]; x2, y2 = r[(i + 1) % len(r)]
        s += x1 * y2 - x2 * y1
    return s

def valide(g):
    if g.is_valid: return g
    g2 = g.buffer(0)
    return g2 if g2.is_valid else g

def ext_poly(b):
    pts = b.get("pts") or []
    if len(pts) < 4: return None
    try: p = Polygon([(float(x), float(y)) for x, y in pts])
    except Exception: return None
    p = valide(p)
    return p if (p.is_valid and p.geom_type == "Polygon" and p.area > 1) else None

def biggest(g):
    if g.is_empty: return None
    if g.geom_type == "Polygon": return g
    if g.geom_type in ("MultiPolygon", "GeometryCollection"):
        best = None
        for x in g.geoms:
            if x.geom_type == "Polygon" and (best is None or x.area > best.area): best = x
        return best
    return None

def recompute_roof(b, new_ring):
    """Recalcule sv/f du bloc roof sur le NOUVEAU contour (trous inchanges). Retombe
    plat (roof retire) si le squelette degenere. Batiment sans roof : rien a faire
    (le C++ triangule pts directement)."""
    roof = b.get("roof")
    if roof is None:
        return "flat"   # pas de recompute, le toit plat suit pts
    holes = b.get("holes") or []
    ring = [(float(x), float(y)) for x, y in new_ring]
    hs = [[(float(x), float(y)) for x, y in h] for h in holes]
    if ring_area2(ring) <= 0 or any(ring_area2(h) >= 0 for h in hs):
        b.pop("roof", None); return "flat_secours"
    try:
        v, faces, htotal = P.skeleton_faces(ring, hs if hs else None)
        n = len(ring)
        roof["sv"] = v[n + htotal:]
        roof["f"] = faces
        return "recomp"
    except Exception:
        b.pop("roof", None); return "flat_secours"

def fermer(buildings, seuil):
    """Ferme les gaps < seuil. Pour chaque batiment : fermeture morphologique LOCALE
    (batiment + voisins immediats) qui comble les fentes < seuil, puis on rend au
    batiment la part comblee de SON cote (intersection avec son propre tampon seuil/2).
    Connexe et ordre-independant ; rues et venelles > seuil intactes ; cours preservees."""
    half = seuil / 2.0
    idxs, exts = [], []
    for i, b in enumerate(buildings):
        p = ext_poly(b)
        if p is not None:
            idxs.append(i); exts.append(p)
    tree = STRtree(exts)
    n_chg = n_recomp = n_flat = n_flatsec = 0
    for k, (i, p) in enumerate(zip(idxs, exts)):
        try:
            cand = tree.query(p, predicate="dwithin", distance=seuil + 0.1)
        except Exception:
            cand = tree.query(p.buffer(seuil + 0.1))
        near = [exts[int(j)] for j in cand if int(j) != k]
        if not near:
            continue
        # OPTIM : ne traite que les batiments ayant un VRAI gap fin (0,02..seuil) avec
        # au moins un voisin. Ceux qui touchent deja (dist ~0) ou sont loin sont skippes
        # AVANT la fermeture morpho couteuse -> ~131k checks legers, close sur qq milliers.
        if not any(0.02 < p.distance(q) < seuil for q in near):
            continue
        near = [q for q in near if p.distance(q) < seuil + 0.05]
        local = valide(unary_union([p] + near))
        closed = valide(local.buffer(half, join_style=1).buffer(-half, join_style=1))
        ext_zone = valide(closed.intersection(p.buffer(half, join_style=1)))
        new_ext = biggest(valide(p.union(ext_zone)))
        if new_ext is None or new_ext.area - p.area < MIN_GAIN:
            continue
        # simplify 5 cm : les arcs du tampon rond + l'arrondi cm cassent le squelette
        # droit ; a 5 cm on garde la forme et le squelette passe a 100 % (teste).
        new_ext = biggest(valide(new_ext.simplify(0.05, preserve_topology=True)))
        if new_ext is None or new_ext.geom_type != "Polygon":
            continue
        ring = [[round(x, 2), round(y, 2)] for x, y in list(new_ext.exterior.coords)[:-1]]
        if ring_area2(ring) < 0:
            ring = ring[::-1]
        b = buildings[i]
        b["pts"] = ring
        r = recompute_roof(b, ring)
        n_chg += 1
        if r == "recomp": n_recomp += 1
        elif r == "flat_secours": n_flatsec += 1
        else: n_flat += 1
    return n_chg, n_recomp, n_flat, n_flatsec


def selftest():
    # deux carres 10x10 separes de 0,4 m -> doivent se rejoindre (gain > 0)
    A = {"pts": [[0,0],[10,0],[10,10],[0,10]], "h": 9}
    B = {"pts": [[10.4,0],[20.4,0],[20.4,10],[10.4,10]], "h": 9}
    # un troisieme a 2 m (venelle) -> NE doit PAS bouger
    Cc = {"pts": [[22.4,0],[32.4,0],[32.4,10],[22.4,10]], "h": 9}
    bs = [A, B, Cc]
    a0 = ring_area2(A["pts"]); c0 = ring_area2(Cc["pts"])
    chg, *_ = fermer(bs, 0.6)
    aA = abs(ring_area2(A["pts"])) / 2; aC = abs(ring_area2(Cc["pts"])) / 2
    gapAB = Polygon([(float(x),float(y)) for x,y in A["pts"]]).distance(
            Polygon([(float(x),float(y)) for x,y in B["pts"]]))
    ok = chg == 2 and gapAB < 0.05 and abs(aC - 100) < 0.01
    print("  A+B (gap 0,4) fermes : %d modifies, gap final %.3f m" % (chg, gapAB))
    print("  C (venelle 2 m) intact : aire %.1f (attendu 100)" % aC)
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--seuil", type=float, default=SEUIL)
    args = ap.parse_args()
    if args.selftest:
        sys.exit(selftest())
    log("chargement %s" % BATI)
    with open(BATI, encoding="utf-8-sig") as f:
        root = json.load(f)
    bs = root["buildings"]
    log("batiments : %d ; fermeture des interstices < %.2f m" % (len(bs), args.seuil))
    t0 = time.time()
    chg, recomp, flat, flatsec = fermer(bs, args.seuil)
    with open(BATI, "w", encoding="utf-8") as f:
        json.dump(root, f, separators=(",", ":"))
    log("FIN (%.0f s) : %d batiments etendus (%d toits recalcules, %d plats suivent pts, "
        "%d plats de secours)" % (time.time() - t0, chg, recomp, flat, flatsec))
    return 0

if __name__ == "__main__":
    sys.exit(main())
