# J3b-bis - ROGNAGE DES EMPRISES SUR LE CADASTRE.
#
# Constat : les emprises batiments (BD TOPO / bati_enrichi) et les parcelles
# cadastrales (PARCELLAIRE_EXPRESS) sont deux couches IGN qui se recouvrent
# legerement. Resultat en jeu : des batiments poses SUR le trottoir ou sur un
# passage pieton, parce que le corridor public se calcule par soustraction des
# parcelles (j3c_sols_corridor.py) et qu'un batiment qui deborde de sa parcelle
# deborde donc mecaniquement sur le domaine public.
#
# Correction, pilotee par la donnee et jamais par la devinette :
#
#   parcelles_fermees = FERMETURE MORPHOLOGIQUE de union(parcelles touchees) :
#                       union.buffer(+d).buffer(-d), d = FERMETURE.
#   emprise_corrigee  = emprise MOINS les morceaux MINCES de (emprise - parcelles_fermees)
#
# PASSE 2 (deux corrections mesurees, cf. rapport) :
#
#   A. VRAIE fermeture morphologique. La passe 1 faisait buffer(+0,20).buffer(-0,0)
#      -- une DILATATION pure : toutes les parcelles etaient grossies de 20 cm, y
#      compris du cote de la rue, ce qui laissait 2 477 m2 de bati sur le domaine
#      public (le residu ETAIT la tolerance). La fermeture rebouche les interstices
#      plus etroits que 2d SANS repousser la limite exterieure vers la rue.
#
#   B. GARDE-FOU D'EPAISSEUR sur le morceau retire. Notre corridor suppose que tout
#      ce qui n'est pas parcelle est public : vrai en centre-ville, FAUX en zone
#      industrielle ou PARCELLAIRE EXPRESS a des manques. La phenomenologie tranche :
#      un vrai debord sur trottoir est MINCE (20 cm a 1,4 m), une parcelle manquante
#      est EPAISSE (coins de 14 m, de 83 m). Un morceau n'est donc retire que si
#      morceau.buffer(-EPAISSEUR) est vide (epaisseur < 2 x EPAISSEUR) ; les morceaux
#      EPAIS sont RENDUS au batiment.
#
# Garde-fous (aucun batiment ne disparait ni ne se coupe) :
#   1. aucune parcelle sous le batiment           -> INTACT (non cadastre : halle,
#      kiosque, equipement public)
#   2. moins de 50 % de l'aire dans ses parcelles -> INTACT (meme raison)
#   3. resultat vide                              -> INTACT + journalise
#   4. resultat en plusieurs morceaux             -> la plus grande part + journalise
#
# Toits : le bloc "roof" (squelette droit, j3b_prep_toits.py) a ete calcule sur
# l'emprise ORIGINALE. Toute emprise MODIFIEE voit donc son squelette RECALCULE
# (eave / delta / mat conserves, seuls "sv" et "f" sont refaits) ; les emprises
# inchangees gardent leur toit tel quel. Si le squelette echoue apres rognage,
# retombee en toit plat (roof retire) - comportement deja prevu en aval.
#
# Les champs h / u / tint sont preserves a l'identique (tint = teinte reelle du
# toit echantillonnee sur l'ortho, sa reconstruction couterait cher).
#
# Sorties : sauvegarde SourceData/toulouse10_bati.avant_rognage.json puis
# reecriture EN PLACE de SourceData/toulouse10_bati.json (les outils en aval
# attendent ce nom).
#
# Python OBLIGATOIRE : celui de Blender 5.2 (wheels cp313 de Tools/pylib).
#   "C:\Program Files\Blender Foundation\Blender 5.2\5.2\python\bin\python.exe"
#
# Usage :
#   python j3b_rogne_bati.py --selftest       # verrou geometrique synthetique
#   python j3b_rogne_bati.py --dry-run        # mesures seules, rien d'ecrit
#   python j3b_rogne_bati.py                  # passe complete + ecriture
#   python j3b_rogne_bati.py --fenetre -500,-500,500,500   # sous-ensemble
import argparse
import json
import math
import os
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "pylib"))

from shapely.geometry import Polygon, box  # noqa: E402
from shapely.ops import unary_union  # noqa: E402
from shapely.strtree import STRtree  # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

SRC = os.path.join(os.path.dirname(HERE), "SourceData")
BATI_PATH = os.path.join(SRC, "toulouse10_bati.json")
BACKUP_PATH = os.path.join(SRC, "toulouse10_bati.avant_rognage.json")
PARCELLES_PATH = os.path.join(SRC, "GrandFetch", "parcelles.json")
LOG_PATH = os.path.join(SRC, "toulouse10_rognage.progress.log")

AIRE_MIN_TOUCHE = 0.05   # m2 : en dessous, la parcelle ne "porte" pas le batiment
FERMETURE = 0.40         # m : rayon de la FERMETURE morphologique (joints cadastraux)
EPAISSEUR = 1.00         # m : un morceau retire doit etre MINCE (epaisseur < 2 m)
FRAC_MIN = 0.50          # sous ce taux d'appartenance, batiment declare non cadastre
SIMPLIFY = 0.01          # m : nettoie les sommets quasi colineaires nes de l'intersection
AIRE_MIN_MORCEAU = 1e-6  # m2 : bruit numerique de la difference


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


# --------------------------------------------------------------- j3b_prep_toits
# On REUTILISE l'outil de toits en bibliotheque : l'algorithme de squelette droit
# (bpypolyskel vendorise) ne doit exister qu'a un seul endroit.

def charger_prep_toits():
    path = os.path.join(HERE, "j3b_prep_toits.py")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    ns = {"__name__": "j3b_prep_toits_lib", "__file__": path}
    exec(compile(src, path, "exec"), ns)  # noqa: S102
    return ns


PREP = None


def clean_ring(pts):
    return PREP["clean_ring"](pts)


def skeleton_faces(ring):
    return PREP["skeleton_faces"](ring)


# -------------------------------------------------------------------- geometrie

def valide(g):
    if g.is_valid:
        return g
    b = g.buffer(0)
    return b if b.is_valid else g


def polys_of(g):
    if g.is_empty:
        return []
    t = g.geom_type
    if t == "Polygon":
        return [g]
    if t in ("MultiPolygon", "GeometryCollection"):
        out = []
        for s in g.geoms:
            out.extend(polys_of(s))
        return out
    return []


def bbox_of(pts):
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def poly_of_pts(pts):
    if len(pts) < 3:
        return None
    try:
        g = valide(Polygon(pts))
    except Exception:
        return None
    if g.is_empty or g.area <= 0:
        return None
    return g


def charger_parcelles(fenetre=None):
    with open(PARCELLES_PATH, encoding="utf-8") as f:
        data = json.load(f)
    brut = data.get("parcelles", [])
    out = []
    n_inv = 0
    for p in brut:
        rings = p.get("rings") or []
        if not rings or len(rings[0]) < 3:
            continue
        if fenetre is not None:
            bb = bbox_of(rings[0])
            if bb[2] < fenetre[0] or bb[0] > fenetre[2] or bb[3] < fenetre[1] or bb[1] > fenetre[3]:
                continue
        holes = [r for r in rings[1:] if len(r) >= 3]
        try:
            g = Polygon(rings[0], holes)
        except Exception:
            n_inv += 1
            continue
        g = valide(g)
        if not g.is_valid or g.is_empty:
            n_inv += 1
            continue
        out.append(g)
    log("parcelles : %d lues, %d retenues, %d invalides ecartees" % (len(brut), len(out), n_inv))
    return out


def fermeture_morpho(u, d):
    """union.buffer(+d).buffer(-d) : rebouche les interstices < 2d SANS repousser
    la limite exterieure. Une fermeture est par construction un SUR-ENSEMBLE de
    l'union ; on le garantit malgre le bruit numerique en reunissant les deux."""
    if d <= 0:
        return u
    try:
        c = valide(valide(u.buffer(d)).buffer(-d))
    except Exception:
        return u
    if c.is_empty:
        return u
    try:
        return valide(unary_union([u, c]))
    except Exception:
        return u


def rogne_un(bpoly, parcelles, tree, stats=None):
    """(nouveau_polygone_ou_None, motif). None = laisser l'emprise intacte."""
    idx = tree.query(bpoly)
    touchees = []
    for i in idx:
        p = parcelles[int(i)]
        if not p.intersects(bpoly):
            continue
        try:
            a = p.intersection(bpoly).area
        except Exception:
            continue
        if a > AIRE_MIN_TOUCHE:
            touchees.append(p)
    if not touchees:
        return None, "hors cadastre -> intact"
    u = fermeture_morpho(valide(unary_union(touchees)), FERMETURE)
    try:
        r = valide(bpoly.intersection(u))
    except Exception:
        return None, "intersection impossible -> intact"
    if r.is_empty or r.area <= 0:
        return None, "ALERTE resultat vide -> intact"
    # Garde-fou 2 : evalue sur l'appartenance BRUTE aux parcelles (semantique de
    # la passe 1), avant toute restitution de morceau epais.
    if bpoly.area > 0 and r.area / bpoly.area < FRAC_MIN:
        return None, "<50 %% dans ses parcelles -> intact"

    # ---- Correction B : le morceau retire doit etre MINCE, sinon on le REND.
    rendus = []
    try:
        reste = valide(bpoly.difference(u))
    except Exception:
        reste = None
    if reste is not None:
        for m in polys_of(reste):
            if m.area <= AIRE_MIN_MORCEAU:
                continue
            try:
                mince = m.buffer(-EPAISSEUR).is_empty
            except Exception:
                mince = False
            if mince:
                if stats is not None:
                    stats["morceau retire (mince)"] += 1
                    stats["_aire_retiree"] += m.area
            else:
                rendus.append(m)
                if stats is not None:
                    stats["morceau RENDU (epais)"] += 1
                    stats["_aire_rendue"] += m.area
    if rendus:
        try:
            r = valide(unary_union([r] + rendus))
        except Exception:
            pass

    parts = polys_of(r)
    if not parts:
        return None, "ALERTE resultat non surfacique -> intact"
    motif = None
    if len(parts) > 1:
        parts.sort(key=lambda g: -g.area)
        motif = "ALERTE decoupe en %d morceaux -> plus grande part" % len(parts)
    g = parts[0]
    if g.interiors:
        motif = (motif or "") + " | trou ignore (schema anneau simple)"
    return g, motif


def ring_de(g, simplify=None):
    """Anneau exterieur nettoye (CCW, 2 dec) pret pour le schema pts / le squelette."""
    gs = g.simplify(SIMPLIFY if simplify is None else simplify, preserve_topology=True)
    gs = valide(gs)
    parts = polys_of(gs)
    if not parts:
        parts = polys_of(g)
    if not parts:
        return None
    parts.sort(key=lambda p: -p.area)
    coords = [(round(x, 2), round(y, 2)) for x, y in parts[0].exterior.coords]
    return clean_ring(coords)


# -------------------------------------------------------------------- self-test

def selftest():
    ok = True

    def check(nom, cond, detail=""):
        nonlocal ok
        ok = ok and cond
        print("  %-46s : %s %s" % (nom, "PASS" if cond else "FAIL", detail))

    # 1. Debord MINCE : batiment 10x10 dont 1 m depasse a l'est d'une parcelle.
    #    La fermeture ne repousse PAS la limite exterieure : on attend 90 m2 pile.
    parc = [Polygon([(0, 0), (9, 0), (9, 10), (0, 10)])]
    tree = STRtree(parc)
    b = Polygon([(0, 0), (10, 0), (10, 10), (0, 10)])
    g, m = rogne_un(b, parc, tree)
    check("debord mince rogne (pas de dilatation)",
          g is not None and abs(g.area - 90.0) < 0.05,
          "aire %.3f (attendu 90,000 pile)" % (g.area if g else -1))

    # 2. Aucune parcelle -> intact (garde-fou 1).
    parc2 = [Polygon([(100, 100), (110, 100), (110, 110), (100, 110)])]
    g2, m2 = rogne_un(b, parc2, STRtree(parc2))
    check("hors cadastre -> intact", g2 is None and "hors cadastre" in m2, m2)

    # 3. Moins de 50 % dedans -> intact (garde-fou 2).
    parc3 = [Polygon([(0, 0), (3, 0), (3, 10), (0, 10)])]
    g3, m3 = rogne_un(b, parc3, STRtree(parc3))
    check("<50 %% dedans -> intact", g3 is None and "50" in m3, m3)

    # 4. Deux parcelles separees par un joint de 10 cm : SANS fermeture le
    #    batiment se coupe en deux, AVEC il reste d'un seul tenant.
    pa = Polygon([(0, 0), (5, 0), (5, 10), (0, 10)])
    pb = Polygon([(5.1, 0), (10, 0), (10, 10), (5.1, 10)])
    global FERMETURE
    sauve = FERMETURE
    FERMETURE = 0.0
    n_sans = len(polys_of(valide(b.intersection(unary_union([pa, pb])))))
    FERMETURE = sauve
    g4, m4 = rogne_un(b, [pa, pb], STRtree([pa, pb]))
    check("joint cadastral : 2 morceaux sans fermeture",
          n_sans == 2, "%d morceaux" % n_sans)
    check("joint cadastral : 1 seul morceau avec fermeture",
          g4 is not None and m4 is None and abs(g4.area - 100.0) < 0.5,
          "aire %.2f, motif %s" % (g4.area if g4 else -1, m4))

    # 4bis. Correction A : la fermeture est un SUR-ENSEMBLE de l'union.
    uu = valide(unary_union([pa, pb]))
    cc = fermeture_morpho(uu, FERMETURE)
    check("fermeture = sur-ensemble de l'union",
          uu.difference(cc).area < 1e-9, "difference %.3e m2" % uu.difference(cc).area)
    check("fermeture ne repousse pas la limite exterieure",
          abs(cc.bounds[2] - uu.bounds[2]) < 1e-6 and abs(cc.bounds[0] - uu.bounds[0]) < 1e-6,
          "bornes x %.4f..%.4f (union %.4f..%.4f)"
          % (cc.bounds[0], cc.bounds[2], uu.bounds[0], uu.bounds[2]))

    # 4ter. Correction B : un coin EPAIS (5 x 5 m) hors parcelle est RENDU, un
    #       liktere MINCE (0,3 m) est retire.
    bg = Polygon([(0, 0), (20, 0), (20, 20), (0, 20)])
    pc = valide(bg.difference(Polygon([(15, 15), (20, 15), (20, 20), (15, 20)])))
    g5, m5 = rogne_un(bg, [pc], STRtree([pc]))
    check("coin epais 5 x 5 m RENDU (parcelle manquante)",
          g5 is not None and abs(g5.area - 400.0) < 0.05,
          "aire %.2f (attendu 400)" % (g5.area if g5 else -1))
    pd_ = Polygon([(0, 0), (19.7, 0), (19.7, 20), (0, 20)])
    g6, m6 = rogne_un(bg, [pd_], STRtree([pd_]))
    check("lisiere mince 0,3 m retiree",
          g6 is not None and abs(g6.area - 394.0) < 0.05,
          "aire %.2f (attendu 394)" % (g6.area if g6 else -1))

    # 5. Anneau de sortie : CCW, ferme, exploitable par le squelette droit.
    ring = ring_de(g)
    check("anneau de sortie propre", ring is not None and len(ring) == 4,
          "%d sommets" % (len(ring) if ring else -1))
    try:
        v, faces = skeleton_faces(ring)
        check("squelette recalculable sur l'anneau rogne", len(faces) == 4,
              "%d faces" % len(faces))
    except Exception as e:
        check("squelette recalculable sur l'anneau rogne", False, str(e))

    print("SELFTEST " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ------------------------------------------------------------------------ passe

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--fenetre", default="", help="x0,y0,x1,y1 (sous-ensemble de test)")
    ap.add_argument("--fermeture", type=float, default=None, help="rayon de fermeture d (m)")
    ap.add_argument("--epaisseur", type=float, default=None, help="demi-epaisseur mince e (m)")
    args = ap.parse_args()

    global PREP, FERMETURE, EPAISSEUR
    if args.fermeture is not None:
        FERMETURE = args.fermeture
    if args.epaisseur is not None:
        EPAISSEUR = args.epaisseur
    PREP = charger_prep_toits()
    if args.selftest:
        return selftest()

    fen = None
    if args.fenetre:
        fen = tuple(float(v) for v in args.fenetre.split(","))

    t0 = time.time()
    log("=== ROGNAGE DES EMPRISES SUR LE CADASTRE "
        "(fermeture d=%.2f m, epaisseur e=%.2f m) ===" % (FERMETURE, EPAISSEUR))
    # ON PART TOUJOURS DE L'ETAT D'ORIGINE : toulouse10_bati.json peut deja porter
    # une passe de rognage, et rogner deux fois recalculerait des toits sur des toits.
    in_path = BACKUP_PATH if os.path.exists(BACKUP_PATH) else BATI_PATH
    with open(in_path, encoding="utf-8") as f:
        root = json.load(f)
    buildings = root["buildings"]
    log("batiments : %d (ENTREE = %s)" % (len(buildings), in_path))
    parcelles = charger_parcelles(None if fen is None else
                                  (fen[0] - 50, fen[1] - 50, fen[2] + 50, fen[3] + 50))
    tree = STRtree(parcelles)
    log("index spatial pret (%.1f s)" % (time.time() - t0))

    stats = Counter()
    alertes = []
    parts = []
    aire_av = aire_ap = 0.0
    pertes = []
    t1 = time.time()

    for i, b in enumerate(buildings):
        if i % 10000 == 0 and i > 0:
            log("  %d/%d (%.0f s, modifies %d, toits refaits %d)"
                % (i, len(buildings), time.time() - t1,
                   stats["modifie"], stats["toit recalcule"]))
        pts = b.get("pts") or []
        bpoly = poly_of_pts(pts)
        if bpoly is None:
            stats["anneau invalide -> intact"] += 1
            parts.append(json.dumps(b, separators=(",", ":")))
            continue
        if fen is not None:
            bb = bbox_of(pts)
            if bb[2] < fen[0] or bb[0] > fen[2] or bb[3] < fen[1] or bb[1] > fen[3]:
                parts.append(json.dumps(b, separators=(",", ":")))
                continue
        aire_av += bpoly.area

        g, motif = rogne_un(bpoly, parcelles, tree, stats)
        if g is None:
            stats[motif] += 1
            if motif.startswith("ALERTE"):
                alertes.append((i, motif, bpoly.area))
            aire_ap += bpoly.area
            parts.append(json.dumps(b, separators=(",", ":")))
            continue
        if motif:
            for m in motif.split(" | "):
                stats[m] += 1
            if motif.startswith("ALERTE"):
                alertes.append((i, motif, bpoly.area))

        ring = ring_de(g)
        if ring is None:
            stats["ALERTE anneau resultant vide -> intact"] += 1
            alertes.append((i, "anneau resultant vide", bpoly.area))
            aire_ap += bpoly.area
            parts.append(json.dumps(b, separators=(",", ":")))
            continue

        new_pts = [[p[0], p[1]] for p in ring]
        old_pts = [[round(float(p[0]), 2), round(float(p[1]), 2)] for p in pts]
        change = new_pts != old_pts
        gg = poly_of_pts(new_pts)
        aire_ap += gg.area if gg is not None else bpoly.area
        if not change:
            stats["inchange"] += 1
            parts.append(json.dumps(b, separators=(",", ":")))
            continue

        stats["modifie"] += 1
        perte = bpoly.area - (gg.area if gg is not None else bpoly.area)
        pertes.append((perte, i))

        nb = dict(b)
        nb["pts"] = new_pts
        if "roof" in b:
            old_roof = b["roof"]
            # Echelle de repli : si le squelette droit echoue sur l'anneau rogne,
            # on retente sur un anneau plus simplifie (les sommets quasi colineaires
            # nes de l'intersection sont la cause connue) AVANT de retomber a plat.
            # Le toit d'un batiment ne se sacrifie que si les trois essais echouent.
            err = None
            for simp in (None, 0.05, 0.15):
                try:
                    r_ring = ring if simp is None else ring_de(g, simp)
                    if r_ring is None or len(r_ring) < 3:
                        continue
                    v, faces = skeleton_faces(r_ring)
                except Exception as e:
                    err = e
                    continue
                nb["pts"] = [[p[0], p[1]] for p in r_ring]
                n = len(r_ring)
                nb["roof"] = {"eave": old_roof["eave"], "delta": old_roof["delta"],
                              "mat": old_roof["mat"], "sv": v[n:], "f": faces}
                stats["toit recalcule"] += 1
                if simp is not None:
                    stats["  toit sauve au repli simplify %.2f" % simp] += 1
                err = None
                break
            if err is not None:
                nb["pts"] = new_pts
                nb.pop("roof", None)
                stats["toit -> plat de secours"] += 1
                stats["  motif plat : " + str(err)[:40]] += 1
        parts.append(json.dumps(nb, separators=(",", ":")))

    dt = time.time() - t1
    log("passe terminee en %.0f s" % dt)
    log("aire batie : %.0f m2 -> %.0f m2  (%.3f %% rognes)"
        % (aire_av, aire_ap, 100.0 * (aire_av - aire_ap) / max(aire_av, 1e-9)))
    for k, v in stats.most_common():
        if k.startswith("_aire"):
            log("  %-46s : %.1f m2" % (k, v))
        else:
            log("  %-46s : %d" % (k, v))
    if pertes:
        pertes.sort(reverse=True)
        log("  perte max %.1f m2 (index %d), p99 %.1f m2, mediane %.2f m2"
            % (pertes[0][0], pertes[0][1],
               pertes[len(pertes) // 100][0], pertes[len(pertes) // 2][0]))
    for i, m, a in alertes[:40]:
        log("  ALERTE index %d (%.1f m2) : %s" % (i, a, m))
    if len(alertes) > 40:
        log("  ... %d alertes supplementaires" % (len(alertes) - 40))

    if args.dry_run:
        log("--dry-run : rien n'a ete ecrit")
        return 0

    if not os.path.exists(BACKUP_PATH):
        with open(BATI_PATH, "rb") as fi, open(BACKUP_PATH, "wb") as fo:
            fo.write(fi.read())
        log("sauvegarde ecrite : %s" % BACKUP_PATH)
    else:
        log("sauvegarde deja presente, conservee : %s" % BACKUP_PATH)

    out = ('{"source":"j3b_rogne_bati.py passe 2 (emprises rognees sur la FERMETURE '
           'morphologique d=%.2f m des parcelles, morceaux epais (e=%.2f m) rendus ; '
           'toits modifies recalcules) / %s",'
           '"origin":%s,"sizeM":%s,"buildings":[%s]}') % (
        FERMETURE, EPAISSEUR, str(root.get("source", "")).replace('"', "'"),
        json.dumps(root.get("origin"), separators=(",", ":")),
        json.dumps(root.get("sizeM"), separators=(",", ":")),
        ",".join(parts))
    tmp = BATI_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(out)
    os.replace(tmp, BATI_PATH)
    log("ecrit : %s (%.1f Mo, %d batiments)"
        % (BATI_PATH, os.path.getsize(BATI_PATH) / 1e6, len(parts)))
    log("TOTAL %.0f s" % (time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
