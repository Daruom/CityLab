# J3c « MAQUETTE DU SOL » — LE SOL EST PEINT, LE RELIEF EST MAILLE.
#
# Le corridor cadastral (Tools/j3c_sols_corridor.py, VALIDE) donne la verite
# geometrique du sol public. Ce script la CUIT, cellule de 500 m par cellule :
#
#   1. un MASQUE PNG 1024 x 1024 (48,8 cm/px) par cellule :
#        R = identifiant de CLASSE (0 hors corridor, 1 trottoir/place,
#            2 chaussee publique, 3 voirie privee, 4 gravier/chemin) ;
#        G = champ de distance SIGNE a la frontiere de la CHAUSSEE ;
#        B = idem voirie privee ; A = idem gravier.
#      Les SDF sont calcules a 24,4 cm/px (surechantillonnage x2) puis moyennes :
#      c'est eux, et non le canal R, qui donnent au shader des bords NETS au zoom
#      (le canal R reste pour le diagnostic et la verification).
#      Encodage SDF : octet = 0,5 + clamp(d, -2 m, +2 m) / 4 m, d > 0 = DEDANS,
#      soit 1,57 cm par niveau.
#
#   2. un JSON par cellule, le RELIEF qui restera maille :
#        curbs     polylignes de BORDURE (frontiere chaussee <-> trottoir),
#                  ORIENTEES chaussee A GAUCHE, hors autoroutier (accotement sans
#                  bordure), hors voirie privee, coupees aux traversees et aux ponts ;
#        crossings sites de PASSAGE PIETON (axe pieton OSM qui coupe la chaussee) ;
#        axial     tirets de ligne axiale deja decoupes (3 m plein / 1,5 m vide),
#                  voies >= 2, ecartes de 8 m des carrefours.
#
# Les PONTS (position_par_rapport_au_sol > 0) sont EXCLUS du masque : ils restent
# les rubans/tabliers du generateur C++ — un pont ne se peint pas sur le sol.
# Les espaces VERTS ne sont pas peints non plus : ils restent les meshes existants
# poses au-dessus de la dalle.
#
# Usage :
#   python j3c_sols_masks.py --selftest        # verrou geometrique synthetique
#   python j3c_sols_masks.py                   # zone proto : cellules -1..0 x -1..0
#   python j3c_sols_masks.py --cells -1,-1 0,0 # cellules explicites
#   python j3c_sols_masks.py --no-preview      # sans les PNG de controle
import argparse
import json
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "pylib"))
import numpy as np
from PIL import Image, ImageDraw
from shapely.geometry import LineString, Point, Polygon, box
from shapely.ops import linemerge, unary_union
from shapely.prepared import prep

import j3c_sols_corridor as C

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "SourceData")
GF = os.path.join(SRC, "GrandFetch")
OUT_DIR = os.path.join(SRC, "Sols")
SAVED = os.path.join(ROOT, "Saved")
LOG_PATH = os.path.join(SRC, "sols_masks.progress.log")
OSM_PATH = os.path.join(SRC, "toulouse10.json")

CELL_M = 500.0          # cote d'une cellule (== CellSizeM du generateur)
OUT_PX = 1024           # cote du masque cuit : 48,83 cm/px
SS = 2                  # surechantillonnage du calcul du SDF : 24,41 cm/px
HI = 4                  # surechantillonnage de la RASTERISATION : 6,10 cm/px
MARGIN_PX = 16          # marge de calcul (en px SS) : les voisins comptent au bord
SDF_RANGE_M = 2.0       # +/- 2 m encodes sur l'octet -> 1,57 cm par niveau
BAND_PX = 10            # rayon exact du calcul de distance, en px SS (2,44 m)

CLS_HORS = 0            # sous parcelle / bati / eau : la dalle, sans plus
CLS_TROTTOIR = 1        # corridor public hors chaussee : trottoirs, places, parvis
CLS_CHAUSSEE = 2
CLS_PRIVEE = 3
CLS_GRAVIER = 4

# Natures BD TOPO qui ne portent PAS de bordure : sur autoroute et bretelle, la
# chaussee finit en accotement, pas en trottoir.
NATURES_SANS_BORDURE = {"type autoroutier", "bretelle"}
# Types OSM pietons : ils ne peignent rien (le pieton, c'est la dalle) mais leur
# croisement avec la chaussee dit ou la vraie ville avait un passage.
OSM_PIETON = {"pedestrian", "footway", "path", "sidewalk", "steps", "platform", "track"}

DASH_ON_M = 3.0         # tiret plein
DASH_OFF_M = 1.5        # vide
DASH_CLEAR_M = 8.0      # distance minimale a un carrefour
# Une ligne axiale ne se peint pas dans une ruelle. BD TOPO code « 2 voies » des
# qu'une rue est a double sens — dans le centre de Toulouse, c'est la moitie des
# ruelles de 4 m. Le seuil de largeur est le meme que celui du marquage historique
# du generateur (bMarking, 5,50 m).
DASH_MIN_WIDTH_M = 5.5
# Deux sentiers OSM traversent souvent la meme rue a quelques metres : un seul
# passage pieton par rayon. Un vrai carrefour garde ses passages (un par branche,
# separes de plus que ca).
CROSS_DEDUP_M = 7.0
CROSS_HALF_LEN_M = 2.0  # demi-longueur du passage dans l'axe de la rue (GCrossingHalfLenCm)
CURB_MIN_LEN_M = 1.5    # une bordure plus courte que ca est un artefact de decoupe
CURB_SIMPLIFY_M = 0.15


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


# --------------------------------------------------------------- distance exacte
def edt_band(mask, radius):
    """Distance euclidienne (en px) de chaque pixel au pixel True le plus proche.

    EXACTE tant que la distance est <= radius, saturee au-dela : c'est tout ce que
    demande un SDF borne a +/- 2 m, et ca evite un transform complet.
    Methode en deux temps, entierement vectorisee :
      1. distance 1D par LIGNE (deux balayages cumules d'indices) ;
      2. enveloppe des paraboles sur une FENETRE de +/- radius lignes — au-dela,
         la contribution ne peut plus produire une distance <= radius.
    """
    h, w = mask.shape
    big = np.float32(1e6)
    xs = np.arange(w, dtype=np.float32)[None, :]
    seeds_f = np.where(mask, xs, np.float32(-1e6))
    d_left = xs - np.maximum.accumulate(seeds_f, axis=1)
    seeds_b = np.where(mask, xs, big)
    d_right = np.minimum.accumulate(seeds_b[:, ::-1], axis=1)[:, ::-1] - xs
    g = np.minimum(d_left, d_right)
    lim = np.float32(radius + 1)
    g = np.clip(g, 0.0, lim)
    g2 = (g * g).astype(np.float32)
    out = g2.copy()
    fill = np.float32(lim * lim)
    for k in range(1, radius + 1):
        kk = np.float32(k * k)
        up = np.empty_like(g2)
        up[:k, :] = fill
        up[k:, :] = g2[:-k, :]
        np.minimum(out, up + kk, out=out)
        dn = np.empty_like(g2)
        dn[-k:, :] = fill
        dn[:-k, :] = g2[k:, :]
        np.minimum(out, dn + kk, out=out)
    return np.sqrt(out)


def signed_distance_px(mask, radius):
    """SDF en PIXELS, positif DEDANS, convention demi-pixel (le bord tombe entre
    deux centres de pixels : un pixel colle au bord vaut donc +/- 0,5)."""
    if not mask.any():
        return np.full(mask.shape, -(radius + 1.0), dtype=np.float32)
    if mask.all():
        return np.full(mask.shape, radius + 1.0, dtype=np.float32)
    d_out = edt_band(~mask, radius)   # pour un pixel DEDANS : distance au dehors
    d_in = edt_band(mask, radius)     # pour un pixel DEHORS : distance au dedans
    return np.where(mask, d_out - 0.5, -(d_in - 0.5)).astype(np.float32)


def encode_sdf(sdf_px, m_per_px):
    """SDF pixels -> octet 1..255 (128 = frontiere, +/- SDF_RANGE_M aux bornes).

    Plancher a 1 et non 0 : une cellule SANS gravier (le centre-ville n'en a pas)
    donnerait un canal alpha UNIFORMEMENT nul, et un PNG dont l'alpha est plat a
    zero est exactement le genre de piege que les importeurs simplifient. Le cout
    est de 1,57 cm au-dela d'une borne deja saturee — rien."""
    d_m = sdf_px * m_per_px
    v = 0.5 + np.clip(d_m, -SDF_RANGE_M, SDF_RANGE_M) / (2.0 * SDF_RANGE_M)
    return np.clip(np.rint(v * 255.0), 1, 255).astype(np.uint8)


# ------------------------------------------------------------------ rasterisation
def rasterize(geom, size, x0, y0, px_m):
    """Masque booleen d'une geometrie shapely (repere local metres -> pixels, la
    ligne croit avec y : NORD en haut, cf. j3c_sols_corridor).

    PIL remplit un polygone de bord a bord INCLUS : rasterise directement a
    24 cm/px, toute bande sort 25 cm trop large — mesure : une chaussee de 10,000 m
    devient 10,252 m. Ce n'est pas du bruit, c'est un DECALAGE systematique entre la
    peinture et la bordure maillee (qui, elle, vient de la geometrie exacte). Parade
    mesuree : rasteriser HI fois plus fin, moyenner l'aire, seuiller a 50 % —
    la meme chaussee ressort a 10,008 m, soit 8 mm, trente fois sous la
    quantification du SDF lui-meme."""
    n = size * HI
    img = Image.new("L", (n, n), 0)
    if geom is not None and not geom.is_empty:
        d = ImageDraw.Draw(img)
        px = px_m / HI

        def tx(ring):
            return [((c[0] - x0) / px - 0.5, (c[1] - y0) / px - 0.5) for c in ring.coords]

        for poly in C.polys_of(geom):
            d.polygon(tx(poly.exterior), fill=255)
            for r in poly.interiors:
                d.polygon(tx(r), fill=0)
    cov = np.asarray(img, dtype=np.uint8).reshape(size, HI, size, HI).mean(axis=(1, 3),
                                                                          dtype=np.float32)
    return cov >= 127.5


# --------------------------------------------------------------------- chargements
def charger_routes_bdtopo(fen):
    """Tous les troncons BD TOPO de la fenetre, avec ce qu'il faut pour trancher :
    largeur mesuree, nature, position (pont), nombre de voies."""
    with open(C.ROUTES_PATH, encoding="utf-8") as f:
        data = json.load(f)
    out = []
    stats = {"lus": 0, "gardes": 0, "mesure": 0, "repli": 0, "pont": 0,
             "souterrain": 0, "nature": 0, "autre": 0}
    for tr in data.get("troncons", []):
        pts = tr.get("pts") or []
        stats["lus"] += 1
        if len(pts) < 2 or not C.bbox_hit(C.bbox_of(pts), *fen):
            continue
        nat = C.norm(tr.get("nature"))
        pos = C.as_int(tr.get("position_par_rapport_au_sol"), 0) or 0
        if nat in C.NATURES_EXCLUES:
            stats["nature"] += 1
            continue
        if (C.norm(tr.get("acces_vehicule_leger")) == "physiquement impossible"
                or tr.get("fictif") is True
                or (tr.get("etat_de_l_objet") and C.norm(tr.get("etat_de_l_objet")) != "en service")):
            stats["autre"] += 1
            continue
        if pos < 0:
            stats["souterrain"] += 1
            continue
        largeur, source = C.largeur_de(tr)
        rec = {
            "line": LineString(pts),
            "largeur": largeur,
            "nature": nat,
            "pont": pos > 0,
            "voies": C.as_int(tr.get("nombre_de_voies"), 0) or 0,
            "etroit": nat in C.NATURES_ETROITES,
            "sans_bordure": nat in NATURES_SANS_BORDURE,
        }
        if rec["pont"]:
            stats["pont"] += 1
        else:
            stats["gardes"] += 1
            stats[source] += 1
        out.append(rec)
    log("routes BD TOPO : %d troncons dans la fenetre (%d au sol, %d ponts exclus du "
        "masque, %d souterrains, %d natures pietonnes, %d hors service) ; largeurs "
        "mesurees %d / replis %d"
        % (len(out), stats["gardes"], stats["pont"], stats["souterrain"], stats["nature"],
           stats["autre"], stats["mesure"], stats["repli"]))
    return out


def charger_axes_pietons(fen):
    """Axes pietons OSM (toulouse10.json) : ils ne peignent rien, ils DISENT ou la
    ville a des traversees."""
    if not os.path.exists(OSM_PATH):
        log("ATTENTION : %s absent, aucun passage pieton ne sera propose" % OSM_PATH)
        return []
    with open(OSM_PATH, encoding="utf-8-sig") as f:
        data = json.load(f)
    out = []
    for r in data.get("roads", []):
        if r.get("t") not in OSM_PIETON:
            continue
        pts = r.get("pts") or []
        if len(pts) < 2 or not C.bbox_hit(C.bbox_of(pts), *fen):
            continue
        out.append(LineString(pts))
    log("axes pietons OSM : %d dans la fenetre" % len(out))
    return out


# ------------------------------------------------------------------- une cellule
def classes_de_cellule(zone, parcelles, eaux, routes):
    """corridor / chaussee / privee / gravier d'une emprise donnee (deja elargie de
    la marge). Meme soustraction que le corridor valide, plus la separation des
    natures etroites (gravier) et des bandes tombees dans une parcelle (privee)."""
    u_parc = unary_union(parcelles) if parcelles else None
    u_eau = unary_union(eaux) if eaux else None
    corridor = zone
    if u_parc is not None:
        corridor = corridor.difference(u_parc)
    if u_eau is not None:
        corridor = corridor.difference(u_eau)
    corridor = C.valide(corridor)

    def bande(sel):
        parts = [r["line"].buffer(r["largeur"] / 2.0, cap_style=2, join_style=2, mitre_limit=3.0)
                 for r in routes if sel(r) and r["largeur"] > 0]
        return C.valide(unary_union(parts)) if parts else zone.difference(zone)

    # Le PONT ne se peint pas : il reste un ruban 3D. On le retire de tout.
    b_pont = bande(lambda r: r["pont"])
    b_large = bande(lambda r: not r["pont"] and not r["etroit"])
    b_etroite = bande(lambda r: not r["pont"] and r["etroit"])
    if not b_pont.is_empty:
        b_large = C.valide(b_large.difference(b_pont))
        b_etroite = C.valide(b_etroite.difference(b_pont))

    chaussee = C.valide(b_large.intersection(corridor))
    gravier = C.valide(b_etroite.intersection(zone).difference(chaussee))
    privee = C.valide(b_large.intersection(zone).difference(corridor).difference(gravier))
    return corridor, chaussee, privee, gravier


def curb_lines(chaussee, cell_box, routes, crossings):
    """Polylignes de bordure : la frontiere de la chaussee, PRIVEE de ce qui n'a pas
    de bordure (autoroutier, pont) et de ce qui l'interrompt (les traversees), puis
    ramenee a la cellule et ORIENTEE chaussee a gauche."""
    if chaussee.is_empty:
        return []
    bnd = chaussee.boundary
    coupes = []
    for r in routes:
        if r["pont"] or r["sans_bordure"]:
            coupes.append(r["line"].buffer(r["largeur"] / 2.0 + 1.0, cap_style=2, join_style=2))
    for cr in crossings:
        d = cr["d"]
        n = (-d[1], d[0])
        half = cr["halfW"] + 0.6           # jusqu'au pied du trottoir
        along = CROSS_HALF_LEN_M + 0.4
        cx, cy = cr["p"]
        quad = Polygon([
            (cx - d[0] * along - n[0] * half, cy - d[1] * along - n[1] * half),
            (cx + d[0] * along - n[0] * half, cy + d[1] * along - n[1] * half),
            (cx + d[0] * along + n[0] * half, cy + d[1] * along + n[1] * half),
            (cx - d[0] * along + n[0] * half, cy - d[1] * along + n[1] * half)])
        coupes.append(quad)
    if coupes:
        bnd = bnd.difference(unary_union(coupes))
    bnd = bnd.intersection(cell_box)
    if bnd.is_empty:
        return []
    try:
        merged = linemerge(bnd)
    except Exception:
        merged = bnd
    lignes = []
    for g in (merged.geoms if merged.geom_type.startswith("Multi") or
              merged.geom_type == "GeometryCollection" else [merged]):
        if g.geom_type != "LineString" or g.length < CURB_MIN_LEN_M:
            continue
        s = g.simplify(CURB_SIMPLIFY_M, preserve_topology=False)
        if s.length >= CURB_MIN_LEN_M and len(s.coords) >= 2:
            lignes.append(s)

    # ORIENTATION : la chaussee doit etre a GAUCHE du sens de parcours. On ne se fie
    # pas au sens des anneaux shapely apres decoupe — on SONDE, segment par segment.
    pre = prep(chaussee)
    out = []
    for ln in lignes:
        cs = list(ln.coords)
        votes = 0
        for i in range(len(cs) - 1):
            ax, ay = cs[i]
            bx, by = cs[i + 1]
            dx, dy = bx - ax, by - ay
            d = math.hypot(dx, dy)
            if d < 1e-6:
                continue
            dx, dy = dx / d, dy / d
            mx, my = (ax + bx) * 0.5, (ay + by) * 0.5
            gauche = pre.contains(Point(mx - dy * 0.10, my + dx * 0.10))
            droite = pre.contains(Point(mx + dy * 0.10, my - dx * 0.10))
            if gauche and not droite:
                votes += 1
            elif droite and not gauche:
                votes -= 1
        if votes < 0:
            cs.reverse()
        out.append(cs)
    return out


def crossing_sites(chaussee, cell_box, routes, pietons):
    """Un site par croisement d'un axe pieton OSM avec la chaussee : position au
    milieu de la traversee, direction et demi-largeur EMPRUNTEES a la chaussee la
    plus proche (le quad se pose dans l'axe de la rue, comme BuildCrossing)."""
    if chaussee.is_empty or not pietons:
        return []
    autos = [r for r in routes if not r["pont"] and not r["etroit"]]
    if not autos:
        return []
    sites = []
    for ped in pietons:
        try:
            inter = ped.intersection(chaussee)
        except Exception:
            continue
        if inter.is_empty:
            continue
        parts = ([inter] if inter.geom_type == "LineString"
                 else [g for g in getattr(inter, "geoms", []) if g.geom_type == "LineString"])
        for seg in parts:
            if seg.length < 1.0:
                continue
            p = seg.interpolate(seg.length * 0.5)
            if not cell_box.contains(p):
                continue
            best = None
            for r in autos:
                d = r["line"].distance(p)
                if best is None or d < best[0]:
                    best = (d, r)
            if best is None or best[0] > best[1]["largeur"] * 0.75 + 2.0:
                continue
            r = best[1]
            s = r["line"].project(p)
            a = r["line"].interpolate(max(0.0, s - 2.0))
            b = r["line"].interpolate(min(r["line"].length, s + 2.0))
            dx, dy = b.x - a.x, b.y - a.y
            n = math.hypot(dx, dy)
            if n < 1e-6:
                continue
            sites.append({"p": (round(p.x, 2), round(p.y, 2)),
                          "d": (round(dx / n, 5), round(dy / n, 5)),
                          "halfW": round(r["largeur"] * 0.5, 3)})
    # Deduplication GLOUTONNE : la rue la plus large d'abord, puis on refuse tout
    # site a moins de CROSS_DEDUP_M d'un site deja retenu. Une grille de cles
    # arrondies ne suffisait pas — deux sites de part et d'autre d'une frontiere de
    # case passaient tous les deux.
    sites.sort(key=lambda s: -s["halfW"])
    gardes = []
    r2 = CROSS_DEDUP_M * CROSS_DEDUP_M
    for s in sites:
        if all((s["p"][0] - g["p"][0]) ** 2 + (s["p"][1] - g["p"][1]) ** 2 > r2 for g in gardes):
            gardes.append(s)
    return gardes


def junction_points(routes):
    """Carrefours BD TOPO : extremites partagees par au moins 3 troncons, plus les
    points ou un troncon en traverse un autre en son milieu."""
    compte = {}
    for r in routes:
        if r["pont"]:
            continue
        cs = list(r["line"].coords)
        for p in (cs[0], cs[-1]):
            k = (round(p[0] * 10), round(p[1] * 10))
            compte[k] = compte.get(k, 0) + 1
    return [(k[0] / 10.0, k[1] / 10.0) for k, n in compte.items() if n >= 3]


def axial_dashes(routes, chaussee, cell_box, jonctions):
    """Tirets de ligne axiale : voies >= 2, dans la chaussee, a plus de 8 m d'un
    carrefour. Le decoupage est fait ICI (le C++ ne fait que poser des quads)."""
    if chaussee.is_empty:
        return []
    pre = prep(chaussee)
    jx = np.array([p[0] for p in jonctions], dtype=np.float64) if jonctions else None
    jy = np.array([p[1] for p in jonctions], dtype=np.float64) if jonctions else None
    out = []
    period = DASH_ON_M + DASH_OFF_M
    for r in routes:
        if (r["pont"] or r["etroit"] or r["voies"] < 2
                or r["largeur"] < DASH_MIN_WIDTH_M):
            continue
        ln = r["line"]
        L = ln.length
        if L < period:
            continue
        s = DASH_OFF_M * 0.5
        while s + DASH_ON_M <= L:
            a = ln.interpolate(s)
            b = ln.interpolate(s + DASH_ON_M)
            s += period
            mx, my = (a.x + b.x) * 0.5, (a.y + b.y) * 0.5
            if not cell_box.contains(Point(mx, my)):
                continue
            if jx is not None and jx.size:
                if np.min((jx - mx) ** 2 + (jy - my) ** 2) < DASH_CLEAR_M * DASH_CLEAR_M:
                    continue
            if not pre.contains(Point(mx, my)):
                continue
            out.append([round(a.x, 2), round(a.y, 2), round(b.x, 2), round(b.y, 2)])
    return out


def cuire_cellule(cx, cy, parcelles, eaux, routes, pietons, preview=True):
    t0 = time.time()
    x0, y0 = cx * CELL_M, cy * CELL_M
    cell_box = box(x0, y0, x0 + CELL_M, y0 + CELL_M)
    px_ss = CELL_M / (OUT_PX * SS)                      # metres par pixel de calcul
    marge_m = MARGIN_PX * px_ss
    zone = box(x0 - marge_m, y0 - marge_m, x0 + CELL_M + marge_m, y0 + CELL_M + marge_m)

    loc_parc = [p for p in parcelles if p.intersects(zone)]
    loc_eau = [e for e in eaux if e.intersects(zone)]
    loc_routes = [r for r in routes if r["line"].intersects(zone)]
    corridor, chaussee, privee, gravier = classes_de_cellule(zone, loc_parc, loc_eau, loc_routes)

    # --- rasterisation (grille de calcul : cellule + marge, a 24,41 cm/px)
    size = OUT_PX * SS + 2 * MARGIN_PX
    ox, oy = x0 - marge_m, y0 - marge_m
    m_corr = rasterize(corridor, size, ox, oy, px_ss)
    m_priv = rasterize(privee, size, ox, oy, px_ss)
    m_grav = rasterize(gravier, size, ox, oy, px_ss)
    m_road = rasterize(chaussee, size, ox, oy, px_ss)
    # Priorite du melange : chaussee > gravier > privee > trottoir. Les masques des
    # SDF sont rendus DISJOINTS dans le meme ordre, sinon le shader (qui empile les
    # SDF) et le canal de classe se contrediraient sur les recouvrements.
    m_grav = m_grav & ~m_road
    m_priv = m_priv & ~m_road & ~m_grav
    cls = np.zeros((size, size), dtype=np.uint8)
    cls[m_corr] = CLS_TROTTOIR
    cls[m_priv] = CLS_PRIVEE
    cls[m_grav] = CLS_GRAVIER
    cls[m_road] = CLS_CHAUSSEE

    sdf_road = signed_distance_px(m_road, BAND_PX)
    sdf_priv = signed_distance_px(m_priv, BAND_PX)
    sdf_grav = signed_distance_px(m_grav, BAND_PX)

    def reduire_sdf(sdf):
        c = sdf[MARGIN_PX:MARGIN_PX + OUT_PX * SS, MARGIN_PX:MARGIN_PX + OUT_PX * SS]
        c = c.reshape(OUT_PX, SS, OUT_PX, SS).mean(axis=(1, 3))
        return encode_sdf(c, px_ss)

    crop = (slice(MARGIN_PX, MARGIN_PX + OUT_PX * SS), slice(MARGIN_PX, MARGIN_PX + OUT_PX * SS))
    r_chan = cls[crop][::SS, ::SS]                      # classe : echantillon central
    rgba = np.dstack([r_chan, reduire_sdf(sdf_road), reduire_sdf(sdf_priv), reduire_sdf(sdf_grav)])

    os.makedirs(OUT_DIR, exist_ok=True)
    png = os.path.join(OUT_DIR, "mask_%d_%d.png" % (cx, cy))
    Image.fromarray(rgba, "RGBA").save(png, "PNG", optimize=True)

    # --- le RELIEF qui restera maille
    crossings = crossing_sites(chaussee, cell_box, loc_routes, pietons)
    curbs = curb_lines(chaussee, cell_box, loc_routes, crossings)
    jonctions = junction_points(loc_routes)
    dashes = axial_dashes(loc_routes, chaussee, cell_box, jonctions)

    aires = {}
    for nom, g in (("corridor", corridor), ("chaussee", chaussee),
                   ("privee", privee), ("gravier", gravier)):
        aires[nom] = round(g.intersection(cell_box).area, 1)
    aires["cellule"] = CELL_M * CELL_M

    data = {
        "cell": [cx, cy],
        "cellSizeM": CELL_M,
        "origin": [x0, y0],
        "maskPx": OUT_PX,
        "sdfRangeM": SDF_RANGE_M,
        "classes": {"hors": CLS_HORS, "trottoir": CLS_TROTTOIR, "chaussee": CLS_CHAUSSEE,
                    "privee": CLS_PRIVEE, "gravier": CLS_GRAVIER},
        "curbs": [[[round(c[0], 2), round(c[1], 2)] for c in ln] for ln in curbs],
        "crossings": crossings,
        "axial": dashes,
        "areasM2": aires,
    }
    js = os.path.join(OUT_DIR, "sols_%d_%d.json" % (cx, cy))
    with open(js, "w", encoding="utf-8") as f:
        json.dump(data, f, separators=(",", ":"))

    if preview:
        apercu(cx, cy, r_chan, rgba[:, :, 1], curbs, crossings, dashes, x0, y0)

    pc = 100.0 * aires["chaussee"] / aires["cellule"]
    log("cellule %+d,%+d : chaussee %.1f %% | %d bordures (%.0f m) | %d passages | "
        "%d tirets | masque %.2f Mo | json %.2f Mo | %.1f s"
        % (cx, cy, pc, len(curbs), sum(LineString(l).length for l in curbs if len(l) > 1),
           len(crossings), len(dashes), os.path.getsize(png) / 1048576.0,
           os.path.getsize(js) / 1048576.0, time.time() - t0))
    return {"cell": [cx, cy], "origin": [x0, y0], "png": png, "json": js, "curbs": len(curbs),
            "crossings": len(crossings), "axial": len(dashes),
            "curbLenM": round(sum(LineString(l).length for l in curbs if len(l) > 1), 1),
            "pngBytes": os.path.getsize(png), "jsonBytes": os.path.getsize(js),
            "areasM2": aires}


def apercu(cx, cy, r_chan, g_chan, curbs, crossings, dashes, x0, y0):
    """PNG de controle LISIBLE (le masque, lui, n'est pas fait pour l'oeil)."""
    couleurs = {CLS_HORS: (238, 236, 232), CLS_TROTTOIR: (214, 200, 172),
                CLS_CHAUSSEE: (96, 100, 104), CLS_PRIVEE: (176, 126, 100),
                CLS_GRAVIER: (176, 158, 118)}
    img = np.zeros((OUT_PX, OUT_PX, 3), dtype=np.uint8)
    for k, col in couleurs.items():
        img[r_chan == k] = col
    # Le fil du SDF : la ou il passe par 0,5, la frontiere de chaussee doit tomber
    # EXACTEMENT sur le bord du gris — c'est le controle croise masque / SDF.
    bord = np.abs(g_chan.astype(np.int16) - 128) <= 3
    img[bord] = (255, 64, 64)
    im = Image.fromarray(img, "RGB").resize((OUT_PX, OUT_PX), Image.NEAREST)
    d = ImageDraw.Draw(im)
    sc = OUT_PX / CELL_M

    def tx(p):
        return ((p[0] - x0) * sc, (p[1] - y0) * sc)

    for ln in curbs:
        if len(ln) >= 2:
            d.line([tx(c) for c in ln], fill=(30, 200, 90), width=2)
    for s in dashes:
        d.line([tx((s[0], s[1])), tx((s[2], s[3]))], fill=(255, 255, 255), width=2)
    for c in crossings:
        px, py = tx(c["p"])
        d.ellipse([px - 5, py - 5, px + 5, py + 5], outline=(40, 90, 240), width=3)
    os.makedirs(SAVED, exist_ok=True)
    p = os.path.join(SAVED, "sols_masque_%d_%d.png" % (cx, cy))
    im.save(p, "PNG", optimize=True)
    return p


# ------------------------------------------------------------------------ selftest
def selftest():
    ok = True

    def check(nom, got, att, tol=1e-6):
        nonlocal ok
        bon = abs(got - att) <= tol
        ok = ok and bon
        log("selftest %-34s attendu %10.4f  obtenu %10.4f  %s"
            % (nom, att, got, "OK" if bon else "ECHEC"))

    def check_bool(nom, got, att):
        nonlocal ok
        bon = (bool(got) == bool(att))
        ok = ok and bon
        log("selftest %-34s attendu %10s  obtenu %10s  %s"
            % (nom, str(att), str(got), "OK" if bon else "ECHEC"))

    # 1. Distance exacte dans la bande : un disque de rayon connu.
    n = 128
    yy, xx = np.mgrid[0:n, 0:n]
    disque = ((xx - 64.0) ** 2 + (yy - 64.0) ** 2) <= 30.0 ** 2
    sdf = signed_distance_px(disque, 10)
    check("SDF centre du disque (sature)", float(sdf[64, 64]), 10.5, 0.6)
    check("SDF a 5 px du bord (dedans)", float(sdf[64, 64 + 25]), 5.0, 0.6)
    check("SDF a 5 px du bord (dehors)", float(sdf[64, 64 + 35]), -5.0, 0.6)
    check_bool("SDF signe dedans", sdf[64, 64] > 0, True)
    check_bool("SDF signe dehors", sdf[0, 0] < 0, True)

    # 2. Encodage : la frontiere DOIT tomber sur 128 (0,5) et rester monotone.
    e = encode_sdf(np.array([[-4.0, -2.0, -0.001, 0.001, 2.0, 4.0]], dtype=np.float32), 1.0)
    check("encodage frontiere -> 128", float(e[0, 2]), 128.0, 1.0)
    check("encodage -2 m -> plancher 1", float(e[0, 1]), 1.0, 0.0)
    check("encodage +2 m -> 255", float(e[0, 4]), 255.0, 1.0)
    check("encodage sature en bas", float(e[0, 0]), 1.0, 0.0)
    check("encodage sature en haut", float(e[0, 5]), 255.0, 1.0)

    # 3. Cellule SYNTHETIQUE : deux parcelles, une chaussee de 10 m au milieu, un
    #    chemin de 3 m, une allee privee dans la parcelle de gauche.
    zone = box(0, 0, 100, 100)
    p1 = Polygon([(0, 0), (45, 0), (45, 100), (0, 100)])
    p2 = Polygon([(55, 0), (100, 0), (100, 100), (55, 100)])
    routes = [
        {"line": LineString([(50, -10), (50, 110)]), "largeur": 10.0, "nature": "route a 1 chaussee",
         "pont": False, "voies": 2, "etroit": False, "sans_bordure": False},
        {"line": LineString([(0, 80), (100, 80)]), "largeur": 3.0, "nature": "chemin",
         "pont": False, "voies": 0, "etroit": True, "sans_bordure": False},
        {"line": LineString([(20, 0), (20, 100)]), "largeur": 4.0, "nature": "route a 1 chaussee",
         "pont": False, "voies": 1, "etroit": False, "sans_bordure": False},
        {"line": LineString([(0, 30), (100, 30)]), "largeur": 8.0, "nature": "route a 1 chaussee",
         "pont": True, "voies": 2, "etroit": False, "sans_bordure": False},
    ]
    corridor, chaussee, privee, gravier = classes_de_cellule(zone, [p1, p2], [], routes)
    # Corridor = la bande de 10 m entre les parcelles, sur 100 m.
    check("corridor synthetique (m2)", corridor.area, 1000.0, 1.0)
    # La chaussee de 10 m rognee au corridor = la bande entiere ... moins le pont.
    check("chaussee = corridor - pont", chaussee.area, 1000.0 - 10.0 * 8.0, 1.0)
    check_bool("le pont ne se peint pas", chaussee.intersection(
        box(0, 26, 100, 34)).area < 1.0, True)
    # L'allee de 4 m est DANS la parcelle p1 -> voirie privee, MOINS ce que le
    # pont (8 m) et le chemin (3 m) lui prennent : 400 - 4x8 - 4x3 = 356.
    check("voirie privee (m2)", privee.area, 4.0 * 100.0 - 4.0 * 8.0 - 4.0 * 3.0, 2.0)
    # Le chemin de 3 m traverse tout : 100 m x 3 m, moins ce que la chaussee mange.
    check_bool("gravier present", gravier.area > 200.0, True)
    check_bool("gravier disjoint de la chaussee", gravier.intersection(chaussee).area < 0.5, True)

    # 4. Bordures : orientation (chaussee A GAUCHE) et exclusion de l'autoroutier.
    lignes = curb_lines(chaussee, zone, routes, [])
    check_bool("bordures produites", len(lignes) >= 2, True)
    pre = prep(chaussee)
    mauvais = 0
    total = 0
    for ln in lignes:
        for i in range(len(ln) - 1):
            ax, ay = ln[i]
            bx, by = ln[i + 1]
            dx, dy = bx - ax, by - ay
            d = math.hypot(dx, dy)
            if d < 0.5:
                continue
            dx, dy = dx / d, dy / d
            mx, my = (ax + bx) * 0.5, (ay + by) * 0.5
            total += 1
            if not pre.contains(Point(mx - dy * 0.20, my + dx * 0.20)):
                mauvais += 1
    check("bordures mal orientees", float(mauvais), 0.0, 0.0)
    check_bool("bordures echantillonnees", total >= 4, True)

    routes_auto = [dict(routes[0], sans_bordure=True, nature="type autoroutier")] + routes[1:]
    ch2 = classes_de_cellule(zone, [p1, p2], [], routes_auto)[1]
    l2 = curb_lines(ch2, zone, routes_auto, [])
    check("bordures sur autoroutier", float(sum(len(l) for l in l2)), 0.0, 0.0)

    # 5. Passage pieton : un axe pieton qui coupe la chaussee produit un site,
    #    dans l'axe de la RUE (donc vertical ici) et a la demi-largeur mesuree.
    ped = [LineString([(30, 50), (70, 50)])]
    sites = crossing_sites(chaussee, zone, routes, ped)
    check("sites de passage", float(len(sites)), 1.0, 0.0)
    if sites:
        s = sites[0]
        check("passage : demi-largeur", s["halfW"], 5.0, 0.01)
        check("passage : |dy| (axe de rue)", abs(s["d"][1]), 1.0, 0.01)
        check("passage : x du site", s["p"][0], 50.0, 0.6)
    # ... et la bordure s'y INTERROMPT.
    l3 = curb_lines(chaussee, zone, routes, sites)
    trous = [ln for ln in l3
             if any(abs(c[1] - 50.0) < 2.0 for c in ln)]
    check("bordure coupee a la traversee", float(len(trous)), 0.0, 0.0)

    # 6. Tirets axiaux : la route de 10 m a 2 voies en produit, pas la route a 1 voie.
    dashes = axial_dashes(routes, chaussee, zone, [])
    check_bool("tirets produits", len(dashes) > 10, True)
    longueurs = [math.hypot(d[2] - d[0], d[3] - d[1]) for d in dashes]
    check("longueur de tiret", sum(longueurs) / len(longueurs), DASH_ON_M, 0.05)
    check("tirets hors chaussee", float(sum(
        1 for d in dashes if abs((d[0] + d[2]) * 0.5 - 50.0) > 5.0)), 0.0, 0.0)
    d_jonc = axial_dashes(routes, chaussee, zone, [(50.0, 50.0)])
    proches = sum(1 for d in d_jonc
                  if math.hypot((d[0] + d[2]) * 0.5 - 50.0, (d[1] + d[3]) * 0.5 - 50.0) < DASH_CLEAR_M)
    check("tirets dans un carrefour", float(proches), 0.0, 0.0)
    check_bool("carrefour = moins de tirets", len(d_jonc) < len(dashes), True)

    # 7. Rasterisation : c'est ICI que se joue l'alignement peinture <-> bordure.
    #    Un carre de 50 m dans une grille de 100 m a 1 m/px, puis la MESURE qui
    #    compte vraiment : la largeur rendue d'une bande de 10,00 m a 24,41 cm/px.
    arr = rasterize(box(10, 10, 60, 60), 100, 0.0, 0.0, 1.0)
    check("rasterisation 50 x 50 (m2)", float(arr.sum()), 2500.0, 5.0)
    px = CELL_M / (OUT_PX * SS)
    bande = rasterize(box(20.0, 5.0, 30.0, 105.0), 512, 0.0, 0.0, px)
    cols = np.where(bande.any(axis=0))[0]
    check("largeur rendue d'une chaussee de 10 m",
          float((cols.max() - cols.min() + 1) * px), 10.0, 0.05)

    log("SELFTEST : " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# --------------------------------------------------------------------------- passe
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--cells", nargs="*", default=None,
                    help="cellules 'cx,cy' (defaut : la zone proto -1..0 x -1..0)")
    ap.add_argument("--no-preview", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    if args.cells:
        cells = [tuple(int(v) for v in c.split(",")) for c in args.cells]
    else:
        cells = [(cx, cy) for cx in (-1, 0) for cy in (-1, 0)]

    t0 = time.time()
    log("=== MAQUETTE DU SOL : cuisson de %d cellules %s ==="
        % (len(cells), " ".join("%+d,%+d" % c for c in cells)))
    marge = MARGIN_PX * CELL_M / (OUT_PX * SS) + 5.0
    xs = [c[0] for c in cells]
    ys = [c[1] for c in cells]
    fen = (min(xs) * CELL_M - marge, min(ys) * CELL_M - marge,
           (max(xs) + 1) * CELL_M + marge, (max(ys) + 1) * CELL_M + marge)
    fenetres = {"proto": fen}
    parcelles = C.charger_parcelles(fenetres)["proto"]
    eaux, _verts = C.charger_surfaces(fenetres)
    eaux = eaux["proto"]
    routes = charger_routes_bdtopo(fen)
    pietons = charger_axes_pietons(fen)
    log("chargements : %.1f s" % (time.time() - t0))

    resume = []
    for cx, cy in cells:
        resume.append(cuire_cellule(cx, cy, parcelles, eaux, routes, pietons,
                                    preview=not args.no_preview))
    idx = {"cellSizeM": CELL_M, "maskPx": OUT_PX, "sdfRangeM": SDF_RANGE_M,
           "cells": resume, "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S")}
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "index.json"), "w", encoding="utf-8") as f:
        json.dump(idx, f, indent=1)
    tot_png = sum(r["pngBytes"] for r in resume)
    tot_js = sum(r["jsonBytes"] for r in resume)
    log("TOTAL %.1f s | %d cellules | masques %.1f Mo (%.2f Mo/cellule) | json %.2f Mo"
        % (time.time() - t0, len(resume), tot_png / 1048576.0,
           tot_png / 1048576.0 / max(len(resume), 1), tot_js / 1048576.0))
    log("PEINT   : %d m2 de chaussee, %d m2 de voirie privee, %d m2 de gravier"
        % (sum(r["areasM2"]["chaussee"] for r in resume),
           sum(r["areasM2"]["privee"] for r in resume),
           sum(r["areasM2"]["gravier"] for r in resume)))
    log("MAILLE  : %d polylignes de bordure (%.0f m), %d passages, %d tirets"
        % (sum(r["curbs"] for r in resume), sum(r["curbLenM"] for r in resume),
           sum(r["crossings"] for r in resume), sum(r["axial"] for r in resume)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
