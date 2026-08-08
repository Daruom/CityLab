#pragma once

#include "CoreMinimal.h"
#include "PlanVille.h"

/**
 * =============================================================================
 * LE SOL DU PLAN — la GEOMETRIE, et rien d'autre (lot E2-1)
 * =============================================================================
 *
 * `Doc/Chantier-Plan-de-Ville.md` S3 (le contrat) et S8 (etage 2).
 *
 * Ce module transforme les PARCELLES d'une cellule du plan en TRIANGLES, en
 * appliquant la loi de Z que le plan publie — et rien de plus. Il ne connait ni
 * materiau, ni couleur, ni UV, ni acteur : c'est `CityImportTools` qui pose. La
 * separation est volontaire (doctrine Playbook S11.3 ter : « le C++ reste un
 * poseur bete ») — toute la geometrie qui pourrait se tromper vit ici, isolee.
 *
 * ⛔ LOI DES NAPPES (Playbook S13.1) : il n'y a AUCUNE interpolation de Z ENTRE
 * deux parcelles. Chaque parcelle recoit la cote de SA loi, point. Les marches
 * nues qui apparaissent aux frontieres sont le PLAN tel qu'il est ; les pieces
 * qui les habillent (bordure, mur, emmarchement...) sont le lot E2-2.
 *
 * ⛔ Une subdivision INTERNE a une parcelle n'est pas une nappe : c'est
 * l'echantillonnage de sa propre loi (le MNT pour `drapage`, le profil en long
 * pour `profil_troncon`). Elle ne raccorde rien a rien.
 *
 * UNITES : entree en METRES (les unites du plan), sortie en CENTIMETRES MONDE,
 * Z deja rebase sur le Capitole comme tout le reste du moteur.
 */

/**
 * ⭐ LA PROFONDEUR DE JUPE — CONSTANTE NATIONALE, CHOISIE SUR MESURE.
 *
 * Chaque plaque de parcelle emet, sur TOUT son perimetre, une face verticale qui
 * descend de cette profondeur sous la cote locale. Sans elle, deux plaques a des
 * cotes differentes laissent voir LE CIEL entre elles : ce ne sont pas des faces
 * mal eclairees, ce sont des OUVERTURES REELLES (verdict utilisateur du 08/08).
 * Loi de composition du Playbook S13.2 : LE RECOUVREMENT EST BENIN, LE VIDE EST
 * FATAL — on prefere donc une jupe trop longue a une jupe trop courte.
 *
 * MESURE qui fixe la valeur (`work/E2/py/jupes_mesure.py`, les 117 929 interfaces
 * des 45 cellules CONSTRUCTIBLES) : la marche a fermer vaut p50 = 7,1 cm,
 * p90 = 42,1 cm, p99 = 2,72 m, p99,9 = 7,79 m, et son MAXIMUM national = 11,70 m.
 * Une jupe de 3 m laisserait encore 0,84 % des interfaces ouvertes ; 12,00 m
 * n'en laisse AUCUNE.
 *
 * Pourquoi ne pas etre plus econome : la profondeur ne coute AUCUN triangle (2
 * triangles par arete, quelle que soit sa hauteur), et partout ou il existe une
 * plaque voisine, le bas de la jupe est OCCULTE par cette voisine — seule la
 * hauteur de la marche est reellement visible. La profondeur ne se voit donc
 * qu'aux BORDS du monde construit et la ou il n'y a pas de voisine.
 */
constexpr double GPlanJupeProfondeurCm = 1200.0;

/** Un triangle du sol, en cm monde, Z rebase Capitole. */
struct FPlanSolTri
{
	FVector3f A;
	FVector3f B;
	FVector3f C;

	/**
	 * La normale SORTANTE de la face. `FCityMeshBuilder::AddPoly` s'en sert pour
	 * ORIENTER le polygone lui-meme (il retourne le winding si besoin) : donner
	 * la bonne normale suffit, il n'y a pas d'ordre de sommets a deviner.
	 * (0,0,1) = une plaque de sol ; horizontale = une jupe.
	 */
	FVector3f Normale = FVector3f(0.f, 0.f, 1.f);
};

/**
 * Les triangles d'une CLASSE DE SURFACE. La classe est un simple entier decide
 * par l'APPELANT (`ClasseDe`) : c'est lui qui connait la table des revetements
 * du projet, pas ce module. Ici on ne fait que grouper — aucune doctrine de
 * materiau n'entre dans la geometrie.
 */
struct FPlanSolLot
{
	int32 Classe = 0;
	TArray<FPlanSolTri> Tris;
};

/** Ce que la construction a fait — pour le resume et pour le juge. */
struct FPlanSolStats
{
	int32 Parcelles = 0;
	int32 Triangles = 0;

	/** Par loi de Z. */
	int32 Constante = 0;
	int32 ProfilTroncon = 0;
	int32 Drapage = 0;

	/** Par proprietaire (index = (int32)EPlanProprio). */
	int32 ParProprio[6] = { 0, 0, 0, 0, 0, 0 };

	/** Parcelles ecartees par le filtre de perimetre de l'appelant. */
	int32 HorsPerimetre = 0;

	/** Parcelles a TROU (plusieurs anneaux) effectivement traitees. */
	int32 AvecTrous = 0;

	/** Aire des anneaux exterieurs construits, m2 (temoin de couverture). */
	double AireM2 = 0.0;

	/**
	 * ⛔ Parcelles que la triangulation n'a PAS su rendre. Jamais devinees,
	 * jamais approximees : listees, comptees, et remontees a l'appelant.
	 */
	int32 Refusees = 0;
	TArray<FString> IdsRefuses;

	/** Sommets FONDUS parce que le plan ne les distingue pas (< 1 mm, sa propre
	 *  precision declaree). Compte, jamais tu. */
	int32 PointsFondus = 0;

	/** LES JUPES : aretes de perimetre habillees et triangles qu'elles ont coute
	 *  (2 par arete). Tous les anneaux comptent, TROUS COMPRIS — le bord d'un
	 *  trou est une fente comme une autre. */
	int32 JupeAretes = 0;
	int32 JupeTriangles = 0;

	/**
	 * ⛔ LES REPLIS DE REMPLISSAGE, comptes SEPAREMENT — un repli n'est jamais
	 * silencieux. `Solide` = le remplissage par nombre de tours a echoue (anneaux
	 * qui se touchent). `Generalise` = les aretes contraintes se CROISENT et le
	 * moteur n'a pas pu toutes les poser ; c'est le cas des anneaux qui
	 * s'auto-intersectent.
	 */
	int32 RemplissageSolide = 0;
	int32 RemplissageGeneralise = 0;


	/**
	 * ⭐ LA MESURE DE COPLANARITE — le risque de z-fighting, chiffre, jamais
	 * suppose. A chaque sommet pose, l'ecart entre le Z que le PLAN prescrit et
	 * le Z de la surface RENDUE aujourd'hui (le drapage, plafond de lit et
	 * profil de berge compris). Par proprietaire, en centimetres.
	 * Sur les parcelles `drapage` l'ecart est nul par construction : c'est la
	 * meme source. Il n'a de sens que sur `constante` et `profil_troncon` — et
	 * c'est precisement la ou un ouvrage existant peut se retrouver colle au
	 * sol du plan.
	 */
	int32 EcartN[6] = { 0, 0, 0, 0, 0, 0 };
	double EcartSommeCm[6] = { 0, 0, 0, 0, 0, 0 };
	double EcartMaxCm[6] = { 0, 0, 0, 0, 0, 0 };
	/** Sommets dont l'ecart est < 2 cm — le seuil du socle anti-z-fight deja
	 *  employe par le projet (`PartitionLiftCm`). */
	int32 EcartSous2cm[6] = { 0, 0, 0, 0, 0, 0 };
};

/** Filtre de PERIMETRE : quels proprietaires l'appelant veut voir construits.
 *  C'est une decision de LOT, pas une decision du plan — elle vit donc ici, en
 *  clair, et se revoque sans toucher a la geometrie. */
struct FPlanSolPerimetre
{
	bool bVoirie = true;
	bool bZone = true;
	bool bOrganique = true;
	bool bBatiment = true;
	bool bOuvrage = true;

	bool Accepte(EPlanProprio P) const
	{
		switch (P)
		{
		case EPlanProprio::Voirie:    return bVoirie;
		case EPlanProprio::Zone:      return bZone;
		case EPlanProprio::Organique: return bOrganique;
		case EPlanProprio::Batiment:  return bBatiment;
		case EPlanProprio::Ouvrage:   return bOuvrage;
		default:                      return false;
		}
	}
};

/**
 * Construit les triangles du sol d'UNE cellule du plan.
 *
 * @param Cellule    la cellule deja lue et validee par `FPlanVille`.
 * @param AltCapCm   l'altitude du Capitole en cm (le zero du monde moteur).
 * @param ZDrapageCm rend le Z moteur (cm) du MNT en (Xcm, Ycm) — exactement
 *                   celui du pipeline actuel, pour que la loi `drapage` ne
 *                   diverge pas d'un centimetre de ce qui existe.
 * @param PasM       longueur d'arete visee, en metres, pour la subdivision des
 *                   lois non constantes. Vient de la maille de sol EXISTANTE
 *                   (cote de cellule / GroundGridN) : aucune constante neuve.
 * @param Perimetre  quels proprietaires construire.
 * @param ClasseDe   rend la classe de surface d'une parcelle (l'appelant decide
 *                   : matiere, proprietaire, ou les deux — la table des
 *                   revetements du projet lui appartient).
 * @param OutLots    un lot par matiere rencontree.
 * @param Stats      comptes et refus.
 * @return false si au moins une parcelle a ete REFUSEE (l'appelant decide quoi
 *         en faire : le lot ne se tait jamais sur ce qu'il n'a pas su faire).
 */
bool ConstruirePlanSol(
	const FPlanCellule& Cellule,
	float AltCapCm,
	TFunctionRef<float(double, double)> ZDrapageCm,
	double PasM,
	const FPlanSolPerimetre& Perimetre,
	TFunctionRef<int32(const FPlanParcelle&)> ClasseDe,
	TArray<FPlanSolLot>& OutLots,
	FPlanSolStats& Stats);

/**
 * =============================================================================
 * LE Z DU SOL DU PLAN, INTERROGEABLE EN UN POINT (lot JUPES, chantier 3)
 * =============================================================================
 *
 * POURQUOI. Les murets herites (passe Murs, breaklines `SourceData/Murs`) lisent
 * leur Z sur `FRenderedGroundZ`, c'est-a-dire sur le DRAPE ANALYTIQUE du MNT.
 * Tant que le sol ETAIT ce drape, c'etait juste. Depuis que le sol vient du
 * plan, ce n'est plus le meme terrain : les murets flottent ou s'enfoncent
 * (verdict utilisateur du 08/08). Ils ne sont pas mal poses — ils sont poses sur
 * un sol qui n'existe plus.
 *
 * CE QUE C'EST. Un index spatial des parcelles d'UNE cellule qui repond : « en
 * (x, y), quelle parcelle, et a quelle cote SA loi la met ». C'est la MEME loi
 * de Z que `ConstruirePlanSol` applique — donc, par construction, le meme sol,
 * pas une approximation de celui-ci.
 *
 * ⛔ Il ne DEVINE jamais : hors de toute parcelle (bord de cellule, trou du
 * contrat), il rend false et l'appelant garde son comportement d'avant.
 */
class FPlanZ
{
public:
	/** Indexe les parcelles de la cellule. `AltCapCm` = le zero moteur. */
	void Construire(const FPlanCellule& Cellule, float AltCapCm,
		TFunctionRef<float(double, double)> ZDrapageCm);

	/** (Xcm, Ycm) monde -> Z moteur en cm. false = aucune parcelle ici. */
	bool At(double Xcm, double Ycm, float& OutZCm) const;

	bool EstConstruit() const { return bConstruit; }
	int32 Parcelles() const { return Sacs.Num(); }

private:
	/** Une parcelle reduite a ce qu'il faut pour repondre : ses anneaux et sa loi. */
	struct FSac
	{
		TArray<TArray<FVector2D>> Anneaux;   // metres, [0] = exterieur
		const FPlanLoiZ* Loi = nullptr;
		FVector2D Min = FVector2D::ZeroVector;
		FVector2D Max = FVector2D::ZeroVector;
		TArray<double> S;                    // abscisses cumulees de l'axe
	};

	TArray<FSac> Sacs;
	/** Grille reguliere : chaque case liste les parcelles dont la boite la touche. */
	TArray<TArray<int32>> Cases;
	FVector2D Origine = FVector2D::ZeroVector;
	double PasCaseM = 5.0;
	int32 NCases = 0;
	float AltCap = 0.f;
	/** COPIE de la fonction de drapage : la loi `drapage` doit rester lisible
	 *  apres la construction (la passe Murs interroge plus tard). */
	TFunction<float(double, double)> Drapage;
	bool bConstruit = false;
};
