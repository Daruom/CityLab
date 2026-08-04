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
from shapely.ops import linemerge, nearest_points, substring, unary_union
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
# --- LOT QUAIS V3 : LA PROMENADE DE BERGE ------------------------------------
# CE QUE LA MESURE A CHANGE AU PLAN. Le brief prevoyait de peindre la berge basse
# avec la classe pietonne du bake ; sonde en lecture seule sur 126 points de
# promenade des cellules -2_0 et -1_0 : ils sont DEJA en classe 1 (trottoir) a
# 96,0 %, parce que le corridor vaut « cellule - parcelles - eau » et qu'une berge
# non cadastree y tombe par construction. Repeindre serait un no-op — et c'est
# justement le grief : la berge n'a pas moins d'identite que le reste, elle a la
# MEME, celle de toute surface residuelle.
# LA REGLE RETENUE, NATIONALE : « un chemin qui longe le pied d'un mur de classe
# quai, mesure du cote BAS, est une PROMENADE DE BERGE : une bande stabilisee ».
# Elle se rend avec la classe EXISTANTE `gravier`, celle a laquelle BD TOPO fait
# deja correspondre ses natures etroites (chemin, route empierree) : aucune classe
# nouvelle, aucun materiau nouveau, une seule grammaire minerale.
# Le side-car est produit par work/QUAIS/q3_promenade.py. Dossier absent ou cellule
# sans fichier = aucune promenade, sans erreur (comme les murs).
PROMENADE_DIR = os.path.join(SRC, "Promenade")
PROMENADE_ON = True        # False = rollback complet, sans re-cuire le side-car
# --- LOT SIMPLIFICATION BERGE (2026-08-04) : LA BANDE DE QUAI NE SE PEINT PLUS.
# DECISION UTILISATEUR, gravee : « qu'on enleve les tracages pietons (couleur
# beige) ». Sur la bande `quai_dur` (de la frontiere d'eau a 21 m cote terre, la
# ou un mur de quai est a portee), la peinture beige — classe `gravier`, celle ou
# se versent les natures etroites BD TOPO et la promenade de berge — est ETEINTE :
# la plateforme y garde sa CLASSE DE BASE, le pavage. C'est le RENDU qui s'eteint,
# PAS la donnee : `SourceData/Promenade` n'est pas touche, et hors bande la
# peinture des chemins reste (parcs, berges naturelles, etc.).
# L'emprise est lue dans le side-car de frontiere (champ `bande_quai`, cuit par
# work/SIMPLE/s2_ouvrage.py) : une seule verite geometrique, la meme que celle de
# l'ouvrage de berge du generateur. Champ absent = aucune extinction, sans erreur.
# Drapeau de rollback SANS re-cuisson : remettre True.
PEINTURE_BANDE_QUAI = False
FRONTIERE_DIR = os.path.join(SRC, "Frontiere")
# --- LOT FINITION QUAIS : UN OUVRAGE NE SE PEINT PAS SUR LE SOL ---------------
# REGLE NATIONALE : un troncon de route qui appartient a un OUVRAGE (la chaine
# connexe du side-car SourceData/Ponts, celle-la meme qui porte le tablier 3D du
# C++) ne se peint PAS sur le sol — ni chaussee, ni ligne axiale, ni bordure : le
# tablier les porte, plusieurs metres plus haut.
# `position_par_rapport_au_sol > 0` ne suffit pas et ne suffira jamais : BD TOPO ne
# code « pont » que la travee franchie, les RAMPES restent a `pos = 0` tout en
# etant en l'air (mesure du lot PONTS : 44 des 98 troncons d'ouvrage du proto).
# `routes_bdtopo.json` (WFS) n'a pas de `cleabs` : l'appariement se fait sur la
# GEOMETRIE, et il separe sans ambiguite (mesure : 98 troncons a >= 99,9 % de
# recouvrement, le suivant est a 47 % — cf. work/FINQUAIS/f2_appariement.json).
PONTS_DIR = os.path.join(SRC, "Ponts")
PONTS_ON = True            # False = rollback complet (les ouvrages redeviennent peints)
PONT_APP_TOL_M = 1.0       # tolerance laterale de l'appariement geometrique
PONT_APP_FRAC = 0.5        # part de la longueur du troncon a recouvrir pour apparier
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

# --- HERBE V3 (lot FINITION_SOL V3, 2026-08-01). Verdict utilisateur sur la v2 :
# « pas tres beau ». Trois griefs, trois reponses, et UN RETRAIT.
#
# RETRAIT : LA REGLE DE COMPARTIMENT ET SON HALO DE 80 m SONT SUPPRIMES.
#   Ils remplissaient un ilot majoritairement vert jusqu'a ses limites, y compris
#   jusqu'a des LIMITES CADASTRALES INVISIBLES au rendu (aucun mur, aucune bordure a
#   cet endroit) : le remplissage s'arretait sur une frontiere que l'oeil ne peut pas
#   justifier. Ils coutaient en plus 35 s de cuisson sur 49 s (le halo doublait le
#   calcul des classes) — ~80 min a 400 cellules au lieu de ~24. Le code est retire,
#   pas neutralise : `compartiments()` et `remplir_compartiments()` n'existent plus.
#   MESURE DU RETRAIT (km2 proto) : herbe 53 749 -> 51 186 m2, et surtout
#   58 -> 41 fragments, dont 22 -> 4 en dessous de 15 m2 (les fils de 1 cm ouverts
#   aux limites de parcelle etaient une fabrique a miettes).
#
# 1. MIETTES. Un fragment d'herbe isole plus petit que ce seuil n'est pas une
#    pelouse : c'est un confetti de releve. Regle SYMETRIQUE du bouchage de trous
#    (HERBE_TROU_M2) : sous le plancher de motif de la charte du sol, une tache ne
#    porte aucune information de forme. CALIBRE PAR LA DISTRIBUTION MESUREE : sur le
#    proto, les fragments font 0,2 / 0,2 / 0,3 / 1,4 m2 puis SAUTENT a 92,5 m2. Le
#    seuil ne tombe donc pas dans une queue de distribution mais dans un VIDE de
#    90 m2 : tout choix entre 2 et 90 m2 donne exactement le meme resultat. 15 m2 est
#    pose au milieu de la fourchette du brief, loin des deux bords du vide.
HERBE_MIETTE_M2 = 15.0
#
# 2. REGULARISATION DU CONTOUR. Une frontiere OCS GE est un RELEVE : elle gigote a
#    l'echelle du metre sans qu'aucun objet de la ville ne le justifie, et ca se lit
#    comme une amibe. On decime le contour FRANCHEMENT (Douglas-Peucker, donc angles
#    nets et segments longs) AVANT l'accostage — l'accostage vient ensuite recoller
#    le contour regularise sur les frontieres reelles, et la re-soustraction finale
#    garantit zero debord.
#    CALIBRATION MESUREE (4 cellules, sommets / longueur moyenne de segment / ecart
#    max au contour d'origine) :
#      0,55 m (v2) :  969 sommets,  7,5 m,  0,54 m      1,25 m :  592,  12,3 m, 1,53 m
#      0,80 m      :  856 sommets,  8,5 m,  1,03 m      1,50 m :  523,  14,0 m, 2,16 m
#      1,00 m      :  714 sommets, 10,2 m,  1,53 m      2,00 m :  443,  16,6 m, 2,70 m
#    1,25 m est retenu : c'est la valeur qui donne les segments les PLUS LONGS
#    (+21 % sur 1,00 m) SANS augmenter l'ecart maximal (1,53 m dans les deux cas,
#    soit 3 texels de masque). Au-dela, l'ecart part a 2,2 m et le contour commence a
#    quitter la pelouse. L'aire ne bouge que de +1,1 %.
HERBE_REGUL_M = 1.25
#
# 3. BORDURETTE 3D — le coeur de la v3. DOCTRINE : n'importe quelle forme BORDEE lit
#    « amenagement voulu ». Une pelouse qui meurt sur la dalle sans rien lit
#    « releve » ; la meme pelouse ceinturee d'une pierre lit « pelouse ». C'est ca qui
#    remplace la course a la peinture parfaite — on arrete de chercher le contour
#    juste, on POSE la pierre sur le contour qu'on a.
#    Le bake emet la liste `grassEdges` (memes conventions que `curbs` : polylignes en
#    metres, MINERAL A GAUCHE du sens de parcours) ; le C++ y pose le meme profil que
#    BuildMaskCurb, en plus bas (relief 7 cm au lieu de 12, chant 14 cm), avec le
#    MATERIAU DE BORDURE EXISTANT — aucun materiau nouveau.
#    QUATRE EXCLUSIONS, toutes mesurees :
#      a. le long des FACADES : l'herbe meurt sur le mur, une pierre au pied d'un mur
#         est un artefact de decoupe (43,0 % du contour du proto est du mur) ;
#      b. la ou une BORDURE DE CHAUSSEE est deja posee : pas de double pierre. Le test
#         porte sur les polylignes `curbs` REELLEMENT emises pour la cellule, pas sur
#         la frontiere de chaussee — la frontiere est coupee (bati, bout pendant,
#         autoroutier) a des endroits ou aucune pierre n'existe et ou la bordurette a
#         donc sa place ;
#      c. l'EAU : une berge n'est pas un trottoir ;
#      d. les segments plus courts que GRASS_EDGE_MIN_LEN_M : meme plancher que les
#         bordures de chaussee (CURB_MIN_LEN_M) — un bout de pierre de 80 cm est un
#         artefact de decoupe, pas un amenagement.
#    MESURE (4 cellules) : contour 6 846 m -> 2 628 m retenus en 111 segments
#    (23,7 m de moyenne), 9 m seulement ecartes par le plancher de longueur.
GRASS_EDGE_CLEAR_M = 0.40   # rayon de silence autour du bati, des bordures et de l'eau
GRASS_EDGE_MIN_LEN_M = 1.5  # = CURB_MIN_LEN_M : meme plancher physique
GRASS_EDGE_SIMPLIFY_M = 0.15  # = CURB_SIMPLIFY_M : la polyligne maillee, pas le masque

# --- V4, correctif 1 : LA PEINTURE MEURT SOUS LA PIERRE ----------------------
# Le grief utilisateur « peinture et pierre desalignees » a deux causes distinctes,
# et une seule des deux est ici :
#   (a) le MEANDRE DE BRUIT du materiau, qui deplacait la frontiere peinte de
#       +-87 cm autour de sa propre pierre. Traite dans Tools/import_ground_masks.py
#       (amplitudes mises a zero : le bruit datait des bords ORGANIQUES, avant que
#       le contour ne soit regularise a la regle et borde de pierre) ;
#   (b) ce qui reste apres (a) : la largeur de transition du shader (EdgeMinM = 4 cm
#       au sol, plus large a distance) et la discretisation du SDF. Meme minuscule,
#       elle se voit parce qu'elle tombe A COTE de la pierre.
# Remede : la peinture RECULE sous le chant de la bordurette. Le chant fait
# GRASS_CURB_TOP_W_M (14 cm cote herbe, valeur C++ GGrassCurbTopWidthCm) : un recul
# de 10 cm place la frontiere peinte SOUS la pierre avec 4 cm de marge. Aller au-dela
# (les ~20 cm evoques au brief) exigerait d'elargir le chant d'autant, sinon le recul
# ressort de l'autre cote de la pierre et rouvre un lisere de dalle — c'est un choix
# visuel, pas un reglage : il est laisse a l'utilisateur.
# Le recul s'applique UNIQUEMENT le long des polylignes de bordurette : ailleurs
# (pied de facade, berge, bordure de chaussee) l'herbe continue de mourir sur son
# obstacle, ce qui preserve le volet LISERE_MUR valide en v2.
GRASS_CURB_TOP_W_M = 0.14     # = GGrassCurbTopWidthCm cote C++
GRASS_PAINT_RETRACT_M = 0.10  # = chant - 4 cm de marge
# --- V4, correctif 3 : RACCORDS -----------------------------------------------
# GRASS_EDGE_CLEAR_M coupe le contour a 40 cm des bordures de chaussee : la
# bordurette s'arrete donc systematiquement 40 cm trop tot et le raccord se lit
# comme un ratage. Toute extremite a moins de GRASS_EDGE_SNAP_M d'une polyligne
# `curbs` est RAMENEE dessus (les bouchons de fin, cote C++, ferment le reste).
GRASS_EDGE_SNAP_M = 1.0

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
# --- V3 : LA REGLE SE DECIDE PAR RUE, PAS PAR TRONCON.
# Verdict utilisateur sur la v2 : « Alsace-Lorraine melange des troncons ayants-droit
# et Libre dans la MEME rue -> des tirets en pointilles par segments ». Mesure : la
# rue d'Alsace-Lorraine du proto compte 18 troncons, 661,6 m « ayants droit » et
# 215,1 m « Libre » — dont 136,5 m de « Libre » EN PLEIN MILIEU de la rue, qui
# portaient donc des tirets isoles entre deux tronces nus. Une rue est un objet
# UNIQUE : son regime se decide sur l'ensemble.
# GROUPE = troncons de MEME NOM de voie, connexes par leurs noeuds. Le nom vient de
# BD TOPO (`nom_voie_ban_gauche`, repli `nom_collaboratif_gauche`) : sur le km2 proto
# il couvre 96,5 % de la longueur et 69 / 69 des troncons eligibles aux tirets. La
# connexite evite de fusionner deux morceaux homonymes separes par la ville.
# REPLI SANS NOM (petites communes, donnee incomplete) : chainage geometrique au
# noeud, meme nature et largeur a AXIAL_RUE_LARGEUR_TOL_M pres, COUPE aux carrefours
# (un noeud partage par 3 troncons ou plus n'enchaine pas) — c'est-a-dire exactement
# la definition d'une rue quand on n'a pas son nom.
# DECISION : si la MAJORITE DE LA LONGUEUR du groupe est « restreinte aux ayants
# droit », le groupe entier perd ses tirets. Sinon il les garde tous.
AXIAL_RUE_MAJORITE = 0.50
AXIAL_RUE_LARGEUR_TOL_M = 1.0
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
_OUVRAGES_CACHE = {}


def axes_ouvrages():
    """Les AXES des ouvrages (side-car SourceData/Ponts), prepares une fois.

    Meme contrat que les autres side-cars : dossier absent = aucun ouvrage, sans
    erreur. Rend (geometrie preparee | None, longueur d'axe totale, nb troncons)."""
    if "u" in _OUVRAGES_CACHE:
        return _OUVRAGES_CACHE["u"], _OUVRAGES_CACHE["m"], _OUVRAGES_CACHE["n"]
    bandes, tot = [], 0.0
    if PONTS_ON and os.path.isdir(PONTS_DIR):
        import glob as _glob
        for p in sorted(_glob.glob(os.path.join(PONTS_DIR, "ponts_*.json"))):
            try:
                with open(p, encoding="utf-8") as f:
                    data = json.load(f)
            except Exception as ex:  # noqa: BLE001
                log("ATTENTION : %s illisible (%s) — ignore" % (os.path.basename(p), ex))
                continue
            for o in data.get("ponts", []):
                pts = [(c[0], c[1]) for c in (o.get("pts") or [])]
                if len(pts) < 2:
                    continue
                ln = LineString(pts)
                if ln.length <= 0.0:
                    continue
                tot += ln.length
                bandes.append(ln.buffer(PONT_APP_TOL_M, cap_style=2, join_style=2))
    u = prep(C.valide(unary_union(bandes))) if bandes else None
    _OUVRAGES_CACHE.update({"u": u, "m": tot, "n": len(bandes),
                            "g": C.valide(unary_union(bandes)) if bandes else None})
    return u, tot, len(bandes)


def est_ouvrage(line):
    """Vrai si `line` est le troncon d'un OUVRAGE : au moins PONT_APP_FRAC de sa
    longueur a moins de PONT_APP_TOL_M d'un axe du side-car Ponts."""
    u, _, _ = axes_ouvrages()
    if u is None or line.length <= 0.0:
        return False
    if not u.intersects(line):
        return False
    return line.intersection(_OUVRAGES_CACHE["g"]).length / line.length >= PONT_APP_FRAC


def charger_routes_bdtopo(fen):
    """Tous les troncons BD TOPO de la fenetre, avec ce qu'il faut pour trancher :
    largeur mesuree, nature, position (pont), nombre de voies."""
    with open(C.ROUTES_PATH, encoding="utf-8") as f:
        data = json.load(f)
    out = []
    stats = {"lus": 0, "gardes": 0, "mesure": 0, "repli": 0, "pont": 0,
             "souterrain": 0, "nature": 0, "autre": 0, "ouvrage": 0}
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
        ligne = LineString(pts)
        # LOT FINITION QUAIS : un troncon d'OUVRAGE est un pont pour le bake, meme
        # si BD TOPO le laisse a pos = 0 (les rampes). Un seul discriminant,
        # `rec["pont"]`, et TOUT ce qui le lit suit : bandes peintes, bordures,
        # bouts pendants, carrefours, tirets axiaux.
        ouvrage = False
        if pos <= 0 and est_ouvrage(ligne):
            ouvrage = True
            stats["ouvrage"] += 1
        rec = {
            "line": ligne,
            "largeur": largeur,
            "nature": nat,
            "pont": pos > 0 or ouvrage,
            "voies": C.as_int(tr.get("nombre_de_voies"), 0) or 0,
            "etroit": nat in C.NATURES_ETROITES,
            "sans_bordure": nat in NATURES_SANS_BORDURE,
            # Qui a le droit de rouler la : sert a la regle nationale de ligne axiale
            # (ACCES_SANS_AXIALE). Normalise en ASCII minuscule comme la nature.
            "acces": acces,
            # V3 : le NOM de la voie regroupe les troncons en RUES. `nom_voie_ban_gauche`
            # d'abord (adresse BAN, la plus complete), repli sur le nom collaboratif.
            # Vide = repli sur le chainage geometrique (cf. rues_de_troncons).
            "nom": (C.norm(tr.get("nom_voie_ban_gauche"))
                    or C.norm(tr.get("nom_collaboratif_gauche")) or ""),
        }
        if rec["pont"]:
            stats["pont"] += 1
        else:
            stats["gardes"] += 1
            stats[source] += 1
        out.append(rec)
    _, m_ouv, n_ouv = axes_ouvrages()
    log("routes BD TOPO : %d troncons dans la fenetre (%d au sol, %d ponts exclus du "
        "masque dont %d RAMPES d'ouvrage a pos <= 0, %d souterrains, %d natures "
        "pietonnes, %d hors service) ; largeurs mesurees %d / replis %d ; side-car "
        "Ponts : %d troncons / %.0f m d'axe"
        % (len(out), stats["gardes"], stats["pont"], stats["ouvrage"], stats["souterrain"],
           stats["nature"], stats["autre"], stats["mesure"], stats["repli"], n_ouv, m_ouv))
    return out


def charger_promenade(cx, cy):
    """QUAIS V3 : les polylignes de promenade de berge d'une cellule.

    Meme contrat que les side-cars de murs et d'escaliers : dossier absent ou
    cellule sans fichier = AUCUNE promenade, sans erreur (cuisson partielle,
    emprise hors zone cuite). Rend [{'pts': [(x,y)...], 'largeur': m}]."""
    path = os.path.join(PROMENADE_DIR, "promenade_%d_%d.json" % (cx, cy))
    if not os.path.exists(path):
        return []
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except Exception as ex:  # noqa: BLE001
        log("ATTENTION : promenade_%d_%d.json illisible (%s) — ignoree" % (cx, cy, ex))
        return []
    if abs(float(data.get("cellSizeM", CELL_M)) - CELL_M) > 0.01:
        log("ATTENTION : promenade_%d_%d.json cuit pour des cellules de %.0f m "
            "(bake a %.0f m) — ignoree" % (cx, cy, data.get("cellSizeM", 0), CELL_M))
        return []
    defaut = float(data.get("largeur_defaut_m", 4.0))
    out = []
    for s in data.get("promenade", []):
        pts = [(float(a), float(b)) for a, b in s.get("pts", [])]
        if len(pts) < 2:
            continue
        out.append({"pts": pts, "largeur": float(s.get("largeur_m", defaut))})
    return out


def charger_bande_quai(cx, cy):
    """LOT SIMPLIFICATION : l'emprise en plan de la bande de quai d'une cellule.

    Meme contrat que les autres side-cars : dossier absent, cellule sans fichier
    ou champ `bande_quai` absent = AUCUNE bande, sans erreur (et donc aucune
    extinction de peinture — le comportement historique, mot pour mot)."""
    path = os.path.join(FRONTIERE_DIR, "frontiere_%d_%d.json" % (cx, cy))
    if not os.path.exists(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except Exception as ex:  # noqa: BLE001
        log("ATTENTION : frontiere_%d_%d.json illisible (%s) — bande ignoree"
            % (cx, cy, ex))
        return None
    anneaux = data.get("bande_quai") or []
    polys = []
    for r in anneaux:
        if len(r) < 4:
            continue
        g = Polygon([(float(a), float(b)) for a, b in r])
        if not g.is_valid:
            g = g.buffer(0)
        if not g.is_empty and g.area > 0:
            polys.append(g)
    if not polys:
        return None
    return C.valide(unary_union(polys))


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


def polygones(g):
    """Les composantes POLYGONALES d'une geometrie shapely, et rien d'autre.

    `C.valide` peut rendre une GeometryCollection (un polygone qui touche un autre
    par un point produit un fil de dimension 1) : le contour d'une telle collection
    est VIDE, et tout ce qui lit le contour de l'herbe en sort silencieusement a
    zero. Piege paye en v3 sur la cellule -1,+0 (10 417 m2 d'herbe, contour mesure a
    0 m). Passer par ici est obligatoire pour tout ce qui parcourt l'herbe."""
    if g is None or g.is_empty:
        return []
    return [p for p in getattr(g, "geoms", [g])
            if p.geom_type == "Polygon" and not p.is_empty]


def contour_de(g):
    """Contour (exterieur + trous) des composantes polygonales, en MultiLineString."""
    ls = []
    for p in polygones(g):
        ls.append(LineString(p.exterior.coords))
        for r in p.interiors:
            ls.append(LineString(r.coords))
    return unary_union(ls) if ls else LineString()


def retirer_miettes(herbe, aire_min=None):
    """MIETTES (herbe v3) : un fragment d'herbe isole plus petit que `aire_min`
    n'est pas une pelouse, c'est un confetti de releve — on le retire.

    Regle SYMETRIQUE de `boucher_trous` : sous le plancher de motif, ni un trou ni
    une tache ne portent d'information de forme. Le seuil est calibre sur la
    distribution mesuree des fragments (cf. HERBE_MIETTE_M2 : un vide de 90 m2
    separe les confettis des vraies pelouses)."""
    retirer_miettes.dernier = {"fragments": 0, "retires": 0, "aire_m2": 0.0}
    amin = HERBE_MIETTE_M2 if aire_min is None else aire_min
    if herbe is None or herbe.is_empty or amin <= 0.0:
        return herbe
    gardes = []
    perdu = 0.0
    for p in polygones(herbe):
        retirer_miettes.dernier["fragments"] += 1
        if p.area < amin:
            retirer_miettes.dernier["retires"] += 1
            perdu += p.area
            continue
        gardes.append(p)
    retirer_miettes.dernier["aire_m2"] = round(perdu, 2)
    if retirer_miettes.dernier["retires"] == 0:
        return herbe
    return C.valide(unary_union(gardes)) if gardes else herbe.difference(herbe)


def regulariser_herbe(herbe, tol=None):
    """REGULARISATION FRANCHE du contour d'herbe (herbe v3), AVANT l'accostage.

    Douglas-Peucker : il ne lisse pas, il DECIME — les sommets qui restent sont des
    sommets d'origine et les angles restent nets. C'est ce qu'on veut : un contour
    dessine a la regle, pas un contour arrondi. L'accostage qui suit recolle ce
    contour sur les frontieres reelles, et la re-soustraction finale d'`accoster_herbe`
    garantit qu'aucun debord ne survit a l'operation."""
    regulariser_herbe.dernier = {"avant": 0, "apres": 0}
    t = HERBE_REGUL_M if tol is None else tol
    if herbe is None or herbe.is_empty or t <= 0.0:
        return herbe
    regulariser_herbe.dernier["avant"] = sum(
        len(p.exterior.coords) + sum(len(r.coords) for r in p.interiors)
        for p in polygones(herbe))
    out = C.valide(herbe.simplify(t, preserve_topology=True))
    regulariser_herbe.dernier["apres"] = sum(
        len(p.exterior.coords) + sum(len(r.coords) for r in p.interiors)
        for p in polygones(out))
    return out


def grass_edges(herbe, cell_box, curbs, u_bati=None, u_eau=None):
    """BORDURETTE (herbe v3) : les polylignes du contour d'herbe FINAL sur lesquelles
    le C++ posera une pierre basse. Memes conventions que `curb_lines` : metres,
    MINERAL A GAUCHE du sens de parcours (le C++ pose la face verticale a gauche et
    le chant vers la droite, donc la face regarde le mineral et le chant deborde de
    14 cm sur la pelouse — exactement une bordurette de jardin).

    Exclusions (cf. GRASS_EDGE_*) : les facades, les bordures de chaussee DEJA
    posees (les polylignes `curbs` de la cellule, pas la frontiere de chaussee), les
    berges, et les segments plus courts que le plancher physique. Le contour est
    d'abord ramene a la CELLULE : la partie qui vit dans la marge de calcul
    appartient a la cellule voisine."""
    grass_edges.dernier = {"contour_m": 0.0, "retenu_m": 0.0, "segments": 0,
                           "courts_m": 0.0, "snaps": 0, "snap_m": 0.0}
    if herbe is None or herbe.is_empty:
        return []
    bnd = contour_de(herbe)
    if bnd.is_empty:
        return []
    grass_edges.dernier["contour_m"] = round(bnd.length, 1)
    coupes = []
    if u_bati is not None and not u_bati.is_empty:
        coupes.append(u_bati.buffer(GRASS_EDGE_CLEAR_M))
    if u_eau is not None and not u_eau.is_empty:
        coupes.append(u_eau.buffer(GRASS_EDGE_CLEAR_M))
    lignes_curb = [LineString(l) for l in (curbs or []) if len(l) > 1]
    if lignes_curb:
        coupes.append(unary_union(lignes_curb).buffer(GRASS_EDGE_CLEAR_M))
    if coupes:
        bnd = bnd.difference(unary_union(coupes))
    bnd = bnd.intersection(cell_box)
    if bnd.is_empty:
        return []
    try:
        merged = linemerge(bnd)
    except Exception:
        merged = bnd
    brut = [g for g in (merged.geoms if merged.geom_type.startswith("Multi") or
                        merged.geom_type == "GeometryCollection" else [merged])
            if g.geom_type == "LineString"]
    pre = prep(herbe)
    out = []
    for g in brut:
        if g.length < GRASS_EDGE_MIN_LEN_M:
            grass_edges.dernier["courts_m"] += g.length
            continue
        s = g.simplify(GRASS_EDGE_SIMPLIFY_M, preserve_topology=False)
        cs = retirer_pointes(list(s.coords))
        if len(cs) < 2:
            continue
        s = LineString(cs)
        if s.length < GRASS_EDGE_MIN_LEN_M:
            grass_edges.dernier["courts_m"] += s.length
            continue
        # ORIENTATION : on SONDE comme curb_lines (le sens des anneaux shapely ne
        # survit pas a la decoupe). Ici c'est l'HERBE qui doit tomber a DROITE.
        cs = list(s.coords)
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
            if droite and not gauche:
                votes += 1
            elif gauche and not droite:
                votes -= 1
        if votes < 0:
            cs.reverse()
        # V4 — SNAP DES EXTREMITES sur la bordure de chaussee voisine. Le rayon de
        # silence de 40 cm autour des `curbs` laisse un trou a chaque raccord : on le
        # referme en ramenant l'extremite sur la polyligne la plus proche, si elle est
        # a moins de GRASS_EDGE_SNAP_M. Au-dela, ce n'est pas un raccord rate : c'est
        # une bordurette qui finit vraiment dans le vide (bouchon de fin cote C++).
        if lignes_curb and GRASS_EDGE_SNAP_M > 0.0:
            u_curb = unary_union(lignes_curb)
            for idx in (0, -1):
                p = Point(cs[idx])
                d = p.distance(u_curb)
                if 1e-9 < d <= GRASS_EDGE_SNAP_M:
                    q = nearest_points(p, u_curb)[1]
                    cs[idx] = (q.x, q.y)
                    grass_edges.dernier["snaps"] += 1
                    grass_edges.dernier["snap_m"] += d
            s = LineString(cs)
        out.append(cs)
        grass_edges.dernier["retenu_m"] += s.length
    grass_edges.dernier["segments"] = len(out)
    grass_edges.dernier["retenu_m"] = round(grass_edges.dernier["retenu_m"], 1)
    grass_edges.dernier["courts_m"] = round(grass_edges.dernier["courts_m"], 1)
    return out



def retracter_peinture(herbe, gedges, recul=None):
    """V4 — LA PEINTURE MEURT SOUS LA PIERRE.

    Rend le polygone d'herbe A PEINDRE : le meme que celui qui a servi a poser la
    bordurette, moins une bande de `recul` metres le long des polylignes de
    bordurette. Le contour VECTORIEL (donc la pierre) n'est pas touche : seule la
    peinture recule, et seulement la ou il y a une pierre pour la couvrir.

    Bande a bouts DROITS (cap_style=2) : un bout arrondi mangerait l'herbe au-dela
    de l'extremite de la pierre, c'est-a-dire exactement la ou il n'y a plus rien
    pour cacher le recul."""
    retracter_peinture.dernier = {"segments": 0, "recul_m": 0.0, "aire_m2": 0.0}
    r = GRASS_PAINT_RETRACT_M if recul is None else recul
    retracter_peinture.dernier["recul_m"] = r
    if herbe is None or herbe.is_empty or not gedges or r <= 0.0:
        return herbe
    lignes = [LineString(l) for l in gedges if len(l) > 1]
    if not lignes:
        return herbe
    retracter_peinture.dernier["segments"] = len(lignes)
    bande = unary_union([ln.buffer(r, cap_style=2, join_style=1) for ln in lignes])
    avant = herbe.area
    out = C.valide(herbe.difference(bande))
    retracter_peinture.dernier["aire_m2"] = round(avant - out.area, 2)
    return out


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
        # V3 : LE QUAI EST BORNE AU VOISINAGE DE L'HERBE (4r = 4 m).
        # Raison premiere, mesuree : le quai contient l'union des 2 611 emprises
        # baties (l'accostage des facades, valide en v2) ; fermer sur cette union
        # entiere coutait 6,5 s pour la seule cellule -1,-1, soit l'essentiel des
        # 30,5 s du bake v3. Borne a 4 m, le bake retombe a 14,7 s pour 4 cellules —
        # le niveau v1, ce que le lot exigeait.
        # Effet mesure sur le resultat (ce n'est PAS une operation neutre, et c'est
        # tant mieux) : herbe 50 107 -> 50 017 m2 (-0,18 %) et 60 -> 39 fragments.
        # Ce qui disparait, ce sont les comblements qui TOUCHAIENT l'herbe par un
        # seul point et couraient ensuite le long d'un mur a 10 ou 50 m de la
        # pelouse — c'est-a-dire des miettes fabriquees par l'accostage lui-meme
        # (miettes retirees : 23 avant la borne, 2 apres). Un accostage borne a 2 m
        # ne doit pas produire de langue a 50 m : la borne remet la regle en accord
        # avec son propre enonce.
        try:
            proche = C.valide(quai.intersection(herbe.buffer(4.0 * r)))
            if not proche.is_empty:
                quai_acc = proche
            else:
                quai_acc = quai
        except Exception:
            quai_acc = quai
        u = C.valide(unary_union([herbe, quai_acc]))
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


def rues_de_troncons(routes):
    """Regroupe les troncons en RUES et rend, pour chaque troncon, l'index de sa rue.

    Deux troncons sont dans la meme rue s'ils portent le MEME NOM de voie et
    partagent un noeud (transitivement). Sans nom, on chaine geometriquement : meme
    noeud, meme nature, largeur a AXIAL_RUE_LARGEUR_TOL_M pres, et JAMAIS a travers
    un carrefour (noeud partage par 3 troncons ou plus) — sinon deux rues qui se
    croisent deviendraient une seule.

    Union-find sur les indices ; la cle de noeud est arrondie au decimetre, la meme
    que junction_points et dangling_ends."""
    n = len(routes)
    parent = list(range(n))

    def trouve(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def unir(i, j):
        a, b = trouve(i), trouve(j)
        if a != b:
            parent[b] = a

    noeuds = {}
    for i, r in enumerate(routes):
        cs = list(r["line"].coords)
        for p in (cs[0], cs[-1]):
            noeuds.setdefault((round(p[0] * 10), round(p[1] * 10)), []).append(i)
    for k, idx in noeuds.items():
        degre = len(idx)
        for a in range(len(idx)):
            for b in range(a + 1, len(idx)):
                ra, rb = routes[idx[a]], routes[idx[b]]
                na, nb = ra.get("nom") or "", rb.get("nom") or ""
                if na and nb:
                    if na == nb:
                        unir(idx[a], idx[b])
                    continue
                # Repli sans nom : chainage geometrique, coupe aux carrefours.
                if degre >= 3:
                    continue
                if (ra.get("nature") == rb.get("nature")
                        and abs(float(ra.get("largeur") or 0.0)
                                - float(rb.get("largeur") or 0.0)) <= AXIAL_RUE_LARGEUR_TOL_M):
                    unir(idx[a], idx[b])
    return [trouve(i) for i in range(n)]


def rues_sans_axiale(routes):
    """Ensemble des indices de RUE dont la MAJORITE DE LA LONGUEUR est « restreinte
    aux ayants droit » : ces rues perdent leur ligne axiale EN ENTIER.

    Une rue est un objet unique — le regime d'acces se lit sur la rue, pas sur le
    decoupage BD TOPO. Rend aussi la table des groupes pour le journal et les
    verrous."""
    groupes = rues_de_troncons(routes)
    tot = {}
    ayd = {}
    for i, r in enumerate(routes):
        g = groupes[i]
        L = float(r["line"].length)
        tot[g] = tot.get(g, 0.0) + L
        if r.get("acces") in ACCES_SANS_AXIALE:
            ayd[g] = ayd.get(g, 0.0) + L
    muettes = {g for g, L in tot.items()
               if L > 0.0 and ayd.get(g, 0.0) > AXIAL_RUE_MAJORITE * L}
    rues_sans_axiale.dernier = {"groupes": len(tot), "muettes": len(muettes),
                                "longueur_muette_m": round(sum(tot[g] for g in muettes), 1)}
    return groupes, muettes


def axial_dashes(routes, chaussee, cell_box, jonctions):
    """Tirets de ligne axiale : voies >= 2, dans la chaussee, a plus de 8 m d'un
    carrefour, et sur une RUE ouverte a la circulation. Le decoupage est fait ICI
    (le C++ ne fait que poser des quads)."""
    if chaussee.is_empty:
        return []
    groupes, muettes = rues_sans_axiale(routes)
    pre = prep(chaussee)
    jx = np.array([p[0] for p in jonctions], dtype=np.float64) if jonctions else None
    jy = np.array([p[1] for p in jonctions], dtype=np.float64) if jonctions else None
    out = []
    period = DASH_ON_M + DASH_OFF_M
    for i, r in enumerate(routes):
        if (r["pont"] or r["etroit"] or r["voies"] < 2
                or r["largeur"] < DASH_MIN_WIDTH_M):
            continue
        # REGLE NATIONALE, PAR RUE (v3) : pas d'axe peint sur une rue dont la majorite
        # de la longueur est « restreinte aux ayants droit » — la decision porte sur
        # la RUE entiere, jamais sur un troncon isole (sinon : tirets en pointilles).
        # Une route sans l'attribut reste ouverte : la regle n'efface jamais un
        # marquage par silence de la donnee.
        if groupes[i] in muettes:
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

    # --- QUAIS V3 : LA PROMENADE DE BERGE, versee dans la classe GRAVIER.
    # Elle vient APRES classes_de_cellule et n'en change aucune regle : c'est une
    # bande de plus dans une classe qui existe deja. On la soustrait de la chaussee
    # et du bati (une promenade ne passe ni sur la rue ni sous un immeuble) mais PAS
    # de l'eau : le quai de la Daurade descend jusqu'au fil de l'eau, et c'est le
    # polygone d'eau qui doit ceder au bord, pas la promenade. La soustraction du
    # corridor n'a pas lieu d'etre non plus : la promenade EST du corridor, elle en
    # est simplement la part identifiee.
    aire_prom = 0.0
    if PROMENADE_ON:
        bandes = []
        for pr in charger_promenade(cx, cy):
            ligne = LineString(pr["pts"])
            if ligne.length <= 0.0:
                continue
            bandes.append(ligne.buffer(pr["largeur"] / 2.0, cap_style=2, join_style=2,
                                       mitre_limit=3.0))
        if bandes:
            bande = C.valide(unary_union(bandes).intersection(zone))
            bande = C.valide(bande.difference(chaussee))
            if u_bati is not None:
                bande = C.valide(bande.difference(u_bati))
            if not bande.is_empty:
                aire_prom = bande.intersection(cell_box).area
                gravier = C.valide(gravier.union(bande))
                # La voirie PRIVEE est calculee sur la zone entiere : sans ce
                # retrait, un morceau de promenade tombe dans une parcelle
                # ressortirait peint en privee par-dessus (meme raison que le
                # retrait du bati dans classes_de_cellule).
                privee = C.valide(privee.difference(bande))

    # --- LOT SIMPLIFICATION : la BANDE DE QUAI ne porte plus de beige.
    # Elle vient APRES la promenade, donc elle eteint aussi la promenade qui
    # tombe dans la bande — c'est exactement ce qui est demande. Hors bande,
    # rien ne bouge. Le retrait sur l'HERBE est plus bas (l'herbe est le
    # complement du mineral peint : sans lui, elle reprendrait la bande).
    bande_quai = charger_bande_quai(cx, cy) if not PEINTURE_BANDE_QUAI else None
    aire_bande_eteinte = 0.0
    if bande_quai is not None and not bande_quai.is_empty:
        avant = gravier.intersection(cell_box).area
        gravier = C.valide(gravier.difference(bande_quai))
        aire_bande_eteinte = avant - gravier.intersection(cell_box).area

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
    if bande_quai is not None and not bande_quai.is_empty and not herbe.is_empty:
        # La bande de quai garde sa CLASSE DE BASE (pavage) : eteindre le beige
        # sans ce retrait ferait simplement reprendre la place par l'herbe.
        herbe = C.valide(herbe.difference(bande_quai))
    boucher_trous.dernier = {"trous": 0, "combles": 0, "aire_m2": 0.0}
    regulariser_herbe.dernier = {"avant": 0, "apres": 0}
    retirer_miettes.dernier = {"fragments": 0, "retires": 0, "aire_m2": 0.0}
    aire_releve = herbe.intersection(cell_box).area     # le releve nu, avant tout
    aire_regul = aire_releve
    if loc_verts:
        # HERBE V3, dans cet ordre et pas un autre :
        #   1. REGULARISER le contour du releve (decimation franche : c'est LA que
        #      l'amibe devient un dessin) ;
        #   2. ACCOSTER (chaussee / privee / gravier / FACADES) : le contour
        #      regularise vient se recoller sur les frontieres reelles, et
        #      `accoster_herbe` finit par re-soustraire le mineral et le bati — donc
        #      la regularisation ne peut pas produire de debord ;
        #   3. RETIRER LES MIETTES : ce qui reste isole sous le plancher de motif.
        # La regularisation vient AVANT l'accostage : l'inverse decollerait l'herbe
        # des murs qu'elle vient d'accoster (Douglas-Peucker deplace le contour des
        # deux cotes), c'est-a-dire qu'elle detruirait le volet valide de la v2.
        herbe = regulariser_herbe(herbe)
        aire_regul = herbe.intersection(cell_box).area
        herbe = accoster_herbe(herbe, chaussee, privee, gravier, u_bati, u_eau)
        herbe = retirer_miettes(herbe)
    trous = dict(getattr(boucher_trous, "dernier",
                         {"trous": 0, "combles": 0, "aire_m2": 0.0}))
    regul = dict(regulariser_herbe.dernier)
    miettes = dict(retirer_miettes.dernier)

    # --- le RELIEF qui restera maille
    # V4 : ce bloc est REMONTE avant la rasterisation. La peinture d'herbe doit
    # reculer sous la pierre, il faut donc savoir OU est la pierre avant de peindre.
    # Aucune de ces trois passes ne depend du raster : elles ne lisent que les
    # geometries vectorielles deja calculees.
    # Les passages viennent des NOEUDS OSM `highway=crossing` deja filtres sur le
    # marquage (parametre `noeuds_pp`). Chaque site coupe la bordure a son droit —
    # c'est legitime maintenant que la traversee est une DONNEE, pas une inference.
    crossings = (crossing_sites(chaussee, cell_box, loc_routes, loc_pp, u_bati)
                 if CROSSINGS_ON else [])
    curbs = curb_lines(chaussee, cell_box, loc_routes, crossings, u_bati)
    jonctions = junction_points(loc_routes)
    dashes = axial_dashes(loc_routes, chaussee, cell_box, jonctions)
    # BORDURETTE (v3) : APRES les bordures de chaussee, parce qu'elle les evite.
    gedges = grass_edges(herbe, cell_box, curbs, u_bati, u_eau)
    ged = dict(grass_edges.dernier)
    # V4 : la PEINTURE (et elle seule) recule sous le chant de la bordurette.
    herbe_peinte = retracter_peinture(herbe, gedges)
    retrait = dict(retracter_peinture.dernier)

    # --- rasterisation (grille de calcul : cellule + marge, a 24,41 cm/px)
    size = OUT_PX * SS + 2 * MARGIN_PX
    ox, oy = x0 - marge_m, y0 - marge_m
    m_corr = rasterize(corridor, size, ox, oy, px_ss)
    m_priv = rasterize(privee, size, ox, oy, px_ss)
    m_grav = rasterize(gravier, size, ox, oy, px_ss)
    m_road = rasterize(chaussee, size, ox, oy, px_ss)
    m_grass = rasterize(herbe_peinte, size, ox, oy, px_ss)
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
    # V4 : la reference du raster est l'herbe PEINTE (retractee sous la pierre) —
    # comparer le PNG a l'herbe vectorielle mesurerait le recul, pas la fidelite.
    aire_herbe_vec = herbe.intersection(cell_box).area
    aire_peinte_vec = herbe_peinte.intersection(cell_box).area
    aire_herbe_png = float((g_grass >= 128).sum()) * (CELL_M / OUT_PX) ** 2

    os.makedirs(OUT_DIR, exist_ok=True)
    png = os.path.join(OUT_DIR, "mask_%d_%d.png" % (cx, cy))
    Image.fromarray(rgba, "RGBA").save(png, "PNG", optimize=True)

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
        # BORDURETTE d'herbe (v3) : memes conventions que `curbs`, mineral A GAUCHE.
        "grassEdges": [[[round(c[0], 2), round(c[1], 2)] for c in ln] for ln in gedges],
        "crossings": crossings,
        "axial": dashes,
        "areasM2": aires,
        # QUAIS V3 : la part de la classe `gravier` qui vient de la promenade de
        # berge. C'est LE discriminant de la passe : a 0, la promenade n'a rien
        # peint et la capture ne peut rien montrer.
        "promenadeM2": round(aire_prom, 1),
        # LOT SIMPLIFICATION : m2 de beige ETEINTS sur la bande de quai.
        "bandeQuaiEteinteM2": round(aire_bande_eteinte, 1),
    }
    js = os.path.join(OUT_DIR, "sols_%d_%d.json" % (cx, cy))
    with open(js, "w", encoding="utf-8") as f:
        json.dump(data, f, separators=(",", ":"))

    if preview:
        apercu(cx, cy, r_chan, rgba[:, :, 1], curbs, crossings, dashes, x0, y0, gedges)

    pc = 100.0 * aires["chaussee"] / aires["cellule"]
    ecart_pc = (100.0 * abs(aire_herbe_png - aire_peinte_vec) / aire_peinte_vec
                if aire_peinte_vec > 1.0 else 0.0)
    log("cellule %+d,%+d : chaussee %.1f %% | herbe %.0f m2 (raster %.0f m2, ecart "
        "%.2f %%) | %d bordures (%.0f m) | %d bordurettes (%.0f m) | %d passages | "
        "%d tirets | masque %.2f Mo | json %.2f Mo | %.1f s"
        % (cx, cy, pc, aire_herbe_vec, aire_herbe_png, ecart_pc,
           len(curbs), sum(LineString(l).length for l in curbs if len(l) > 1),
           len(gedges), ged["retenu_m"],
           len(crossings), len(dashes), os.path.getsize(png) / 1048576.0,
           os.path.getsize(js) / 1048576.0, time.time() - t0))
    log("   herbe v3 : releve %.0f m2 -> regularisation %+.0f m2 (%d -> %d sommets) "
        "-> accostage+trous %+.0f m2 | trous vus %d, combles %d (%.0f m2) | miettes "
        "%d/%d retirees (%.1f m2) | contour %.0f m -> bordurette %.0f m"
        % (aire_releve, aire_regul - aire_releve, regul["avant"], regul["apres"],
           aire_herbe_vec - aire_regul, trous["trous"], trous["combles"],
           trous["aire_m2"], miettes["retires"], miettes["fragments"],
           miettes["aire_m2"], ged["contour_m"], ged["retenu_m"]))
    log("   bordurette v4 : %d extremites recalees sur une bordure de chaussee "
        "(%.2f m cumules) | peinture retractee de %.2f m sous %d pierres "
        "(-%.1f m2, soit %.2f %% de l'herbe)"
        % (ged.get("snaps", 0), ged.get("snap_m", 0.0), retrait["recul_m"],
           retrait["segments"], retrait["aire_m2"],
           100.0 * retrait["aire_m2"] / aire_herbe_vec if aire_herbe_vec > 1.0 else 0.0))
    return {"cell": [cx, cy], "origin": [x0, y0], "png": png, "json": js, "curbs": len(curbs),
            "crossings": len(crossings), "axial": len(dashes),
            "curbLenM": round(sum(LineString(l).length for l in curbs if len(l) > 1), 1),
            "pngBytes": os.path.getsize(png), "jsonBytes": os.path.getsize(js),
            "areasM2": aires, "herbeRasterM2": round(aire_herbe_png, 1),
            "promenadeM2": round(aire_prom, 1),
            "bandeQuaiEteinteM2": round(aire_bande_eteinte, 1),
            "herbeEcartPc": round(ecart_pc, 3),
            "herbeReleveM2": round(aire_releve, 1),
            "herbeRegulM2": round(aire_regul - aire_releve, 1),
            "herbeAccostM2": round(aire_herbe_vec - aire_regul, 1),
            "sommetsAvant": regul["avant"], "sommetsApres": regul["apres"],
            "fragments": miettes["fragments"], "miettes": miettes["retires"],
            "miettesAireM2": miettes["aire_m2"],
            "grassEdges": len(gedges), "grassEdgeLenM": ged["retenu_m"],
            "grassContourM": ged["contour_m"], "grassCourtsM": ged["courts_m"],
            "trousVus": trous["trous"], "trousCombles": trous["combles"],
            "trousAireM2": trous["aire_m2"]}


def apercu(cx, cy, r_chan, g_chan, curbs, crossings, dashes, x0, y0, gedges=None):
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
    for ln in (gedges or []):
        if len(ln) >= 2:
            d.line([tx(c) for c in ln], fill=(230, 40, 200), width=2)
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

    # 3 bis. FINITION QUAIS — l'APPARIEMENT geometrique des troncons d'OUVRAGE.
    #        Le side-car est injecte en dur (hermetique, zero acces disque) : un
    #        ouvrage de 100 m sur l'axe y = 60. On verifie que la RAMPE (meme axe,
    #        pos = 0) est appariee, qu'une route qui passe DESSOUS en travers ne
    #        l'est pas, et qu'une route PARALLELE a 6 m ne l'est pas non plus.
    sauve = dict(_OUVRAGES_CACHE)
    try:
        _OUVRAGES_CACHE.clear()
        axe_ouv = LineString([(0, 60), (100, 60)])
        g_ouv = axe_ouv.buffer(PONT_APP_TOL_M, cap_style=2, join_style=2)
        _OUVRAGES_CACHE.update({"u": prep(g_ouv), "g": g_ouv, "m": 100.0, "n": 1})
        check_bool("ouvrage : la RAMPE sur l'axe est appariee",
                   est_ouvrage(LineString([(10, 60), (60, 60)])), True)
        check_bool("ouvrage : une route qui passe DESSOUS en travers n'est PAS appariee",
                   est_ouvrage(LineString([(50, 0), (50, 100)])), False)
        check_bool("ouvrage : une route PARALLELE a 6 m n'est PAS appariee",
                   est_ouvrage(LineString([(0, 66), (100, 66)])), False)
        check_bool("ouvrage : un troncon a moitie dedans (60 %) est apparie",
                   est_ouvrage(LineString([(40, 60), (100, 60), (100, 100)])), True)
        # Le meme troncon, mais avec seulement 40 % dedans : ecarte.
        check_bool("ouvrage : un troncon a 40 % dedans est ECARTE",
                   est_ouvrage(LineString([(60, 60), (100, 60), (100, 160)])), False)
        # Et sans side-car (dossier absent) : personne n'est apparie.
        _OUVRAGES_CACHE.clear()
        _OUVRAGES_CACHE.update({"u": None, "g": None, "m": 0.0, "n": 0})
        check_bool("ouvrage : side-car absent = aucun appariement",
                   est_ouvrage(LineString([(10, 60), (60, 60)])), False)
    finally:
        _OUVRAGES_CACHE.clear()
        _OUVRAGES_CACHE.update(sauve)

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

    # ------------------------------------------------ FINITION_SOL V3 : verrou 15
    # 15. LIGNE AXIALE : LA REGLE SE DECIDE PAR RUE, PAS PAR TRONCON.
    #     C'est le grief utilisateur de la v2 : Alsace-Lorraine melangeait des
    #     troncons « ayants droit » et « Libre » et sortait en pointilles.
    z_ad = box(0, 0, 400, 200)

    def _tr(x0_, x1_, acces, nom="", y=60.0, larg=9.0, nature="route a 1 chaussee"):
        return {"line": LineString([(x0_, y), (x1_, y)]), "largeur": larg,
                "nature": nature, "pont": False, "voies": 2,
                "etroit": False, "sans_bordure": False, "acces": acces, "nom": nom}

    #  (a) une rue entierement LIBRE garde tous ses tirets ; la meme rue entierement
    #      « ayants droit » n'en garde aucun.
    r_lib = [_tr(0.0, 200.0, "libre", "rue de metz")]
    r_ayd = [_tr(0.0, 200.0, "restreint aux ayants droit", "rue de metz")]
    ch_ad = classes_de_cellule(z_ad, [], [], r_lib)[1]
    n_lib = len(axial_dashes(r_lib, ch_ad, z_ad, []))
    n_ayd = len(axial_dashes(r_ayd, ch_ad, z_ad, []))
    check_bool("axiale : une rue LIBRE garde ses tirets (%d)" % n_lib, n_lib > 20, True)
    check("axiale : zero tiret sur une rue « ayants droit »", float(n_ayd), 0.0, 0.0)
    #  (b) LE CAS ALSACE-LORRAINE : une rue de 4 troncons contigus, 3 « ayants droit »
    #      (150 m) et 1 « Libre » (50 m) AU MILIEU. Par troncon, le troncon libre
    #      sortirait des tirets isoles ; par RUE, la rue entiere est muette.
    r_mix = [_tr(0.0, 60.0, "restreint aux ayants droit", "rue d'alsace lorraine"),
             _tr(60.0, 110.0, "libre", "rue d'alsace lorraine"),
             _tr(110.0, 160.0, "restreint aux ayants droit", "rue d'alsace lorraine"),
             _tr(160.0, 200.0, "restreint aux ayants droit", "rue d'alsace lorraine")]
    ch_mix = classes_de_cellule(z_ad, [], [], r_mix)[1]
    grp, muettes = rues_sans_axiale(r_mix)
    check("axiale par rue : les 4 troncons forment UNE rue",
          float(len(set(grp))), 1.0, 0.0)
    check("axiale par rue : la rue mixte est muette (150 m / 200 m)",
          float(len(muettes)), 1.0, 0.0)
    check("axiale par rue : zero tiret sur la rue mixte",
          float(len(axial_dashes(r_mix, ch_mix, z_ad, []))), 0.0, 0.0)
    #  (c) MINORITE : la meme rue avec 50 m « ayants droit » sur 200 m garde TOUT.
    r_min = [_tr(0.0, 50.0, "restreint aux ayants droit", "rue de bayard"),
             _tr(50.0, 200.0, "libre", "rue de bayard")]
    ch_min = classes_de_cellule(z_ad, [], [], r_min)[1]
    _g, mu_min = rues_sans_axiale(r_min)
    check("axiale par rue : une rue minoritairement restreinte garde son axe",
          float(len(mu_min)), 0.0, 0.0)
    check_bool("axiale par rue : et elle garde bien ses tirets",
               len(axial_dashes(r_min, ch_min, z_ad, [])) > 20, True)
    #  (d) La rue VOISINE, de nom different et pourtant connectee par un noeud, n'est
    #      PAS contaminee : c'est le verrou de non-regression du volet B.
    r_deux = r_mix + [_tr(200.0, 380.0, "libre", "rue de metz")]
    ch_deux = classes_de_cellule(z_ad, [], [], r_deux)[1]
    g2, mu2 = rues_sans_axiale(r_deux)
    check("axiale par rue : deux rues distinctes malgre le noeud partage",
          float(len(set(g2))), 2.0, 0.0)
    n_deux = len(axial_dashes(r_deux, ch_deux, z_ad, []))
    check_bool("axiale par rue : la rue Libre voisine garde ses tirets (%d)" % n_deux,
               n_deux > 15, True)
    #  (e) REPLI SANS NOM : chainage geometrique, mais COUPE au carrefour. Deux
    #      troncons alignes forment une rue ; un troisieme qui arrive en T sur leur
    #      noeud commun casse le chainage (un carrefour n'enchaine pas).
    r_sn = [_tr(0.0, 100.0, "restreint aux ayants droit"),
            _tr(100.0, 200.0, "restreint aux ayants droit")]
    g_sn, mu_sn = rues_sans_axiale(r_sn)
    check("axiale sans nom : deux troncons alignes = une rue",
          float(len(set(g_sn))), 1.0, 0.0)
    r_sn3 = r_sn + [{"line": LineString([(100.0, 60.0), (100.0, 180.0)]),
                     "largeur": 9.0, "nature": "route a 1 chaussee", "pont": False,
                     "voies": 2, "etroit": False, "sans_bordure": False,
                     "acces": "libre", "nom": ""}]
    g_sn3, _m = rues_sans_axiale(r_sn3)
    check("axiale sans nom : le carrefour (3 troncons) COUPE le chainage",
          float(len(set(g_sn3))), 3.0, 0.0)
    #  (f) Une route sans attribut d'acces reste OUVERTE (la regle n'efface jamais un
    #      marquage par silence de la donnee).
    r_sans = [dict(_tr(0.0, 200.0, None, "rue de metz"))]
    del r_sans[0]["acces"]
    check("axiale : attribut absent = voie ouverte",
          float(len(axial_dashes(r_sans, ch_ad, z_ad, []))), float(n_lib), 0.0)

    # ------------------------------------------------ FINITION_SOL V3 : verrou 16
    # 16. HERBE V3 : facades (conserve de la v2), trous internes (conserve),
    #     REGULARISATION, MIETTES et BORDURETTE. La regle de compartiment de la v2 a
    #     ete RETIREE : ce bloc est reecrit, pas troue.
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
    #  (c) MIETTES : regle SYMETRIQUE du bouchage de trous. Un confetti part, une
    #      vraie pelouse reste, et l'aire retiree est exactement celle des confettis.
    gros = box(0.0, 0.0, 40.0, 40.0)                       # 1 600 m2
    moyen = box(60.0, 0.0, 65.0, 5.0)                      # 25 m2 : au-dessus du seuil
    miette1 = box(80.0, 0.0, 82.0, 2.0)                    # 4 m2
    miette2 = box(90.0, 0.0, 91.0, 1.0)                    # 1 m2
    sem = C.valide(unary_union([gros, moyen, miette1, miette2]))
    net = retirer_miettes(sem)
    check("miettes : 4 fragments vus", float(retirer_miettes.dernier["fragments"]), 4.0, 0.0)
    check("miettes : 2 retires", float(retirer_miettes.dernier["retires"]), 2.0, 0.0)
    check("miettes : 5 m2 retires", sem.area - net.area, 5.0, 0.01)
    check_bool("miettes : la pelouse de 25 m2 (au-dessus du seuil) est INTACTE",
               net.intersection(moyen).area > 24.9, True)
    check_bool("miettes : le gros fragment est INTACT",
               abs(net.intersection(gros).area - gros.area) < 1e-6, True)
    #  (d) REGULARISATION : un contour de releve qui gigote a l'echelle du metre est
    #      DECIME (les sommets qui restent sont des sommets d'origine : angles nets),
    #      et elle ne peut pas deborder, l'accostage re-soustrayant le mineral.
    dents = [(0.0, 0.0)]
    for i in range(1, 100):
        dents.append((i * 1.0, 0.9 if i % 2 else 0.0))
    dents += [(100.0, 0.0), (100.0, 60.0), (0.0, 60.0)]
    releve = C.valide(Polygon(dents))
    reg = regulariser_herbe(releve)
    check_bool("regularisation : le contour est decime (%d -> %d sommets)"
               % (regulariser_herbe.dernier["avant"], regulariser_herbe.dernier["apres"]),
               regulariser_herbe.dernier["apres"]
               < regulariser_herbe.dernier["avant"] / 3.0, True)
    check_bool("regularisation : l'aire est preservee a 2 %",
               abs(reg.area - releve.area) / releve.area < 0.02, True)
    route_r = box(-20.0, -20.0, 120.0, -1.0)               # du mineral sous la dent
    apres = accoster_herbe(C.valide(reg.union(box(0.0, -1.0, 100.0, 0.0))),
                           route_r, vide, vide)
    check("regularisation : zero debord sur le mineral apres re-soustraction",
          apres.intersection(route_r).area, 0.0, 1e-6)
    #  (e) BORDURETTE : une pelouse carree posee sur la dalle, un mur colle a un cote
    #      et une bordure de chaussee le long d'un autre. Il doit rester DEUX cotes
    #      bordes, mineral A GAUCHE, et rien le long du mur ni de la bordure.
    cb = box(0.0, 0.0, 200.0, 200.0)
    pel = box(50.0, 50.0, 150.0, 150.0)                    # 100 x 100
    mur_b = box(150.0, 40.0, 160.0, 160.0)                 # colle au cote EST (x=150)
    curb_b = [[(50.0, 48.0), (50.0, 152.0)]]               # bordure le long du cote OUEST
    ge = grass_edges(pel, cb, curb_b, mur_b, None)
    ltot = sum(LineString(l).length for l in ge)
    check_bool("bordurette : au moins un segment pose (%d segments, %.0f m)"
               % (len(ge), ltot), len(ge) >= 1 and ltot > 150.0, True)
    check_bool("bordurette : rien le long du MUR",
               all(LineString(l).distance(mur_b) > 0.30 for l in ge), True)
    # V4 : le verrou porte desormais sur le CORPS de la bordurette, pas sur ses bouts.
    # Le snap v4 POSE volontairement une extremite sur la bordure de chaussee voisine
    # (c'est le correctif « raccords ») : exiger 30 cm partout interdirait le raccord
    # qu'on vient d'ajouter. Ce qui reste interdit, et qui est le vrai grief d'origine,
    # c'est de POSER UNE DEUXIEME PIERRE LE LONG d'une pierre existante.
    def corps(ligne, marge=1.5):
        s = LineString(ligne)
        if s.length <= 2.0 * marge + 0.5:
            return None
        return substring(s, marge, s.length - marge)

    corps_ok = True
    for l in ge:
        c_ = corps(l)
        if c_ is not None and c_.distance(LineString(curb_b[0])) <= 0.30:
            corps_ok = False
    check_bool("bordurette : rien LE LONG de la bordure de chaussee existante "
               "(bouts exclus : le raccord v4 y est colle expres)", corps_ok, True)
    check_bool("bordurette : le total vaut les deux cotes libres (200 m, obtenu %.0f)"
               % ltot, 180.0 < ltot < 205.0, True)
    # ORIENTATION : mineral a gauche <=> herbe a droite, sur chaque segment.
    ok_or = True
    for l in ge:
        for i in range(len(l) - 1):
            ax, ay = l[i]
            bx, by = l[i + 1]
            dx, dy = bx - ax, by - ay
            n_ = math.hypot(dx, dy)
            if n_ < 0.5:
                continue
            dx, dy = dx / n_, dy / n_
            mx, my = (ax + bx) * 0.5, (ay + by) * 0.5
            if not pel.contains(Point(mx + dy * 0.10, my - dx * 0.10)):
                ok_or = False
            if pel.contains(Point(mx - dy * 0.10, my + dx * 0.10)):
                ok_or = False
    check_bool("bordurette : l'herbe est A DROITE, le mineral A GAUCHE", ok_or, True)
    #  (f) BORDURETTE : un BOUT de contour plus court que le plancher n'est pas pose.
    #      Pelouse ceinturee de bordures de chaussee sauf un trou de 1 m : apres les
    #      rayons de silence il ne reste que 20 cm de contour libre — trop court pour
    #      une pierre. On ne pose rien plutot qu'un caillou de 20 cm.
    pel_f = box(0.0, 0.0, 10.0, 10.0)
    curb_f = [[(5.5, 0.0), (10.0, 0.0), (10.0, 10.0), (0.0, 10.0), (0.0, 0.0), (4.5, 0.0)]]
    ge_court = grass_edges(pel_f, box(-10.0, -10.0, 20.0, 20.0), curb_f, None, None)
    check("bordurette : le trou de 1 m entre deux bordures ne produit rien",
          float(len(ge_court)), 0.0, 0.0)
    check_bool("bordurette : ce bout est bien compte comme ecarte (trop court)",
               grass_edges.dernier["courts_m"] > 0.0, True)

    # ---- 17. BORDURETTE V4 : la peinture meurt sous la pierre, les bouts se recalent
    #      (a) RETRACTION : une pelouse carree de 20 m bordee sur UN cote perd
    #          exactement recul x longueur de peinture, et RIEN de plus. Le polygone
    #          vectoriel (donc la pierre) n'est pas touche.
    pel_r = box(0.0, 0.0, 20.0, 20.0)
    ge_r = [[(0.0, 0.0), (20.0, 0.0)]]
    peint = retracter_peinture(pel_r, ge_r, 0.10)
    check("v4 retraction : la peinture perd recul x longueur (0,10 x 20 = 2 m2)",
          pel_r.area - peint.area, 2.0, 0.02)
    check("v4 retraction : le polygone d'ORIGINE est intact (la pierre ne bouge pas)",
          pel_r.area, 400.0, 0.001)
    check_bool("v4 retraction : le recul se fait bien DANS l'herbe (y=0,05 sorti)",
               peint.contains(Point(10.0, 0.05)), False)
    check_bool("v4 retraction : au-dela du recul l'herbe est intacte (y=0,20 garde)",
               peint.contains(Point(10.0, 0.20)), True)
    check_bool("v4 retraction : le cote NON borde ne recule pas (x=0,05 garde)",
               peint.contains(Point(0.05, 10.0)), True)
    check("v4 retraction : sans bordurette, aucun recul",
          retracter_peinture(pel_r, [], 0.10).area, 400.0, 0.001)
    check("v4 retraction : recul nul = polygone inchange",
          retracter_peinture(pel_r, ge_r, 0.0).area, 400.0, 0.001)
    #      (b) BOUTS DROITS : une bande a bouts arrondis mangerait l'herbe au-dela de
    #          l'extremite de la pierre — la ou plus rien ne cache le recul.
    ge_court_r = [[(5.0, 0.0), (15.0, 0.0)]]
    peint_c = retracter_peinture(pel_r, ge_court_r, 0.10)
    check_bool("v4 retraction : bouts DROITS (pas de morsure au-dela de la pierre)",
               peint_c.contains(Point(4.90, 0.05)), True)
    #      (c) SNAP DES EXTREMITES : pelouse dont le contour libre s'arrete a 40 cm
    #          d'une bordure de chaussee (le rayon de silence). La bordurette doit
    #          RECALER son extremite sur la polyligne de bordure.
    pel_s = box(0.0, 0.0, 30.0, 30.0)
    curb_s = [[(-5.0, -1.0), (35.0, -1.0)]]   # bordure droite sous la pelouse
    ge_s = grass_edges(pel_s, box(-10.0, -10.0, 40.0, 40.0), curb_s, None, None)
    u_curb_s = unary_union([LineString(l) for l in curb_s])
    dmin = min(min(Point(l[0]).distance(u_curb_s), Point(l[-1]).distance(u_curb_s))
               for l in ge_s) if ge_s else 99.0
    check_bool("v4 snap : au moins une extremite est POSEE sur la bordure (d = 0)",
               dmin < 1e-6, True)
    check_bool("v4 snap : le recalage est compte",
               grass_edges.dernier.get("snaps", 0) > 0, True)
    #      (d) une extremite VRAIMENT loin (3 m) n'est pas aimantee : ce n'est pas un
    #          raccord rate, c'est une bordurette qui finit dans le vide.
    curb_l = [[(-5.0, -3.5), (35.0, -3.5)]]
    ge_l = grass_edges(pel_s, box(-10.0, -10.0, 40.0, 40.0), curb_l, None, None)
    u_curb_l = unary_union([LineString(l) for l in curb_l])
    dmin_l = min(min(Point(l[0]).distance(u_curb_l), Point(l[-1]).distance(u_curb_l))
                 for l in ge_l) if ge_l else 0.0
    check_bool("v4 snap : au-dela de 1 m, aucune extremite n'est deplacee",
               dmin_l > 1.0, True)

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
    # La fenetre de CHARGEMENT porte la marge de calcul du SDF plus une reserve. Le
    # halo de 80 m de la regle de compartiment (v2) a disparu avec la regle : c'est
    # lui qui faisait passer la cuisson de 14 s a 49 s pour 4 cellules.
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
    log("QUAIS V3 : %d m2 de PROMENADE DE BERGE dans la classe gravier, sur %d cellules"
        % (sum(r.get("promenadeM2", 0.0) for r in resume),
           sum(1 for r in resume if r.get("promenadeM2", 0.0) > 0.0)))
    log("PEINT   : %d m2 de chaussee, %d m2 de voirie privee, %d m2 de gravier, "
        "%d m2 d'HERBE (ecart raster max %.2f %%)"
        % (sum(r["areasM2"]["chaussee"] for r in resume),
           sum(r["areasM2"]["privee"] for r in resume),
           sum(r["areasM2"]["gravier"] for r in resume),
           sum(r["areasM2"]["herbe"] for r in resume),
           max((r["herbeEcartPc"] for r in resume), default=0.0)))
    log("HERBE V3: releve %d m2 -> regularisation %+d m2 (%d -> %d sommets) -> "
        "accostage+trous %+d m2 | trous vus %d, combles %d (%.0f m2) | miettes %d/%d "
        "retirees (%.1f m2)"
        % (sum(r["herbeReleveM2"] for r in resume),
           sum(r["herbeRegulM2"] for r in resume),
           sum(r["sommetsAvant"] for r in resume),
           sum(r["sommetsApres"] for r in resume),
           sum(r["herbeAccostM2"] for r in resume),
           sum(r["trousVus"] for r in resume), sum(r["trousCombles"] for r in resume),
           sum(r["trousAireM2"] for r in resume),
           sum(r["miettes"] for r in resume), sum(r["fragments"] for r in resume),
           sum(r["miettesAireM2"] for r in resume)))
    log("MAILLE  : %d polylignes de bordure (%.0f m), %d bordurettes d'herbe (%.0f m "
        "sur %.0f m de contour, %.0f m ecartes trop courts), %d passages, %d tirets"
        % (sum(r["curbs"] for r in resume), sum(r["curbLenM"] for r in resume),
           sum(r["grassEdges"] for r in resume), sum(r["grassEdgeLenM"] for r in resume),
           sum(r["grassContourM"] for r in resume), sum(r["grassCourtsM"] for r in resume),
           sum(r["crossings"] for r in resume), sum(r["axial"] for r in resume)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
