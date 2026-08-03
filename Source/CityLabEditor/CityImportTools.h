#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "CityImportTools.generated.h"

/**
 * Profil de generation (jalon J2) : un seul pipeline, deux jeux de budgets.
 * La construction par defaut EST le profil MOBILE verrouille — golden path des
 * tests : la generation mobile reste bit-a-bit identique a l'existant.
 * bDesktop=true (appel MCP minimal) : Resolved() bascule vers le prereglage
 * desktop tout champ laisse a sa valeur mobile ; un champ renseigne
 * explicitement est conserve. Spec : Doc/J2-ProfilDesktop.md §4.
 */
USTRUCT(BlueprintType)
struct FCityGenProfile
{
	GENERATED_BODY()

	/** Prereglage desktop : releve les budgets et active le drapage MNT (voir Resolved()). */
	UPROPERTY() bool bDesktop = false;

	/** Subdivisions de la grille de sol par cellule (12 mobile ~42 m/sommet, 64 desktop ~7,8 m). */
	UPROPERTY() int32 GroundGridN = 12;

	/** Grille du trimesh de collision dedie du sol. 0 = boite simple (mobile), 16 desktop. */
	UPROPERTY() int32 GroundCollisionGridN = 0;

	/** Pas de re-echantillonnage des polylignes (routes, rails) avant extrusion, cm. 0 = aucun. */
	UPROPERTY() float RoadResampleStepCm = 0.f;

	/** Drape sol, routes, surfaces, batiments, arbres et reperes sur le MNT (rebase Capitole = z0). */
	UPROPERTY() bool bDrapeToTerrain = false;

	/** Socle enterre des batiments : le mur descend a ZBase - (MaxAlt - MinAlt) - SocleCm. */
	UPROPERTY() float SocleCm = 50.f;

	/** PNG 16 bits du MNT (valeur = alt NGF en cm). Vide = SourceData/toulouse10_mnt.png. */
	UPROPERTY() FString TerrainPngPath;

	/** JSON de georeferencement du MNT. Vide = SourceData/toulouse10_mnt.json. */
	UPROPERTY() FString TerrainJsonPath;

	/**
	 * Lot B (spec §3.3) : fenetres en creux GEOMETRIQUES completes — vitre en retrait
	 * 18 cm + 4 quads de tableaux + appui saillant (3 quads) + linteau saillant
	 * (2 quads) = +9 quads (+18 tris) par fenetre par rapport a la vitre en simple
	 * retrait. Mobile : fenetres dans la texture de facade, inchange.
	 */
	UPROPERTY() bool bWindowReveals = false;

	/**
	 * Lot B : chaque cellule batiments produit DEUX meshes — SM_Bldg_*_Wall (opaque,
	 * Nanite-compatible) et SM_Bldg_*_Glass (vitres) — au lieu des
	 * deux slots d'un seul mesh (spec Q3 : un mesh est Nanite ou ne l'est pas).
	 */
	UPROPERTY() bool bSplitWallGlass = false;

	/** Lot B : Nanite sur TOUS les meshes generes du profil desktop — murs, sol,
	 * routes, proxys ET vitres (J2e, retours utilisateur du 25/07 : le verre est
	 * OPAQUE ; vitres non-Nanite + murs Nanite = fenetres qui « flottent »). */
	UPROPERTY() bool bNanite = false;

	/**
	 * Lot B : materiaux PBR DefaultLit (Lumen) generes par code — M_CityWall_PBR
	 * (atlas facades x VertexColor), M_CityGlass_PBR, M_CityGround_PBR,
	 * M_CityRoad_PBR — a la place de l'unlit a ombrage cuit. Les vertex colors
	 * passent en encodage LINEAIRE (le pow 2.2 est un hack unlit, garde en mobile),
	 * l'ombrage soleil cuit Shade() disparait (Lumen eclaire) et l'UV1 monde
	 * (x,y normalise sur la dalle 10 km, ortho-ready J3) est ecrite sur sol,
	 * routes et batiments (toits).
	 */
	UPROPERTY() bool bPBRMaterials = false;

	/** Cote en pixels de l'atlas de facades T_CityAtlas (grille 4x4 sous-tuiles). */
	UPROPERTY() int32 AtlasSizePx = 2048;

	/**
	 * J3c point 2 « builder sols » : revetements Megascans par CLASSE de surface.
	 * Chaque route et chaque polygone vert part dans un groupe de polygones dedie
	 * (un slot de materiau par classe) avec une UV0 EN METRES ; le materiau
	 * <SurfacesFolder>/<slug>/M_Surf_<slug> divise par la taille physique du scan.
	 * Materiau absent = repli silencieux sur le materiau historique du slot :
	 * le golden path mobile et les tests sans assets Megascans sont inchanges.
	 */
	UPROPERTY() bool bSurfaceMaterials = false;

	/** Dossier des packs de revetements importes. Vide = /Game/City/Surfaces. */
	UPROPERTY() FString SurfacesFolder;

	/**
	 * J3c point 3 « voirie », assainissement des espaces verts. Par DEFAUT (false) :
	 * UNE SEULE herbe (grass_cut) pour tous les polygones verts — verdict utilisateur
	 * sur le proto v4b, « spaghetti des espaces verts » : l'alternance uncut/wild par
	 * graine de polygone, cumulee aux chevauchements de polygones OSM, faisait de
	 * chaque parc un patchwork de trois herbes differentes. true : l'alternance
	 * historique revient (classes conservees en code pour un usage futur berges et
	 * friches, ou la variete se justifiera).
	 */
	UPROPERTY() bool bVariedGrass = false;

	/**
	 * J3c « maquette du sol » : LE SOL EST PEINT, LE RELIEF EST MAILLE.
	 * La dalle d'une cellule qui possede un masque cuit (Tools/j3c_sols_masks.py)
	 * recoit l'instance <GroundMasksAssetFolder>/MI_CityGround_<x>_<y>, laquelle
	 * melange trottoir / chaussee / voirie privee / gravier d'apres le masque de
	 * la cellule. En echange, PLUS AUCUN ruban de chaussee n'est genere au niveau
	 * du sol : la chaussee n'est plus un film pose sur la dalle, elle EST la dalle.
	 * Les PONTS gardent leur ruban (un tablier ne se peint pas sur le terrain), et
	 * le relief se reduit a ce qui merite de la geometrie — bordures, passages
	 * pietons, tirets de ligne axiale — lu dans le JSON de la cellule.
	 * Masque ou JSON absent pour une cellule = comportement actuel pour elle
	 * (le golden path mobile, lui, n'entre jamais dans cette branche).
	 */
	UPROPERTY() bool bMaskedGround = false;

	/** Dossier des masques cuits (sols_<x>_<y>.json). Vide = <projet>/SourceData/Sols. */
	UPROPERTY() FString GroundMasksPath;

	/** Dossier des instances de materiau de sol (MI_CityGround_<x>_<y>). Vide = /Game/City/Ground. */
	UPROPERTY() FString GroundMasksAssetFolder;

	/**
	 * J3b : chemin d'un JSON batiments dedie (toulouse10_bati.json, produit par
	 * Tools/j3b_prep_toits.py) — anneaux nettoyes CCW + bloc "roof" optionnel
	 * (egout/delta/materiau + faces du squelette droit precalcule). Vide = la
	 * section "buildings" du JSON principal (toits plats historiques). Les routes,
	 * arbres et surfaces restent dans le JSON principal.
	 */
	UPROPERTY() FString BuildingsJsonPath;

	/**
	 * J3f DA « marbre blanc taille » : ZERO coloriage. Murs, toits (plats ET en pente)
	 * ET proxys forcent une teinte marbre blanche et la sous-tuile pierre claire (12),
	 * en ignorant UsageTint / UsageTile et la teinte ortho du toit (Roof->Tint). Purement
	 * additif hors profil desktop batiments ; defaut false = comportement historique.
	 */
	UPROPERTY() bool bMarbleWhite = false;

	/**
	 * Flag E « murs marbre + toits terracotta » : applique le blanc marbre (teinte marbre
	 * + sous-tuile pierre claire 12) AUX MURS SEULEMENT (procduraux ET coque Roofer), en
	 * gardant la tuile/teinte REELLE du TOIT (terre cuite). Complementaire de bMarbleWhite
	 * (tout-ou-rien) : si bMarbleWhite est vrai il l'emporte (murs ET toits blancs). N'affecte
	 * que le chemin desktop batiments ; defaut false = comportement historique.
	 */
	UPROPERTY() bool bMarbleWalls = false;

	/**
	 * J3f DA fenetres du chemin batiment desktop (BuildPolygonBuildingDesktop) :
	 *   0 = Defaut (comportement historique : fenetres geometriques, creux si bWindowReveals) ;
	 *   1 = Aucune (murs pleins, pas de travees) ;
	 *   2 = Discretes (mur PLEIN + niche marbre peu profonde 6 cm, ni trou ni vitre) ;
	 *   3 = Normales (fenetres geometriques completes, force meme sans bWindowReveals).
	 * N'affecte que le chemin desktop ; mobile et proxy inchanges.
	 */
	UPROPERTY() int32 WindowMode = 0;

	/**
	 * V6 (decision utilisateur du 01/08) — LA COUCHE PROXY EST SUPPRIMEE.
	 *
	 * Les SM_Proxy_* etaient la couche RESIDENTE grossiere : un rectangle ORIENTE par
	 * batiment (ou un contour reduit a 12 points), retracte de 2 m et SANS les cours.
	 * Elle est OBSOLETE sur desktop : Nanite rend le detail a la densite de l'ecran et
	 * le streame a la demande, et nos maillages sont fusionnes PAR CELLULE (le surcout
	 * en composants est donc trivial). Si des silhouettes lointaines redeviennent
	 * necessaires a l'echelle de l'agglo, la reponse sera le HLOD d'UE, qui les genere
	 * CORRECTES — toits compris — au lieu de blocs a cours pleines.
	 *
	 * Rappel de ce que cette couche a coute (autopsie V5) : sauvee VISIBLE, elle se
	 * superposait au detail, remplissait 96 % de la surface des cours et debordait de
	 * 24 858 m2 hors de toute emprise — le grief « cours bouchees, emprises gonflees,
	 * ancienne construction ». On ne la cache plus : on ne la fabrique plus.
	 *
	 * Defaut false = AUCUN proxy n'est construit ni pose (ni geometrie, ni asset, ni
	 * acteur). true = ancienne couche restituee telle quelle et VISIBLE (la demander
	 * explicitement, c'est vouloir la voir).
	 */
	UPROPERTY() bool bProxyLayer = false;

	/**
	 * C1 « DISCONTINUITES » — MURS DE SOUTENEMENT.
	 *
	 * Le sol est un drape 2,5D : la dalle n'echantillonne le MNT qu'aux coins de ses
	 * quads (GroundGridN, soit 7,8125 m en desktop). Toute discontinuite verticale
	 * REELLE — mur de quai, berge maconnee de canal, tranchee ferroviaire — y devient
	 * une RAMPE de la largeur d'un quad, aux texels etires : le grief « pente bizarre ».
	 *
	 * Cette passe pose, le long des breaklines cuites dans SourceData/Murs
	 * (murs_<x>_<y>.json, produit par work/DISCONT/c1_bake_3x3.py depuis LE MEME MNT
	 * que le drape), une face VERTICALE surmontee d'un COURONNEMENT horizontal qui
	 * recouvre la rampe jusqu'a la retrouver de niveau. Materiau : celui des bordures
	 * (aucun materiau nouveau).
	 *
	 * Strategie v1 ASSUMEE : on MASQUE la rampe, on ne re-maille PAS la grille du sol.
	 * Le Z est lu sur la SURFACE RENDUE (doctrine du Playbook §6), jamais sur le MNT.
	 *
	 * Cellule sans fichier = aucun mur pour elle, sans erreur (cuisson partielle).
	 */
	UPROPERTY() bool bRetainingWalls = true;

	/** Dossier des breaklines cuites (murs_<x>_<y>.json). Vide = <projet>/SourceData/Murs. */
	UPROPERTY() FString RetainingWallsPath;

	/**
	 * Classes de mur posees, en minuscules et separees par des virgules.
	 * Vide = les trois classes du side-car (« quai,tranchee,talus »). La classe
	 * « talus » est la moins sure des trois (rupture de pente d'un versant, pas
	 * forcement maconnee) : ce champ permet de la retirer sans re-cuire le side-car.
	 */
	UPROPERTY() FString RetainingWallClasses;

	/**
	 * LOT QUAIS (V2) — LES ESCALIERS.
	 *
	 * Un mur de quai sans escalier est un decor : dans la realite on descend sur la
	 * berge. Cette passe pose, le long des polylignes cuites dans
	 * SourceData/Escaliers (escaliers_<x>_<y>.json, derive des troncons BD TOPO
	 * `nature = 'Escalier'` et du complement OSM `highway = steps`), une VOLEE de
	 * marches procedurales : contremarche verticale + giron horizontal par marche,
	 * plus deux limons pleins qui l'asseyent dans la pente.
	 *
	 * Le side-car ne porte AUCUN Z (ni BD TOPO ni OSM ne codent l'altitude de ces
	 * troncons — verifie sur les 36 troncons de l'emprise) : le denivele est LU SUR
	 * LA SURFACE RENDUE aux deux extremites, doctrine du sol rendu (Playbook §6).
	 * Le nombre de marches en decoule (contremarche standard francaise ~16,5 cm),
	 * le giron se deduit de la longueur en plan, et des PALIERS absorbent ce qui
	 * reste quand la volee est plus longue que ses marches — ce qui est justement
	 * le cas des grands escaliers de quai (mesure aux deux escaliers du Pont
	 * Saint-Pierre : 7,8 m de denivele pour 31,9 m de trace en plan).
	 *
	 * Materiau : celui des bordures et des murs (aucun materiau nouveau).
	 * Cellule sans fichier = aucun escalier pour elle, sans erreur.
	 */
	UPROPERTY() bool bStairs = true;

	/** Dossier des escaliers cuits (escaliers_<x>_<y>.json). Vide = <projet>/SourceData/Escaliers. */
	UPROPERTY() FString StairsPath;

	/**
	 * CHANTIER C2 (03/08) — LES PONTS, ET POURQUOI LE TABLIER A SA PROPRE COTE.
	 *
	 * MESURE qui fonde la passe (work/PONTS/p1b_cotes.py) : le tablier du Pont
	 * Saint-Pierre est a 142,10-142,70 m NGF dans BD TOPO 3D, le MNT dessous est a
	 * 132,3-135,0 m — soit +8,75 m en moyenne. Le code d'avant ce chantier posait le
	 * ruban de pont par INTERPOLATION DU MNT ENTRE SES DEUX BOUTS
	 * (`ComputePolylineZ`, bBridge=true) : il manquait 7,62 m en moyenne au tablier,
	 * qui traversait donc la promenade au niveau du sable, sa ligne axiale peinte
	 * dessus. Sur le proto 3x3, 70 rubans de pont etaient construits ainsi, dont 26
	 * de classe marquee ; les quatre pires manquaient 6,4 a 8,5 m.
	 *
	 * REGLE NATIONALE, zero cas particulier : le side-car
	 * `SourceData/Ponts/ponts_<x>_<y>.json` porte l'OUVRAGE — la chaine connexe de
	 * troncons amorcee sur `position_par_rapport_au_sol >= 1` et etendue aux voisins
	 * encore EN L'AIR (BD TOPO ne code « pont » que la travee au-dessus de l'eau ;
	 * les rampes restent pos=0 et pourtant a la cote du tablier). La chaine s'arrete
	 * sur le troncon qui ATTERRIT, si bien qu'aucune culee aveugle n'est necessaire :
	 * le tablier rejoint le sol tout seul. Le Z vient de la GEOMETRIE 3D de BD TOPO ;
	 * quand elle n'en a pas (18 troncons sur 98 dans le proto), on retombe sur
	 * l'interpolation entre les cotes RENDUES des deux rives, et on le journalise.
	 *
	 * Materiaux : chaussee = l'asphalte des rubans, sous-face/bandeaux/parapets = la
	 * pierre des bordures et des murs. AUCUN materiau nouveau, AUCUNE classe de sol.
	 * Exige le drapage (sans relief il n'y a rien a franchir) : le golden path mobile
	 * n'est pas concerne.
	 */
	UPROPERTY() bool bBridges = true;

	/** Dossier des ponts cuits (ponts_<x>_<y>.json). Vide = <projet>/SourceData/Ponts. */
	UPROPERTY() FString BridgesPath;

	/**
	 * C2 : les PARAPETS, poses seulement la ou le tablier est REELLEMENT en l'air (un
	 * troncon code « pont » au ras du sol — il y en a : Quai Saint Pierre, 2 cm de
	 * hauteur mesuree — ne doit pas se retrouver borde de deux murets). false =
	 * tablier nu : rollback sans re-cuire le side-car ni rebuilder.
	 */
	UPROPERTY() bool bBridgeParapets = true;

	/**
	 * C2 : ROLLBACK du remplacement des rubans. Par defaut, quand la passe ponts est
	 * active, les rubans OSM `bridge=true` ne sont PLUS construits — c'est le tablier
	 * a sa cote qui les remplace. true restitue les rubans drapes d'avant C2 (et le
	 * grief avec) : sert a refaire l'A/B sans toucher a autre chose.
	 */
	UPROPERTY() bool bBridgeRibbonsHistorique = false;

	/**
	 * LOT QUAIS (V4) — LES GRADINS.
	 *
	 * Regle NATIONALE : la ou un mur de classe « quai » borde une zone PIETONNE
	 * BASSE (le side-car le mesure au prep et pose le drapeau `borde_pieton`), la
	 * face lisse se rend en GRADINS — la volee de larges marches sur laquelle on
	 * s'assoit au bord de l'eau. Sans zone pietonne en contrebas, un quai reste une
	 * face lisse : un gradin qui donne sur rien n'a aucun sens.
	 *
	 * Ne s'applique qu'au-dessus d'une hauteur MESUREE plancher (un mur trop bas
	 * n'a pas la place d'en porter deux). Les dimensions sont des constantes du
	 * .cpp (a tourner en boucle B). false = tous les murs restent lisses : c'est le
	 * rollback, sans re-cuire le side-car ni rebuilder.
	 *
	 * ITERATION UTILISATEUR 1 (02/08) — le couple A/B a ete TRANCHE par
	 * l'utilisateur : c'est la FACE LISSE. Le defaut passe donc a `false` ; la dette
	 * (« basculer le defaut au prochain build ») est soldee au lot PIE.
	 *
	 * ⭐ LOT FINITION QUAIS (03/08) — LE DEFAUT REPASSE A `true`, ET LE SENS DU
	 * DRAPEAU CHANGE. Ce que l'utilisateur avait rejete, ce n'etait pas le gradin
	 * (« tres realiste ») : c'etait des gradins SUR TOUT LE QUAI, poses sur un
	 * critere devine (`borde_pieton`), donc partout. Une DONNEE existe pour dire ou
	 * ils sont vraiment : OSM `leisure=bleachers`, un vocabulaire mesure comme
	 * NATIONAL (99 elements / 92 polygones sur 5 agglomerations). Depuis, les
	 * gradins ne s'appliquent qu'a la PORTION de mur tombant dans une emprise du
	 * side-car `SourceData/Gradins` — sans side-car, AUCUN gradin nulle part.
	 * `true` signifie donc « suis la donnee », pas « mets-en partout », et la
	 * regression « gradins sur tout le quai » est structurellement impossible.
	 * `false` reste le rollback total, sans re-cuire ni rebuilder.
	 */
	UPROPERTY() bool bQuayTiers = true;

	/**
	 * ⭐ LOT EAU (03/08) — LA SURFACE EN EAU, A SA COTE MESUREE.
	 *
	 * Regle NATIONALE, zero cas particulier :
	 *   EMPRISE = BD TOPO `surface_hydrographique` (en service, hors souterrain),
	 *             decoupee par cellule dans le side-car `SourceData/Eau`.
	 *   COTE    = le MNT LiDAR LUI-MEME, PAR SOMMET du contour. Le LiDAR ne penetre
	 *             pas l'eau : ses retours SONT la surface du plan d'eau (mesure sur
	 *             le proto : mode 132,50 m NGF, 68 % des echantillons a +-6 cm).
	 *             Le side-car porte donc une ALTITUDE NGF par sommet ; le rebase sur
	 *             l'origine Unreal se fait ICI (AltCapCm), comme pour les ponts.
	 *
	 * CE QUE CA CORRIGE, mesure : le film d'eau historique posait UN plan par
	 * polygone au p10 du MNT du polygone ENTIER. Le polygone de la Garonne traverse
	 * la chaussee du Bazacle (6 m de chute) : son p10 valait la cote AVAL, soit
	 * ~6 m SOUS le lit toulousain — le plan etait enterre et la Garonne A SEC sous
	 * tous les ponts du proto. Une cote PAR SOMMET suit la realite (biefs plats,
	 * chute au barrage) sans aucune liste d'ouvrages.
	 *
	 * Materiau : `WaterMaterialPath` — le shading model SINGLE LAYER WATER du
	 * moteur (celui-la meme que le plugin Water emploie pour ses surfaces).
	 * AUCUNE collision (comme les autres films de surface).
	 * false = aucune surface en eau ; les films teintes historiques reviennent.
	 */
	UPROPERTY() bool bWater = true;

	/** Dossier des surfaces en eau cuites (eau_<x>_<y>.json). Vide = <projet>/SourceData/Eau. */
	UPROPERTY() FString WaterPath;

	/** Materiau de la surface en eau. Vide = /Game/Dev/MI_CityWater.MI_CityWater. */
	UPROPERTY() FString WaterMaterialPath;

	/**
	 * LOT EAU : ROLLBACK. true restitue EN PLUS les films d'eau teintes historiques
	 * (le placeholder p10) par-dessus la vraie surface — sert a refaire l'A/B sans
	 * toucher a autre chose. Quand `bWater` est faux, ils reviennent de toute facon.
	 */
	UPROPERTY() bool bWaterFilmsHistorique = false;

	/**
	 * ⭐ LOT PIE (02/08) — LA COLLISION DE LA VEGETATION, ET POURQUOI ELLE PART.
	 *
	 * MESURE, pas opinion. Un `UInstancedStaticMeshComponent` cree **un corps
	 * physique par instance**, et — nos HISM etant `Movable` — il les cree UN PAR UN
	 * (`FBodyInstance::InitBody`, moteur `InstancedMeshComponentBodies.cpp` l. 111)
	 * au lieu du chemin par lot `InitStaticBodies`. Sur le proto 3x3 cela fait
	 * **1 253 686 corps Chaos** crees a chaque « Play » et detruits a chaque « Stop ».
	 * Cout mesure du cycle PIE AVANT : demarrage **10,3 s** (dont 8,83 s de
	 * `InitializeActorsForPlay`) et arret **604,5 s**, dont **602,4 s = 99,97 %**
	 * dans `DestroyGarbage` (destruction de 862 objets) — le gel que l'utilisateur
	 * reglait au gestionnaire des taches.
	 *
	 * LA REGLE EST LUE DANS LA DONNEE, elle n'est pas choisie : un mesh de
	 * vegetation **sans aucune primitive de collision SIMPLE** (`AggGeom` vide) n'a
	 * pas de collision voulue — son `CTF_USE_DEFAULT` retombe sur la collision
	 * COMPLEXE, c'est-a-dire le repli decime du Nanite : ni voulue, ni utilisable, et
	 * payee 1,2 million de fois. Mesure du catalogue du proto : les 12 herbes
	 * (1 180 237 touffes), les fosses carrees et rondes (25 054) et les sureaux
	 * portent **0 primitive** ; les erables et les hetres en portent **2 a 3
	 * convexes** — ceux-la GARDENT leur collision, aucun arbitrage n'est demande.
	 *
	 * `true` = comportement historique (tout collisionne). C'est le rollback, et il
	 * ne demande PAS de rebuild.
	 */
	UPROPERTY() bool bVegCollisionHistorique = false;

	/**
	 * LOT VELOCITE — MODE DISTRICT : « ne regenere QUE ces cellules ».
	 *
	 * Liste de cellules « x_y » separees par des virgules, ex. "-2_0,-2_1,-1_0".
	 * VIDE (defaut) = comportement historique, ville entiere, bit pour bit : toutes
	 * les branches ci-dessous sont gardees par un booleen qui reste faux.
	 *
	 * Ce que le filtre change, passe par passe :
	 *  - ImportCityStreamed : la purge d'idempotence ne detruit QUE les acteurs
	 *    SM_Ground_/SM_Slab_/SM_Bldg_ des cellules VISEES (le reste de la ville n'est
	 *    pas touche) ; batiments, routes, masques, murs, dalles et blocs de streaming
	 *    ne traitent que ces cellules ; seuls les sous-niveaux qui les portent sont
	 *    charges, remplis et sauves.
	 *  - ImportCitySurfaces : idem pour les SM_Surface_.
	 *  - ImportVegetation : la vege est par MESH et non par cellule — on ne detruit
	 *    donc PAS les acteurs CityVeg_* ; on RETIRE de chaque HISM les seules
	 *    instances dont la position tombe dans l'emprise visee, puis on y ajoute
	 *    celles que la passe vient de semer. L'autorite de pose reste unique : c'est
	 *    le meme code qui trace et qui pose, seule sa PORTEE change.
	 *
	 * Ce que le filtre NE fait PAS, et c'est volontaire : les compteurs du resume
	 * portent alors sur les cellules VISEES, pas sur la ville — une comparaison de
	 * non-regression se fait sur une regeneration COMPLETE.
	 *
	 * Incompatible avec bProxyLayer (la couche proxy a sa propre maille) : le filtre
	 * l'emporte et la couche n'est pas construite, avec une ligne de log.
	 */
	UPROPERTY() FString CellFilter;

	/**
	 * Taille, en metres, des cellules auxquelles CellFilter fait reference.
	 * 0 (defaut) = la taille de cellule de la passe (CellSizeM). ImportVegetation
	 * n'a PAS de parametre CellSizeM : elle EXIGE ce champ des que CellFilter est
	 * renseigne, sinon elle ignore le filtre et le DIT (une passe silencieusement
	 * complete serait pire qu'un refus).
	 */
	UPROPERTY() float CellFilterSizeM = 0.f;

	/** Prereglage desktop complet : 64x64 drape, collision 16x16, pas routes 15 m. */
	static FCityGenProfile Desktop();

	/** Profil effectif : si bDesktop, les champs laisses en valeur mobile prennent le prereglage desktop. */
	FCityGenProfile Resolved() const;
};

/** Counts of what ImportCityDistrict generated. */
USTRUCT(BlueprintType)
struct FCityImportSummary
{
	GENERATED_BODY()

	/** Buildings generated from footprints. */
	UPROPERTY() int32 Buildings = 0;

	/** Road ribbons generated. */
	UPROPERTY() int32 Roads = 0;

	/** Tree instances placed. */
	UPROPERTY() int32 Trees = 0;

	/** Static mesh assets created (one per grid cell, plus the tree mesh). */
	UPROPERTY() int32 Meshes = 0;
};

/** Counts of what ImportCityStreamed generated. */
USTRUCT(BlueprintType)
struct FCityStreamedSummary
{
	GENERATED_BODY()

	/** Buildings generated (detail + proxy). */
	UPROPERTY() int32 Buildings = 0;

	/** Road ribbons generated (resident ground layer). */
	UPROPERTY() int32 Roads = 0;

	/** Tree instances placed (resident). */
	UPROPERTY() int32 Trees = 0;

	/** Resident ground meshes (slabs + roads), one per cell. */
	UPROPERTY() int32 GroundMeshes = 0;

	/** Resident proxy meshes (box buildings), one per proxy cell. */
	UPROPERTY() int32 ProxyMeshes = 0;

	/** Detail building meshes, one per cell, spawned inside streaming blocks. */
	UPROPERTY() int32 BuildingMeshes = 0;

	/** Dedicated building collision meshes (SM_Bldg_*_Col), one per desktop cell. */
	UPROPERTY() int32 BuildingColMeshes = 0;

	/** Streaming sublevels created or refilled. */
	UPROPERTY() int32 StreamingBlocks = 0;

	/** J3b : batiments generes avec un toit en pente (squelette droit precalcule). */
	UPROPERTY() int32 RoofsPitched = 0;

	/** J3c v2 : disques du revetement dominant poses sur les carrefours. */
	UPROPERTY() int32 JunctionPatches = 0;

	/** J3c point 3 : quads de BORDURE poses (face verticale + chant), toutes rues. */
	UPROPERTY() int32 CurbQuads = 0;

	/**
	 * FINITION_SOL V3 : quads de BORDURETTE D'HERBE (meme mecanique et meme materiau
	 * que la bordure de chaussee, profil reduit 7 / 14 cm). Compteur SEPARE de
	 * CurbQuads : la bordurette est un ajout, elle ne doit jamais se confondre avec
	 * la voirie dans les comparaisons de non-regression.
	 */
	UPROPERTY() int32 GrassCurbQuads = 0;

	/** J3c point 3 : passages pietons poses (un par noeud chaussee x voie pietonne). */
	UPROPERTY() int32 Crossings = 0;

	/** J3c point 3 : passages REPORTES — un patch de carrefour couvrait deja le noeud. */
	UPROPERTY() int32 CrossingsDeferred = 0;

	/** J3c point 3 : rubans ORPHELINS ecartes (< 25 m et aucun noeud partage). */
	UPROPERTY() int32 OrphanRibbons = 0;

	/** J3c maquette : cellules dont la dalle a recu son instance de materiau masquee. */
	UPROPERTY() int32 MaskedCells = 0;

	/** J3c maquette : rubans de chaussee au sol supprimes — la peinture les remplace. */
	UPROPERTY() int32 GroundRibbonsSkipped = 0;

	/** J3c maquette : rubans de PONT conserves (un tablier ne se peint pas sur le sol). */
	UPROPERTY() int32 BridgeRibbons = 0;

	/** J3c maquette : tirets de ligne axiale poses (quads de 15 cm). */
	UPROPERTY() int32 AxialDashes = 0;

	/**
	 * C1 : quads de MUR DE SOUTENEMENT poses (face verticale + couronnement + dos,
	 * plus deux bouchons par polyligne). Compteur SEPARE de CurbQuads : un mur n'est
	 * pas une bordure, et les comparaisons de non-regression ne doivent pas les
	 * melanger.
	 */
	UPROPERTY() int32 RetainingWallQuads = 0;

	/** C1 : polylignes de mur effectivement posees (au moins un quad). */
	UPROPERTY() int32 RetainingWalls = 0;

	/** C1 : polylignes ECARTEES — la surface rendue n'y presente aucune rampe a masquer. */
	UPROPERTY() int32 RetainingWallsSkipped = 0;

	/** QUAIS V2 : volees d'escalier effectivement posees (au moins une marche). */
	UPROPERTY() int32 Stairs = 0;

	/** QUAIS V2 : marches posees, toutes volees confondues. Le discriminant de la passe. */
	UPROPERTY() int32 StairSteps = 0;

	/**
	 * QUAIS V2 : volees ECARTEES — la surface rendue n'y presente pas de denivele
	 * exploitable (moins d'une marche), ou la geometrie qui en decoulerait ne serait
	 * pas un escalier (giron impossible). Compte, jamais tu en silence.
	 */
	UPROPERTY() int32 StairsSkipped = 0;

	/** QUAIS V4 : murs de quai rendus en GRADINS au lieu d'une face lisse. */
	UPROPERTY() int32 QuayTierWalls = 0;

	/** QUAIS V4 : gradins poses, tous murs confondus. */
	UPROPERTY() int32 QuayTiers = 0;

	/**
	 * FINITION QUAIS : LONGUEUR de mur reellement rendue en gradins, en DECIMETRES.
	 * C'est le seul compteur qui rende la regle verifiable : les gradins ne
	 * s'appliquent qu'a la PORTION d'un mur de quai tombant dans une emprise OSM
	 * `leisure=bleachers`, et « 0 m ailleurs » ne se prouve pas avec un compte de
	 * murs. En decimetres pour rester entier dans le resume.
	 */
	UPROPERTY() int32 QuayTierDm = 0;

	/** FINITION QUAIS : emprises de gradins lues dans le side-car (cellules visees). */
	UPROPERTY() int32 QuayTierEmprises = 0;

	/** C2 : tabliers de pont poses A LEUR COTE (un par troncon d'ouvrage). */
	UPROPERTY() int32 Bridges = 0;

	/** C2 : quads poses par la passe ponts (tablier + sous-face + bandeaux + parapets). */
	UPROPERTY() int32 BridgeQuads = 0;

	/** C2 : metres de tablier poses (longueur en plan). Le discriminant de la passe. */
	UPROPERTY() int32 BridgeDeckM = 0;

	/** C2 : ouvrages ECARTES — trace inexploitable ou surface absente. Jamais tu en silence. */
	UPROPERTY() int32 BridgesSkipped = 0;

	/** C2 : tabliers dont le Z venait du REPLI (side-car sans geometrie 3D). */
	UPROPERTY() int32 BridgesZFallback = 0;

	/** C2 : rubans OSM `bridge=true` NON construits — remplaces par le tablier a sa cote. */
	UPROPERTY() int32 BridgeRibbonsReplaced = 0;
};

/** Counts of what GenerateBuildingCollisionCell produced for one cell. */
USTRUCT(BlueprintType)
struct FCityBldgColSummary
{
	GENERATED_BODY()

	/** Buildings whose footprint prism went into the cell's collision mesh. */
	UPROPERTY() int32 Buildings = 0;

	/** Triangles in the generated collision mesh. */
	UPROPERTY() int32 Triangles = 0;

	/** True when SM_Bldg_<x>_<y>_Wall existed and now uses the _Col as ComplexCollisionMesh. */
	UPROPERTY() bool bWallWired = false;

	/** True when the touched packages were saved to disk. */
	UPROPERTY() bool bSaved = false;
};

/** Counts of what GenerateBuildingCollisionAll produced. */
USTRUCT(BlueprintType)
struct FCityBldgColBatchSummary
{
	GENERATED_BODY()

	/** Cells that received a SM_Bldg_*_Col mesh. */
	UPROPERTY() int32 Cells = 0;

	/** Building prisms built, all cells together. */
	UPROPERTY() int32 Buildings = 0;

	/** Wall meshes wired to their _Col. */
	UPROPERTY() int32 WiredWalls = 0;

	/** Cells whose SM_Bldg_*_Wall asset was missing (the _Col is still created). */
	UPROPERTY() int32 MissingWalls = 0;
};

/** Counts of what ImportCitySurfaces generated. */
USTRUCT(BlueprintType)
struct FCitySurfacesSummary
{
	GENERATED_BODY()

	/** Water polygons built. */
	UPROPERTY() int32 Water = 0;

	/** Green polygons built (parks, grass, woods). */
	UPROPERTY() int32 Green = 0;

	/** Rail ribbons built. */
	UPROPERTY() int32 Rails = 0;

	/** Procedural trees scattered inside forest polygons. */
	UPROPERTY() int32 ScatterTrees = 0;

	/** Static mesh assets created (one per grid cell). */
	UPROPERTY() int32 Meshes = 0;

	/** LOT EAU : surfaces en eau posees depuis le side-car (une par piece de cellule). */
	UPROPERTY() int32 WaterBodies = 0;

	/** LOT EAU : triangles poses par la passe eau. */
	UPROPERTY() int32 WaterTris = 0;

	/** LOT EAU : aire en eau posee, en m2 (entier : c'est un ordre de grandeur). */
	UPROPERTY() int32 WaterAreaM2 = 0;

	/** LOT EAU : pieces ecartees (moins de 3 sommets, ou cote absente). */
	UPROPERTY() int32 WaterSkipped = 0;

	/** LOT EAU : cellules du side-car effectivement lues. */
	UPROPERTY() int32 WaterCells = 0;
};

/** Counts of what ImportVegetation placed. */
USTRUCT(BlueprintType)
struct FCityVegSummary
{
	GENERATED_BODY()

	/** Vegetation instances placed (all meshes together). */
	UPROPERTY() int32 Instances = 0;

	/** Distinct meshes instanced (one HISM per mesh). */
	UPROPERTY() int32 Meshes = 0;

	/** Actors spawned to carry the HISM components (one per mesh). */
	UPROPERTY() int32 Actors = 0;

	/**
	 * Instances REJECTED because no ground (SM_Surface_/SM_Slab_) was found in their
	 * column. There is no analytic GroundZ fallback any more: no ground, no instance.
	 * Their positions are written to "<VegJsonPath>.skipped.json" for tracing.
	 */
	UPROPERTY() int32 Skipped = 0;

	/** Planting pits generated at the foot of trees growing through mineral ground. */
	UPROPERTY() int32 Pits = 0;
};

/**
 * Imports a real-world city district from a prepared JSON file (building footprints with
 * heights, road polylines with widths, tree positions — all in meters around a local
 * origin) and generates unlit city geometry: extruded buildings with windowed facades,
 * road ribbons with sidewalks and markings, ground slabs, and instanced trees.
 * Shading and per-usage tint are baked into vertex colors.
 */
UCLASS(MinimalAPI)
class UCityImportTools : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Generates a district from a city JSON file. Geometry is merged into one static
	 * mesh per grid cell and spawned as actors; trees use a single instanced component.
	 * Meshes are written under AssetFolder; existing assets with the same names are
	 * regenerated in place. Packages are left dirty; save afterwards.
	 * @param JsonFilePath Absolute path to the district JSON (see SourceData/*.json).
	 * @param AssetFolder Package folder for generated meshes, e.g. "/Game/City/Capitole".
	 * @param WallMaterialPath Opaque vertex-color material for walls, roads, ground, trees.
	 * @param GlassMaterialPath Material for window panes.
	 * @param CellSizeM Grid cell size used to merge geometry, meters.
	 * @param Location World position of the district origin.
	 * @return Counts of generated content.
	 */
	UFUNCTION(meta = (AICallable), Category = "CityImportTools")
	static FCityImportSummary ImportCityDistrict(const FString& JsonFilePath, const FString& AssetFolder,
		const FString& WallMaterialPath, const FString& GlassMaterialPath, float CellSizeM, FVector Location);

	/**
	 * Places orientation markers from a JSON file with a "markers" array of
	 * {x, y (meters), k (kind: "metro", "metro_e", "church", "townhall"), n (label)}.
	 * Each kind gets a colored totem (instanced); named markers also get a floating
	 * cross of text labels. Marker meshes are written under AssetFolder.
	 * @param JsonFilePath Absolute path to the markers JSON.
	 * @param AssetFolder Package folder for the totem meshes, e.g. "/Game/City/Capitole".
	 * @param WallMaterialPath Opaque vertex-color material for the totems.
	 * @param Location World position of the district origin.
	 * @param Profile Generation profile; default (all fields omitted) is the mobile
	 *        golden path (totems at z=0), desktop drapes them onto the MNT.
	 * @return Number of markers placed.
	 */
	UFUNCTION(meta = (AICallable), Category = "CityImportTools")
	static int32 ImportCityMarkers(const FString& JsonFilePath, const FString& AssetFolder,
		const FString& WallMaterialPath, FVector Location, const FCityGenProfile& Profile);

	/**
	 * Places lightweight orientation surfaces from a JSON file with "water", "green"
	 * and "rails" arrays: flat tinted polygons (water below roads, parks and woods at
	 * ground level) plus dark rail ribbons — placeholders meant to be replaced later.
	 * Forest polygons also get procedural trees scattered on a jittered grid, added
	 * to a dedicated instanced component. Geometry is merged per grid cell like the
	 * district import; re-running replaces the previous surfaces (labels SM_Surface_*).
	 * @param JsonFilePath Absolute path to the surfaces JSON (see SourceData/*_surfaces.json).
	 * @param AssetFolder Package folder for generated meshes, e.g. "/Game/City/Capitole".
	 * @param WallMaterialPath Opaque vertex-color material for every surface.
	 * @param CellSizeM Grid cell size used to merge geometry, meters.
	 * @param Location World position of the district origin.
	 * @param Profile Generation profile; default (all fields omitted) is the mobile
	 *        golden path (flat stacked films). Desktop drapes greens and rails onto
	 *        the MNT, water becomes a horizontal plane at the low percentile (p10)
	 *        of the terrain under each polygon.
	 * @return Counts of generated content.
	 */
	UFUNCTION(meta = (AICallable), Category = "CityImportTools")
	static FCitySurfacesSummary ImportCitySurfaces(const FString& JsonFilePath, const FString& AssetFolder,
		const FString& WallMaterialPath, float CellSizeM, FVector Location, const FCityGenProfile& Profile);

	/**
	 * Places vegetation instances from a prepared JSON ("instances" array of
	 * {mesh (package or object path), x, y (meters), scale, yaw (degrees), kind
	 * (optional "tree"/"hedge"/"clump")}) — the SINGLE authority that seats every plant
	 * of the city, trees, hedges and grass clumps alike.
	 *
	 * Seating is done by TRACING the VISIBLE surface (throwaway trace proxies of the
	 * SM_Surface_ films and SM_Slab_ slabs), not by an analytic terrain model: the
	 * highest ground hit of the column wins, base at 0, ZERO min_Z offset (Megascans
	 * pivot at the foot). NO GroundZ FALLBACK: an instance whose column has no ground
	 * at all is simply NOT placed, counted in Skipped and listed in
	 * "<VegJsonPath>.skipped.json".
	 *
	 * A TREE whose central hit is the slab AND whose ground mask says "mineral"
	 * (R channel of SourceData/Sols/mask_<x>_<y>.png, < 128) gets a ~1.2 m planting
	 * pit generated at its foot (one shared HISM, actor "CityVeg_TreePits").
	 *
	 * Instances are grouped by mesh (HISM constraint: one component per mesh); each
	 * distinct mesh is loaded with LoadObject (never recreated) and its materials are
	 * left untouched — only MATUSAGE_InstancedStaticMeshes is flagged (F.39). Grass
	 * clumps additionally get the validated render settings (45-60 m fade, no shadow).
	 * One "CityVeg_*" actor per mesh; re-running is a vegetation-only pass that first
	 * destroys every previous "CityVeg*" actor.
	 * @param VegJsonPath Absolute path to the vegetation JSON.
	 * @param AssetFolder Package folder context (also receives the generated pit mesh).
	 * @param Location World position of the district origin (instances are world-space).
	 * @param Profile Generation profile; must match the district's (Desktop, bDrapeToTerrain=true).
	 * @return Counts of what was placed, skipped and dug.
	 */
	UFUNCTION(meta = (AICallable), Category = "CityImportTools")
	static FCityVegSummary ImportVegetation(const FString& VegJsonPath, const FString& AssetFolder,
		FVector Location, const FCityGenProfile& Profile);

	/**
	 * Imports a district split into three layers for distance streaming on device:
	 *  - resident ground layer (slabs + road ribbons, collision), actors "SM_Ground_*";
	 *  - resident proxy layer (windowless box buildings shrunk 30 cm so the detail
	 *    version hides them, no collision), actors "SM_Proxy_*";
	 *  - detail buildings (windowed facades, collision) merged per cell and spawned
	 *    into streaming sublevels of BlockSizeM, named "L_T10_B_<bx>_<by>" under
	 *    BlocksFolder (ULevelStreamingDynamic, not initially loaded).
	 * Trees stay resident (single HISM). Re-running replaces previous layers and
	 * refills existing sublevels; legacy "SM_City_*" actors are removed.
	 * @param JsonFilePath Absolute path to the district JSON (see SourceData/*.json).
	 * @param SurfacesJsonFilePath Optional surfaces JSON: slab grid vertices are tinted
	 *        by sampling its water/green polygons — the always-resident painted ground
	 *        that carries the map's look beyond the 3D films' cull distance. Empty = plain.
	 * @param AssetFolder Package folder for generated meshes, e.g. "/Game/City/Toulouse10".
	 * @param BlocksFolder Package folder for streaming sublevels, e.g. "/Game/Maps/T10Blocks".
	 * @param WallMaterialPath Opaque vertex-color material for walls, roads, ground, trees.
	 * @param GlassMaterialPath Material for window panes.
	 * @param CellSizeM Merge cell size for ground and detail meshes, meters.
	 * @param BlockSizeM Streaming sublevel size, meters (multiple of CellSizeM).
	 * @param ProxyCellSizeM Merge cell size for the proxy layer, meters.
	 * @param Location World position of the district origin.
	 * @param Profile Generation profile; default (all fields omitted) is the mobile
	 *        golden path, bit-identical to the historical generation. Desktop (J2):
	 *        ground grid draped onto the MNT with a dedicated low-res collision
	 *        trimesh, roads resampled then draped (roads tagged bridge=true keep a
	 *        linear deck between abutments), buildings seated at the terrain with a
	 *        buried plinth, trees at terrain height.
	 * @return Counts of generated content.
	 */
	UFUNCTION(meta = (AICallable), Category = "CityImportTools")
	static FCityStreamedSummary ImportCityStreamed(const FString& JsonFilePath,
		const FString& SurfacesJsonFilePath, const FString& AssetFolder,
		const FString& BlocksFolder, const FString& WallMaterialPath, const FString& GlassMaterialPath,
		float CellSizeM, float BlockSizeM, float ProxyCellSizeM, FVector Location,
		const FCityGenProfile& Profile);

	/**
	 * Builds the dedicated building-collision mesh of one grid cell: one CLOSED prism
	 * per building whose centroid falls in the cell (ear-clipped footprint as top and
	 * bottom caps + one quad per edge), seated exactly like the district import
	 * (roof = ZBase + h, foot = ZBase - socle; flat profile = 0..h). The mesh
	 * SM_Bldg_<CellX>_<CellY>_Col is written under AssetFolder (complex-as-simple
	 * trimesh, no Nanite, default material) and wired as ComplexCollisionMesh of
	 * SM_Bldg_<CellX>_<CellY>_Wall when that mesh exists. Touched packages are saved.
	 * @param JsonFilePath Absolute path to the district JSON (see SourceData/*.json).
	 * @param AssetFolder Package folder of the generated meshes, e.g. "/Game/City/Toulouse10".
	 * @param CellSizeM Grid cell size the district was imported with, meters.
	 * @param CellX Cell index X (floor of building centroid X / cell size).
	 * @param CellY Cell index Y.
	 * @param Profile Generation profile; must match the city's generation profile so
	 *        the prisms seat at the same Z (desktop drapes onto the MNT).
	 * @return Counts for the cell.
	 */
	UFUNCTION(meta = (AICallable), Category = "CityImportTools")
	static FCityBldgColSummary GenerateBuildingCollisionCell(const FString& JsonFilePath,
		const FString& AssetFolder, float CellSizeM, int32 CellX, int32 CellY,
		const FCityGenProfile& Profile);

	/**
	 * Runs GenerateBuildingCollisionCell over EVERY cell that contains buildings,
	 * parsing the district JSON once. Packages are saved cell by cell, so a killed
	 * run keeps everything already generated.
	 * @param JsonFilePath Absolute path to the district JSON (see SourceData/*.json).
	 * @param AssetFolder Package folder of the generated meshes, e.g. "/Game/City/Toulouse10".
	 * @param CellSizeM Grid cell size the district was imported with, meters.
	 * @param Profile Generation profile; must match the city's generation profile.
	 * @return Aggregated counts.
	 */
	UFUNCTION(meta = (AICallable), Category = "CityImportTools")
	static FCityBldgColBatchSummary GenerateBuildingCollisionAll(const FString& JsonFilePath,
		const FString& AssetFolder, float CellSizeM, const FCityGenProfile& Profile);
};
