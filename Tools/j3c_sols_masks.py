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
#                  bordure), hors voirie privee, hors emprise batie, hors bout
#                  pendant de troncon, coupees aux traversees et aux ponts ;
#        crossings sites de PASSAGE PIETON — VIDE tant que CROSSINGS_ON est faux
#                  (couche INFEREE, hors doctrine : voir la constante) ;
#        axial     tirets de ligne axiale deja decoupes (3 m plein / 1,5 m vide),
#                  voies >= 2, ecartes de 8 m des carrefours.
#
# Les PONTS (position_par_rapport_au_sol > 0) sont EXCLUS du masque : ils restent
# les rubans/tabliers du generateur C++ — un pont ne se peint pas sur le sol.
#
# SOLVERT (2026-07-30) — L'HERBE EST LA 5e COUCHE DU MASQUE (canal R).
# Les films verts (meshes SM_Surface_* poses au-dessus de la dalle) sont abandonnes :
# l'herbe devient un ETAT DU SOL, decrit par un champ de distance comme la chaussee.
#   R = SDF signe HERBE (etait : identifiant de classe, que le shader ne lisait pas ;
#       la classe reste calculee pour l'apercu et le selftest, elle n'est plus cuite).
# Source vegetale : IGN OCS GE CS2.* ENTIER (ligneux + herbace — une pelouse sous les
# arbres d'un square est classee ligneuse, cf. work/SOLVERT/ocsge_eval.md), convertie
# en repere local par work/SOLVERT/solvert_prep.py -> SourceData/ocsge_verts.json.
#   herbe = union(CS2.*) - chaussee - privee - gravier - bati - eau
# L'herbe est LE COMPLEMENT de la voirie peinte : le debordement (grief 3) est
# structurellement impossible. Ouverture morphologique VECTORIELLE de 1 m
# (buffer -1/+1) : les languettes, croissants et fils meurent d'un coup (grief 4),
# operateur uniforme et national, sans parametre par ville. Les fosses d'arbres
# inventees (disques 4-6 m) ne sont PLUS generees (doctrine CROSSINGS_ON : « mieux
# vaut pas de marquage qu'un marquage invente »).
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
# SOLVERT : polygones vegetaux OCS GE CS2.* deja convertis en repere local (metres,
# x = est, y = sud). Produit par work/SOLVERT/solvert_prep.py depuis les GeoJSON MVT.
OCSGE_PATH = os.path.join(SRC, "ocsge_verts.json")
# FINITION_SOL : noeuds OSM `highway=crossing` deja filtres et convertis en repere
# local par Tools/fetch_osm_crossings.py (cache disque : zero fetch reseau en lot).
CROSSINGS_PATH = os.path.join(SRC, "Reseau", "osm_crossings_carre10.json")

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
CLS_HERBE = 5           # SOLVERT : sol vegetal OCS GE (apercu/selftest seulement)

# SOLVERT : rayon de l'ouverture morphologique vectorielle appliquee a l'herbe
# (buffer -r puis +r). 1 m efface toute structure de moins de 2 m de large — la
# languette le long d'un trottoir meurt, une vraie pelouse ne perd que son lisere.
# Le rayon d'influence (2 m) reste sous la marge de calcul des cellules (~3,9 m) :
# l'operation par cellule est identique a l'operation globale.
HERBE_OUVERTURE_M = 1.0

# --- HERBE : ACCOSTAGE et LISSAGE du bord (lot FINITION_SOL, 2026-08-01).
# Constat utilisateur : des bords d'herbe « en amibe » qui s'arretent n'importe ou
# sur le mineral. Cause : la frontiere d'un polygone OCS GE est une limite de RELEVE
# d'occupation du sol, pas une limite d'AMENAGEMENT — elle tombe a 40 cm du trottoir
# sans raison, et laisse un liere de dalle que rien ne justifie.
# ACCOSTAGE : quand l'herbe passe a moins de HERBE_ACCOSTAGE_M d'une frontiere
# minerale DEJA CONNUE DU BAKE (chaussee, voirie privee, gravier), on comble
# l'interstice — l'herbe vient mourir SUR la frontiere, exactement. C'est une
# fermeture morphologique de l'union herbe+mineral, restreinte a ce qui touche
# l'herbe : une regle globale, aucune nouvelle zone, aucune nouvelle donnee. Ailleurs
# (bord d'herbe en plein parc) rien n'est touche : c'est un bord organique legitime.
HERBE_ACCOSTAGE_M = 2.0
# LISSAGE : le masque cuit fait 48,83 cm par texel. Un gigotement de contour plus fin
# que ca n'est pas representable — c'est du bruit de releve qui ne produit que de
# l'escalier. On simplifie le contour juste au-dessus du plancher texel.
HERBE_SIMPLIFY_M = 0.55

# --- HERBE V2 (lot FINITION_SOL V2, 2026-08-01). Verdict utilisateur sur la v1 :
# « mieux, mais pas fini — trous, liseres le long des murs et des allees. On veut que
# ca epouse la forme parfaite jusqu'a la limite, mur ou bord de chaussee. »
#
# 1. ACCOSTAGE DES FACADES. La v1 accostait la chaussee, la voirie privee et le
#    gravier, mais pas le BATI — alors qu'un mur est la frontiere la plus dure qui
#    soit. `u_bati` etait deja dans la main de la fonction (seulement soustrait).
#    Mesure sur le km2 proto : +1 040 m2, exactement les liseres denonces.
#
# 2. TROUS INTERNES. Un trou dans le polygone d'herbe plus petit que ce seuil et qui
#    ne contient AUCUN objet connu du bake (chaussee, privee, gravier, BATI, eau) est
#    un trou de RELEVE, pas une clairiere : on le comble. Meme logique que le plancher
#    de motif de la charte du sol. Calibre par la mesure : le proto ne contient que
#    DEUX trous internes (52,6 et 50,8 m2) et tous deux contiennent un batiment —
#    la regle ne tire donc AUCUN coup ici, et c'est le resultat correct (combler un
#    trou de batiment mettrait de la pelouse sur un toit). Le seuil est pose au-dessus
#    de ces deux cas mesures pour rester utile ailleurs qu'au centre de Toulouse.
HERBE_TROU_M2 = 50.0
#
# 3. REGLE DE COMPARTIMENT — la reponse a « epouser la forme parfaite ».
#    On partitionne l'espace en ILOTS delimites par des frontieres REELLES et par
#    elles seules : chaussee, voirie privee, gravier, emprise batie, la frontiere du
#    CORRIDOR (ce qui separe le trottoir public de l'interieur d'une parcelle) et la
#    LIMITE DE PARCELLE cadastrale — un parc EST une parcelle, et c'est jusqu'a SA
#    limite que l'herbe doit aller. Un ilot dont l'herbe couvre deja au moins
#    HERBE_COMPART_TAUX est peint jusqu'a ses limites ; les autres ne bougent pas.
#
#    CALIBRATION MESUREE (km2 proto, 4 cellules) — la parcelle change tout :
#      variante            0,60      0,65      0,70      0,75      0,80      0,90
#      sans parcelle    4 539 m2  2 009 m2    521 m2    521 m2    340 m2    208 m2
#      AVEC parcelle    3 863 m2  3 750 m2  2 321 m2  2 099 m2  1 449 m2    384 m2
#    Sans la parcelle la regle ne fait presque rien (521 m2) ; avec, elle complete
#    chaque parcelle deja majoritairement verte (2 321 m2) — c'est ce que montre le
#    rendu de diagnostic (work/FINITION_SOL/diag/variante_*.png) : des liseres et des
#    encoches qui ferment la forme, pas des blocs.
#    Le seuil est pose a 0,70 par TROIS mesures concordantes :
#      a. sous 0,65, le remplissage attrape des COURS BATIES entieres (verifie a
#         l'oeil sur le rendu : la tache prend la forme decoupee des cours) ;
#      b. les allees : sous 0,65 la regle mangerait 53 m de sentiers BD TOPO, a 0,70
#         elle n'en mange AUCUN ;
#      c. la distribution des taux par ilot (famille hors-corridor, >= 200 m2) est
#         quasi vide entre 0,62 et 0,69 : le seuil n'est pas sensible a +/- 5 %.
HERBE_COMPART_TAUX = 0.70
#    Un ilot plus petit que ca n'est pas un compartiment, c'est une miette de decoupe.
HERBE_COMPART_AIRE_MIN_M2 = 20.0
#    HALO : l'ilot est evalue sur une fenetre ELARGIE, pas sur le fragment que la
#    cellule en voit. Sans ce halo, un parc a cheval sur deux cellules recoit deux
#    taux differents et donc deux decisions differentes -> une COUTURE rectiligne
#    d'herbe sur la grille de 500 m. Mesure du piege : l'ilot [-317,-452] passe de
#    0,710 (tronque par la cellule) a moins de 0,70 (evalue en entier) — la decision
#    basculait. 80 m couvre un ilot urbain francais typique.
HERBE_COMPART_HALO_M = 80.0
#    Epaisseur du fil retire aux limites de parcelle pour separer les ilots, puis
#    rendu au remplissage : 1 cm, soit 1/49e de texel — invisible a la cuisson.
HERBE_COMPART_FIL_M = 0.01

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
# --- LIGNE AXIALE : REGLE NATIONALE « AYANTS DROIT » (lot FINITION_SOL V2).
# BD TOPO `acces_vehicule_leger` decrit qui a le droit de rouler la. « Restreint aux
# ayants droit » = rue pietonne a acces riverains/livraisons (le centre de Toulouse en
# est fait : rue d'Alsace-Lorraine, rue de la Pomme, rue Saint-Rome...). On n'y trace
# PAS de ligne axiale : une voie ou le pieton est chez lui n'a pas d'axe de
# circulation peint. La regle est NATIONALE (un attribut BD TOPO, pas une liste de
# rues) et se pose au bake, dans la liste `axial` — aucun C++.
# Mesure sur le km2 proto : 53 troncons « Restreint aux ayants droit » sur 509, dont
# 16 portaient des tirets (859 m d'axe). Les 456 troncons « Libre » ne bougent pas.
ACCES_SANS_AXIALE = {"restreint aux ayants droit"}
# Deux sentiers OSM traversent souvent la meme rue a quelques metres : un seul
# passage pieton par rayon. Un vrai carrefour garde ses passages (un par branche,
# separes de plus que ca).
CROSS_DEDUP_M = 7.0
CROSS_HALF_LEN_M = 2.0  # demi-longueur du passage dans l'axe de la rue (GCrossingHalfLenCm)
# Un noeud OSM `highway=crossing` est pose SUR l'axe. On refuse le site si l'axe le
# plus proche est plus loin que ca : le noeud ne parle alors pas de CETTE chaussee
# (traversee d'un chemin, d'une voie ferree, d'un parking...).
CROSS_SNAP_M = 6.0
# Le quad pose par le C++ mesure 2 x CROSS_HALF_LEN_M dans l'axe et 2 x halfW en
# travers. Il doit tomber SUR de l'asphalte : on exige cette fraction de son aire
# dans la chaussee. C'est le garde-fou qui manquait a la v1 (375/383 debordaient).
CROSS_MIN_DEDANS = 0.90
CURB_MIN_LEN_M = 1.5    # une bordure plus courte que ca est un artefact de decoupe
CURB_SIMPLIFY_M = 0.15
# --- BORDURES : les deux familles d'artefacts, mesurees le 2026-08-01 (lot
# FINITION_SOL). Constat utilisateur : « pierres de bordure isolees en travers de
# l'asphalte ou au milieu des places ». Deux causes DISTINCTES, deux regles globales.
#
# 1. POINTE DEGENEREE. Deux troncons consecutifs partagent un noeud, mais leurs
#    buffers a bout PLAT (cap_style=2) n'y sont pas colinaires : ~2 deg d'ecart de
#    cap sur 4,5 m de demi-largeur laissent un coin de ~17 cm. `simplify` l'ecrase a
#    largeur nulle -> la polyligne part vers l'axe et revient sur elle-meme, et le
#    C++ en fait DEUX murs de 12 cm dos a dos EN TRAVERS de la rue. Mesure sur le km2
#    proto : 156 pointes, 744,8 m, ecart median entre les deux branches 2 cm.
#    Regle : une bordure ne rebrousse pas chemin. On retire toute excursion plus
#    MINCE que le texel du masque — rien de plus fin n'est une forme de la ville
#    (charte du sol). Calibre : 150 pointes sous 0,4883 m, puis un trou net jusqu'a
#    0,878 m — au-dela ce sont de vraies epingles (contour de terre-plein), gardees.
CURB_POINTE_LARGEUR_M = CELL_M / OUT_PX      # 48,83 cm : le texel du masque
CURB_POINTE_ANGLE_DEG = 30.0                 # rebroussement franc
# 2. FRONTIERE INTERNE. Une bordure est la FRONTIERE de la chaussee : elle est AU
#    BORD. Ce qui est ENFONCE dans l'union des chaussees est une frontiere de DONNEE
#    sans contrepartie visuelle (coupe cadastrale au milieu de la rue : cas mesure de
#    47 m de « bordure » a 28 cm de l'axe d'une rue de 4 m). Mesure : 260,5 m
#    (0,63 %) au-dela du texel. ATTENTION : c'est l'enfoncement dans l'UNION qu'il
#    faut mesurer, pas dans chaque bande — par bande, les coudes (raccord mitre) font
#    des faux positifs (un cas mesure a 0,73 m dedans sa voisine mais a 4 cm du bord
#    reel de la chaussee).
CURB_DEDANS_M = CELL_M / OUT_PX              # meme plancher physique : le texel

# --- PASSAGES PIETONS : REBRANCHES SUR LA DONNEE REELLE (2026-08-01).
# HISTOIRE : la couche a ete coupee le 2026-07-30 parce que ses sites etaient
# DEVINES — « un axe pieton OSM croise la chaussee, donc il y avait probablement un
# passage la ». C'etait la seule couche inferee du pipeline, et la mesure lui a donne
# tort : sur le km2 proto, 375 des 383 sites debordaient de la chaussee et 32
# mordaient un batiment. Doctrine gravee : « mieux vaut pas de marquage qu'un
# marquage invente ».
# CE QUI CHANGE : la SOURCE, pas le principe. Les sites viennent desormais des
# noeuds OSM `highway=crossing` (Tools/fetch_osm_crossings.py, cache disque) : un
# noeud est pose SUR l'axe de la chaussee, a l'endroit exact du passage, et OSM dit
# lui-meme s'il est marque. On ne peint QUE la ou la donnee affirme un marquage ;
# `crossing:markings=no` -> RIEN. La sous-couverture des petites communes est
# acceptee : la degradation est silencieuse et sure, jamais un passage invente.
# Les garde-fous mesures de la v1 RESTENT (c'est ce qui distingue ce rebranchement
# d'un retour en arriere) : dedoublonnage, quad entierement dans la chaussee, rejet
# sur emprise batie.
#
# --- COUPE A NOUVEAU LE 2026-08-01 (V2), SUR VERDICT UTILISATEUR SUR CAPTURES.
# « Soit partiels, soit mal placés. » Les 122 passages etaient poses sur de la vraie
# donnee OSM, dans l'axe, sans en inventer un seul — et ils rendaient quand meme mal.
# Trois causes admises : (1) la LARGEUR vient du troncon BD TOPO, pas de la chaussee
# reellement peinte, donc le passage ne va pas de bordure a bordure ; (2) la TEINTE du
# pack `pedestrian_crossing_lines_veggecd` est terracotta et grise, or un passage
# pieton francais est BLANC ; (3) le proto est le coeur PIETON de Toulouse : sur un
# sol visuellement continu, un passage n'a rien a traverser.
# TOUT LE MECANISME EST CONSERVE (fetch_osm_crossings.py + son cache, les 4 garde-fous
# de crossing_sites, quad_de_passage, la coupe de bordure, les verrous de self-test) :
# seul le drapeau tombe. LES TROIS CONDITIONS DU REBRANCHEMENT, au chantier
# props/marquages :
#   a. peinture BLANCHE — le materiau `marking` deja utilise par les tirets axiaux,
#      pas un pack de texture teinte ;
#   b. PLEINE LARGEUR bordure-a-bordure — la demi-largeur doit se mesurer sur la
#      geometrie de chaussee cuite, pas sur l'attribut de largeur du troncon ;
#   c. UNIQUEMENT sur chaussee roulable non ambigue — jamais en zone de rencontre ni
#      sur un plateau pieton, ou la traversee n'a pas de sens.
CROSSINGS_ON = False
# Emprises baties : buffer applique a l'union avant de la retirer du corridor. 20 cm
# absorbent le desaccord de calage entre le cadastre (parcelles) et le bati, sans
# manger de trottoir reel.
BATI_BUFFER_M = 0.20
# Bordures : marge SUPPLEMENTAIRE autour du bati ou aucune bordure ne se pose (soit
# 50 cm au total depuis la facade). Une bordure qui longe un mur est un artefact de
# la decoupe par emprise, pas une marche de la ville.
BATI_CURB_CLEAR_M = 0.30
# Bordures : rayon du disque de silence autour d'une extremite PENDANTE de troncon
# (largeur/2 + cette marge). Le bout PLAT du buffer d'axe (cap_style=2) ferme la
# chaussee en travers de la rue ; sans ce disque, curb_lines y poserait une marche de
# 12 cm perpendiculaire a la voie, qui se lit comme un mur. C'est NOTRE artefact de
# decoupe, pas une donnee de la ville : on le retire, et rien d'autre — la voie
# s'arrete parce que la donnee dit qu'elle s'arrete.
CURB_DANGLE_CLEAR_M = 0.5


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
        acces = C.norm(tr.get("acces_vehicule_leger"))
        if (acces == "physiquement impossible"
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
            # Qui a le droit de rouler la : sert a la regle nationale de ligne axiale
            # (ACCES_SANS_AXIALE). Normalise en ASCII minuscule comme la nature.
            "acces": acces,
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


def charger_verts_ocsge(fen):
    """Polygones vegetaux OCS GE CS2.* (SourceData/ocsge_verts.json, deja en repere
    local). C'est le SOCLE national de la couche herbe — partition sans trou, UMC
    500 m2 (cf. work/SOLVERT/ocsge_eval.md). Retourne aussi la fraction de la
    fenetre couverte par OCS GE toutes classes confondues : si elle est faible, la
    fenetre depasse l'AOI recuperee et il faut refetcher (MVT, 4 pieges documentes)."""
    if not os.path.exists(OCSGE_PATH):
        log("ATTENTION : %s absent, la couche HERBE du masque sera VIDE" % OCSGE_PATH)
        return []
    with open(OCSGE_PATH, encoding="utf-8") as f:
        data = json.load(f)
    out = []
    n_lus = 0
    for rec in data.get("cs2", []):
        n_lus += 1
        pts = rec.get("pts") or []
        if len(pts) < 3 or not C.bbox_hit(C.bbox_of(pts), *fen):
            continue
        holes = [h for h in (rec.get("holes") or []) if len(h) >= 3]
        try:
            g = C.valide(Polygon(pts, holes))
        except Exception:
            continue
        if not g.is_empty:
            out.append(g)
    log("verts OCS GE : %d polygones CS2.* lus, %d dans la fenetre" % (n_lus, len(out)))
    return out


def charger_axes_pietons(fen):
    """Axes pietons OSM (toulouse10.json) : ils ne peignent rien. Ils servaient de
    source DEVINEE aux passages pietons (couche coupee le 30/07) — la source est
    desormais charger_crossings_osm. Conserve : lecture de reference, et repli si
    l'on veut un jour re-mesurer l'ancien chemin."""
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


def charger_crossings_osm(fen):
    """Noeuds OSM `highway=crossing` DEJA FILTRES par Tools/fetch_osm_crossings.py :
    seuls remontent ceux dont OSM AFFIRME le marquage au sol (`peint`). Le fichier
    est un cache disque (zero fetch reseau en lot). Rend la liste des positions.

    C'est LA source de verite des passages : une donnee reelle, posee sur l'axe, avec
    son propre aveu de marquage. Rien n'est infere ici."""
    if not os.path.exists(CROSSINGS_PATH):
        log("ATTENTION : %s absent -> AUCUN passage pieton. Lancer "
            "`python fetch_osm_crossings.py --half 5000 --nom carre10`" % CROSSINGS_PATH)
        return []
    with open(CROSSINGS_PATH, encoding="utf-8") as f:
        data = json.load(f)
    tot = peints = dedans = 0
    out = []
    for n in data.get("noeuds", []):
        tot += 1
        if not n.get("peint"):
            continue
        peints += 1
        p = n.get("p") or []
        if len(p) < 2:
            continue
        if not (fen[0] <= p[0] <= fen[2] and fen[1] <= p[1] <= fen[3]):
            continue
        dedans += 1
        out.append((float(p[0]), float(p[1])))
    log("passages OSM : %d noeuds lus, %d peints (%d refuses par le filtre de "
        "marquage), %d dans la fenetre" % (tot, peints, tot - peints, dedans))
    return out


# ------------------------------------------------------------------- une cellule
def classes_de_cellule(zone, parcelles, eaux, routes, batis=None):
    """corridor / chaussee / privee / gravier / u_bati d'une emprise donnee (deja
    elargie de la marge). Meme soustraction que le corridor valide, plus la
    separation des natures etroites (gravier) et des bandes tombees dans une parcelle
    (privee), plus le retrait des EMPRISES BATIES.

    Le BATI dans le corridor : mesure sur le km2 proto, 5 612 m2 de corridor (2,2 %)
    et 409 m2 de chaussee (0,4 %) tombaient SOUS des immeubles — le cadastre ne
    couvre pas tout (14 batiments entierement en zone non cadastree), donc la
    soustraction zone - parcelles - eau laissait du trottoir peint sous les murs. Une
    emprise batie est un polygone REEL, exactement comme une parcelle : elle a sa
    place dans la soustraction.

    EXCEPTION PORCHE : la ou l'AXE ROUTIER LUI-MEME passe sous un batiment, la rue
    passe VRAIMENT dessous (porche, passage couvert, arcade — Toulouse en est plein :
    48 m d'axes sur 23 366, soit 0,21 %). Trouer la chaussee la-dessous casserait la
    rue en deux. On rend donc au corridor la bande de chaussee des porches."""
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

    u_bati = None
    masque_bati = None      # le bati MOINS les porches : ce qui ne se peint jamais
    if batis:
        u_bati = C.valide(unary_union([b.buffer(BATI_BUFFER_M) for b in batis]))
        if u_bati.is_empty:
            u_bati = None
    if u_bati is not None:
        masque_bati = u_bati
        avant_bati = corridor                       # corridor cadastral, sans le bati
        corridor = C.valide(corridor.difference(u_bati))
        # Les porches, bande par bande : le morceau d'axe reellement sous un
        # batiment, bufferise a la largeur MESUREE de son propre troncon (meme
        # buffer que les bandes de chaussee : bout plat, raccord mitre).
        porches = []
        for r in routes:
            if r["pont"] or r["largeur"] <= 0:
                continue
            try:
                sous = r["line"].intersection(u_bati)
            except Exception:
                continue
            if sous.is_empty or sous.length <= 0.0:
                continue
            porches.append(sous.buffer(r["largeur"] / 2.0, cap_style=2, join_style=2,
                                       mitre_limit=3.0))
        if porches:
            # ... rendus au corridor, mais SEULEMENT dans ce qui etait deja du
            # corridor cadastral : un porche ne cree pas de rue la ou le cadastre
            # dit « parcelle privee ».
            rendu = C.valide(unary_union(porches).intersection(avant_bati))
            if not rendu.is_empty:
                corridor = C.valide(corridor.union(rendu))
                masque_bati = C.valide(u_bati.difference(rendu))

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
    if masque_bati is not None:
        # gravier et privee se calculent sur la ZONE (une cour, une allee de
        # residence sont dans une parcelle, donc hors corridor par construction) :
        # sans ce retrait, tout ce que le corridor vient de perdre sous les murs
        # reviendrait peint en voirie PRIVEE. Sous un batiment, on ne peint RIEN
        # (classe 0) — sauf le porche, deja rendu au corridor.
        gravier = C.valide(gravier.difference(masque_bati))
        privee = C.valide(privee.difference(masque_bati))
    # u_bati est rendu TEL QUEL (porches compris) : c'est curb_lines qui s'en sert
    # comme zone de silence, et une bordure ne se pose pas plus le long d'un mur de
    # porche que le long d'une facade.
    return corridor, chaussee, privee, gravier, u_bati


def compartiments(zone_eval, corridor, chaussee, privee, gravier, u_bati, parcelles):
    """ILOTS de la regle de compartiment (herbe v2) : les composantes connexes de
    l'espace libre, decoupe par des frontieres REELLES et par elles seules.

    Bornes, dans l'ordre ou elles sont posees :
      - chaussee, voirie privee, gravier, emprise batie : ce qui est deja peint ou
        bati ne fait partie d'aucun compartiment ;
      - la frontiere du CORRIDOR : elle separe le trottoir PUBLIC de l'interieur des
        parcelles. Sans elle, un parc et le trottoir qui en fait le tour forment un
        seul ilot, et remplir l'ilot mettrait de la pelouse sur le trottoir ;
      - la LIMITE DE PARCELLE cadastrale : un parc EST une parcelle. C'est la borne
        qui fait toute la difference a la mesure (521 m2 sans, 2 321 m2 avec).

    Rend (ilots, libre) : `libre` sert a re-fermer les fils de 1 cm ouverts aux
    limites de parcelle une fois les ilots retenus."""
    bornes = [g for g in (chaussee, privee, gravier, u_bati)
              if g is not None and not g.is_empty]
    libre = C.valide(zone_eval.difference(unary_union(bornes))) if bornes else zone_eval
    if libre.is_empty:
        return [], libre
    fams = []
    if corridor is not None and not corridor.is_empty:
        fams.append(C.valide(libre.intersection(corridor)))
        hors = C.valide(libre.difference(corridor))
    else:
        hors = libre
    if parcelles:
        try:
            fil = unary_union([p.boundary for p in parcelles]).buffer(HERBE_COMPART_FIL_M)
            hors = C.valide(hors.difference(fil))
        except Exception:
            pass
    fams.append(hors)
    ilots = []
    for fam in fams:
        if fam is None or fam.is_empty:
            continue
        for isl in getattr(fam, "geoms", [fam]):
            if (isl.is_empty or isl.geom_type != "Polygon"
                    or isl.area < HERBE_COMPART_AIRE_MIN_M2):
                continue
            ilots.append(isl)
    return ilots, libre


def remplir_compartiments(herbe, herbe_ref, ilots, libre):
    """REGLE DE COMPARTIMENT : un ilot dont l'herbe couvre deja au moins
    HERBE_COMPART_TAUX de sa surface est peint herbe JUSQU'A SES LIMITES ; un ilot
    minoritaire (une place, une cour, un parvis avec deux carres de pelouse) n'est
    pas touche du tout.

    Le taux se mesure sur `herbe_ref` — l'herbe BRUTE issue du releve, avant
    accostage et avant remplissage : sinon la regle se nourrirait de son propre
    resultat d'une passe a l'autre. `herbe_ref` est calculee sur la fenetre ELARGIE
    (halo), donc l'ilot est juge sur son extension reelle et deux cellules voisines
    prennent la MEME decision (pas de couture sur la grille de 500 m)."""
    remplir_compartiments.dernier = {"ilots": 0, "remplis": 0, "gain_m2": 0.0}
    if herbe is None or not ilots:
        return herbe
    gardes = []
    for isl in ilots:
        a = isl.area
        if a <= 0.0:
            continue
        try:
            h = isl.intersection(herbe_ref).area if not herbe_ref.is_empty else 0.0
        except Exception:
            continue
        if h >= HERBE_COMPART_TAUX * a:
            gardes.append(isl)
    remplir_compartiments.dernier["ilots"] = len(ilots)
    remplir_compartiments.dernier["remplis"] = len(gardes)
    if not gardes:
        return herbe
    u = C.valide(unary_union(gardes))
    # Les fils de 1 cm ouverts aux limites de parcelle sont rendus : deux parcelles
    # voisines toutes deux retenues doivent redevenir jointives. Le buffer est borne
    # par `libre`, donc il ne peut pas mordre une frontiere reelle.
    u = C.valide(u.buffer(HERBE_COMPART_FIL_M * 1.5).intersection(libre))
    gain = u.difference(herbe).area if not herbe.is_empty else u.area
    remplir_compartiments.dernier["gain_m2"] = round(gain, 1)
    return C.valide(herbe.union(u)) if not herbe.is_empty else u


def boucher_trous(herbe, obstacles, aire_max=None):
    """Comble les TROUS INTERNES du polygone d'herbe plus petits que `aire_max` qui
    ne contiennent AUCUN objet connu du bake. Un trou de releve n'est pas une
    clairiere : sous le plancher de motif, il ne produit qu'un confetti de dalle au
    milieu d'une pelouse. Un trou qui contient un batiment, une chaussee, du gravier
    ou de l'eau est REEL et reste ouvert — c'est le seul cas present sur le proto."""
    boucher_trous.dernier = {"trous": 0, "combles": 0, "aire_m2": 0.0}
    amax = HERBE_TROU_M2 if aire_max is None else aire_max
    if herbe is None or herbe.is_empty or amax <= 0.0:
        return herbe
    combles = []
    for poly in getattr(herbe, "geoms", [herbe]):
        if poly.is_empty or poly.geom_type != "Polygon":
            continue
        for ring in poly.interiors:
            try:
                t = Polygon(ring)
            except Exception:
                continue
            if t.is_empty or t.area <= 0.0:
                continue
            boucher_trous.dernier["trous"] += 1
            if t.area > amax:
                continue
            if (obstacles is not None and not obstacles.is_empty
                    and t.intersects(obstacles)):
                continue
            combles.append(t)
    if combles:
        u = C.valide(unary_union(combles))
        boucher_trous.dernier["combles"] = len(combles)
        boucher_trous.dernier["aire_m2"] = round(u.area, 1)
        herbe = C.valide(herbe.union(u))
    return herbe


def accoster_herbe(herbe, chaussee, privee, gravier, u_bati=None, u_eau=None):
    """ACCOSTAGE + BOUCHAGE + LISSAGE du bord d'herbe (lot FINITION_SOL, v2).

    1. ACCOSTAGE. Une frontiere OCS GE est une limite de RELEVE, pas d'amenagement :
       elle s'arrete a 40 cm du trottoir sans raison et laisse un lisere de dalle que
       rien ne justifie. On FERME les interstices plus etroits que HERBE_ACCOSTAGE_M
       entre l'herbe et le mineral DEJA CONNU DU BAKE — chaussee, privee, gravier, et
       depuis la v2 les FACADES (`u_bati`) : un mur est la frontiere la plus dure qui
       soit, et le lisere le long des murs etait le premier grief utilisateur.
       L'herbe vient alors mourir exactement SUR la frontiere. Fermeture morphologique
       de l'union herbe+mineral, puis on ne garde du comblement que ce qui TOUCHE
       l'herbe : un interstice entre deux morceaux de mineral n'a rien a faire en
       herbe.
       Ailleurs — bord d'herbe en plein parc, loin de tout mineral — rien n'est
       touche : c'est un bord organique legitime.
    2. TROUS INTERNES (v2) : cf. boucher_trous.
    3. LISSAGE. Le masque cuit fait 48,83 cm par texel : un gigotement de contour
       plus fin n'est pas representable, il ne produit que de l'escalier. On
       simplifie juste au-dessus du plancher texel.
    4. L'herbe reste le COMPLEMENT STRICT du mineral peint et du bati : on
       re-soustrait a la fin (le lissage pourrait sinon mordre de quelques
       centimetres). C'est le garde-fou anti-debordement, et il est structurel.
    Des regles globales, aucune nouvelle zone, aucune nouvelle donnee."""
    if herbe is None or herbe.is_empty:
        return herbe
    # Le mineral qui SERT DE QUAI a l'accostage : tout ce qui est une frontiere dure
    # deja connue du bake, bati compris.
    quais = [g for g in (chaussee, privee, gravier, u_bati)
             if g is not None and not g.is_empty]
    quai = C.valide(unary_union(quais)) if quais else None
    # Le mineral qu'on RE-SOUSTRAIT a la fin : le peint (le bati est traite a part,
    # il a sa propre soustraction, et l'eau n'est jamais rendue a l'herbe).
    peints = [g for g in (chaussee, privee, gravier) if g is not None and not g.is_empty]
    mineral = C.valide(unary_union(peints)) if peints else None
    if quai is not None and not quai.is_empty and HERBE_ACCOSTAGE_M > 0.0:
        r = HERBE_ACCOSTAGE_M * 0.5
        u = C.valide(unary_union([herbe, quai]))
        ferme = C.valide(u.buffer(r).buffer(-r))
        comble = C.valide(ferme.difference(u))
        if not comble.is_empty:
            # On garde les interstices ENTIERS qui touchent l'herbe (jamais un bout :
            # un comblement partiel ne ferait que deplacer la frontiere en amibe).
            gardes = [g for g in getattr(comble, "geoms", [comble])
                      if not g.is_empty and g.area > 1e-6 and g.distance(herbe) < 1e-6]
            if gardes:
                herbe = C.valide(herbe.union(C.valide(unary_union(gardes))))
    obstacles = [g for g in (quai, u_eau) if g is not None and not g.is_empty]
    herbe = boucher_trous(herbe, C.valide(unary_union(obstacles)) if obstacles else None)
    if HERBE_SIMPLIFY_M > 0.0 and not herbe.is_empty:
        herbe = C.valide(herbe.simplify(HERBE_SIMPLIFY_M, preserve_topology=True))
    # L'herbe reste le COMPLEMENT STRICT de ce qui est peint, bati ou en eau.
    for g in (mineral, u_bati, u_eau):
        if g is not None and not g.is_empty and not herbe.is_empty:
            herbe = C.valide(herbe.difference(g))
    return herbe


def dangling_ends(routes):
    """Extremites PENDANTES : bout de troncon non-pont partage par UN SEUL troncon.

    Meme cle arrondie au decimetre que junction_points (qui, lui, cherche n >= 3) —
    ici n == 1, c'est-a-dire la vraie impasse (entree de cour, cul-de-sac) ou le bout
    d'une voie que rien ne prolonge. On rend aussi la largeur du troncon concerne :
    c'est elle qui donne le rayon du disque de silence des bordures."""
    compte = {}
    larg = {}
    for r in routes:
        if r["pont"]:
            continue
        cs = list(r["line"].coords)
        for p in (cs[0], cs[-1]):
            k = (round(p[0] * 10), round(p[1] * 10))
            compte[k] = compte.get(k, 0) + 1
            larg[k] = max(larg.get(k, 0.0), float(r["largeur"] or 0.0))
    return [((k[0] / 10.0, k[1] / 10.0), larg[k]) for k, n in compte.items() if n == 1]


def retirer_pointes(cs, largeur_min=None, angle_max=None):
    """Retire les EXCURSIONS DEGENEREES d'une polyligne : les aller-retour plus
    MINCES que largeur_min. Une bordure ne rebrousse pas chemin — quand elle le fait,
    c'est la langue de largeur nulle laissee par deux buffers de troncons consecutifs
    au nœud partage, ecrasee par simplify (cf. CURB_POINTE_LARGEUR_M).

    Largeur de l'excursion A->B->C = 2 * aire(ABC) / branche la plus longue, soit la
    distance entre les deux branches. Nulle pour une pointe, egale a la largeur du
    terre-plein pour une VRAIE epingle (qu'on garde). Itere jusqu'a stabilite : une
    pointe peut en cacher une autre."""
    lmin = CURB_POINTE_LARGEUR_M if largeur_min is None else largeur_min
    amax = CURB_POINTE_ANGLE_DEG if angle_max is None else angle_max
    cs = list(cs)
    change = True
    n_ret = 0
    while change and len(cs) >= 3:
        change = False
        i = 1
        while i < len(cs) - 1:
            ax, ay = cs[i - 1]
            bx, by = cs[i]
            cx2, cy2 = cs[i + 1]
            ux, uy = ax - bx, ay - by
            vx, vy = cx2 - bx, cy2 - by
            lu = math.hypot(ux, uy)
            lv = math.hypot(vx, vy)
            if lu < 1e-9 or lv < 1e-9:
                del cs[i]
                change = True
                n_ret += 1
                continue
            cosang = (ux * vx + uy * vy) / (lu * lv)
            ang = math.degrees(math.acos(max(-1.0, min(1.0, cosang))))
            larg = abs(ux * vy - uy * vx) / max(lu, lv)      # 2*aire / plus longue
            if ang <= amax and larg < lmin:
                del cs[i]
                change = True
                n_ret += 1
                continue
            i += 1
    retirer_pointes.dernier = n_ret
    return cs


def curb_lines(chaussee, cell_box, routes, crossings, u_bati=None):
    """Polylignes de bordure : la frontiere de la chaussee, PRIVEE de ce qui n'a pas
    de bordure (autoroutier, pont, emprise batie, bout pendant de troncon, INTERIEUR
    de la chaussee) et de ce qui l'interrompt (les traversees), puis ramenee a la
    cellule, DEPOINTEE et ORIENTEE chaussee a gauche."""
    if chaussee.is_empty:
        return []
    bnd = chaussee.boundary
    coupes = []
    # FRONTIERE INTERNE (lot FINITION_SOL) : une bordure est AU BORD de la chaussee.
    # Ce qui est enfonce de plus de CURB_DEDANS_M dans l'UNION des bandes roulables
    # est une frontiere de DONNEE (coupe cadastrale au milieu de la rue, langue entre
    # deux buffers) : aucune contrepartie visuelle, on ne la maille pas. L'erosion se
    # fait sur l'UNION et non bande par bande — par bande, les raccords mitre des
    # coudes font des faux positifs (mesure).
    bandes = [r["line"].buffer(r["largeur"] / 2.0, cap_style=2, join_style=2, mitre_limit=3.0)
              for r in routes if not r["pont"] and not r["etroit"] and r["largeur"] > 0]
    if bandes:
        interieur = C.valide(C.valide(unary_union(bandes)).buffer(-CURB_DEDANS_M))
        if not interieur.is_empty:
            coupes.append(interieur)
    for r in routes:
        if r["pont"] or r["sans_bordure"]:
            coupes.append(r["line"].buffer(r["largeur"] / 2.0 + 1.0, cap_style=2, join_style=2))
    # Depuis que le corridor connait le bati, la frontiere de la chaussee longe des
    # FACADES : sans cette coupe, on maillerait une marche de 12 cm le long des murs.
    if u_bati is not None and not u_bati.is_empty:
        coupes.append(u_bati.buffer(BATI_CURB_CLEAR_M))
    # Bout PLAT du buffer d'axe a une extremite pendante = frontiere de chaussee EN
    # TRAVERS de la rue. Artefact de notre decoupe : on le fait taire, sans rien
    # poser a la place.
    for p, w in dangling_ends(routes):
        coupes.append(Point(p).buffer(max(w, 0.0) / 2.0 + CURB_DANGLE_CLEAR_M))
    for cr in crossings:
        # meme geometrie que le quad maille, elargie de 60 cm en travers (jusqu'au
        # pied du trottoir) et de 40 cm dans l'axe : la bordure s'interrompt AU
        # passage, comme un vrai bateau.
        coupes.append(quad_de_passage(cr["p"][0], cr["p"][1], cr["d"][0], cr["d"][1],
                                      cr["halfW"], along=CROSS_HALF_LEN_M + 0.4,
                                      marge_travers=0.6))
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
        # POINTES DEGENEREES : c'est simplify qui les CREE (il ecrase la langue de
        # 17 cm a largeur nulle), donc on depointe APRES lui, jamais avant.
        cs = retirer_pointes(list(s.coords))
        if len(cs) < 2:
            continue
        s = LineString(cs)
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


def quad_de_passage(px, py, dx, dy, halfW, along=None, marge_travers=0.0):
    """Le quad que BuildCrossing posera : 2 x along dans l'axe, 2 x halfW en travers.
    Une seule ecriture de cette geometrie, partagee par le filtre et par curb_lines —
    sinon le garde-fou testerait une autre forme que celle qui sera maillee."""
    a = CROSS_HALF_LEN_M if along is None else along
    h = halfW + marge_travers
    nx, ny = -dy, dx
    return Polygon([
        (px - dx * a - nx * h, py - dy * a - ny * h),
        (px + dx * a - nx * h, py + dy * a - ny * h),
        (px + dx * a + nx * h, py + dy * a + ny * h),
        (px - dx * a + nx * h, py - dy * a + ny * h)])


def crossing_sites(chaussee, cell_box, routes, noeuds, u_bati=None):
    """Un site par NOEUD OSM `highway=crossing` MARQUE (source reelle, deja filtree
    par fetch_osm_crossings.py). Le noeud donne la position ; l'axe BD TOPO le plus
    proche donne l'orientation et la demi-largeur — le quad se pose dans l'axe de la
    rue, exactement comme BuildCrossing le maillera.

    GARDE-FOUS (ce qui distingue ce rebranchement du retour en arriere) :
      a. l'axe le plus proche doit etre a moins de CROSS_SNAP_M (un noeud OSM est
         pose SUR l'axe : plus loin, il parle d'autre chose) ;
      b. le quad doit tomber a au moins CROSS_MIN_DEDANS de son aire DANS la
         chaussee — c'est ce qui manquait a la v1 (375 des 383 sites debordaient) ;
      c. le quad ne doit mordre AUCUNE emprise batie ;
      d. dedoublonnage glouton a CROSS_DEDUP_M, la rue la plus large d'abord."""
    if chaussee.is_empty or not noeuds:
        return []
    autos = [r for r in routes if not r["pont"] and not r["etroit"] and r["largeur"] > 0]
    if not autos:
        return []
    pre = prep(chaussee)
    sites = []
    rejets = {"hors_cellule": 0, "sans_axe": 0, "deborde": 0, "bati": 0}
    for (nx_, ny_) in noeuds:
        p = Point(nx_, ny_)
        if not cell_box.contains(p):
            rejets["hors_cellule"] += 1
            continue
        best = None
        for r in autos:
            d = r["line"].distance(p)
            if best is None or d < best[0]:
                best = (d, r)
        if best is None or best[0] > CROSS_SNAP_M:
            rejets["sans_axe"] += 1
            continue
        r = best[1]
        # SNAP sur l'axe : le quad doit etre centre sur la chaussee, pas sur la
        # position brute du noeud (OSM la pose sur l'axe, mais a quelques cm pres).
        s = r["line"].project(p)
        q = r["line"].interpolate(s)
        a = r["line"].interpolate(max(0.0, s - 2.0))
        b = r["line"].interpolate(min(r["line"].length, s + 2.0))
        dx, dy = b.x - a.x, b.y - a.y
        n = math.hypot(dx, dy)
        if n < 1e-6:
            rejets["sans_axe"] += 1
            continue
        dx, dy = dx / n, dy / n
        halfW = r["largeur"] * 0.5
        quad = quad_de_passage(q.x, q.y, dx, dy, halfW)
        if quad.area <= 0.0:
            rejets["deborde"] += 1
            continue
        if not pre.contains(quad):
            try:
                dedans = quad.intersection(chaussee).area / quad.area
            except Exception:
                dedans = 0.0
            if dedans < CROSS_MIN_DEDANS:
                rejets["deborde"] += 1
                continue
        if u_bati is not None and not u_bati.is_empty and quad.intersects(u_bati):
            rejets["bati"] += 1
            continue
        sites.append({"p": (round(q.x, 2), round(q.y, 2)),
                      "d": (round(dx, 5), round(dy, 5)),
                      "halfW": round(halfW, 3)})
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
    crossing_sites.rejets = dict(rejets, doublons=len(sites) - len(gardes))
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
    carrefour, et sur une voie OUVERTE a la circulation. Le decoupage est fait ICI
    (le C++ ne fait que poser des quads)."""
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
        # REGLE NATIONALE : pas d'axe peint sur une voie « restreinte aux ayants
        # droit » (rue pietonne a acces riverains). `.get` et non `[]` : les scenes
        # synthetiques des self-tests n'ont pas cet attribut, et une route sans
        # attribut reste traitee comme ouverte.
        if r.get("acces") in ACCES_SANS_AXIALE:
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


def cuire_cellule(cx, cy, parcelles, eaux, routes, noeuds_pp, batis=None, verts=None,
                  preview=True):
    """`noeuds_pp` : positions des noeuds OSM `highway=crossing` DEJA filtres sur le
    marquage (charger_crossings_osm). C'etait auparavant la liste des axes pietons
    OSM, qui servait a DEVINER les traversees — la source a change, pas le contrat."""
    t0 = time.time()
    x0, y0 = cx * CELL_M, cy * CELL_M
    cell_box = box(x0, y0, x0 + CELL_M, y0 + CELL_M)
    px_ss = CELL_M / (OUT_PX * SS)                      # metres par pixel de calcul
    marge_m = MARGIN_PX * px_ss
    zone = box(x0 - marge_m, y0 - marge_m, x0 + CELL_M + marge_m, y0 + CELL_M + marge_m)

    loc_pp = [q for q in (noeuds_pp or [])
              if x0 - 10.0 <= q[0] <= x0 + CELL_M + 10.0
              and y0 - 10.0 <= q[1] <= y0 + CELL_M + 10.0]
    loc_parc = [p for p in parcelles if p.intersects(zone)]
    loc_eau = [e for e in eaux if e.intersects(zone)]
    loc_routes = [r for r in routes if r["line"].intersects(zone)]
    loc_bati = [b for b in (batis or []) if b.intersects(zone)]
    corridor, chaussee, privee, gravier, u_bati = classes_de_cellule(
        zone, loc_parc, loc_eau, loc_routes, loc_bati)

    # --- SOLVERT : la couche HERBE, complement de la voirie peinte.
    # herbe = union(OCS GE CS2.*) - chaussee - privee - gravier - bati - eau, puis
    # ouverture morphologique vectorielle de HERBE_OUVERTURE_M. La soustraction se
    # fait contre LA GEOMETRIE MEME qui sera peinte : deborder est impossible.
    loc_verts = [v for v in (verts or []) if v.intersects(zone)]
    u_eau = C.valide(unary_union(loc_eau)) if loc_eau else None

    def herbe_brute(emprise, verts_loc, ch, pr, gr, ub, eau, ouvrir=True):
        """Herbe telle que la donne le RELEVE : union OCS GE moins tout ce qui est
        peint, bati ou en eau. C'est la reference — ni accostee, ni remplie."""
        if not verts_loc:
            return emprise.difference(emprise)
        g = C.valide(unary_union(verts_loc).intersection(emprise))
        for m in (ch, pr, gr, ub, eau):
            if m is not None and not m.is_empty and not g.is_empty:
                g = C.valide(g.difference(m))
        if ouvrir and not g.is_empty:
            g = C.valide(g.buffer(-HERBE_OUVERTURE_M).buffer(HERBE_OUVERTURE_M))
        return g

    herbe = herbe_brute(zone, loc_verts, chaussee, privee, gravier, u_bati, u_eau)
    compart = {"ilots": 0, "remplis": 0, "gain_m2": 0.0}
    boucher_trous.dernier = {"trous": 0, "combles": 0, "aire_m2": 0.0}
    aire_releve = herbe.intersection(cell_box).area     # avant toute regle v2
    aire_compart = aire_releve
    if loc_verts:
        # --- REGLE DE COMPARTIMENT, evaluee sur une fenetre ELARGIE (halo) pour que
        # deux cellules voisines prennent la MEME decision sur un ilot a cheval.
        if HERBE_COMPART_TAUX > 0.0 and HERBE_COMPART_HALO_M > 0.0:
            hm = HERBE_COMPART_HALO_M
            zh = box(x0 - hm, y0 - hm, x0 + CELL_M + hm, y0 + CELL_M + hm)
            h_parc = [p for p in parcelles if p.intersects(zh)]
            h_eau = [e for e in eaux if e.intersects(zh)]
            h_rou = [r for r in routes if r["line"].intersects(zh)]
            h_bat = [b for b in (batis or []) if b.intersects(zh)]
            h_ver = [v for v in (verts or []) if v.intersects(zh)]
            (h_corr, h_ch, h_pr, h_gr, h_ub) = classes_de_cellule(
                zh, h_parc, h_eau, h_rou, h_bat)
            h_ref = herbe_brute(zh, h_ver, h_ch, h_pr, h_gr, h_ub,
                                C.valide(unary_union(h_eau)) if h_eau else None)
            ilots, libre = compartiments(zh, h_corr, h_ch, h_pr, h_gr, h_ub, h_parc)
            rempli = remplir_compartiments(herbe, h_ref, ilots, libre)
            compart = dict(remplir_compartiments.dernier)
            if rempli is not None and not rempli.is_empty:
                herbe = C.valide(rempli.intersection(zone))
            aire_compart = herbe.intersection(cell_box).area
            compart["gain_m2"] = round(aire_compart - aire_releve, 1)
        herbe = accoster_herbe(herbe, chaussee, privee, gravier, u_bati, u_eau)
    trous = dict(getattr(boucher_trous, "dernier",
                         {"trous": 0, "combles": 0, "aire_m2": 0.0}))

    # --- rasterisation (grille de calcul : cellule + marge, a 24,41 cm/px)
    size = OUT_PX * SS + 2 * MARGIN_PX
    ox, oy = x0 - marge_m, y0 - marge_m
    m_corr = rasterize(corridor, size, ox, oy, px_ss)
    m_priv = rasterize(privee, size, ox, oy, px_ss)
    m_grav = rasterize(gravier, size, ox, oy, px_ss)
    m_road = rasterize(chaussee, size, ox, oy, px_ss)
    m_grass = rasterize(herbe, size, ox, oy, px_ss)
    # Priorite du melange : chaussee > gravier > privee > herbe > trottoir. Les
    # masques des SDF sont rendus DISJOINTS dans le meme ordre, sinon le shader (qui
    # empile les SDF) et le canal de classe se contrediraient sur les recouvrements.
    m_grav = m_grav & ~m_road
    m_priv = m_priv & ~m_road & ~m_grav
    m_grass = m_grass & ~m_road & ~m_grav & ~m_priv
    cls = np.zeros((size, size), dtype=np.uint8)
    cls[m_corr] = CLS_TROTTOIR
    cls[m_grass] = CLS_HERBE
    cls[m_priv] = CLS_PRIVEE
    cls[m_grav] = CLS_GRAVIER
    cls[m_road] = CLS_CHAUSSEE

    sdf_road = signed_distance_px(m_road, BAND_PX)
    sdf_priv = signed_distance_px(m_priv, BAND_PX)
    sdf_grav = signed_distance_px(m_grav, BAND_PX)
    sdf_grass = signed_distance_px(m_grass, BAND_PX)

    def reduire_sdf(sdf):
        c = sdf[MARGIN_PX:MARGIN_PX + OUT_PX * SS, MARGIN_PX:MARGIN_PX + OUT_PX * SS]
        c = c.reshape(OUT_PX, SS, OUT_PX, SS).mean(axis=(1, 3))
        return encode_sdf(c, px_ss)

    crop = (slice(MARGIN_PX, MARGIN_PX + OUT_PX * SS), slice(MARGIN_PX, MARGIN_PX + OUT_PX * SS))
    r_chan = cls[crop][::SS, ::SS]                      # classe : apercu/mesure seulement
    g_grass = reduire_sdf(sdf_grass)
    rgba = np.dstack([g_grass, reduire_sdf(sdf_road), reduire_sdf(sdf_priv), reduire_sdf(sdf_grav)])

    # Mesure de fidelite raster (decision 1024 vs 2048, cf. commande SOLVERT) : aire
    # vectorielle de l'herbe dans la cellule VS aire « SDF cuit > 0,5 » au PNG final.
    aire_herbe_vec = herbe.intersection(cell_box).area
    aire_herbe_png = float((g_grass >= 128).sum()) * (CELL_M / OUT_PX) ** 2

    os.makedirs(OUT_DIR, exist_ok=True)
    png = os.path.join(OUT_DIR, "mask_%d_%d.png" % (cx, cy))
    Image.fromarray(rgba, "RGBA").save(png, "PNG", optimize=True)

    # --- le RELIEF qui restera maille
    # Les passages viennent des NOEUDS OSM `highway=crossing` deja filtres sur le
    # marquage (parametre `noeuds_pp`). Chaque site coupe la bordure a son droit —
    # c'est legitime maintenant que la traversee est une DONNEE, pas une inference.
    crossings = (crossing_sites(chaussee, cell_box, loc_routes, loc_pp, u_bati)
                 if CROSSINGS_ON else [])
    curbs = curb_lines(chaussee, cell_box, loc_routes, crossings, u_bati)
    jonctions = junction_points(loc_routes)
    dashes = axial_dashes(loc_routes, chaussee, cell_box, jonctions)

    aires = {}
    for nom, g in (("corridor", corridor), ("chaussee", chaussee),
                   ("privee", privee), ("gravier", gravier), ("herbe", herbe)):
        aires[nom] = round(g.intersection(cell_box).area, 1)
    aires["cellule"] = CELL_M * CELL_M

    data = {
        "cell": [cx, cy],
        "cellSizeM": CELL_M,
        "origin": [x0, y0],
        "maskPx": OUT_PX,
        "sdfRangeM": SDF_RANGE_M,
        "classes": {"hors": CLS_HORS, "trottoir": CLS_TROTTOIR, "chaussee": CLS_CHAUSSEE,
                    "privee": CLS_PRIVEE, "gravier": CLS_GRAVIER, "herbe": CLS_HERBE},
        "maskChannels": {"R": "sdf_herbe", "G": "sdf_chaussee",
                         "B": "sdf_privee", "A": "sdf_gravier"},
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
    ecart_pc = (100.0 * abs(aire_herbe_png - aire_herbe_vec) / aire_herbe_vec
                if aire_herbe_vec > 1.0 else 0.0)
    log("cellule %+d,%+d : chaussee %.1f %% | herbe %.0f m2 (raster %.0f m2, ecart "
        "%.2f %%) | %d bordures (%.0f m) | %d passages | %d tirets | masque %.2f Mo | "
        "json %.2f Mo | %.1f s"
        % (cx, cy, pc, aire_herbe_vec, aire_herbe_png, ecart_pc,
           len(curbs), sum(LineString(l).length for l in curbs if len(l) > 1),
           len(crossings), len(dashes), os.path.getsize(png) / 1048576.0,
           os.path.getsize(js) / 1048576.0, time.time() - t0))
    log("   herbe v2 : releve %.0f m2 -> compartiment %+.0f m2 (%d ilots remplis sur "
        "%d) -> accostage+trous %+.0f m2 | trous vus %d, combles %d (%.0f m2)"
        % (aire_releve, aire_compart - aire_releve, compart["remplis"], compart["ilots"],
           aire_herbe_vec - aire_compart, trous["trous"], trous["combles"],
           trous["aire_m2"]))
    return {"cell": [cx, cy], "origin": [x0, y0], "png": png, "json": js, "curbs": len(curbs),
            "crossings": len(crossings), "axial": len(dashes),
            "curbLenM": round(sum(LineString(l).length for l in curbs if len(l) > 1), 1),
            "pngBytes": os.path.getsize(png), "jsonBytes": os.path.getsize(js),
            "areasM2": aires, "herbeRasterM2": round(aire_herbe_png, 1),
            "herbeEcartPc": round(ecart_pc, 3),
            "herbeReleveM2": round(aire_releve, 1),
            "herbeCompartM2": round(aire_compart - aire_releve, 1),
            "herbeAccostM2": round(aire_herbe_vec - aire_compart, 1),
            "compartIlots": compart["ilots"], "compartRemplis": compart["remplis"],
            "trousVus": trous["trous"], "trousCombles": trous["combles"],
            "trousAireM2": trous["aire_m2"]}


def apercu(cx, cy, r_chan, g_chan, curbs, crossings, dashes, x0, y0):
    """PNG de controle LISIBLE (le masque, lui, n'est pas fait pour l'oeil)."""
    couleurs = {CLS_HORS: (238, 236, 232), CLS_TROTTOIR: (214, 200, 172),
                CLS_CHAUSSEE: (96, 100, 104), CLS_PRIVEE: (176, 126, 100),
                CLS_GRAVIER: (176, 158, 118), CLS_HERBE: (118, 174, 108)}
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
    # Le verrou 11 cuit une cellule bidon a resolution reduite : il touche donc aux
    # globales de resolution, et les remet en place dans un finally.
    global OUT_PX, SS, HI
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
    corridor, chaussee, privee, gravier, u_b = classes_de_cellule(zone, [p1, p2], [], routes)
    check_bool("sans bati : pas d'union batie", u_b is None, True)
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

    # 5. Passage pieton — SOURCE OSM (lot FINITION_SOL). Un noeud pose sur l'axe
    #    produit un site, dans l'axe de la RUE (donc vertical ici) et a la
    #    demi-largeur mesuree. Un noeud LOIN de tout axe n'en produit aucun.
    sites = crossing_sites(chaussee, zone, routes, [(50.2, 50.0)], u_b)
    check("sites de passage", float(len(sites)), 1.0, 0.0)
    if sites:
        s = sites[0]
        check("passage : demi-largeur", s["halfW"], 5.0, 0.01)
        check("passage : |dy| (axe de rue)", abs(s["d"][1]), 1.0, 0.01)
        check("passage : x du site (snappe sur l'axe)", s["p"][0], 50.0, 0.05)
    # Un noeud sur le TROTTOIR (hors de toute chaussee) : aucun site.
    check("passage : noeud sans axe rejete",
          float(len(crossing_sites(chaussee, zone, routes, [(8.0, 60.0)], u_b))), 0.0, 0.0)
    # Un noeud sur une rue TROP ETROITE pour le quad : le garde-fou « le quad doit
    # tomber dans la chaussee » le refuse (la rue de 4 m est hors corridor ici, donc
    # ce n'est pas de la chaussee du tout).
    check("passage : noeud hors chaussee rejete",
          float(len(crossing_sites(chaussee, zone, routes, [(20.0, 60.0)], u_b))), 0.0, 0.0)
    # Dedoublonnage : deux noeuds a 1 m l'un de l'autre ne font qu'un passage.
    check("passage : dedoublonnage a %.0f m" % CROSS_DEDUP_M,
          float(len(crossing_sites(chaussee, zone, routes, [(50.0, 50.0), (50.0, 51.0)], u_b))),
          1.0, 0.0)
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

    # ------------------------------------------------------------------ correctif 2
    # 8. LE CORRIDOR CONNAIT LE BATI. Scene dediee : corridor de 20 m entre deux
    #    parcelles, une chaussee de 8 m au milieu, et trois batiments :
    #      B1 pose sur le corridor, LOIN de tout axe   -> il troue le corridor ;
    #      B2 a cheval sur le bord de la chaussee      -> il troue la chaussee et
    #                                                     tue la bordure le long du mur ;
    #      B3 TRAVERSE par deux axes                   -> PORCHE : la rue passe dessous.
    zb = box(0, 0, 100, 100)
    q1 = Polygon([(0, 0), (40, 0), (40, 100), (0, 100)])
    q2 = Polygon([(60, 0), (100, 0), (100, 100), (60, 100)])
    r_vert = {"line": LineString([(50, -10), (50, 110)]), "largeur": 8.0,
              "nature": "route a 1 chaussee", "pont": False, "voies": 2,
              "etroit": False, "sans_bordure": False}
    r_hori = {"line": LineString([(-10, 50), (110, 50)]), "largeur": 6.0,
              "nature": "route a 1 chaussee", "pont": False, "voies": 2,
              "etroit": False, "sans_bordure": False}
    routes_b = [r_vert, r_hori]
    b1 = box(41.0, 20.0, 45.0, 30.0)          # dans le corridor, hors chaussee
    b2 = box(44.0, 70.0, 48.0, 80.0)          # mord la chaussee (46..54), pas l'axe
    b3 = box(42.0, 45.0, 58.0, 55.0)          # traverse par les deux axes : porche

    cor0, ch0, _pv0, _gv0, _ub0 = classes_de_cellule(zb, [q1, q2], [], routes_b)
    check("corridor sans bati (m2)", cor0.area, 2000.0, 1.0)
    cor1, ch1, pv1, gv1, ub1 = classes_de_cellule(zb, [q1, q2], [], routes_b, [b1])
    check_bool("union batie rendue", ub1 is not None and not ub1.is_empty, True)
    # « Exactement l'aire du batiment » = son emprise BUFFEREE de BATI_BUFFER_M,
    # c'est cette geometrie-la qui est soustraite (le buffer absorbe le desaccord de
    # calage cadastre / bati).
    check("corridor - batiment (m2)", cor0.area - cor1.area, b1.buffer(BATI_BUFFER_M).area, 0.02)
    check("corridor sous batiment (m2)", cor1.intersection(b1).area, 0.0, 1e-6)

    cor2, ch2b, pv2, gv2, ub2 = classes_de_cellule(zb, [q1, q2], [], routes_b, [b2])
    check("chaussee sous batiment (m2)", ch2b.intersection(b2).area, 0.0, 1e-6)
    check_bool("la chaussee perd bien du terrain", ch2b.area < ch0.area - 5.0, True)
    # La voirie PRIVEE ne doit pas ramasser ce que le corridor perd sous les murs.
    check("privee sous batiment (m2)", pv2.intersection(b2).area, 0.0, 1e-6)
    lb = curb_lines(ch2b, zb, routes_b, [], ub2)
    dans_bati = sum(1 for ln in lb for c in ln if b2.contains(Point(c)))
    check("points de bordure dans le bati", float(dans_bati), 0.0, 0.0)
    lg_bati = sum(LineString(ln).intersection(b2).length for ln in lb if len(ln) > 1)
    check("longueur de bordure dans le bati", lg_bati, 0.0, 1e-6)
    # La coupe ne doit pas tout emporter : la frontiere de la chaussee est ici un
    # anneau unique (croix), la coupe du batiment l'ouvre — une seule polyligne,
    # mais qui doit rester longue.
    lg_reste = sum(LineString(ln).length for ln in lb if len(ln) > 1)
    check_bool("des bordures subsistent ailleurs", len(lb) >= 1 and lg_reste > 200.0, True)

    # 9. PORCHE : l'axe passe sous le batiment -> la chaussee est CONSERVEE dessous.
    cor3, ch3, _pv3, _gv3, ub3 = classes_de_cellule(zb, [q1, q2], [], routes_b, [b3])
    bandes_b = unary_union([r["line"].buffer(r["largeur"] / 2.0, cap_style=2,
                                             join_style=2, mitre_limit=3.0)
                            for r in routes_b])
    att_porche = bandes_b.intersection(b3).area
    check_bool("le porche a de la surface", att_porche > 100.0, True)
    check("chaussee conservee sous le porche", ch3.intersection(b3).area, att_porche, 0.5)
    check_bool("le centre du porche est de la chaussee", ch3.contains(Point(50.0, 50.0)), True)
    # ... mais le RESTE du batiment (hors bande d'axe) sort quand meme du corridor.
    reste = b3.difference(bandes_b.buffer(0.25))
    check("corridor sous le porche hors bande", cor3.intersection(reste).area, 0.0, 0.05)

    # ------------------------------------------------------------------ correctif 3
    # 10. CUL-DE-SAC : le bout PLAT du buffer ferme la chaussee en travers ; la
    #     frontiere existe bien dans la geometrie, mais AUCUNE bordure ne s'y pose.
    zc = box(0, 0, 100, 100)
    r_impasse = {"line": LineString([(10, 50), (60, 50)]), "largeur": 8.0,
                 "nature": "route a 1 chaussee", "pont": False, "voies": 1,
                 "etroit": False, "sans_bordure": False}
    cor4, ch4, _pv4, _gv4, ub4 = classes_de_cellule(zc, [], [], [r_impasse])
    pend = dangling_ends([r_impasse])
    check("extremites pendantes", float(len(pend)), 2.0, 0.0)
    check("largeur retenue au bout", pend[0][1], 8.0, 1e-9)
    l4 = curb_lines(ch4, zc, [r_impasse], [])
    check_bool("bordures d'impasse produites", len(l4) >= 1, True)
    for bx_, by_ in ((10.0, 50.0), (60.0, 50.0)):
        rayon = 8.0 / 2.0 + CURB_DANGLE_CLEAR_M
        disque = Point(bx_, by_).buffer(rayon)
        # l'artefact EXISTE dans la geometrie de la chaussee ...
        check_bool("bout plat present dans la chaussee (%d)" % int(bx_),
                   ch4.boundary.intersection(disque).length > 5.0, True)
        # ... et il ne ressort PAS en bordure.
        lg = sum(LineString(ln).intersection(disque).length for ln in l4 if len(ln) > 1)
        check("bordure au bout pendant (%d)" % int(bx_), lg, 0.0, 1e-6)
    # Un carrefour en T : l'extremite qui touche deux autres troncons n'est PAS
    # pendante, elle ne doit pas faire taire la bordure.
    r_t = [dict(r_impasse, line=LineString([(10, 50), (60, 50)])),
           dict(r_impasse, line=LineString([(60, 50), (90, 50)])),
           dict(r_impasse, line=LineString([(60, 50), (60, 90)]))]
    pend_t = [p for p, _w in dangling_ends(r_t)]
    check("pendantes du T (le noeud exclu)", float(len(pend_t)), 3.0, 0.0)
    check_bool("le noeud du T n'est pas pendant",
               all(math.hypot(p[0] - 60.0, p[1] - 50.0) > 0.5 for p in pend_t), True)

    # --------------------------------------------------- FINITION_SOL : verrou 11
    # 11. LES PASSAGES SONT COUPES — ET LE MECANISME EST INTACT.
    #     Etat de verite du 2026-08-01 (V2, verdict utilisateur) : la cuisson n'ecrit
    #     AUCUN passage. Mais le mecanisme reste teste EN DIRECT (crossing_sites,
    #     quad_de_passage, les garde-fous) : le jour du rebranchement au chantier
    #     props/marquages, ces verrous-la sont deja verts et disent ce qui est promis.
    check_bool("CROSSINGS_ON coupe", CROSSINGS_ON, False)
    X0 = 9999 * CELL_M
    pp1 = box(X0 - 20, X0 - 20, X0 + 245, X0 + 520)
    pp2 = box(X0 + 255, X0 - 20, X0 + 520, X0 + 520)
    rr = [{"line": LineString([(X0 + 250, X0 - 20), (X0 + 250, X0 + 520)]), "largeur": 10.0,
           "nature": "route a 1 chaussee", "pont": False, "voies": 2,
           "etroit": False, "sans_bordure": False}]
    pd = [(X0 + 250.0, X0 + 250.0)]                     # UN noeud OSM marque, sur l'axe
    _c, ch5, _p, _g, _u = classes_de_cellule(
        box(X0 - 4, X0 - 4, X0 + CELL_M + 4, X0 + CELL_M + 4), [pp1, pp2], [], rr)
    # --- MECANISME (appel direct, independant du drapeau) : un noeud OSM marque
    #     donne UN site, dans l'axe de la rue et a sa demi-largeur ; zero noeud donne
    #     zero site (« jamais un passage invente »).
    bcell = box(X0, X0, X0 + CELL_M, X0 + CELL_M)
    sites_1 = crossing_sites(ch5, bcell, rr, pd)
    check("crossing_sites appelee directement", float(len(sites_1)), 1.0, 0.0)
    check("crossing_sites SANS noeud OSM (jamais invente)",
          float(len(crossing_sites(ch5, bcell, rr, []))), 0.0, 0.0)
    check("site propose : demi-largeur", float(sites_1[0]["halfW"]), 5.0, 0.01)
    check("site propose : dans l'axe de la rue", abs(float(sites_1[0]["d"][1])), 1.0, 0.01)
    # Cuisson reelle d'une cellule bidon (hors zone de production), a resolution
    # reduite : c'est le CHEMIN COMPLET qu'on verrouille, pas une expression
    # recopiee. Les globales de resolution sont restaurees quoi qu'il arrive.
    sav = (OUT_PX, SS, HI)
    OUT_PX, SS, HI = 64, 1, 1
    res = None
    try:
        # MEME donnee, MEME noeud injecte que le mecanisme ci-dessus : la cuisson,
        # elle, n'ecrit RIEN. C'est le drapeau qui coupe, et lui seul.
        res = cuire_cellule(9999, 9999, [pp1, pp2], [], rr, pd, [], preview=False)
        with open(res["json"], encoding="utf-8") as f:
            cuit = json.load(f)
    finally:
        OUT_PX, SS, HI = sav
        for p in ((res["png"], res["json"]) if res else ()):
            try:
                os.remove(p)
            except OSError:
                pass
    check("passages ecrits par la cuisson (CROSSINGS_ON coupe)",
          float(len(cuit["crossings"])), 0.0, 0.0)
    check_bool("la cuisson ecrit toujours ses bordures", len(cuit["curbs"]) > 0, True)
    check_bool("la cuisson ecrit toujours ses tirets", len(cuit["axial"]) > 0, True)
    # La bordure n'est plus interrompue par un passage : elle redevient CONTINUE.
    # (v1 : le quad coupait la rive au droit de chaque site.)
    check_bool("bordure continue : au plus 2 rives sur une rue droite",
               len(cuit["curbs"]) <= 2, True)

    # --------------------------------------------------- FINITION_SOL : verrou 13
    # 13. BORDURES ARTEFACTS. Deux familles mesurees, deux verrous.
    #  (a) POINTE DEGENEREE : une MARCHE DE LARGEUR entre deux troncons consecutifs
    #      fabrique une langue en travers de la rue. Elle ne doit plus ressortir.
    zm = box(0, 0, 120, 120)
    r_m = [{"line": LineString([(0, 60), (60, 60)]), "largeur": 9.0,
            "nature": "route a 1 chaussee", "pont": False, "voies": 2,
            "etroit": False, "sans_bordure": False},
           {"line": LineString([(60, 60), (120, 60.9)]), "largeur": 9.0,
            "nature": "route a 1 chaussee", "pont": False, "voies": 2,
            "etroit": False, "sans_bordure": False}]
    ch_m = classes_de_cellule(zm, [], [], r_m)[1]
    l_m = curb_lines(ch_m, zm, r_m, [])
    # Aucun segment de bordure ne doit traverser la rue : on compte les segments
    # dont l'ecart en Y depasse 3 m (la rue est horizontale, large de 9 m).
    travers = 0
    lg_m = 0.0
    for ln in l_m:
        for a, b in zip(ln, ln[1:]):
            lg_m += math.hypot(b[0] - a[0], b[1] - a[1])
            if abs(b[1] - a[1]) > 3.0:
                travers += 1
    check("marche de largeur : aucune bordure en travers", float(travers), 0.0, 0.0)
    # les deux rives de 120 m survivent (240 m attendus, moins les bouts pendants)
    check_bool("marche de largeur : les bordures longitudinales restent (%.0f m)" % lg_m,
               lg_m > 200.0, True)
    #  (b) La fonction de depointage, isolee : une pointe de 2 cm de large meurt,
    #      une VRAIE epingle (terre-plein de 3 m) survit.
    pointe = [(0.0, 0.0), (10.0, 0.0), (10.0, 4.5), (10.02, 0.0), (20.0, 0.0)]
    check("depointage : pointe de 2 cm retiree",
          float(len(retirer_pointes(pointe))), 4.0, 0.0)
    epingle = [(0.0, 0.0), (10.0, 0.0), (10.0, 20.0), (13.0, 20.0), (13.0, 0.0), (23.0, 0.0)]
    check("depointage : epingle de 3 m conservee",
          float(len(retirer_pointes(epingle))), 6.0, 0.0)
    #  (c) FRONTIERE INTERNE : une bordure enfoncee dans la chaussee n'est pas une
    #      bordure. Une coupe cadastrale au milieu de la rue ne doit rien mailler.
    zi = box(0, 0, 100, 100)
    r_i = [{"line": LineString([(50, -10), (50, 110)]), "largeur": 12.0,
            "nature": "route a 1 chaussee", "pont": False, "voies": 2,
            "etroit": False, "sans_bordure": False}]
    # parcelle privee qui mange la MOITIE DROITE de la rue : la chaussee publique
    # s'arrete a l'axe, et cette frontiere-la est une donnee, pas une marche.
    par_i = [box(-10, -10, 50, 110), box(56, -10, 110, 110)]
    ch_i = classes_de_cellule(zi, par_i, [], r_i)[1]
    l_i = curb_lines(ch_i, zi, r_i, [])
    dedans = 0.0
    for ln in l_i:
        for a, b in zip(ln, ln[1:]):
            mx = (a[0] + b[0]) * 0.5
            if abs(mx - 50.0) < 6.0 - CURB_DEDANS_M - 0.01:
                dedans += math.hypot(b[0] - a[0], b[1] - a[1])
    check("frontiere interne : rien de maille dans la chaussee", dedans, 0.0, 1e-6)

    # ------------------------------------------------------------------ SOLVERT
    # 12. LA COUCHE HERBE (canal R). Scene synthetique : une pelouse de 120 x 120 m
    #     TRAVERSEE par la chaussee de 10 m, plus une LANGUETTE de 1,6 m de large
    #     (l'artefact type du grief 4) : la pelouse est peinte MOINS la chaussee
    #     (complement : deborder est impossible), la languette est effacee par
    #     l'ouverture morphologique de 1 m, et le canal R du PNG cuit porte bien le
    #     SDF (>= 200 au coeur de la pelouse, <= 60 sur la chaussee, 128 +/- au bord).
    X1 = 8888 * CELL_M
    hp1 = box(X1 - 20, X1 - 20, X1 + 245, X1 + 520)
    hp2 = box(X1 + 255, X1 - 20, X1 + 520, X1 + 520)
    hr = [{"line": LineString([(X1 + 250, X1 - 20), (X1 + 250, X1 + 520)]), "largeur": 10.0,
           "nature": "route a 1 chaussee", "pont": False, "voies": 2,
           "etroit": False, "sans_bordure": False}]
    pelouse = box(X1 + 190, X1 + 190, X1 + 310, X1 + 310)          # a cheval sur la rue
    languette = box(X1 + 50, X1 + 50, X1 + 51.6, X1 + 80)          # 1,6 m x 30 m
    sav = (OUT_PX, SS, HI)
    OUT_PX, SS, HI = 128, 1, 2
    res_h = None
    try:
        res_h = cuire_cellule(8888, 8888, [hp1, hp2], [], hr, [], [],
                              [pelouse, languette], preview=False)
        px_png = np.asarray(Image.open(res_h["png"]))
    finally:
        OUT_PX, SS, HI = sav
        for p in ((res_h["png"], res_h["json"]) if res_h else ()):
            try:
                os.remove(p)
            except OSError:
                pass
    aires_h = res_h["areasM2"]
    # Pelouse 120 x 120 m moins la bande de chaussee de 10 m qui la traverse, moins
    # le lisere de l'ouverture (les coins convexes de la decoupe sont adoucis) :
    # l'aire doit etre proche de 120*120 - 10*120 = 13200, jamais au-dessus, et la
    # languette (48 m2) ne doit PAS y figurer.
    check_bool("herbe : pelouse peinte", 12600.0 < aires_h["herbe"] <= 13210.0, True)
    check_bool("herbe : complement de la chaussee",
               aires_h["herbe"] <= 120.0 * 120.0 - 10.0 * 120.0 + 1.0, True)
    # Echantillons du canal R du PNG cuit (128 px sur 500 m -> 3,9 m/px ; la cellule
    # va de X1 a X1+500, la ligne 0 du PNG est y = X1).
    def png_at(mx, my):
        px = int((mx - X1) / CELL_M * px_png.shape[1])
        py = int((my - X1) / CELL_M * px_png.shape[0])
        return int(px_png[min(py, px_png.shape[0] - 1), min(px, px_png.shape[1] - 1), 0])
    check_bool("canal R : coeur de pelouse >= 200", png_at(X1 + 220, X1 + 250) >= 200, True)
    check_bool("canal R : chaussee <= 60", png_at(X1 + 250, X1 + 250) <= 60, True)
    check_bool("canal R : languette EFFACEE", png_at(X1 + 50.8, X1 + 65) <= 128, True)
    check_bool("canal R : hors tout vert <= 60", png_at(X1 + 100, X1 + 400) <= 60, True)
    # Sans verts fournis (retro-compatibilite), le canal R est un SDF vide (plancher).
    check_bool("herbe absente par defaut (verrou 11)", cuit["areasM2"]["herbe"] == 0.0, True)

    # --------------------------------------------------- FINITION_SOL : verrou 14
    # 14. ACCOSTAGE ET LISSAGE DU BORD D'HERBE.
    #     (a) une pelouse separee de la chaussee par un lisere de 60 cm de dalle
    #         vient ACCOSTER la chaussee : le lisere disparait, l'herbe touche ;
    #     (b) une pelouse a 5 m de tout mineral n'est PAS tiree vers lui (un bord
    #         organique en plein parc est legitime) ;
    #     (c) l'herbe reste le COMPLEMENT STRICT : zero recouvrement du mineral.
    route_a = box(0.0, 0.0, 10.0, 100.0)                # « chaussee »
    vide = Polygon()
    pel_pres = box(10.6, 0.0, 40.0, 100.0)              # lisere de 60 cm
    acc = accoster_herbe(pel_pres, route_a, vide, vide)
    check("accostage : le lisere de 60 cm est comble", acc.area, 29.4 * 100.0 + 0.6 * 100.0, 1.5)
    check("accostage : l'herbe touche la chaussee", acc.distance(route_a), 0.0, 1e-6)
    check("accostage : zero recouvrement du mineral", acc.intersection(route_a).area, 0.0, 1e-6)
    pel_loin = box(15.0, 0.0, 40.0, 100.0)              # 5 m de dalle : bord legitime
    acc2 = accoster_herbe(pel_loin, route_a, vide, vide)
    check("accostage : a 5 m, l'herbe n'est PAS tiree", acc2.area, 25.0 * 100.0, 1.5)
    check_bool("accostage : l'ecart de 5 m est conserve",
               abs(acc2.distance(route_a) - 5.0) < 0.05, True)
    # (d) LISSAGE : un contour qui gigote sous le texel est aplani.
    dents = [(20.0, 0.0)]
    for i in range(1, 200):
        dents.append((20.0 + (0.2 if i % 2 else 0.0), i * 0.5))
    dents += [(20.0, 100.0), (40.0, 100.0), (40.0, 0.0)]
    brut = C.valide(Polygon(dents))
    liss = accoster_herbe(brut, vide, vide, vide)
    check_bool("lissage : le contour dentele est simplifie (%d -> %d sommets)"
               % (len(brut.exterior.coords), len(liss.exterior.coords)),
               len(liss.exterior.coords) < len(brut.exterior.coords) / 3.0, True)
    check_bool("lissage : l'aire est preservee a 1 %%",
               abs(liss.area - brut.area) / brut.area < 0.01, True)

    # ------------------------------------------------ FINITION_SOL V2 : verrou 15
    # 15. LIGNE AXIALE ET « AYANTS DROIT ». Deux troncons RIGOUREUSEMENT identiques
    #     (meme geometrie, meme largeur, meme nombre de voies) : seul l'attribut
    #     d'acces change. Celui qui est restreint ne recoit plus AUCUN tiret, celui
    #     qui est libre garde exactement les siens.
    z_ad = box(0, 0, 200, 200)
    def _tr(y, acces):
        return {"line": LineString([(0.0, y), (200.0, y)]), "largeur": 9.0,
                "nature": "route a 1 chaussee", "pont": False, "voies": 2,
                "etroit": False, "sans_bordure": False, "acces": acces}
    r_lib = [_tr(60.0, "libre")]
    r_ayd = [_tr(60.0, "restreint aux ayants droit")]
    ch_ad = classes_de_cellule(z_ad, [], [], r_lib)[1]
    n_lib = len(axial_dashes(r_lib, ch_ad, z_ad, []))
    n_ayd = len(axial_dashes(r_ayd, ch_ad, z_ad, []))
    check_bool("axiale : une rue LIBRE garde ses tirets (%d)" % n_lib, n_lib > 20, True)
    check("axiale : zero tiret sur « ayants droit »", float(n_ayd), 0.0, 0.0)
    # Une route sans l'attribut (scene synthetique, donnee incomplete) reste ouverte :
    # la regle ne doit jamais effacer un marquage par silence de la donnee.
    r_sans = [dict(_tr(60.0, None))]
    del r_sans[0]["acces"]
    check("axiale : attribut absent = voie ouverte",
          float(len(axial_dashes(r_sans, ch_ad, z_ad, []))), float(n_lib), 0.0)

    # ------------------------------------------------ FINITION_SOL V2 : verrou 16
    # 16. HERBE V2 : facades, trous internes, compartiment.
    #  (a) FACADE : une pelouse separee d'un MUR par un lisere de 60 cm vient
    #      accoster le mur, exactement comme elle accoste une chaussee.
    mur = box(0.0, 0.0, 10.0, 100.0)
    pel_mur = box(10.6, 0.0, 40.0, 100.0)
    acc_m = accoster_herbe(pel_mur, vide, vide, vide, mur)
    check("facade : le lisere de 60 cm est comble", acc_m.area, 30.0 * 100.0, 1.5)
    check("facade : l'herbe touche le mur", acc_m.distance(mur), 0.0, 1e-6)
    check("facade : zero recouvrement du bati", acc_m.intersection(mur).area, 0.0, 1e-6)
    #  (b) TROUS INTERNES : un trou de releve se comble, un trou qui contient un
    #      objet du bake reste ouvert, un trou plus grand que le seuil reste ouvert.
    pleine = box(0.0, 0.0, 100.0, 100.0)
    t_petit = box(10.0, 10.0, 15.0, 15.0)                  # 25 m2, vide
    t_objet = box(30.0, 30.0, 35.0, 35.0)                  # 25 m2, avec un batiment
    t_grand = box(60.0, 60.0, 70.0, 70.0)                  # 100 m2 > seuil
    troue = C.valide(pleine.difference(unary_union([t_petit, t_objet, t_grand])))
    obst = box(31.0, 31.0, 34.0, 34.0)                     # le batiment dans t_objet
    bou = boucher_trous(troue, obst)
    check("trous : les 3 trous sont vus", float(boucher_trous.dernier["trous"]), 3.0, 0.0)
    check("trous : un seul comble", float(boucher_trous.dernier["combles"]), 1.0, 0.0)
    check("trous : c'est bien le trou VIDE (25 m2)", bou.area - troue.area, 25.0, 0.01)
    check_bool("trous : le trou a batiment reste ouvert",
               bou.intersection(obst).area < 1e-6, True)
    check_bool("trous : le trou de 100 m2 reste ouvert",
               bou.intersection(t_grand.buffer(-0.5)).area < 1e-6, True)
    #  (c) COMPARTIMENT : deux ilots separes par une chaussee. Celui qui est deja
    #      majoritairement herbe est peint jusqu'a ses limites ; l'autre ne bouge pas.
    z_c = box(0.0, 0.0, 200.0, 100.0)
    route_c = box(95.0, 0.0, 105.0, 100.0)
    ilot_g = box(0.0, 0.0, 95.0, 100.0)                    # 9 500 m2
    ilot_d = box(105.0, 0.0, 200.0, 100.0)                 # 9 500 m2
    h_maj = box(0.0, 0.0, 95.0, 80.0)                      # 7 600 m2 -> 80 % de l'ilot
    h_min = box(105.0, 0.0, 200.0, 20.0)                   # 1 900 m2 -> 20 % de l'ilot
    h_ref = C.valide(unary_union([h_maj, h_min]))
    ils, libre_c = compartiments(z_c, None, route_c, vide, vide, None, [])
    check("compartiment : 2 ilots trouves", float(len(ils)), 2.0, 0.0)
    remp = remplir_compartiments(h_ref, h_ref, ils, libre_c)
    check("compartiment : 1 seul ilot rempli",
          float(remplir_compartiments.dernier["remplis"]), 1.0, 0.0)
    check("compartiment : l'ilot majoritaire est peint jusqu'a ses limites",
          remp.intersection(ilot_g).area, ilot_g.area, 1.0)
    check("compartiment : l'ilot minoritaire est INTACT",
          remp.intersection(ilot_d).area, h_min.area, 1.0)
    check("compartiment : zero debordement sur la chaussee",
          remp.intersection(route_c).area, 0.0, 1e-6)
    #  (d) Le taux se mesure sur la reference du RELEVE : une herbe deja accostee ne
    #      doit pas faire basculer un ilot minoritaire (pas de boucle de retroaction).
    remp2 = remplir_compartiments(h_ref, C.valide(h_ref.difference(ilot_g)), ils, libre_c)
    check("compartiment : sans herbe de reference, l'ilot n'est pas rempli",
          float(remplir_compartiments.dernier["remplis"]), 0.0, 0.0)

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
    # La fenetre de CHARGEMENT porte la marge de calcul du SDF ET le halo de la regle
    # de compartiment : sans le halo, les ilots de bord seraient juges sur une donnee
    # tronquee (routes et parcelles manquantes) et la decision de remplissage
    # basculerait au bord de la zone cuite.
    marge = MARGIN_PX * CELL_M / (OUT_PX * SS) + 5.0 + HERBE_COMPART_HALO_M
    xs = [c[0] for c in cells]
    ys = [c[1] for c in cells]
    fen = (min(xs) * CELL_M - marge, min(ys) * CELL_M - marge,
           (max(xs) + 1) * CELL_M + marge, (max(ys) + 1) * CELL_M + marge)
    fenetres = {"proto": fen}
    parcelles = C.charger_parcelles(fenetres)["proto"]
    eaux, _verts = C.charger_surfaces(fenetres)
    eaux = eaux["proto"]
    routes = charger_routes_bdtopo(fen)
    # SOURCE DES PASSAGES : les noeuds OSM `highway=crossing` marques (donnee reelle,
    # cache disque). L'ancienne source devinee (axes pietons OSM) n'est plus lue.
    noeuds_pp = charger_crossings_osm(fen)
    # Le BATI entre dans le CALCUL, il ne sert plus seulement aux cartes de controle :
    # un immeuble est un polygone reel, il a sa place dans la soustraction du sol.
    batis = C.charger_bati(fenetres)["proto"]
    # SOLVERT : source vegetale = OCS GE CS2.* (socle national). Les verts OSM
    # (_verts) restent charges-et-jetes : repli possible pour des fenetres hors AOI
    # OCS GE, non active tant que la couverture mesuree est complete (99,9 % sur le
    # proto, cf. work/SOLVERT/solvert_prep.py).
    verts = charger_verts_ocsge(fen)
    log("chargements : %.1f s" % (time.time() - t0))

    resume = []
    for cx, cy in cells:
        resume.append(cuire_cellule(cx, cy, parcelles, eaux, routes, noeuds_pp, batis,
                                    verts, preview=not args.no_preview))
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
    log("PEINT   : %d m2 de chaussee, %d m2 de voirie privee, %d m2 de gravier, "
        "%d m2 d'HERBE (ecart raster max %.2f %%)"
        % (sum(r["areasM2"]["chaussee"] for r in resume),
           sum(r["areasM2"]["privee"] for r in resume),
           sum(r["areasM2"]["gravier"] for r in resume),
           sum(r["areasM2"]["herbe"] for r in resume),
           max((r["herbeEcartPc"] for r in resume), default=0.0)))
    log("HERBE V2: releve %d m2 -> compartiment %+d m2 (%d ilots remplis sur %d) -> "
        "accostage+trous %+d m2 | trous vus %d, combles %d (%.0f m2)"
        % (sum(r["herbeReleveM2"] for r in resume),
           sum(r["herbeCompartM2"] for r in resume),
           sum(r["compartRemplis"] for r in resume),
           sum(r["compartIlots"] for r in resume),
           sum(r["herbeAccostM2"] for r in resume),
           sum(r["trousVus"] for r in resume), sum(r["trousCombles"] for r in resume),
           sum(r["trousAireM2"] for r in resume)))
    log("MAILLE  : %d polylignes de bordure (%.0f m), %d passages, %d tirets"
        % (sum(r["curbs"] for r in resume), sum(r["curbLenM"] for r in resume),
           sum(r["crossings"] for r in resume), sum(r["axial"] for r in resume)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
