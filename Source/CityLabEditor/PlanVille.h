#pragma once

#include "CoreMinimal.h"

/**
 * =============================================================================
 * PLAN DE VILLE — LE LECTEUR-VALIDATEUR (etage 2, lot E2-0)
 * =============================================================================
 *
 * `Doc/Chantier-Plan-de-Ville.md` S2 (architecture), S8 (etage 2), S12 (contrat
 * livre). Le plan est COMPILE hors moteur (Python pur, `Tools/PlanVille/c0..c8`)
 * et installe dans `SourceData/PlanVille/`. Ici, le C++ ne DECIDE plus rien :
 * il lit un contrat, en verifie l'integrite, et REFUSE tout ce qui n'est pas
 * exactement ce que le manifeste declare.
 *
 * ⛔ LA GARDE (S2, garde_etage_2 du manifeste) :
 *   « un plan incomplet ou a l'empreinte fausse = build REFUSE, jamais devine
 *     (double empreinte octets + contenu logique LF) »
 * Il n'y a AUCUN chemin de passage partiel silencieux : toute anomalie entre
 * dans `FPlanRapport::Defauts` et fait echouer l'appel qui l'a trouvee.
 *
 * ⛔ DOUBLE EMPREINTE — la lecon CRLF, deja payee sur la carte de partition
 * (`CityImportTools.cpp`, garde de `carte_v2.json` : 660 913 fins de ligne
 * traduites, fichier INTACT, empreinte fausse). Le manifeste publie DEUX md5
 * par fichier : `md5_octets` (les octets tels quels) et `md5_logique` (les
 * memes octets, CRLF normalises en LF). Un fichier est accepte si son md5
 * d'octets egale `md5_octets`, OU si son md5 logique egale `md5_logique` — et
 * dans ce second cas on le DIT (les octets ont bouge, le contenu non).
 *
 * ⛔ DISTRICT-FIRST (convention du contrat, S12) : les parcelles et les
 * frontieres sont DECOUPEES PAR CELLULE. `ChargerCellule()` n'ouvre QUE les
 * trois side-cars de la cellule demandee — jamais un quatrieme. Le compteur
 * `FichiersOuverts()` le prouve, et un test d'automation le verifie.
 *
 * ⚠️ UNITES : tout ce que ce lecteur rend est dans LES UNITES DU PLAN, telles
 * que `plan_index.json` les declare — GEOMETRIES EN METRES (arrondies au mm),
 * Z = altitude NGF en metres, angles en degres. AUCUNE conversion en
 * centimetres moteur n'est faite ici : convertir serait deja construire, et
 * E2-0 ne construit rien. La conversion appartient a E2-1.
 *
 * ⚠️ AXES du plan : x = est (m), y = SUD (m), z = altitude NGF (m).
 */

DECLARE_LOG_CATEGORY_EXTERN(LogPlanVille, Log, All);

/** Les TROIS formes de loi de Z du plan (S3 ②, catalogue ferme anti-nappe). */
enum class EPlanForme : uint8
{
	Inconnue = 0,
	/** Place, pelouse encaissee : une cote unique pour toute la parcelle. */
	Constante,
	/** Rue : profil en long (abscisse, cote) le long d'un axe, deja regularise. */
	ProfilTroncon,
	/** Organique pur : le MNT intouche. */
	Drapage
};

/** La matiere du plan (S3 ③, catalogue ferme). */
enum class EPlanMatiere : uint8
{
	Inconnue = 0,
	Mineral,
	Vegetal,
	Eau
};

/** Le proprietaire de la parcelle (carte v2.1, catalogue ferme). */
enum class EPlanProprio : uint8
{
	Inconnu = 0,
	Batiment,
	Voirie,
	Ouvrage,
	Zone,
	Organique
};

/** La loi de Z d'une parcelle. Cotes et longueurs en METRES. */
struct FPlanLoiZ
{
	EPlanForme Forme = EPlanForme::Inconnue;

	/** Forme CONSTANTE : la cote NGF, en metres. */
	double ZM = 0.0;

	/** Forme PROFIL_TRONCON : longueur de l'axe, en metres. */
	double LM = 0.0;

	/** Forme PROFIL_TRONCON : plafond de pente applique, en pour cent. */
	double PenteMaxPc = 0.0;

	/** Forme PROFIL_TRONCON : l'axe du troncon, points (x, y) en metres. */
	TArray<FVector2D> Axe;

	/** Forme PROFIL_TRONCON : le profil en long, points (abscisse_m, cote_NGF_m). */
	TArray<FVector2D> Profil;

	/** Non vide si la loi est RECOPIEE de la parcelle citee (bandes annexees). */
	FString HeriteeDe;
};

/** Une PIECE de parcelle : la part d'une parcelle qui vit dans UNE cellule. */
struct FPlanParcelle
{
	/** Identifiant de la parcelle ENTIERE (`bat/20488#0`, `voi/7745#1`...). */
	FString Id;

	EPlanProprio Proprietaire = EPlanProprio::Inconnu;
	EPlanMatiere Matiere = EPlanMatiere::Inconnue;
	FPlanLoiZ Loi;

	/** Anneaux du polygone de la PIECE (le 1er est l'exterieur), en metres. */
	TArray<TArray<FVector2D>> Anneaux;

	/** Aire de la PIECE, en m2. */
	double AireM2 = 0.0;

	/** Aire de la parcelle ENTIERE, en m2 (identique dans chaque piece). */
	double AireTotaleM2 = 0.0;

	/**
	 * La cellule du point representatif de la parcelle ENTIERE. C'est la CLE de
	 * reconstitution des comptes distincts : une parcelle n'est comptee que dans
	 * sa cellule porteuse (mesure : 46 424 pieces porteuses = 46 424 ids
	 * distincts = le manifeste).
	 */
	FIntPoint CellulePorteuse = FIntPoint::ZeroValue;

	/** Vrai si la parcelle tient tout entiere dans cette cellule. */
	bool bEntiere = false;

	/** Champs optionnels des bandes annexees (absents ailleurs). */
	bool bBande = false;
	bool bHeritee = false;
	bool bTrouComble = false;
	double LargeurM = 0.0;
	FString Provenance;
};

/** Une PIECE de frontiere : la part d'une interface qui vit dans UNE cellule. */
struct FPlanInterface
{
	/** Les deux parcelles qui se rencontrent. Le COUPLE (A, B) est l'identite
	 *  de l'interface : mesure, 96 986 couples distincts = le manifeste. */
	FString A;
	FString B;

	/** La resolution du catalogue ferme, telle qu'ecrite dans le plan. */
	FName Resolution;

	/** Vrai si la resolution est `arbitrage_demande` : hors catalogue, NON
	 *  CONSTRUCTIBLE tant que l'utilisateur n'a pas tranche (S12). */
	bool bArbitrageDemande = false;

	/** Les polylignes de contact de la PIECE, en metres. */
	TArray<TArray<FVector2D>> Polylignes;

	/** Denivele le long du contact, en metres (median et maximum). */
	double DzM = 0.0;
	double DzMaxM = 0.0;

	/** Hauteur de l'ouvrage de resolution (mur, bordure...), en metres.
	 *  `bHauteurPubliee` est faux quand le plan n'en publie pas. */
	double HM = 0.0;
	bool bHauteurPubliee = false;

	/** Longueur de la PIECE et de la frontiere ENTIERE, en metres. */
	double LongueurM = 0.0;
	double LongueurTotaleM = 0.0;

	EPlanMatiere MatiereA = EPlanMatiere::Inconnue;
	EPlanMatiere MatiereB = EPlanMatiere::Inconnue;

	/** Motif publie par le compilateur (renseigne sur `arbitrage_demande`). */
	FString Motif;
};

/** Une instance de semis retenue par le plan. Positions en METRES. */
struct FPlanInstance
{
	FName Kind;
	FString Mesh;
	double X = 0.0;
	double Y = 0.0;
	double YawDeg = 0.0;
	double Echelle = 1.0;
};

/** Le contenu d'UNE cellule : ce qu'E2-1 aura a construire, et rien d'autre. */
struct FPlanCellule
{
	FIntPoint Cell = FIntPoint::ZeroValue;

	/** Cote de la cellule, en metres (declare par le side-car). */
	double CelluleM = 0.0;

	TArray<FPlanParcelle> Parcelles;
	TArray<FPlanInterface> Interfaces;
	TArray<FPlanInstance> Semis;

	/** Le catalogue ferme des resolutions, tel que le side-car le publie.
	 *  La liste des resolutions licites SORT DES DONNEES, elle n'y entre pas. */
	TArray<FName> Catalogue;

	/** Nombre d'interfaces `arbitrage_demande` trouvees dans cette cellule. */
	int32 ArbitragesN = 0;

	/**
	 * SECTEUR NON CONSTRUCTIBLE : faux des qu'une interface reclame un
	 * arbitrage. Le lecteur charge quand meme la cellule (on peut la lire,
	 * la mesurer, l'afficher) mais E2-1 a l'interdiction d'y batir.
	 */
	bool bConstructible = true;

	int32 ParcellesDistinctesPortees = 0;
};

/** Un defaut constate. Aucun n'est tolere : leur presence = refus. */
struct FPlanDefaut
{
	/** Chemin ou nom du fichier fautif (vide si le defaut porte sur l'ensemble). */
	FString Fichier;

	/** Ce qui ne va pas, en clair, avec les chiffres attendus et trouves. */
	FString Motif;
};

/** Ce que rend toute operation du lecteur : des chiffres et, s'il y a lieu,
 *  la LISTE COMPLETE des defauts (jamais un seul, jamais un booleen nu). */
struct FPlanRapport
{
	TArray<FPlanDefaut> Defauts;

	/** Comptes de PIECES effectivement lues. */
	int32 ParcellesPieces = 0;
	int32 InterfacesPieces = 0;
	int32 Instances = 0;

	/** Comptes DISTINCTS reconstitues (voir `FPlanVille::ValiderTout`). */
	int32 ParcellesDistinctes = 0;
	int32 InterfacesDistinctes = 0;

	/** Fichiers dont l'empreinte a ete verifiee avec succes. */
	int32 FichiersVerifies = 0;

	/** Fichiers acceptes sur le CONTENU LOGIQUE seulement (fins de ligne
	 *  traduites depuis la compilation — contenu intact, octets differents). */
	int32 FichiersAcceptesEnLogique = 0;

	/** Les cellules REFUSEES a la construction (arbitrage en attente). */
	TArray<FIntPoint> CellulesRefusees;

	/** Duree de l'operation, en secondes. */
	double Secondes = 0.0;

	bool EstValide() const { return Defauts.Num() == 0; }

	void Ajoute(const FString& Fichier, const FString& Motif);

	/** Texte multi-ligne : la totalite des defauts (ou le resume du succes). */
	FString Texte(int32 MaxDefauts = 40) const;
};

/**
 * L'index du plan : `plan.json` (le manifeste) + `data/plan_index.json`
 * (l'inventaire des 147 side-cars avec leur double empreinte). C'est le SEUL
 * etat charge en permanence ; il pese quelques dizaines de ko.
 */
struct FPlanIndex
{
	/** Empreintes publiees d'un fichier du plan. */
	struct FEmpreinte
	{
		FString Md5Octets;
		FString Md5Logique;
		int64 Octets = 0;
	};

	/** Comptes de PIECES d'une cellule, tels que l'index les declare. */
	struct FComptesCellule
	{
		int32 Parcelles = 0;
		int32 Interfaces = 0;
		int32 Instances = 0;
	};

	FString Version;
	FString VersionDonnees;

	/** Le domaine : les cellules que le plan couvre (la carte fait autorite). */
	TArray<FIntPoint> Cellules;
	double CelluleM = 0.0;

	/** Totaux du manifeste — la verite a reconstituer. */
	int32 ParcellesDistinctes = 0;
	int32 ParcellesPieces = 0;
	int32 InterfacesDistinctes = 0;
	int32 InterfacesPieces = 0;
	int32 Instances = 0;

	/** Empreinte de chaque side-car, par nom de fichier (147 entrees). */
	TMap<FString, FEmpreinte> Fichiers;

	/** Comptes attendus par cellule (49 entrees). */
	TMap<FIntPoint, FComptesCellule> ParCellule;

	bool bCharge = false;
};

/**
 * LE LECTEUR-VALIDATEUR. Cycle d'usage :
 *
 *   FPlanVille Plan;
 *   FPlanRapport R;
 *   if (!Plan.Ouvrir(FPlanVille::DossierParDefaut(), R)) { REFUS — R.Texte() }
 *   if (!Plan.ValiderTout(R))                            { REFUS — R.Texte() }
 *   FPlanCellule C;
 *   if (!Plan.ChargerCellule(FIntPoint(0,0), C, R))      { REFUS — R.Texte() }
 *
 * `ValiderTout()` est la GARDE de build : elle lit les 147 side-cars, verifie
 * chaque empreinte, et RECONSTITUE les comptes distincts pour les confronter au
 * manifeste. `Ouvrir()` + `ChargerCellule()` est le chemin district-first
 * d'E2-1 : 2 fichiers d'index + 3 side-cars, rien de plus.
 */
class FPlanVille
{
public:
	/** `<ProjectDir>/SourceData/PlanVille`. */
	static FString DossierParDefaut();

	/**
	 * Charge et VERIFIE `plan.json` puis `data/plan_index.json`. Refus si l'un
	 * manque, ne parse pas, porte une empreinte fausse, ou si l'index contredit
	 * le manifeste (totaux, domaine, inventaire des 147 fichiers).
	 */
	bool Ouvrir(const FString& Dossier, FPlanRapport& Rapport);

	/**
	 * DISTRICT-FIRST : charge les TROIS side-cars de la cellule et rien
	 * d'autre. Verifie leur double empreinte, confronte leurs comptes a ceux
	 * que l'index declare pour cette cellule, et marque le secteur NON
	 * CONSTRUCTIBLE s'il porte une interface `arbitrage_demande`.
	 */
	bool ChargerCellule(const FIntPoint& Cell, FPlanCellule& Out, FPlanRapport& Rapport);

	/**
	 * LA GARDE DE BUILD. Parcourt les 49 cellules du domaine, verifie les 147
	 * empreintes, et reconstitue les comptes DISTINCTS :
	 *   - parcelles : une piece n'est comptee que dans sa `cellule_porteuse` ;
	 *   - interfaces : le couple (A, B) est l'identite — le plan ne publie pas
	 *     de cellule porteuse pour les frontieres, la reconstitution du compte
	 *     DISTINCT est donc GLOBALE par construction (elle n'a lieu qu'ici, pas
	 *     dans le chemin district-first) ;
	 *   - instances : elles ne sont pas decoupees, la somme suffit.
	 * Tout ecart avec le manifeste = defaut liste, retour false.
	 */
	bool ValiderTout(FPlanRapport& Rapport);

	const FPlanIndex& Index() const { return Idx; }
	const FString& Dossier() const { return Racine; }

	/** Nombre de fichiers ouverts depuis `Ouvrir()`. Sert de PREUVE au
	 *  district-first (un test d'automation en fait un critere). */
	int32 FichiersOuverts() const { return NbFichiersOuverts; }

	/** Nom de side-car d'une cellule : famille = `qui`/`interfaces`/`semis`. */
	static FString NomSideCar(const TCHAR* Famille, const FIntPoint& Cell);

	/** `"-1_-2"` <-> `FIntPoint(-1, -2)`. */
	static bool CelluleDepuisTexte(const FString& S, FIntPoint& Out);
	static FString TexteDeCellule(const FIntPoint& Cell);

private:
	/**
	 * Lit un fichier du plan et verifie sa double empreinte contre `Attendue`.
	 * Rend le texte JSON. Toute anomalie (absence, taille, empreinte) entre
	 * dans le rapport et rend false.
	 */
	bool LireEtVerifier(const FString& Chemin, const FString& NomCourt,
		const FPlanIndex::FEmpreinte& Attendue, FString& OutTexte, FPlanRapport& Rapport);

	/** Charge un side-car de cellule (empreinte comprise) et le parse. */
	bool ChargeSideCar(const TCHAR* Famille, const FIntPoint& Cell,
		TSharedPtr<class FJsonObject>& OutRoot, FPlanRapport& Rapport);

	FString Racine;
	FPlanIndex Idx;
	int32 NbFichiersOuverts = 0;
};
