# build_reseau.py -- LE GRAPHE ROUTIER SIDE-CAR, cellule par cellule.
#
# POURQUOI : la geometrie generee est FUSIONNEE par cellule (1 SM_Slab + 1
# SM_Ground pour toutes les routes d'une cellule de 500 m). Il n'existe aucun
# acteur, aucun composant, aucune section de mesh par route : rien ou accrocher
# la semantique. Si elle n'est pas ecrite A COTE de la geometrie, au moment ou on
# la connait encore, elle ne se rattrape qu'en REGENERANT les 461 km2.
#
# CE SCRIPT EST DONC LA COUCHE 1 : il persiste, par cellule, le squelette du
# reseau (geometrie + connectivite + sens + voies + largeur + hierarchie +
# identifiants IGN perennes), lu directement dans le GPKG BD TOPO departemental
# (la source RICHE : 88 colonnes, contre 12 attributs ecrases par l'ancien
# convertisseur WFS). Aucune donnee n'est inventee, aucune n'est jetee.
#
#   Tools/reseau_classes.json   la table de classification — SEUL fichier a editer
#   SourceData/Reseau/reseau_<cx>_<cy>.json   un fichier par cellule
#   SourceData/Reseau/index_<emprise>.json    l'inventaire + les controles
#   Doc/Reseau-Sidecar.md       le format et ses conventions
#
# Usage :
#   python build_reseau.py --selftest
#   python build_reseau.py --emprise proto
#   python build_reseau.py --emprise carre10
#   python build_reseau.py --cells 0,0 0,1 -1,3
#   python build_reseau.py --emprise carre10 --valider-seulement
import argparse
import hashlib
import json
import math
import os
import sqlite3
import sys
import time
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_local as G

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "Tools")
OUT_DIR = os.path.join(ROOT, "SourceData", "Reseau")
CONF_PATH = os.path.join(TOOLS, "reseau_classes.json")
LOG_PATH = os.path.join(OUT_DIR, "reseau.progress.log")

GPKG = os.path.join(ROOT, "SourceData", "Agglo", "BDTOPO",
                    "BDTOPO_3-5_TOUSTHEMES_GPKG_LAMB93_D031_2026-06-15", "BDTOPO",
                    "1_DONNEES_LIVRAISON_2026-06-00418",
                    "BDT_3-5_GPKG_LAMB93_D031_ED2026-06-15",
                    "BDT_3-5_GPKG_LAMB93_D031-ED2026-06-15.gpkg")
MILLESIME = "BD TOPO 3.5 GPKG LAMB93 D031 ED2026-06-15 (IGN, Licence Ouverte 2.0)"

# Colonnes lues dans le GPKG. Ajouter ici + dans edge_from() pour enrichir.
COLS = ["cleabs", "nature", "importance", "nombre_de_voies", "largeur_de_chaussee",
        "sens_de_circulation", "vitesse_moyenne_vl", "acces_vehicule_leger",
        "acces_pieton", "reserve_aux_bus", "urbain", "prive", "fictif",
        "etat_de_l_objet", "position_par_rapport_au_sol",
        "amenagement_cyclable_gauche", "amenagement_cyclable_droit",
        "nom_voie_ban_gauche", "id_ban_odonyme_gauche", "cpx_numero",
        "cpx_classement_administratif"]

EPS_LEN = 0.01          # une arete plus courte que 1 cm est degeneree
EPS_BORD = 0.02         # tolerance « le point est sur la frontiere de cellule »
HEARTBEAT = [None]

# --- LOT A-bis (correctifs 1 et 3) : COHERENCE DE CLASSE LE LONG DES RUES --------
# Le patchwork au sol venait de la classification TRONCON PAR TRONCON : une meme
# rue changeait de revetement a chaque frontiere d'arete BD TOPO, en plein bloc.
# Regle de ville implantee ICI (dans la classification, pas dans la cuisson) :
# UNE RUE GARDE UNE CLASSE ENTRE DEUX CARREFOURS ; les changements de classe ne
# surviennent qu'aux noeuds de carrefour. Trois niveaux, appliques dans l'ordre :
#   1. RUN — chaine d'aretes reliees par des noeuds de degre 2 (continuite
#      geometrique) : vote majoritaire pondere par la longueur. Par construction,
#      plus AUCUNE bascule en pleine rue. C'est aussi l'hysteresis : un
#      micro-troncon ne renverse jamais le vote d'un bloc entier.
#   2. RUE NOMMEE (sandwich) — un run court d'une voie nommee, encadre a ses deux
#      bouts par des runs de la MEME voie et d'une MEME autre classe, l'adopte
#      (un boulevard ne change pas de revetement 90 m au milieu de sa longueur).
#   3. ABSORPTION DES POCHES ENCLAVEES — une POCHE (groupe CONNEXE de runs d'une
#      meme classe C ; un run isole en est une) dont TOUS les voisins peints sont
#      d'une meme autre classe D, avec au moins deux points de contact, est
#      absorbee par D si elle est courte, ou si deux de ses contacts debouchent
#      dans la meme zone D (plus court chemin en aretes D <= ZONE_BFS_MAX_M : le
#      fragment traverse une place), ou — cas de l'ILOT INJOIGNABLE — si D est la
#      classe pietonne : une voirie circulee entierement cernee par du pieton est
#      injoignable en voiture, la classification se contredit elle-meme.
# Seules les classes de REVETEMENT DE VOIRIE votent et peuvent changer ; les
# classes de NATURE sont verrouillees (un escalier reste un escalier, un chemin
# de gravier reste du gravier, l'autoroutier reste autoroutier).
LISSAGE_ON = True
CLASSES_VOTANTES = {"artere", "bus", "rue", "ruelle", "pietonne_pavee"}
SANDWICH_MAX_M = 200.0
ABSORB_MAX_M = 55.0
# Une pietonne_pavee vient d'une DONNEE reelle (acces VL impossible / OSM
# pedestrian) : on ne l'absorbe que tres courte (bruit de matching).
ABSORB_PAVEE_MAX_M = 20.0
ABSORB_ZONE_MAX_M = 150.0
ZONE_BFS_MAX_M = 250.0
# Ilot injoignable en voiture (absorbe VERS pietonne_pavee) : plafond de sante.
ABSORB_ILOT_MAX_M = 400.0


def lisser_classes(troncons):
    """Lissage de coherence sur la liste [(base, cleabs, lignes)] AVANT decoupe.
    Modifie base['classe_rendu'] EN PLACE ; l'ancienne classe est tracee dans
    base['classe_brute'] et le motif dans base['lissage']. Renvoie les compteurs."""
    # graphe : une arete par polyligne, noeuds = cles decimetriques des extremites
    edges = []
    inc = defaultdict(list)
    for ti, (base, cleabs, lignes) in enumerate(troncons):
        for pts in lignes:
            if len(pts) < 2:
                continue
            a = G.noeud_key(pts[0][0], pts[0][1])
            b = G.noeud_key(pts[-1][0], pts[-1][1])
            ei = len(edges)
            edges.append({"a": a, "b": b, "ti": ti, "L": longueur(pts)})
            inc[a].append(ei)
            inc[b].append(ei)
    deg = {k: len(v) for k, v in inc.items()}

    def classe(ei):
        return troncons[edges[ei]["ti"]][0]["classe_rendu"]

    def nom(ei):
        return norm(troncons[edges[ei]["ti"]][0].get("nom"))

    stats = Counter()

    def changer(eis, nouvelle, motif):
        for ei in eis:
            base = troncons[edges[ei]["ti"]][0]
            if base["classe_rendu"] == nouvelle:
                continue
            base.setdefault("classe_brute", base["classe_rendu"])
            base["classe_rendu"] = nouvelle
            base["lissage"] = motif
            stats[motif] += 1

    # --- les RUNS : chaines maximales a travers les noeuds de degre 2
    visited = [False] * len(edges)
    runs = []
    for e0 in range(len(edges)):
        if visited[e0]:
            continue
        visited[e0] = True
        chain = [e0]
        for k0 in (edges[e0]["a"], edges[e0]["b"]):
            k, cur = k0, e0
            while deg.get(k) == 2:
                o = inc[k][0] if inc[k][1] == cur else inc[k][1]
                if visited[o]:
                    break
                visited[o] = True
                chain.append(o)
                k = edges[o]["b"] if edges[o]["a"] == k else edges[o]["a"]
                cur = o
        runs.append(chain)
    stats["runs"] = len(runs)

    # --- niveau 1 : vote majoritaire pondere par la longueur, par run
    for chain in runs:
        poids = Counter()
        for ei in chain:
            c = classe(ei)
            if c in CLASSES_VOTANTES:
                poids[c] += edges[ei]["L"]
        if len(poids) <= 1:
            continue
        stats["runs_heterogenes"] += 1
        gagnant = sorted(poids.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
        changer([ei for ei in chain if classe(ei) in CLASSES_VOTANTES],
                gagnant, "run_vote")

    # --- infos de run pour les niveaux 2 et 3
    infos = []
    run_of = {}
    for ri, chain in enumerate(runs):
        L = sum(edges[ei]["L"] for ei in chain)
        bouts = []
        for ei in chain:
            for k in (edges[ei]["a"], edges[ei]["b"]):
                if deg.get(k) != 2:
                    bouts.append(k)
        cls_votables = set(classe(ei) for ei in chain if classe(ei) in CLASSES_VOTANTES)
        noms = Counter()
        for ei in chain:
            if nom(ei):
                noms[nom(ei)] += edges[ei]["L"]
        infos.append({"chain": chain, "L": L, "bouts": bouts,
                      "classe": (cls_votables.pop() if len(cls_votables) == 1 else None),
                      "nom": (noms.most_common(1)[0][0] if noms else "")})
        for ei in chain:
            run_of[ei] = ri

    # --- niveau 2 : sandwich de rue nommee (jusqu'a stabilite, 3 passes max)
    for _ in range(3):
        n_avant = stats["rue_nommee"]
        for ri, R in enumerate(infos):
            if (R["classe"] is None or not R["nom"] or R["L"] > SANDWICH_MAX_M
                    or len(R["bouts"]) != 2 or R["bouts"][0] == R["bouts"][1]):
                continue
            voisins_cls = []
            ok = True
            for k in R["bouts"]:
                memes = set()
                for ej in inc[k]:
                    rj = run_of[ej]
                    if rj == ri:
                        continue
                    V = infos[rj]
                    if V["nom"] == R["nom"] and V["classe"] is not None:
                        memes.add(V["classe"])
                if len(memes) != 1:
                    ok = False
                    break
                voisins_cls.append(memes.pop())
            if (ok and voisins_cls[0] == voisins_cls[1]
                    and voisins_cls[0] != R["classe"]):
                changer([ei for ei in R["chain"] if classe(ei) in CLASSES_VOTANTES],
                        voisins_cls[0], "rue_nommee")
                R["classe"] = voisins_cls[0]
        if stats["rue_nommee"] == n_avant:
            break

    # --- niveau 3 : absorption des POCHES enclavees (2 passes)
    def meme_zone(k1, k2, cls_d, exclus):
        """Plus court chemin de k1 a k2 par des aretes de classe cls_d,
        <= ZONE_BFS_MAX_M ? (Dijkstra borne, graphe local minuscule.)"""
        import heapq
        dist = {k1: 0.0}
        pile = [(0.0, k1)]
        while pile:
            d, k = heapq.heappop(pile)
            if k == k2:
                return True
            if d > dist.get(k, 1e18) or d > ZONE_BFS_MAX_M:
                continue
            for ej in inc[k]:
                if ej in exclus or classe(ej) != cls_d:
                    continue
                o = edges[ej]["b"] if edges[ej]["a"] == k else edges[ej]["a"]
                nd = d + edges[ej]["L"]
                if nd <= ZONE_BFS_MAX_M and nd < dist.get(o, 1e18):
                    dist[o] = nd
                    heapq.heappush(pile, (nd, o))
        return False

    absorbes = []
    for _ in range(2):
        n_avant = stats["absorption"]
        # POCHES : composantes connexes de runs votables de MEME classe (deux runs
        # de meme classe qui partagent un noeud d'extremite sont dans la meme poche).
        runs_par_bout = defaultdict(list)
        for ri, R in enumerate(infos):
            if R["classe"] is not None:
                for k in R["bouts"]:
                    runs_par_bout[k].append(ri)
        vus = set()
        poches = []
        for r0, R0 in enumerate(infos):
            if r0 in vus or R0["classe"] is None:
                continue
            pile = [r0]
            vus.add(r0)
            membres = []
            while pile:
                ri = pile.pop()
                membres.append(ri)
                for k in infos[ri]["bouts"]:
                    for rj in runs_par_bout[k]:
                        if rj not in vus and infos[rj]["classe"] == R0["classe"]:
                            vus.add(rj)
                            pile.append(rj)
            poches.append(membres)

        for membres in poches:
            c_cls = infos[membres[0]]["classe"]
            aretes = set()
            for ri in membres:
                aretes.update(infos[ri]["chain"])
            bouts = set()
            for ri in membres:
                bouts.update(infos[ri]["bouts"])
            voisins = set()
            contacts = set()          # noeuds ou la poche touche la classe D
            for k in bouts:
                for ej in inc[k]:
                    if ej in aretes:
                        continue
                    c = classe(ej)
                    if c in CLASSES_VOTANTES:
                        voisins.add(c)
                        contacts.add(k)
            if len(voisins) != 1:
                continue
            d_cls = voisins.pop()
            if d_cls == c_cls or len(contacts) < 2:
                continue          # une impasse (1 seul contact) n'est pas une enclave
            L = sum(edges[ei]["L"] for ei in aretes
                    if classe(ei) in CLASSES_VOTANTES)
            if c_cls == "pietonne_pavee":
                absorbe = L <= ABSORB_PAVEE_MAX_M
            elif d_cls == "pietonne_pavee":
                # ILOT INJOIGNABLE : de la voirie circulee entierement cernee de
                # pieton — aucune voiture ne peut y entrer, la donnee se contredit.
                absorbe = L <= ABSORB_ILOT_MAX_M
            else:
                cl = sorted(contacts)
                absorbe = (L <= ABSORB_MAX_M
                           or (L <= ABSORB_ZONE_MAX_M
                               and meme_zone(cl[0], cl[-1], d_cls, aretes)))
            if absorbe:
                ei0 = infos[membres[0]]["chain"][0]
                x, y = edges[ei0]["a"]
                absorbes.append("%.0f m %s->%s (%d runs) vers (%.0f,%.0f) %s"
                                % (L, c_cls, d_cls, len(membres), x / 10.0, y / 10.0,
                                   troncons[edges[ei0]["ti"]][0].get("nom") or "?"))
                changer([ei for ei in aretes if classe(ei) in CLASSES_VOTANTES],
                        d_cls, "absorption")
                for ri in membres:
                    infos[ri]["classe"] = d_cls
        if stats["absorption"] == n_avant:
            break

    # --- controle final : bascules residuelles en pleine rue (objectif 0 votable)
    for k, eis in inc.items():
        if len(eis) != 2:
            continue
        c1, c2 = classe(eis[0]), classe(eis[1])
        if c1 != c2:
            if c1 in CLASSES_VOTANTES and c2 in CLASSES_VOTANTES:
                stats["bascules_restantes_votables"] += 1
            else:
                stats["bascules_restantes_nature"] += 1

    stats["fragments_absorbes"] = len(absorbes)
    for a in absorbes:
        log("  absorption : " + a)
    log("lissage : %d runs (%d heterogenes), %d aretes re-classees par vote, "
        "%d par rue nommee, %d absorbees (%d fragments) ; bascules restantes "
        "en pleine rue : %d votables (objectif 0), %d de nature"
        % (stats["runs"], stats["runs_heterogenes"], stats["run_vote"],
           stats["rue_nommee"], stats["absorption"], stats["fragments_absorbes"],
           stats["bascules_restantes_votables"], stats["bascules_restantes_nature"]))
    return dict(stats)


# --- LOT A (A3) : l'overlay pieton OSM ------------------------------------------
# Le critere BD TOPO strict rate une partie du plateau pietonnier reel — MESURE :
# le millesime 2026-06 code rue du Taur et rue Saint Rome en acces 'Libre'. Le
# critere est donc HYBRIDE : strict BD TOPO ∪ OSM highway=pedestrian. Ce matcher
# calcule un CHAMP VIRTUEL 'osm_pieton' ('oui'/'non') par troncon, que la table
# reseau_classes.json peut citer comme n'importe quelle colonne BD TOPO.
# Seuls les ways LINEAIRES votent (ni fermes ni area=yes) : l'anneau d'une place
# pietonne longe des facades et matcherait les axes des rues adjacentes.
class OsmPietonMatcher:
    def __init__(self, ways, buffer_m=6.0, couverture_min=0.6, pas_m=3.0,
                 bucket_m=25.0):
        self.buffer_m = buffer_m
        self.couverture_min = couverture_min
        self.pas_m = pas_m
        self.bucket_m = bucket_m
        self.grid = defaultdict(list)      # (bx,by) -> [(ax,ay,bx2,by2), ...]
        self.n_segments = 0
        r = buffer_m
        for w in ways:
            if w.get("ferme") or w.get("aire"):
                continue
            pts = w.get("pts") or []
            for i in range(len(pts) - 1):
                ax, ay = pts[i]
                bx, by = pts[i + 1]
                self.n_segments += 1
                x0, x1 = min(ax, bx) - r, max(ax, bx) + r
                y0, y1 = min(ay, by) - r, max(ay, by) + r
                for gx in range(int(math.floor(x0 / bucket_m)),
                                int(math.floor(x1 / bucket_m)) + 1):
                    for gy in range(int(math.floor(y0 / bucket_m)),
                                    int(math.floor(y1 / bucket_m)) + 1):
                        self.grid[(gx, gy)].append((ax, ay, bx, by))

    def _pres(self, x, y):
        """distance(point, un segment OSM) <= buffer ?"""
        b = self.buffer_m
        b2 = b * b
        k = (int(math.floor(x / self.bucket_m)), int(math.floor(y / self.bucket_m)))
        for (ax, ay, bx, by) in self.grid.get(k, ()):
            dx, dy = bx - ax, by - ay
            l2 = dx * dx + dy * dy
            if l2 <= 1e-12:
                t = 0.0
            else:
                t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / l2))
            px, py = ax + t * dx, ay + t * dy
            if (x - px) ** 2 + (y - py) ** 2 <= b2:
                return True
        return False

    def couverture(self, lignes):
        """fraction des echantillons (pas_m) de la polyligne couverts par l'overlay."""
        tot = dedans = 0
        for pts in lignes:
            for i in range(len(pts) - 1):
                ax, ay = pts[i][0], pts[i][1]
                bx, by = pts[i + 1][0], pts[i + 1][1]
                seg = math.hypot(bx - ax, by - ay)
                n = max(1, int(math.ceil(seg / self.pas_m)))
                for j in range(n + (1 if i == len(pts) - 2 else 0)):
                    t = min(1.0, j / float(n))
                    tot += 1
                    if self._pres(ax + t * (bx - ax), ay + t * (by - ay)):
                        dedans += 1
        return (dedans / float(tot)) if tot else 0.0

    def est_pieton(self, lignes):
        return self.couverture(lignes) >= self.couverture_min


def charger_osm_pieton(conf):
    """Matcher depuis le fichier cache par Tools/fetch_osm_pieton.py, ou None si la
    section/ le fichier manquent (le critere retombe alors sur le strict BD TOPO)."""
    sec = conf.get("osm_pieton")
    if not sec:
        return None
    path = os.path.join(ROOT, *sec["fichier"].split("/"))
    if not os.path.exists(path):
        log("ATTENTION : overlay OSM pieton absent (%s) — critere strict seul" % path)
        return None
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    m = OsmPietonMatcher(d.get("ways") or [],
                         buffer_m=sec.get("buffer_m", 6.0),
                         couverture_min=sec.get("couverture_min", 0.6),
                         pas_m=sec.get("pas_echantillon_m", 3.0))
    log("overlay OSM pieton : %s — %d segments lineaires indexes (buffer %.1f m, "
        "couverture >= %.0f %%)"
        % (os.path.basename(path), m.n_segments, m.buffer_m, m.couverture_min * 100))
    return m


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
    print(line, flush=True)
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass
    hb = HEARTBEAT[0]
    if hb:
        try:
            with open(hb, "w", encoding="utf-8") as f:
                f.write(line + "\n")
            with open(os.path.splitext(hb)[0].replace("_heartbeat", "_progress") + ".log",
                      "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except Exception:
            pass


# --- config --------------------------------------------------------------------
def norm(s):
    """Minuscule sans accents — meme normalisation que j3c_sols_corridor.norm()."""
    if s is None:
        return ""
    s = str(s).lower()
    for a, b in (("à", "a"), ("â", "a"), ("ä", "a"), ("é", "e"),
                 ("è", "e"), ("ê", "e"), ("ë", "e"), ("î", "i"),
                 ("ï", "i"), ("ô", "o"), ("ö", "o"), ("û", "u"),
                 ("ù", "u"), ("ü", "u"), ("ç", "c")):
        s = s.replace(a, b)
    return s.strip()


def charger_config(path=CONF_PATH):
    with open(path, "rb") as f:
        brut = f.read()
    conf = json.loads(brut.decode("utf-8"))
    conf["_sha1"] = hashlib.sha1(brut).hexdigest()[:12]
    # pre-normalisation des regles : le matching ne refait pas norm() par troncon
    regles = []
    for r in conf["regles"]:
        regles.append({
            "classe": r["classe"],
            "si": {k: set(norm(v) for v in vs) for k, vs in (r.get("si") or {}).items()
                   if not k.startswith("_")},
            "sauf": {k: set(norm(v) for v in vs) for k, vs in (r.get("sauf") or {}).items()
                     if not k.startswith("_")},
        })
    conf["_regles"] = regles
    vals = conf.get("valeurs", {})
    conf["_valeurs"] = {k: {norm(a): b for a, b in v.items()}
                        for k, v in vals.items() if not k.startswith("_")}
    return conf


def classer(props, conf):
    """classe_rendu d'un troncon : PREMIERE regle qui matche, sinon classe_defaut."""
    for r in conf["_regles"]:
        ok = True
        for champ, valeurs in r["si"].items():
            if norm(props.get(champ)) not in valeurs:
                ok = False
                break
        if not ok:
            continue
        for champ, valeurs in r["sauf"].items():
            if norm(props.get(champ)) in valeurs:
                ok = False
                break
        if ok:
            return r["classe"]
    return conf["classe_defaut"]


def valeur(conf, champ, brut):
    """Normalise une enumeration via la table 'valeurs' ; recopie brute sinon."""
    if brut is None or brut == "":
        return None
    t = conf["_valeurs"].get(champ)
    if t is None:
        return brut
    return t.get(norm(brut), brut)


def as_int(v, defaut=None):
    try:
        return int(str(v).strip())
    except Exception:
        return defaut


def as_float(v):
    try:
        f = float(str(v).replace(",", "."))
        return f
    except Exception:
        return None


def largeur_de(props, conf):
    """(largeur_m, 'mesure'|'repli') — table IDENTIQUE a j3c_sols_corridor."""
    rep = conf["largeur_repli_m"]
    l = as_float(props.get("largeur_de_chaussee"))
    if l is not None and l > 0.5:
        return round(l, 2), "mesure"
    if norm(props.get("nature")) in set(norm(x) for x in rep["_natures_etroites"]):
        return rep["chemin"], "repli"
    imp = str(props.get("importance") or "").strip()
    base = rep["par_importance"].get(imp, rep["defaut"])
    voies = as_int(props.get("nombre_de_voies"))
    if voies is not None and voies >= 2:
        return round(max(base, 3.0 * voies), 2), "repli"
    return round(base, 2), "repli"


# --- decoupe d'une polyligne par la grille -------------------------------------
def _lerp(p, q, t):
    z = None
    if p[2] is not None and q[2] is not None:
        z = p[2] + (q[2] - p[2]) * t
    elif t < 0.5:
        z = p[2]
    else:
        z = q[2]
    return (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t, z)


def decouper(pts, cell_m):
    """Polyligne locale -> morceaux ENTIEREMENT contenus dans une cellule.

    Renvoie [(cx, cy, [(x,y,z)...], abs0, cut0, cut1)] dans l'ordre du parcours.
    abs0 = abscisse curviligne (m) du debut du morceau dans la polyligne COMPLETE,
    cut0/cut1 = l'extremite vient d'une coupe de frontiere (et non d'une vraie
    extremite de troncon). Les points de coupe sont CALES exactement sur la ligne
    de grille : les deux cellules voisines voient rigoureusement le meme point,
    donc la meme cle decimetrique — le recollage est une egalite, pas un rayon.
    """
    out = []
    cur = None
    absc = 0.0
    for i in range(len(pts) - 1):
        p, q = pts[i], pts[i + 1]
        dx, dy = q[0] - p[0], q[1] - p[1]
        seg = math.hypot(dx, dy)
        coupes = []
        if abs(dx) > 1e-12:
            k0 = int(math.floor(min(p[0], q[0]) / cell_m)) + 1
            k1 = int(math.ceil(max(p[0], q[0]) / cell_m)) - 1
            for k in range(k0, k1 + 1):
                t = (k * cell_m - p[0]) / dx
                if 1e-12 < t < 1.0 - 1e-12:
                    coupes.append((t, 0, k * cell_m))
        if abs(dy) > 1e-12:
            k0 = int(math.floor(min(p[1], q[1]) / cell_m)) + 1
            k1 = int(math.ceil(max(p[1], q[1]) / cell_m)) - 1
            for k in range(k0, k1 + 1):
                t = (k * cell_m - p[1]) / dy
                if 1e-12 < t < 1.0 - 1e-12:
                    coupes.append((t, 1, k * cell_m))
        coupes.sort()
        bornes = [(0.0, None, None)] + coupes + [(1.0, None, None)]
        for j in range(len(bornes) - 1):
            t0, ax0, v0 = bornes[j]
            t1, ax1, v1 = bornes[j + 1]
            if t1 - t0 <= 1e-12:
                continue
            a = _lerp(p, q, t0)
            b = _lerp(p, q, t1)
            if ax0 is not None:      # calage exact sur la ligne de grille
                a = (v0, a[1], a[2]) if ax0 == 0 else (a[0], v0, a[2])
            if ax1 is not None:
                b = (v1, b[1], b[2]) if ax1 == 0 else (b[0], v1, b[2])
            mx, my = (a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5
            c = G.cell_of(mx, my, cell_m)
            if cur is None or cur[0] != c:
                if cur is not None:
                    # le morceau precedent se termine sur une frontiere : cut1 = True
                    out.append((cur[0][0], cur[0][1], cur[1], cur[2], cur[3], True))
                cur = [c, [a], absc + t0 * seg, ax0 is not None]
            cur[1].append(b)
        absc += seg
    if cur is not None:
        out.append((cur[0][0], cur[0][1], cur[1], cur[2], cur[3], False))
    return out


def longueur(pts):
    return sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
               for i in range(len(pts) - 1))


# --- construction --------------------------------------------------------------
def cellules_de(conf, args):
    cell_m = conf["grille"]["cellSizeM"]
    if args.cells:
        cs = []
        for s in args.cells:
            a, b = s.split(",")
            cs.append((int(a), int(b)))
        return cs, cell_m, "cells"
    emp = conf["emprises"][args.emprise]
    if "cellules" in emp:
        return [tuple(c) for c in emp["cellules"]], cell_m, args.emprise
    cx0, cx1 = emp["cx"]
    cy0, cy1 = emp["cy"]
    return [(x, y) for x in range(cx0, cx1 + 1) for y in range(cy0, cy1 + 1)], cell_m, args.emprise


def lire_gpkg(bbox_local, cell_m):
    """Troncons dont la bbox Lambert 93 touche la fenetre. Renvoie (props, lignes)."""
    minx, miny, maxx, maxy = bbox_local
    bx0, by0, bx1, by1 = G.local_bbox_to_lamb93(minx, miny, maxx, maxy, 200.0)
    if not os.path.exists(GPKG):
        raise SystemExit("GPKG introuvable : %s" % GPKG)
    con = sqlite3.connect("file:%s?mode=ro" % GPKG.replace("\\", "/"), uri=True)
    cur = con.cursor()
    sql = ("SELECT t.geometrie, " + ", ".join("t." + c for c in COLS) +
           " FROM troncon_de_route t"
           " JOIN rtree_troncon_de_route_geometrie r ON r.id = t.fid"
           " WHERE r.maxx >= ? AND r.minx <= ? AND r.maxy >= ? AND r.miny <= ?")
    n = 0
    for row in cur.execute(sql, (bx0, bx1, by0, by1)):
        props = dict(zip(COLS, row[1:]))
        lignes = []
        for ligne in G.gpkg_lines(row[0]):
            loc = [G.lamb93_to_local(x, y) + (z,) for (x, y, z) in ligne]
            lignes.append(loc)
        if lignes:
            n += 1
            yield props, lignes
    con.close()


def lire_riche(path, bbox_local, cell_m):
    """REPLI sans GPKG : Tools/fetch_routes_gpkg.py --> routes_riche_<nom>.json.

    Deux differences a connaitre, documentees dans Doc/Reseau-Sidecar.md :
      - la geometrie y est deja AMINCIE a 1 m (seuil de Manhattan) : les sommets
        intermediaires les plus fins sont perdus, la connectivite ne l'est pas ;
      - il n'y a PAS d'altitude Z (le fetch compatible n'en transporte pas).
    """
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    log("repli sans GPKG : %s (%d troncons, %s)"
        % (os.path.basename(path), len(d["troncons"]), d.get("format")))
    for tr in d["troncons"]:
        pts = tr.get("pts") or []
        if len(pts) < 2:
            continue
        yield tr, [[(p[0], p[1], None) for p in pts]]


def edge_props(props, conf):
    """Attributs persistes d'une arete. Les champs nuls sont OMIS (volumetrie) ;
    'absent' signifie donc 'non renseigne dans BD TOPO', jamais 'faux'."""
    e = {}
    w, wsrc = largeur_de(props, conf)
    e["w"] = w
    if wsrc != "mesure":
        e["w_src"] = wsrc                       # absent = largeur MESUREE par l'IGN
    e["nature"] = props.get("nature")
    imp = as_int(props.get("importance"))
    if imp is not None:
        e["importance"] = imp
    voies = as_int(props.get("nombre_de_voies"))
    if voies is not None:
        e["lanes"] = voies
    for cle, champ in (("sens", "sens_de_circulation"),
                       ("acces_vl", "acces_vehicule_leger"),
                       ("acces_pieton", "acces_pieton"),
                       ("bus", "reserve_aux_bus")):
        v = valeur(conf, champ, props.get(champ))
        if v is not None:
            e[cle] = v
    vit = as_int(props.get("vitesse_moyenne_vl"))
    if vit is not None:
        e["vit_moy"] = vit                      # ⚠ vitesse de PARCOURS, pas limitation
    for cle, champ in (("cyclable_g", "amenagement_cyclable_gauche"),
                       ("cyclable_d", "amenagement_cyclable_droit")):
        if props.get(champ):
            e[cle] = props[champ]
    if props.get("urbain") is not None:
        e["urbain"] = bool(props["urbain"])
    if props.get("prive"):
        e["prive"] = True                       # absent = non prive OU non renseigne
    if props.get("fictif"):
        e["fictif"] = True                      # absent = non fictif
    etat = props.get("etat_de_l_objet")
    if etat and norm(etat) != "en service":
        e["etat"] = etat                        # absent = « En service »
    pos = props.get("position_par_rapport_au_sol")
    niv = as_int(pos, None)
    if niv is None:
        e["niveau"] = 0
        if pos:
            e["niveau_txt"] = pos               # ex. « Gué ou radier »
    else:
        e["niveau"] = niv
    if e["niveau"] > 0:
        e["pont"] = True
    elif e["niveau"] < 0:
        e["tunnel"] = True
    for cle, champ in (("nom", "nom_voie_ban_gauche"), ("ban", "id_ban_odonyme_gauche"),
                       ("num", "cpx_numero"), ("classement", "cpx_classement_administratif")):
        if props.get(champ):
            e[cle] = props[champ]
    e["classe_rendu"] = classer(props, conf)
    return e


def construire(cells, cell_m, conf, nom_emprise, source_json=None):
    cible = set(cells)
    minx = min(c[0] for c in cells) * cell_m
    maxx = (max(c[0] for c in cells) + 1) * cell_m
    miny = min(c[1] for c in cells) * cell_m
    maxy = (max(c[1] for c in cells) + 1) * cell_m
    log("emprise %s : %d cellules, fenetre locale X[%.0f;%.0f] Y[%.0f;%.0f] m"
        % (nom_emprise, len(cells), minx, maxx, miny, maxy))

    par_cell = defaultdict(list)
    n_tr = n_geo = n_multi = n_degen = 0
    n_osm = 0
    parts_par_cleabs = Counter()
    t0 = time.time()
    osm = charger_osm_pieton(conf)
    src = (lire_riche(source_json, (minx, miny, maxx, maxy), cell_m) if source_json
           else lire_gpkg((minx, miny, maxx, maxy), cell_m))
    # PASSE 1 (LOT A-bis) : collecter TOUS les troncons de la fenetre avant la
    # decoupe — le lissage de coherence a besoin du graphe entier, pas des morceaux.
    troncons = []
    for props, lignes in src:
        n_tr += 1
        if len(lignes) > 1:
            n_multi += 1
        # LOT A (A3) — champ VIRTUEL 'osm_pieton', calcule sur la geometrie
        # COMPLETE du troncon (pas les morceaux) : la table de classification
        # peut le citer comme une colonne BD TOPO ordinaire.
        if osm is not None:
            props = dict(props)
            props["osm_pieton"] = "oui" if osm.est_pieton(lignes) else "non"
            if props["osm_pieton"] == "oui":
                n_osm += 1
        base = edge_props(props, conf)
        if props.get("osm_pieton") == "oui":
            base["osm_pieton"] = True       # trace : POURQUOI la classe (hybride)
        troncons.append((base, props.get("cleabs"), lignes))
        if n_tr % 20000 == 0:
            log("  ... %d troncons lus (%.0f s)" % (n_tr, time.time() - t0))

    # LOT A-bis (correctifs 1 et 3) : coherence de classe le long des rues.
    # NB en emprise partielle (proto), les rues coupees par le bord de fenetre ne
    # voient qu'un contexte partiel — la fenetre lit large (bbox + 200 m), et la
    # regeneration complete (carre10) refait la meme operation sur tout le graphe.
    stats_lissage = lisser_classes(troncons) if LISSAGE_ON else {}

    # PASSE 2 : la decoupe par la grille, inchangee.
    for (base, cleabs, lignes) in troncons:
        for ligne in lignes:
            n_geo += 1
            for (cx, cy, pts, abs0, cut0, cut1) in decouper(ligne, cell_m):
                if (cx, cy) not in cible:
                    continue
                L = longueur(pts)
                if len(pts) < 2 or L < EPS_LEN:
                    n_degen += 1
                    continue
                par_cell[(cx, cy)].append((base, cleabs, pts, abs0, cut0, cut1, L))
                parts_par_cleabs[cleabs] += 1
    log("lecture %s : %d troncons (%d geometries, %d multi-parties), "
        "%d morceaux retenus, %d degeneres ecartes, %d matches OSM pieton, %.1f s"
        % ("JSON riche" if source_json else "GPKG", n_tr, n_geo, n_multi,
           sum(len(v) for v in par_cell.values()), n_degen, n_osm, time.time() - t0))
    return par_cell, parts_par_cleabs, {
        "source": os.path.basename(source_json) if source_json else "GPKG " + MILLESIME,
        "troncons_lus": n_tr, "geometries": n_geo,
        "multi_parties": n_multi, "degeneres": n_degen,
        "osm_pieton_matches": n_osm, "lissage": stats_lissage}


def ecrire_cellule(cx, cy, morceaux, cell_m, conf, parts, nom_emprise):
    x0, y0, x1, y1 = G.cell_box(cx, cy, cell_m)
    noeuds = {}
    ordre = []

    def noeud(x, y, z, cut):
        k = G.noeud_key(x, y)
        nd = noeuds.get(k)
        if nd is None:
            bord = (abs(x - x0) < EPS_BORD or abs(x - x1) < EPS_BORD or
                    abs(y - y0) < EPS_BORD or abs(y - y1) < EPS_BORD)
            nd = {"id": len(ordre), "k": [k[0], k[1]], "p": [round(x, 2), round(y, 2)],
                  "deg": 0}
            if z is not None:
                nd["z"] = round(z, 2)
            if bord:
                nd["bord"] = True
            noeuds[k] = nd
            ordre.append(nd)
        nd["deg"] += 1
        if not cut:
            nd["jonction"] = True               # vraie extremite de troncon BD TOPO
        if nd.get("z") is None and z is not None:
            nd["z"] = round(z, 2)
        return nd["id"]

    edges = []
    par_cleabs = Counter()
    for (base, cleabs, pts, abs0, cut0, cut1, L) in morceaux:
        e = dict(base)
        e["id"] = len(edges)
        e["cleabs"] = cleabs
        e["a"] = noeud(pts[0][0], pts[0][1], pts[0][2], cut0)
        e["b"] = noeud(pts[-1][0], pts[-1][1], pts[-1][2], cut1)
        e["pts"] = [[round(p[0], 2), round(p[1], 2)] for p in pts]
        zs = [p[2] for p in pts]
        if any(z is not None for z in zs):
            e["z"] = [None if z is None else round(z, 2) for z in zs]
        e["len"] = round(L, 2)
        if abs0 > 0.005:
            e["abs0"] = round(abs0, 2)
        n = parts[cleabs]
        if n > 1:
            e["part"] = par_cleabs[cleabs]
            e["nparts"] = n
            par_cleabs[cleabs] += 1
        if cut0:
            e["cut0"] = True
        if cut1:
            e["cut1"] = True
        edges.append(e)

    km = Counter()
    for e in edges:
        km[e["classe_rendu"]] += e["len"]
    doc = {
        "cell": [cx, cy],
        "cellSizeM": cell_m,
        "origin": conf["grille"]["origine"],
        "crs": "local_citylab_equirect_capitole",
        "axes": "x = est (m), y = sud (m) ; NORD = -Y",
        "bbox": [x0, y0, x1, y1],
        "source": MILLESIME,
        "classes": "reseau_classes.json v%s sha1=%s" % (conf["version"], conf["_sha1"]),
        "ecrit_par": nom_emprise,
        "_ecrit_par": ("emprise du DERNIER build qui a ecrit cette cellule. Les emprises "
                       "se CHEVAUCHENT (proto est inclus dans carre10) et un fichier de "
                       "cellule est indexe par (cx,cy) SEUL : le contenu est identique "
                       "quelle que soit l'emprise (meme source, meme grille, meme table), "
                       "seul ce libelle change. Ne jamais filtrer un lot sur ce champ : "
                       "utiliser la liste 'cellules' de index_<emprise>.json."),
        "genere": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "nodeKey": "decimetre : k = [round(x*10), round(y*10)] — meme cle que FJunctionMap::Key (C++)",
        "stats": {
            "edges": len(edges),
            "nodes": len(ordre),
            "longueur_m": round(sum(e["len"] for e in edges), 1),
            "km_par_classe": {k: round(v / 1000.0, 3) for k, v in sorted(km.items())},
        },
        "nodes": ordre,
        "edges": edges,
    }
    path = os.path.join(OUT_DIR, "reseau_%d_%d.json" % (cx, cy))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, separators=(",", ":"))
    return path, doc


# --- validation ----------------------------------------------------------------
def valider(cells, cell_m, nom_emprise, meta=None):
    """CONTROLE SUR LE PRODUIT ECRIT (pas sur la memoire) : on relit les fichiers
    de cellule et on reconstruit le graphe par recollage des cles decimetriques."""
    cible = set(cells)
    deg = Counter()
    adj = defaultdict(set)
    km_classe = Counter()
    n_classe = Counter()
    surf_classe = Counter()
    edges_tot = nodes_tot = 0
    degen = 0
    manquants = []
    tailles = []
    bords = defaultdict(set)          # (cx,cy) -> cles de noeuds de bord
    fictifs = ponts = tunnels = hors_service = 0
    cleabs_vus = set()
    cle_vers_cleabs = defaultdict(list)   # cle decimetrique -> cleabs incidents
    coupes = []                           # (cle, cleabs) des extremites COUPEES
    cls_par_noeud = defaultdict(list)     # LOT A-bis : bascules en pleine rue

    for (cx, cy) in cells:
        path = os.path.join(OUT_DIR, "reseau_%d_%d.json" % (cx, cy))
        if not os.path.exists(path):
            manquants.append((cx, cy))
            continue
        tailles.append((os.path.getsize(path), cx, cy))
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        if abs(d["cellSizeM"] - cell_m) > 1e-6:
            raise SystemExit("cellule %d,%d : cellSizeM %s != %s" % (cx, cy, d["cellSizeM"], cell_m))
        nk = {n["id"]: tuple(n["k"]) for n in d["nodes"]}
        for n in d["nodes"]:
            if n.get("bord"):
                bords[(cx, cy)].add(tuple(n["k"]))
        nodes_tot += len(d["nodes"])
        for e in d["edges"]:
            edges_tot += 1
            cleabs_vus.add(e["cleabs"])
            if e["len"] < EPS_LEN:
                degen += 1
            c = e["classe_rendu"]
            km_classe[c] += e["len"]
            n_classe[c] += 1
            surf_classe[c] += e["len"] * e.get("w", 0.0)
            if e.get("fictif"):
                fictifs += 1
            if e.get("pont"):
                ponts += 1
            if e.get("tunnel"):
                tunnels += 1
            if e.get("etat"):
                hors_service += 1
            a, b = nk[e["a"]], nk[e["b"]]
            deg[a] += 1
            deg[b] += 1
            cls_par_noeud[a].append(c)
            cls_par_noeud[b].append(c)
            cle_vers_cleabs[a].append(e["cleabs"])
            cle_vers_cleabs[b].append(e["cleabs"])
            if e.get("cut0"):
                coupes.append((a, e["cleabs"]))
            if e.get("cut1"):
                coupes.append((b, e["cleabs"]))
            if a != b:
                adj[a].add(b)
                adj[b].add(a)

    # composantes connexes du graphe RECOLLE (toutes cellules confondues)
    seen = set()
    comps = []
    for n0 in adj:
        if n0 in seen:
            continue
        pile = [n0]
        seen.add(n0)
        taille = 0
        while pile:
            u = pile.pop()
            taille += 1
            for v in adj[u]:
                if v not in seen:
                    seen.add(v)
                    pile.append(v)
        comps.append(taille)
    comps.sort(reverse=True)

    # RECOLLEMENT : toute extremite COUPEE par la grille doit se retrouver a
    # l'identique (meme cle decimetrique, meme cleabs) dans la cellule voisine.
    # On ne teste pas le voisin nomme mais l'EXISTENCE de la continuite : un point
    # de coupe peut tomber sur un coin, et la suite du troncon partir en diagonale.
    # Les coupes du bord EXTERIEUR de l'emprise sont exclues (la suite est dehors).
    absents = set(manquants)
    orphelins = 0
    testes = 0
    exterieures = 0
    for (k, cleabs) in coupes:
        x, y = k[0] / 10.0, k[1] / 10.0
        touchees = set()
        for (dx, dy) in ((-EPS_BORD, -EPS_BORD), (EPS_BORD, -EPS_BORD),
                         (-EPS_BORD, EPS_BORD), (EPS_BORD, EPS_BORD)):
            touchees.add(G.cell_of(x + dx, y + dy, cell_m))
        if any(c not in cible or c in absents for c in touchees):
            exterieures += 1
            continue
        testes += 1
        if cle_vers_cleabs[k].count(cleabs) < 2:
            orphelins += 1

    dd = Counter(deg.values())
    # LOT A-bis : bascules de classe en PLEINE RUE (noeud de degre 2 dont les deux
    # aretes incidentes different). Objectif 0 entre classes votables ; une
    # transition vers une classe de NATURE (escalier, chemin) est legitime.
    basc_vot = basc_nat = 0
    for k, cs in cls_par_noeud.items():
        if deg[k] == 2 and len(set(cs)) > 1:
            if all(c in CLASSES_VOTANTES for c in cs):
                basc_vot += 1
            else:
                basc_nat += 1
    res = {
        "emprise": nom_emprise,
        "bascules_pleine_rue_votables": basc_vot,
        "bascules_pleine_rue_nature": basc_nat,
        "cellules_attendues": len(cells),
        "cellules_manquantes": [list(m) for m in manquants],
        "edges": edges_tot,
        "noeuds_par_cellule_cumules": nodes_tot,
        "noeuds_distincts_recolles": len(deg),
        "cleabs_distincts": len(cleabs_vus),
        "aretes_degenerees": degen,
        "longueur_km": round(sum(km_classe.values()) / 1000.0, 2),
        "km_par_classe": {k: round(v / 1000.0, 2) for k, v in km_classe.most_common()},
        "aretes_par_classe": dict(n_classe.most_common()),
        "ha_chaussee_par_classe": {k: round(v / 10000.0, 2) for k, v in surf_classe.most_common()},
        "degres": {str(k): dd[k] for k in sorted(dd)},
        "composantes": len(comps),
        "plus_grande_composante": comps[0] if comps else 0,
        "pct_plus_grande": round(100.0 * comps[0] / max(1, len(deg)), 2) if comps else 0.0,
        "composantes_1_2_noeuds": sum(1 for c in comps if c <= 2),
        "recollement_coupes_testees": testes,
        "recollement_coupes_orphelines": orphelins,
        "recollement_coupes_hors_emprise": exterieures,
        "noeuds_de_bord": sum(len(v) for v in bords.values()),
        "fictifs": fictifs, "ponts": ponts, "tunnels": tunnels,
        "hors_en_service": hors_service,
        "volumetrie": {
            "total_Mo": round(sum(t[0] for t in tailles) / 1048576.0, 2),
            "cellules": len(tailles),
            "min_Ko": round(min(t[0] for t in tailles) / 1024.0, 1) if tailles else 0,
            "median_Ko": round(sorted(t[0] for t in tailles)[len(tailles) // 2] / 1024.0, 1) if tailles else 0,
            "max_Ko": round(max(t[0] for t in tailles) / 1024.0, 1) if tailles else 0,
            "cellule_max": "reseau_%d_%d.json" % (max(tailles)[1], max(tailles)[2]) if tailles else None,
        },
    }
    if meta:
        res["lecture_source"] = meta
    return res


# --- selftest ------------------------------------------------------------------
def selftest():
    ok = True

    def chk(nom, cond, detail=""):
        nonlocal ok
        print("  %s %s %s" % ("OK " if cond else "KO ", nom, detail))
        if not cond:
            ok = False

    conf = charger_config()
    cm = 500.0

    # 1. decoupe : une ligne droite traversant 3 cellules conserve sa longueur
    pts = [(-600.0, 250.0, None), (900.0, 250.0, None)]
    ms = decouper(pts, cm)
    chk("decoupe droite : 4 morceaux", len(ms) == 4, "-> %d" % len(ms))
    chk("decoupe droite : longueur conservee",
        abs(sum(longueur(m[2]) for m in ms) - 1500.0) < 1e-6)
    chk("decoupe droite : cellules -2,0 -1,0 0,0 1,0",
        [(m[0], m[1]) for m in ms] == [(-2, 0), (-1, 0), (0, 0), (1, 0)],
        str([(m[0], m[1]) for m in ms]))
    chk("decoupe droite : abs0 croissants",
        all(ms[i][3] <= ms[i + 1][3] for i in range(len(ms) - 1)))
    chk("decoupe droite : points de coupe cales sur la grille",
        all(abs(m[2][-1][0] % cm) < 1e-9 for m in ms[:-1]))

    # 2. les extremites de morceaux voisins sont IDENTIQUES (recollage exact)
    chk("recollage exact des coupes",
        all(G.noeud_key(ms[i][2][-1][0], ms[i][2][-1][1]) ==
            G.noeud_key(ms[i + 1][2][0][0], ms[i + 1][2][0][1])
            for i in range(len(ms) - 1)))

    # 3. diagonale traversant un coin de cellule
    d = decouper([(-10.0, -10.0, None), (10.0, 10.0, None)], cm)
    chk("diagonale au coin : longueur conservee",
        abs(sum(longueur(m[2]) for m in d) - math.hypot(20.0, 20.0)) < 1e-6,
        "%d morceaux" % len(d))

    # 4. ligne entierement interieure : 1 morceau, aucune coupe
    d = decouper([(10.0, 10.0, None), (20.0, 30.0, None)], cm)
    chk("ligne interieure : 1 morceau non coupe",
        len(d) == 1 and d[0][4] is False and d[0][5] is False)

    # 5. interpolation Z
    d = decouper([(-10.0, 250.0, 100.0), (10.0, 250.0, 120.0)], cm)
    zc = d[0][2][-1][2]
    chk("Z interpole a la coupe", abs(zc - 110.0) < 1e-6, "z=%s" % zc)

    # 6. classification
    cas = [({"nature": "Type autoroutier", "importance": "1"}, "autoroute"),
           ({"nature": "Bretelle", "importance": "2"}, "autoroute"),
           ({"nature": "Route à 1 chaussée", "importance": "5",
             "acces_vehicule_leger": "Physiquement impossible"}, "pietonne_pavee"),
           ({"nature": "Sentier", "importance": "6",
             "acces_vehicule_leger": "Physiquement impossible"}, "pieton"),
           ({"nature": "Route à 1 chaussée", "importance": "5",
             "reserve_aux_bus": "Sens direct"}, "bus"),
           ({"nature": "Chemin", "importance": "6"}, "chemin"),
           ({"nature": "Route empierrée", "importance": "5"}, "chemin"),
           ({"nature": "Route à 2 chaussées", "importance": "2"}, "artere"),
           ({"nature": "Route à 1 chaussée", "importance": "3"}, "rue"),
           ({"nature": "Route à 1 chaussée", "importance": "4"}, "rue"),
           ({"nature": "Rond-point", "importance": "5"}, "ruelle"),
           ({"nature": "Route à 1 chaussée", "importance": "6"}, "ruelle"),
           ({"nature": "Route a 1 chaussee", "importance": None}, "ruelle")]
    for props, attendu in cas:
        got = classer(props, conf)
        chk("classe %-40s -> %s" % (str(props.get("nature")) + "/" + str(props.get("importance")),
                                    attendu), got == attendu, "(got %s)" % got)

    # 6b. LOT A (A3) : matcher OSM pieton — spatial + regle hybride
    ways = [{"pts": [[0.0, 0.0], [100.0, 0.0]]},
            {"pts": [[0.0, 50.0], [0.0, 60.0], [10.0, 60.0], [10.0, 50.0],
                     [0.0, 50.0]], "ferme": True, "aire": True}]
    m = OsmPietonMatcher(ways, buffer_m=6.0, couverture_min=0.6, pas_m=3.0)
    chk("osm : axe decale de 3 m matche", m.est_pieton([[(0.0, 3.0), (100.0, 3.0)]]))
    chk("osm : axe a 10 m ne matche pas", not m.est_pieton([[(0.0, 10.0), (100.0, 10.0)]]))
    chk("osm : couverture 1/3 < seuil 60 %",
        not m.est_pieton([[(0.0, 3.0), (300.0, 3.0)]]))
    chk("osm : un anneau de place ne vote pas",
        not m.est_pieton([[(2.0, 55.0), (8.0, 55.0)]]))
    for props, attendu in [
            ({"nature": "Route à 1 chaussée", "importance": "5",
              "acces_vehicule_leger": "Libre", "osm_pieton": "oui"}, "pietonne_pavee"),
            ({"nature": "Sentier", "importance": "6", "osm_pieton": "oui"}, "pieton"),
            ({"nature": "Route à 1 chaussée", "importance": "5",
              "osm_pieton": "non"}, "ruelle"),
            ({"nature": "Route à 1 chaussée", "importance": "5",
              "reserve_aux_bus": "Sens direct", "osm_pieton": "oui"}, "pietonne_pavee")]:
        got = classer(props, conf)
        chk("classe hybride %-30s -> %s" % (str(props.get("osm_pieton")) + "/"
            + str(props.get("nature")), attendu), got == attendu, "(got %s)" % got)

    # 6c. LOT A-bis : lissage de coherence (runs, sandwich, absorption).
    def T(cls, pts, nom_=None, cleabs="T"):
        b = {"classe_rendu": cls}
        if nom_:
            b["nom"] = nom_
        return (b, cleabs, [[(p[0], p[1], None) for p in pts]])

    # vote de run : un micro-troncon pave au milieu d'une ruelle -> ruelle
    ts = [T("ruelle", [(0, 0), (100, 0)]),
          T("pietonne_pavee", [(100, 0), (110, 0)]),
          T("ruelle", [(110, 0), (190, 0)])]
    st = lisser_classes(ts)
    chk("lissage : vote de run efface la bascule",
        all(t[0]["classe_rendu"] == "ruelle" for t in ts)
        and ts[1][0].get("classe_brute") == "pietonne_pavee"
        and st.get("bascules_restantes_votables", 0) == 0)

    # au carrefour (deg 3), le changement de classe est PERMIS
    ts = [T("rue", [(0, 0), (100, 0)]),
          T("ruelle", [(100, 0), (200, 0)]),
          T("ruelle", [(100, 0), (100, 80)])]
    lisser_classes(ts)
    chk("lissage : changement au carrefour conserve",
        ts[0][0]["classe_rendu"] == "rue" and ts[1][0]["classe_rendu"] == "ruelle")

    # une classe de NATURE (pieton) ne vote pas et ne change pas
    ts = [T("pietonne_pavee", [(0, 0), (60, 0)]),
          T("pieton", [(60, 0), (75, 0)]),
          T("pietonne_pavee", [(75, 0), (130, 0)])]
    lisser_classes(ts)
    chk("lissage : l'escalier reste un escalier",
        ts[1][0]["classe_rendu"] == "pieton"
        and ts[0][0]["classe_rendu"] == "pietonne_pavee")

    # sandwich de rue nommee : ruelle de 80 m entre deux runs 'rue' du meme nom
    ts = [T("rue", [(0, 0), (200, 0)], "Bd de Strasbourg"),
          T("ruelle", [(200, 0), (280, 0)], "Bd de Strasbourg"),
          T("rue", [(280, 0), (480, 0)], "Bd de Strasbourg"),
          T("ruelle", [(200, 0), (200, 90)], "Rue Laterale"),
          T("ruelle", [(280, 0), (280, 90)], "Rue Laterale 2")]
    st = lisser_classes(ts)
    chk("lissage : sandwich de rue nommee", ts[1][0]["classe_rendu"] == "rue"
        and st.get("rue_nommee", 0) >= 1)

    # absorption : fragment ruelle de 30 m enclave dans du pave aux deux bouts
    ts = [T("ruelle", [(0, 0), (30, 0)]),
          T("pietonne_pavee", [(0, 0), (-50, 10)]),
          T("pietonne_pavee", [(0, 0), (-50, -10)]),
          T("pietonne_pavee", [(30, 0), (80, 10)]),
          T("pietonne_pavee", [(30, 0), (80, -10)])]
    st = lisser_classes(ts)
    chk("lissage : enclave courte absorbee",
        ts[0][0]["classe_rendu"] == "pietonne_pavee"
        and ts[0][0].get("lissage") == "absorption")

    # absorption zonale : fragment de 69 m > seuil simple, mais ses deux bouts
    # sont relies par un anneau 'rue' de ~140 m -> absorbe (branche meme_zone)
    ts = [T("ruelle", [(0, 0), (69, 0)]),
          T("rue", [(0, 0), (34, 60), (69, 0)]),
          T("rue", [(0, 0), (-40, 0)]),
          T("rue", [(69, 0), (110, 0)])]
    st = lisser_classes(ts)
    chk("lissage : enclave zonale (place) absorbee",
        ts[0][0]["classe_rendu"] == "rue")

    # ILOT INJOIGNABLE : une poche de DEUX runs ruelle relies (170 m au total,
    # au-dela des seuils simples) entierement cernee de pieton pave -> absorbee
    # (aucune voiture ne peut y entrer), meme sans anneau court.
    ts = [T("ruelle", [(0, 0), (100, 0)]),
          T("ruelle", [(100, 0), (170, 0)]),
          T("ruelle", [(100, 0), (100, 40)]),      # branche qui fait le carrefour
          T("pietonne_pavee", [(0, 0), (-50, 5)]),
          T("pietonne_pavee", [(0, 0), (-50, -5)]),
          T("pietonne_pavee", [(170, 0), (230, 5)]),
          T("pietonne_pavee", [(170, 0), (230, -5)]),
          T("pietonne_pavee", [(100, 40), (100, 100)]),
          T("pietonne_pavee", [(100, 40), (150, 60)])]
    st = lisser_classes(ts)
    chk("lissage : ilot injoignable absorbe (poche de 3 runs)",
        all(t[0]["classe_rendu"] == "pietonne_pavee" for t in ts[:3]))

    # une pietonne_pavee de 45 m entouree de ruelle N'EST PAS absorbee (donnee
    # reelle, seuil 20 m) ; une de 15 m l'est (bruit de matching)
    ts = [T("pietonne_pavee", [(0, 0), (45, 0)]),
          T("ruelle", [(0, 0), (-60, 5)]), T("ruelle", [(0, 0), (-60, -5)]),
          T("ruelle", [(45, 0), (100, 5)]), T("ruelle", [(45, 0), (100, -5)])]
    lisser_classes(ts)
    chk("lissage : pavee 45 m conservee (donnee reelle)",
        ts[0][0]["classe_rendu"] == "pietonne_pavee")
    ts = [T("pietonne_pavee", [(0, 0), (15, 0)]),
          T("ruelle", [(0, 0), (-60, 5)]), T("ruelle", [(0, 0), (-60, -5)]),
          T("ruelle", [(15, 0), (80, 5)]), T("ruelle", [(15, 0), (80, -5)])]
    lisser_classes(ts)
    chk("lissage : pavee 15 m absorbee (bruit)",
        ts[0][0]["classe_rendu"] == "ruelle")

    # une impasse n'est PAS une enclave (un seul bout debouche)
    ts = [T("pietonne_pavee", [(0, 0), (47, 0)]),
          T("bus", [(0, 0), (-60, 5)]), T("bus", [(0, 0), (-60, -5)])]
    lisser_classes(ts)
    chk("lissage : l'impasse pavee n'est pas absorbee",
        ts[0][0]["classe_rendu"] == "pietonne_pavee")

    # 7. largeur : mesure prioritaire, replis identiques au corridor
    for props, attl, atts in (({"largeur_de_chaussee": 6.5, "importance": "5"}, 6.5, "mesure"),
                              ({"importance": "5"}, 4.5, "repli"),
                              ({"importance": "3", "nombre_de_voies": 4}, 12.0, "repli"),
                              ({"nature": "Chemin"}, 3.0, "repli")):
        gl, gs = largeur_de(props, conf)
        chk("largeur %-34s -> %.1f/%s" % (str(props), attl, atts),
            abs(gl - attl) < 1e-6 and gs == atts, "(got %.2f/%s)" % (gl, gs))

    # 8. la table de config couvre bien toutes les classes annoncees
    declarees = set(conf["_classes_connues"].keys())
    utilisees = set(r["classe"] for r in conf["regles"]) | {conf["classe_defaut"]}
    chk("classes declarees == classes utilisees", declarees == utilisees,
        str(declarees ^ utilisees))

    print("SELFTEST build_reseau :", "PASS" if ok else "ECHEC")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emprise", default="proto")
    ap.add_argument("--cells", nargs="*")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--valider-seulement", action="store_true")
    ap.add_argument("--source-json",
                    help="repli sans GPKG : un routes_riche_<nom>.json de fetch_routes_gpkg.py")
    ap.add_argument("--suffixe", default="",
                    help="suffixe du fichier d'index (comparaison de deux sources)")
    ap.add_argument("--heartbeat")
    args = ap.parse_args()
    if args.heartbeat:
        HEARTBEAT[0] = args.heartbeat
    if args.selftest:
        return selftest()

    conf = charger_config()
    cells, cell_m, nom = cellules_de(conf, args)
    os.makedirs(OUT_DIR, exist_ok=True)

    meta = None
    if not args.valider_seulement:
        t0 = time.time()
        par_cell, parts, meta = construire(cells, cell_m, conf, nom, args.source_json)
        n = 0
        for (cx, cy) in cells:
            ecrire_cellule(cx, cy, par_cell.get((cx, cy), []), cell_m, conf, parts, nom)
            n += 1
            if n % 50 == 0 or n == len(cells):
                log("ecriture : %d / %d cellules" % (n, len(cells)))
        log("ecriture terminee en %.1f s" % (time.time() - t0))

    res = valider(cells, cell_m, nom, meta)
    path = os.path.join(OUT_DIR, "index_%s%s.json" % (nom, args.suffixe))
    res["cellSizeM"] = cell_m
    res["source"] = MILLESIME
    res["classes"] = "reseau_classes.json v%s sha1=%s" % (conf["version"], conf["_sha1"])
    res["cellules"] = [list(c) for c in cells]
    with open(path, "w", encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False, indent=1)
    log("VALIDATION %s : %d aretes, %d noeuds recolles, %.1f km, %d composantes "
        "(la plus grande %.2f %%), %d degenerees, coupes %d testees / %d orphelines, %.2f Mo"
        % (nom, res["edges"], res["noeuds_distincts_recolles"], res["longueur_km"],
           res["composantes"], res["pct_plus_grande"], res["aretes_degenerees"],
           res["recollement_coupes_testees"], res["recollement_coupes_orphelines"],
           res["volumetrie"]["total_Mo"]))
    log("km par classe : " + ", ".join("%s=%.1f" % (k, v)
                                       for k, v in res["km_par_classe"].items()))
    log("index ecrit : %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
