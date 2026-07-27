# J3b - preparation des batiments AVEC toits : lit GrandFetch/bati_enrichi.json,
# calcule le squelette droit de chaque emprise via bpypolyskel (lib GPL-3 vendorisee
# dans Tools/pylib, OUTIL de preparation uniquement - rien n'en est distribue) et
# ecrit SourceData/toulouse10_bati.json, LA source batiments de la generation :
#
#   {"origin":...,"sizeM":...,"buildings":[
#      {"pts":[[x,y],...],       # anneau NETTOYE CCW (metres, 2 dec) - les faces s'y referent
#       "h":9.5,"u":"res",       # schema historique -> compat totale (collision, proxys, plat)
#       "roof":{                 # OPTIONNEL - absent => toit plat historique
#          "eave":5.5,           # hauteur egout - sol (m, alt IGN min_toit - min_sol)
#          "delta":1.6,          # faitage - egout (m), borne [0.3 ; 30]
#          "mat":"tuile",        # tuile|ardoise|zinc|beton|autre (nomenclature dmatto)
#          "sv":[[x,y,d],...],   # noeuds du squelette SEULS (d = retrait en m > 0)
#          "f":[[i,...],...]}    # versants ; i < n(pts) => anneau, sinon sv[i-n]
#      },...]}
#
# Tout le risque geometrique (squelette droit) reste ici, en Python rejouable ;
# le builder C++ ne recoit que des faces precalculees.
#
# Usage :
#   python j3b_prep_toits.py --selftest          # verrou geometrique (rect, L, triangle)
#   python j3b_prep_toits.py --limit 2000        # echantillon chronometre
#   python j3b_prep_toits.py                     # passe complete
#   python j3b_prep_toits.py --exclude 123,456   # rejouer en excluant des index
import json, math, os, sys, time, argparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "pylib"))
from bpypolyskel import bpypolyskel
from mathutils import Vector

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SourceData")
IN_PATH = os.path.join(SRC, "GrandFetch", "bati_enrichi.json")
OUT_PATH = os.path.join(SRC, "toulouse10_bati.json")
LOG_PATH = os.path.join(SRC, "toulouse10_bati.progress.log")

# Bornes de validite (verrou 1 : outlier delta 243 m constate dans la donnee).
DELTA_MIN, DELTA_MAX = 0.3, 30.0
EAVE_MIN = 2.0


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    with open(LOG_PATH, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def clean_ring(pts):
    """Dedup consecutifs (<1 cm), fermeture retiree, orientation CCW (shoelace > 0)."""
    ring = []
    for p in pts:
        if ring and abs(p[0] - ring[-1][0]) + abs(p[1] - ring[-1][1]) < 0.01:
            continue
        ring.append((float(p[0]), float(p[1])))
    if len(ring) > 1 and abs(ring[0][0] - ring[-1][0]) + abs(ring[0][1] - ring[-1][1]) < 0.01:
        ring.pop()
    if len(ring) < 3:
        return None
    area2 = 0.0
    for i in range(len(ring)):
        x1, y1 = ring[i]
        x2, y2 = ring[(i + 1) % len(ring)]
        area2 += x1 * y2 - x2 * y1
    if area2 < 0:
        ring.reverse()
    return ring


def skeleton_faces(ring, holes=None):
    """Squelette droit d'un polygone avec trous OPTIONNELS.

    Retourne (out_v [[x,y,d]], faces [[idx]], htotal).

    CONTRAT DE SORTIE (le C++ le respecte a l'identique — cf. BuildPolygonBuildingDesktop) :
    l'espace d'indices des faces est, DANS CET ORDRE,
        [0 .. n)               : sommets du CONTOUR (d = 0),
        [n .. n+htotal)         : sommets des TROUS a plat (d = 0), concatenes dans
                                  l'ordre de la liste `holes`, chaque trou tel quel,
        [n+htotal .. )          : noeuds du SQUELETTE (d = retrait > 0).
    Le bloc roof ne stocke que `sv` = les noeuds de squelette (v[n+htotal:]) : le C++
    reconstruit l'espace complet comme  contour ++ trous ++ sv  (le contour vient de
    "pts", les trous de "holes", tous deux du niveau batiment).

    Orientation EXIGEE en entree : contour CCW (aire signee > 0), trous CW (< 0)
    — c'est ce que bpypolyskel.polygonize attend (dremonstrateur test_toit_trous.py).
    Exception si degenere.
    """
    holes = holes or []
    verts = [Vector((x, y, 0.0)) for x, y in ring]
    n = len(ring)
    holes_info = None
    if holes:
        holes_info = []
        idx = n
        for h in holes:
            if len(h) < 3:
                raise ValueError("trou < 3 sommets")
            holes_info.append((idx, len(h)))
            verts += [Vector((x, y, 0.0)) for x, y in h]
            idx += len(h)
    htotal = sum(len(h) for h in holes)
    nb = n + htotal  # sommets de BORD (contour + trous), tous a d = 0
    faces = bpypolyskel.polygonize(verts, 0, n, holes_info, 0.0, 1.0, None, None)
    if not faces:
        raise ValueError("aucune face")
    out_v = []
    for v in verts:
        d = v[2] if len(v) > 2 else 0.0
        if not (math.isfinite(v[0]) and math.isfinite(v[1]) and math.isfinite(d)) or d < -0.01:
            raise ValueError("sommet invalide")
        out_v.append([round(v[0], 2), round(v[1], 2), round(max(d, 0.0), 3)])
    maxd = max((v[2] for v in out_v[nb:]), default=0.0)  # sur les seuls noeuds de squelette
    if maxd <= 0.005:
        raise ValueError("squelette plat (maxd=0)")
    # Aretes de BORD admissibles comme 1re arete d'un versant : celles du contour ET
    # celles de chaque trou (l'egout exterieur OU l'avant-toit de cour). bpypolyskel
    # garantit ce contrat ; on le VERIFIE ici, il est critique pour le C++.
    bnd = set()
    for (s, L) in [(0, n)] + (holes_info or []):
        for i in range(L):
            bnd.add((s + i, s + (i + 1) % L))
    for f in faces:
        if len(f) < 3 or any(i < 0 or i >= len(out_v) for i in f):
            raise ValueError("face invalide")
        if (f[0], f[1]) not in bnd:
            raise ValueError("1re arete hors bord")
    return out_v, faces, htotal


def usage_of(b):
    u = b.get("usage_1") or ""
    if u.startswith("Résidentiel") or u.startswith("Residentiel"):
        return "res"
    if u.startswith("Commercial"):
        return "com"
    if u.startswith("Industriel"):
        return "ind"
    return "oth"


def height_of(b):
    h = b.get("hauteur")
    if h and h > 0:
        return round(float(h), 1)
    et = b.get("nombre_d_etages")
    if et and et > 0:
        return round(3.0 * et + 1.0, 1)
    return 9.0


def mat_of(b):
    """Classe de materiau depuis le code cadastral dmatto (2 chiffres, principal+secondaire)."""
    code = b.get("materiaux_de_la_toiture") or ""
    table = {"1": "tuile", "2": "ardoise", "3": "zinc", "4": "beton", "9": "autre"}
    for ch in code:
        if ch in table:
            return table[ch]
    return "tuile"  # indetermine/absent -> defaut toulousain (verrou 1 : ~79 % tuile)


def roof_of(b, ring, holes=None):
    """Bloc roof ou (None, raison). Bornes DELTA/EAVE du verrou 1.
    holes OPTIONNEL (cours) : passe tel quel a skeleton_faces (contour CCW, trous CW).
    `sv` = noeuds de squelette SEULS (v[n+htotal:]) — le C++ reconstruit
    contour ++ trous ++ sv (les trous viennent de "holes" au niveau batiment)."""
    amin_t = b.get("altitude_minimale_toit")
    amax_t = b.get("altitude_maximale_toit")
    amin_s = b.get("altitude_minimale_sol")
    if amin_t is None or amax_t is None or amin_s is None:
        return None, "alts absentes"
    delta = amax_t - amin_t
    if delta < DELTA_MIN:
        return None, "quasi plat"
    if delta > DELTA_MAX:
        return None, "delta aberrant"
    eave = amin_t - amin_s
    if eave < EAVE_MIN:
        return None, "egout trop bas"
    v, faces, htotal = skeleton_faces(ring, holes)  # exceptions comptees par l'appelant
    n = len(ring)
    return {
        "eave": round(eave, 1),
        "delta": round(delta, 1),
        "mat": mat_of(b),
        "sv": v[n + htotal:],
        "f": faces,
    }, None


def selftest():
    ok = True

    def check(name, ring, exp_faces, exp_maxd):
        nonlocal ok
        try:
            # Meme chemin que la passe reelle : nettoyage PUIS squelette.
            v, f, _ = skeleton_faces(clean_ring(ring))
            maxd = max(x[2] for x in v)
            good = len(f) == exp_faces and abs(maxd - exp_maxd) < 0.05
            print(f"  {name:24s} : {len(f)} faces (attendu {exp_faces}), "
                  f"maxd {maxd:.2f} (attendu {exp_maxd}) -> {'PASS' if good else 'FAIL'}")
            ok = ok and good
        except Exception as e:
            print(f"  {name:24s} : EXCEPTION {e} -> FAIL")
            ok = False

    # Rectangle 12x10 : 4 versants, faitage a l'inradius 5 (2 noeuds a d=5).
    check("rectangle 12x10", [(0, 0), (12, 0), (12, 10), (0, 10)], 4, 5.0)
    # L 20x20 avec encoche 10x10 : 6 aretes -> 6 versants, inradius 5.
    check("L 20/10", [(0, 0), (20, 0), (20, 10), (10, 10), (10, 20), (0, 20)], 6, 5.0)
    # Triangle 12,10 : 3 versants, sommet a l'incentre r = aire/s.
    a, b, c = 12.0, math.hypot(12, 10), 10.0
    r = (0.5 * 12 * 10) / (0.5 * (a + b + c))
    check("triangle rect 12x10", [(0, 0), (12, 0), (0, 10)], 3, round(r, 2))
    # Anneau sale : point double + fermeture explicite -> nettoye puis 4 versants.
    check("rect sale (dedup)", [(0, 0), (0, 0), (12, 0), (12, 10), (0, 10), (0, 0)], 4, 5.0)
    # Ordre CW en entree -> reoriente CCW par clean_ring.
    check("rect CW reoriente", [(0, 10), (12, 10), (12, 0), (0, 0)], 4, 5.0)

    # Le bloc roof complet sur un batiment synthetique type verrou 1.
    b = {"usage_1": "Résidentiel", "hauteur": 7.0, "materiaux_de_la_toiture": "10",
         "altitude_minimale_toit": 141.4, "altitude_maximale_toit": 143.0,
         "altitude_minimale_sol": 135.9}
    ring = clean_ring([(0, 0), (12, 0), (12, 10), (0, 10)])
    roof, why = roof_of(b, ring)
    good = (roof is not None and roof["eave"] == 5.5 and roof["delta"] == 1.6
            and roof["mat"] == "tuile" and len(roof["f"]) == 4 and len(roof["sv"]) == 2)
    print(f"  bloc roof synthetique    : eave={roof and roof['eave']} delta={roof and roof['delta']} "
          f"mat={roof and roof['mat']} -> {'PASS' if good else 'FAIL'}")
    ok = ok and good
    # Garde-fous : delta aberrant et alts absentes -> plat.
    b2 = dict(b, altitude_maximale_toit=400.0)
    r2, why2 = roof_of(b2, ring)
    b3 = {k: v for k, v in b.items() if k != "altitude_minimale_sol"}
    r3, why3 = roof_of(b3, ring)
    good = r2 is None and why2 == "delta aberrant" and r3 is None and why3 == "alts absentes"
    print(f"  garde-fous plat          : {why2} / {why3} -> {'PASS' if good else 'FAIL'}")
    ok = ok and good

    # --- Toit A COUR : carre 40x40 avec cour carree 10x10 (contour CCW, trou CW). ---
    def area2(r):
        s = 0.0
        for i in range(len(r)):
            x1, y1 = r[i]
            x2, y2 = r[(i + 1) % len(r)]
            s += x1 * y2 - x2 * y1
        return s

    ext = clean_ring([(0, 0), (40, 0), (40, 40), (0, 40)])   # CCW
    hole = [(15, 15), (25, 15), (25, 25), (15, 25)]
    if area2(hole) > 0:                                       # bpypolyskel EXIGE le trou CW
        hole = hole[::-1]
    try:
        v0, f0, h0 = skeleton_faces(ext)                     # sans trou
        v1, f1, h1 = skeleton_faces(ext, [hole])             # avec trou
        sv = v1[8:]                                          # noeuds de squelette (n=4, htotal=4)
        heights_ok = all(math.isfinite(x[2]) and x[2] >= 0.0 for x in v1)
        # Plus de faces qu'a plein, htotal=4, bords a d=0, squelette souleve (d>0),
        # et 1res aretes toutes sur un bord (skeleton_faces leve sinon).
        good = (len(f1) > len(f0) and h1 == 4 and h0 == 0 and len(sv) >= 1
                and heights_ok
                and all(abs(x[2]) < 1e-6 for x in v1[:8])
                and all(x[2] > 0.0 for x in sv))
        print(f"  carre 40 a cour 10x10    : {len(f0)} faces plein -> {len(f1)} avec cour, "
              f"htotal={h1}, {len(sv)} noeuds squelette -> {'PASS' if good else 'FAIL'}")
        ok = ok and good
    except Exception as e:  # noqa: BLE001
        print(f"  carre 40 a cour 10x10    : EXCEPTION {e} -> FAIL")
        ok = False

    print("SELFTEST " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def _ring_area2(ring):
    s = 0.0
    for i in range(len(ring)):
        x1, y1 = ring[i]
        x2, y2 = ring[(i + 1) % len(ring)]
        s += x1 * y2 - x2 * y1
    return s


def _dumps_bati(root):
    """Serialisation CANONIQUE de toulouse10_bati.json (round-trip octet identique)."""
    return ('{"source":%s,"origin":%s,"sizeM":%s,"buildings":[%s]}') % (
        json.dumps(root["source"], ensure_ascii=False),
        json.dumps(root.get("origin"), separators=(",", ":")),
        json.dumps(root.get("sizeM"), separators=(",", ":")),
        ",".join(json.dumps(b, separators=(",", ":"), ensure_ascii=False)
                 for b in root["buildings"]))


def cours_pass():
    """Recalcule le bloc roof des SEULS batiments a cour (cle "holes"), EN PLACE dans
    toulouse10_bati.json. Reutilise eave/delta/mat/tint (issus des alts IGN, INDEPENDANTS
    des trous) : seuls sv et f changent. Contour et trous pris VERBATIM (memes sommets,
    meme ordre que le C++ : contour "pts" CCW ++ "holes" CW ++ sv). Retombe en toit PLAT
    (bloc roof retire) si le squelette a trous degenere. Ne touche a AUCUN autre batiment."""
    log(f"--cours : chargement {OUT_PATH}")
    with open(OUT_PATH, encoding="utf-8") as f:
        root = json.load(f)
    buildings = root["buildings"]
    n_cour = n_recomp = n_flat = n_sans_roof = n_bad_orient = n_flat_cut = 0
    for b in buildings:
        holes = b.get("holes")
        if not holes:
            continue
        n_cour += 1
        roof = b.get("roof")
        if roof is None:
            # J3c cours : un batiment a cours SANS toit en pente (alts IGN absentes ou
            # toit trop plat) restait un PRISME PLEIN — le C++ conditionne la decoupe
            # des cours a l'existence d'un bloc roof (bUseHoles = Roof != nullptr). C'est
            # LE bug du "bloc blanc qui recouvre le passage". On lui fabrique donc un toit
            # QUASI PLAT (delta 0,5 m, invisible) portant les faces du squelette a trous :
            # le C++ monte alors les murs de cour et ajoure le toit, exactement comme pour
            # un toit en pente. mat/tint restent au defaut (tuile, 0,95 blanc) -> l'aspect
            # plat blanc est INCHANGE, seule la cour s'ouvre.
            h = float(b.get("h") or height_of(b))
            delta = 0.5
            eave = round(h - delta, 1)
            if eave < 2.0:                 # trop bas pour un egout valide (ParseRoof) -> plein
                n_sans_roof += 1
                continue
            ring = [(float(x), float(y)) for x, y in b["pts"]]
            hs = [[(float(x), float(y)) for x, y in hh] for hh in holes]
            if _ring_area2(ring) <= 0 or any(_ring_area2(hh) >= 0 for hh in hs):
                n_bad_orient += 1
                continue
            try:
                v, faces, htotal = skeleton_faces(ring, hs)
                n = len(ring)
                b["roof"] = {"eave": eave, "delta": delta, "mat": mat_of(b),
                             "sv": v[n + htotal:], "f": faces}
                n_flat_cut += 1
            except Exception as e:  # noqa: BLE001
                n_sans_roof += 1   # squelette a trous degenere -> reste plein (rare)
                log(f"  cour (toit plat) degeneree -> reste pleine : {str(e)[:50]}")
            continue
        # VERBATIM : aucun dedup/reorient (le C++ lit ces memes sommets sans les toucher).
        ring = [(float(x), float(y)) for x, y in b["pts"]]
        hs = [[(float(x), float(y)) for x, y in h] for h in holes]
        if _ring_area2(ring) <= 0 or any(_ring_area2(h) >= 0 for h in hs):
            # contour non CCW ou trou non CW : donnee incoherente -> plat de secours.
            n_bad_orient += 1
            b.pop("roof", None)
            n_flat += 1
            continue
        try:
            v, faces, htotal = skeleton_faces(ring, hs)
            n = len(ring)
            roof["sv"] = v[n + htotal:]
            roof["f"] = faces
            n_recomp += 1
        except Exception as e:  # noqa: BLE001
            b.pop("roof", None)
            n_flat += 1
            log(f"  cour degeneree -> toit plat : {str(e)[:60]}")
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write(_dumps_bati(root))
    log(f"FIN --cours : {n_cour} batiments a cour ; {n_recomp} toits en pente recalcules "
        f"avec trous, {n_flat_cut} toits PLATS ajoures (quasi-plat + cours decoupees), "
        f"{n_flat} plats de secours (dont {n_bad_orient} orientation incoherente), "
        f"{n_sans_roof} restent pleins (egout trop bas ou squelette degenere)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--cours", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--exclude", type=str, default="")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(selftest())
    if args.cours:
        sys.exit(cours_pass())

    excl = set(int(x) for x in args.exclude.split(",") if x.strip())
    log(f"chargement {IN_PATH}")
    with open(IN_PATH, encoding="utf-8") as f:
        root = json.load(f)
    buildings = root["buildings"]
    total = len(buildings) if args.limit <= 0 else min(args.limit, len(buildings))
    log(f"batiments : {total} (exclusions : {sorted(excl) if excl else 'aucune'})")

    t0 = time.time()
    n_pitched = n_flat = 0
    reasons = {}
    parts = []

    def flat(reason):
        nonlocal n_flat
        n_flat += 1
        reasons[reason] = reasons.get(reason, 0) + 1

    for i in range(total):
        b = buildings[i]
        if i % 5000 == 0 and i > 0:
            log(f"  {i}/{total} (pentes {n_pitched}, plats {n_flat})")
        ring = clean_ring(b["pts"])
        if ring is None:
            continue  # pas de batiment du tout (comme un anneau invalide du fetch)
        rec = '{"pts":%s,"h":%s,"u":"%s"' % (
            json.dumps([[p[0], p[1]] for p in ring], separators=(",", ":")),
            json.dumps(height_of(b)), usage_of(b))
        roof = None
        if i in excl:
            flat("exclu")
        else:
            try:
                roof, why = roof_of(b, ring)
                if roof is None:
                    flat(why)
            except Exception as e:
                flat("squelette : " + str(e)[:40])
        if roof is not None:
            rec += ',"roof":' + json.dumps(roof, separators=(",", ":"))
            n_pitched += 1
        parts.append(rec + "}")

    out = ('{"source":"j3b_prep_toits.py (bati_enrichi + squelette droit bpypolyskel)",'
           '"origin":%s,"sizeM":%s,"buildings":[%s]}') % (
        json.dumps(root.get("origin"), separators=(",", ":")),
        json.dumps(root.get("sizeM"), separators=(",", ":")),
        ",".join(parts))
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write(out)
    dt = time.time() - t0
    size_mb = os.path.getsize(OUT_PATH) / 1e6
    n = n_pitched + n_flat
    log(f"FIN : {len(parts)} batiments ecrits, {n_pitched} toits en pente "
        f"({100.0 * n_pitched / max(n, 1):.1f} %), {n_flat} plats, "
        f"{dt:.0f} s ({1000.0 * dt / max(total, 1):.1f} ms/bat), {size_mb:.1f} Mo")
    for k, v in sorted(reasons.items(), key=lambda x: -x[1])[:10]:
        log(f"  plat : {k} x{v}")


if __name__ == "__main__":
    main()
