# -*- coding: utf-8 -*-
"""L1a — LA MATRICE DE COHERENCE + LE REGISTRE DES REGLES.

⛔ TABLE RASE. Le registre part VIDE. Une regle n'y entre que TIREE par une
case de la matrice, et son contenu est cherche DANS CET ORDRE :
    ① le REEL   — norme BTP / CEREMA / accessibilite / genie civil, CITEE ;
    ② la DONNEE — un attribut reellement mesure (L0, side-cars, MNT) ;
    ③ sinon     — la case passe en ARBITRAGE et je ne tranche pas.
Aucune regle de l'ancien pipeline n'est importee. Quand une regle ancienne
merite de revivre, elle est RE-DERIVEE ici depuis sa norme d'origine et
proposee comme telle (le champ `re_derivee` le dit).

La matrice croise TOUTES les familles retenues contre TOUTES. Chaque case
recoit UN statut : CONTRAT (avec son invariant chiffre cible 0 et la mesure qui
le verifiera) · AUCUNE INTERACTION (justifiee) · ARBITRAGE (borne).
Invariant du lot : 0 case sans statut.
"""
import io
import json
import os
import sys
import time

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import CACHE, chrono, jalon

# ============================================================ LES FAMILLES ====
# niveau : 'sol' (porte le sol) · 'sur' (au-dessus du sol) · 'sous' (enterre)
# dim    : 'L' lineaire · 'S' surfacique · 'P' ponctuel
# porte_sol : la famille EST une surface de sol praticable
F = []


def fam(cle, nom, meca, dim, niveau, porte_sol, matiere, note):
    F.append({"cle": cle, "nom": nom, "meca": meca, "dim": dim,
              "niveau": niveau, "porte_sol": porte_sol, "matiere": matiere,
              "note": note})


# ① GABARIT LINEAIRE
fam("chaussee", "chaussee (rues par classe)", "1", "L", "sol", True, "mineral",
    "profil en travers extrude sur l'axe du graphe")
fam("trottoir", "trottoir / bande de rive", "1", "L", "sol", True, "mineral",
    "partie du gabarit de la rue, pas un objet separe")
fam("voie_ferree", "voie ferree (plateforme + ballast)", "1", "L", "sol", True,
    "mineral", "arbitrage coordinateur : gabarit ①")
fam("canal", "canal (cuvette + berges + halage)", "1", "L", "sol", True, "eau",
    "arbitrage coordinateur : gabarit ①, pas une simple surface d'eau")
fam("piste_aero", "piste d'aerodrome", "1", "L", "sol", True, "mineral",
    "arbitrage coordinateur : gabarit ① tres large / ④")
# ② PIECE NODALE
fam("carrefour", "carrefour", "2", "S", "sol", True, "mineral",
    "plateau au noeud du graphe")
fam("rond_point", "rond-point (anneau + ilot central)", "2", "S", "sol", True,
    "mineral", "arbitrage coordinateur : piece nodale ②")
fam("echangeur", "echangeur (composition de bretelles)", "2", "S", "sol", True,
    "mineral", "arbitrage coordinateur : piece nodale ②, bretelles denivelees")
# ③ OBJET D'OUVRAGE
fam("pont", "pont / viaduc / passerelle", "3", "L", "sur", True, "mineral",
    "tablier porte, franchit un autre element")
fam("dalot", "ouvrage affleurant (dalot, buse, ponceau)", "3", "L", "sol",
    True, "mineral",
    "la route passe DESSUS et rien ne passe dessous : hauteur declaree "
    "inferieure au plus petit gabarit de passage du reel (2,20 m). Ni "
    "epaisseur de tablier, ni intrados, ni hauteur libre — mais la continuite "
    "du dessus avec la chaussee qui le franchit.")
fam("escalier", "escalier / emmarchement", "3", "L", "sol", True, "mineral",
    "raccorde deux cotes de sol")
fam("gradins", "gradins", "3", "S", "sol", True, "mineral", "assises etagees")
fam("mur_sout", "mur de soutenement / mur de quai", "3", "L", "sol", False,
    "mineral", "retient une difference de terrain ou borde l'eau")
fam("ouvrage_hydro", "ouvrage hydraulique (ecluse, barrage, seuil, vanne, "
    "pont-canal)", "3", "S", "sol", False, "eau",
    "arbitrage coordinateur : ③ ; separe deux biefs de cotes differentes")
fam("aqueduc", "aqueduc", "3", "L", "sur", False, "eau",
    "arbitrage coordinateur : ③ ; canal porte")
fam("edicule", "edicule d'emergence (metro, gare, parking souterrain)", "3",
    "S", "sol", False, "mineral",
    "arbitrage coordinateur : ③ ; le sous-sol n'existe que par ses emergences")
fam("tremie", "tremie / tete de tunnel", "3", "L", "sol", True, "mineral",
    "arbitrage coordinateur : ③ ; la voirie plonge sous le sol")
# ④ SURFACE DE PLAN
fam("sol_mineral", "sol mineral (place, parvis, cour, esplanade)", "4", "S",
    "sol", True, "mineral", "surface de plan a loi de Z")
fam("sol_vegetal", "sol vegetal (parc, pelouse, bois)", "4", "S", "sol", True,
    "vegetal", "surface de plan a loi de Z")
fam("eau_surface", "surface d'eau (bief, plan d'eau)", "4", "S", "sol", True,
    "eau", "surface a cote de bief")
fam("parking", "parking (surface reglee)", "4", "S", "sol", True, "mineral",
    "arbitrage coordinateur : surface ④")
fam("terrain_sport", "terrain de sport (assiette plane)", "4", "S", "sol",
    True, "mineral", "arbitrage coordinateur : surface ④ a assiette plane")
fam("batiment", "emprise de batiment (assiette / pad)", "4", "S", "sol", False,
    "mineral", "le sol s'y arrete ; la 3D est posee sur l'assiette")
# ⑤ INSTANCE PONCTUELLE
fam("semis", "semis d'instances (arbres, haies)", "5", "P", "sur", False,
    "vegetal", "positions candidates filtrees par le plan")
# TRANSVERSALES
fam("terrassement", "terrassement (talus calcule, soutenement)", "T", "S",
    "sol", True, "mineral",
    "emprise de raccordement entre un element regle et le terrain")
fam("breakline", "talus / levee DECLARES par la donnee (breaklines)", "T", "L",
    "sol", False, "mineral",
    "arbitrage coordinateur : DONNEE d'entree du terrassement")
fam("terrain_naturel", "terrain naturel drape (MNT)", "T", "S", "sol", True,
    "vegetal", "le plan n'y ecrit aucun Z : le moteur drape")
fam("sous_sol", "reseau souterrain hors emergences (metro, tunnel, "
    "canalisation, parking enterre)", "S", "L", "sous", False, "mineral",
    "la REGLE DU SOUS-SOL : il n'existe dans le plan que par ses emergences ; "
    "sa ligne dans la matrice sert a rendre cette absence EXPLICITE plutot que "
    "silencieuse")

CLES = [f["cle"] for f in F]
FAM = {f["cle"]: f for f in F}

# ======================================================== LE CATALOGUE DES ===
# ============================================================== CONTRATS =====
CONTRATS = {
    "ancrage": "les deux portent la MEME cote au contact (l'un impose, "
               "l'autre suit)",
    "appui": "l'objet declare son emprise d'appui ; le sol est REGLE a la cote "
             "de l'objet sur cette assiette",
    "exclusion": "les deux emprises ne peuvent pas se recouvrir en plan",
    "degagement": "l'un franchit l'autre : hauteur libre garantie sous "
                  "l'ouvrage",
    "jonction": "le contact est resolu par une PIECE du catalogue "
                "(bordure / mur / emmarchement / talus / affleurement / rien)",
    "continuite": "les deux sont la MEME surface : ressaut borne au contact",
}

# ============================================================= LE REGISTRE ====
REG = []
_vus = {}


def regle(cle, enonce, provenance, reference, invariant, mesure, cellules,
          re_derivee=False):
    """Ajoute une regle AU MOMENT OU une case la tire. Une regle deja presente
    voit seulement s'ajouter la case qui l'exige : le registre reste la liste
    des besoins reels, pas un catalogue a priori."""
    if cle in _vus:
        r = _vus[cle]
        if cellules not in r["cellules"]:
            r["cellules"].append(cellules)
        return cle
    r = {"cle": cle, "enonce": enonce, "provenance": provenance,
         "reference": reference, "invariant": invariant, "mesure": mesure,
         "cellules": [cellules], "re_derivee": bool(re_derivee)}
    REG.append(r)
    _vus[cle] = r
    return cle


# ---- les regles NOMMEES, chacune tiree par au moins une case -----------------
def R_ressaut(cell):
    return regle(
        "ressaut_max", "Au contact de deux surfaces praticables continues, le "
        "ressaut n'excede pas 2 cm (au-dela il doit etre traite par une piece "
        "declaree : bordure, marche, talus).", "reel",
        "arrete du 15/01/2007 relatif a l'accessibilite de la voirie (ressaut "
        "<= 2 cm, 4 cm si chanfreine a 1/3)", "0 contact continu dont le "
        "ressaut depasse 0,02 m", "dZ echantillonne le long de la frontiere "
        "(mediane et max), compare a 0,02 m", cell, re_derivee=True)


def R_bordure(cell):
    return regle(
        "bordure_vue", "Une bordure de trottoir presente une vue nominale de "
        "0,14 m, bornee a 0,20 m ; au-dela ce n'est plus une bordure mais un "
        "mur.", "reel",
        "norme NF EN 1340 / profils CEREMA T2-CC1 (vue courante 12-14 cm) ; "
        "guide CEREMA de la voirie urbaine",
        "0 piece `bordure` dont la hauteur sort de [0,02 ; 0,20] m",
        "hauteur de la piece = dZ mesure au contact, compare aux bornes",
        cell, re_derivee=True)


def R_pente_voirie(cell):
    return regle(
        "pente_voirie", "La pente en long d'une chaussee urbaine reste sous "
        "12 % ; un cheminement accessible reste sous 5 %.", "reel",
        "guide CEREMA « Voirie urbaine » (pente courante) ; arrete du "
        "15/01/2007 (cheminement accessible 5 %)",
        "0 troncon dont la pente depasse son plafond de classe sans etre "
        "declare hors norme", "pente de chaque segment du profil en long, "
        "comparee au plafond de la classe", cell, re_derivee=True)


def R_assiette(cell):
    return regle(
        "assiette", "Tout objet declare (pont, escalier, gradins, batiment, "
        "ouvrage hydraulique, edicule) declare son EMPRISE D'APPUI ; le "
        "terrain est regle a la cote de l'objet sur cette emprise. Le terrain "
        "s'adapte a l'objet, jamais l'inverse.", "reel",
        "regle de l'art du genie civil : un ouvrage se fonde sur une assiette "
        "nivelee (DTU 13 fondations superficielles ; terrassements "
        "prealables)", "0 objet declare dont l'assiette n'est pas reglee a sa "
        "cote", "ecart entre la cote du sol sous l'assiette et la cote "
        "declaree de l'objet ; max et p95", cell)


def R_seuil_bati(cell):
    return regle(
        "seuil_batiment", "Le sol fini au droit d'une entree de batiment est "
        "au niveau du seuil, avec un ressaut <= 2 cm ; l'assiette du batiment "
        "domine le terrain voisin d'une marge d'egouttage.", "reel",
        "arrete du 15/01/2007 (acces aux ERP et logements : ressaut <= 2 cm) ; "
        "DTU 20.1 (protection au pied d'ouvrage)",
        "0 emprise de batiment dont l'ecart au sol voisin depasse la marge "
        "declaree sans piece declaree",
        "dZ entre l'assiette et le sol adjacent, le long du contact", cell)


def R_talus(cell):
    return regle(
        "talus_pente", "Un raccordement en terre entre deux cotes se fait par "
        "un talus a pente bornee ; au-dela de cette pente le raccord exige un "
        "ouvrage de soutenement.", "reel",
        "regles de terrassement routier (GTR / SETRA-CEREMA) : talus courant "
        "3H/2V en deblai, 2H/1V en remblai selon les sols",
        "0 raccord terrain dont la pente depasse la pente de talus admissible "
        "sans soutenement declare",
        "pente du raccord mesuree sur l'emprise de terrassement", cell)


def R_ouvrage_affleurant(cell):
    return regle(
        "ouvrage_affleurant", "Un ouvrage de franchissement dont la hauteur "
        "DECLAREE est inferieure au plus petit gabarit de passage du reel "
        "(2,20 m) est un ouvrage AFFLEURANT : dalot, buse ou ponceau. La route "
        "passe dessus et rien ne passe dessous. Il ne porte ni epaisseur de "
        "tablier, ni intrados, ni exigence de hauteur libre ; il doit en "
        "revanche etre CONTINU avec la chaussee qui le franchit.", "donnee",
        "side-car Ponts, champ `hauteur_moy_m` : mediane 2,07 m sur 95 "
        "objets, 26 % sous 1,00 m, 13 % sous 0,50 m ; sur le plan l'extrados "
        "declare n'est qu'a +0,29 m du terrain (mediane). Le seuil de 2,20 m "
        "est celui du cheminement pieton, deja au registre (arrete du "
        "15/01/2007)",
        "0 ouvrage affleurant portant une exigence de hauteur libre, et "
        "0 ouvrage affleurant discontinu avec sa chaussee",
        "hauteur declaree de chaque ouvrage comparee au seuil ; puis dZ au "
        "contact entre l'ouvrage affleurant et la chaussee qui le franchit",
        cell)


def R_epaisseur_tablier(cell):
    return regle(
        "epaisseur_tablier", "L'epaisseur d'un tablier vaut sa portee divisee "
        "par 20, bornee a [0,40 ; 2,50] m ; la cote d'intrados est la cote "
        "d'extrados moins cette epaisseur.", "reel",
        "regle de l'art des ouvrages d'art : elancement d'un tablier courant "
        "~ portee/20 (arbitrage coordinateur rendu ; la donnee ne porte NI "
        "l'epaisseur NI la position des appuis, la portee est donc celle "
        "d'une travee unique et le contrat le dit)",
        "0 tablier sans cote d'intrados declaree",
        "epaisseur et cote d'intrados calculees pour chaque tablier, et "
        "comparaison de l'intrados au gabarit de la voie franchie", cell)


GABARITS_M = {
    "route": 4.30,
    "route_revanche": 4.75,
    "voie_ferree": 6.00,
    "canal": 3.70,
    "chaussee_pietonne": 2.20,
}


def R_hauteur_libre(cell):
    return regle(
        "hauteur_libre", "Un ouvrage qui en franchit un autre garantit la "
        "hauteur libre normalisee de la voie franchie : route 4,30 m (4,75 m "
        "avec revanche de renforcement), voie ferree electrifiee 6,00 m, "
        "voie d'eau 3,70 m de tirant d'air, cheminement pieton 2,20 m. La "
        "mesure se fait contre la cote d'INTRADOS declaree du tablier.",
        "reel",
        "route : instruction technique sur les ouvrages d'art (hauteur libre "
        "4,30 m, portee a 4,75 m pour la revanche de renforcement de "
        "chaussee) · fer : gabarit des installations fixes sous catenaire "
        "(6,00 m au-dessus du plan de roulement, referentiels SNCF Reseau) · "
        "eau : tirant d'air VNF, 3,70 m pour le gabarit Freycinet · pieton : "
        "arrete du 15/01/2007, hauteur libre de 2,20 m sur un cheminement "
        "accessible",
        "0 franchissement dont la hauteur libre est inferieure au gabarit de "
        "la voie franchie",
        "cote d'intrados declaree du tablier moins cote de la surface "
        "franchie au droit du croisement, comparee au gabarit de la classe "
        "franchie", cell)


def R_bief(cell):
    return regle(
        "bief_plat", "Un bief est PLAT : une surface d'eau porte une cote "
        "unique ; deux biefs de cotes differentes ne se touchent que par un "
        "ouvrage hydraulique declare.", "donnee",
        "side-cars Eau : z_min_ngf_m / z_max_ngf_m mesures par bief (L0 : "
        "52 biefs cotes)",
        "0 contact direct entre deux biefs de cotes differentes sans ouvrage",
        "amplitude z_max - z_min par bief, et dZ aux contacts bief|bief", cell)


def R_exclusion(cell):
    return regle(
        "exclusion_emprise", "Deux emprises de sol ne se recouvrent jamais : "
        "la partition du sol est disjointe et couvre 100 % du domaine.",
        "reel", "principe de partition (une surface, un proprietaire) : regle "
        "d'arpentage et de conduite de chantier — deux ouvrages ne peuvent "
        "occuper le meme sol au meme niveau",
        "0 m2 de recouvrement entre deux emprises de meme niveau ; couverture "
        "100,000000 %", "aire d'intersection deux a deux, et aire du domaine "
        "moins l'union", cell)


def R_gabarit_largeur(cell):
    return regle(
        "gabarit_largeur", "La largeur d'un gabarit lineaire est celle que "
        "porte la donnee ; a defaut, la largeur normalisee de sa classe.",
        "donnee", "BD TOPO 3.5 : `largeur_de_chaussee` et `nombre_de_voies` "
        "renseignes (L0 : 733 858 m de largeur cumulee sur 228 335 troncons)",
        "0 troncon construit sans largeur (de la donnee ou de sa classe)",
        "part des troncons dont la largeur vient de la donnee vs de la classe",
        cell)


def R_semis_dur(cell):
    return regle(
        "semis_hors_dur", "Aucune instance semee ne vit sur une surface "
        "minerale construite.", "reel",
        "evidence physique : un arbre ne pousse pas dans une chaussee ; "
        "les fosses de plantation sont des ouvrages declares",
        "0 instance retenue dont la position tombe sur une surface minerale",
        "test de la position de chaque instance contre les emprises minerales, "
        "verifie sur les PIXELS des masques et non sur une vectorisation",
        cell)


def R_continuite_gabarit(cell):
    return regle(
        "continuite_gabarit", "Deux gabarits lineaires de meme nature qui se "
        "suivent forment une surface continue : meme cote au raccord.",
        "reel", "regles de l'art de la voirie : une chaussee est une surface "
        "continue ; les raccords de profil se font sans ressaut",
        "0 raccord entre deux troncons de meme nature dont le dZ depasse le "
        "ressaut", "dZ au contact des deux emprises, mediane et max", cell)


def R_ilot(cell):
    return regle(
        "ilot_central", "Un rond-point se compose d'un anneau de chaussee et "
        "d'un ilot central non circulable, separes par une bordure "
        "franchissable ou non.", "reel",
        "guide CEREMA « Carrefours giratoires » (anneau, ilot central, bande "
        "franchissable)",
        "0 rond-point sans ilot central declare",
        "presence et aire de l'ilot pour chaque piece nodale de type "
        "rond-point", cell)


def R_devers(cell):
    return regle(
        "devers", "Le profil en travers d'une chaussee porte un devers "
        "d'assainissement ; la surface n'est jamais rigoureusement horizontale "
        "en travers.", "reel",
        "guide CEREMA de la voirie urbaine : devers courant 2 % a 2,5 % pour "
        "l'ecoulement des eaux",
        "0 gabarit de chaussee sans devers declare",
        "devers lu sur le gabarit de chaque classe", cell)


def R_emergence(cell):
    return regle(
        "emergence_sous_sol", "Un ouvrage souterrain n'existe dans le plan que "
        "par ses EMERGENCES declarees (edicules, tetes de tunnel, tremies) ; "
        "son trace enterre ne contraint pas le sol.", "arbitrage",
        "decision coordinateur (§Familles) : emergences -> ③ edicules simples, "
        "tunnel = emergences + tremies — a confirmer en maquette",
        "0 ouvrage souterrain qui contraint le sol ailleurs qu'a ses "
        "emergences declarees",
        "liste des ouvrages souterrains et de leurs emergences ; verification "
        "qu'aucune autre contrainte n'est posee", cell)


def R_breakline(cell):
    return regle(
        "breakline_donnee", "Les talus et levees DECLARES par la donnee sont "
        "des lignes de rupture d'entree du terrassement : le terrain les "
        "respecte au lieu de les lisser.", "arbitrage",
        "decision coordinateur (§Familles) : talus/levees declares = DONNEE "
        "d'entree du terrassement (breaklines) — a confirmer en maquette ; "
        "volumetrie mesuree en L0 : 7 883 objets, 1 858 km",
        "0 breakline declaree que le terrain traverse en la lissant",
        "ecart entre le terrain calcule et la ligne de rupture declaree, le "
        "long de la breakline", cell)


def R_assiette_plane(cell):
    return regle(
        "assiette_plane_sport", "Une aire de jeu reglementaire est PLANE : sa "
        "surface porte une cote unique, aux tolerances de la discipline.",
        "reel", "reglements federaux d'installations sportives (planeite et "
        "pente maximale des aires de jeu, typiquement <= 1 %)",
        "0 terrain de sport dont l'assiette porte plus d'une cote",
        "amplitude de cote sur l'emprise du terrain", cell)


def R_fidelite(cell):
    return regle(
        "fidelite_releve", "Hors des emprises declarees, le niveau projet ne "
        "s'ecarte pas du releve au-dela d'une borne declaree : lisser sans "
        "denaturer.", "donnee",
        "MNT RGE ALTI 1 m, parite de lecture prouvee (ecart median 0,28 mm "
        "entre la lecture Python et le releve du monde rendu)",
        "0 point hors emprise declaree dont |z_projet - z_releve| depasse la "
        "borne", "distribution de |z_projet - z_releve| sur le domaine, hors "
        "emprises declarees", cell)


def R_croisement(cell):
    return regle(
        "type_croisement", "Le type de croisement entre deux gabarits "
        "lineaires (a niveau ou denivele) est LU DANS LA DONNEE : passage a "
        "niveau declare, ou ouvrage de franchissement declare. La donnee "
        "muette n'autorise pas a deviner.", "donnee",
        "BD TOPO 3.5 : couche `point_du_reseau` nature « Passage a niveau » "
        "(145 objets mesures en L0) et couche `construction_lineaire` nature "
        "« Pont » (3 664 objets, 231 km de constructions lineaires mesurees)",
        "0 croisement d'axes dont le type n'est ni declare a niveau ni declare "
        "denivele",
        "croisement des axes deux a deux, puis recherche d'un passage a niveau "
        "ou d'un ouvrage declare a moins de la tolerance ; le reliquat est "
        "chiffre et part en arbitrage", cell)


def R_rive(cell):
    return regle(
        "piece_de_rive", "Un contact terre|eau recoit une piece de rive : mur "
        "de quai la ou la donnee declare un ouvrage de rive, berge en talus "
        "sinon.", "donnee",
        "BD TOPO 3.5 : `construction_lineaire` nature « Quai » (96 objets) et "
        "« Mur de soutenement » (292 objets) ; side-cars Murs portant un "
        "profil crete/pied mesure (195 objets)",
        "0 metre de contact terre|eau sans piece de rive determinee",
        "longueur de contact terre|eau, repartie entre mur de quai declare et "
        "berge ; la part dont la pente de berge depasserait le talus "
        "admissible est chiffree et part en arbitrage", cell)


def R_marche(cell):
    return regle(
        "geometrie_marche", "Une marche d'escalier ou de gradin respecte "
        "hauteur <= 0,16 m et giron >= 0,28 m, et la relation de Blondel "
        "(2 hauteurs + 1 giron compris entre 0,60 et 0,65 m) ; toutes les "
        "marches d'une meme volee sont identiques.", "reel",
        "arrete du 15/01/2007 (escaliers de la voirie : h <= 16 cm, "
        "giron >= 28 cm) et regle de Blondel (2h + g = 60 a 65 cm)",
        "0 volee dont une marche sort de ces bornes ou differe des autres",
        "hauteur et giron de chaque marche generee, compares aux bornes et "
        "entre eux", cell, re_derivee=True)


def R_echangeur(cell):
    return regle(
        "composition_echangeur", "La composition d'un echangeur (nombre, "
        "ordre et niveaux de ses bretelles) n'est pas determinee.",
        "arbitrage",
        "la donnee ne porte qu'un point `equipement_de_transport` "
        "nature_detaillee « Echangeur complet » (43 objets) ou « Echangeur "
        "partiel » mesures en L0, sans composition ni niveaux ; le reel offre "
        "deux conduites : composer depuis les bretelles du graphe routier, ou "
        "declarer l'echangeur comme UNE piece nodale unique",
        "0 echangeur construit sans composition declaree",
        "pour chaque echangeur : nombre de bretelles rattachees et niveaux "
        "distincts trouves dans le graphe", cell)


# ==================================================== LA REGLE DE LA MATRICE ==
PROPRES = {
    "chaussee": ["pente_voirie", "devers", "gabarit_largeur"],
    "trottoir": ["gabarit_largeur", "ressaut_max"],
    "voie_ferree": ["gabarit_largeur", "pente_voirie"],
    "canal": ["gabarit_largeur", "bief_plat"],
    "piste_aero": ["gabarit_largeur", "assiette_plane_sport"],
    "rond_point": ["ilot_central"],
    "escalier": ["geometrie_marche"],
    "dalot": ["ouvrage_affleurant"],
    "gradins": ["geometrie_marche"],
    "echangeur": ["composition_echangeur"],
    "terrain_sport": ["assiette_plane_sport"],
    "parking": ["assiette_plane_sport"],
    "eau_surface": ["bief_plat"],
    "terrain_naturel": ["fidelite_releve"],
    "terrassement": ["talus_pente"],
    "breakline": ["breakline_donnee"],
    "edicule": ["emergence_sous_sol"],
    "tremie": ["emergence_sous_sol", "pente_voirie"],
    "batiment": ["seuil_batiment"],
    "semis": ["semis_hors_dur"],
}
FABRIQUES = {}


def tire_propres(cle, cell):
    """La diagonale d'une famille TIRE les regles qui lui sont propres."""
    out = []
    for r in PROPRES.get(cle, []):
        f = FABRIQUES.get(r)
        if f:
            out.append(f(cell))
    return out


def statut(a, b):
    """Rend (statut, contrat, invariant, mesure, regles, justification).

    L'ordre des tests EST la doctrine : d'abord ce que le reel impose
    (exclusion, appui, franchissement), ensuite la continuite des surfaces,
    enfin la jonction par piece. Ce qui ne tombe dans rien devient ARBITRAGE.
    """
    A, B = FAM[a], FAM[b]
    cell = "%s x %s" % (a, b)
    reg = []

    # -- LE SOUS-SOL : aucune interaction, SAUF par ses emergences ----------
    if "sous_sol" in (a, b):
        autre = b if a == "sous_sol" else a
        if a == b:
            reg += [R_emergence(cell)]
            return ("CONTRAT", "exclusion",
                    "0 reseau souterrain qui contraigne un autre reseau "
                    "souterrain dans le plan (ils ne s'y rencontrent pas)",
                    "liste des contraintes posees par chaque reseau "
                    "souterrain : elle doit etre vide hors emergences", reg,
                    None)
        if autre in ("edicule", "tremie"):
            reg += [R_emergence(cell), R_assiette(cell)]
            return ("CONTRAT", "appui",
                    "0 reseau souterrain dont l'emergence n'est pas declaree "
                    "et reglee sur son assiette",
                    "pour chaque reseau souterrain : ses emergences declarees, "
                    "et l'ecart assiette/sol a chacune", reg, None)
        reg += [R_emergence(cell)]
        return ("AUCUNE INTERACTION", None, None, None, reg,
                "regle du sous-sol : un reseau enterre ne contraint le sol "
                "nulle part ailleurs qu'a ses emergences declarees, qui sont "
                "traitees par les familles `edicule` et `tremie`")

    # -- ECHANGEUR : la composition n'est ni dans le reel ni dans la donnee --
    COMPOSANTS = ("chaussee", "pont", "tremie", "carrefour", "rond_point",
                  "echangeur")
    if "echangeur" in (a, b) and (
            (a == "echangeur" and b in COMPOSANTS)
            or (b == "echangeur" and a in COMPOSANTS)):
        reg += [R_echangeur(cell), R_hauteur_libre(cell)]
        return ("ARBITRAGE", None, None, None, reg,
                "composer l'echangeur depuis les bretelles du graphe, ou le "
                "declarer comme UNE piece nodale unique : deux contrats "
                "plausibles, la donnee ne porte que le point d'echangeur")

    # -- 0. la diagonale : la famille avec elle-meme -------------------------
    if a == b:
        reg += tire_propres(a, cell)
        if A["dim"] == "P":
            return ("AUCUNE INTERACTION", None, None, None, [],
                    "deux instances ponctuelles ne se rencontrent pas : elles "
                    "n'ont pas d'emprise de sol")
        if A["porte_sol"]:
            reg += [R_exclusion(cell), R_continuite_gabarit(cell),
                    R_ressaut(cell)]
            return ("CONTRAT", "continuite",
                    "0 recouvrement entre deux emprises de la famille ET "
                    "0 raccord dont le ressaut depasse 0,02 m ENTRE DEUX "
                    "PARCELLES DE LA MEME SURFACE (meme objet source, meme "
                    "porteur, meme communaute, meme bief) ; deux surfaces "
                    "distinctes de la meme famille se rencontrent par une "
                    "PIECE du catalogue",
                    "aire d'intersection deux a deux ; dZ au contact, "
                    "restreint aux paires de la meme surface", reg, None)
        reg += [R_exclusion(cell)]
        return ("CONTRAT", "exclusion",
                "0 m2 de recouvrement entre deux emprises de la famille",
                "aire d'intersection deux a deux", reg, None)

    # -- 1. un ponctuel contre le reste --------------------------------------
    if "P" in (A["dim"], B["dim"]):
        autre = B if A["dim"] == "P" else A
        if autre["porte_sol"] and autre["matiere"] == "mineral":
            reg += [R_semis_dur(cell)]
            return ("CONTRAT", "exclusion",
                    "0 instance semee sur cette surface minerale",
                    "position de chaque instance testee contre l'emprise, sur "
                    "les pixels du masque", reg, None)
        if autre["matiere"] == "vegetal" or autre["cle"] == "terrain_naturel":
            return ("CONTRAT", "appui",
                    "0 instance dont l'altitude d'assise s'ecarte du sol "
                    "porteur de plus de la tolerance declaree",
                    "z de l'instance moins z du sol a sa position",
                    [R_assiette(cell)], None)
        if autre["matiere"] == "eau":
            return ("AUCUNE INTERACTION", None, None, None, [],
                    "une instance vegetale ne se seme pas dans l'eau libre : "
                    "la surface d'eau n'est pas une assise")
        return ("CONTRAT", "exclusion",
                "0 instance semee sur l'emprise de cet objet",
                "position de chaque instance testee contre l'emprise",
                [R_semis_dur(cell)], None)

    # -- 2. un element PORTE (au-dessus) franchit un element de sol -----------
    porte = [x for x in (A, B) if x["niveau"] == "sur"]
    sol = [x for x in (A, B) if x["niveau"] == "sol"]
    if porte and sol:
        reg += [R_hauteur_libre(cell), R_epaisseur_tablier(cell),
                R_assiette(cell)]
        return ("CONTRAT", "degagement",
                "0 franchissement dont la hauteur libre est inferieure au "
                "gabarit de la voie franchie, ET 0 appui de l'ouvrage qui ne "
                "soit pas regle sur son assiette",
                "cote d'intrados moins cote de la surface franchie au droit du "
                "croisement ; ecart assiette/objet aux appuis", reg, None)
    if len(porte) == 2:
        reg += [R_hauteur_libre(cell)]
        return ("CONTRAT", "degagement",
                "0 croisement de deux ouvrages portes sans hauteur libre "
                "declaree entre eux",
                "difference des cotes d'intrados et d'extrados au croisement",
                reg, None)

    # -- 3. un objet declare qui NE porte PAS de sol : appui + exclusion ------
    objets = [x for x in (A, B) if not x["porte_sol"]]
    sols = [x for x in (A, B) if x["porte_sol"]]
    if len(objets) == 2:
        reg += [R_exclusion(cell)]
        return ("CONTRAT", "exclusion",
                "0 m2 de recouvrement entre les deux emprises",
                "aire d'intersection des deux emprises", reg, None)
    if len(objets) == 1 and len(sols) == 1:
        o, s = objets[0], sols[0]
        if o["cle"] == "batiment":
            reg += [R_assiette(cell), R_seuil_bati(cell), R_exclusion(cell)]
            return ("CONTRAT", "appui",
                    "0 emprise de batiment dont l'assiette n'est pas reglee a "
                    "sa cote, ET 0 contact au sol dont le ressaut depasse la "
                    "marge de seuil declaree",
                    "ecart assiette/sol sur l'emprise ; dZ le long du contact",
                    reg, None)
        if o["cle"] == "mur_sout":
            reg += [R_talus(cell), R_assiette(cell)]
            return ("CONTRAT", "jonction",
                    "0 difference de terrain superieure a la pente de talus "
                    "admissible qui ne soit pas reprise par un soutenement "
                    "declare",
                    "dZ de part et d'autre du mur, compare a la pente de talus",
                    reg, None)
        if o["cle"] == "ouvrage_hydro":
            # CORRECTION L1b-2 : une ecluse, un barrage, un seuil SEPARENT deux
            # niveaux — c'est leur fonction meme. Exiger l'egalite des cotes de
            # part et d'autre etait un contresens (mesure : 9 contacts sur 9
            # fautifs). Le contact est une piece du catalogue, un mur.
            reg += [R_bief(cell), R_assiette(cell), R_bordure(cell)]
            return ("CONTRAT", "jonction",
                    "0 contact ouvrage hydraulique | surface sans piece du "
                    "catalogue (un ouvrage qui separe deux biefs porte un mur, "
                    "il n'egalise pas les cotes)",
                    "type de piece attribue au contact et dZ compare a ses "
                    "bornes", reg, None)
        if o["cle"] == "breakline":
            reg += [R_breakline(cell), R_talus(cell)]
            return ("CONTRAT", "ancrage",
                    "0 breakline declaree que la surface traverse en la "
                    "lissant",
                    "ecart entre la surface calculee et la ligne de rupture",
                    reg, None)
        if o["cle"] == "edicule":
            reg += [R_assiette(cell), R_emergence(cell), R_exclusion(cell)]
            return ("CONTRAT", "appui",
                    "0 edicule dont l'assiette n'est pas reglee, ET 0 "
                    "contrainte posee au sol ailleurs qu'a l'emergence",
                    "ecart assiette/sol ; liste des contraintes posees", reg,
                    None)
        reg += [R_assiette(cell), R_exclusion(cell)]
        return ("CONTRAT", "appui",
                "0 objet dont l'assiette n'est pas reglee a sa cote sur "
                "l'emprise qu'il declare",
                "ecart entre la cote du sol sous l'assiette et la cote de "
                "l'objet", reg, None)

    # -- 4. deux surfaces de sol : continuite, jonction, ou ancrage -----------
    ma, mb = A["matiere"], B["matiere"]
    if "eau" in (ma, mb) and ma != mb:
        reg += [R_bief(cell), R_rive(cell), R_talus(cell)]
        return ("CONTRAT", "jonction",
                "0 contact terre|eau sans piece de rive determinee : mur de "
                "quai la ou un ouvrage est declare par la donnee, berge en "
                "talus sinon",
                "pour chaque metre de contact terre|eau : presence d'un "
                "ouvrage de rive declare a moins de la tolerance ; le reste "
                "recoit une berge, et les cas ou la pente exigee depasse le "
                "talus admissible alimentent la liste d'arbitrage", reg, None)
    if ma == "eau" and mb == "eau":
        reg += [R_bief(cell)]
        return ("CONTRAT", "ancrage",
                "0 contact entre deux surfaces d'eau de cotes differentes sans "
                "ouvrage hydraulique declare",
                "dZ au contact des deux surfaces d'eau", reg, None)
    meme_meca = A["meca"] == B["meca"]
    lineaires = {"chaussee", "trottoir", "voie_ferree", "canal", "piste_aero"}
    nodales = {"carrefour", "rond_point", "echangeur"}
    if a in lineaires and b in lineaires:
        if {a, b} == {"chaussee", "trottoir"}:
            reg += [R_bordure(cell), R_ressaut(cell), R_devers(cell)]
            return ("CONTRAT", "jonction",
                    "0 contact chaussee|trottoir dont la hauteur sort des "
                    "bornes de la bordure [0,02 ; 0,20] m",
                    "dZ mesure le long du contact, compare aux bornes", reg,
                    None)
        reg += [R_gabarit_largeur(cell), R_croisement(cell),
                R_ressaut(cell)]
        return ("CONTRAT", "jonction",
                "0 croisement de deux gabarits lineaires dont le type (a "
                "niveau ou denivele) n'est pas lu dans la donnee",
                "pour chaque croisement d'axes : presence d'un passage a "
                "niveau declare, d'un ouvrage de franchissement declare, ou "
                "d'aucun des deux (ce dernier cas alimente la liste "
                "d'arbitrage)", reg, None)
    if ("trottoir" in (a, b)) and (a in nodales or b in nodales):
        # CORRECTION L1b-2 : un trottoir borde la piece nodale comme il borde
        # la chaussee — a +14 cm. Leur contact est une BORDURE declaree, pas
        # une continuite. Mesure qui l'a revele : 838 contacts fautifs sur
        # 1 214, tous au voisinage de la vue de bordure.
        reg += [R_bordure(cell), R_ressaut(cell)]
        return ("CONTRAT", "jonction",
                "0 contact trottoir|piece nodale dont la hauteur sort des "
                "bornes de la bordure [0,02 ; 0,20] m",
                "dZ mesure le long du contact, compare aux bornes", reg, None)
    if (a in lineaires and b in nodales) or (a in nodales and b in lineaires):
        reg += [R_continuite_gabarit(cell), R_ressaut(cell)]
        return ("CONTRAT", "continuite",
                "0 raccord dont le ressaut depasse 0,02 m entre une piece "
                "nodale et un gabarit qui lui est INCIDENT (c'est ce que la "
                "piece nodale promet) ; un gabarit qui passe a cote sans y "
                "aboutir releve de la jonction",
                "dZ au contact, restreint aux gabarits incidents au noeud",
                reg, None)
    if a in nodales and b in nodales:
        reg += [R_exclusion(cell), R_ressaut(cell)]
        return ("CONTRAT", "exclusion",
                "0 recouvrement entre deux pieces nodales, et 0 ressaut "
                "superieur a 0,02 m si elles se touchent",
                "aire d'intersection ; dZ au contact", reg, None)
    if "terrain_naturel" in (a, b):
        reg += [R_talus(cell), R_fidelite(cell)]
        return ("CONTRAT", "jonction",
                "0 rencontre entre un element regle et le terrain naturel sans "
                "emprise de raccordement calculee",
                "presence et pente de l'emprise de terrassement a chaque "
                "rencontre", reg, None)
    if "terrassement" in (a, b):
        reg += [R_talus(cell), R_ressaut(cell)]
        return ("CONTRAT", "continuite",
                "0 raccord terrassement|surface dont le ressaut depasse "
                "0,02 m", "dZ au contact de l'emprise de raccordement", reg,
                None)
    if "escalier" in (a, b) or "gradins" in (a, b):
        reg += [R_marche(cell), R_assiette(cell), R_ressaut(cell)]
        return ("CONTRAT", "jonction",
                "0 raccord escalier/gradins avec une surface dont la premiere "
                "et la derniere marche ne tombent pas au niveau des sols "
                "raccordes (ressaut <= 0,02 m aux deux extremites)",
                "dZ entre la marche d'extremite et le sol raccorde, aux deux "
                "bouts", reg, None)
    if ma == mb:
        # ⚠️ CORRECTION apportee par la MESURE (L1b). « Meme matiere donc
        # continuite » etait une derivation trop forte : entre une chaussee et
        # une place minerale, le reel pose une BORDURE — les deux surfaces sont
        # minerales sans etre la MEME surface. La mesure le disait : 71,1 % des
        # contacts chaussee|sol_mineral depassaient le ressaut, non par defaut
        # du plan mais parce que l'invariant demandait la mauvaise chose.
        # Le contrat juste est la JONCTION, qui subsume le cas de plain-pied
        # (la piece vaut alors `affleurement`). La CONTINUITE reste pour les
        # surfaces qui sont reellement la meme : meme famille (diagonale),
        # gabarit contre piece nodale, terrassement contre sa surface.
        reg += [R_bordure(cell), R_ressaut(cell)]
        return ("CONTRAT", "jonction",
                "0 contact entre deux surfaces de meme matiere mais de "
                "familles differentes sans piece du catalogue",
                "type de piece attribue a chaque contact, et dZ compare aux "
                "bornes de la piece", reg, None)
    reg += [R_bordure(cell), R_ressaut(cell)]
    return ("CONTRAT", "jonction",
            "0 contact entre deux matieres differentes sans piece du catalogue",
            "type de piece attribue a chaque contact, et dZ compare aux bornes "
            "de la piece", reg, None)


FABRIQUES.update({
    "ressaut_max": R_ressaut, "bordure_vue": R_bordure,
    "pente_voirie": R_pente_voirie, "assiette": R_assiette,
    "seuil_batiment": R_seuil_bati, "talus_pente": R_talus,
    "hauteur_libre": R_hauteur_libre, "bief_plat": R_bief,
    "exclusion_emprise": R_exclusion, "gabarit_largeur": R_gabarit_largeur,
    "semis_hors_dur": R_semis_dur, "continuite_gabarit": R_continuite_gabarit,
    "ilot_central": R_ilot, "devers": R_devers,
    "emergence_sous_sol": R_emergence, "breakline_donnee": R_breakline,
    "assiette_plane_sport": R_assiette_plane, "fidelite_releve": R_fidelite,
    "type_croisement": R_croisement, "piece_de_rive": R_rive,
    "geometrie_marche": R_marche, "composition_echangeur": R_echangeur,
    "epaisseur_tablier": R_epaisseur_tablier,
    "ouvrage_affleurant": R_ouvrage_affleurant,
})


def main():
    t0 = time.time()
    cells = []
    for i, a in enumerate(CLES):
        for b in CLES[i:]:
            st, contrat, inv, mes, reg, just = statut(a, b)
            cells.append({"a": a, "b": b, "statut": st, "contrat": contrat,
                          "contrat_def": CONTRATS.get(contrat),
                          "invariant": inv, "mesure": mes,
                          "regles": sorted(set(reg)),
                          "justification": just})
    sans = [c for c in cells if not c["statut"]]
    cpt = {}
    for c in cells:
        cpt[c["statut"]] = cpt.get(c["statut"], 0) + 1
    prov = {}
    for r in REG:
        prov[r["provenance"]] = prov.get(r["provenance"], 0) + 1
    arb = [c for c in cells if c["statut"] == "ARBITRAGE"]
    jalon("L1a/⭐ MATRICE : %d familles -> %d cases uniques (%d x %d avec la "
          "symetrie) ; %s ; 0 case sans statut : %s"
          % (len(CLES), len(cells), len(CLES), len(CLES),
             " | ".join("%s %d" % (k, v) for k, v in sorted(cpt.items())),
             "VERIFIE" if not sans else "ECHEC (%d)" % len(sans)))
    jalon("L1a/⭐ REGISTRE (parti VIDE, %d regles tirees par les cases) : %s ; "
          "%d regles re-derivees d'une norme au lieu d'etre importees"
          % (len(REG), " | ".join("%s %d" % (k, v)
                                  for k, v in sorted(prov.items())),
             sum(1 for r in REG if r["re_derivee"])))
    jalon("L1a/⚠️ CASES ARBITRAGE (%d, liste bornee) : %s"
          % (len(arb), " ; ".join("%s x %s" % (c["a"], c["b"])
                                  for c in arb[:12])))
    rep = {"familles": F, "contrats": CONTRATS, "cases": cells,
           "registre": REG, "compteurs_cases": cpt,
           "compteurs_provenance": prov,
           "arbitrages": arb, "cases_sans_statut": len(sans)}
    with io.open(os.path.join(CACHE, "l1a_matrice.json"), "w",
                 encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(rep, indent=1, sort_keys=True, ensure_ascii=False))
    chrono("L1a/matrice", time.time() - t0,
           "%d cases, %d regles" % (len(cells), len(REG)))


if __name__ == "__main__":
    main()
