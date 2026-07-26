# J3c - "l'ortho comme pigment" : la BD ORTHO n'est JAMAIS affichee (moche de pres),
# elle sert de DONNEE de couleur. Ce script echantillonne la couleur REELLE de chaque
# toit dans les tuiles SourceData/Ortho/ortho_20cm_<cx>_<cy>.jpg et ecrit dans
# SourceData/toulouse10_bati.json un champ par batiment :
#
#   {"pts":[...],"h":9.5,"u":"res","roof":{...},"tint":[1.08,0.94,0.86]}
#
# tint = mediane_par_canal(pixels du toit) / gris_de_reference, ou le gris de
# reference est la mediane GLOBALE des toits en PENTE : le toit "moyen" garde
# tint = 1 et la texture terracotta de l'atlas reste calibree telle quelle.
# Chaque canal est borne a [0,4 ; 1,6]. Statistique = MEDIANE (pas moyenne) :
# voitures, ombres, cheminees et velux polluent trop la moyenne.
#
# Georef (identique a Fetch-Toulouse10-Ortho.ps1) : la tuile (cx,cy) couvre
# x [cx*500,(cx+1)*500] m et y [cy*500,(cy+1)*500] m locaux, pixel (0,0) = coin NW,
# 1 px = 20 cm (le pas reel est relu dans l'image, pas suppose).
#
# Usage :
#   python j3c_tint_toits.py --selftest      # verrou couleur (tuiles synthetiques)
#   python j3c_tint_toits.py --limit 5000    # echantillon chronometre (sans ecriture)
#   python j3c_tint_toits.py                 # passe complete + reecriture du JSON
#   python j3c_tint_toits.py --dry-run       # passe complete SANS ecrire
import argparse
import json
import math
import os
import shutil
import sys
import time
from array import array
from collections import defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "pylib"))
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SourceData")
BATI_PATH = os.path.join(SRC, "toulouse10_bati.json")
BACKUP_PATH = os.path.join(SRC, "toulouse10_bati.avant_tint.json")
ORTHO_DIR = os.path.join(SRC, "Ortho")
LOG_PATH = os.path.join(SRC, "toulouse10_tint.progress.log")

CELL_M = 500.0            # cote d'une cellule/tuile en metres
CELL_MIN, CELL_MAX = -10, 9  # bornes de la dalle 10x10 km (cx et cy)
TARGET_PTS = 64           # points d'echantillonnage vises par batiment
SIDE_MAX = 12             # cote maximal de la grille au 1er essai (12x12 = 144)
SIDE_CAP = 48             # cote maximal apres raffinements (slivers)
MIN_SAMPLES = 3           # sous ce nombre de pixels, pas de teinte
TINT_MIN, TINT_MAX = 0.4, 1.6


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    with open(LOG_PATH, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def median(vals):
    """Mediane d'une sequence non vide (copie triee ; O(n log n) suffit ici)."""
    s = sorted(vals)
    n = len(s)
    if n % 2:
        return float(s[n // 2])
    return 0.5 * (s[n // 2 - 1] + s[n // 2])


def cell_of(v):
    """Index de cellule contenant la coordonnee v (metres locaux)."""
    return int(math.floor(v / CELL_M))


def point_in_ring(x, y, ring):
    """Ray casting horizontal. ring = [(x,y),...] non ferme, sens indifferent."""
    inside = False
    n = len(ring)
    j = n - 1
    for i in range(n):
        xi, yi = ring[i]
        xj, yj = ring[j]
        if (yi > y) != (yj > y):
            xint = xi + (y - yi) * (xj - xi) / (yj - yi)
            if x < xint:
                inside = not inside
        j = i
    return inside


def interior_points(ring):
    """Grille de points INTERIEURS a l'emprise (~30-100 points). Raffine si la
    grille initiale tombe a cote (batiments en lame de couteau)."""
    xs = [p[0] for p in ring]
    ys = [p[1] for p in ring]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    w, h = x1 - x0, y1 - y0
    if w <= 0.0 or h <= 0.0:
        return []
    step = math.sqrt(w * h / float(TARGET_PTS))
    nx = min(max(int(round(w / step)), 1), SIDE_MAX)
    ny = min(max(int(round(h / step)), 1), SIDE_MAX)
    while True:
        out = []
        for iy in range(ny):
            py = y0 + (iy + 0.5) * h / ny
            for ix in range(nx):
                px = x0 + (ix + 0.5) * w / nx
                if point_in_ring(px, py, ring):
                    out.append((px, py))
        if len(out) >= MIN_SAMPLES or nx >= SIDE_CAP or ny >= SIDE_CAP:
            return out
        nx = min(nx * 2, SIDE_CAP)
        ny = min(ny * 2, SIDE_CAP)


class DiskTiles:
    """Fournisseur de tuiles ortho depuis le disque, UNE seule en memoire.
    get(cx,cy) -> (octets RGB, largeur, hauteur) ou None si la tuile manque."""

    def __init__(self, folder):
        self.folder = folder
        self.key = None
        self.data = None

    def path(self, cx, cy):
        return os.path.join(self.folder, "ortho_20cm_%d_%d.jpg" % (cx, cy))

    def has(self, cx, cy):
        return os.path.isfile(self.path(cx, cy))

    def get(self, cx, cy):
        if self.key == (cx, cy):
            return self.data
        self.key = (cx, cy)
        self.data = None
        p = self.path(cx, cy)
        if os.path.isfile(p):
            with Image.open(p) as im:
                im = im.convert("RGB")
                self.data = (im.tobytes(), im.width, im.height)
        return self.data


class MemTiles:
    """Fournisseur de tuiles en memoire pour le self-test."""

    def __init__(self, tiles):
        self.tiles = tiles  # {(cx,cy): (bytes, w, h)}

    def has(self, cx, cy):
        return (cx, cy) in self.tiles

    def get(self, cx, cy):
        return self.tiles.get((cx, cy))


def sample_points(tiles, pts, acc_r, acc_g, acc_b):
    """Echantillonne les pixels ortho sous les points pts (metres locaux) et les
    empile dans acc_*. Retourne (n_pixels, n_hors_dalle, set des tuiles manquantes)."""
    n = 0
    n_out = 0
    missing = set()
    for (x, y) in pts:
        cx, cy = cell_of(x), cell_of(y)
        if cx < CELL_MIN or cx > CELL_MAX or cy < CELL_MIN or cy > CELL_MAX:
            n_out += 1
            continue
        t = tiles.get(cx, cy)
        if t is None:
            missing.add((cx, cy))
            continue
        buf, w, h = t
        # pixel (0,0) = coin NW ; x croit vers l'est (colonnes), y vers le sud (lignes).
        col = int((x - cx * CELL_M) * w / CELL_M)
        row = int((y - cy * CELL_M) * h / CELL_M)
        if col < 0 or col >= w or row < 0 or row >= h:
            n_out += 1
            continue
        o = (row * w + col) * 3
        acc_r.append(buf[o])
        acc_g.append(buf[o + 1])
        acc_b.append(buf[o + 2])
        n += 1
    return n, n_out, missing


def to_tint(med, ref):
    """Teinte multiplicative bornee : mediane du toit / gris de reference."""
    out = []
    for i in range(3):
        v = med[i] / ref[i] if ref[i] > 0.0 else 1.0
        out.append(round(min(max(v, TINT_MIN), TINT_MAX), 3))
    return out


def solid_tile(w, h, rgb):
    return (bytes(rgb) * (w * h), w, h)


def painted_tile(w, h, bg, fg, box):
    """Tuile bg avec un rectangle fg sur box = (col0,row0,col1,row1) exclusif."""
    buf = bytearray(bytes(bg) * (w * h))
    c0, r0, c1, r1 = box
    for r in range(max(r0, 0), min(r1, h)):
        base = r * w
        for c in range(max(c0, 0), min(c1, w)):
            o = (base + c) * 3
            buf[o] = fg[0]
            buf[o + 1] = fg[1]
            buf[o + 2] = fg[2]
    return (bytes(buf), w, h)


def median_of_building(tiles, ring):
    """(mediane RGB ou None, n_pixels, n_hors_dalle, tuiles manquantes)."""
    pts = interior_points(ring)
    r, g, b = array("B"), array("B"), array("B")
    n, n_out, missing = sample_points(tiles, pts, r, g, b)
    if n < MIN_SAMPLES:
        return None, n, n_out, missing
    return (median(r), median(g), median(b)), n, n_out, missing


def selftest():
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print("  %-34s : %s%s" % (name, "PASS" if cond else "FAIL",
                                  ("  " + detail) if detail else ""))
        ok = ok and cond

    # Tuile de test 500 px (2 m/px) : la georef est relue dans l'image, donc une
    # tuile plus petite qu'en production doit marcher a l'identique.
    W = H = 500
    GRAY = (128, 128, 128)
    RED = (200, 60, 60)
    # Rectangle rouge sur toute la moitie nord-ouest de la tuile (0,0), soit
    # x [0,250] m et y [0,250] m locaux.
    tile00 = painted_tile(W, H, GRAY, RED, (0, 0, 250, 250))
    tiles = MemTiles({(0, 0): tile00,
                      (1, 0): solid_tile(W, H, GRAY),
                      (0, -1): solid_tile(W, H, (60, 60, 200))})

    # 1. Batiment ENTIEREMENT dans le rectangle rouge -> mediane = rouge exact.
    ring = [(100.0, 100.0), (140.0, 100.0), (140.0, 130.0), (100.0, 130.0)]
    med, n, n_out, miss = median_of_building(tiles, ring)
    check("toit rouge : mediane", med == (200.0, 60.0, 60.0),
          "med=%s n=%d" % (med, n))
    check("toit rouge : 30-100 pixels", 30 <= n <= 100, "n=%d" % n)
    # ... et sa teinte face au gris de reference : rougeatre, r > 1 > g == b.
    tint = to_tint(med, (128.0, 128.0, 128.0))
    check("toit rouge : teinte rougeatre",
          tint[0] > 1.05 and tint[1] < 0.95 and abs(tint[1] - tint[2]) < 1e-9,
          "tint=%s" % tint)
    # 2. Batiment sur le gris pur -> teinte neutre exactement 1.
    ring_g = [(300.0, 300.0), (340.0, 300.0), (340.0, 330.0), (300.0, 330.0)]
    med_g, n_g, _, _ = median_of_building(tiles, ring_g)
    check("toit gris : teinte neutre", to_tint(med_g, (128.0, 128.0, 128.0)) == [1.0, 1.0, 1.0],
          "tint=%s" % to_tint(med_g, (128.0, 128.0, 128.0)))
    # 3. Bornes [0,4 ; 1,6] : blanc sur reference sombre, noir sur reference claire.
    check("bornes de teinte",
          to_tint((255.0, 255.0, 255.0), (100.0, 100.0, 100.0)) == [1.6, 1.6, 1.6]
          and to_tint((5.0, 5.0, 5.0), (128.0, 128.0, 128.0)) == [0.4, 0.4, 0.4])
    # 4. Batiment A CHEVAL sur les tuiles (0,0) et (1,0) : tous les points comptes.
    ring_s = [(480.0, 300.0), (520.0, 300.0), (520.0, 330.0), (480.0, 330.0)]
    med_s, n_s, out_s, miss_s = median_of_building(tiles, ring_s)
    check("a cheval 2 tuiles : tout echantillonne",
          n_s == len(interior_points(ring_s)) and n_s >= 30 and not miss_s and out_s == 0,
          "n=%d/%d" % (n_s, len(interior_points(ring_s))))
    # 5. La ligne 0 est bien au NORD : la tuile (0,-1) est bleue, y = -100 doit
    #    tomber dedans (y croit vers le sud) et non dans la tuile (0,0).
    ring_n = [(100.0, -120.0), (140.0, -120.0), (140.0, -90.0), (100.0, -90.0)]
    med_n, _, _, _ = median_of_building(tiles, ring_n)
    check("georef nord = -Y (tuile cy=-1)", med_n == (60.0, 60.0, 200.0), "med=%s" % (med_n,))
    # 6. Garde-fou : emprise hors dalle -> pas de teinte, points comptes hors dalle.
    ring_far = [(9000.0, 9000.0), (9040.0, 9000.0), (9040.0, 9030.0), (9000.0, 9030.0)]
    med_f, n_f, out_f, miss_f = median_of_building(tiles, ring_far)
    check("hors dalle : pas de teinte", med_f is None and n_f == 0 and out_f > 0,
          "n=%d hors=%d" % (n_f, out_f))
    # 7. Garde-fou : tuile absente -> pas de teinte, tuile manquante signalee.
    ring_m = [(1200.0, 100.0), (1240.0, 100.0), (1240.0, 130.0), (1200.0, 130.0)]
    med_m, n_m, out_m, miss_m = median_of_building(tiles, ring_m)
    check("tuile manquante : pas de teinte",
          med_m is None and n_m == 0 and miss_m == {(2, 0)}, "manquantes=%s" % (miss_m,))
    # 8. La mediane resiste aux polluants : 40 % de la tuile en noir (voitures,
    #    ombres) sous un toit clair majoritaire -> mediane = la valeur du toit.
    tile_p = painted_tile(W, H, (210, 190, 170), (10, 10, 10), (0, 0, 500, 60))
    tp = MemTiles({(0, 0): tile_p})
    ring_p = [(0.0, 0.0), (200.0, 0.0), (200.0, 300.0), (0.0, 300.0)]
    med_p, n_p, _, _ = median_of_building(tp, ring_p)
    check("mediane robuste aux polluants", med_p == (210.0, 190.0, 170.0),
          "med=%s" % (med_p,))
    # 9. Sliver : batiment plus fin que le pas initial -> raffinement, pas d'echec.
    ring_sl = [(100.0, 200.0), (180.0, 200.0), (180.0, 200.5), (100.0, 200.5)]
    med_sl, n_sl, _, _ = median_of_building(tiles, ring_sl)
    check("sliver 80 x 0,5 m echantillonne", med_sl is not None and n_sl >= MIN_SAMPLES,
          "n=%d" % n_sl)

    print("SELFTEST " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--limit", type=int, default=0, help="n premiers batiments (implique --dry-run)")
    ap.add_argument("--dry-run", action="store_true", help="ne pas reecrire le JSON")
    # Le gris de reference est par DEFAUT la mediane globale (p50) des toits en
    # pente : le toit moyen garde tint = 1. ATTENTION : FCityMeshBuilder::Encode
    # ecrete les couleurs de sommet a [0 ; 1], donc toute teinte > 1 est perdue
    # (un toit clair ne peut pas eclaircir la texture, seulement l'assombrir).
    # Monter ce percentile (p. ex. 85) fait tenir le haut de la distribution sous
    # 1 et rend la variation des toits CLAIRS visible, au prix d'un assombrissement
    # global. Decision du superviseur : p50 par defaut, conforme a la spec J3c.
    ap.add_argument("--ref-pct", type=float, default=50.0,
                    help="percentile du gris de reference (defaut 50 = mediane)")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(selftest())

    t0 = time.time()
    log("chargement %s" % BATI_PATH)
    with open(BATI_PATH, encoding="utf-8") as f:
        root = json.load(f)
    buildings = root["buildings"]
    total = len(buildings) if args.limit <= 0 else min(args.limit, len(buildings))
    dry = args.dry_run or args.limit > 0
    tiles_on_disk = len([p for p in os.listdir(ORTHO_DIR) if p.startswith("ortho_20cm_")
                         and p.endswith(".jpg")]) if os.path.isdir(ORTHO_DIR) else 0
    log("batiments : %d/%d, tuiles ortho sur disque : %d%s"
        % (total, len(buildings), tiles_on_disk, " (DRY-RUN)" if dry else ""))

    # Index tuile -> batiments dont la bbox la touche : on charge chaque JPEG UNE
    # fois (18 Mo decompresses) et on traite tous ses toits d'un coup.
    tile_map = defaultdict(list)
    n_outside = 0
    for i in range(total):
        ring = buildings[i]["pts"]
        xs = [p[0] for p in ring]
        ys = [p[1] for p in ring]
        cx0, cx1 = cell_of(min(xs)), cell_of(max(xs))
        cy0, cy1 = cell_of(min(ys)), cell_of(max(ys))
        touched = 0
        for cx in range(max(cx0, CELL_MIN), min(cx1, CELL_MAX) + 1):
            for cy in range(max(cy0, CELL_MIN), min(cy1, CELL_MAX) + 1):
                tile_map[(cx, cy)].append(i)
                touched += 1
        if touched == 0:
            n_outside += 1
    log("index : %d tuiles concernees, %d batiments hors dalle" % (len(tile_map), n_outside))

    tiles = DiskTiles(ORTHO_DIR)
    acc = {}                 # i -> (array r, array g, array b)
    partial = set()          # batiments dont au moins une tuile manque
    missing_tiles = set()
    n_pix = 0
    done = 0
    for k, (cx, cy) in enumerate(sorted(tile_map.keys())):
        if not tiles.has(cx, cy):
            missing_tiles.add((cx, cy))
            for i in tile_map[(cx, cy)]:
                partial.add(i)
            continue
        for i in tile_map[(cx, cy)]:
            a = acc.get(i)
            if a is None:
                a = (array("B"), array("B"), array("B"))
                acc[i] = a
            n, _, miss = sample_points(tiles, interior_points(buildings[i]["pts"]),
                                       a[0], a[1], a[2])
            n_pix += n
            if miss:
                partial.add(i)
        done += 1
        if done % 25 == 0:
            log("  %d/%d tuiles traitees (%d batiments touches, %.1f M pixels)"
                % (done, len(tile_map), len(acc), n_pix / 1e6))
    tiles.data = None

    # Medianes par batiment, puis gris de reference = mediane GLOBALE des toits
    # en PENTE (le toit moyen garde tint = 1 : la texture terracotta reste calibree).
    med = {}
    for i, (r, g, b) in acc.items():
        if len(r) >= MIN_SAMPLES:
            med[i] = (median(r), median(g), median(b))
    acc.clear()
    pitched = [i for i in med if buildings[i].get("roof")]
    if not pitched:
        log("ERREUR : aucun toit en pente echantillonne, pas de gris de reference")
        sys.exit(2)

    def percentile(v, p):
        s = sorted(v)
        return float(s[min(max(int(round(p / 100.0 * (len(s) - 1))), 0), len(s) - 1)])

    ref = tuple(percentile([med[i][c] for i in pitched], args.ref_pct) for c in range(3))
    log("gris de reference (p%g de %d toits en pente) : R=%.1f G=%.1f B=%.1f"
        % (args.ref_pct, len(pitched), ref[0], ref[1], ref[2]))

    n_tint_pitched = n_tint_flat = 0
    chans = ([], [], [])
    for i, m in med.items():
        t = to_tint(m, ref)
        buildings[i]["tint"] = t
        if buildings[i].get("roof"):
            n_tint_pitched += 1
        else:
            n_tint_flat += 1
        for c in range(3):
            chans[c].append(t[c])

    n_pitched_tot = sum(1 for i in range(total) if buildings[i].get("roof"))
    log("teintes : %d batiments (%.1f %%), dont %d en pente (%.1f %% des %d pentes) "
        "et %d plats" % (len(med), 100.0 * len(med) / max(total, 1), n_tint_pitched,
                         100.0 * n_tint_pitched / max(n_pitched_tot, 1), n_pitched_tot,
                         n_tint_flat))
    log("sans teinte : %d hors dalle, %d touches par une tuile manquante, "
        "%d tuiles manquantes sur la dalle" % (n_outside, len(partial), len(missing_tiles)))
    if missing_tiles:
        log("  tuiles manquantes : %s%s"
            % (sorted(missing_tiles)[:12], " ..." if len(missing_tiles) > 12 else ""))

    for c, name in enumerate("RGB"):
        v = chans[c]
        log("  canal %s : min %.3f  p10 %.3f  p50 %.3f  p90 %.3f  max %.3f"
            % (name, min(v), percentile(v, 10), percentile(v, 50),
               percentile(v, 90), max(v)))
    n_tot = 3 * max(len(med), 1)
    n_clamp_lo = sum(1 for c in range(3) for x in chans[c] if x <= TINT_MIN)
    n_clamp_hi = sum(1 for c in range(3) for x in chans[c] if x >= TINT_MAX)
    log("  canaux ecretes par le script : %d a %.1f, %d a %.1f (sur %d)"
        % (n_clamp_lo, TINT_MIN, n_clamp_hi, TINT_MAX, n_tot))
    # Verrou d'information : Encode() du builder C++ ecrete les vertex colors a 1,0
    # (elles finissent en FColor 8 bits). Tout canal > 1/0,95 est donc plafonne au
    # rendu — la part eclaircissante du signal ne passe pas.
    n_gt1 = sum(1 for c in range(3) for x in chans[c] if x > 1.0 / 0.95)
    log("  canaux > 1/0,95 (plafonnes par Encode au rendu) : %d (%.1f %% de %d)"
        % (n_gt1, 100.0 * n_gt1 / n_tot, n_tot))

    if dry:
        log("DRY-RUN : JSON non reecrit. %.0f s" % (time.time() - t0))
        return

    if not os.path.isfile(BACKUP_PATH):
        shutil.copy2(BATI_PATH, BACKUP_PATH)
        log("sauvegarde %s (%.1f Mo)" % (BACKUP_PATH, os.path.getsize(BACKUP_PATH) / 1e6))
    else:
        log("sauvegarde deja presente, conservee : %s" % BACKUP_PATH)

    # Reecriture COMPLETE dans le style compact de j3b_prep_toits.py (ordre des
    # cles preserve : pts, h, u, roof, tint) pour que le diff reste lisible.
    size_before = os.path.getsize(BATI_PATH)
    parts = []
    for b in buildings:
        rec = '{"pts":%s,"h":%s,"u":"%s"' % (
            json.dumps(b["pts"], separators=(",", ":")),
            json.dumps(b["h"]), b["u"])
        if b.get("roof"):
            rec += ',"roof":' + json.dumps(b["roof"], separators=(",", ":"))
        if b.get("tint"):
            rec += ',"tint":' + json.dumps(b["tint"], separators=(",", ":"))
        parts.append(rec + "}")
    src = root.get("source", "")
    if "j3c_tint_toits" not in src:
        src = (src + " + j3c_tint_toits.py (teinte BD ORTHO)").strip(" +")
    out = ('{"source":%s,"origin":%s,"sizeM":%s,"buildings":[%s]}') % (
        json.dumps(src, separators=(",", ":")),
        json.dumps(root.get("origin"), separators=(",", ":")),
        json.dumps(root.get("sizeM"), separators=(",", ":")),
        ",".join(parts))
    with open(BATI_PATH, "w", encoding="utf-8") as f:
        f.write(out)
    log("FIN : %s %.1f -> %.1f Mo, %d batiments, %.1f M pixels lus, %.0f s"
        % (os.path.basename(BATI_PATH), size_before / 1e6,
           os.path.getsize(BATI_PATH) / 1e6, len(parts), n_pix / 1e6, time.time() - t0))


if __name__ == "__main__":
    main()
