# J3c - "SOLS PAR LE CADASTRE" : le corridor public par SOUSTRACTION, jamais par
# devinette. Les batiments du jeu marchent parce qu'ils extrudent des POLYGONES
# vrais ; les sols echouaient parce qu'on devinait des surfaces depuis des AXES.
#
#   corridor public = carre de zone - union(parcelles cadastrales) - eau
#   chaussee        = union(axes routiers bufferises a largeur_de_chaussee/2) I corridor
#   trottoirs/places= corridor - chaussee
#
# Les parcelles (CADASTRALPARCELS.PARCELLAIRE_EXPRESS) couvrent tout le PRIVE +
# le domaine public cadastre ; ce qui reste ("non cadastre") EST la rue, la place,
# le parvis. Les largeurs viennent de BD TOPO (largeur_de_chaussee, MESUREE) et
# non d'une table inventee ; le repli par importance n'est qu'un filet et son
# taux d'usage est rapporte.
#
# Sorties : Saved/sols_corridor_centre.png et Saved/sols_corridor_peripherie.png
# (4000 x 4000 px, 25 cm/px, 1 km de cote), plus les statistiques au log.
#
# Usage :
#   python j3c_sols_corridor.py --selftest    # verrou geometrique (cellule synthetique)
#   python j3c_sols_corridor.py               # les deux zones + les deux PNG
#   python j3c_sols_corridor.py --zone centre # une seule zone
#   python j3c_sols_corridor.py --no-png      # statistiques seules
import argparse
import json
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "pylib"))
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import LineString, Polygon, box
from shapely.ops import unary_union

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "SourceData")
GF = os.path.join(SRC, "GrandFetch")
SAVED = os.path.join(ROOT, "Saved")
PARCELLES_PATH = os.path.join(GF, "parcelles.json")
ROUTES_PATH = os.path.join(GF, "routes_bdtopo.json")
SURFACES_PATH = os.path.join(SRC, "toulouse10_surfaces.json")
BATI_PATH = os.path.join(SRC, "toulouse10_bati.json")
LOG_PATH = os.path.join(SRC, "sols_corridor.progress.log")

# Zones de 1 km2 : le centre dense (Capitole) et une peripherie residentielle.
ZONES = {
    "centre": (-500.0, -500.0, 500.0, 500.0, "centre (Capitole)"),
    "peripherie": (2000.0, 2000.0, 3000.0, 3000.0, "peripherie residentielle (SE)"),
}

PX_M = 0.25              # resolution de la carte : 25 cm par pixel
MARGE_M = 80.0           # marge de chargement autour de la zone (axes debordants)

# Repli de largeur (m) quand largeur_de_chaussee est absente. Filet de securite
# uniquement : le taux d'usage est rapporte, il doit rester marginal.
FALLBACK_LARGEUR = {1: 10.5, 2: 7.0, 3: 6.5, 4: 5.5, 5: 4.5, 6: 3.5}
FALLBACK_DEFAUT = 4.0
FALLBACK_CHEMIN = 3.0    # chemins / routes empierrees : bande etroite

# Natures BD TOPO qui ne sont PAS de la chaussee (elles restent en trottoirs/places).
# Mesure du fetch : ces natures-la sont AUSSI celles ou largeur_de_chaussee est
# vide (sentier/escalier/chemin/empierree = 0 % de remplissage) -- le tri par
# nature et le tri par donnee disponible disent la meme chose.
# Les ronds-points RESTENT de la chaussee (2032 objets, largeur remplie a 100 %).
NATURES_EXCLUES = {"sentier", "escalier", "bac auto", "bac pieton", "piste cyclable"}
NATURES_ETROITES = {"chemin", "route empierree"}

COL_FOND = (255, 255, 255)
COL_PARCELLE = (233, 233, 233)
COL_PARCELLE_BORD = (198, 198, 198)
COL_CORRIDOR = (214, 196, 164)
COL_TROTTOIR = (238, 227, 205)
COL_CHAUSSEE = (92, 96, 100)
COL_PRIVEE = (176, 126, 100)   # bande routiere tombee DANS une parcelle : voirie privee
COL_EAU = (116, 168, 212)
COL_BATI = (150, 150, 150)
COL_VERT = (118, 174, 108)
COL_TEXTE = (30, 30, 30)


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def norm(s):
    """Minuscule sans accents (comparaison des natures BD TOPO en ASCII pur)."""
    if s is None:
        return ""
    s = str(s).lower()
    for a, b in (("\u00e0", "a"), ("\u00e2", "a"), ("\u00e4", "a"), ("\u00e9", "e"),
                 ("\u00e8", "e"), ("\u00ea", "e"), ("\u00eb", "e"), ("\u00ee", "i"),
                 ("\u00ef", "i"), ("\u00f4", "o"), ("\u00f6", "o"), ("\u00f9", "u"),
                 ("\u00fb", "u"), ("\u00fc", "u"), ("\u00e7", "c")):
        s = s.replace(a, b)
    return s.strip()


def as_float(v):
    if v is None:
        return None
    if isinstance(v, (int, float)):
        return float(v)
    try:
        return float(str(v).replace(",", "."))
    except ValueError:
        return None


def as_int(v, defaut=None):
    f = as_float(v)
    return defaut if f is None else int(round(f))


def bbox_of(pts):
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def bbox_hit(bb, x0, y0, x1, y1):
    return not (bb[2] < x0 or bb[0] > x1 or bb[3] < y0 or bb[1] > y1)


def valide(geom):
    """Polygone shapely reparable : buffer(0) reste le rustine la plus sure."""
    if geom.is_valid:
        return geom
    g = geom.buffer(0)
    return g if g.is_valid else geom


def polys_of(geom):
    """Liste des Polygon d'une geometrie quelconque (ignore lignes et points)."""
    if geom.is_empty:
        return []
    gt = geom.geom_type
    if gt == "Polygon":
        return [geom]
    if gt in ("MultiPolygon", "GeometryCollection"):
        out = []
        for g in geom.geoms:
            out.extend(polys_of(g))
        return out
    return []


# ---------------------------------------------------------------- chargements

def charger_parcelles(fenetres):
    """Parcelles (avec trous) dont la bbox touche une des fenetres demandees."""
    with open(PARCELLES_PATH, encoding="utf-8") as f:
        data = json.load(f)
    brut = data.get("parcelles", [])
    res = {k: [] for k in fenetres}
    nb_trous = 0
    nb_invalides = 0
    for p in brut:
        rings = p.get("rings") or []
        if not rings or len(rings[0]) < 3:
            continue
        bb = bbox_of(rings[0])
        cibles = [k for k, w in fenetres.items() if bbox_hit(bb, *w)]
        if not cibles:
            continue
        holes = [r for r in rings[1:] if len(r) >= 3]
        nb_trous += len(holes)
        try:
            g = Polygon(rings[0], holes)
        except Exception:
            nb_invalides += 1
            continue
        if not g.is_valid:
            g = valide(g)
            if not g.is_valid or g.is_empty:
                nb_invalides += 1
                continue
        for k in cibles:
            res[k].append(g)
    log("parcelles : %d lues, %s retenues, %d trous, %d invalides ecartees"
        % (len(brut), " / ".join("%s=%d" % (k, len(v)) for k, v in res.items()),
           nb_trous, nb_invalides))
    return res


def charger_surfaces(fenetres):
    """Eau et espaces verts (toulouse10_surfaces.json) par fenetre."""
    with open(SURFACES_PATH, encoding="utf-8") as f:
        data = json.load(f)
    out = {}
    for cle in ("water", "green"):
        res = {k: [] for k in fenetres}
        for s in data.get(cle, []):
            pts = s.get("pts") or []
            if len(pts) < 3:
                continue
            bb = bbox_of(pts)
            for k, w in fenetres.items():
                if bbox_hit(bb, *w):
                    g = valide(Polygon(pts))
                    if not g.is_empty:
                        res[k].append(g)
        out[cle] = res
        log("surfaces %s : %s" % (cle, " / ".join("%s=%d" % (k, len(v)) for k, v in res.items())))
    return out["water"], out["green"]


def charger_bati(fenetres):
    with open(BATI_PATH, encoding="utf-8") as f:
        data = json.load(f)
    res = {k: [] for k in fenetres}
    for b in data.get("buildings", []):
        pts = b.get("pts") or []
        if len(pts) < 3:
            continue
        bb = bbox_of(pts)
        for k, w in fenetres.items():
            if bbox_hit(bb, *w):
                g = valide(Polygon(pts))
                if not g.is_empty:
                    res[k].append(g)
    log("batiments : %s" % " / ".join("%s=%d" % (k, len(v)) for k, v in res.items()))
    return res


def largeur_de(tr):
    """(largeur_m, source) ou source vaut 'mesure' ou 'repli'."""
    l = as_float(tr.get("largeur_de_chaussee"))
    if l is not None and l > 0.5:
        return l, "mesure"
    nat = norm(tr.get("nature"))
    if nat in NATURES_ETROITES:
        return FALLBACK_CHEMIN, "repli"
    imp = as_int(tr.get("importance"))
    voies = as_int(tr.get("nombre_de_voies"))
    if voies is not None and voies >= 2:
        return max(FALLBACK_LARGEUR.get(imp, FALLBACK_DEFAUT), 3.0 * voies), "repli"
    return FALLBACK_LARGEUR.get(imp, FALLBACK_DEFAUT), "repli"


def charger_routes(fenetres):
    """Axes routiers "auto" par fenetre + comptes de tri (le verrou du rapport)."""
    with open(ROUTES_PATH, encoding="utf-8") as f:
        data = json.load(f)
    brut = data.get("troncons", [])
    res = {k: [] for k in fenetres}
    stats = {k: {"n": 0, "mesure": 0, "repli": 0, "natures": {}, "long_m": 0.0}
             for k in fenetres}
    rejets = {k: {"nature": 0, "souterrain": 0, "fictif": 0, "hors service": 0,
                  "acces": 0} for k in fenetres}
    for tr in brut:
        pts = tr.get("pts") or []
        if len(pts) < 2:
            continue
        bb = bbox_of(pts)
        cibles = [k for k, w in fenetres.items() if bbox_hit(bb, *w)]
        if not cibles:
            continue
        nat = norm(tr.get("nature"))
        motif = None
        if nat in NATURES_EXCLUES:
            motif = "nature"
        elif norm(tr.get("acces_vehicule_leger")) == "physiquement impossible":
            motif = "acces"
        elif tr.get("fictif") is True:
            motif = "fictif"
        elif tr.get("etat_de_l_objet") and norm(tr.get("etat_de_l_objet")) != "en service":
            motif = "hors service"
        elif (as_int(tr.get("position_par_rapport_au_sol"), 0) or 0) < 0:
            motif = "souterrain"
        if motif:
            for k in cibles:
                rejets[k][motif] += 1
            continue
        largeur, source = largeur_de(tr)
        ligne = LineString(pts)
        for k in cibles:
            res[k].append((ligne, largeur))
            st = stats[k]
            st["n"] += 1
            st[source] += 1
            st["long_m"] += ligne.length
            st["natures"][nat] = st["natures"].get(nat, 0) + 1
    for k in fenetres:
        log("routes %s : %d axes retenus (%d mesures, %d replis), rejets %s"
            % (k, stats[k]["n"], stats[k]["mesure"], stats[k]["repli"],
               ", ".join("%s=%d" % (m, c) for m, c in rejets[k].items() if c)))
    return res, stats, rejets


# ------------------------------------------------------------------ geometrie

def buffer_axes(axes):
    """Bandes de chaussee : bout PLAT (pas de rotule au bout d'un troncon) et
    raccord MITRE (les angles de rue restent des angles, pas des chanfreins)."""
    bandes = []
    for ligne, largeur in axes:
        if largeur <= 0:
            continue
        bandes.append(ligne.buffer(largeur / 2.0, cap_style=2, join_style=2,
                                   mitre_limit=3.0))
    if not bandes:
        return None
    return valide(unary_union(bandes))


def corridor_de(zone_poly, parcelles, eaux, axes):
    """corridor / chaussee / trottoirs + mesures brutes."""
    m = {}
    u_parc = unary_union(parcelles) if parcelles else None
    u_eau = unary_union(eaux) if eaux else None
    m["aire_zone"] = zone_poly.area
    m["aire_parcelles"] = zone_poly.intersection(u_parc).area if u_parc is not None else 0.0
    m["aire_eau"] = zone_poly.intersection(u_eau).area if u_eau is not None else 0.0
    corridor = zone_poly
    if u_parc is not None:
        corridor = corridor.difference(u_parc)
    if u_eau is not None:
        corridor = corridor.difference(u_eau)
    corridor = valide(corridor)
    bande = buffer_axes(axes)
    privee = zone_poly.difference(zone_poly)  # polygone vide
    if bande is None:
        chaussee = zone_poly.difference(zone_poly)
        m["aire_bande_brute"] = 0.0
    else:
        m["aire_bande_brute"] = zone_poly.intersection(bande).area
        chaussee = valide(corridor.intersection(bande))
        # Ce qui reste de la bande hors corridor = voirie PRIVEE (allee de
        # lotissement, cour, parking de residence) : le cadastre la possede.
        privee = valide(zone_poly.intersection(bande).difference(corridor))
    m["aire_chaussee_privee"] = privee.area
    trottoirs = valide(corridor.difference(chaussee)) if not chaussee.is_empty else corridor
    m["aire_corridor"] = corridor.area
    m["aire_chaussee"] = chaussee.area
    m["aire_trottoirs"] = trottoirs.area
    # Longueur d'axes REELLEMENT dans la zone (la liste inclut la marge de
    # chargement : la compter entiere gonflerait les largeurs moyennes).
    lg = 0.0
    lg_pond = 0.0
    for ligne, largeur in axes:
        d = ligne.intersection(zone_poly).length
        lg += d
        lg_pond += d * largeur
    m["long_axes_zone"] = lg
    m["largeur_moy_ponderee"] = (lg_pond / lg) if lg > 0 else 0.0
    # Diagnostic cle : l'AXE lui-meme tombe-t-il dans une parcelle ? Si oui, ce
    # n'est pas "la chaussee deborde le domaine public", c'est un desaccord de
    # calage entre le cadastre et les axes BD TOPO.
    hors = 0.0
    for ligne, largeur in axes:
        seg = ligne.intersection(zone_poly)
        if not seg.is_empty:
            hors += seg.difference(corridor).length
    m["long_axe_hors_corridor"] = hors
    parts = polys_of(corridor)
    m["n_corridor"] = len(parts)
    m["n_trous_corridor"] = sum(len(p.interiors) for p in parts)
    m["aire_max_corridor"] = max((p.area for p in parts), default=0.0)
    m["n_slivers"] = sum(1 for p in parts if p.area < 5.0)
    m["aire_slivers"] = sum(p.area for p in parts if p.area < 5.0)
    return corridor, chaussee, trottoirs, privee, m


# ----------------------------------------------------------------- rendu PNG

def make_tx(x0, y0):
    """Local (m) -> pixel. y local croit vers le SUD (NORD = -Y) donc le NORD
    est en haut sans inversion : la ligne d'ecran croit avec y."""
    def tx(p):
        return ((p[0] - x0) / PX_M, (p[1] - y0) / PX_M)
    return tx


def masque(geom, taille, tx):
    m = Image.new("L", taille, 0)
    d = ImageDraw.Draw(m)
    for poly in polys_of(geom):
        d.polygon([tx(c) for c in poly.exterior.coords], fill=255)
        for r in poly.interiors:
            d.polygon([tx(c) for c in r.coords], fill=0)
    return m


def peindre(img, geom, couleur, tx):
    if geom is None or geom.is_empty:
        return
    img.paste(couleur, (0, 0), masque(geom, img.size, tx))


def peindre_translucide(img, geoms, couleur, alpha, tx):
    if not geoms:
        return
    g = unary_union(geoms)
    if g.is_empty:
        return
    calque = Image.new("RGB", img.size, couleur)
    img.paste(Image.blend(img, calque, alpha), (0, 0), masque(g, img.size, tx))


def police(taille):
    for nom in ("arial.ttf", "segoeui.ttf", "tahoma.ttf"):
        try:
            return ImageFont.truetype(os.path.join("C:\\Windows\\Fonts", nom), taille)
        except Exception:
            continue
    return ImageFont.load_default()


def dessiner_carte(chemin, titre, zone, parcelles, eaux, verts, batis,
                   corridor, chaussee, trottoirs, privee, mesures):
    x0, y0, x1, y1 = zone
    n = int(round((x1 - x0) / PX_M))
    tx = make_tx(x0, y0)
    img = Image.new("RGB", (n, n), COL_FOND)
    d = ImageDraw.Draw(img)

    if parcelles:
        peindre(img, unary_union(parcelles), COL_PARCELLE, tx)
        for p in parcelles:
            # Une parcelle rognee par la zone peut devenir un MultiPolygon.
            for sp in polys_of(p):
                for anneau in [sp.exterior] + list(sp.interiors):
                    pts = [tx(c) for c in anneau.coords]
                    if len(pts) >= 2:
                        d.line(pts, fill=COL_PARCELLE_BORD, width=1)
    peindre(img, privee, COL_PRIVEE, tx)
    peindre(img, corridor, COL_CORRIDOR, tx)
    peindre(img, trottoirs, COL_TROTTOIR, tx)
    peindre(img, chaussee, COL_CHAUSSEE, tx)
    if eaux:
        peindre(img, unary_union(eaux), COL_EAU, tx)
    peindre_translucide(img, verts, COL_VERT, 0.55, tx)
    if batis:
        peindre(img, unary_union(batis), COL_BATI, tx)

    f_titre = police(46)
    f_txt = police(30)
    d.rectangle([0, 0, n - 1, 78], fill=(255, 255, 255))
    d.text((22, 20), titre, fill=COL_TEXTE, font=f_titre)
    d.line([(0, 78), (n, 78)], fill=(180, 180, 180), width=2)

    # Legende + echelle, coin bas gauche.
    entrees = [
        (COL_CHAUSSEE, "chaussee (axes BD TOPO x largeur mesuree)"),
        (COL_PRIVEE, "voirie privee (bande routiere dans une parcelle)"),
        (COL_TROTTOIR, "trottoirs / places (corridor - chaussee)"),
        (COL_CORRIDOR, "corridor public brut (non cadastre)"),
        (COL_PARCELLE, "parcelles cadastrales (prive + public cadastre)"),
        (COL_BATI, "emprises batiments"),
        (COL_EAU, "eau"),
        (COL_VERT, "espaces verts (calque translucide)"),
    ]
    lh = 46
    bh = lh * len(entrees) + 150
    bw = 900
    bx, by = 30, n - bh - 30
    d.rectangle([bx, by, bx + bw, by + bh], fill=(255, 255, 255), outline=(120, 120, 120), width=2)
    yy = by + 18
    for coul, txt in entrees:
        d.rectangle([bx + 20, yy + 6, bx + 60, yy + 34], fill=coul, outline=(110, 110, 110))
        d.text((bx + 76, yy + 6), txt, fill=COL_TEXTE, font=f_txt)
        yy += lh
    # Echelle : 100 m.
    ech = int(round(100.0 / PX_M))
    ey = yy + 26
    d.line([(bx + 20, ey), (bx + 20 + ech, ey)], fill=COL_TEXTE, width=5)
    for xx in (bx + 20, bx + 20 + ech):
        d.line([(xx, ey - 12), (xx, ey + 12)], fill=COL_TEXTE, width=5)
    d.text((bx + 20 + ech + 18, ey - 18), "100 m  (25 cm/px)", fill=COL_TEXTE, font=f_txt)
    pc = 100.0 * mesures["aire_corridor"] / mesures["aire_zone"]
    pch = 100.0 * mesures["aire_chaussee"] / max(mesures["aire_corridor"], 1e-9)
    d.text((bx + 20, ey + 30),
           "corridor = %.1f %% de la zone ; chaussee = %.1f %% du corridor" % (pc, pch),
           fill=COL_TEXTE, font=f_txt)

    # Reperes de coordonnees locales aux coins.
    d.text((n - 620, 92), "X %.0f .. %.0f m" % (x0, x1), fill=COL_TEXTE, font=f_txt)
    d.text((n - 620, 128), "Y %.0f .. %.0f m (NORD en haut)" % (y0, y1), fill=COL_TEXTE, font=f_txt)

    os.makedirs(os.path.dirname(chemin), exist_ok=True)
    img.save(chemin, "PNG", optimize=True)
    return chemin


# ------------------------------------------------------------------ self-test

def selftest():
    """Cellule synthetique : 2 parcelles + 1 axe, aires attendues a la main."""
    ok = True
    zone = box(0, 0, 100, 100)
    p1 = Polygon([(10, 10), (45, 10), (45, 90), (10, 90)])   # 35 x 80 = 2800
    p2 = Polygon([(55, 10), (90, 10), (90, 90), (55, 90)])   # 35 x 80 = 2800
    axe = LineString([(50, 0), (50, 100)])
    corridor, chaussee, trottoirs, privee, m = corridor_de(zone, [p1, p2], [], [(axe, 10.0)])
    attendus = [("aire_zone", 10000.0), ("aire_parcelles", 5600.0),
                ("aire_corridor", 4400.0), ("aire_chaussee", 1000.0),
                ("aire_trottoirs", 3400.0)]
    for cle, att in attendus:
        got = m[cle]
        bon = abs(got - att) < 1e-6
        ok = ok and bon
        log("selftest %-16s attendu %9.2f  obtenu %9.2f  %s"
            % (cle, att, got, "OK" if bon else "ECHEC"))

    # Trou de parcelle (enclave non cadastree) : il DOIT revenir au corridor.
    p3 = Polygon([(10, 10), (90, 10), (90, 90), (10, 90)],
                 [[(40, 40), (60, 40), (60, 60), (40, 60)]])  # 6400 - 400
    c2, ch2, tr2, pv2, m2 = corridor_de(zone, [p3], [], [])
    bon = abs(m2["aire_corridor"] - (10000.0 - 6000.0)) < 1e-6
    ok = ok and bon
    log("selftest %-16s attendu %9.2f  obtenu %9.2f  %s"
        % ("trou -> corridor", 4000.0, m2["aire_corridor"], "OK" if bon else "ECHEC"))

    # Eau : elle sort du corridor.
    eau = Polygon([(0, 0), (100, 0), (100, 10), (0, 10)])
    c3, ch3, tr3, pv3, m3 = corridor_de(zone, [p1, p2], [eau], [])
    bon = abs(m3["aire_corridor"] - (4400.0 - 1000.0)) < 1e-6
    ok = ok and bon
    log("selftest %-16s attendu %9.2f  obtenu %9.2f  %s"
        % ("eau retiree", 3400.0, m3["aire_corridor"], "OK" if bon else "ECHEC"))

    # Rasterisation : un carre de 50 m -> 200 x 200 px pleins a 25 cm/px.
    tx = make_tx(0, 0)
    mk = masque(box(10, 10, 60, 60), (400, 400), tx)
    plein = sum(mk.histogram()[128:])
    bon = abs(plein - 40000) <= 800
    ok = ok and bon
    log("selftest %-16s attendu %9d  obtenu %9d  %s"
        % ("masque 50 m", 40000, plein, "OK" if bon else "ECHEC"))

    # Repli de largeur : mesure prioritaire, repli par importance sinon.
    cas = [({"largeur_de_chaussee": 6.5, "importance": "5"}, 6.5, "mesure"),
           ({"largeur_de_chaussee": None, "importance": "5"}, 4.5, "repli"),
           ({"importance": "3", "nombre_de_voies": 4}, 12.0, "repli"),
           ({"nature": "Chemin"}, FALLBACK_CHEMIN, "repli")]
    for tr, attl, atts in cas:
        gl, gs = largeur_de(tr)
        bon = abs(gl - attl) < 1e-9 and gs == atts
        ok = ok and bon
        log("selftest largeur %-22s attendu %5.2f/%s obtenu %5.2f/%s  %s"
            % (str(tr.get("nature") or tr.get("importance")), attl, atts, gl, gs,
               "OK" if bon else "ECHEC"))
    log("SELFTEST : " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------------- passe

def rapport_zone(nom, m, st, rej, duree):
    z = m["aire_zone"]
    lignes = [
        "--- ZONE %s ---" % nom,
        "  aire zone         : %.0f m2" % z,
        "  parcelles         : %.0f m2 (%.1f %%)" % (m["aire_parcelles"], 100.0 * m["aire_parcelles"] / z),
        "  eau               : %.0f m2 (%.1f %%)" % (m["aire_eau"], 100.0 * m["aire_eau"] / z),
        "  CORRIDOR public   : %.0f m2 (%.1f %% de la zone), %d morceaux, %d trous"
        % (m["aire_corridor"], 100.0 * m["aire_corridor"] / z, m["n_corridor"], m["n_trous_corridor"]),
        "  plus gros morceau : %.0f m2 (%.1f %% du corridor)"
        % (m["aire_max_corridor"], 100.0 * m["aire_max_corridor"] / max(m["aire_corridor"], 1e-9)),
        "  chaussee          : %.0f m2 (%.1f %% du corridor, %.1f %% de la zone)"
        % (m["aire_chaussee"], 100.0 * m["aire_chaussee"] / max(m["aire_corridor"], 1e-9),
           100.0 * m["aire_chaussee"] / z),
        "  trottoirs/places  : %.0f m2 (%.1f %% du corridor)"
        % (m["aire_trottoirs"], 100.0 * m["aire_trottoirs"] / max(m["aire_corridor"], 1e-9)),
        "  bande brute avant clip : %.0f m2 -> %.1f %% rogne par parcelles/eau"
        % (m["aire_bande_brute"],
           100.0 * (m["aire_bande_brute"] - m["aire_chaussee"]) / max(m["aire_bande_brute"], 1e-9)),
        "  voirie privee     : %.0f m2 (bande routiere tombee dans des parcelles)"
        % m["aire_chaussee_privee"],
        "  axes routiers     : %d objets, %.0f m dans la zone, largeur mesuree %d (%.1f %%), repli %d (%.1f %%)"
        % (st["n"], m["long_axes_zone"], st["mesure"], 100.0 * st["mesure"] / max(st["n"], 1),
           st["repli"], 100.0 * st["repli"] / max(st["n"], 1)),
        "  largeur moyenne   : %.2f m (ponderee par la longueur) ; corridor moyen %.2f m par metre d'axe"
        % (m["largeur_moy_ponderee"], m["aire_corridor"] / max(m["long_axes_zone"], 1e-9)),
        "  natures gardees   : " + ", ".join("%s=%d" % (k, v) for k, v in
                                             sorted(st["natures"].items(), key=lambda kv: -kv[1])),
        "  axes ecartes      : " + (", ".join("%s=%d" % (k, v) for k, v in rej.items() if v) or "aucun"),
        "  axe hors corridor : %.0f m (%.1f %% de la longueur) -> desaccord cadastre/axes"
        % (m["long_axe_hors_corridor"],
           100.0 * m["long_axe_hors_corridor"] / max(m["long_axes_zone"], 1e-9)),
        "  anomalies         : %d morceaux < 5 m2 (%.1f m2 cumules)" % (m["n_slivers"], m["aire_slivers"]),
        "  duree zone        : %.1f s" % duree,
    ]
    for l in lignes:
        log(l)
    return lignes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--zone", default="", help="centre | peripherie (defaut : les deux)")
    ap.add_argument("--no-png", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    noms = [args.zone] if args.zone else list(ZONES.keys())
    for nm in noms:
        if nm not in ZONES:
            log("zone inconnue : %s" % nm)
            return 2
    t0 = time.time()
    fenetres = {}
    for nm in noms:
        x0, y0, x1, y1 = ZONES[nm][:4]
        fenetres[nm] = (x0 - MARGE_M, y0 - MARGE_M, x1 + MARGE_M, y1 + MARGE_M)

    log("=== SOLS PAR LE CADASTRE : zones %s ===" % ", ".join(noms))
    parcelles = charger_parcelles(fenetres)
    eaux, verts = charger_surfaces(fenetres)
    axes, stats, rejets = charger_routes(fenetres)
    batis = charger_bati(fenetres)
    log("chargements : %.1f s" % (time.time() - t0))

    sorties = []
    for nm in noms:
        tz = time.time()
        x0, y0, x1, y1, libelle = ZONES[nm]
        zone_poly = box(x0, y0, x1, y1)
        corridor, chaussee, trottoirs, privee, m = corridor_de(
            zone_poly, parcelles[nm], eaux[nm], axes[nm])
        rapport_zone(nm, m, stats[nm], rejets[nm], time.time() - tz)
        if not args.no_png:
            tp = time.time()
            chemin = os.path.join(SAVED, "sols_corridor_%s.png" % nm)
            titre = ("SOLS PAR LE CADASTRE - %s - 1 km x 1 km - corridor public = zone - parcelles - eau"
                     % libelle)
            dessiner_carte(chemin, titre, (x0, y0, x1, y1),
                           [p.intersection(zone_poly) for p in parcelles[nm]
                            if p.intersects(zone_poly)],
                           [e.intersection(zone_poly) for e in eaux[nm] if e.intersects(zone_poly)],
                           [v.intersection(zone_poly) for v in verts[nm] if v.intersects(zone_poly)],
                           [b.intersection(zone_poly) for b in batis[nm] if b.intersects(zone_poly)],
                           corridor, chaussee, trottoirs, privee, m)
            log("carte %s : %s (%.1f Mo, rendu %.1f s)"
                % (nm, chemin, os.path.getsize(chemin) / 1048576.0, time.time() - tp))
            sorties.append(chemin)
    log("TOTAL : %.1f s" % (time.time() - t0))
    for s in sorties:
        log("sortie : " + s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
