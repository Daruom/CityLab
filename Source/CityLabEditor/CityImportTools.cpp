#include "CityImportTools.h"

#include "Algo/Reverse.h"
#include "TerrainSampler.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/LevelStreamingDynamic.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/HitResult.h"
#include "CollisionQueryParams.h"
#include "Engine/CollisionProfile.h"
#include "Engine/TextRenderActor.h"
#include "Components/TextRenderComponent.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "PlanSol.h"
#include "PlanVille.h"
#include "Misc/Paths.h"
// PARTITION : l'empreinte md5 de la carte, verifiee au chargement.
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
// Empattement des arbres : lecture des sommets du mesh (donnees de rendu LOD0).
#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogCityImport, Log, All);

namespace
{
	void RaiseError(const FString& Message)
	{
		UE_LOG(LogCityImport, Error, TEXT("%s"), *Message);
		UKismetSystemLibrary::RaiseScriptError(Message);
	}

	// LOT A-ter : le SOL demande 100 fois plus de texels que le streaming ne croit.
	// MESURE (lot 3, point 2) : le generateur ecrit l'UV0 des dalles EN METRES (les
	// materiaux M_Surf_* / MI_CityGround_* divisent l'UV0 par la taille physique du
	// scan) alors que le monde est en CENTIMETRES. La metrique de streaming raisonne
	// en unites monde par unite d'UV : elle croit la texture etiree 100 fois, demande
	// 100 fois moins de texels et retombe sur le plancher de 64 px — le sol reste
	// flou jusqu'a bout portant. 100 n'est donc pas un reglage a l'oeil, c'est le
	// rapport metre/centimetre. Pose A LA CREATION : c'est une propriete PAR
	// COMPOSANT, elle ne survit pas a une regeneration si on la met apres coup.
	const float GGroundStreamingDistanceMultiplier = 100.f;

	void ApplyGroundTextureStreaming(UStaticMeshComponent* Component)
	{
		if (Component)
		{
			Component->StreamingDistanceMultiplier = GGroundStreamingDistanceMultiplier;
		}
	}

	// Sampler MNT partage : la dalle 10k x 10k pese ~200 Mo decompressee, elle se
	// charge UNE fois et reste en cache pour tout l'import (et les suivants).
	// Cache un emplacement : rechargee seulement si les chemins changent.
	FTerrainSampler* GetTerrainSampler(const FString& PngPath, const FString& JsonPath)
	{
		static FTerrainSampler Sampler;
		static FString LoadedKey;
		const FString Key = PngPath + TEXT("|") + JsonPath;
		if (LoadedKey != Key)
		{
			LoadedKey.Empty();
			if (!Sampler.Load(PngPath, JsonPath))
			{
				return nullptr;
			}
			LoadedKey = Key;
		}
		return &Sampler;
	}

	// -------------------------------------------------------------------------
	// ⭐ CHANTIER FRONTIERE DE BERGE — LE PLAFOND DU LIT.
	//
	// Side-car `SourceData/Frontiere/frontiere_<x>_<y>.json`, champ `plafond_cm` :
	// une valeur par NOEUD de la grille de dalle ((GroundGridN + 1)^2), en
	// CENTIMETRES NGF, ou la sentinelle = aucune contrainte. Tout le reglage
	// (enfoncement sous le plan d'eau, marge de rive, seuil de type) vit dans la
	// CUISSON : le moteur ne fait que POSER, comme pour les murs, les ponts, l'eau.
	//
	// Le plafond ne repond QU'AUX NOEUDS. C'est volontaire : `BuildGroundGrid` et
	// `FRenderedGroundZ::At` n'echantillonnent que des noeuds (le second discretise
	// en floor(X/pas)*pas) et la surface RENDUE est plane entre eux. Repondre
	// ailleurs reviendrait a inventer une interpolation que personne ne demande —
	// et a plafonner des points de TERRE au voisinage d'une berge.
	// -------------------------------------------------------------------------
	struct FCityBedCeiling
	{
		enum { Sentinelle = 9999999 };

		// ⭐ CHANTIER PROFIL DE BERGE — LES DEUX NIVEAUX DE L'OUVRAGE.
		// Le meme fichier porte desormais, sur la MEME grille de noeuds, deux
		// grilles de plus :
		//   `plateforme_cm` : la COTE D'EAU LOCALE (cm NGF) sous la bande de quai
		//                     basse. Le moteur y ajoute QuayPlatformHeightM — la
		//                     seule valeur de DESIGN, elle vit ici (cf. .h).
		//   `esplanade_cm`  : le Z ABSOLU (cm NGF) de l'esplanade haute, mesure
		//                     LOIN du bord, pour la bande situee cote terre du mur.
		// Les deux FORCENT le noeud (elles ne le bornent pas) : la bande est un
		// OUVRAGE, pas du terrain drape. Sentinelle = rien, comportement historique.
		// ⭐ LOT SIMPLIFICATION BERGE — L'OUVRAGE, parce que la grille NE PEUT PAS.
		// Mesure (work/SIMPLE/s1_enquete.py) : la dalle PERCE le plan de plateforme
		// sur 61 % de l'emprise, et encore 14 % (depassement max 5,83 m) meme en
		// enfoncant les noeuds de bande de DIX metres — la rampe entre le noeud de
		// bande et le noeud d'esplanade traverse le plan A L'INTERIEUR de l'emprise
		// quelle que soit la profondeur. « Poser un ouvrage par-dessus la dalle » est
		// donc impossible : la bande CEDE LA PLACE.
		//   `ouvrage_quads` : indices lineaires (GY * GridN + GX) des quads de dalle
		//                     qui ne sont plus rendus par la grille ;
		//   `ouvrage_z_cm`  : a leur place, (K+1)^2 cotes en cm NGF au pas fin
		//                     (Step / K), dans l'ordre (sy, sx), quad par quad.
		// Hors bande, la cuisson y met la cote que la DALLE aurait rendue : les
		// aretes de BORD sont donc l'interpolation des memes noeuds que le voisin,
		// et la couture est exacte (pas de jour, pas de z-fight, une seule surface).
		struct FGrilles
		{
			TArray<int32> Plafond;
			TArray<int32> Plateforme;
			TArray<int32> Esplanade;
			TArray<int32> OuvrageZ;
			TMap<int32, int32> OuvrageQuads;   // index de quad -> rang dans OuvrageZ
			int32 OuvrageK = 0;
			bool bCharge = false;
		};

		FString Dir;
		int32 GridN = 0;
		float StepCm = 0.f;
		float AltCapCm = 0.f;
		bool bActive = false;
		bool bProfil = false;      // CHANTIER PROFIL : bQuayPlatform
		float PlatHeightCm = 0.f;  // CHANTIER PROFIL : QuayPlatformHeightM * 100
		mutable TMap<FIntPoint, FGrilles> Cells;
		mutable int32 Charges = 0;
		mutable int32 Manquants = 0;
		mutable int32 TailleKo = 0;
		mutable int32 NoeudsProfil = 0;

		void Reset()
		{
			Cells.Empty();
			Charges = 0;
			Manquants = 0;
			TailleKo = 0;
			NoeudsProfil = 0;
			bActive = false;
			bProfil = false;
			PlatHeightCm = 0.f;
			GridN = 0;
			StepCm = 0.f;
			Dir.Empty();
		}

		/** Lit un champ de (GridN+1)^2 entiers ; vide si absent ou mal dimensionne. */
		bool LireGrille(const TSharedPtr<FJsonObject>& Root, const TCHAR* Champ,
			TArray<int32>& Out, bool& bOutTailleKo) const
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!Root->TryGetArrayField(Champ, Arr))
			{
				return false;
			}
			const int32 Attendu = (GridN + 1) * (GridN + 1);
			if (Arr->Num() != Attendu)
			{
				bOutTailleKo = true;
				return false;
			}
			Out.Reserve(Attendu);
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Out.Add((int32)V->AsNumber());
			}
			return true;
		}

		const FGrilles* Grille(const FIntPoint& Key) const
		{
			if (const FGrilles* Found = Cells.Find(Key))
			{
				return Found->bCharge ? Found : nullptr;
			}
			FGrilles& Slot = Cells.Add(Key);
			const FString Path = FPaths::Combine(
				Dir, FString::Printf(TEXT("frontiere_%d_%d.json"), Key.X, Key.Y));
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				++Manquants;
				return nullptr;
			}
			TSharedPtr<FJsonObject> Root;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root)
				|| !Root.IsValid())
			{
				RaiseError(FString::Printf(TEXT("Frontiere file '%s' is not valid JSON."), *Path));
				return nullptr;
			}
			double BakedGrid = 0.0;
			if (Root->TryGetNumberField(TEXT("grilleN"), BakedGrid)
				&& (int32)BakedGrid != GridN)
			{
				// Une grille cuite pour une AUTRE resolution de dalle ne se
				// reechantillonne pas : on la refuse et on le DIT. Le silence
				// donnerait un lit a moitie ecrase, indebogable.
				++TailleKo;
				return nullptr;
			}
			bool bKo = false;
			const bool bPlaf = LireGrille(Root, TEXT("plafond_cm"), Slot.Plafond, bKo);
			// CHANTIER PROFIL : side-car anterieur = champs absents, et tout se
			// comporte comme avant, bit pour bit.
			LireGrille(Root, TEXT("plateforme_cm"), Slot.Plateforme, bKo);
			LireGrille(Root, TEXT("esplanade_cm"), Slot.Esplanade, bKo);
			if (bKo)
			{
				++TailleKo;
				return nullptr;
			}
			if (!bPlaf)
			{
				++Manquants;
				return nullptr;
			}
			for (const int32 V : Slot.Plateforme)
			{
				NoeudsProfil += (V < (int32)Sentinelle) ? 1 : 0;
			}
			for (const int32 V : Slot.Esplanade)
			{
				NoeudsProfil += (V < (int32)Sentinelle) ? 1 : 0;
			}
			// LOT SIMPLIFICATION : l'ouvrage de berge. Champs absents = side-car
			// anterieur, et tout se comporte comme avant, bit pour bit.
			double KOuv = 0.0;
			const TArray<TSharedPtr<FJsonValue>>* QArr = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* ZArr = nullptr;
			if (Root->TryGetNumberField(TEXT("ouvrage_k"), KOuv) && (int32)KOuv > 0
				&& Root->TryGetArrayField(TEXT("ouvrage_quads"), QArr)
				&& Root->TryGetArrayField(TEXT("ouvrage_z_cm"), ZArr))
			{
				const int32 K = (int32)KOuv;
				const int32 NV = (K + 1) * (K + 1);
				if (QArr->Num() > 0 && ZArr->Num() == QArr->Num() * NV)
				{
					Slot.OuvrageK = K;
					Slot.OuvrageZ.Reserve(ZArr->Num());
					for (const TSharedPtr<FJsonValue>& V : *ZArr)
					{
						Slot.OuvrageZ.Add((int32)V->AsNumber());
					}
					for (int32 q = 0; q < QArr->Num(); ++q)
					{
						Slot.OuvrageQuads.Add((int32)(*QArr)[q]->AsNumber(), q);
					}
				}
				else
				{
					// Jamais en silence : une taille incoherente se DIT (la cellule
					// n'est pas refusee pour autant — le plafond reste valable).
					UE_LOG(LogCityImport, Display,
						TEXT("Frontiere %d_%d : ouvrage de berge IGNORE (quads=%d, z=%d, attendu %d)."),
						Key.X, Key.Y, QArr->Num(), ZArr->Num(), QArr->Num() * NV);
				}
			}
			Slot.bCharge = true;
			++Charges;
			return &Slot;
		}

		/** Index du noeud (Xcm, Ycm) dans sa cellule, ou nullptr hors noeud. */
		const FGrilles* Noeud(double Xcm, double Ycm, int32& OutIndex) const
		{
			if (!bActive || GridN <= 0 || StepCm <= 0.f)
			{
				return nullptr;
			}
			const double Fi = Xcm / (double)StepCm;
			const double Fj = Ycm / (double)StepCm;
			const double Ri = FMath::RoundToDouble(Fi);
			const double Rj = FMath::RoundToDouble(Fj);
			if (FMath::Abs(Fi - Ri) > 1e-4 || FMath::Abs(Fj - Rj) > 1e-4)
			{
				return nullptr;
			}
			const int64 I = (int64)Ri;
			const int64 J = (int64)Rj;
			const int32 Cx = (int32)FMath::FloorToDouble((double)I / (double)GridN);
			const int32 Cy = (int32)FMath::FloorToDouble((double)J / (double)GridN);
			const FGrilles* G = Grille(FIntPoint(Cx, Cy));
			if (!G)
			{
				return nullptr;
			}
			const int32 Li = (int32)(I - (int64)Cx * GridN);
			const int32 Lj = (int32)(J - (int64)Cy * GridN);
			if (Li < 0 || Lj < 0 || Li > GridN || Lj > GridN)
			{
				return nullptr;
			}
			OutIndex = Lj * (GridN + 1) + Li;
			return G;
		}

		/** Plafond (Z Unreal cm) AU NOEUD (Xcm, Ycm) ; +inf partout ailleurs. */
		float At(double Xcm, double Ycm) const
		{
			int32 Idx = 0;
			const FGrilles* G = Noeud(Xcm, Ycm, Idx);
			if (!G || !G->Plafond.IsValidIndex(Idx))
			{
				return TNumericLimits<float>::Max();
			}
			const int32 V = G->Plafond[Idx];
			if (V >= (int32)Sentinelle)
			{
				return TNumericLimits<float>::Max();
			}
			return (float)V - AltCapCm;
		}

		/**
		 * ⭐ CHANTIER PROFIL — le Z FORCE de la bande de berge (Unreal cm), ou
		 * -inf si ce noeud n'appartient a aucun des deux niveaux de l'ouvrage.
		 * Plateforme d'abord (elle est cote eau du mur), esplanade ensuite.
		 */
		float ProfilAt(double Xcm, double Ycm) const
		{
			if (!bProfil)
			{
				return TNumericLimits<float>::Lowest();
			}
			int32 Idx = 0;
			const FGrilles* G = Noeud(Xcm, Ycm, Idx);
			if (!G)
			{
				return TNumericLimits<float>::Lowest();
			}
			if (G->Plateforme.IsValidIndex(Idx))
			{
				const int32 V = G->Plateforme[Idx];
				if (V < (int32)Sentinelle)
				{
					// V = cote d'eau LOCALE, mesuree. La hauteur au-dessus d'elle
					// est le seul choix de design, et il est dans le profil.
					return (float)V + PlatHeightCm - AltCapCm;
				}
			}
			if (G->Esplanade.IsValidIndex(Idx))
			{
				const int32 V = G->Esplanade[Idx];
				if (V < (int32)Sentinelle)
				{
					return (float)V - AltCapCm;
				}
			}
			return TNumericLimits<float>::Lowest();
		}
	};

	FCityBedCeiling& BedCeilingSingleton()
	{
		static FCityBedCeiling Bed;
		return Bed;
	}

	// =========================================================================
	// ⭐ CHANTIER BUILDQUAY — LE CONSTRUCTEUR DE QUAI. `BuildQuay` ne CALCULE
	// rien : il POSE, exactement comme les ponts, les murs et l'eau.
	//
	// Le diagnostic acte : huit mecanismes se superposaient sur la bande de
	// berge (forcage de noeuds, pieces d'ouvrage sur quads, murs C1, peinture,
	// dalle...) et le chaos vivait dans leurs COUTURES. Dans ce projet, tout ce
	// qui n'a jamais fait de grief est un CONSTRUCTEUR ; tout ce qui fait grief
	// est du TERRAIN MODIFIE. La bande `quai_dur` est donc CONSTRUITE D'UN BLOC,
	// et dans son emprise le reste n'existe plus.
	//
	// La cuisson (`work/BUILDQUAY/bq2_quai.py`) balaye le profil transversal le
	// long de la CHAINE de frontiere — donc aligne sur elle, jamais sur la
	// grille de la dalle : face verticale sur la ligne de frontiere, plateforme
	// plate a `cote d'eau + 1,20 m` la ou la largeur existe, mur (ou GRADINS
	// tailles dans la meme piece) jusqu'au couronnement a la cote d'esplanade,
	// tablier, puis raccord qui rejoint la dalle a son Z EXACT — l'unique
	// couture, propre par construction.
	//
	// =========================================================================
	// ⭐ CHANTIER SOL DE BERGE (04/08) — LE FORMAT A CHANGE DE NATURE.
	//
	// Ce qui precede reste vrai pour l'HISTOIRE, mais le side-car ne porte plus
	// une PIECE posee par-dessus la dalle : il porte LA DALLE ELLE-MEME dans la
	// bande. Un quai n'est pas un objet pose sur le terrain — un quai EST le
	// terrain. Dans les quads traverses par la bande, la grille reguliere est
	// remplacee par une TRIANGULATION CONTRAINTE DU QUAD ENTIER, cuite hors
	// moteur ; les quatre coins du quad restent les NOEUDS DE LA GRILLE et les
	// seuls autres sommets d'un bord de quad sont les traversees des lignes
	// contraintes, pre-decoupees sur les lignes de grille — donc partagees a
	// l'identique par les deux quads voisins. Coutures, trous et chevauchements
	// sont IMPOSSIBLES par construction, pas corriges.
	//
	//   `bande_quads`   : quads de dalle (GY*GridN+GX) que la triangulation
	//                     REMPLACE. Ce n'est PAS un masque : rien n'est cache
	//                     derriere, la triangulation EST la couverture (la
	//                     cuisson mesure 0,00e+00 m2 d'ecart a l'aire du quad) ;
	//   `tris_cm`       : 9 REELS par triangle (3 sommets x, y, z en cm NGF).
	//                     REELS et non entiers : le pas de grille vaut 781,25 cm
	//                     et un arrondi au cm decalerait les coins de 2,5 mm —
	//                     le partage de sommets ne serait plus EXACT ;
	//   `tris_uv_cm`    : 6 reels (3 UV en cm : monde en plan pour un pan
	//                     horizontal, arc x hauteur pour un pan vertical) ;
	//   `tris_slot`     : 0 = pan horizontal (revetement de sol), 1 = pan
	//                     VERTICAL (pierre de quai — face de quai et face de
	//                     mur vivent dans le MEME maillage) ;
	//   `tris_quad`     : le quad de rattachement (index de `bande_quads`) —
	//                     c'est lui qui permet a `FRenderedGroundZ` de repondre
	//                     JUSTE sur la nouvelle surface, sans recherche globale ;
	//   `objets_cm` / `objets_uv_cm` : les GRADINS et les VOLEES. Ce ne sont plus
	//                     des entailles taillees dans le sol : ce sont des OBJETS
	//                     POSES SUR le sol, comme les batiments (ils vont dans le
	//                     mesh `SM_Ground_` de la cellule, pas dans la dalle) ;
	//   `murs_exclus`   : les murs BD TOPO de classe `quai` que la piece
	//                     remplace (exclusion PAR TYPE — C1 garde la ville) ;
	//   `gradins_dm` / `gradins_n` : ce que la bande porte, pour le resume.
	// DOSSIER ABSENT = AUCUNE BANDE, sans erreur : c'est le contrat des autres
	// side-cars, et c'est le rollback (effacer le dossier, aucune recompilation :
	// la bande redevient du drapage regulier historique, bit pour bit).
	struct FCityQuay
	{
		struct FCell
		{
			TSet<int32> Bande;              // quads REMPLACES par la triangulation
			TArray<FVector3f> Tris;         // 3 sommets par triangle
			TArray<FVector2f> TriUVs;       // 3 UV par triangle
			TArray<uint8> TriSlot;          // 1 par triangle
			TArray<int32> TriQuad;          // 1 par triangle
			TMap<int32, TArray<int32>> ParQuad;   // quad -> rangs de triangle
			TArray<FVector3f> Objets;       // 3 sommets par triangle (poses)
			TArray<FVector2f> ObjUVs;
			int32 GradinsDm = 0;
			int32 GradinsN = 0;
			bool bCharge = false;
		};

		FString Dir;
		int32 GridN = 0;
		float AltCapCm = 0.f;
		bool bActive = false;
		TSet<FString> MursExclus;
		// QUAIV2 : les volees BD TOPO que la piece RECONSTRUIT en entaille
		// (cf. `volees_constructeur` de l'index) — `BuildStairs` ne doit plus
		// les draper. Vide = comportement d'avant ce lot.
		TSet<FString> VoleesConstructeur;
		mutable TMap<FIntPoint, FCell> Cells;
		mutable int32 Charges = 0;
		mutable int32 Triangles = 0;
		mutable int32 TrisVerticaux = 0;
		mutable int32 ObjetsTris = 0;
		mutable int32 QuadsBande = 0;

		void Reset()
		{
			Cells.Empty();
			MursExclus.Empty();
			VoleesConstructeur.Empty();
			Dir.Empty();
			GridN = 0;
			AltCapCm = 0.f;
			bActive = false;
			Charges = Triangles = TrisVerticaux = ObjetsTris = QuadsBande = 0;
		}

		static bool LireEntiers(const TSharedPtr<FJsonObject>& Root, const TCHAR* Champ,
			TArray<int32>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!Root->TryGetArrayField(Champ, Arr))
			{
				return false;
			}
			Out.Reserve(Arr->Num());
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Out.Add((int32)V->AsNumber());
			}
			return true;
		}

		/**
		 * SOL DE BERGE : les positions sont des REELS en cm, pas des entiers.
		 * Le pas de la grille de dalle vaut 781,25 cm — arrondir au cm decalerait
		 * les coins de quad de 2,5 mm et le partage de sommets avec la grille
		 * reguliere ne serait plus EXACT. C'est toute la difference entre « une
		 * couture propre » et « pas de couture ».
		 */
		static bool LireReels(const TSharedPtr<FJsonObject>& Root, const TCHAR* Champ,
			TArray<double>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!Root->TryGetArrayField(Champ, Arr))
			{
				return false;
			}
			Out.Reserve(Arr->Num());
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Out.Add(V->AsNumber());
			}
			return true;
		}

		const FCell* Cellule(const FIntPoint& Key) const
		{
			if (!bActive)
			{
				return nullptr;
			}
			if (const FCell* Found = Cells.Find(Key))
			{
				return Found->bCharge ? Found : nullptr;
			}
			FCell& Slot = Cells.Add(Key);
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir,
				FString::Printf(TEXT("quai_%d_%d.json"), Key.X, Key.Y))))
			{
				return nullptr;
			}
			TSharedPtr<FJsonObject> Root;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root)
				|| !Root.IsValid())
			{
				RaiseError(FString::Printf(TEXT("Quai file for cell %d_%d is not valid JSON."),
					Key.X, Key.Y));
				return nullptr;
			}
			double G = 0.0;
			if (Root->TryGetNumberField(TEXT("grilleN"), G) && (int32)G != GridN)
			{
				// Une piece cuite pour une AUTRE resolution de dalle ne se
				// reechantillonne pas : on la refuse et on le DIT (Playbook S10).
				UE_LOG(LogCityImport, Display,
					TEXT("Quai %d_%d : grilleN cuite %d != %d — PIECE IGNOREE."),
					Key.X, Key.Y, (int32)G, GridN);
				return nullptr;
			}
			TArray<int32> B, S, QI;
			TArray<double> T, U, O, OU;
			LireEntiers(Root, TEXT("bande_quads"), B);
			LireEntiers(Root, TEXT("tris_slot"), S);
			LireEntiers(Root, TEXT("tris_quad"), QI);
			LireReels(Root, TEXT("tris_cm"), T);
			LireReels(Root, TEXT("tris_uv_cm"), U);
			LireReels(Root, TEXT("objets_cm"), O);
			LireReels(Root, TEXT("objets_uv_cm"), OU);
			const int32 NT = S.Num();
			if (T.Num() != NT * 9 || U.Num() != NT * 6 || QI.Num() != NT
				|| (O.Num() % 9) != 0 || OU.Num() != (O.Num() / 9) * 6)
			{
				// Jamais en silence : une taille incoherente se DIT.
				UE_LOG(LogCityImport, Display,
					TEXT("Quai %d_%d : tailles incoherentes (tris=%d pos=%d uv=%d quad=%d objets=%d/%d) — BANDE IGNOREE."),
					Key.X, Key.Y, NT, T.Num(), U.Num(), QI.Num(), O.Num(), OU.Num());
				return nullptr;
			}
			for (const int32 V : B)
			{
				Slot.Bande.Add(V);
			}
			Slot.Tris.Reserve(NT * 3);
			Slot.TriUVs.Reserve(NT * 3);
			Slot.TriSlot.Reserve(NT);
			Slot.TriQuad.Reserve(NT);
			for (int32 i = 0; i < NT; ++i)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					Slot.Tris.Add(FVector3f((float)T[i * 9 + c * 3], (float)T[i * 9 + c * 3 + 1],
						(float)T[i * 9 + c * 3 + 2] - AltCapCm));
					Slot.TriUVs.Add(FVector2f((float)(U[i * 6 + c * 2] * 0.01),
						(float)(U[i * 6 + c * 2 + 1] * 0.01)));
				}
				Slot.TriSlot.Add((uint8)S[i]);
				Slot.TriQuad.Add(QI[i]);
				// L'INDEX PAR QUAD : c'est ce qui rend `FRenderedGroundZ` juste
				// sur la nouvelle surface a cout constant (un triangle appartient
				// a UN seul quad, par construction de la cuisson).
				if (S[i] == 0)
				{
					Slot.ParQuad.FindOrAdd(QI[i]).Add(i);
				}
				TrisVerticaux += (S[i] != 0) ? 1 : 0;
			}
			Slot.Objets.Reserve(O.Num() / 3);
			for (int32 i = 0; i + 2 < O.Num(); i += 3)
			{
				Slot.Objets.Add(FVector3f((float)O[i], (float)O[i + 1], (float)O[i + 2] - AltCapCm));
			}
			Slot.ObjUVs.Reserve(OU.Num() / 2);
			for (int32 i = 0; i + 1 < OU.Num(); i += 2)
			{
				Slot.ObjUVs.Add(FVector2f((float)(OU[i] * 0.01), (float)(OU[i + 1] * 0.01)));
			}
			double D = 0.0;
			Root->TryGetNumberField(TEXT("gradins_dm"), D);
			Slot.GradinsDm = (int32)D;
			D = 0.0;
			Root->TryGetNumberField(TEXT("gradins_n"), D);
			Slot.GradinsN = (int32)D;
			// `murs_exclus` est lu UNE fois dans l'index (les murs sont poses
			// AVANT que la moindre cellule de quai soit chargee : le lire ici
			// arriverait trop tard).
			Slot.bCharge = true;
			++Charges;
			Triangles += NT;
			ObjetsTris += Slot.Objets.Num() / 3;
			QuadsBande += Slot.Bande.Num();
			return &Slot;
		}

		/**
		 * ⭐ LE POINT DUR DU CHANTIER : LES LECTEURS DU SOL.
		 * Z (Unreal cm) de la surface de bande sous (Xcm, Ycm), ou -inf si le
		 * point n'est pas dans un quad de bande. Le triangle est cherche dans le
		 * SEUL quad qui peut le contenir. Quand plusieurs pans horizontaux se
		 * superposent en plan (jamais dans la bande : la cuisson mesure 0,00e+00
		 * m2 de recouvrement — mais la garde reste), on rend LE PLUS HAUT : c'est
		 * la meme regle que le lancer de rayon de la vegetation.
		 */
		float SurfaceZ(double Xcm, double Ycm, float CellCm) const
		{
			if (!bActive || GridN <= 0 || CellCm <= 0.f)
			{
				return TNumericLimits<float>::Lowest();
			}
			const double Step = (double)CellCm / (double)GridN;
			const FIntPoint Key((int32)FMath::FloorToDouble(Xcm / (double)CellCm),
				(int32)FMath::FloorToDouble(Ycm / (double)CellCm));
			const FCell* C = Cellule(Key);
			if (!C)
			{
				return TNumericLimits<float>::Lowest();
			}
			const int32 GX = (int32)FMath::FloorToDouble(
				(Xcm - (double)Key.X * CellCm) / Step);
			const int32 GY = (int32)FMath::FloorToDouble(
				(Ycm - (double)Key.Y * CellCm) / Step);
			if (GX < 0 || GY < 0 || GX >= GridN || GY >= GridN)
			{
				return TNumericLimits<float>::Lowest();
			}
			const TArray<int32>* Rangs = C->ParQuad.Find(GY * GridN + GX);
			if (!Rangs)
			{
				return TNumericLimits<float>::Lowest();
			}
			float Best = TNumericLimits<float>::Lowest();
			for (const int32 R : *Rangs)
			{
				const FVector3f& A = C->Tris[R * 3];
				const FVector3f& B2 = C->Tris[R * 3 + 1];
				const FVector3f& C2 = C->Tris[R * 3 + 2];
				const double D = (double)(B2.Y - C2.Y) * (A.X - C2.X)
					+ (double)(C2.X - B2.X) * (A.Y - C2.Y);
				if (FMath::Abs(D) < 1e-9)
				{
					continue;
				}
				const double L1 = ((double)(B2.Y - C2.Y) * (Xcm - C2.X)
					+ (double)(C2.X - B2.X) * (Ycm - C2.Y)) / D;
				const double L2 = ((double)(C2.Y - A.Y) * (Xcm - C2.X)
					+ (double)(A.X - C2.X) * (Ycm - C2.Y)) / D;
				const double L3 = 1.0 - L1 - L2;
				if (L1 < -1e-6 || L2 < -1e-6 || L3 < -1e-6)
				{
					continue;
				}
				Best = FMath::Max(Best,
					(float)(L1 * A.Z + L2 * B2.Z + L3 * C2.Z));
			}
			return Best;
		}
	};

	FCityQuay& QuaySingleton()
	{
		static FCityQuay Quay;
		return Quay;
	}

	// =========================================================================
	// ⭐ CHANTIER PARTITION DU SOL (E2-a) — LA CARTE DE PROPRIETE DU SOL.
	//
	// Jusqu'ici le sol etait « le reste » : tout ce que personne ne reclamait
	// retombait dans le drapage du MNT, et les frontieres etaient EMERGENTES,
	// donc defectueuses (c'est la cause unique des 5 griefs de la saga berge).
	// La carte `carte/v2.1`, cuite hors moteur et figee par son md5, dit QUI
	// possede chaque m2.
	//
	// Ce que le moteur en fait, et RIEN d'autre (Playbook S11.3 ter — la
	// cuisson livre, le moteur pose) :
	//   * il VERIFIE l'empreinte du fichier (une carte qui a bouge sans qu'on
	//     le sache serait pire qu'une carte absente) ;
	//   * il pose les BANDES en RUBANS sur leur LIGNE PORTEUSE, au Z de la
	//     surface RENDUE, avec une classe de revetement EXISTANTE ;
	//   * il COUD les FRONTIERES ou la surface presente une marche.
	//
	// Absent / illisible / empreinte fausse = AUCUN effet, sans erreur : le
	// generateur se comporte exactement comme avant, bit pour bit. C'est la
	// meme convention que le plafond du lit et le constructeur de quai.
	// =========================================================================
	struct FCityPartition
	{
		enum class EProprio : uint8 { Ouvrage, Voirie, Batiment, Zone, Inconnu };

		struct FBande
		{
			EProprio Proprio = EProprio::Inconnu;
			FIntPoint Cellule = FIntPoint::ZeroValue;
			double AireM2 = 0.0;
			// Anneau exterieur, en cm monde. Les trous ne sont pas portes : une
			// bande de 10 m2 n'en a pas (mesure : 0 trou sur 6 589 bandes).
			TArray<FVector2D> Ext;
			// Les LIGNES PORTEUSES : le contact avec le proprietaire. Le ruban
			// se pose SUR elles — c'est ce qui garantit qu'il part du bon bord.
			TArray<TArray<FVector2D>> Lignes;
		};

		struct FFrontiere
		{
			FIntPoint Cellule = FIntPoint::ZeroValue;
			bool bEngagee = false;      // le dur est a moins de 3 m (engagement d'E0)
			double LongueurM = 0.0;
			TArray<FVector2D> Poly;     // cm monde
		};

		bool bActive = false;
		FString Fichier;
		FString Version;
		FString Md5Fichier;
		FString Md5Geometries;
		float CelluleM = 0.f;
		// LA REGLE, telle que la CARTE la publie — pas une constante moteur. C'est
		// elle qui borne la largeur d'une bande : le C++ ne redecide rien.
		float BandeMaxM = 0.f;
		float CollierM = 0.f;
		TArray<FBande> Bandes;
		TArray<FFrontiere> Frontieres;

		void Reset()
		{
			bActive = false;
			Fichier.Empty();
			Version.Empty();
			Md5Fichier.Empty();
			Md5Geometries.Empty();
			CelluleM = 0.f;
			Bandes.Reset();
			Frontieres.Reset();
		}

		static EProprio ProprioDe(const FString& S)
		{
			if (S == TEXT("ouvrage")) { return EProprio::Ouvrage; }
			if (S == TEXT("voirie")) { return EProprio::Voirie; }
			if (S == TEXT("batiment")) { return EProprio::Batiment; }
			if (S == TEXT("zone")) { return EProprio::Zone; }
			return EProprio::Inconnu;
		}

		static const TCHAR* NomDe(EProprio P)
		{
			switch (P)
			{
			case EProprio::Ouvrage:  return TEXT("ouvrage");
			case EProprio::Voirie:   return TEXT("voirie");
			case EProprio::Batiment: return TEXT("batiment");
			case EProprio::Zone:     return TEXT("zone");
			default:                 return TEXT("inconnu");
			}
		}

		// Les coordonnees de la carte sont en METRES (comme toutes les cuissons
		// du projet) ; le moteur travaille en cm. Une seule conversion, ici.
		static void LirePoly(const TArray<TSharedPtr<FJsonValue>>& In, TArray<FVector2D>& Out)
		{
			Out.Reset(In.Num());
			for (const TSharedPtr<FJsonValue>& V : In)
			{
				const TArray<TSharedPtr<FJsonValue>>* C = nullptr;
				if (V->TryGetArray(C) && C->Num() >= 2)
				{
					Out.Add(FVector2D((*C)[0]->AsNumber() * 100.0, (*C)[1]->AsNumber() * 100.0));
				}
			}
		}

		/**
		 * Charge la carte et VERIFIE son empreinte. Le md5 attendu vit dans le
		 * fichier `.md5` depose a cote par la cuisson : c'est la cuisson qui
		 * fait autorite sur ce qu'elle a produit, pas une constante moteur qu'il
		 * faudrait recompiler a chaque recuisson. Empreinte absente = on
		 * journalise et on charge quand meme (la garde protege de la CORRUPTION,
		 * elle n'interdit pas de travailler sans elle) ; empreinte PRESENTE ET
		 * FAUSSE = refus net.
		 */
		bool Charger(const FString& Dir)
		{
			Reset();
			const FString Chemin = FPaths::Combine(Dir, TEXT("carte_v2.json"));
			FString Texte;
			if (!FFileHelper::LoadFileToString(Texte, *Chemin))
			{
				return false;   // pas de carte : pas d'objet, et pas d'erreur
			}
			// L'empreinte se calcule SUR LES OCTETS **et** sur le contenu LOGIQUE
			// (fins de ligne normalisees en LF), et l'attendu doit egaler l'une des
			// deux. Ce n'est pas une complaisance : c'est le seul moyen qu'une
			// empreinte survive a une TRADUCTION DE FINS DE LIGNE, que le systeme
			// fait dans le dos de tout le monde (`open(..., 'w')` de Python sur
			// Windows, `git core.autocrlf`, un editeur qui resauve). Cas paye ici
			// meme : la carte publiee etait empreintee AVANT sa traduction CRLF —
			// 660 913 fins de ligne changees, fichier INTACT, empreinte fausse.
			// Ce qu'on veut garantir, c'est que le CONTENU n'a pas bouge.
			auto Md5De = [](const uint8* Donnees, int32 Taille)
			{
				FMD5 H;
				H.Update(Donnees, Taille);
				uint8 D[16];
				H.Final(D);
				FString S;
				for (int32 i = 0; i < 16; ++i) { S += FString::Printf(TEXT("%02x"), D[i]); }
				return S;
			};
			TArray<uint8> Octets;
			FString CalculeOctets, CalculeLogique;
			if (FFileHelper::LoadFileToArray(Octets, *Chemin))
			{
				CalculeOctets = Md5De(Octets.GetData(), Octets.Num());
				TArray<uint8> Normalise;
				Normalise.Reserve(Octets.Num());
				for (int32 i = 0; i < Octets.Num(); ++i)
				{
					if (Octets[i] == '\r' && i + 1 < Octets.Num() && Octets[i + 1] == '\n')
					{
						continue;   // CRLF -> LF
					}
					Normalise.Add(Octets[i]);
				}
				CalculeLogique = Md5De(Normalise.GetData(), Normalise.Num());
			}
			const FString Calcule = CalculeOctets;
			FString Attendu;
			if (FFileHelper::LoadFileToString(Attendu, *(Chemin + TEXT(".md5"))))
			{
				Attendu = Attendu.TrimStartAndEnd().ToLower();
				// Le fichier peut porter « <md5>  <nom> » (format md5sum).
				FString G, D;
				if (Attendu.Split(TEXT(" "), &G, &D)) { Attendu = G.TrimStartAndEnd(); }
				if (!Attendu.IsEmpty() && Attendu != CalculeOctets && Attendu != CalculeLogique)
				{
					UE_LOG(LogCityImport, Display,
						TEXT("PARTITION : carte REFUSEE — md5 attendu %s ; calcule sur les octets %s, "
							 "sur le contenu logique %s ('%s'). "
							 "Aucune bande, aucune couture : le sol reste EXACTEMENT ce qu'il etait."),
						*Attendu, *CalculeOctets, *CalculeLogique, *Chemin);
					return false;
				}
				if (!Attendu.IsEmpty() && Attendu != CalculeOctets)
				{
					UE_LOG(LogCityImport, Display,
						TEXT("PARTITION : empreinte VERIFIEE sur le CONTENU LOGIQUE (%s) — les octets "
							 "du fichier donnent %s (fins de ligne traduites depuis la cuisson). "
							 "Le contenu n'a pas bouge."),
						*CalculeLogique, *CalculeOctets);
				}
			}
			else
			{
				UE_LOG(LogCityImport, Display,
					TEXT("PARTITION : pas de '%s.md5' — carte chargee SANS garde d'empreinte."),
					*Chemin);
			}
			TSharedPtr<FJsonObject> Root;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Texte), Root) || !Root.IsValid())
			{
				UE_LOG(LogCityImport, Display,
					TEXT("PARTITION : '%s' n'est pas un JSON valide — carte IGNOREE."), *Chemin);
				return false;
			}
			Root->TryGetStringField(TEXT("version"), Version);
			Root->TryGetStringField(TEXT("empreinte_geometries_v2"), Md5Geometries);
			double CellM = 0.0;
			Root->TryGetNumberField(TEXT("cellule_m"), CellM);
			CelluleM = (float)CellM;
			const TSharedPtr<FJsonObject>* Regle = nullptr;
			if (Root->TryGetObjectField(TEXT("regle"), Regle) && Regle && Regle->IsValid())
			{
				double V = 0.0;
				if ((*Regle)->TryGetNumberField(TEXT("BANDE_MAX_M"), V)) { BandeMaxM = (float)V; }
				if ((*Regle)->TryGetNumberField(TEXT("COLLIER_M"), V)) { CollierM = (float)V; }
			}
			Md5Fichier = Calcule;
			Fichier = Chemin;

			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Root->TryGetArrayField(TEXT("bandes"), Arr))
			{
				Bandes.Reserve(Arr->Num());
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					const TSharedPtr<FJsonObject>& O = V->AsObject();
					if (!O.IsValid()) { continue; }
					FBande B;
					FString Prop;
					O->TryGetStringField(TEXT("proprietaire"), Prop);
					B.Proprio = ProprioDe(Prop);
					O->TryGetNumberField(TEXT("aire_m2"), B.AireM2);
					const TArray<TSharedPtr<FJsonValue>>* Cel = nullptr;
					if (O->TryGetArrayField(TEXT("cellule"), Cel) && Cel->Num() >= 2)
					{
						B.Cellule = FIntPoint((int32)(*Cel)[0]->AsNumber(), (int32)(*Cel)[1]->AsNumber());
					}
					const TArray<TSharedPtr<FJsonValue>>* Anneaux = nullptr;
					if (O->TryGetArrayField(TEXT("anneaux"), Anneaux) && Anneaux->Num() > 0)
					{
						const TSharedPtr<FJsonObject>& A0 = (*Anneaux)[0]->AsObject();
						const TArray<TSharedPtr<FJsonValue>>* Ext = nullptr;
						if (A0.IsValid() && A0->TryGetArrayField(TEXT("ext"), Ext))
						{
							LirePoly(*Ext, B.Ext);
						}
					}
					const TArray<TSharedPtr<FJsonValue>>* Lignes = nullptr;
					if (O->TryGetArrayField(TEXT("ligne_porteuse"), Lignes))
					{
						for (const TSharedPtr<FJsonValue>& LV : *Lignes)
						{
							const TArray<TSharedPtr<FJsonValue>>* L = nullptr;
							if (!LV->TryGetArray(L)) { continue; }
							TArray<FVector2D> Pts;
							LirePoly(*L, Pts);
							if (Pts.Num() >= 2) { B.Lignes.Add(MoveTemp(Pts)); }
						}
					}
					Bandes.Add(MoveTemp(B));
				}
			}
			if (Root->TryGetArrayField(TEXT("frontieres"), Arr))
			{
				Frontieres.Reserve(Arr->Num());
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					const TSharedPtr<FJsonObject>& O = V->AsObject();
					if (!O.IsValid()) { continue; }
					FFrontiere F;
					O->TryGetBoolField(TEXT("couture_engagee"), F.bEngagee);
					O->TryGetNumberField(TEXT("longueur_m"), F.LongueurM);
					const TArray<TSharedPtr<FJsonValue>>* Cel = nullptr;
					if (O->TryGetArrayField(TEXT("cellule"), Cel) && Cel->Num() >= 2)
					{
						F.Cellule = FIntPoint((int32)(*Cel)[0]->AsNumber(), (int32)(*Cel)[1]->AsNumber());
					}
					const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
					if (O->TryGetArrayField(TEXT("polyligne"), P))
					{
						LirePoly(*P, F.Poly);
					}
					if (F.Poly.Num() >= 2) { Frontieres.Add(MoveTemp(F)); }
				}
			}
			bActive = (Bandes.Num() > 0 || Frontieres.Num() > 0);
			return bActive;
		}
	};

	FCityPartition& PartitionSingleton()
	{
		static FCityPartition Partition;
		return Partition;
	}

	// Contexte de drapage resolu en debut d'import : sampler charge une fois +
	// altitude de rebase (Capitole -> z=0). Sampler nul = profil plat (mobile),
	// GroundZ rend alors exactement 0 (golden path bit-a-bit).
	struct FDrapeContext
	{
		const FTerrainSampler* Sampler = nullptr;
		float AltCapCm = 0.f;
		// CHANTIER FRONTIERE : le plafond du lit. Nul = comportement historique,
		// bit pour bit (c'est le rollback `bWaterBedCrush = false`).
		const FCityBedCeiling* Bed = nullptr;

		bool IsActive() const { return Sampler != nullptr; }

		float GroundZ(double Xcm, double Ycm) const
		{
			const float Z = Sampler ? Sampler->AltCmAt(Xcm, Ycm) - AltCapCm : 0.f;
			if (!Bed)
			{
				return Z;
			}
			// ⭐ CHANTIER PROFIL : sur la bande de berge, le sol n'est plus du
			// terrain — c'est un OUVRAGE A NIVEAUX. Le noeud est donc FORCE
			// (plateforme ou esplanade), pas borne. Hors de la bande, rien ne
			// change : le plafond du lit reprend la main, bit pour bit.
			const float Profil = Bed->ProfilAt(Xcm, Ycm);
			if (Profil > TNumericLimits<float>::Lowest())
			{
				return Profil;
			}
			return FMath::Min(Z, Bed->At(Xcm, Ycm));
		}
	};

	// Charge le MNT si le profil le demande. Rend false (apres RaiseError) si le
	// MNT est introuvable — un import desktop sans relief serait un faux resultat.
	bool MakeDrapeContext(const FCityGenProfile& Profile, FDrapeContext& Out)
	{
		if (!Profile.bDrapeToTerrain)
		{
			return true;
		}
		const FString Png = Profile.TerrainPngPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/toulouse10_mnt.png"))
			: Profile.TerrainPngPath;
		const FString Jsn = Profile.TerrainJsonPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/toulouse10_mnt.json"))
			: Profile.TerrainJsonPath;
		FTerrainSampler* Sampler = GetTerrainSampler(Png, Jsn);
		if (!Sampler)
		{
			RaiseError(FString::Printf(TEXT("Cannot load MNT ('%s' + '%s')."), *Png, *Jsn));
			return false;
		}
		Out.Sampler = Sampler;
		Out.AltCapCm = Sampler->AltCapitoleCm();
		// CHANTIER FRONTIERE : le plafond du lit, cuit aux noeuds de la dalle.
		// Recharge a CHAQUE import (le side-car peut avoir ete recuit entre deux
		// passes — c'est la boucle d'iteration du Playbook S11).
		FCityBedCeiling& Bed = BedCeilingSingleton();
		Bed.Reset();
		if (Profile.bWaterBedCrush)
		{
			const FString Dir = Profile.FrontierePath.IsEmpty()
				? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Frontiere"))
				: Profile.FrontierePath;
			// Le PAS des noeuds vient du side-car, pas d'une constante moteur : il
			// est le produit de `cellSizeM` et `grilleN` de la CUISSON. S'ils ne
			// coincident pas avec le profil, on refuse — sans etre fatal (Playbook
			// S10 : un refus se journalise en Display, il ne casse pas la passe).
			FString Text;
			TSharedPtr<FJsonObject> Idx;
			double CellM = 0.0, GrilleN = 0.0;
			if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, TEXT("index.json")))
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Idx)
				&& Idx.IsValid()
				&& Idx->TryGetNumberField(TEXT("cellSizeM"), CellM)
				&& Idx->TryGetNumberField(TEXT("grilleN"), GrilleN)
				&& CellM > 0.0 && GrilleN > 0.0)
			{
				if ((int32)GrilleN != FMath::Clamp(Profile.GroundGridN, 1, 256))
				{
					UE_LOG(LogCityImport, Display,
						TEXT("Frontiere: grilleN cuite %d != GroundGridN %d — plafond du lit IGNORE."),
						(int32)GrilleN, Profile.GroundGridN);
				}
				else
				{
					Bed.Dir = Dir;
					Bed.GridN = (int32)GrilleN;
					Bed.StepCm = (float)(CellM * 100.0 / GrilleN);
					Bed.AltCapCm = Out.AltCapCm;
					Bed.bActive = true;
					// CHANTIER PROFIL : le profil de berge voyage dans le MEME
					// side-car et la MEME grille. Side-car anterieur (sans les
					// deux champs) = comportement historique, bit pour bit.
					Bed.bProfil = Profile.bQuayPlatform;
					Bed.PlatHeightCm = Profile.QuayPlatformHeightM * 100.f;
					Out.Bed = &Bed;
				}
			}
			else
			{
				UE_LOG(LogCityImport, Display,
					TEXT("Frontiere: pas d'index.json lisible dans '%s' — plafond du lit IGNORE "
						 "(pas de donnee, pas d'objet)."), *Dir);
			}
		}
		// ⭐ BUILDQUAY : le constructeur de quai. Recharge a CHAQUE import, comme
		// le plafond. Dossier ou index absents = AUCUNE piece, sans erreur : le
		// generateur se comporte alors exactement comme avant, bit pour bit.
		FCityQuay& Quay = QuaySingleton();
		Quay.Reset();
		{
			const FString QDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Quai"));
			FString QText;
			TSharedPtr<FJsonObject> QIdx;
			double QCellM = 0.0, QGrilleN = 0.0;
			if (FFileHelper::LoadFileToString(QText, *FPaths::Combine(QDir, TEXT("index.json")))
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(QText), QIdx)
				&& QIdx.IsValid()
				&& QIdx->TryGetNumberField(TEXT("cellSizeM"), QCellM)
				&& QIdx->TryGetNumberField(TEXT("grilleN"), QGrilleN)
				&& QCellM > 0.0 && QGrilleN > 0.0)
			{
				if ((int32)QGrilleN != FMath::Clamp(Profile.GroundGridN, 1, 256))
				{
					UE_LOG(LogCityImport, Display,
						TEXT("Quai: grilleN cuite %d != GroundGridN %d — CONSTRUCTEUR IGNORE."),
						(int32)QGrilleN, Profile.GroundGridN);
				}
				else
				{
					Quay.Dir = QDir;
					Quay.GridN = (int32)QGrilleN;
					Quay.AltCapCm = Out.AltCapCm;
					Quay.bActive = true;
					const TArray<TSharedPtr<FJsonValue>>* Ex = nullptr;
					if (QIdx->TryGetArrayField(TEXT("murs_exclus"), Ex))
					{
						for (const TSharedPtr<FJsonValue>& V : *Ex)
						{
							Quay.MursExclus.Add(V->AsString());
						}
					}
					// QUAIV2 : les VOLEES que le constructeur RECONSTRUIT en
					// ENTAILLE entre les deux niveaux. Nominatif, exactement
					// comme `murs_exclus` : la REGLE est nationale et vit dans
					// la cuisson ; le moteur se contente de ne pas draper une
					// seconde fois ce qui est deja bati.
					const TArray<TSharedPtr<FJsonValue>>* Vo = nullptr;
					if (QIdx->TryGetArrayField(TEXT("volees_constructeur"), Vo))
					{
						for (const TSharedPtr<FJsonValue>& V : *Vo)
						{
							Quay.VoleesConstructeur.Add(V->AsString());
						}
					}
					UE_LOG(LogCityImport, Display,
						TEXT("BUILDQUAY : constructeur de quai ACTIF ('%s', grille %d), %d murs exclus nominativement, %d volees RECONSTRUITES par la piece."),
						*QDir, Quay.GridN, Quay.MursExclus.Num(),
						Quay.VoleesConstructeur.Num());
				}
			}
		}
		// ⭐ PARTITION : LA CARTE DE PROPRIETE DU SOL. Rechargee a CHAQUE import,
		// comme le plafond et le quai (la carte peut avoir ete recuite entre deux
		// passes). Les DEUX passes qui touchent au sol la voient donc, et elles
		// lisent LE MEME fichier — une seule verite.
		FCityPartition& Part = PartitionSingleton();
		Part.Reset();
		if (Profile.bPartition)
		{
			const FString PDir = Profile.PartitionPath.IsEmpty()
				? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Partition"))
				: Profile.PartitionPath;
			if (Part.Charger(PDir))
			{
				UE_LOG(LogCityImport, Display,
					TEXT("PARTITION : carte '%s' CHARGEE ('%s') — md5 %s, empreinte geometries %s, "
						 "%d bandes, %d frontieres, cellule %.0f m."),
					*Part.Version, *Part.Fichier, *Part.Md5Fichier, *Part.Md5Geometries,
					Part.Bandes.Num(), Part.Frontieres.Num(), Part.CelluleM);
			}
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// v4 — LE Z DU SOL RENDU (doctrine du Playbook §6 : toute geometrie posee sur
	// le sol echantillonne la surface RENDUE, jamais le terrain abstrait).
	//
	// La dalle n'est PAS le MNT. C'est une grille de quads de Cell / GroundGridN
	// (7,8125 m en desktop : 500 m / 64) dont SEULS LES QUATRE COINS lisent
	// Drape.GroundZ ; entre les coins la surface est PLANE. Sur du relief convexe
	// la dalle passe donc AU-DESSUS du MNT, et une pierre posee a Drape.GroundZ
	// s'enterre — jusqu'a disparaitre entierement (la bordurette d'herbe ne fait
	// que 7 cm de relief, la bordure de chaussee 12).
	//
	// Le n-gon du MeshDescription peut etre triangule par l'une OU l'autre
	// diagonale du quad : on prend l'ENVELOPPE SUPERIEURE des deux triangulations.
	// Elle coincide avec la surface rendue partout sauf sur la moitie de quad ou
	// la diagonale opposee a ete retenue, ou elle passe AU PLUS de quelques
	// centimetres au-dessus — et c'est le bon cote de l'erreur : le pied de la
	// bordure est enterre de 10 cm et absorbe un exces, alors qu'un defaut fait
	// disparaitre la pierre.
	struct FRenderedGroundZ
	{
		const FDrapeContext* Drape = nullptr;
		float StepCm = 0.f;   // pas de la grille de dalle ; 0 = pas de discretisation
		// ⭐ SOL DE BERGE : la taille de cellule, pour interroger la bande. 0 =
		// aucune bande consultee (comportement historique, bit pour bit).
		float CelluleCm = 0.f;

		void Init(const FDrapeContext& InDrape, int32 GroundGridN, float CellCm)
		{
			Drape = &InDrape;
			const int32 GridN = FMath::Clamp(GroundGridN, 1, 256);
			StepCm = (CellCm > 0.f) ? CellCm / (float)GridN : 0.f;
			// La bande n'est consultee que si sa cuisson est accordee a CETTE
			// grille — sinon on ne sait pas de quoi on parle et on se tait.
			const FCityQuay& Q = QuaySingleton();
			CelluleCm = (Q.bActive && Q.GridN == GridN) ? CellCm : 0.f;
		}

		bool IsDiscretized() const
		{
			return Drape && Drape->IsActive() && StepCm > 0.f;
		}

		/** Z du MNT continu — ce que lisait le code d'avant la v4. */
		float RawZ(double Xcm, double Ycm) const
		{
			return Drape ? Drape->GroundZ(Xcm, Ycm) : 0.f;
		}

		float At(double Xcm, double Ycm) const
		{
			// ⭐ SOL DE BERGE — LES LECTEURS DU SOL REPONDENT SUR LA VRAIE
			// SURFACE. Dans la bande, la surface rendue n'est plus la grille :
			// c'est la triangulation contrainte. Escaliers, gradins, vegetation
			// et murs hors bande en dependent — s'ils continuaient a lire la
			// grille, ils se poseraient sur un terrain qui n'existe plus.
			// HORS bande, `SurfaceZ` rend -inf et rien ne change, bit pour bit.
			if (CelluleCm > 0.f)
			{
				const float ZB = QuaySingleton().SurfaceZ(Xcm, Ycm, CelluleCm);
				if (ZB > TNumericLimits<float>::Lowest())
				{
					return ZB;
				}
			}
			if (!IsDiscretized())
			{
				return RawZ(Xcm, Ycm);
			}
			const double S = (double)StepCm;
			const double X0 = FMath::FloorToDouble(Xcm / S) * S;
			const double Y0 = FMath::FloorToDouble(Ycm / S) * S;
			const float u = (float)FMath::Clamp((Xcm - X0) / S, 0.0, 1.0);
			const float v = (float)FMath::Clamp((Ycm - Y0) / S, 0.0, 1.0);
			return RenderedQuadZ(Drape->GroundZ(X0, Y0), Drape->GroundZ(X0 + S, Y0),
				Drape->GroundZ(X0 + S, Y0 + S), Drape->GroundZ(X0, Y0 + S), u, v);
		}
	};

	// Ecart maximal, sur un segment, entre la corde 3D (Z lineaire de A a B) et la
	// surface rendue — plus l'abscisse ou il se produit. Sert au decoupage adaptatif.
	float MaxChordSagCm(const FVector2D& A, const FVector2D& B,
		const FRenderedGroundZ& RGZ, int32 Samples, float& OutBestT)
	{
		const float ZA = RGZ.At(A.X, A.Y);
		const float ZB = RGZ.At(B.X, B.Y);
		float Best = 0.f;
		OutBestT = 0.5f;
		for (int32 i = 1; i < Samples; ++i)
		{
			const float T = (float)i / (float)Samples;
			const FVector2D P = FMath::Lerp(A, B, (double)T);
			const float Sag = RGZ.At(P.X, P.Y) - FMath::Lerp(ZA, ZB, T);
			if (Sag > Best)
			{
				Best = Sag;
				OutBestT = T;
			}
		}
		return Best;
	}

	// Decoupage ADAPTATIF d'une polyligne pour qu'elle epouse la dalle rendue : on
	// n'ajoute un sommet que la ou la corde s'enfonce de plus de ToleranceCm sous la
	// surface. En ville plate (l'immense majorite du lineaire) la polyligne ressort
	// INCHANGEE — le cout en quads ne se paie que sur le relief.
	void SubdivideOnRenderedGround(const TArray<FVector2D>& In, const FRenderedGroundZ& RGZ,
		float ToleranceCm, int32 MaxDepth, TArray<FVector2D>& Out)
	{
		Out.Reset();
		if (In.Num() < 2)
		{
			Out = In;
			return;
		}
		if (!RGZ.IsDiscretized() || ToleranceCm <= 0.f)
		{
			Out = In;
			return;
		}
		TFunction<void(const FVector2D&, const FVector2D&, int32)> Rec =
			[&](const FVector2D& A, const FVector2D& B, int32 Depth)
		{
			float T = 0.5f;
			const float Len = (float)(B - A).Size();
			// Un echantillonnage plus fin que le pas de grille ne sert a rien.
			const int32 Samples = FMath::Clamp(FMath::CeilToInt32(Len / (RGZ.StepCm * 0.5f)), 2, 32);
			if (Depth <= 0 || Len < 50.f || MaxChordSagCm(A, B, RGZ, Samples, T) <= ToleranceCm)
			{
				Out.Add(B);
				return;
			}
			const FVector2D M = FMath::Lerp(A, B, (double)T);
			Rec(A, M, Depth - 1);
			Rec(M, B, Depth - 1);
		};
		Out.Add(In[0]);
		for (int32 i = 0; i + 1 < In.Num(); ++i)
		{
			Rec(In[i], In[i + 1], MaxDepth);
		}
	}

	// Re-echantillonne une polyligne a pas fixe : chaque segment plus long que
	// StepCm est subdivise, sommets d'origine conserves — sans cela les longs
	// segments droits OSM traversent les bosses du MNT (spec J2 §3.2).
	TArray<FVector2D> ResamplePolyline(const TArray<FVector2D>& Pts, float StepCm)
	{
		TArray<FVector2D> Out;
		Out.Reserve(Pts.Num());
		for (int32 i = 0; i < Pts.Num(); ++i)
		{
			if (i > 0)
			{
				const FVector2D& A = Pts[i - 1];
				const FVector2D& B = Pts[i];
				const int32 Sub = FMath::CeilToInt32((float)(B - A).Size() / StepCm);
				for (int32 s = 1; s < Sub; ++s)
				{
					Out.Add(FMath::Lerp(A, B, (double)s / (double)Sub));
				}
			}
			Out.Add(Pts[i]);
		}
		return Out;
	}

	// Z terrain par sommet d'une polyligne : drape sur le MNT, ou — pont — Z
	// interpole lineairement entre les deux culees par abscisse curviligne (le MNT
	// est un sol nu : une route drapee plongerait dans la Garonne, spec J2 Q5).
	void ComputePolylineZ(const TArray<FVector2D>& Pts, const FDrapeContext& Drape,
		bool bBridge, TArray<float>& OutZ)
	{
		OutZ.SetNum(Pts.Num());
		if (!bBridge)
		{
			for (int32 i = 0; i < Pts.Num(); ++i)
			{
				OutZ[i] = Drape.GroundZ(Pts[i].X, Pts[i].Y);
			}
			return;
		}
		const float Z0 = Drape.GroundZ(Pts[0].X, Pts[0].Y);
		const float Z1 = Drape.GroundZ(Pts.Last().X, Pts.Last().Y);
		float Total = 0.f;
		for (int32 i = 0; i + 1 < Pts.Num(); ++i)
		{
			Total += (float)(Pts[i + 1] - Pts[i]).Size();
		}
		float Arc = 0.f;
		for (int32 i = 0; i < Pts.Num(); ++i)
		{
			if (i > 0)
			{
				Arc += (float)(Pts[i] - Pts[i - 1]).Size();
			}
			OutZ[i] = Total > 1.f ? FMath::Lerp(Z0, Z1, Arc / Total) : Z0;
		}
	}

	// Copie assumee du builder de BuildingTools.cpp, version couleur RGB + triangles.
	// A consolider avec BuildingTools une fois le pipeline ville valide.
	struct FCityMeshBuilder
	{
		FMeshDescription MeshDesc;
		FStaticMeshAttributes Attributes;
		FPolygonGroupID WallGroup;
		FPolygonGroupID GlassGroup;
		int32 QuadCount = 0;

		// J3c point 2 : groupes de polygones SUPPLEMENTAIRES nommes (un par classe de
		// revetement). Wall = slot 0, Glass = slot 1, puis les extras dans leur ordre
		// de creation — CreateMeshAsset ajoute un FStaticMaterial par extra, de meme
		// nom de slot, ce qui donne une section de mesh par classe. Un builder sans
		// extra produit exactement les deux slots historiques (golden path mobile).
		TArray<FName> ExtraSlotNames;
		TArray<UMaterialInterface*> ExtraSlotMaterials;
		TMap<FName, FPolygonGroupID> ExtraGroups;

		// Groupe d'une classe de revetement, cree a la PREMIERE utilisation (jamais de
		// slot vide). MatOrNull = materiau du pack ; nul -> CreateMeshAsset retombe sur
		// le materiau de repli du mesh.
		FPolygonGroupID GetOrCreateGroup(FName SlotName, UMaterialInterface* MatOrNull)
		{
			if (const FPolygonGroupID* Found = ExtraGroups.Find(SlotName))
			{
				return *Found;
			}
			const FPolygonGroupID Group = MeshDesc.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[Group] = SlotName;
			ExtraGroups.Add(SlotName, Group);
			ExtraSlotNames.Add(SlotName);
			ExtraSlotMaterials.Add(MatOrNull);
			return Group;
		}

		// Lot B (desktop PBR) : vertex colors encodees LINEAIRE (le pow 2.2 est un
		// hack de lecture brute en unlit — faux pour un materiau lit, spec Q10).
		bool bLinearColors = false;

		// Lot B (desktop) : UV1 = (x,y) monde normalise sur la dalle 10 km
		// (coin NW a -5000 m -> [0,1]), prete pour l'ortho BD ORTHO (J3).
		bool bWorldUV1 = false;

		FCityMeshBuilder()
			: Attributes(MeshDesc)
		{
			Attributes.Register();
			WallGroup = MeshDesc.CreatePolygonGroup();
			GlassGroup = MeshDesc.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[WallGroup] = FName(TEXT("Wall"));
			Attributes.GetPolygonGroupMaterialSlotNames()[GlassGroup] = FName(TEXT("Glass"));
		}

		void EnableWorldUV1()
		{
			if (!bWorldUV1)
			{
				bWorldUV1 = true;
				MeshDesc.VertexInstanceAttributes().SetAttributeChannelCount(
					MeshAttribute::VertexInstance::TextureCoordinate, 2);
			}
		}

		static FVector2f WorldUV(const FVector3f& P)
		{
			return FVector2f((P.X + 500000.f) / 1000000.f, (P.Y + 500000.f) / 1000000.f);
		}

		// Compensation gamma : le build encode en sRGB, le shader lit brut (cf. BuildingTools).
		// bLinearColors (desktop PBR) : encodage lineaire, le shader lit VertexColor en lineaire.
		FVector4f Encode(const FVector3f& C) const
		{
			if (bLinearColors)
			{
				return FVector4f(FMath::Clamp(C.X, 0.f, 1.f), FMath::Clamp(C.Y, 0.f, 1.f),
					FMath::Clamp(C.Z, 0.f, 1.f), 1.f);
			}
			return FVector4f(FMath::Pow(FMath::Clamp(C.X, 0.f, 1.f), 2.2f),
				FMath::Pow(FMath::Clamp(C.Y, 0.f, 1.f), 2.2f),
				FMath::Pow(FMath::Clamp(C.Z, 0.f, 1.f), 2.2f), 1.f);
		}

		// Variante avec UNE couleur PAR SOMMET (grille de sol peinte).
		void AddPolyPerVertexColors(FPolygonGroupID Group, const FVector3f* Corners, int32 Num,
			const FVector3f& Outward, const FVector2f* UVs, const FVector3f* VertexColors)
		{
			const FVector3f RenderedNormal =
				FVector3f::CrossProduct(Corners[2] - Corners[0], Corners[1] - Corners[0]).GetSafeNormal();
			const bool bFlip = FVector3f::DotProduct(RenderedNormal, Outward) < 0.f;
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector2f> UV0 = Attributes.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
			TArray<FVertexInstanceID> InstanceIDs;
			InstanceIDs.Reserve(Num);
			for (int32 i = 0; i < Num; ++i)
			{
				const int32 c = bFlip ? (Num - 1 - i) % Num : i;
				const FVertexID VertexID = MeshDesc.CreateVertex();
				Positions[VertexID] = Corners[c];
				const FVertexInstanceID InstanceID = MeshDesc.CreateVertexInstance(VertexID);
				Normals[InstanceID] = Outward;
				UV0.Set(InstanceID, 0, UVs[c]);
				if (bWorldUV1)
				{
					UV0.Set(InstanceID, 1, WorldUV(Corners[c]));
				}
				Colors[InstanceID] = Encode(VertexColors[c]);
				InstanceIDs.Add(InstanceID);
			}
			MeshDesc.CreatePolygon(Group, InstanceIDs);
			++QuadCount;
		}

		void AddPoly(FPolygonGroupID Group, const FVector3f* Corners, int32 Num, const FVector3f& Outward,
			const FVector2f* UVs, const FVector3f& Color)
		{
			const FVector3f RenderedNormal =
				FVector3f::CrossProduct(Corners[2] - Corners[0], Corners[1] - Corners[0]).GetSafeNormal();
			const bool bFlip = FVector3f::DotProduct(RenderedNormal, Outward) < 0.f;

			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector2f> UV0 = Attributes.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();

			const FVector4f Encoded = Encode(Color);
			TArray<FVertexInstanceID> InstanceIDs;
			InstanceIDs.Reserve(Num);
			for (int32 i = 0; i < Num; ++i)
			{
				const int32 c = bFlip ? (Num - 1 - i) % Num : i;
				const FVertexID VertexID = MeshDesc.CreateVertex();
				Positions[VertexID] = Corners[c];
				const FVertexInstanceID InstanceID = MeshDesc.CreateVertexInstance(VertexID);
				Normals[InstanceID] = Outward;
				UV0.Set(InstanceID, 0, UVs[c]);
				if (bWorldUV1)
				{
					UV0.Set(InstanceID, 1, WorldUV(Corners[c]));
				}
				Colors[InstanceID] = Encoded;
				InstanceIDs.Add(InstanceID);
			}
			MeshDesc.CreatePolygon(Group, InstanceIDs);
			++QuadCount;
		}

		void AddQuad(FPolygonGroupID Group, const FVector3f& A, const FVector3f& B, const FVector3f& C,
			const FVector3f& D, const FVector3f& Outward, const FVector3f& Color, float UVScale = 0.0025f)
		{
			if ((A - B).IsNearlyZero(0.01f) || (B - C).IsNearlyZero(0.01f) || (C - D).IsNearlyZero(0.01f))
			{
				return;
			}
			const FVector3f P[4] = { A, B, C, D };
			FVector2f UV[4];
			for (int32 i = 0; i < 4; ++i)
			{
				// Projection planaire selon l'axe dominant de la normale.
				UV[i] = FMath::Abs(Outward.Z) > 0.5f
					? FVector2f(P[i].X * UVScale, P[i].Y * UVScale)
					: (FMath::Abs(Outward.X) > FMath::Abs(Outward.Y)
						? FVector2f(P[i].Y * UVScale, P[i].Z * UVScale)
						: FVector2f(P[i].X * UVScale, P[i].Z * UVScale));
			}
			AddPoly(Group, P, 4, Outward, UV, Color);
		}

		void AddTri(FPolygonGroupID Group, const FVector3f& A, const FVector3f& B, const FVector3f& C,
			const FVector3f& Outward, const FVector3f& Color)
		{
			if ((A - B).IsNearlyZero(0.01f) || (B - C).IsNearlyZero(0.01f) || (C - A).IsNearlyZero(0.01f))
			{
				return;
			}
			const FVector3f P[3] = { A, B, C };
			FVector2f UV[3];
			for (int32 i = 0; i < 3; ++i)
			{
				UV[i] = FVector2f(P[i].X * 0.0025f, P[i].Y * 0.0025f);
			}
			AddPoly(Group, P, 3, Outward, UV, Color);
		}
	};

	// Soleil fixe monde pour l'ombrage cuit (memes valeurs que le bench batiment heros).
	const FVector3f GSunDir = FVector3f(0.4f, -0.6f, 0.7f).GetSafeNormal();

	FVector3f Shade(const FVector3f& Base, const FVector3f& N, float Zcm)
	{
		const float L = 0.30f + 0.70f * FMath::Max(0.f, FVector3f::DotProduct(N, GSunDir));
		const float Ground = FMath::Lerp(0.85f, 1.f, FMath::Clamp(Zcm / 200.f, 0.f, 1.f));
		return Base * L * Ground;
	}

	double SignedArea(const TArray<FVector2D>& P)
	{
		double A = 0;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const FVector2D& U = P[i];
			const FVector2D& V = P[(i + 1) % P.Num()];
			A += U.X * V.Y - V.X * U.Y;
		}
		return A * 0.5;
	}

	// Ear clipping simple sur un anneau CCW. Fallback en eventail si degenere.
	void TriangulateRing(const TArray<FVector2D>& P, TArray<int32>& OutTris)
	{
		auto Cross = [](const FVector2D& a, const FVector2D& b, const FVector2D& c)
		{
			return (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
		};
		auto InTri = [&](const FVector2D& p, const FVector2D& a, const FVector2D& b, const FVector2D& c)
		{
			return Cross(a, b, p) >= 0 && Cross(b, c, p) >= 0 && Cross(c, a, p) >= 0;
		};
		TArray<int32> V;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			V.Add(i);
		}
		int32 Guard = P.Num() * P.Num() * 2;
		while (V.Num() > 3 && Guard-- > 0)
		{
			bool bClipped = false;
			for (int32 i = 0; i < V.Num(); ++i)
			{
				const int32 i0 = V[(i + V.Num() - 1) % V.Num()];
				const int32 i1 = V[i];
				const int32 i2 = V[(i + 1) % V.Num()];
				if (Cross(P[i0], P[i1], P[i2]) <= 0)
				{
					continue;
				}
				bool bEar = true;
				for (int32 j = 0; j < V.Num(); ++j)
				{
					const int32 vj = V[j];
					if (vj == i0 || vj == i1 || vj == i2)
					{
						continue;
					}
					if (InTri(P[vj], P[i0], P[i1], P[i2]))
					{
						bEar = false;
						break;
					}
				}
				if (!bEar)
				{
					continue;
				}
				OutTris.Append({ i0, i1, i2 });
				V.RemoveAt(i);
				bClipped = true;
				break;
			}
			if (!bClipped)
			{
				break;
			}
		}
		if (V.Num() == 3)
		{
			OutTris.Append({ V[0], V[1], V[2] });
		}
		else if (V.Num() > 3)
		{
			for (int32 i = 1; i + 1 < V.Num(); ++i)
			{
				OutTris.Append({ V[0], V[i], V[i + 1] });
			}
		}
	}

	FVector3f UsageTint(const FString& Usage, int32 Seed)
	{
		FVector3f Base;
		if (Usage == TEXT("res")) { Base = FVector3f(0.80f, 0.55f, 0.40f); }        // brique toulousaine
		else if (Usage == TEXT("com")) { Base = FVector3f(0.82f, 0.75f, 0.62f); }   // enduit clair
		else if (Usage == TEXT("ind")) { Base = FVector3f(0.60f, 0.60f, 0.62f); }
		else { Base = FVector3f(0.74f, 0.66f, 0.55f); }
		const float Var = 0.90f + 0.20f * FMath::Frac(FMath::Sin(Seed * 12.9898f) * 43758.5453f);
		return Base * Var;
	}

	// J3f DA « marbre blanc taille » : teinte de vertex marbre chaud (encodage LINEAIRE
	// desktop, ~albedo clair mais pas sature). Sur la sous-tuile pierre claire (12) du
	// materiau PBR = pierre/marbre uni, sans dessin de facade.
	FVector3f MarbleTint()
	{
		return FVector3f(0.88f, 0.87f, 0.83f);
	}

	// Batiment a emprise polygonale : facades avec fenetres par travee + toit plat.
	void BuildPolygonBuilding(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float Hcm,
		const FVector3f& Tint)
	{
		const int32 Floors = FMath::Clamp(FMath::RoundToInt32(Hcm / 290.f), 1, 40);
		const float FloorH = Hcm / Floors;
		const FVector3f RoofColor = FVector3f(0.42f, 0.40f, 0.38f);

		const int32 N = PtsCm.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = PtsCm[e];
			const FVector2D B2 = PtsCm[(e + 1) % N];
			const FVector2D Dir2 = (B2 - A2);
			const float Len = Dir2.Size();
			if (Len < 30.f)
			{
				continue;
			}
			const FVector2D T2 = Dir2 / Len;
			// Polygone CCW : l'exterieur est a droite du parcours.
			const FVector3f Nout = FVector3f(T2.Y, -T2.X, 0.f);
			const FVector3f A = FVector3f(A2.X, A2.Y, 0.f);
			const FVector3f T = FVector3f(T2.X, T2.Y, 0.f);
			auto WallPoint = [&](float U, float Z) { return A + T * U + FVector3f(0, 0, Z); };
			auto Wall = [&](float U0, float U1, float Z0, float Z1)
			{
				if (U1 - U0 < 1.f || Z1 - Z0 < 1.f)
				{
					return;
				}
				QM.AddQuad(QM.WallGroup, WallPoint(U0, Z0), WallPoint(U1, Z0), WallPoint(U1, Z1),
					WallPoint(U0, Z1), Nout, Shade(Tint, Nout, (Z0 + Z1) * 0.5f));
			};

			const float Margin = 40.f;
			const float Usable = Len - 2.f * Margin;
			const int32 Bays = Usable > 200.f ? FMath::Max(1, FMath::RoundToInt32(Usable / 280.f)) : 0;

			float Z0 = 0.f;
			for (int32 F = 0; F < Floors; ++F)
			{
				const float Z1 = Z0 + FloorH;
				if (Bays == 0 || FloorH < 220.f)
				{
					Wall(0.f, Len, Z0, Z1);
					Z0 = Z1;
					continue;
				}
				Wall(0.f, Margin, Z0, Z1);
				Wall(Len - Margin, Len, Z0, Z1);
				const float BayW = Usable / Bays;
				for (int32 B = 0; B < Bays; ++B)
				{
					const float BU0 = Margin + B * BayW;
					const float BU1 = BU0 + BayW;
					const float WinW = BayW * 0.5f;
					const float WinH = FloorH * (F == 0 ? 0.60f : 0.45f);
					const float WU0 = (BU0 + BU1 - WinW) * 0.5f;
					const float WU1 = WU0 + WinW;
					const float WZ0 = F == 0 ? Z0 + 25.f : Z0 + (FloorH - WinH) * 0.5f;
					const float WZ1 = WZ0 + WinH;
					Wall(BU0, WU0, Z0, Z1);
					Wall(WU1, BU1, Z0, Z1);
					Wall(WU0, WU1, Z0, WZ0);
					Wall(WU0, WU1, WZ1, Z1);
					// Fenetre en creux simplifiee : vitre en retrait, sans tableaux (cout).
					const FVector3f In = -Nout * 15.f;
					const FVector3f G0 = WallPoint(WU0, WZ0) + In, G1 = WallPoint(WU1, WZ0) + In;
					const FVector3f G2 = WallPoint(WU1, WZ1) + In, G3 = WallPoint(WU0, WZ1) + In;
					const FVector3f P[4] = { G0, G1, G2, G3 };
					const FVector2f UV[4] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
					QM.AddPoly(QM.GlassGroup, P, 4, Nout, UV,
						Shade(FVector3f(0.35f, 0.35f, 0.35f), Nout, (WZ0 + WZ1) * 0.5f));
				}
				Z0 = Z1;
			}
		}

		// Toit plat par triangulation de l'emprise.
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		const FVector3f RoofShaded = Shade(RoofColor, FVector3f(0, 0, 1), Hcm);
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			QM.AddTri(QM.WallGroup,
				FVector3f(PtsCm[Tris[t]].X, PtsCm[Tris[t]].Y, Hcm),
				FVector3f(PtsCm[Tris[t + 1]].X, PtsCm[Tris[t + 1]].Y, Hcm),
				FVector3f(PtsCm[Tris[t + 2]].X, PtsCm[Tris[t + 2]].Y, Hcm),
				FVector3f(0, 0, 1), RoofShaded);
		}
	}

	// -----------------------------------------------------------------------------
	// J3c point 2 « builder sols » : classes de revetement Megascans.
	//
	// Une classe = un pack scanne importe sous <SurfacesFolder>/<Slug>/M_Surf_<Slug>
	// (Tools/import_surfaces.py). Le materiau divise l'UV0 par la taille PHYSIQUE du
	// scan (lue au JSON du pack) : le generateur ecrit donc des UV0 EN METRES, et un
	// meme UV metrique donne la bonne echelle reelle quel que soit le pack.
	//   AcrossM    taille physique du scan EN TRAVERS de la route (m).
	//   bFullWidth la largeur ENTIERE du ruban est mappee sur AcrossM — reserve aux
	//              scans qui portent deja leur ligne axiale (fine_road_*, marked_*) :
	//              la ligne tombe alors exactement au milieu du ruban. Sinon la
	//              texture tuile aussi en travers (UV0.V = metres reels).
	//   bSwapUV    le scan est tourne de 90 deg (son axe « le long de la route » est
	//              V et non U) — verifie image par image, cf. fine_road_viciaalew.
	//   ZClassCm   v2 : offset d'empilement DETERMINISTE PAR CLASSE au-dessus du
	//              plancher des rubans (55 cm). L'ancien offset par ordre d'arrivee
	//              (RoadIndex % 7) faisait passer un trottoir SOUS une chaussee ici
	//              et AU-DESSUS la ou elle la recroisait : frontieres instables,
	//              « effet bacle » (verdict utilisateur v1). Ordre impose : gravier
	//              le plus bas, pieton le plus haut.
	//   bAuto      v3 : la classe est une CHAUSSEE AUTOMOBILE. Sert au filtre des
	//              patchs de carrefour (voir FJunctionNode) : un carrefour n'existe
	//              qu'entre voitures.
	// -----------------------------------------------------------------------------
	struct FSurfaceClass
	{
		const TCHAR* Slug;
		float AcrossM;
		bool bFullWidth;
		bool bSwapUV;
		float ZClassCm;
		bool bAuto;
	};

	const FSurfaceClass GSurfGravel{ TEXT("gravel_on_soil_okosdmp0"), 0.89f, false, false, 0.f, false };
	const FSurfaceClass GSurfGrassCut{ TEXT("grass_cut_pjxmz0"), 1.f, false, false, 2.f, false };
	const FSurfaceClass GSurfAsphalt{ TEXT("asphalt_road_tiggcjdo"), 2.f, false, false, 4.f, true };
	const FSurfaceClass GSurfRoadMedium{ TEXT("fine_road_viciaalew"), 4.f, true, true, 10.f, true };
	const FSurfaceClass GSurfRoadWide{ TEXT("fine_road_vgdlejpew"), 8.f, true, false, 13.f, true };
	const FSurfaceClass GSurfGrassUncut{ TEXT("uncut_grass_oilpt20"), 2.f, false, false, 2.f, false };
	const FSurfaceClass GSurfGrassWild{ TEXT("wild_grass_sfknaeoa"), 2.f, false, false, 2.f, false };
	// v4 — LA DALLE. Ce n'est pas un ruban : c'est le sol porteur de toute la ville,
	// pose au Z du terrain sous tout le reste. Verdict DA v3 : « grand puzzle » — la
	// dalle etait restee a la teinte unie blanc-bleu de J2, si bien que chaque ruban
	// texture ressemblait a un autocollant sur du papier et que les interstices entre
	// rubans laissaient voir le vide. Aucun reglage de palette ne rattrape un fond nu.
	// dirty_sidewalk_tiles : le scan mineral le plus NEUTRE et le plus CLAIR de la
	// bibliotheque une fois harmonise (0,0991) — il ne raconte rien, c'est ce qu'on
	// demande a un fond. ZClassCm ne sert pas (la dalle n'entre pas dans l'empilement
	// des rubans).
	const FSurfaceClass GSurfSlab{ TEXT("dirty_sidewalk_tiles_ugxjcdpn"), 2.f, false, false, 0.f, false };

	// -----------------------------------------------------------------------------
	// v5 « VOIRIE » (J3c point 3). Verdict utilisateur sur la v4b : « il manque la
	// structure des rues (rives) » — la ville lisait comme une esplanade continue
	// ou personne ne sait ou finit la chaussee. Le ruban de chaussee, qui couvrait
	// historiquement chaussee + 2 x 1,70 m d'un seul tenant, est RE-PARTITIONNE :
	//   bande centrale = classe chaussee ;
	//   bordure       = face verticale de 12 cm + chant horizontal de 15 cm ;
	//   bandes rives  = 1,70 m de classe DALLE (meme scan que le fond de ville :
	//                   le trottoir n'est PAS un revetement de plus, c'est le sol
	//                   de la ville que la bordure vient decoller de 12 cm).
	// C'est le relief de 12 cm qui donne la lecture de la rue : deux aretes eclairees
	// differemment de part et d'autre de la chaussee.
	// -----------------------------------------------------------------------------
	constexpr float GSidewalkWidthCm = 170.f; // rive, largeur historique du « trottoir »
	constexpr float GCurbHeightCm = 12.f;     // relief de la bordure
	constexpr float GCurbTopWidthCm = 15.f;   // chant horizontal, entre face et rive
	// Pas de sous-decoupe des quads lateraux, sauf au voisinage d'un patch de
	// carrefour ou la bordure doit s'interrompre au plus pres du disque.
	constexpr float GCurbClipStepCm = 200.f;

	// La BORDURE : meme matiere que la dalle, assombrie x0,92 (materiau derive
	// M_Surf_curb fabrique par Tools/import_surfaces.py a partir des textures du
	// pack de dalle). Un materiau dedie plutot qu'une teinte de sommet : les
	// M_Surf_* ne lisent PAS la VertexColor (BaseColor = scan x constante).
	const FSurfaceClass GSurfCurb{ TEXT("curb"), 2.f, false, false, 0.f, false };
	// LOT3 — LA TERRE DES FOSSES DE PLANTATION. Pack DEDIE (scan dirt_ground 2 x 2 m
	// assombri, Tools/import_surfaces via work/SOLVERT/lot3_import_soil.py) et non le
	// gravier : le gravier beige clair sur du pave gris etait precisement le « sticker
	// de sable » refuse par l'utilisateur. Son materiau est le SEUL du set sols a lire
	// la VertexColor — c'est par elle que la terre s'assombrit au pied du tronc.
	// Absent -> repli sur le gravier (l'ancien comportement, jamais un materiau par
	// defaut gris vif).
	const FSurfaceClass GSurfPitSoil{ TEXT("treepit_soil"), 2.f, false, false, 0.f, false };
	// L'ENTOURAGE des fosses : pack DERIVE de la dalle, assombri de moitie
	// (Tools/import_surfaces via work/SOLVERT/lot3_import_frame.py). Un entourage doit
	// se VOIR : la bordure de trottoir (dalle x0,92) ne dessinait rien a plat sur le
	// meme pave — constat par capture. Absent -> repli sur la bordure, puis la terre.
	const FSurfaceClass GSurfPitFrame{ TEXT("treepit_frame"), 2.f, false, false, 0.f, false };
	// PASSAGE PIETON. Le scan fait 4 x 2 m : son axe de 4 m porte la REPETITION des
	// bandes (8 bandes, pas de 50 cm) et son axe de 2 m leur LONGUEUR, avec un trait
	// blanc en travers a mi-hauteur. L'axe de 4 m part donc EN TRAVERS de la rue
	// (bandes de 50 cm paralleles a l'axe de la chaussee, norme francaise) et l'axe
	// de 2 m le long de la rue, cale pour que le trait blanc tombe exactement sur les
	// deux bords du passage. AcrossM/bFullWidth/ZClassCm ne servent pas : les UV du
	// passage sont calculees a la main dans BuildCrossing.
	const FSurfaceClass GSurfCrossing{ TEXT("pedestrian_crossing_lines_veggecd"), 4.f, false, false, 0.f, false };
	// LOT EAU — L'EAU. Ce n'est PAS une classe de sol : c'est une surface a part,
	// dont le materiau ne vient pas du dossier des revetements mais du profil
	// (`WaterMaterialPath`), parce que ce n'est pas un scan mais le shading model
	// SINGLE LAYER WATER du moteur. Le slug ne sert qu'a nommer le slot du mesh.
	const FSurfaceClass GSurfWater{ TEXT("water"), 2.f, false, false, 0.f, false };
	constexpr float GCrossingHalfLenCm = 200.f; // 4 m dans l'axe de la rue
	constexpr float GCrossingLiftCm = 9.f;      // au-dessus de la chaussee et du patch, sous le chant (12)

	// -----------------------------------------------------------------------------
	// J3c « MAQUETTE DU SOL » — LE SOL EST PEINT, LE RELIEF EST MAILLE.
	//
	// Le corridor cadastral etant desormais cuit en masques par cellule
	// (Tools/j3c_sols_masks.py), la chaussee n'est plus un ruban pose SUR la dalle :
	// elle EST la dalle, peinte par MI_CityGround_<x>_<y>. Ne reste en geometrie que
	// ce qu'un masque ne peut pas rendre :
	//   - la BORDURE, seule chose qui donne du relief a la rue (c'est elle qui prend
	//     la lumiere autrement que le sol) ;
	//   - le PASSAGE PIETON et les TIRETS axiaux, dont on veut le trait franc a
	//     n'importe quelle distance ;
	//   - les PONTS, qui restent les rubans/tabliers existants (un tablier ne se
	//     peint pas sur le terrain qu'il survole).
	// -----------------------------------------------------------------------------
	// Le materiau de la dalle masquee. Pas de scan : le master melange lui-meme les
	// quatre revetements d'apres le masque. AcrossM/bFullWidth/ZClassCm ne servent
	// pas — l'UV0 metrique monde de la dalle est deja exactement ce qu'il attend.
	const FSurfaceClass GSurfMaskedGround{ TEXT("ground_masked"), 1.f, false, false, 0.f, false };
	// La peinture blanche des tirets (Tools/import_ground_masks.py).
	const FSurfaceClass GSurfMarking{ TEXT("marking"), 1.f, false, false, 0.f, false };

	constexpr float GAxialWidthCm = 15.f;      // largeur d'un tiret de ligne axiale
	// Pied de bordure ENTERRE : la dalle est drapee sur le MNT par une grille de
	// 7,8 m de pas, la bordure suit le MNT continu — entre deux sommets, les deux
	// surfaces divergent de quelques centimetres. Enterrer le pied coute zero
	// triangle et supprime tout risque de jour sous la bordure.
	constexpr float GMaskCurbSinkCm = 10.f;
	// LOT FINITION_SOL V3 — LA BORDURETTE D'HERBE.
	// DOCTRINE : n'importe quelle forme BORDEE lit « amenagement voulu ». Une pelouse
	// qui meurt sur la dalle sans rien lit « releve d'occupation du sol » ; la meme
	// pelouse ceinturee d'une pierre lit « pelouse ». C'est ce qui remplace la course
	// a la peinture parfaite — on arrete de chercher le contour juste, on POSE la
	// pierre sur le contour qu'on a.
	// Le decoupage est fait au bake (Tools/j3c_sols_masks.py, liste `grassEdges` :
	// memes conventions que `curbs`, MINERAL A GAUCHE, hors facades, hors bordures de
	// chaussee deja posees, hors eau, hors segments < 1,5 m). Ici on ne fait que
	// poser des quads, avec la MEME mecanique que BuildMaskCurb et le MEME materiau
	// de bordure (GSurfCurb) — aucun materiau nouveau.
	// PROFIL REDUIT : une bordurette de pelouse n'est pas une bordure de chaussee.
	// 7 cm de relief au lieu de 12 (elle ne doit pas se lire comme une marche de
	// trottoir depuis le ciel) et 14 cm de chant, qui deborde sur la pelouse — c'est
	// exactement une bordurette de jardin posee au ras de la terre.
	constexpr float GGrassCurbHeightCm = 7.f;
	constexpr float GGrassCurbTopWidthCm = 14.f;
	// v4 — decoupage adaptatif des bordures sur la dalle rendue. 2 cm : un tiers du
	// relief de la bordurette (7 cm), donc invisible ; le decoupage ne se declenche
	// que la ou la corde s'enfonce davantage. Profondeur 6 = au pire 64 morceaux par
	// segment d'origine, largement de quoi suivre une grille de 7,8 m.
	constexpr float GCurbSagToleranceCm = 2.f;
	constexpr int32 GCurbSagMaxDepth = 6;
	constexpr float GMaskCrossLiftCm = 4.f;    // passage pieton au-dessus de la peinture
	constexpr float GMaskDashLiftCm = 6.f;     // tiret au-dessus du passage, sous le chant (12)

	// -----------------------------------------------------------------------------
	// C1 « DISCONTINUITES » — LES MURS DE SOUTENEMENT.
	//
	// LE PROBLEME, EN UNE PHRASE. La dalle n'echantillonne le MNT qu'aux COINS de ses
	// quads (7,8125 m en desktop) : une marche verticale reelle de h metres y devient
	// une RAMPE de h metres sur 7,8 m, aux texels etires. C'est le grief « pente
	// bizarre » aux quais de la Garonne et aux berges du Canal du Midi.
	//
	// CE QU'ON POSE. Le long des breaklines cuites (SourceData/Murs/murs_<x>_<y>.json,
	// detectees sur LE MEME MNT que le drape, cf. work/DISCONT/murs_bake.py) :
	//   1. une FACE VERTICALE au PIED de la rampe ;
	//   2. un COURONNEMENT horizontal, a l'altitude du haut, qui recouvre la rampe
	//      jusqu'a l'endroit ou la surface rendue retrouve cette altitude ;
	//   3. un DOS court, pour que la piece ne soit pas un plan sans envers ;
	//   4. deux BOUCHONS par polyligne (meme raison que pour les bordures).
	//
	// POURQUOI LE COURONNEMENT VA JUSQU'AU HAUT DE LA RAMPE, et pas 40 cm comme une
	// vraie pierre de couronnement : un couronnement etroit FLOTTERAIT au-dessus de
	// la rampe (la surface rendue, elle, continue de descendre derriere lui) et on
	// verrait la rampe etiree entre la pierre et le sol. En le poussant jusqu'au
	// point ou la surface rendue rejoint l'altitude du haut, la piece se REFERME sur
	// le sol : aucun jour, aucune rampe visible. Physiquement, ca lit comme le
	// couronnement + la promenade d'un quai, le chemin de halage d'un canal, la berme
	// d'une tranchee — ce qui est exactement ce qu'il y a la en realite.
	//
	// TOUT EST MESURE SUR LA SURFACE RENDUE (doctrine du Playbook §6), jamais sur le
	// MNT : c'est la surface rendue qui porte la rampe, c'est elle qu'il faut couvrir.
	// Le side-car ne sert qu'a dire OU et DANS QUEL SENS ; ses z_haut/z_bas servent de
	// GARDE-FOU (un mur mesure trois fois plus haut que ce que la donnee annonce est
	// une erreur de lecture, pas une falaise).
	//
	// v1 ASSUMEE : on masque la rampe, ON NE RE-MAILLE PAS la grille du sol. Si la
	// mesure montre un jour que ca ne suffit pas (vue plongeante rasante, ou la
	// largeur du couronnement se lit comme une esplanade), l'option v2 est le
	// re-maillage local de la dalle le long de la breakline — a decider sur capture,
	// pas a improviser.
	//
	// =========================================================================
	// LOT BERGES (03/08) — v1 bis : LE COURONNEMENT S'ARRETE AU PALIER REEL.
	//
	// LA MESURE QUI L'IMPOSE (work/BERGES/b3_decision.py, 239 murs poses) :
	//   largeur de la MARCHE, lue sur la DONNEE D'ALTITUDE ... p50  3,50 m
	//   largeur de l'EMPRISE posee, lue sur le drape ........ p50 12,00 m
	//   ... et 14,00 m sur la zone des captures utilisateur — soit 3,4 fois la
	//   marche. 2 185 arbres du proto (969 sous des quais) tombent sous cette
	//   emprise, couronnement compris : c'est le grief « les murs prennent trop de
	//   place, ils avalent l'allee et les arbres », et le symptome nomme par
	//   l'utilisateur (« une fosse d'arbre chevauchee par la dalle de crete »).
	//
	// LA CAUSE, EN UNE PHRASE : le sondage marche sur la surface RENDUE, et la
	// surface rendue ETALE la marche sur +-1 quad (7,81 m). Le sondage suit donc
	// l'etalement jusqu'a son bout et le couronnement recouvre TOUT — y compris
	// plusieurs metres de terrasse haute qui, elle, est REELLE et plate.
	//
	// LE CORRECTIF, MINIMAL ET NATIONAL : le cote CRETE est BORNE par le palier de
	// la marche, lu sur la DONNEE D'ALTITUDE (RGZ.RawZ = le MNT), qui connait la
	// marche. Le PIED, lui, NE BOUGE PAS : la mesure dit qu'il est deja a 0,95 m
	// (p50) de la ligne d'eau BD TOPO, et l'axe du mur a 0,25 m (p50 signe) de la
	// ligne `Quai`/`Mur de soutenement` de BD TOPO — il EST sur la donnee, le
	// deplacer serait une regression.
	//
	// CE QUE CA NE CASSE PAS. L'invariant « la piece se referme EXACTEMENT sur le
	// sol » tient : ZCrest reste lu SUR LA SURFACE RENDUE, au point borne. Il n'y a
	// donc ni jour ni couronnement flottant — l'objection de la v1 (« un couronnement
	// de 40 cm flotterait ») visait une largeur FIXE, pas un arret sur palier mesure.
	// Ce que le mur rend au sol est une berme de pente mesuree ~0,3 m/m qui porte
	// les materiaux du sol (UV0 en metres MONDE : etirement 4,5 %), au lieu d'une
	// dalle de pierre posee sur les fosses d'arbres.
	//
	// Rollback SANS rebuild : `FCityGenProfile::bWallCrestOnPlateau = false`.
	// =========================================================================
	// -----------------------------------------------------------------------------
	// Pied ENTERRE, meme raison que la bordure (la dalle rendue et le mur divergent de
	// quelques centimetres entre deux sommets de grille) — mais a l'echelle d'un mur.
	constexpr float GWallSinkCm = 40.f;
	// Marche de recherche du pied et de la crete, le long de la normale. Pas de 50 cm,
	// portee 1,5 quad de dalle : au-dela, ce n'est plus la rampe du drape, c'est le
	// terrain. Tolerance de 8 cm = on considere la surface « de niveau ».
	constexpr float GWallProbeStepCm = 50.f;
	constexpr float GWallProbeSpanQuads = 1.5f;
	constexpr float GWallLevelTolCm = 8.f;
	// Un mur de moins de 1 m de hauteur MESUREE sur la surface rendue n'a rien a
	// masquer : la rampe y est deja invisible. Ecarte et compte.
	constexpr float GWallMinHeightCm = 100.f;
	// Garde-fou : la hauteur mesuree ne peut pas depasser trois fois celle qu'annonce
	// le side-car (une lecture qui part dans le decor plutot qu'une vraie falaise).
	constexpr float GWallHeightGuard = 3.0f;
	// LOT BERGES — le PALIER de la marche, sur la DONNEE D'ALTITUDE.
	// Pente au-dessous de laquelle la donnee dit « palier » (une allee, un quai, une
	// berme). 0,12 m/m : au-dessus du bruit du MNT reechantillonne (plateaux de
	// ~3,5 m) et TRES au-dessous du seuil qui a servi a detecter les murs
	// (0,50 m/m). Mesuree sur la base 2 m, comme la detection (work/DISCONT).
	constexpr float GWallPlateauSlope = 0.12f;
	constexpr float GWallPlateauBaseCm = 100.f;   // demi-base de la pente : +-1 m
	// Un mur garde toujours une pierre de couronnement : le bornage ne peut pas
	// reduire le cote crete a rien (sinon la face n'aurait plus de dessus).
	constexpr float GWallCrestMinCm = 100.f;

	// Une breakline, telle qu'elle sort du side-car (deja decoupee a la cellule).
	struct FRetainingWall
	{
		TArray<FVector2D> PtsCm;   // polyligne, cm locaux
		int32 CoteBas = -1;        // +1/-1 le long de la normale GAUCHE du sens de parcours
		float HMedCm = 0.f;        // hauteur mediane annoncee par le side-car (garde-fou)
		FString Classe;            // quai | tranchee | talus
		// QUAIS V4 : le prep a MESURE qu'une zone pietonne basse borde ce mur du
		// cote bas. C'est la condition NATIONALE des gradins (voir GTier* plus bas).
		bool bBordePieton = false;
		// BERGES : ce bout est-il un VRAI bout de mur, ou une coupe de cellule ?
		// Defaut true = comportement historique (un bouchon a chaque bout).
		bool bBoutDebut = true;
		bool bBoutFin = true;
		// BUILDQUAY : le RANG du mur dans le side-car de sa cellule, pris AVANT
		// tout filtrage de classe — c'est l'identifiant stable `cx_cy#rang` que
		// la cuisson emploie (miroir `b_lib.charger_murs`). Sans lui, la liste
		// `murs_exclus` designerait un autre mur des que `RetainingWallClasses`
		// est renseigne.
		int32 Rang = 0;
	};

	// =========================================================================
	// LOT QUAIS V4 — LES GRADINS DE QUAI.
	//
	// La photo de reference de l'utilisateur (quai de la Daurade au crepuscule)
	// montre ce que la face lisse ne sait pas rendre : le quai n'y tombe pas d'un
	// bloc dans l'eau, il DESCEND EN LARGES MARCHES sur lesquelles les gens
	// s'assoient. C'est un motif francais courant partout ou un quai borde une
	// promenade basse — donc une regle nationale, pas un cas toulousain.
	//
	// Dimensions : une assise, pas une marche. 45 cm de haut (on s'y assoit, hauteur
	// de banc haute) x 70 cm de profondeur (on y pose les pieds, ou on s'y allonge).
	// Ce sont les deux constantes a tourner en boucle B.
	constexpr float GTierRiseCm = 45.f;
	constexpr float GTierRunCm = 70.f;
	// Sous 2 m de hauteur MESUREE, un mur ne porte pas deux gradins : il reste lisse.
	constexpr float GTierMinHeightCm = 200.f;
	// Un gradin ne peut pas manger plus que la portee de sondage du mur : au-dela on
	// ne masque plus la rampe, on construit une esplanade. Le nombre de gradins est
	// donc PLAFONNE par la place disponible entre le pied et la crete.
	constexpr int32 GTierMaxCount = 24;

	FString RetainingWallsDir(const FCityGenProfile& Gen)
	{
		return Gen.RetainingWallsPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Murs"))
			: Gen.RetainingWallsPath;
	}

	// =========================================================================
	// LOT FINITION QUAIS — LES EMPRISES DE GRADINS (OSM `leisure=bleachers`).
	//
	// REGLE NATIONALE : le mecanisme de gradins (`bQuayTiers`) ne s'applique qu'a
	// la PORTION d'un mur de classe `quai` qui tombe DANS un polygone d'emprise.
	// Hors emprise le mur reste lisse — ce qui etait le comportement par defaut,
	// mais par DECISION ; il l'est desormais par ABSENCE DE DONNEE, ce qui est la
	// seule justification acceptable (doctrine « pas de donnee -> pas d'objet »).
	//
	// L'emprise dit OU, et RIEN d'autre : le nombre de gradins, le giron et la
	// contremarche restent produits par la regle, calee sur le denivele MESURE sur
	// la surface rendue. AUCUN identifiant OSM ne circule ici : la verite locale
	// vit dans le verrou nominatif (work/FINQUAIS/f_verrou_gradins.py).
	//
	// La regle est BORNEE PAR ELLE-MEME : sur le proto 3x3, trois emprises
	// existent et une seule paire croise un mur de quai. « Des gradins sur tout le
	// quai » est structurellement impossible.
	// Side-car : Tools/fetch_osm_bleachers.py -> SourceData/Gradins/gradins_X_Y.json.
	// Dossier absent ou cellule sans fichier = AUCUNE emprise, sans erreur.
	struct FGradinEmprise
	{
		TArray<FVector2D> PtsCm;      // anneau FERME, en cm, repere local
	};

	FString GradinsDir(const FCityGenProfile& Gen)
	{
		return FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Gradins"));
	}

	bool LoadGradinsCell(const FString& Dir, int32 CellX, int32 CellY, float CellSizeM,
		TArray<FGradinEmprise>& Out, int32& OutTailleKo)
	{
		const FString Path = FPaths::Combine(Dir,
			FString::Printf(TEXT("gradins_%d_%d.json"), CellX, CellY));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			RaiseError(FString::Printf(TEXT("Gradins file '%s' is not valid JSON."), *Path));
			return false;
		}
		double BakedCellM = 0.0;
		if (Root->TryGetNumberField(TEXT("cellSizeM"), BakedCellM) &&
			!FMath::IsNearlyEqual((float)BakedCellM, CellSizeM, 0.01f))
		{
			++OutTailleKo;
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("gradins"), Arr))
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (!O.IsValid() || !O->TryGetArrayField(TEXT("pts"), P))
			{
				continue;
			}
			FGradinEmprise E;
			for (const TSharedPtr<FJsonValue>& PV : *P)
			{
				const TArray<TSharedPtr<FJsonValue>>& C = PV->AsArray();
				if (C.Num() >= 2)
				{
					E.PtsCm.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
				}
			}
			if (E.PtsCm.Num() >= 4)
			{
				Out.Add(MoveTemp(E));
			}
		}
		return true;
	}

	// Appartenance d'un point a l'une des emprises (lancer de rayon pair/impair —
	// l'anneau est ferme, donc le test est exact et sans dependance externe).
	bool DansUneEmprise(const TArray<FGradinEmprise>& Emprises, const FVector2D& P)
	{
		for (const FGradinEmprise& E : Emprises)
		{
			bool bIn = false;
			const int32 N = E.PtsCm.Num();
			for (int32 i = 0, j = N - 1; i < N; j = i++)
			{
				const FVector2D& A = E.PtsCm[i];
				const FVector2D& B = E.PtsCm[j];
				if (((A.Y > P.Y) != (B.Y > P.Y)) &&
					(P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X))
				{
					bIn = !bIn;
				}
			}
			if (bIn)
			{
				return true;
			}
		}
		return false;
	}

	// =========================================================================
	// LOT VELOCITE — LE FILTRE DE CELLULES (« mode district »)
	//
	// Une regeneration complete du proto 3x3 coute ~35 min ; l'immense majorite
	// des iterations ne regarde qu'un quartier. Le filtre borne CHAQUE passe aux
	// cellules visees. Il est FERME PAR DEFAUT (chaine vide) : sans lui, pas une
	// seule branche du generateur ne change.
	// =========================================================================

	/**
	 * "-2_0, -2_1" -> { (-2,0), (-2,1) }. Rend false si la chaine est vide ou si
	 * AUCUNE cellule n'a pu etre lue (auquel cas l'appelant traite la ville entiere :
	 * un filtre illisible ne doit jamais se traduire par « ne rien generer », qui
	 * ressemblerait a une purge reussie).
	 */
	bool ParseCellFilter(const FString& Spec, TSet<FIntPoint>& Out)
	{
		Out.Reset();
		if (Spec.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
		TArray<FString> Parts;
		Spec.ParseIntoArray(Parts, TEXT(","), true);
		int32 Mauvais = 0;
		for (FString& P : Parts)
		{
			FString Sx, Sy;
			// Split PAR LA FIN : l'indice X peut etre negatif ("-2_0"), un split par le
			// debut couperait sur le signe et rendrait ("", "2_0").
			if (!P.TrimStartAndEnd().Split(TEXT("_"), &Sx, &Sy,
				ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				++Mauvais;
				continue;
			}
			Sx.TrimStartAndEndInline();
			Sy.TrimStartAndEndInline();
			if (Sx.IsEmpty() || Sy.IsEmpty() || !Sx.IsNumeric() || !Sy.IsNumeric())
			{
				++Mauvais;
				continue;
			}
			Out.Add(FIntPoint(FCString::Atoi(*Sx), FCString::Atoi(*Sy)));
		}
		if (Mauvais > 0)
		{
			// Display et non Warning : l'automation eleve les Warning en echec de test
			// (lecon C1 §12.3), et une entree mal formee se voit deja au compte final.
			UE_LOG(LogCityImport, Display,
				TEXT("Filtre de cellules : %d entree(s) illisible(s) dans '%s' — %d cellule(s) retenue(s)."),
				Mauvais, *Spec, Out.Num());
		}
		return Out.Num() > 0;
	}

	/**
	 * "SM_Bldg_-2_0_Wall" avec le prefixe "SM_Bldg_" -> (-2, 0).
	 * Rend false si le label ne porte pas le prefixe ou pas deux entiers derriere.
	 * C'est ce qui permet a la purge d'idempotence de ne detruire QUE les acteurs
	 * des cellules visees — la seule chose qui separe une regeneration partielle
	 * d'une destruction de la ville.
	 */
	bool CellFromLabel(const FString& Label, const TCHAR* Prefix, FIntPoint& Out)
	{
		FString Rest = Label;
		if (!Rest.RemoveFromStart(Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		TArray<FString> Toks;
		Rest.ParseIntoArray(Toks, TEXT("_"), false);
		if (Toks.Num() < 2 || !Toks[0].IsNumeric() || !Toks[1].IsNumeric())
		{
			return false;
		}
		Out = FIntPoint(FCString::Atoi(*Toks[0]), FCString::Atoi(*Toks[1]));
		return true;
	}

	/** Boite englobante (cm) des cellules visees, dilatee de MarginCm. */
	FBox2D CellFilterBounds(const TSet<FIntPoint>& Cells, float CellCm, float MarginCm)
	{
		FBox2D Box(ForceInit);
		for (const FIntPoint& K : Cells)
		{
			Box += FVector2D(K.X * CellCm, K.Y * CellCm);
			Box += FVector2D((K.X + 1) * CellCm, (K.Y + 1) * CellCm);
		}
		if (Box.bIsValid)
		{
			Box = Box.ExpandBy(MarginCm);
		}
		return Box;
	}

	// Rend false SANS erreur si la cellule n'a pas de fichier : une cellule sans
	// breakline cuite est un cas NORMAL (cuisson partielle, zone hors emprise).
	bool LoadRetainingWallCell(const FString& Dir, int32 CellX, int32 CellY, float CellSizeM,
		const TSet<FString>& Classes, TArray<FRetainingWall>& Out, int32& OutTailleKo,
		double& OutTailleCuiteM)
	{
		const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("murs_%d_%d.json"), CellX, CellY));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			RaiseError(FString::Printf(TEXT("Retaining wall file '%s' is not valid JSON."), *Path));
			return false;
		}
		// GARDE DE TAILLE DE CELLULE — et pourquoi ce n'est PAS une erreur.
		// Un side-car cuit pour une AUTRE taille de cellule poserait ses murs a une
		// demi-cellule d'ecart : on refuse, comme pour les masques de sol. Mais le
		// dossier par DEFAUT est partage par tous les imports du projet, et un import
		// a une autre maille (les tests d'automation tournent a 100 m) rencontre alors
		// un side-car qui n'est simplement PAS POUR LUI — ce n'est pas une faute, c'est
		// un cas normal. On COMPTE et on le dira UNE fois en Display a la fin de la
		// passe. (Ni Error, ni Warning : l'automation eleve les deux en echec de test —
		// meme raison que le repli de FSurfaceLibrary::Resolve. Regression trouvee par
		// la spec le 02/08 : 2 tests ProfilDesktop tombaient sur 36 erreurs de garde.)
		double BakedCellM = 0.0;
		if (Root->TryGetNumberField(TEXT("cellSizeM"), BakedCellM) &&
			!FMath::IsNearlyEqual((float)BakedCellM, CellSizeM, 0.01f))
		{
			++OutTailleKo;
			OutTailleCuiteM = BakedCellM;
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("murs"), Arr))
		{
			return true;
		}
		int32 RangBrut = -1;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			++RangBrut;
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid())
			{
				continue;
			}
			FRetainingWall W;
			W.Rang = RangBrut;
			// TryGet* et non Get* : un champ manquant est un side-car d'une version
			// anterieure, pas une erreur — et Get* journalise, ce que l'automation
			// eleverait en echec de test.
			FString Classe;
			O->TryGetStringField(TEXT("classe"), Classe);
			W.Classe = Classe.ToLower();
			if (Classes.Num() > 0 && !Classes.Contains(W.Classe))
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (!O->TryGetArrayField(TEXT("pts"), P))
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& PV : *P)
			{
				const TArray<TSharedPtr<FJsonValue>>& C = PV->AsArray();
				if (C.Num() >= 2)
				{
					W.PtsCm.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
				}
			}
			if (W.PtsCm.Num() < 2)
			{
				continue;
			}
			double CoteBas = -1.0;
			double HMed = 0.0;
			O->TryGetNumberField(TEXT("cote_bas"), CoteBas);
			O->TryGetNumberField(TEXT("h_med"), HMed);
			W.CoteBas = (CoteBas >= 0.0) ? 1 : -1;
			W.HMedCm = (float)(HMed * 100.0);
			// QUAIS V4 : absent d'un side-car anterieur au lot = faux, donc face lisse.
			O->TryGetBoolField(TEXT("borde_pieton"), W.bBordePieton);
			// BERGES : les VRAIS bouts. Absents d'un side-car anterieur = true des
			// deux cotes, c'est-a-dire le comportement historique (bouchon partout).
			O->TryGetBoolField(TEXT("bout_debut"), W.bBoutDebut);
			O->TryGetBoolField(TEXT("bout_fin"), W.bBoutFin);
			Out.Add(MoveTemp(W));
		}
		return true;
	}

	// =========================================================================
	// LOT QUAIS V2 — LES ESCALIERS.
	//
	// LES CONSTANTES, ET D'OU ELLES VIENNENT.
	// La contremarche de 16 a 17 cm est la valeur d'usage francaise (et la borne
	// haute reglementaire des ERP) : c'est elle qui FIXE LE NOMBRE DE MARCHES, parce
	// que c'est la seule des deux dimensions qu'on ne peut pas negocier — un escalier
	// dont les contremarches ne sont pas egales se lit immediatement comme faux.
	constexpr float GStairRiserCm = 16.5f;
	// Bornes de rattrapage : le nombre de marches est un ENTIER, la contremarche
	// reelle vaut donc dZ/n et doit rester dans la fourchette d'usage.
	constexpr float GStairRiserMinCm = 13.0f;
	constexpr float GStairRiserMaxCm = 19.0f;
	// Le giron se DEDUIT de la longueur en plan. Sous 22 cm ce n'est plus un
	// escalier mais une echelle : on ECARTE et on compte. Au-dessus de 45 cm ce
	// n'est plus une marche : le surplus part en PALIERS (voir ci-dessous).
	constexpr float GStairTreadMinCm = 22.f;
	constexpr float GStairTreadMaxCm = 45.f;
	// LE PALIER, ET POURQUOI IL EXISTE. Mesure aux deux escaliers du Pont
	// Saint-Pierre : 7,84 m de denivele pour 31,9 m de trace en plan. Un giron
	// uniforme vaudrait 66 cm — ni une marche, ni une pente : une aberration. Un
	// grand escalier de quai francais est en realite une suite de VOLEES separees
	// de PALIERS de repos. On repartit donc le surplus de longueur en paliers de
	// ~2 m, et la volee redevient lisible.
	constexpr float GStairLandingCm = 200.f;
	// Largeur par defaut quand aucune source ne la donne (mesure : 0/35 renseignes
	// en BD TOPO, 0/436 en OSM sur l'emprise) — volee urbaine courante.
	constexpr float GStairWidthCm = 250.f;
	constexpr float GStairWidthMinCm = 80.f;
	constexpr float GStairWidthMaxCm = 1200.f;
	// Sous une marche de denivele, il n'y a pas d'escalier a poser : on ECARTE.
	constexpr float GStairMinDropCm = 33.f;
	// Un escalier de plus de 200 marches sur l'emprise d'une cellule est une erreur
	// de lecture du sol rendu, pas un escalier.
	constexpr int32 GStairMaxSteps = 200;
	// Pied ENTERRE : meme raison que le mur — la dalle rendue et la volee divergent
	// de quelques centimetres entre deux sommets de la grille du drape.
	constexpr float GStairSinkCm = 30.f;
	// PENTE MINIMALE — contre les BISEAUX (iteration utilisateur 2 du 02/08, promue
	// ici depuis le corps de `BuildStairs` au build de consolidation du lot PIE).
	// Grief : des « lames anguleuses » sur la place. Cause MESUREE : une volee BD TOPO
	// longue posee sur du terrain presque plat garde assez de denivele pour passer le
	// plancher de 33 cm, mais son giron theorique explose ; le surplus part en paliers
	// et la piece devient un biseau de 20 a 40 m couche au sol (21,6 m pour 74,7 cm de
	// denivele, soit 90 % de palier). Le critere qui SEPARE n'est ni le denivele ni le
	// nombre de marches — les deux se recouvrent — c'est la PENTE. Distribution des
	// 36 volees de l'emprise 3x3 : un VIDE FRANC entre 9,1 % et 12,7 %. Le seuil est
	// pose au MILIEU de ce vide ; aucune valeur mesuree n'est a moins de 1,7 point de
	// lui. Les deux volees du Pont Saint-Pierre sont a 23,4 et 23,6 %, la volee raide
	// de la Daurade a 33,5 % : un escalier reel tient tres largement au-dessus.
	constexpr float GStairMinSlopePct = 11.0f;

	// Une volee, telle qu'elle sort du side-car (deja decoupee a la cellule).
	struct FCityStairs
	{
		TArray<FVector2D> PtsCm;    // polyligne EN PLAN, cm locaux — aucun Z
		float WidthCm = GStairWidthCm;
		int32 MarchesSideCar = 0;   // step_count OSM quand il existe : GARDE-FOU seul
		FString Source;             // bdtopo | osm
		FString Id;
	};

	FString StairsDir(const FCityGenProfile& Gen)
	{
		return Gen.StairsPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Escaliers"))
			: Gen.StairsPath;
	}

	// Meme grammaire, memes gardes et meme discipline de journalisation que
	// LoadRetainingWallCell : cellule sans fichier = cas NORMAL (false sans erreur),
	// side-car cuit pour une autre maille = refus COMPTE, en Display, jamais en
	// Error (l'automation eleve Error et Warning en echec de test — regression payee
	// par le lot C1 le 02/08).
	bool LoadStairsCell(const FString& Dir, int32 CellX, int32 CellY, float CellSizeM,
		TArray<FCityStairs>& Out, int32& OutTailleKo, double& OutTailleCuiteM)
	{
		const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("escaliers_%d_%d.json"), CellX, CellY));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			RaiseError(FString::Printf(TEXT("Stairs file '%s' is not valid JSON."), *Path));
			return false;
		}
		double BakedCellM = 0.0;
		if (Root->TryGetNumberField(TEXT("cellSizeM"), BakedCellM) &&
			!FMath::IsNearlyEqual((float)BakedCellM, CellSizeM, 0.01f))
		{
			++OutTailleKo;
			OutTailleCuiteM = BakedCellM;
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("escaliers"), Arr))
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid())
			{
				continue;
			}
			FCityStairs S;
			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (!O->TryGetArrayField(TEXT("pts"), P))
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& PV : *P)
			{
				const TArray<TSharedPtr<FJsonValue>>& C = PV->AsArray();
				if (C.Num() >= 2)
				{
					S.PtsCm.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
				}
			}
			if (S.PtsCm.Num() < 2)
			{
				continue;
			}
			double W = 0.0;
			if (O->TryGetNumberField(TEXT("largeur_m"), W))
			{
				S.WidthCm = FMath::Clamp((float)(W * 100.0), GStairWidthMinCm, GStairWidthMaxCm);
			}
			double M = 0.0;
			if (O->TryGetNumberField(TEXT("marches"), M))
			{
				S.MarchesSideCar = (int32)M;
			}
			O->TryGetStringField(TEXT("source"), S.Source);
			O->TryGetStringField(TEXT("id"), S.Id);
			Out.Add(MoveTemp(S));
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// CHANTIER C2 (03/08) — LES PONTS.
	//
	// Ce que la passe remplace, MESURE : le ruban de pont d'avant C2 recevait son Z
	// par interpolation du MNT ENTRE SES DEUX BOUTS (ComputePolylineZ, bBridge=true).
	// Sur le Pont Saint-Pierre cela donnait 134,50 -> 135,03 m alors que le tablier
	// est a 142,10-142,70 m : 7,62 m de manque MOYEN, 7,95 m au pire. Le pont
	// traversait donc la promenade au niveau du sable. La cote vient desormais de la
	// GEOMETRIE 3D de BD TOPO, cuite par cellule dans SourceData/Ponts.
	//
	// PAS DE PILES. Rien dans la donnee ne dit ou sont les appuis d'un ouvrage : les
	// inventer serait exactement la faute des gradins (« pas de donnee, pas d'objet »).
	// Le tablier est donc une DALLE qui part du sol d'une rive et y revient — la
	// chaine d'ouvrage du side-car s'arrete sur le troncon qui atterrit, ce qui
	// garantit qu'elle rejoint le terrain sans marche et sans culee aveugle.
	constexpr float GBridgeDeckThickCm = 90.f;   // epaisseur du tablier (sous-face)
	constexpr float GBridgeParapetHCm = 100.f;   // hauteur du parapet au-dessus de la chaussee
	constexpr float GBridgeParapetWCm = 25.f;    // epaisseur du parapet
	constexpr float GBridgeStepCm = 400.f;       // re-echantillonnage de l'axe
	// En dessous de cette hauteur libre, le « pont » est au ras du sol : ni parapet
	// ni sous-face visible. Mesure qui l'exige : le troncon « Quai Saint Pierre » est
	// code position=+1 pour 2 cm de hauteur reelle.
	constexpr float GBridgeMinClearCm = 100.f;
	// Le bord du tablier s'enfonce sous le sol au raccord : meme remede que le pied
	// de bordure (GMaskCurbSinkCm) — zero jour, zero triangle.
	constexpr float GBridgeSinkCm = 25.f;

	struct FCityBridge
	{
		TArray<FVector2D> PtsCm;   // axe EN PLAN, cm locaux
		TArray<float> ZCm;         // Z UNREAL par sommet (vide si le side-car n'a pas de Z)
		bool bHasZ = false;
		float WidthCm = 600.f;
		int32 Lanes = 0;
		int32 Pos = 0;
		FString Id;
		FString Nom;
		bool bFreeStart = false;
		bool bFreeEnd = false;
	};

	FString BridgesDir(const FCityGenProfile& Gen)
	{
		return Gen.BridgesPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Ponts"))
			: Gen.BridgesPath;
	}

	// Meme grammaire, memes gardes et meme discipline de journalisation que
	// LoadStairsCell / LoadRetainingWallCell. AltCapCm : le side-car porte des
	// ALTITUDES NGF en metres (c'est la donnee IGN telle quelle) ; le rebase sur
	// l'origine Unreal se fait ICI, au seul endroit qui connaisse les deux reperes.
	bool LoadBridgesCell(const FString& Dir, int32 CellX, int32 CellY, float CellSizeM,
		float AltCapCm, TArray<FCityBridge>& Out, int32& OutTailleKo, double& OutTailleCuiteM)
	{
		const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("ponts_%d_%d.json"), CellX, CellY));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			RaiseError(FString::Printf(TEXT("Bridges file '%s' is not valid JSON."), *Path));
			return false;
		}
		double BakedCellM = 0.0;
		if (Root->TryGetNumberField(TEXT("cellSizeM"), BakedCellM) &&
			!FMath::IsNearlyEqual((float)BakedCellM, CellSizeM, 0.01f))
		{
			++OutTailleKo;
			OutTailleCuiteM = BakedCellM;
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("ponts"), Arr))
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid())
			{
				continue;
			}
			FCityBridge B;
			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (!O->TryGetArrayField(TEXT("pts"), P))
			{
				continue;
			}
			bool bAllZ = true;
			for (const TSharedPtr<FJsonValue>& PV : *P)
			{
				const TArray<TSharedPtr<FJsonValue>>& C = PV->AsArray();
				if (C.Num() < 2)
				{
					continue;
				}
				B.PtsCm.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
				if (C.Num() >= 3 && C[2].IsValid() && C[2]->Type == EJson::Number)
				{
					B.ZCm.Add((float)(C[2]->AsNumber() * 100.0) - AltCapCm);
				}
				else
				{
					B.ZCm.Add(0.f);
					bAllZ = false;
				}
			}
			if (B.PtsCm.Num() < 2)
			{
				continue;
			}
			B.bHasZ = bAllZ;
			double W = 0.0;
			if (O->TryGetNumberField(TEXT("largeur_m"), W) && W > 0.0)
			{
				B.WidthCm = FMath::Clamp((float)(W * 100.0), 250.f, 3000.f);
			}
			double L = 0.0;
			if (O->TryGetNumberField(TEXT("voies"), L))
			{
				B.Lanes = (int32)L;
			}
			double Pos = 0.0;
			if (O->TryGetNumberField(TEXT("pos"), Pos))
			{
				B.Pos = (int32)Pos;
			}
			O->TryGetStringField(TEXT("id"), B.Id);
			O->TryGetStringField(TEXT("nom"), B.Nom);
			const TArray<TSharedPtr<FJsonValue>>* Libres = nullptr;
			if (O->TryGetArrayField(TEXT("bout_libre"), Libres) && Libres->Num() >= 2)
			{
				B.bFreeStart = (*Libres)[0]->AsBool();
				B.bFreeEnd = (*Libres)[1]->AsBool();
			}
			Out.Add(MoveTemp(B));
		}
		return true;
	}

	// -----------------------------------------------------------------------------
	// LOT EAU — LES SURFACES EN EAU. Meme grammaire de side-car que Ponts/, Murs/,
	// Escaliers/ et Promenade/ : une cellule par fichier, `cellSizeM` verifie a la
	// lecture, et une ALTITUDE NGF par sommet — le rebase sur l'origine Unreal se
	// fait ICI, au seul endroit qui connaisse les deux reperes.
	// -----------------------------------------------------------------------------
	struct FCityWaterBody
	{
		TArray<FVector2D> PtsCm;   // contour, repere Unreal cm
		TArray<float> ZCm;         // cote de CHAQUE sommet, Z Unreal cm
		// LOT BERGES — L'ECOULEMENT, PAR SOMMET, LU DANS LA DONNEE.
		// Direction unitaire (x, y) de l'ecoulement AVAL, cuite dans le side-car
		// depuis les troncons hydrographiques BD TOPO (qui portent le sens). Elle
		// voyage jusqu'au materiau dans la COULEUR DE SOMMET (R = 0,5 + 0,5 dx,
		// G = 0,5 + 0,5 dy) : le canal etait libre — le materiau d'eau ne lisait pas
		// la VertexColor — et c'est le seul moyen d'avoir un ecoulement qui suit le
		// fleuve au lieu d'une constante globale, qui serait un cas particulier.
		TArray<FVector2D> Flux;
		// LOT FRONTIERE-Z — LES TRIANGLES VIENNENT DE LA CUISSON.
		// Mesure (work/FRONTZ/z4, z5) : les « rayons » sur le fleuve — dont celui
		// qui passe SOUS le Pont Saint-Pierre — ne sont ni un barrage ni une
		// frontiere de piece : ce sont les aretes INTERIEURES du repli en EVENTAIL
		// de `TriangulateRing` (p50 67,96 m, p90 270,91 m, max 474,60 m, contre
		// p50 10,11 m pour les aretes de bord ; un sommet portait 40 triangles sur
		// 47). La cote etant interpolee LINEAIREMENT sur ces triangles tres
		// allonges, chaque arete est une cassure de la surface qui accroche la
		// lumiere rasante. La cuisson livre desormais une triangulation de Delaunay
		// CONTRAINTE (ear-clipping + bascules de Lawson : meme couverture exacte,
		// meme nombre de triangles, aucun sommet ajoute) et le moteur ne fait que
		// POSER — comme pour les murs, les ponts et la promenade.
		// Vide = side-car anterieur : `TriangulateRing` reprend la main, bit pour bit.
		TArray<int32> Tris;
		FString Cleabs;
		FString Nature;
		double AreaM2 = 0.0;
	};

	FString WaterDir(const FCityGenProfile& Gen)
	{
		return Gen.WaterPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Eau"))
			: Gen.WaterPath;
	}

	bool LoadWaterCell(const FString& Dir, int32 CellX, int32 CellY, float CellSizeM,
		float AltCapCm, TArray<FCityWaterBody>& Out, int32& OutTailleKo, double& OutTailleCuiteM)
	{
		const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("eau_%d_%d.json"), CellX, CellY));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			RaiseError(FString::Printf(TEXT("Water file '%s' is not valid JSON."), *Path));
			return false;
		}
		double BakedCellM = 0.0;
		if (Root->TryGetNumberField(TEXT("cellSizeM"), BakedCellM) &&
			!FMath::IsNearlyEqual((float)BakedCellM, CellSizeM, 0.01f))
		{
			++OutTailleKo;
			OutTailleCuiteM = BakedCellM;
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("eau"), Arr))
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (!O->TryGetArrayField(TEXT("pts"), P))
			{
				continue;
			}
			FCityWaterBody W;
			bool bAllZ = true;
			for (const TSharedPtr<FJsonValue>& PV : *P)
			{
				const TArray<TSharedPtr<FJsonValue>>& C = PV->AsArray();
				if (C.Num() < 3 || !C[2].IsValid() || C[2]->Type != EJson::Number)
				{
					bAllZ = false;
					continue;
				}
				W.PtsCm.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
				W.ZCm.Add((float)(C[2]->AsNumber() * 100.0) - AltCapCm);
			}
			// « Pas de cote, pas d'objet » : une surface en eau sans altitude n'a
			// aucun repli acceptable — le plan enterre est justement le defaut qu'on
			// corrige. On la garde a part pour que la passe la journalise.
			O->TryGetStringField(TEXT("cleabs"), W.Cleabs);
			O->TryGetStringField(TEXT("nature"), W.Nature);
			O->TryGetNumberField(TEXT("aire_m2"), W.AreaM2);
			// BERGES : l'ecoulement par sommet. Absent d'un side-car anterieur =
			// aucun flux, et le materiau retombe sur son defaut (pas de derive).
			const TArray<TSharedPtr<FJsonValue>>* FluxArr = nullptr;
			if (O->TryGetArrayField(TEXT("flux"), FluxArr) && FluxArr->Num() == W.PtsCm.Num())
			{
				for (const TSharedPtr<FJsonValue>& FV : *FluxArr)
				{
					const TArray<TSharedPtr<FJsonValue>>& C = FV->AsArray();
					W.Flux.Add(C.Num() >= 2
						? FVector2D(C[0]->AsNumber(), C[1]->AsNumber())
						: FVector2D::ZeroVector);
				}
			}
			// FRONTIERE-Z : la triangulation cuite. On la REFUSE en silence si elle
			// n'est pas exploitable (taille non multiple de 3, indice hors bornes,
			// triangle degenere) — le repli sur `TriangulateRing` est le
			// comportement historique, il n'y a donc rien a casser.
			const TArray<TSharedPtr<FJsonValue>>* TriArr = nullptr;
			if (O->TryGetArrayField(TEXT("tris"), TriArr) && TriArr->Num() >= 3
				&& (TriArr->Num() % 3) == 0)
			{
				bool bOk = true;
				TArray<int32> T;
				T.Reserve(TriArr->Num());
				for (const TSharedPtr<FJsonValue>& TV : *TriArr)
				{
					const int32 I = (int32)TV->AsNumber();
					if (I < 0 || I >= W.PtsCm.Num())
					{
						bOk = false;
						break;
					}
					T.Add(I);
				}
				for (int32 t = 0; bOk && t + 2 < T.Num(); t += 3)
				{
					if (T[t] == T[t + 1] || T[t + 1] == T[t + 2] || T[t + 2] == T[t])
					{
						bOk = false;
					}
				}
				if (bOk)
				{
					W.Tris = MoveTemp(T);
				}
			}
			if (!bAllZ)
			{
				W.ZCm.Empty();
			}
			Out.Add(MoveTemp(W));
		}
		return true;
	}

	// Le RELIEF d'une cellule, lu dans sols_<x>_<y>.json. Tout est deja decoupe au
	// prep (bordures orientees chaussee a gauche, tirets debites, passages
	// dedoublonnes) : ici on ne fait que poser des quads.
	struct FMaskCrossing
	{
		FVector2D PosCm = FVector2D::ZeroVector;
		FVector2D DirCm = FVector2D::ZeroVector;
		float HalfWCm = 0.f;
	};

	struct FGroundMaskCell
	{
		TArray<TArray<FVector2D>> Curbs;  // polylignes cm, chaussee A GAUCHE
		// V3 : contour d'herbe a border. Memes conventions que Curbs, MINERAL a
		// gauche — donc la face verticale de la pierre regarde le mineral et son
		// chant deborde sur la pelouse.
		TArray<TArray<FVector2D>> GrassEdges;
		TArray<FMaskCrossing> Crossings;
		TArray<FVector4> Axial;           // (ax, ay, bx, by) en cm
	};

	FString GroundMasksDir(const FCityGenProfile& Gen)
	{
		return Gen.GroundMasksPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Sols"))
			: Gen.GroundMasksPath;
	}

	FString GroundMasksAssetDir(const FCityGenProfile& Gen)
	{
		return Gen.GroundMasksAssetFolder.IsEmpty()
			? FString(TEXT("/Game/City/Ground")) : Gen.GroundMasksAssetFolder;
	}

	// Rend false SANS erreur si la cellule n'a pas de masque : une cellule sans
	// masque garde le comportement actuel, c'est un mode de fonctionnement normal
	// (cuisson partielle, zone proto).
	bool LoadGroundMaskCell(const FString& Dir, int32 CellX, int32 CellY, float CellSizeM,
		FGroundMaskCell& Out)
	{
		const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("sols_%d_%d.json"), CellX, CellY));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			RaiseError(FString::Printf(TEXT("Ground mask file '%s' is not valid JSON."), *Path));
			return false;
		}
		// Le masque est cuit POUR une taille de cellule : le cuire a 500 m puis
		// generer a 250 m decalerait chaque masque d'une demi-cellule sans que rien
		// ne proteste. On refuse plutot que de peindre a cote.
		double BakedCellM = 0.0;
		if (Root->TryGetNumberField(TEXT("cellSizeM"), BakedCellM) &&
			!FMath::IsNearlyEqual((float)BakedCellM, CellSizeM, 0.01f))
		{
			RaiseError(FString::Printf(
				TEXT("Ground mask '%s' was baked for %.0f m cells but the import uses %.0f m."),
				*Path, BakedCellM, CellSizeM));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		// Les polylignes (bordures de chaussee et bordurettes d'herbe) ont exactement
		// le meme encodage : une seule lecture, deux champs.
		auto LoadPolylines = [&](const TCHAR* Field, TArray<TArray<FVector2D>>& Dest)
		{
			const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
			if (!Root->TryGetArrayField(Field, A))
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				TArray<FVector2D> Line;
				for (const TSharedPtr<FJsonValue>& PV : V->AsArray())
				{
					const TArray<TSharedPtr<FJsonValue>>& Comp = PV->AsArray();
					if (Comp.Num() >= 2)
					{
						Line.Add(FVector2D(Comp[0]->AsNumber() * 100.0, Comp[1]->AsNumber() * 100.0));
					}
				}
				if (Line.Num() >= 2)
				{
					Dest.Add(MoveTemp(Line));
				}
			}
		};
		LoadPolylines(TEXT("curbs"), Out.Curbs);
		// Champ ABSENT sur un masque cuit avant la v3 : aucune bordurette, et rien
		// d'autre ne change. Une regeneration sur d'anciens masques reste valide.
		LoadPolylines(TEXT("grassEdges"), Out.GrassEdges);
		if (Root->TryGetArrayField(TEXT("crossings"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject>& O = V->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* D = nullptr;
				if (!O.IsValid() || !O->TryGetArrayField(TEXT("p"), P) ||
					!O->TryGetArrayField(TEXT("d"), D) || P->Num() < 2 || D->Num() < 2)
				{
					continue;
				}
				FMaskCrossing Site;
				Site.PosCm = FVector2D((*P)[0]->AsNumber() * 100.0, (*P)[1]->AsNumber() * 100.0);
				Site.DirCm = FVector2D((*D)[0]->AsNumber(), (*D)[1]->AsNumber());
				Site.HalfWCm = (float)(O->GetNumberField(TEXT("halfW")) * 100.0);
				if (Site.HalfWCm > 0.f && !Site.DirCm.IsNearlyZero())
				{
					Out.Crossings.Add(Site);
				}
			}
		}
		if (Root->TryGetArrayField(TEXT("axial"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TArray<TSharedPtr<FJsonValue>>& S = V->AsArray();
				if (S.Num() >= 4)
				{
					Out.Axial.Add(FVector4(S[0]->AsNumber() * 100.0, S[1]->AsNumber() * 100.0,
						S[2]->AsNumber() * 100.0, S[3]->AsNumber() * 100.0));
				}
			}
		}
		return true;
	}

	// -----------------------------------------------------------------------------
	// LES MASQUES DE SOL CUITS = LES QUATRE CHAMPS DE DISTANCE DU SHADER DE DALLE.
	//
	// mask_<cx>_<cy>.png, une image carree par cellule de 500 m, quatre distances
	// SIGNEES a la frontiere de chaque couche (octet 128 = frontiere, pleine echelle
	// +-2 m, soit d = (octet/255 - 0,5) x 4 m) :
	//     R = HERBE      G = CHAUSSEE      B = VOIRIE PRIVEE      A = GRAVIER
	//
	// Convention de coordonnees REPRISE TELLE QUELLE du semis, deja validee a l'oeil
	// (les touffes tombent bien dans l'herbe) : colonne = X croissant, LIGNE = Y
	// croissant, aucune inversion verticale.
	//
	// LE SOL RENDU N'EST PAS LE CANAL R. Le master M_CityGroundMasked
	// (Tools/import_ground_masks.py) empile : dalle -> HERBE -> gravier -> privee ->
	// chaussee, chaque couche remplacant la precedente des que son poids vaut 1, et
	// le poids est un seuil FRANC a d = 0. Autrement dit LE PEINT GAGNE TOUJOURS SUR
	// L'HERBE. De plus la frontiere d'herbe est DEPLACEE par deux octaves d'un bruit
	// tuilable (jusqu'a +-87 cm) : c'est ce bord bruite que l'oeil voit, pas le bord
	// brut du canal R.
	// Le sol RENDU est donc vert si et seulement si l'herbe gagne ET qu'aucune couche
	// peinte ne repasse par-dessus ; sinon il est MINERAL, et un arbre qui y pousse
	// merite sa fosse. Juger sur le seul canal R laissait sans fosse les arbres de
	// LISIERE, exactement le defaut signale.
	//
	// COUPLAGE ASSUME : les cinq constantes ci-dessous sont celles de
	// Tools/import_ground_masks.py (SDF_RANGE_M, NOISE1_M/AMP, NOISE2_M/AMP). Elles
	// decrivent un FORMAT DE DONNEES cuit sur disque — les changer d'un cote sans
	// l'autre ferait mentir la regle d'attribution.
	//
	// Aucun masque ne couvre le point -> on ne DEVINE pas : pas de masque, pas de
	// fosse (une fosse inventee au hasard serait pire que rien).
	// -----------------------------------------------------------------------------
	constexpr float GMaskSdfRangeM = 2.f;    // octet 0/255 = -2 m / +2 m
	constexpr float GMaskNoise1PeriodM = 9.f;
	constexpr float GMaskNoise1Amp = 0.55f;
	constexpr float GMaskNoise2PeriodM = 48.f;
	constexpr float GMaskNoise2Amp = 1.20f;

	struct FGrassMaskSampler
	{
		explicit FGrassMaskSampler(const FString& InDir)
			: Dir(InDir)
		{
			// La taille de cellule est declaree PAR LES MASQUES eux-memes
			// (SourceData/Sols/index.json) : jamais une constante devinee ici.
			FString Text;
			TSharedPtr<FJsonObject> Root;
			if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, TEXT("index.json"))) &&
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) &&
				Root.IsValid())
			{
				double V = 0.0;
				if (Root->TryGetNumberField(TEXT("cellSizeM"), V) && V > 0.0)
				{
					CellSizeM = (float)V;
				}
			}
			LoadNoise();
		}

		/** Les 4 octets du masque au point. false si aucun masque ne couvre. */
		bool SampleRGBA(double Xcm, double Ycm, uint8 Out[4])
		{
			if (CellSizeM <= 0.f)
			{
				return false;
			}
			const double Xm = Xcm / 100.0;
			const double Ym = Ycm / 100.0;
			const FIntPoint Key(FMath::FloorToInt32(Xm / (double)CellSizeM),
				FMath::FloorToInt32(Ym / (double)CellSizeM));
			const FCell* Cell = GetCell(Key);
			if (!Cell || Cell->Size <= 0)
			{
				return false;
			}
			const double PxM = (double)CellSizeM / (double)Cell->Size;
			const int32 Col = FMath::Clamp(
				FMath::FloorToInt32((Xm - (double)Key.X * CellSizeM) / PxM), 0, Cell->Size - 1);
			const int32 Row = FMath::Clamp(
				FMath::FloorToInt32((Ym - (double)Key.Y * CellSizeM) / PxM), 0, Cell->Size - 1);
			const int64 O = ((int64)Row * Cell->Size + Col) * 4;
			for (int32 c = 0; c < 4; ++c)
			{
				Out[c] = Cell->Pixels[O + c];
			}
			return true;
		}

		int32 SampleR(double Xcm, double Ycm)
		{
			uint8 P[4];
			return SampleRGBA(Xcm, Ycm, P) ? (int32)P[0] : -1;
		}

		/** true seulement si le masque EXISTE et dit « mineral » AU CANAL R BRUT. */
		bool IsMineral(double Xcm, double Ycm)
		{
			const int32 R = SampleR(Xcm, Ycm);
			return R >= 0 && R < 128;
		}

		/**
		 * true seulement si le masque EXISTE et si le sol TEL QUE LE SHADER LE REND
		 * y est mineral. C'est la regle d'attribution des fosses.
		 */
		bool IsRenderedMineral(double Xcm, double Ycm)
		{
			uint8 P[4];
			if (!SampleRGBA(Xcm, Ycm, P))
			{
				return false;
			}
			const float Ech = 2.f * GMaskSdfRangeM;
			const float DHerbeBrut = ((float)P[0] / 255.f - 0.5f) * Ech;
			const float DChaussee = ((float)P[1] / 255.f - 0.5f) * Ech;
			const float DPrivee = ((float)P[2] / 255.f - 0.5f) * Ech;
			const float DGravier = ((float)P[3] / 255.f - 0.5f) * Ech;
			const double Xm = Xcm / 100.0;
			const double Ym = Ycm / 100.0;
			const float DHerbe = DHerbeBrut
				+ (SampleNoise(Xm, Ym, GMaskNoise1PeriodM) - 0.5f) * GMaskNoise1Amp
				+ (SampleNoise(Xm, Ym, GMaskNoise2PeriodM) - 0.5f) * GMaskNoise2Amp;
			const bool bHerbeGagne = DHerbe > 0.f;
			const bool bPeint = (DChaussee > 0.f) || (DPrivee > 0.f) || (DGravier > 0.f);
			return !bHerbeGagne || bPeint;
		}

		/**
		 * LOT6 — DISTANCE SIGNEE A LA CHAUSSEE PEINTE, en centimetres (>0 = SUR la
		 * chaussee, <0 = a cote, la valeur EST la distance au bord).
		 *
		 * Le canal G du masque N'EST PAS un booleen : c'est deja le champ de distance
		 * signee de la couche chaussee, sature a +-GMaskSdfRangeM. Il porte donc a la
		 * fois la direction (par son gradient) et la distance (par sa valeur) dont la
		 * retraction des plantations a besoin. Aucun bruit ne lui est applique : seule
		 * la frontiere d'HERBE est deplacee par le bruit du materiau, le peint garde son
		 * bord franc — c'est ce bord-la que l'oeil voit comme « le bord de la route ».
		 *
		 * Hors masque, on ne DEVINE pas : valeur tres negative = aucune chaussee connue
		 * ici, donc rien a retracter (une retraction inventee serait pire que rien).
		 */
		float RoadDistCm(double Xcm, double Ycm)
		{
			uint8 P[4];
			if (!SampleRGBA(Xcm, Ycm, P))
			{
				return -1.0e6f;
			}
			return ((float)P[1] / 255.f - 0.5f) * (2.f * GMaskSdfRangeM) * 100.f;
		}

		int32 NumCellsLoaded() const { return Loaded; }
		bool HasNoise() const { return NoiseSize > 0; }
		/** Taille de cellule DECLAREE par les masques (index.json), jamais devinee. */
		float GetCellSizeM() const { return CellSizeM; }

	private:
		struct FCell
		{
			int32 Size = 0;
			TArray<uint8> Pixels;   // RGBA entrelace
		};

		void LoadNoise()
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes,
				*FPaths::Combine(Dir, TEXT("noise512.png"))))
			{
				return;
			}
			IImageWrapperModule& Module =
				FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
			const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
			TArray64<uint8> Raw;
			if (Wrapper.IsValid() && Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) &&
				Wrapper->GetWidth() == Wrapper->GetHeight() &&
				Wrapper->GetRaw(ERGBFormat::RGBA, 8, Raw))
			{
				const int32 N = (int32)Wrapper->GetWidth();
				if (Raw.Num() >= (int64)N * N * 4)
				{
					NoiseSize = N;
					Noise.SetNumUninitialized(N * N);
					for (int32 i = 0; i < N * N; ++i)
					{
						Noise[i] = Raw[(int64)i * 4];
					}
				}
			}
		}

		/** Bruit tuilable, echantillonnage BILINEAIRE en UV monde metriques —
		    exactement le noeud TextureCoordinate(1/periode) du materiau. */
		float SampleNoise(double Xm, double Ym, float PeriodM) const
		{
			if (NoiseSize <= 0 || PeriodM <= 0.f)
			{
				return 0.5f;   // neutre : le bruit ne deplace rien
			}
			const double U = (Xm / (double)PeriodM) * (double)NoiseSize;
			const double V = (Ym / (double)PeriodM) * (double)NoiseSize;
			const int64 X0 = (int64)FMath::FloorToDouble(U);
			const int64 Y0 = (int64)FMath::FloorToDouble(V);
			const float Fx = (float)(U - (double)X0);
			const float Fy = (float)(V - (double)Y0);
			auto Texel = [&](int64 X, int64 Y) -> float
			{
				const int32 Cx = (int32)(((X % NoiseSize) + NoiseSize) % NoiseSize);
				const int32 Cy = (int32)(((Y % NoiseSize) + NoiseSize) % NoiseSize);
				return (float)Noise[Cy * NoiseSize + Cx] / 255.f;
			};
			const float A = FMath::Lerp(Texel(X0, Y0), Texel(X0 + 1, Y0), Fx);
			const float B = FMath::Lerp(Texel(X0, Y0 + 1), Texel(X0 + 1, Y0 + 1), Fx);
			return FMath::Lerp(A, B, Fy);
		}

		const FCell* GetCell(const FIntPoint& Key)
		{
			if (TUniquePtr<FCell>* Found = Cells.Find(Key))
			{
				return Found->Get();
			}
			TUniquePtr<FCell> Cell = MakeUnique<FCell>();
			const FString Path = FPaths::Combine(Dir,
				FString::Printf(TEXT("mask_%d_%d.png"), Key.X, Key.Y));
			TArray<uint8> Bytes;
			if (FFileHelper::LoadFileToArray(Bytes, *Path))
			{
				IImageWrapperModule& Module =
					FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
				const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
				TArray64<uint8> Raw;
				if (Wrapper.IsValid() && Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) &&
					Wrapper->GetWidth() == Wrapper->GetHeight() &&
					Wrapper->GetRaw(ERGBFormat::RGBA, 8, Raw))
				{
					const int32 N = (int32)Wrapper->GetWidth();
					if (Raw.Num() >= (int64)N * N * 4)
					{
						Cell->Size = N;
						Cell->Pixels.SetNumUninitialized((int64)N * N * 4);
						FMemory::Memcpy(Cell->Pixels.GetData(), Raw.GetData(),
							(int64)N * N * 4);
						++Loaded;
					}
				}
			}
			const FCell* Out = Cell.Get();
			Cells.Add(Key, MoveTemp(Cell));
			return Out;
		}

		FString Dir;
		float CellSizeM = 500.f;   // defaut si index.json est absent
		int32 Loaded = 0;
		TMap<FIntPoint, TUniquePtr<FCell>> Cells;
		int32 NoiseSize = 0;
		TArray<uint8> Noise;
	};

	// -----------------------------------------------------------------------------
	// v4 — LE PIETON EST LA DALLE. Verdict DA v3 : « grand puzzle ». Le coupable
	// n'etait pas la palette (deja reduite a 3 classes en v3) mais LE FOND : la dalle
	// urbaine restait a la teinte unie de J2, et les centaines de petits rubans
	// pietons du centre-ville s'y collaient comme des autocollants sur du papier, en
	// laissant voir le vide dans leurs interstices. La v4 renverse le probleme :
	//   - la DALLE recoit la matiere minerale (GSurfSlab), donc toute la ville repose
	//     sur un sol credible ;
	//   - les voies PIETONNES ne produisent plus AUCUN ruban (IsPedestrianRibbon,
	//     early-continue cote generation) : marcher, c'est marcher sur la dalle. Plus
	//     un seul interstice a boucher, plus un seul lacis a harmoniser.
	// Il ne reste donc en rubans que ce qui se DISTINGUE vraiment du sol de la ville :
	//   (a) CHAUSSEE AUTO — asphalt_road partout ; les fine_road_* (ligne axiale
	//       peinte dans le scan) restent reserves aux voies annoncees lanes >= 2 ;
	//   (b) ALLEE NATURELLE — gravel_on_soil sur tag surface non revetu (surtout les
	//       allees de parc, qui doivent se lire sur l'herbe).
	// Une rue pavee (sett, paving_stones...) NON pietonne est une chaussee : elle part
	// desormais en asphalte, plus en revetement pave — cobblestone, marked_rough_road
	// et herringbone_brick_pavement ne sont plus references nulle part.
	// WidthCm = largeur de chaussee du JSON (hors trottoirs).
	// -----------------------------------------------------------------------------

	// Voies dont le ruban est SUPPRIME en profil revetements : leur sol, c'est la
	// dalle. Un seul endroit de verite — la passe de releve des carrefours et la passe
	// de generation doivent skipper exactement les memes voies (sinon un noeud
	// purement pieton continuerait de peser sur la dominante d'un carrefour).
	bool IsPedestrianRibbon(const FString& Type)
	{
		return Type == TEXT("pedestrian") || Type == TEXT("footway") || Type == TEXT("path") ||
			Type == TEXT("sidewalk") || Type == TEXT("steps") || Type == TEXT("platform") ||
			Type == TEXT("track");
	}

	const FSurfaceClass* SurfaceClassForRoad(const FString& Surface, const FString& Type,
		int32 Lanes, float WidthCm)
	{
		// 1) Sol NATUREL annonce par la donnee : prime sur le type.
		if (Surface == TEXT("gravel") || Surface == TEXT("fine_gravel") ||
			Surface == TEXT("compacted") || Surface == TEXT("dirt") || Surface == TEXT("ground") ||
			Surface == TEXT("earth") || Surface == TEXT("unpaved") || Surface == TEXT("sand"))
		{
			return &GSurfGravel;
		}
		// 2) CHAUSSEE AUTO. Les scans a ligne axiale sont reserves aux voies dont la
		//    donnee annonce au moins 2 files ; le reste est de l'asphalte nu.
		if (Lanes >= 3 || (Lanes >= 2 && WidthCm >= 900.f))
		{
			return &GSurfRoadWide;
		}
		if (Lanes == 2)
		{
			return &GSurfRoadMedium;
		}
		return &GSurfAsphalt;
	}

	// Classe resolue : le pack + le materiau charge (nul = repli sur l'historique).
	struct FResolvedSurface
	{
		const FSurfaceClass* Class = nullptr;
		UMaterialInterface* Material = nullptr;

		FName SlotName() const { return FName(Class->Slug); }
	};

	// Cache de chargement des materiaux de revetement. L'ABSENCE d'un materiau n'est
	// PAS une erreur : le groupe est quand meme cree (geometrie et UV metriques
	// identiques) et CreateMeshAsset lui donne le materiau de repli du mesh.
	struct FSurfaceLibrary
	{
		void Init(bool bOn, const FString& InFolder)
		{
			bEnabled = bOn;
			Folder = InFolder.IsEmpty() ? TEXT("/Game/City/Surfaces") : InFolder;
		}

		// Rend nullptr si les revetements sont desactives (profil mobile) : tout
		// appelant retombe alors sur le chemin historique, a l'octet pres.
		// Entrees en TUniquePtr : les pointeurs rendus restent valides quand le cache
		// grandit (le rehash d'une TMap de valeurs les invaliderait).
		const FResolvedSurface* Resolve(const FSurfaceClass* Class)
		{
			if (!bEnabled || !Class)
			{
				return nullptr;
			}
			const FString Key(Class->Slug);
			if (TUniquePtr<FResolvedSurface>* Found = Resolved.Find(Key))
			{
				return Found->Get();
			}
			const FString Path = FString::Printf(TEXT("%s/%s/M_Surf_%s.M_Surf_%s"),
				*Folder, Class->Slug, Class->Slug, Class->Slug);
			TUniquePtr<FResolvedSurface> Entry = MakeUnique<FResolvedSurface>();
			Entry->Class = Class;
			Entry->Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr,
				LOAD_NoWarn | LOAD_Quiet);
			if (!Entry->Material)
			{
				// Display et NON Warning : le repli est un mode de fonctionnement
				// normal (tests sans assets Megascans, generation avant import) —
				// et l'automation eleve les warnings en erreurs de test.
				UE_LOG(LogCityImport, Display,
					TEXT("Revetement '%s' absent (%s) : repli sur le materiau historique."),
					Class->Slug, *Path);
			}
			const FResolvedSurface* Out = Entry.Get();
			Resolved.Add(Key, MoveTemp(Entry));
			return Out;
		}

	private:
		FString Folder;
		bool bEnabled = false;
		TMap<FString, TUniquePtr<FResolvedSurface>> Resolved;
	};

	// -----------------------------------------------------------------------------
	// v2 — CARREFOURS. Verdict utilisateur sur le proto v1 : « les revetements se
	// rencontrent sans harmonie, coupes franches, superpositions ». La cause est aux
	// noeuds : N rubans de classes differentes s'y empilent et les tirets axiaux
	// traversent le croisement. Parade en deux temps :
	//   1. un PATCH polygonal du revetement dominant recouvre le disque de rencontre ;
	//   2. les segments de ruban a moins de GJunctionPlainCm d'un noeud passent en
	//      asphalte NU (plus de tiret qui traverse le carrefour).
	// Un noeud est un carrefour s'il est partage par >= 3 routes, OU s'il est un point
	// INTERIEUR d'au moins une route (une route qui passe au travers). Deux routes qui
	// s'y terminent seulement = simple decoupage OSM d'une meme rue : ce n'est PAS un
	// carrefour, et y effacer les tirets creverait le marquage de tout un boulevard.
	// -----------------------------------------------------------------------------
	constexpr float GJunctionPlainCm = 800.f;  // rayon d'effacement des tirets (8 m)
	constexpr float GJunctionGridCm = 800.f;   // pas de la grille de recherche
	constexpr float GJunctionPatchMarginCm = 100.f; // rayon = max demi-largeur + 1 m
	constexpr float GJunctionPatchLiftCm = 5.f;     // patch pose au-dessus du ruban le plus haut
	// v5 point 4 — FRAGMENTS ORPHELINS. Verdict utilisateur sur la v4b : « morceaux
	// perdus » — des bouts de voie de quelques metres, sans aucun noeud commun avec le
	// reseau, poses seuls au milieu de la dalle uniforme (troncons OSM coupes par la
	// fenetre d'extraction, contre-allees, acces de parking). Un ruban court ET
	// deconnecte n'apporte rien : il ne raconte pas une rue, il salit le fond.
	constexpr float GOrphanMaxLenCm = 2500.f;
	// Grille de l'index des DISQUES DE PATCH (plus large que celle des noeuds : un
	// disque deborde de sa cellule). Chaque disque s'inscrit dans toutes les cellules
	// que touche sa boite englobante ELARGIE de GPatchSlackMaxCm, si bien qu'une
	// requete ne consulte qu'une seule cellule.
	constexpr float GPatchGridCm = 3200.f;
	constexpr float GPatchSlackMaxCm = 2000.f;

	struct FJunctionNode
	{
		FVector2D PosCm = FVector2D::ZeroVector;
		int32 FirstRoad = INDEX_NONE;
		int32 LastRoad = INDEX_NONE;
		int32 NumRoads = 0;
		int32 NumAutoRoads = 0;
		int32 NumInterior = 0;
		float MaxHalfCm = 0.f;
		float MaxZClassCm = 0.f;
		const FSurfaceClass* Dominant = nullptr;
		float DominantHalfCm = -1.f;

		// NumRoads >= 2 est une PRECONDITION : sans elle, chaque sommet interieur
		// d'une route SEULE passait pour un carrefour — mesure sur le proto v2 :
		// 3 042 « carrefours » sur 3 920 noeuds releves, soit un patch tous les
		// quelques metres et plus un seul tiret axial nulle part.
		bool IsJunction() const { return NumRoads >= 2 && (NumRoads >= 3 || NumInterior >= 1); }

		// v3 — un patch de carrefour n'a de sens qu'entre VOITURES. Verdict DA sur le
		// proto v2 : dans le lacis pieton du centre, chaque croisement de sentiers
		// posait son disque d'un autre revetement — « peau de leopard ». Condition :
		// la voie DOMINANTE est une chaussee auto ET au moins une AUTRE voie du noeud
		// l'est aussi (le disque recouvre alors une vraie zone de roulement).
		bool WantsPatch() const
		{
			return IsJunction() && Dominant && Dominant->bAuto && NumAutoRoads >= 2;
		}
	};

	struct FJunctionMap
	{
		// Quantification au decimetre : les noeuds partages viennent du MEME noeud OSM
		// et traversent la conversion a l'identique — le decimetre absorbe le bruit
		// d'arrondi du JSON (2 decimales de metre) sans fusionner deux vrais noeuds.
		static FIntPoint Key(const FVector2D& P)
		{
			return FIntPoint(FMath::RoundToInt(P.X / 10.f), FMath::RoundToInt(P.Y / 10.f));
		}

		void Add(int32 RoadIndex, const TArray<FVector2D>& PtsCm, float HalfCm,
			const FSurfaceClass* Class)
		{
			for (int32 i = 0; i < PtsCm.Num(); ++i)
			{
				FJunctionNode& Node = Nodes.FindOrAdd(Key(PtsCm[i]));
				bool bNewRoadHere = false;
				if (Node.NumRoads == 0)
				{
					Node.PosCm = PtsCm[i];
					Node.FirstRoad = RoadIndex;
					Node.NumRoads = 1;
					bNewRoadHere = true;
				}
				else if (Node.FirstRoad != RoadIndex && Node.LastRoad != RoadIndex)
				{
					++Node.NumRoads;
					bNewRoadHere = true;
				}
				// v3 : compte des CHAUSSEES AUTO distinctes au noeud (meme regle de
				// dedoublonnage que NumRoads) — filtre des patchs de carrefour.
				if (bNewRoadHere && Class && Class->bAuto)
				{
					++Node.NumAutoRoads;
				}
				Node.LastRoad = RoadIndex;
				if (i > 0 && i + 1 < PtsCm.Num())
				{
					++Node.NumInterior;
				}
				Node.MaxHalfCm = FMath::Max(Node.MaxHalfCm, HalfCm);
				if (Class)
				{
					Node.MaxZClassCm = FMath::Max(Node.MaxZClassCm, Class->ZClassCm);
					// Dominante = la voie la plus LARGE (donc la plus prioritaire) ;
					// a egalite, la premiere rencontree (deterministe : l'ordre du JSON).
					if (HalfCm > Node.DominantHalfCm)
					{
						Node.DominantHalfCm = HalfCm;
						Node.Dominant = Class;
					}
				}
			}
		}

		// Index spatial des SEULS vrais carrefours, construit une fois la collecte finie.
		// v5 : le meme passage remplit l'index des DISQUES DE PATCH — l'emprise ou la
		// bordure s'interrompt et ou un passage pieton est reporte. Meme condition
		// EXACTE que la passe de generation des patchs (WantsPatch + demi-largeur
		// mini) : un seul endroit de verite, sinon la bordure se couperait la ou aucun
		// disque n'est pose.
		void Build()
		{
			for (TPair<FIntPoint, FJunctionNode>& Pair : Nodes)
			{
				if (Pair.Value.IsJunction())
				{
					Grid.FindOrAdd(FIntPoint(FMath::FloorToInt(Pair.Value.PosCm.X / GJunctionGridCm),
						FMath::FloorToInt(Pair.Value.PosCm.Y / GJunctionGridCm))).Add(Pair.Value.PosCm);
					++NumJunctions;
					if (Pair.Value.WantsPatch())
					{
						++NumAutoJunctions;
						if (Pair.Value.MaxHalfCm >= 150.f)
						{
							AddPatchDisc(Pair.Value.PosCm, Pair.Value.MaxHalfCm + GJunctionPatchMarginCm);
						}
					}
				}
			}
		}

		// Disque de patch reellement pose (centre + rayon), pour le decoupage de la
		// bordure et le report des passages pietons.
		struct FPatchDisc
		{
			FVector2D PosCm = FVector2D::ZeroVector;
			float RadiusCm = 0.f;
		};

		void AddPatchDisc(const FVector2D& P, float RadiusCm)
		{
			const float Reach = RadiusCm + GPatchSlackMaxCm;
			const int32 X0 = FMath::FloorToInt((P.X - Reach) / GPatchGridCm);
			const int32 X1 = FMath::FloorToInt((P.X + Reach) / GPatchGridCm);
			const int32 Y0 = FMath::FloorToInt((P.Y - Reach) / GPatchGridCm);
			const int32 Y1 = FMath::FloorToInt((P.Y + Reach) / GPatchGridCm);
			const FPatchDisc Disc{ P, RadiusCm };
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					PatchGrid.FindOrAdd(FIntPoint(X, Y)).Add(Disc);
				}
			}
			++NumPatchDiscs;
		}

		// Le point est-il couvert par un disque de patch (SlackCm = marge d'approche) ?
		// Les disques etant inscrits dans toutes les cellules de leur boite elargie de
		// GPatchSlackMaxCm, une SEULE cellule suffit tant que SlackCm reste sous cette
		// borne — le clamp evite un faux negatif silencieux si un appelant la depasse.
		bool IsInPatch(const FVector2D& P, float SlackCm = 0.f) const
		{
			const float Slack = FMath::Min(SlackCm, GPatchSlackMaxCm);
			const TArray<FPatchDisc>* Cell = PatchGrid.Find(
				FIntPoint(FMath::FloorToInt(P.X / GPatchGridCm), FMath::FloorToInt(P.Y / GPatchGridCm)));
			if (!Cell)
			{
				return false;
			}
			for (const FPatchDisc& D : *Cell)
			{
				const float R = D.RadiusCm + Slack;
				if (FVector2D::DistSquared(P, D.PosCm) <= R * R)
				{
					return true;
				}
			}
			return false;
		}

		bool IsNear(const FVector2D& P, float RadiusCm) const
		{
			const int32 GX = FMath::FloorToInt(P.X / GJunctionGridCm);
			const int32 GY = FMath::FloorToInt(P.Y / GJunctionGridCm);
			const float R2 = RadiusCm * RadiusCm;
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dx = -1; dx <= 1; ++dx)
				{
					if (const TArray<FVector2D>* Cell = Grid.Find(FIntPoint(GX + dx, GY + dy)))
					{
						for (const FVector2D& Q : *Cell)
						{
							if (FVector2D::DistSquared(P, Q) <= R2)
							{
								return true;
							}
						}
					}
				}
			}
			return false;
		}

		TMap<FIntPoint, FJunctionNode> Nodes;
		TMap<FIntPoint, TArray<FVector2D>> Grid;
		TMap<FIntPoint, TArray<FPatchDisc>> PatchGrid;
		int32 NumJunctions = 0;
		int32 NumAutoJunctions = 0; // v3 : ceux qui recoivent reellement un patch
		int32 NumPatchDiscs = 0;    // v5 : disques indexes (== patchs poses)
	};

	// Patch de carrefour : disque du revetement dominant, pose AU-DESSUS de tous les
	// rubans du noeud, UV0 monde en metres (jamais de ligne axiale au milieu d'un
	// croisement). Le triangle-fan part du centre : 16 secteurs suffisent a un disque
	// de 5-15 m vu depuis un drone.
	void BuildJunctionPatch(FCityMeshBuilder& QM, const FVector2D& CenterCm, float RadiusCm,
		float Zcm, const FResolvedSurface* Surf, const FVector3f& Tint)
	{
		constexpr int32 Sides = 16;
		const FVector3f Up(0, 0, 1);
		const FPolygonGroupID Group = QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material);
		auto At = [&](int32 i)
		{
			const float Ang = 2.f * PI * i / Sides;
			return FVector3f((float)CenterCm.X + RadiusCm * FMath::Cos(Ang),
				(float)CenterCm.Y + RadiusCm * FMath::Sin(Ang), Zcm);
		};
		const FVector3f C((float)CenterCm.X, (float)CenterCm.Y, Zcm);
		for (int32 i = 0; i < Sides; ++i)
		{
			const FVector3f P[3] = { C, At(i), At(i + 1) };
			const FVector2f UV[3] = {
				FVector2f(P[0].X * 0.01f, P[0].Y * 0.01f),
				FVector2f(P[1].X * 0.01f, P[1].Y * 0.01f),
				FVector2f(P[2].X * 0.01f, P[2].Y * 0.01f) };
			QM.AddPoly(Group, P, 3, Up, UV, Tint);
		}
	}

	// -----------------------------------------------------------------------------
	// v5 « VOIRIE » — RIVES ET BORDURES d'un segment de chaussee. Pour chaque cote :
	//   1. la FACE de bordure, quad VERTICAL de 12 cm tourne vers la chaussee (c'est
	//      elle qui prend la lumiere autrement que le sol : la rue se lit) ;
	//   2. le CHANT, bande horizontale de 15 cm au sommet de la bordure ;
	//   3. la RIVE, 1,70 m de classe DALLE au meme niveau que le chant.
	// Chant et rive portent une UV0 MONDE en metres, exactement comme la dalle qui les
	// porte : le scan y est EN PHASE avec le fond de ville, la rive prolonge le sol au
	// lieu d'y coller un rectangle.
	// Junctions : sur l'emprise d'un patch de carrefour, la structure laterale
	// s'efface (le disque du patch EST la zone de roulement — une bordure la
	// traverserait de part en part). Le decoupage ne se sous-divise qu'au VOISINAGE
	// d'un patch : ailleurs, un quad par segment et par bande, comme avant.
	// -----------------------------------------------------------------------------
	void BuildStreetSides(FCityMeshBuilder& QM, const FVector2D& A, const FVector2D& B,
		const FVector2D& NrmA, const FVector2D& NrmB, float ZA, float ZB, float Arc, float SegLen,
		float RoadHalfCm, const FResolvedSurface* SurfCurb, const FResolvedSurface* SurfSlab,
		const FVector3f& Tint, const FJunctionMap* Junctions, int32* OutCurbQuads)
	{
		if (SegLen < 1.f)
		{
			return;
		}
		const FVector3f Up(0, 0, 1);
		const FPolygonGroupID CurbGroup = QM.GetOrCreateGroup(SurfCurb->SlotName(), SurfCurb->Material);
		const FPolygonGroupID SlabGroup = QM.GetOrCreateGroup(SurfSlab->SlotName(), SurfSlab->Material);

		// Sous-decoupe UNIQUEMENT si un disque de patch est a portee du segment.
		int32 Sub = 1;
		const bool bClip = Junctions && Junctions->NumPatchDiscs > 0 &&
			(Junctions->IsInPatch(A, SegLen) || Junctions->IsInPatch(B, SegLen));
		if (bClip)
		{
			Sub = FMath::Clamp(FMath::CeilToInt(SegLen / GCurbClipStepCm), 1, 32);
		}
		for (int32 s = 0; s < Sub; ++s)
		{
			const float T0 = (float)s / Sub;
			const float T1 = (float)(s + 1) / Sub;
			const FVector2D S0 = FMath::Lerp(A, B, T0);
			const FVector2D S1 = FMath::Lerp(A, B, T1);
			if (bClip && Junctions->IsInPatch(FMath::Lerp(S0, S1, 0.5f)))
			{
				continue;
			}
			const FVector2D N0 = FMath::Lerp(NrmA, NrmB, T0);
			const FVector2D N1 = FMath::Lerp(NrmA, NrmB, T1);
			const float Z0 = FMath::Lerp(ZA, ZB, T0);
			const float Z1 = FMath::Lerp(ZA, ZB, T1);
			const float U0 = (Arc + SegLen * T0) * 0.01f;
			const float U1 = (Arc + SegLen * T1) * 0.01f;
			// Side = +1 / -1 : les deux rives, symetriques par rapport a l'axe.
			for (int32 Side = -1; Side <= 1; Side += 2)
			{
				auto At = [&](const FVector2D& S, const FVector2D& Nl, float Z, float Lateral, float Lift)
				{
					const FVector2D P = S + Nl * (Side * Lateral);
					return FVector3f((float)P.X, (float)P.Y, Z + Lift);
				};
				auto WorldUV = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };

				// 1. FACE de bordure : quad vertical, normale vers l'axe de la rue.
				const FVector3f F[4] = {
					At(S0, N0, Z0, RoadHalfCm, 0.f),
					At(S1, N1, Z1, RoadHalfCm, 0.f),
					At(S1, N1, Z1, RoadHalfCm, GCurbHeightCm),
					At(S0, N0, Z0, RoadHalfCm, GCurbHeightCm) };
				const FVector2f FUV[4] = {
					FVector2f(U0, 0.f), FVector2f(U1, 0.f),
					FVector2f(U1, GCurbHeightCm * 0.01f), FVector2f(U0, GCurbHeightCm * 0.01f) };
				const FVector2D Inward = -N0 * (float)Side;
				QM.AddPoly(CurbGroup, F, 4,
					FVector3f((float)Inward.X, (float)Inward.Y, 0.f).GetSafeNormal(), FUV, Tint);

				// 2. CHANT : bande horizontale de 15 cm au sommet de la bordure.
				const FVector3f C[4] = {
					At(S0, N0, Z0, RoadHalfCm, GCurbHeightCm),
					At(S1, N1, Z1, RoadHalfCm, GCurbHeightCm),
					At(S1, N1, Z1, RoadHalfCm + GCurbTopWidthCm, GCurbHeightCm),
					At(S0, N0, Z0, RoadHalfCm + GCurbTopWidthCm, GCurbHeightCm) };
				const FVector2f CUV[4] = { WorldUV(C[0]), WorldUV(C[1]), WorldUV(C[2]), WorldUV(C[3]) };
				QM.AddPoly(CurbGroup, C, 4, Up, CUV, Tint);

				// 3. RIVE : 1,70 m de dalle, de plain-pied avec le chant.
				const FVector3f W[4] = {
					At(S0, N0, Z0, RoadHalfCm + GCurbTopWidthCm, GCurbHeightCm),
					At(S1, N1, Z1, RoadHalfCm + GCurbTopWidthCm, GCurbHeightCm),
					At(S1, N1, Z1, RoadHalfCm + GCurbTopWidthCm + GSidewalkWidthCm, GCurbHeightCm),
					At(S0, N0, Z0, RoadHalfCm + GCurbTopWidthCm + GSidewalkWidthCm, GCurbHeightCm) };
				const FVector2f WUV[4] = { WorldUV(W[0]), WorldUV(W[1]), WorldUV(W[2]), WorldUV(W[3]) };
				QM.AddPoly(SlabGroup, W, 4, Up, WUV, Tint);

				if (OutCurbQuads)
				{
					*OutCurbQuads += 2; // face + chant
				}
			}
		}
	}

	// PASSAGE PIETON (v5 point 2). Les voies pietonnes vivent dans la DONNEE meme si
	// elles ne produisent plus de ruban : la ou l'une d'elles partage un noeud avec une
	// chaussee auto, il y avait un passage dans la vraie ville. On y pose un quad du
	// scan pedestrian_crossing_lines, EN TRAVERS de la chaussee seule (jamais sur les
	// rives : un passage ne monte pas sur le trottoir), aligne sur l'axe de la rue.
	// UV : U = travers de la rue en metres (le scan repete ses bandes de 50 cm tous les
	// 4 m : bandes PARALLELES a l'axe de la chaussee, norme francaise) ; V = 1 -> 3 m,
	// soit UNE tuile du scan calee pour que son trait blanc tombe exactement sur les
	// deux bords du passage plutot qu'en son milieu.
	void BuildCrossing(FCityMeshBuilder& QM, const FVector2D& CenterCm, const FVector2D& DirCm,
		float RoadHalfCm, float Zcm, const FResolvedSurface* Surf, const FVector3f& Tint)
	{
		const FVector2D D = DirCm.GetSafeNormal();
		if (D.IsNearlyZero())
		{
			return;
		}
		const FVector2D Lat(-D.Y, D.X);
		const FVector2D Along = D * GCrossingHalfLenCm;
		const FVector2D Across = Lat * RoadHalfCm;
		auto At = [&](float SAlong, float SAcross)
		{
			const FVector2D P = CenterCm + Along * SAlong + Across * SAcross;
			return FVector3f((float)P.X, (float)P.Y, Zcm);
		};
		const FVector3f P[4] = { At(-1.f, -1.f), At(1.f, -1.f), At(1.f, 1.f), At(-1.f, 1.f) };
		const float AcrossM = RoadHalfCm * 0.01f;
		const FVector2f UV[4] = {
			FVector2f(-AcrossM, 1.f), FVector2f(-AcrossM, 3.f),
			FVector2f(AcrossM, 3.f), FVector2f(AcrossM, 1.f) };
		QM.AddPoly(QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material), P, 4,
			FVector3f(0, 0, 1), UV, Tint);
	}

	// -----------------------------------------------------------------------------
	// J3c maquette — BORDURE le long d'une polyligne de masque. Meme vocabulaire
	// geometrique que BuildStreetSides (face verticale de 12 cm + chant de 15 cm),
	// mais la polyligne n'est plus un AXE de route : c'est la FRONTIERE elle-meme,
	// deja orientee au prep chaussee A GAUCHE. Un troisieme quad ferme la bordure
	// cote trottoir.
	//
	// ASSUME : le sol etant PEINT, il est plan des deux cotes de la bordure — le
	// trottoir n'est pas surhausse. La bordure est donc une PIERRE POSEE (12 cm de
	// relief, 15 cm de chant) et non une marche. C'est ce qui donne la lecture des
	// rives depuis le ciel (deux aretes eclairees differemment) au prix d'un
	// trottoir qui, au ras du sol, est de plain-pied avec la chaussee. Surhausser
	// le trottoir demanderait de deformer la dalle : hors perimetre de la maquette.
	//
	// V3 : le PROFIL est parametre (hauteur de face, largeur de chant). Les valeurs
	// par defaut sont la bordure de chaussee historique (12 / 15 cm) ; la bordurette
	// d'herbe passe le profil reduit GGrassCurb* (7 / 14 cm). Le reste — sens de
	// parcours (mineral A GAUCHE), pied enterre, UV monde du chant, dos visible — est
	// rigoureusement le meme code : c'est ce qui garantit que les deux pierres se
	// lisent comme la meme matiere posee de la meme facon.
	// v4 — DIAGNOSTIC D'ENTERREMENT, mesure sur la vraie donnee (aucun chiffre
	// magique) : combien de sommets de bordure tombent SOUS la dalle rendue, et de
	// combien. Rempli par BuildMaskCurb, journalise en fin de passe.
	struct FCurbSinkStats
	{
		int32 Vertices = 0;
		int32 Over7cm = 0;    // la bordurette d'herbe (7 cm) serait invisible
		int32 Over12cm = 0;   // la bordure de chaussee (12 cm) serait invisible
		float MaxCm = 0.f;
		double SumCm = 0.0;
		int32 AddedVertices = 0;   // sommets ajoutes par le decoupage adaptatif

		void Note(float DeltaCm)
		{
			++Vertices;
			if (DeltaCm > 0.f)
			{
				SumCm += DeltaCm;
				MaxCm = FMath::Max(MaxCm, DeltaCm);
				if (DeltaCm > GGrassCurbHeightCm) { ++Over7cm; }
				if (DeltaCm > GCurbHeightCm) { ++Over12cm; }
			}
		}
	};

	// v4 : le Z ne vient plus du MNT continu mais de la SURFACE RENDUE (FRenderedGroundZ),
	// et la polyligne est decoupee adaptativement pour epouser les facettes de la dalle.
	// Les deux profils (bordure de chaussee ET bordurette d'herbe) passent par ici :
	// la capture utilisateur du 01/08 montre que la chaussee est enterree elle aussi.
	// bCaps : bouchons de fin sur un profil OUVERT (sans eux la pierre se lit comme une
	// tranche de carton vue par la tranche).
	void BuildMaskCurb(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCmIn,
		const FRenderedGroundZ& RGZ, const FResolvedSurface* Surf, const FVector3f& Tint,
		int32* OutQuads, float HeightCm = GCurbHeightCm, float TopWidthCm = GCurbTopWidthCm,
		FCurbSinkStats* Stats = nullptr, bool bCaps = true)
	{
		if (!Surf || PtsCmIn.Num() < 2)
		{
			return;
		}
		TArray<FVector2D> PtsCm;
		SubdivideOnRenderedGround(PtsCmIn, RGZ, GCurbSagToleranceCm, GCurbSagMaxDepth, PtsCm);
		if (Stats)
		{
			Stats->AddedVertices += FMath::Max(0, PtsCm.Num() - PtsCmIn.Num());
			for (const FVector2D& P : PtsCmIn)
			{
				Stats->Note(RGZ.At(P.X, P.Y) - RGZ.RawZ(P.X, P.Y));
			}
		}
		const FVector3f Up(0, 0, 1);
		const FPolygonGroupID Group = QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material);
		auto WorldUV = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };
		float Arc = 0.f;
		int32 Posed = 0;
		FVector3f CapA[4];
		FVector3f CapB[4];
		FVector3f CapANormal(1.f, 0.f, 0.f);
		FVector3f CapBNormal(1.f, 0.f, 0.f);
		bool bHasFirst = false;
		for (int32 i = 0; i + 1 < PtsCm.Num(); ++i)
		{
			const FVector2D A = PtsCm[i];
			const FVector2D B = PtsCm[i + 1];
			const FVector2D D = B - A;
			const float SegLen = (float)D.Size();
			if (SegLen < 1.f)
			{
				continue;
			}
			const FVector2D Dir = D / SegLen;
			// Chaussee A GAUCHE : la normale qui pointe VERS la chaussee est
			// (-Dir.Y, Dir.X), le trottoir est du cote oppose.
			const FVector2D ToRoad(-Dir.Y, Dir.X);
			const FVector2D ToWalk = -ToRoad;
			const float ZA = RGZ.At(A.X, A.Y);
			const float ZB = RGZ.At(B.X, B.Y);
			auto At = [&](const FVector2D& P, float Z, const FVector2D& Lat, float Off, float Lift)
			{
				return FVector3f((float)(P.X + Lat.X * Off), (float)(P.Y + Lat.Y * Off), Z + Lift);
			};
			const float U0 = Arc * 0.01f;
			const float U1 = (Arc + SegLen) * 0.01f;
			Arc += SegLen;

			// 1. FACE cote chaussee (celle qui prend la lumiere rasante).
			const FVector3f F[4] = {
				At(A, ZA, ToRoad, 0.f, -GMaskCurbSinkCm),
				At(B, ZB, ToRoad, 0.f, -GMaskCurbSinkCm),
				At(B, ZB, ToRoad, 0.f, HeightCm),
				At(A, ZA, ToRoad, 0.f, HeightCm) };
			const FVector2f FUV[4] = {
				FVector2f(U0, 0.f), FVector2f(U1, 0.f),
				FVector2f(U1, (HeightCm + GMaskCurbSinkCm) * 0.01f),
				FVector2f(U0, (HeightCm + GMaskCurbSinkCm) * 0.01f) };
			QM.AddPoly(Group, F, 4,
				FVector3f((float)ToRoad.X, (float)ToRoad.Y, 0.f).GetSafeNormal(), FUV, Tint);

			// 2. CHANT horizontal, 15 cm vers le trottoir. UV MONDE : le motif reste
			//    en phase avec la dalle qui le porte (la bordure est de la meme
			//    matiere, juste assombrie).
			const FVector3f C[4] = {
				At(A, ZA, ToRoad, 0.f, HeightCm),
				At(B, ZB, ToRoad, 0.f, HeightCm),
				At(B, ZB, ToWalk, TopWidthCm, HeightCm),
				At(A, ZA, ToWalk, TopWidthCm, HeightCm) };
			const FVector2f CUV[4] = { WorldUV(C[0]), WorldUV(C[1]), WorldUV(C[2]), WorldUV(C[3]) };
			QM.AddPoly(Group, C, 4, Up, CUV, Tint);

			// 3. FACE cote trottoir : sans elle la bordure serait un plan sans dos,
			//    invisible depuis le trottoir.
			const FVector3f W[4] = {
				At(B, ZB, ToWalk, TopWidthCm, HeightCm),
				At(A, ZA, ToWalk, TopWidthCm, HeightCm),
				At(A, ZA, ToWalk, TopWidthCm, -GMaskCurbSinkCm),
				At(B, ZB, ToWalk, TopWidthCm, -GMaskCurbSinkCm) };
			const FVector2f WUV[4] = {
				FVector2f(U1, 0.f), FVector2f(U0, 0.f),
				FVector2f(U0, (HeightCm + GMaskCurbSinkCm) * 0.01f),
				FVector2f(U1, (HeightCm + GMaskCurbSinkCm) * 0.01f) };
			QM.AddPoly(Group, W, 4,
				FVector3f((float)ToWalk.X, (float)ToWalk.Y, 0.f).GetSafeNormal(), WUV, Tint);

			if (OutQuads)
			{
				*OutQuads += 3;
			}
			++Posed;
			// v4 — memoire des sections extremes pour les bouchons de fin.
			if (!bHasFirst)
			{
				bHasFirst = true;
				CapA[0] = At(A, ZA, ToRoad, 0.f, -GMaskCurbSinkCm);
				CapA[1] = At(A, ZA, ToWalk, TopWidthCm, -GMaskCurbSinkCm);
				CapA[2] = At(A, ZA, ToWalk, TopWidthCm, HeightCm);
				CapA[3] = At(A, ZA, ToRoad, 0.f, HeightCm);
				CapANormal = FVector3f(-(float)Dir.X, -(float)Dir.Y, 0.f).GetSafeNormal();
			}
			CapB[0] = At(B, ZB, ToRoad, 0.f, -GMaskCurbSinkCm);
			CapB[1] = At(B, ZB, ToRoad, 0.f, HeightCm);
			CapB[2] = At(B, ZB, ToWalk, TopWidthCm, HeightCm);
			CapB[3] = At(B, ZB, ToWalk, TopWidthCm, -GMaskCurbSinkCm);
			CapBNormal = FVector3f((float)Dir.X, (float)Dir.Y, 0.f).GetSafeNormal();
		}
		// v4 — BOUCHONS DE FIN. Une bordurette est une piece OUVERTE : sans bouchon,
		// ses deux extremites sont des trous par lesquels on voit l'interieur de la
		// pierre. Deux quads pour toute la polyligne, quelle que soit sa longueur.
		if (bCaps && bHasFirst && Posed > 0)
		{
			const FVector2f CapUV[4] = {
				FVector2f(0.f, 0.f), FVector2f(TopWidthCm * 0.01f, 0.f),
				FVector2f(TopWidthCm * 0.01f, (HeightCm + GMaskCurbSinkCm) * 0.01f),
				FVector2f(0.f, (HeightCm + GMaskCurbSinkCm) * 0.01f) };
			QM.AddPoly(Group, CapA, 4, CapANormal, CapUV, Tint);
			QM.AddPoly(Group, CapB, 4, CapBNormal, CapUV, Tint);
			if (OutQuads)
			{
				*OutQuads += 2;
			}
		}
	}

	// -----------------------------------------------------------------------------
	// C1 — POSE D'UN MUR DE SOUTENEMENT le long d'une breakline.
	//
	// Pour chaque sommet on MESURE la rampe sur la surface rendue, des deux cotes de
	// la ligne, en marchant le long de la normale :
	//   - vers le BAS : on avance tant que la surface DESCEND ; le premier palier est
	//     le PIED de la rampe (Zpied, distance Dpied) ;
	//   - vers le HAUT : on avance tant qu'elle MONTE ; le premier palier est la
	//     CRETE (Zcrete, distance Dcrete).
	// Le mur est alors : face verticale au pied, de Zpied - enfoncement a Zcrete ;
	// couronnement horizontal a Zcrete, du pied jusqu'a la crete ; dos au droit de la
	// crete. La piece se referme donc EXACTEMENT sur le sol de part et d'autre.
	//
	// Le sens de parcours ne porte AUCUNE convention implicite : c'est CoteBas qui dit
	// de quel cote est le bas (side-car), et on n'en deduit rien d'autre.
	struct FWallSection
	{
		FVector2D Pied = FVector2D::ZeroVector;   // point du pied, cm
		FVector2D Crete = FVector2D::ZeroVector;  // point de la crete, cm
		float ZPied = 0.f;
		float ZCrete = 0.f;
		bool bValide = false;
	};

	/**
	 * LOT BERGES — LA GEOMETRIE RETENUE, RENDUE AU LOG, MUR PAR MUR.
	 * « Les murs prennent trop de place » est un grief de GEOMETRIE : il ne se juge
	 * ni sur un compte de murs, ni sur un compte de quads. Sans cette sortie
	 * nominative, la seule facon de connaitre l'emprise reelle d'un mur pose etait
	 * de la re-simuler hors moteur. Une ligne par mur, en Display.
	 */
	struct FWallGeom
	{
		float OffFootCm = 0.f;
		float OffCrestCm = 0.f;
		float HMedCm = 0.f;
		float LenM = 0.f;
	};

	/**
	 * LOT BERGES — jusqu'ou la MARCHE monte, sur la DONNEE D'ALTITUDE.
	 *
	 * On avance le long de -NormLow (vers le haut) et on rend la distance du PREMIER
	 * PALIER du MNT : |dZ| sur une base de 2 m < GWallPlateauSlope. C'est la largeur
	 * REELLE du cote haut de la marche — celle que le drape etale ensuite sur un quad.
	 * Rend MaxCm si aucun palier n'est trouve (on ne borne alors rien).
	 */
	float CrestPlateauDistCm(const FVector2D& PCm, const FVector2D& NormLow,
		const FRenderedGroundZ& RGZ, float MaxCm, bool bVersLeBas = false)
	{
		if (!RGZ.Drape || !RGZ.Drape->IsActive())
		{
			return MaxCm;
		}
		// MUR35 : le MEME sondage sert aux deux cotes — vers le haut (crete, lot
		// BERGES) ou vers le BAS (pied). Une seule regle, un seul code.
		const FVector2D Up = bVersLeBas ? NormLow : -NormLow;
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(MaxCm / GWallProbeStepCm));
		for (int32 i = 1; i <= Steps; ++i)
		{
			const float D = (float)i * GWallProbeStepCm;
			const FVector2D Q = PCm + Up * (double)D;
			const FVector2D A = Q - Up * (double)GWallPlateauBaseCm;
			const FVector2D B = Q + Up * (double)GWallPlateauBaseCm;
			const float Pente = FMath::Abs(RGZ.RawZ(B.X, B.Y) - RGZ.RawZ(A.X, A.Y))
				/ (2.f * GWallPlateauBaseCm);
			if (Pente < GWallPlateauSlope)
			{
				return FMath::Min(D, MaxCm);
			}
		}
		return MaxCm;
	}

	FWallSection MeasureWallSection(const FVector2D& PCm, const FVector2D& NormLow,
		const FRenderedGroundZ& RGZ, float QuadCm, float GuardCm, bool bCrestOnPlateau,
		bool bFootOnPlateau = false, float FootMinCm = 100.f)
	{
		FWallSection S;
		const float Span = FMath::Max(QuadCm * GWallProbeSpanQuads, GWallProbeStepCm);
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Span / GWallProbeStepCm));
		// BERGES : borne du cote crete, lue sur la DONNEE D'ALTITUDE.
		const float CrestMaxCm = bCrestOnPlateau
			? FMath::Max(GWallCrestMinCm, CrestPlateauDistCm(PCm, NormLow, RGZ, Span))
			: Span;
		// MUR35 : MEME borne du cote PIED — le palier BAS de la marche. Mesure :
		// la jupe du pied vaut a elle seule p50 6,50 m contre 2,00 m pour la crete.
		const float FootMaxCm = bFootOnPlateau
			? FMath::Max(FootMinCm, CrestPlateauDistCm(PCm, NormLow, RGZ, Span, /*bVersLeBas*/ true))
			: Span;

		// PIED : on descend tant que ca descend, sans depasser le palier BAS.
		S.Pied = PCm;
		S.ZPied = RGZ.At(PCm.X, PCm.Y);
		for (int32 i = 1; i <= Steps; ++i)
		{
			const float D = (float)i * GWallProbeStepCm;
			if (D > FootMaxCm)
			{
				break;
			}
			const FVector2D Q = PCm + NormLow * (double)D;
			const float Z = RGZ.At(Q.X, Q.Y);
			if (Z > S.ZPied - GWallLevelTolCm)
			{
				break;   // la surface ne descend plus : on est au palier bas
			}
			S.Pied = Q;
			S.ZPied = Z;
		}
		// CRETE : on monte tant que ca monte — sans jamais depasser le palier de la
		// MARCHE REELLE (BERGES). Au-dela, c'est la terrasse haute : elle appartient
		// au SOL, pas au couronnement.
		S.Crete = PCm;
		S.ZCrete = RGZ.At(PCm.X, PCm.Y);
		for (int32 i = 1; i <= Steps; ++i)
		{
			const float D = (float)i * GWallProbeStepCm;
			if (D > CrestMaxCm)
			{
				break;
			}
			const FVector2D Q = PCm - NormLow * (double)D;
			const float Z = RGZ.At(Q.X, Q.Y);
			if (Z < S.ZCrete + GWallLevelTolCm)
			{
				break;   // la surface ne monte plus : on est au palier haut
			}
			S.Crete = Q;
			S.ZCrete = Z;
		}
		const float H = S.ZCrete - S.ZPied;
		S.bValide = (H >= GWallMinHeightCm) && (GuardCm <= 0.f || H <= GuardCm * GWallHeightGuard);
		return S;
	}

	// Rend le nombre de quads poses. 0 = la surface rendue ne presente aucune rampe
	// a masquer ici (cas NORMAL sur un mur que le relief a deja avale).
	//
	// v2 (02/08) — UN MUR EST UNE PIECE CONTINUE. Correctif vu SUR CAPTURE, pas
	// deduit : la v1 cherchait le pied et la crete SEGMENT PAR SEGMENT le long
	// d'une normale qui saute a chaque sommet, et SAUTAIT tout segment dont la
	// section echouait. Resultat photographie aux quais de Saint-Pierre : une
	// rangee d'ECRANS decales lateralement, troues, par lesquels on voyait
	// justement la rampe qu'on venait masquer. Trois changements, tous dans le
	// sens de la continuite :
	//   1. NORMALES DE SOMMET (moyenne des deux segments adjacents) : deux quads
	//      consecutifs partagent EXACTEMENT leur arete ;
	//   2. OFFSETS MEDIANS pour toute la polyligne : on marche a chaque sommet mais
	//      on retient la MEDIANE des distances — une seule geometrie laterale pour
	//      tout le mur. Le Z, lui, reste lu sommet par sommet : le mur suit le
	//      terrain en hauteur sans se disloquer en plan ;
	//   3. VALIDATION AU NIVEAU DE LA POLYLIGNE : aucun segment n'est plus saute ;
	//      la ou la marche s'eteint, le mur se PINCE a hauteur nulle.
	int32 BuildRetainingWall(FCityMeshBuilder& QM, const FRetainingWall& Wall,
		const FRenderedGroundZ& RGZ, const FResolvedSurface* Surf, const FVector3f& Tint,
		float QuadCm, bool bTiers, int32& OutTiers,
		const TArray<FGradinEmprise>* Emprises = nullptr, float* OutTierM = nullptr,
		bool bCrestOnPlateau = true, FWallGeom* OutGeom = nullptr,
		bool bNoFlip = true, bool bCapsRealEndsOnly = true, int32* OutCaps = nullptr,
		int32* OutFlipsFixed = nullptr, bool bFootOnPlateau = false,
		float FootMinCm = 100.f)
	{
		OutTiers = 0;
		if (!Surf || Wall.PtsCm.Num() < 2)
		{
			return 0;
		}
		// Le mur suit la dalle RENDUE : meme decoupage adaptatif que les bordures,
		// sinon la face verticale coupe les facettes de la grille en biais.
		TArray<FVector2D> Pts;
		SubdivideOnRenderedGround(Wall.PtsCm, RGZ, GCurbSagToleranceCm, GCurbSagMaxDepth, Pts);
		// FINITION QUAIS : LA FRONTIERE DE L'EMPRISE DOIT TOMBER SUR UN SOMMET.
		// Sans cela l'etendue gradinee est quantifiee au pas de la subdivision (le
		// quad de sol rendu, ~7,8 m) et deborde de l'emprise d'un DEMI-SEGMENT a
		// chaque bout — mesure : 43,0 + 39,0 m poses pour 32,7 + 34,1 m d'emprise.
		// On insere donc le point de traversee, trouve par dichotomie (14 iterations
		// = moins d'un millimetre sur 8 m). Rien d'autre ne change : la face, le
		// couronnement et le dos suivent la meme polyligne qu'avant, un sommet de
		// plus par frontiere.
		if (bTiers && Emprises && Emprises->Num() > 0 && Pts.Num() >= 2)
		{
			TArray<FVector2D> Fins;
			Fins.Reserve(Pts.Num() + 4);
			for (int32 i = 0; i + 1 < Pts.Num(); ++i)
			{
				Fins.Add(Pts[i]);
				const bool bA = DansUneEmprise(*Emprises, Pts[i]);
				const bool bB = DansUneEmprise(*Emprises, Pts[i + 1]);
				if (bA == bB)
				{
					continue;
				}
				double Lo = 0.0, Hi = 1.0;
				for (int32 k = 0; k < 14; ++k)
				{
					const double Mid = 0.5 * (Lo + Hi);
					const FVector2D P = Pts[i] + (Pts[i + 1] - Pts[i]) * Mid;
					if (DansUneEmprise(*Emprises, P) == bA)
					{
						Lo = Mid;
					}
					else
					{
						Hi = Mid;
					}
				}
				const FVector2D Cut = Pts[i] + (Pts[i + 1] - Pts[i]) * (0.5 * (Lo + Hi));
				// Deux sommets confondus ne serviraient a rien et feraient un segment
				// degenere : on n'insere que si la coupe est franche des deux cotes.
				if ((Cut - Pts[i]).Size() > 2.0 && (Pts[i + 1] - Cut).Size() > 2.0)
				{
					Fins.Add(Cut);
				}
			}
			Fins.Add(Pts.Last());
			Pts = MoveTemp(Fins);
		}
		const int32 N = Pts.Num();
		if (N < 2)
		{
			return 0;
		}

		// --- 1. normales de SOMMET ----------------------------------------------
		TArray<FVector2D> Norm;
		Norm.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			FVector2D Dir = Pts[FMath::Min(N - 1, i + 1)] - Pts[FMath::Max(0, i - 1)];
			if (Dir.IsNearlyZero())
			{
				Dir = FVector2D(1.0, 0.0);
			}
			Dir.Normalize();
			Norm[i] = FVector2D(-Dir.Y, Dir.X) * (double)Wall.CoteBas;
		}

		// --- 2. marche a chaque sommet, puis MEDIANE des distances ---------------
		TArray<float> DFoot, DCrest;
		DFoot.Reserve(N);
		DCrest.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			const FWallSection S = MeasureWallSection(Pts[i], Norm[i], RGZ, QuadCm,
				Wall.HMedCm, bCrestOnPlateau, bFootOnPlateau, FootMinCm);
			DFoot.Add((float)(S.Pied - Pts[i]).Size());
			DCrest.Add((float)(S.Crete - Pts[i]).Size());
		}
		auto Mediane = [](TArray<float>& V) -> float
		{
			V.Sort();
			return V.Num() ? V[V.Num() / 2] : 0.f;
		};
		const float OffFoot = Mediane(DFoot);
		const float OffCrest = Mediane(DCrest);

		// --- 3. Z relus aux offsets retenus, pinces (Zcrete >= Zpied) ------------
		TArray<FVector2D> PFoot, PCrest;
		TArray<float> ZFoot, ZCrest, Hauteurs;
		PFoot.SetNum(N);
		PCrest.SetNum(N);
		ZFoot.SetNum(N);
		ZCrest.SetNum(N);
		Hauteurs.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			PFoot[i] = Pts[i] + Norm[i] * (double)OffFoot;
			PCrest[i] = Pts[i] - Norm[i] * (double)OffCrest;
		}
		// --- 3 bis. BERGES : ANTI-RETOURNEMENT ----------------------------------
		// Un decalage MEDIAN constant applique a des normales de SOMMET se CROISE
		// des que la ligne tourne serre : le quad s'inverse, sa normale part a
		// l'envers, et un backface ne recoit aucune lumiere — c'est le « mur vide,
		// on voit l'interieur sombre » des captures 4-6 (mesure : 73 murs sur 239,
		// 142 quads, recul jusqu'a 9,80 m). On RABOTE le decalage aux seuls sommets
		// fautifs jusqu'a ce que la ligne decalee avance partout. La convergence est
		// acquise : a decalage nul la ligne decalee EST l'axe, qui avance toujours.
		int32 FlipsFixed = 0;
		if (bNoFlip && N >= 2)
		{
			auto Deretourne = [&](TArray<FVector2D>& Off, float BaseOff, double Signe)
			{
				if (BaseOff <= 0.f)
				{
					return;
				}
				TArray<float> Echelle;
				Echelle.Init(1.f, N);
				for (int32 Iter = 0; Iter < 10; ++Iter)
				{
					bool bPropre = true;
					for (int32 i = 0; i + 1 < N; ++i)
					{
						FVector2D A = Pts[i + 1] - Pts[i];
						if (A.IsNearlyZero())
						{
							continue;
						}
						A.Normalize();
						if (FVector2D::DotProduct(Off[i + 1] - Off[i], A) >= 0.0)
						{
							continue;
						}
						bPropre = false;
						for (int32 k : { i, i + 1 })
						{
							Echelle[k] *= 0.5f;
							Off[k] = Pts[k] + Norm[k] * (Signe * (double)(BaseOff * Echelle[k]));
						}
					}
					if (bPropre)
					{
						break;
					}
				}
				for (int32 i = 0; i < N; ++i)
				{
					if (Echelle[i] < 1.f)
					{
						++FlipsFixed;
					}
				}
			};
			Deretourne(PFoot, OffFoot, 1.0);
			Deretourne(PCrest, OffCrest, -1.0);
		}
		for (int32 i = 0; i < N; ++i)
		{
			ZFoot[i] = RGZ.At(PFoot[i].X, PFoot[i].Y);
			ZCrest[i] = FMath::Max(RGZ.At(PCrest[i].X, PCrest[i].Y), ZFoot[i]);
			Hauteurs.Add(ZCrest[i] - ZFoot[i]);
		}
		const float HMed = Mediane(Hauteurs);
		if (OutGeom)
		{
			double LongCm = 0.0;
			for (int32 k = 0; k + 1 < Wall.PtsCm.Num(); ++k)
			{
				LongCm += (Wall.PtsCm[k + 1] - Wall.PtsCm[k]).Size();
			}
			OutGeom->OffFootCm = OffFoot;
			OutGeom->OffCrestCm = OffCrest;
			OutGeom->HMedCm = HMed;
			OutGeom->LenM = (float)(LongCm * 0.01);
		}
		if (HMed < GWallMinHeightCm ||
			(Wall.HMedCm > 0.f && HMed > Wall.HMedCm * GWallHeightGuard))
		{
			return 0;
		}

		const FVector3f Up(0, 0, 1);
		const FPolygonGroupID Group = QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material);
		auto WorldUV = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };
		auto V3 = [](const FVector2D& P, float Z) { return FVector3f((float)P.X, (float)P.Y, Z); };

		// --- 3 bis. QUAIS V4 : LES GRADINS -----------------------------------------
		// Trois conditions, toutes MESUREES, aucune devinee :
		//   (a) le profil est demande (bQuayTiers, plus le drapeau du profil) ;
		//   (b) le side-car a mesure une zone PIETONNE BASSE contre ce mur
		//       (`borde_pieton`) — un gradin qui donne sur rien n'a aucun sens ;
		//   (c) la hauteur MESUREE sur la surface rendue laisse la place a au moins
		//       deux gradins, et la profondeur disponible entre le pied et la crete
		//       aussi. Sinon la face reste lisse — sans rien compter comme un echec.
		// Le nombre de gradins est decide UNE FOIS pour toute la polyligne : un mur
		// qui changerait de nombre de gradins d'un segment a l'autre serait un
		// escalier casse, pas un quai.
		//
		// LOT FINITION QUAIS — QUATRIEME CONDITION, ET C'EST ELLE QUI BORNE TOUT :
		//   (d) le SEGMENT doit tomber dans une EMPRISE de gradins (OSM
		//       `leisure=bleachers`, side-car SourceData/Gradins). Sans emprise
		//       lisible, AUCUN gradin nulle part — l'absence de donnee interdit
		//       l'objet, elle ne l'autorise pas. Le nombre de gradins reste decide
		//       une fois pour la polyligne (un quai ne change pas de profil au
		//       milieu) ; seule l'ETENDUE est bornee par la donnee, segment par
		//       segment, ce qui donne « quelques metres d'emmarchements au debouche
		//       de la volee » et rien ailleurs.
		const float SpanCm = OffFoot + OffCrest;
		const bool bEmprisePossible = (Emprises != nullptr && Emprises->Num() > 0);
		int32 NTiers = 0;
		float TierRunCm = 0.f;
		if (bTiers && bEmprisePossible && Wall.bBordePieton
			&& HMed >= GTierMinHeightCm && SpanCm > GTierRunCm)
		{
			const int32 ParHauteur = FMath::RoundToInt32(HMed / GTierRiseCm);
			const int32 ParPlace = FMath::FloorToInt32(SpanCm / GTierRunCm);
			NTiers = FMath::Clamp(FMath::Min(ParHauteur, ParPlace), 0, GTierMaxCount);
			if (NTiers < 2)
			{
				NTiers = 0;    // un gradin unique, c'est une face lisse
			}
			else
			{
				TierRunCm = FMath::Min(GTierRunCm, SpanCm / (float)NTiers);
			}
		}
		// OutTiers n'est renseigne QU'A LA FIN : avec l'emprise, un mur peut avoir
		// un profil de gradins calcule et n'en poser AUCUN (aucun segment dedans).
		// Compter le profil plutot que la pose ferait mentir le compteur.
		float TierM = 0.f;

		int32 Quads = 0;
		float Arc = 0.f;
		for (int32 i = 0; i + 1 < N; ++i)
		{
			const float SegLen = (float)(Pts[i + 1] - Pts[i]).Size();
			if (SegLen < 1.f)
			{
				continue;
			}
			const FVector2D NLow = (Norm[i] + Norm[i + 1]).GetSafeNormal();
			const float U0 = Arc * 0.01f;
			const float U1 = (Arc + SegLen) * 0.01f;
			Arc += SegLen;
			// L'EMPRISE, segment par segment : on teste le MILIEU du segment, sur la
			// polyligne d'axe (celle du side-car) — pas le pied ni la crete, qui
			// dependent d'un decalage MEDIAN et sortiraient de l'emprise avant elle.
			const bool bSegTiers = (NTiers >= 2) && DansUneEmprise(
				*Emprises, (Pts[i] + Pts[i + 1]) * 0.5);
			if (bSegTiers)
			{
				TierM += SegLen * 0.01f;
			}
			// V de l'UV : la hauteur REELLE a chaque extremite (un mur dont la hauteur
			// varie ne doit pas etirer son motif de pierre).
			const float VA = (ZCrest[i] - ZFoot[i] + GWallSinkCm) * 0.01f;
			const float VB = (ZCrest[i + 1] - ZFoot[i + 1] + GWallSinkCm) * 0.01f;

			// Position et altitude a la fraction t du trajet pied -> crete.
			// t = 0 : le pied ; t = 1 : la crete. Le Z suit la MEME fraction, ce qui
			// fait que le profil se referme exactement sur le sol aux deux bouts.
			auto PLerp = [&](int32 k, float t) { return PFoot[k] + (PCrest[k] - PFoot[k]) * (double)t; };
			auto ZLerp = [&](int32 k, float t) { return ZFoot[k] + (ZCrest[k] - ZFoot[k]) * t; };

			// 1. LA FACE, en gradins ou lisse. Dans les deux cas elle part du pied
			//    ENTERRE et arrive a l'altitude de la crete : la difference est le
			//    chemin entre les deux.
			float TFin = 0.f;    // fraction atteinte par la face — le couronnement prend la suite
			if (bSegTiers)
			{
				// GRADINS : contremarche verticale + assise horizontale, NTiers fois.
				// La premiere contremarche part du pied enterre, comme la face lisse.
				for (int32 k = 0; k < NTiers; ++k)
				{
					const float t0 = (float)k * TierRunCm / SpanCm;
					const float t1 = (float)(k + 1) * TierRunCm / SpanCm;
					const float f0 = (float)k / (float)NTiers;
					const float f1 = (float)(k + 1) / (float)NTiers;
					const float ZA0 = (k == 0) ? (ZFoot[i] - GWallSinkCm) : ZLerp(i, f0);
					const float ZB0 = (k == 0) ? (ZFoot[i + 1] - GWallSinkCm) : ZLerp(i + 1, f0);
					// (a) contremarche
					const FVector3f R[4] = {
						V3(PLerp(i, t0), ZA0),
						V3(PLerp(i + 1, t0), ZB0),
						V3(PLerp(i + 1, t0), ZLerp(i + 1, f1)),
						V3(PLerp(i, t0), ZLerp(i, f1)) };
					const FVector2f RUV[4] = {
						FVector2f(U0, 0.f), FVector2f(U1, 0.f),
						FVector2f(U1, (ZLerp(i + 1, f1) - ZB0) * 0.01f),
						FVector2f(U0, (ZLerp(i, f1) - ZA0) * 0.01f) };
					QM.AddPoly(Group, R, 4,
						FVector3f((float)NLow.X, (float)NLow.Y, 0.f).GetSafeNormal(), RUV, Tint);
					// (b) assise
					const FVector3f T[4] = {
						V3(PLerp(i, t0), ZLerp(i, f1)),
						V3(PLerp(i + 1, t0), ZLerp(i + 1, f1)),
						V3(PLerp(i + 1, t1), ZLerp(i + 1, f1)),
						V3(PLerp(i, t1), ZLerp(i, f1)) };
					const FVector2f TUV[4] = { WorldUV(T[0]), WorldUV(T[1]), WorldUV(T[2]), WorldUV(T[3]) };
					QM.AddPoly(Group, T, 4, Up, TUV, Tint);
					Quads += 2;
					TFin = t1;
				}
			}
			else
			{
				// FACE VERTICALE lisse, au pied de la rampe, tournee vers le BAS.
				const FVector3f F[4] = {
					V3(PFoot[i], ZFoot[i] - GWallSinkCm),
					V3(PFoot[i + 1], ZFoot[i + 1] - GWallSinkCm),
					V3(PFoot[i + 1], ZCrest[i + 1]),
					V3(PFoot[i], ZCrest[i]) };
				const FVector2f FUV[4] = {
					FVector2f(U0, 0.f), FVector2f(U1, 0.f),
					FVector2f(U1, VB), FVector2f(U0, VA) };
				QM.AddPoly(Group, F, 4,
					FVector3f((float)NLow.X, (float)NLow.Y, 0.f).GetSafeNormal(), FUV, Tint);
			}

			// 2. COURONNEMENT horizontal, de la fin de la face jusqu'a la crete, a
			//    l'altitude du haut : c'est LUI qui recouvre la rampe etiree. UV
			//    MONDE, comme le chant des bordures — le motif reste en phase avec
			//    la dalle. En gradins il ne couvre que ce que les assises ont laisse.
			const FVector3f C[4] = {
				V3(PLerp(i, TFin), ZCrest[i]),
				V3(PLerp(i + 1, TFin), ZCrest[i + 1]),
				V3(PCrest[i + 1], ZCrest[i + 1]),
				V3(PCrest[i], ZCrest[i]) };
			const FVector2f CUV[4] = { WorldUV(C[0]), WorldUV(C[1]), WorldUV(C[2]), WorldUV(C[3]) };
			QM.AddPoly(Group, C, 4, Up, CUV, Tint);

			// 3. DOS, au droit de la crete : sans lui la piece est un plan sans envers
			//    des qu'on la regarde depuis la terrasse haute.
			//    LOT FINITION QUAIS (M4) — IL DESCEND JUSQU'AU PIED, plus jusqu'a un
			//    bouchon de 40 cm. MESURE : `ZCrest = max(sol rendu en PCrest, ZFoot)`
			//    remonte la crete des que le decalage MEDIAN de crete deborde sur une
			//    contre-pente ; le dos de 40 cm flottait alors au-dessus du sol et on
			//    voyait la FACE PAR DERRIERE — un backface ne recoit aucune lumiere,
			//    d'ou les eclats NOIRS triangulaires vus du haut de quai (leur base
			//    color est identique au dallage : c'est un defaut d'eclairage, donc de
			//    normale, pas de matiere). Fermer la piece de bout en bout supprime la
			//    cause a la racine, n'ajoute AUCUN quad, et ne peut rien ouvrir.
			const float ZBackA = FMath::Min(ZFoot[i], ZCrest[i]) - GWallSinkCm;
			const float ZBackB = FMath::Min(ZFoot[i + 1], ZCrest[i + 1]) - GWallSinkCm;
			const FVector3f W[4] = {
				V3(PCrest[i + 1], ZCrest[i + 1]),
				V3(PCrest[i], ZCrest[i]),
				V3(PCrest[i], ZBackA),
				V3(PCrest[i + 1], ZBackB) };
			const FVector2f WUV[4] = {
				FVector2f(U1, 0.f), FVector2f(U0, 0.f),
				FVector2f(U0, (ZCrest[i] - ZBackA) * 0.01f),
				FVector2f(U1, (ZCrest[i + 1] - ZBackB) * 0.01f) };
			QM.AddPoly(Group, W, 4,
				FVector3f(-(float)NLow.X, -(float)NLow.Y, 0.f).GetSafeNormal(), WUV, Tint);
			// couronnement + dos ; la face lisse en ajoute un, les gradins ont deja
			// compte leurs 2 x NTiers quads dans la boucle.
			Quads += bSegTiers ? 2 : 3;
		}
		if (OutFlipsFixed)
		{
			*OutFlipsFixed += FlipsFixed;
		}
		if (Quads > 0)
		{
			// BOUCHONS de fin : une piece OUVERTE laisse voir son interieur par ses
			// deux bouts (meme raison que pour les bordures).
			// BERGES : ils ne ferment plus que les VRAIS bouts. Un mur continu que la
			// decoupe A LA CELLULE separe en deux recevait quatre bouchons, dont deux
			// dos a dos au milieu du mur (mesure : 31 paires de bouts jointifs a
			// 0,00 m et 0,0 degre, dont le mur Saint-Pierre -> Daurade lui-meme).
			const bool bCapA = Wall.bBoutDebut || !bCapsRealEndsOnly;
			const bool bCapB = Wall.bBoutFin || !bCapsRealEndsOnly;
			const FVector2D D0 = (Pts[1] - Pts[0]).GetSafeNormal();
			const FVector2D D1 = (Pts[N - 1] - Pts[N - 2]).GetSafeNormal();
			// M4 : les bouchons descendent au MEME niveau que le dos (le pied enterre),
			// sinon ils rouvriraient par le bas ce que le dos vient de fermer.
			const float ZCapA = FMath::Min(ZFoot[0], ZCrest[0]) - GWallSinkCm;
			const float ZCapB = FMath::Min(ZFoot[N - 1], ZCrest[N - 1]) - GWallSinkCm;
			const FVector3f CapA[4] = {
				V3(PFoot[0], ZFoot[0] - GWallSinkCm),
				V3(PCrest[0], ZCapA),
				V3(PCrest[0], ZCrest[0]),
				V3(PFoot[0], ZCrest[0]) };
			const FVector3f CapB[4] = {
				V3(PFoot[N - 1], ZFoot[N - 1] - GWallSinkCm),
				V3(PFoot[N - 1], ZCrest[N - 1]),
				V3(PCrest[N - 1], ZCrest[N - 1]),
				V3(PCrest[N - 1], ZCapB) };
			const FVector2f CapUV[4] = {
				FVector2f(0.f, 0.f), FVector2f(1.f, 0.f), FVector2f(1.f, 1.f), FVector2f(0.f, 1.f) };
			if (bCapA)
			{
				QM.AddPoly(Group, CapA, 4,
					FVector3f(-(float)D0.X, -(float)D0.Y, 0.f).GetSafeNormal(), CapUV, Tint);
				++Quads;
			}
			if (bCapB)
			{
				QM.AddPoly(Group, CapB, 4,
					FVector3f((float)D1.X, (float)D1.Y, 0.f).GetSafeNormal(), CapUV, Tint);
				++Quads;
			}
			if (OutCaps)
			{
				*OutCaps += (bCapA ? 1 : 0) + (bCapB ? 1 : 0);
			}
		}
		if (TierM > 0.f)
		{
			OutTiers = NTiers;
			if (OutTierM)
			{
				*OutTierM += TierM;
			}
		}
		return Quads;
	}

	// =========================================================================
	// LOT QUAIS V2 — BuildStairs : LA VOLEE DE MARCHES.
	//
	// CE QUE LA DONNEE DIT, ET CE QU'ELLE NE DIT PAS.
	// Le side-car donne un TRACE EN PLAN et une largeur. Il ne donne AUCUN Z — ni
	// BD TOPO ni OSM ne codent l'altitude de ces troncons. Tout le relief est donc
	// LU SUR LA SURFACE RENDUE (doctrine du Playbook §6, la meme que les murs) : on
	// echantillonne le sol rendu aux deux extremites, la difference est le denivele
	// a franchir, et c'est elle qui commande la volee.
	//
	// LE PROFIL, ET POURQUOI IL EST COMME CA.
	// n = round(dZ / 16,5 cm), puis la contremarche REELLE vaut dZ/n et doit tomber
	// dans la fourchette d'usage [13 ; 19] cm — sinon on ajuste n d'une marche.
	// Le giron se deduit de la longueur en plan. Quand il depasse 45 cm — ce qui est
	// LE cas des grands escaliers de quai (mesure : 7,84 m de denivele pour 31,9 m
	// de trace au Pont Saint-Pierre, soit 66 cm de giron uniforme) — le surplus part
	// en PALIERS de repos, comme dans la realite, et la volee redevient lisible.
	//
	// LES LIMONS. Une volee posee « en l'air » sur un terrain en pente montre son
	// dessous des qu'on la regarde d'en bas — c'est exactement la vue depuis le
	// fleuve, celle de la pose C1. Chaque marche porte donc deux flancs pleins qui
	// descendent jusqu'au sol RENDU (enterres de 30 cm), et la volee est fermee en
	// tete comme en pied.
	//
	// Rend le nombre de marches posees. 0 = ECARTE : la surface rendue ne presente
	// pas de denivele exploitable ici, ou la geometrie ne serait pas un escalier.
	//
	// ITERATION UTILISATEUR 2 (02/08) — DEUX AJOUTS, tous deux dans ce corps :
	//
	// (a) CHAQUE ECART EST JOURNALISE NOMINATIVEMENT. Avant, la passe ne rendait
	//     qu'un COMPTE agrege (`StairsSkipped`) : impossible de savoir QUELLE volee
	//     tombait ni pourquoi, donc impossible de verifier un grief utilisateur
	//     autrement qu'en croyant un total. Une ligne Display par volee, avec son
	//     cleabs et la mesure qui l'a decidee — jamais Warning ni Error (l'automation
	//     les eleve en echec de test, regression payee par le lot C1).
	//
	// (b) LA PENTE MINIMALE, contre les BISEAUX. Grief utilisateur : des « lames
	//     anguleuses » sur la place. Cause mesuree : une volee BD TOPO longue posee
	//     sur du terrain presque plat garde assez de denivele pour passer le plancher
	//     de 33 cm, mais son giron theorique explose ; le surplus part en paliers et
	//     la piece devient un biseau de 20 a 40 m couche au sol (mesure : 21,6 m pour
	//     74,7 cm de denivele, soit 90 % de palier). Le critere qui SEPARE n'est ni le
	//     denivele ni le nombre de marches — les deux se recouvrent — c'est la PENTE.
	//     Distribution mesuree sur les 36 volees de l'emprise 3x3 : un vide franc
	//     entre 9,1 % et 12,7 % (et entre 9,1 % et 15,0 % sur le district). Le seuil
	//     est pose au MILIEU de ce vide, a 11 % : aucune valeur mesuree n'est a moins
	//     de 1,7 point de lui. Un escalier reel, meme a grands paliers de repos, tient
	//     largement au-dessus (les deux volees du Pont Saint-Pierre sont a 23,4 et
	//     23,6 %, la volee raide de la Daurade a 33,5 %).
	int32 BuildStairs(FCityMeshBuilder& QM, const FCityStairs& St,
		const FRenderedGroundZ& RGZ, const FResolvedSurface* Surf, const FVector3f& Tint,
		int32& OutQuads)
	{
		// (La pente minimale a ete PROMUE en `GStairMinSlopePct` aupres des autres
		//  constantes de recette au build de consolidation du lot PIE — elle etait
		//  locale au corps parce qu'elle etait arrivee par un patch Live Coding, qui
		//  interdit de toucher a ce qui est hors fonction.)
		OutQuads = 0;
		if (!Surf || St.PtsCm.Num() < 2)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("QUAIS V2 escalier ECARTE id=%s cause=SANS_SURFACE_OU_TRACE (%d points)."),
				*St.Id, St.PtsCm.Num());
			return 0;
		}

		// --- 1. abscisse curviligne et longueur en plan --------------------------
		const int32 NP = St.PtsCm.Num();
		TArray<float> S;
		S.SetNum(NP);
		S[0] = 0.f;
		for (int32 i = 1; i < NP; ++i)
		{
			S[i] = S[i - 1] + (float)(St.PtsCm[i] - St.PtsCm[i - 1]).Size();
		}
		const float RunCm = S[NP - 1];
		if (RunCm < GStairTreadMinCm * 2.f)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("QUAIS V2 escalier ECARTE id=%s cause=TRACE_TROP_COURT run=%.0f cm (< %.0f)."),
				*St.Id, RunCm, GStairTreadMinCm * 2.f);
			return 0;
		}

		// Position et direction a une abscisse donnee (le trace peut avoir des coudes).
		auto PointAt = [&](float s) -> FVector2D
		{
			s = FMath::Clamp(s, 0.f, RunCm);
			for (int32 i = 1; i < NP; ++i)
			{
				if (s <= S[i] || i == NP - 1)
				{
					const float d = FMath::Max(S[i] - S[i - 1], 1e-4f);
					const float u = FMath::Clamp((s - S[i - 1]) / d, 0.f, 1.f);
					return St.PtsCm[i - 1] + (St.PtsCm[i] - St.PtsCm[i - 1]) * (double)u;
				}
			}
			return St.PtsCm[NP - 1];
		};
		auto DirAt = [&](float s) -> FVector2D
		{
			const FVector2D A = PointAt(FMath::Max(0.f, s - 25.f));
			const FVector2D B = PointAt(FMath::Min(RunCm, s + 25.f));
			const FVector2D D = B - A;
			return D.IsNearlyZero() ? FVector2D(1.0, 0.0) : D.GetSafeNormal();
		};

		// --- 2. le denivele, LU SUR LA SURFACE RENDUE ----------------------------
		// Aux extremites exactes le sol rendu peut deja etre celui du palier : on
		// prend le MIN et le MAX sur un balayage du trace, ce qui est aussi ce qui
		// rend la volee robuste a un trace qui deborde d'un cote ou de l'autre.
		const int32 NS = FMath::Clamp(FMath::CeilToInt32(RunCm / 50.f), 4, 400);
		float ZLo = FLT_MAX, ZHi = -FLT_MAX;
		float SLo = 0.f, SHi = 0.f;
		for (int32 i = 0; i <= NS; ++i)
		{
			const float s = RunCm * (float)i / (float)NS;
			const FVector2D P = PointAt(s);
			const float Z = RGZ.At(P.X, P.Y);
			if (Z < ZLo) { ZLo = Z; SLo = s; }
			if (Z > ZHi) { ZHi = Z; SHi = s; }
		}
		const float DropCm = ZHi - ZLo;
		if (DropCm < GStairMinDropCm)
		{
			// ECARTE : rien a descendre ici
			UE_LOG(LogCityImport, Display,
				TEXT("QUAIS V2 escalier ECARTE id=%s cause=PAS_DE_DENIVELE drop=%.1f cm (< %.0f) run=%.0f cm."),
				*St.Id, DropCm, GStairMinDropCm, RunCm);
			return 0;
		}
		// LE BISEAU : assez de denivele pour passer le plancher, pas assez de pente
		// pour etre un escalier. Voir l'en-tete de la fonction pour la distribution
		// mesuree et le vide ou ce seuil est pose.
		const float SlopePct = 100.f * DropCm / RunCm;
		if (SlopePct < GStairMinSlopePct)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("QUAIS V2 escalier ECARTE id=%s cause=BISEAU_PENTE_TROP_FAIBLE pente=%.1f %% (< %.1f) drop=%.1f cm run=%.0f cm."),
				*St.Id, SlopePct, GStairMinSlopePct, DropCm, RunCm);
			return 0;
		}

		// Sens de parcours : on construit TOUJOURS du BAS vers le HAUT. C'est SHi et
		// SLo qui le disent — aucune convention implicite n'est tiree du sens du
		// trace du side-car (meme discipline que CoteBas pour les murs).
		const bool bMonteAvecS = (SHi > SLo);
		auto SAt = [&](float t) -> float   // t dans [0;1], 0 = bas
		{
			return bMonteAvecS ? (RunCm * t) : (RunCm * (1.f - t));
		};

		// --- 3. le profil : nombre de marches, contremarche, giron, paliers ------
		int32 N = FMath::RoundToInt32(DropCm / GStairRiserCm);
		N = FMath::Clamp(N, 1, GStairMaxSteps);
		// rattrapage : la contremarche reelle doit rester dans la fourchette d'usage
		while (N < GStairMaxSteps && DropCm / (float)N > GStairRiserMaxCm) { ++N; }
		while (N > 1 && DropCm / (float)N < GStairRiserMinCm) { --N; }
		const float RiserCm = DropCm / (float)N;
		if (RiserCm > GStairRiserMaxCm)
		{
			// ECARTE : meme a 200 marches la contremarche reste impossible
			UE_LOG(LogCityImport, Display,
				TEXT("QUAIS V2 escalier ECARTE id=%s cause=CONTREMARCHE_IMPOSSIBLE riser=%.1f cm (> %.0f) drop=%.1f cm n=%d."),
				*St.Id, RiserCm, GStairRiserMaxCm, DropCm, N);
			return 0;
		}

		float TreadCm = RunCm / (float)N;
		int32 NLandings = 0;
		float LandingCm = 0.f;
		if (TreadCm < GStairTreadMinCm)
		{
			// ECARTE : ce serait une echelle, pas un escalier
			UE_LOG(LogCityImport, Display,
				TEXT("QUAIS V2 escalier ECARTE id=%s cause=ECHELLE giron=%.1f cm (< %.0f) n=%d run=%.0f cm."),
				*St.Id, TreadCm, GStairTreadMinCm, N, RunCm);
			return 0;
		}
		if (TreadCm > GStairTreadMaxCm)
		{
			// Le surplus de longueur part en paliers de repos, repartis dans la volee.
			TreadCm = GStairTreadMaxCm;
			const float Reste = RunCm - TreadCm * (float)N;
			NLandings = FMath::Clamp(FMath::RoundToInt32(Reste / GStairLandingCm), 1, FMath::Max(1, N - 1));
			LandingCm = Reste / (float)NLandings;
		}

		// Ou tombent les paliers : apres chaque bloc de marches, repartis regulierement.
		auto EstPalierApres = [&](int32 iMarche) -> bool
		{
			if (NLandings <= 0 || iMarche >= N - 1)
			{
				return false;
			}
			const int32 Bloc = FMath::Max(1, N / (NLandings + 1));
			return ((iMarche + 1) % Bloc) == 0 &&
				((iMarche + 1) / Bloc) <= NLandings;
		};

		// --- 4. pose --------------------------------------------------------------
		const FVector3f Up(0, 0, 1);
		const FPolygonGroupID Group = QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material);
		const float HalfW = St.WidthCm * 0.5f;
		auto V3 = [](const FVector2D& P, float Z) { return FVector3f((float)P.X, (float)P.Y, Z); };
		auto WorldUV = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };

		int32 Marches = 0;
		float sCur = 0.f;     // avancement DANS LE SENS DE LA MONTEE, depuis le bas
		float zCur = ZLo;     // altitude du nez de la marche courante
		for (int32 i = 0; i < N; ++i)
		{
			const float sBas = sCur;
			const float sHaut = sCur + TreadCm;
			const float sFin = sHaut + (EstPalierApres(i) ? LandingCm : 0.f);
			const float zBas = zCur;
			const float zHaut = zCur + RiserCm;

			// Le profil d'un escalier qui monte : la CONTREMARCHE se dresse au NEZ
			// (abscisse sBas) de zBas a zHaut, PUIS le giron court a plat de sBas a
			// sFin. Faire partir le giron de sHaut laisserait un jour de la largeur
			// d'une marche entre les deux.
			const FVector2D PB = PointAt(SAt(sBas / RunCm));
			const FVector2D PF = PointAt(SAt(sFin / RunCm));
			const FVector2D DB = DirAt(SAt(sBas / RunCm));
			const FVector2D DF = DirAt(SAt(sFin / RunCm));
			const FVector2D NB(-DB.Y, DB.X), NF(-DF.Y, DF.X);

			const FVector2D BL = PB - NB * (double)HalfW, BR = PB + NB * (double)HalfW;
			const FVector2D FL = PF - NF * (double)HalfW, FR = PF + NF * (double)HalfW;

			// La normale de la CONTREMARCHE regarde vers le bas de la volee.
			const FVector2D VersBas = bMonteAvecS ? -DB : DB;
			const FVector3f NRise((float)VersBas.X, (float)VersBas.Y, 0.f);

			// (a) CONTREMARCHE : le rectangle vertical qui monte de zBas a zHaut, a
			//     l'aplomb du NEZ de la marche.
			{
				const FVector3f Q[4] = { V3(BL, zBas), V3(BR, zBas), V3(BR, zHaut), V3(BL, zHaut) };
				const FVector2f UV[4] = {
					FVector2f(0.f, 0.f), FVector2f(St.WidthCm * 0.01f, 0.f),
					FVector2f(St.WidthCm * 0.01f, RiserCm * 0.01f), FVector2f(0.f, RiserCm * 0.01f) };
				QM.AddPoly(Group, Q, 4, NRise.GetSafeNormal(), UV, Tint);
				++OutQuads;
			}
			// (b) GIRON (le palier eventuel est dans le MEME quad : c'est la meme
			//     surface horizontale, simplement plus profonde). UV MONDE, comme le
			//     couronnement des murs — le motif de pierre reste en phase.
			{
				const FVector3f Q[4] = { V3(BL, zHaut), V3(BR, zHaut), V3(FR, zHaut), V3(FL, zHaut) };
				const FVector2f UV[4] = { WorldUV(Q[0]), WorldUV(Q[1]), WorldUV(Q[2]), WorldUV(Q[3]) };
				QM.AddPoly(Group, Q, 4, Up, UV, Tint);
				++OutQuads;
			}
			// (c) LES DEUX LIMONS : de la marche jusqu'au sol RENDU, enterres.
			//     Sans eux la volee montre son dessous des qu'on la regarde d'en bas —
			//     c'est-a-dire depuis le fleuve, la pose de reference du lot.
			{
				const float ZsolB = FMath::Min(RGZ.At(BL.X, BL.Y), zBas) - GStairSinkCm;
				const float ZsolF = FMath::Min(RGZ.At(FL.X, FL.Y), zHaut) - GStairSinkCm;
				const FVector3f LG[4] = { V3(BL, zHaut), V3(FL, zHaut), V3(FL, ZsolF), V3(BL, ZsolB) };
				const FVector2f UVL[4] = {
					FVector2f(0.f, 0.f), FVector2f((sFin - sBas) * 0.01f, 0.f),
					FVector2f((sFin - sBas) * 0.01f, (zHaut - ZsolF) * 0.01f),
					FVector2f(0.f, (zHaut - ZsolB) * 0.01f) };
				QM.AddPoly(Group, LG, 4, FVector3f(-(float)NB.X, -(float)NB.Y, 0.f).GetSafeNormal(), UVL, Tint);
				const float ZsolBR = FMath::Min(RGZ.At(BR.X, BR.Y), zBas) - GStairSinkCm;
				const float ZsolFR = FMath::Min(RGZ.At(FR.X, FR.Y), zHaut) - GStairSinkCm;
				const FVector3f LD[4] = { V3(FR, zHaut), V3(BR, zHaut), V3(BR, ZsolBR), V3(FR, ZsolFR) };
				const FVector2f UVR[4] = {
					FVector2f(0.f, 0.f), FVector2f((sFin - sBas) * 0.01f, 0.f),
					FVector2f((sFin - sBas) * 0.01f, (zHaut - ZsolBR) * 0.01f),
					FVector2f(0.f, (zHaut - ZsolFR) * 0.01f) };
				QM.AddPoly(Group, LD, 4, FVector3f((float)NB.X, (float)NB.Y, 0.f).GetSafeNormal(), UVR, Tint);
				OutQuads += 2;
			}

			sCur = sFin;
			zCur = zHaut;
			++Marches;
		}

		// (d) BOUCHONS : la volee est une piece fermee. En PIED, le flanc vertical
		//     sous la premiere contremarche ; en TETE, le dos de la derniere marche.
		if (Marches > 0)
		{
			const FVector2D P0 = PointAt(SAt(0.f));
			const FVector2D D0 = DirAt(SAt(0.f));
			const FVector2D N0(-D0.Y, D0.X);
			const FVector2D L0 = P0 - N0 * (double)HalfW, R0 = P0 + N0 * (double)HalfW;
			const float Z0 = FMath::Min(FMath::Min(RGZ.At(L0.X, L0.Y), RGZ.At(R0.X, R0.Y)), ZLo) - GStairSinkCm;
			const FVector2D VersBas0 = bMonteAvecS ? -D0 : D0;
			const FVector3f Pied[4] = { V3(L0, ZLo), V3(R0, ZLo), V3(R0, Z0), V3(L0, Z0) };
			const FVector2f CapUV[4] = {
				FVector2f(0.f, 0.f), FVector2f(1.f, 0.f), FVector2f(1.f, 1.f), FVector2f(0.f, 1.f) };
			QM.AddPoly(Group, Pied, 4,
				FVector3f((float)VersBas0.X, (float)VersBas0.Y, 0.f).GetSafeNormal(), CapUV, Tint);

			const FVector2D P1 = PointAt(SAt(1.f));
			const FVector2D D1 = DirAt(SAt(1.f));
			const FVector2D N1(-D1.Y, D1.X);
			const FVector2D L1 = P1 - N1 * (double)HalfW, R1 = P1 + N1 * (double)HalfW;
			const float Z1 = FMath::Min(FMath::Min(RGZ.At(L1.X, L1.Y), RGZ.At(R1.X, R1.Y)), ZHi) - GStairSinkCm;
			const FVector2D VersHaut = bMonteAvecS ? D1 : -D1;
			const FVector3f Tete[4] = { V3(R1, ZHi), V3(L1, ZHi), V3(L1, Z1), V3(R1, Z1) };
			QM.AddPoly(Group, Tete, 4,
				FVector3f((float)VersHaut.X, (float)VersHaut.Y, 0.f).GetSafeNormal(), CapUV, Tint);
			OutQuads += 2;
		}

		// La contrepartie de l'ECARTE : la volee POSEE se nomme, elle aussi. C'est
		// cette ligne que lit le VERROU NOMINATIF des deux volees du Pont Saint-Pierre
		// (work/QUAIS/q_verrou_escaliers.py) — un verrou porte une verite locale, la
		// regle au-dessus n'en porte aucune.
		UE_LOG(LogCityImport, Display,
			TEXT("QUAIS V2 escalier POSEE id=%s marches=%d drop=%.1f cm run=%.0f cm pente=%.1f %% riser=%.2f cm giron=%.1f cm paliers=%d largeur=%.0f cm."),
			*St.Id, Marches, DropCm, RunCm, 100.f * DropCm / RunCm, RiserCm, TreadCm,
			NLandings, St.WidthCm);

		return Marches;
	}

	// -------------------------------------------------------------------------
	// C2 — LE TABLIER A SA COTE.
	//
	// Une dalle d'epaisseur GBridgeDeckThickCm suivant l'axe du side-car : chaussee
	// dessus (l'asphalte des rubans), sous-face et bandeaux lateraux en pierre (celle
	// des murs et des bordures). Deux garde-fous de geometrie, tous deux mesures :
	//   (a) la sous-face est CLAMPEE sous le sol rendu au raccord — sans ca, la ou
	//       l'ouvrage atterrit, le bandeau de 90 cm flotterait au-dessus du terrain ;
	//   (b) parapets seulement la ou la hauteur libre depasse GBridgeMinClearCm — un
	//       troncon code pont au ras du sol (Quai Saint Pierre : 2 cm) ne doit pas se
	//       retrouver borde de deux murets.
	// Rend le nombre de METRES de tablier poses (0 = ecarte, avec sa cause au log).
	float BuildBridge(FCityMeshBuilder& QM, const FCityBridge& B, const FRenderedGroundZ& RGZ,
		const FResolvedSurface* DeckSurf, const FResolvedSurface* StoneSurf,
		const FVector3f& StoneTint, bool bParapets, int32& OutQuads)
	{
		OutQuads = 0;
		if (!DeckSurf || !StoneSurf || B.PtsCm.Num() < 2)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("PONTS C2 tablier ECARTE id=%s cause=SANS_SURFACE_OU_TRACE (%d points)."),
				*B.Id, B.PtsCm.Num());
			return 0.f;
		}

		// --- 1. abscisse curviligne ------------------------------------------------
		const int32 NP = B.PtsCm.Num();
		TArray<float> S;
		S.SetNum(NP);
		S[0] = 0.f;
		for (int32 i = 1; i < NP; ++i)
		{
			S[i] = S[i - 1] + (float)(B.PtsCm[i] - B.PtsCm[i - 1]).Size();
		}
		const float RunCm = S[NP - 1];
		if (RunCm < 200.f)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("PONTS C2 tablier ECARTE id=%s cause=TRACE_TROP_COURT run=%.0f cm (< 200)."),
				*B.Id, RunCm);
			return 0.f;
		}

		// --- 2. re-echantillonnage a pas fixe, Z interpole DANS LA DONNEE ----------
		// (le pas est ce qui fait epouser la courbure d'un tablier de 240 m sans
		//  exploser le nombre de quads : 400 cm -> ~60 sections pour Saint-Pierre)
		const int32 NSeg = FMath::Max(1, FMath::CeilToInt32(RunCm / GBridgeStepCm));
		TArray<FVector2D> Axe;
		TArray<float> Z;
		Axe.SetNum(NSeg + 1);
		Z.SetNum(NSeg + 1);
		auto EchantillonneA = [&](float s, FVector2D& OutP, float& OutZ)
		{
			s = FMath::Clamp(s, 0.f, RunCm);
			for (int32 i = 1; i < NP; ++i)
			{
				if (s <= S[i] || i == NP - 1)
				{
					const float d = FMath::Max(S[i] - S[i - 1], 1e-4f);
					const float u = FMath::Clamp((s - S[i - 1]) / d, 0.f, 1.f);
					OutP = B.PtsCm[i - 1] + (B.PtsCm[i] - B.PtsCm[i - 1]) * (double)u;
					OutZ = FMath::Lerp(B.ZCm[i - 1], B.ZCm[i], u);
					return;
				}
			}
			OutP = B.PtsCm[NP - 1];
			OutZ = B.ZCm[NP - 1];
		};
		for (int32 i = 0; i <= NSeg; ++i)
		{
			EchantillonneA(RunCm * (float)i / (float)NSeg, Axe[i], Z[i]);
		}
		// REPLI quand le side-car n'a pas de cote : exactement le comportement
		// d'avant C2 (interpolation entre les deux rives), mais sur le sol RENDU et
		// journalise — jamais en silence.
		if (!B.bHasZ)
		{
			const float Z0 = RGZ.At(Axe[0].X, Axe[0].Y);
			const float Z1 = RGZ.At(Axe[NSeg].X, Axe[NSeg].Y);
			for (int32 i = 0; i <= NSeg; ++i)
			{
				Z[i] = FMath::Lerp(Z0, Z1, (float)i / (float)NSeg);
			}
		}

		// --- 3. pose --------------------------------------------------------------
		const FPolygonGroupID DeckGroup = QM.GetOrCreateGroup(DeckSurf->SlotName(), DeckSurf->Material);
		const FPolygonGroupID StoneGroup = QM.GetOrCreateGroup(StoneSurf->SlotName(), StoneSurf->Material);
		const FVector3f Up(0, 0, 1);
		const FVector3f Down(0, 0, -1);
		const FVector3f DeckTint(1.f, 1.f, 1.f);
		// ⭐ LE PAREMENT VERTICAL D'UN TABLIER EST POSE DES DEUX COTES.
		// Meme regle que la cuisson du quai (« un quad dont l'exterieur n'est pas
		// PROUVE est pose des DEUX cotes »). Ici l'`Outward` fourni est deduit du
		// SENS DE PARCOURS du trace : ce n'est pas une preuve, et le juge de
		// visibilite (2026-08-06) mesure des milliers de pixels ou l'oeil traverse
		// un parement parce que sa face unique tourne le dos a la camera.
		// L'ajout est PUREMENT ADDITIF : chaque quad garde son enroulement et
		// recoit son miroir — aucune face ne peut disparaitre, aucun materiau
		// neuf, aucune constante locale, aucun champ d'en-tete.
		int32 TwoSidedQuads = 0;
		auto AddStoneTwoSided = [&](const FVector3f* C4, const FVector3f& Nout,
			const FVector2f* UV4)
		{
			QM.AddPoly(StoneGroup, C4, 4, Nout, UV4, StoneTint);
			QM.AddPoly(StoneGroup, C4, 4, -Nout, UV4, StoneTint);
			++TwoSidedQuads;
		};
		const float HalfW = B.WidthCm * 0.5f;
		auto V3 = [](const FVector2D& P, float Zc) { return FVector3f((float)P.X, (float)P.Y, Zc); };
		auto WorldUV = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };

		// Normale en plan a chaque section (moyenne des deux segments voisins : sans
		// ca les bords se croisent dans les virages serres).
		TArray<FVector2D> Nrm;
		Nrm.SetNum(NSeg + 1);
		for (int32 i = 0; i <= NSeg; ++i)
		{
			const FVector2D A = Axe[FMath::Max(0, i - 1)];
			const FVector2D C = Axe[FMath::Min(NSeg, i + 1)];
			FVector2D D = C - A;
			if (D.IsNearlyZero())
			{
				D = FVector2D(1.0, 0.0);
			}
			D = D.GetSafeNormal();
			Nrm[i] = FVector2D(-D.Y, D.X);
		}

		// Hauteur libre par section (tablier - sol rendu sous l'axe) : elle decide des
		// parapets, elle est aussi ce que le journal rapporte.
		TArray<float> Clear;
		Clear.SetNum(NSeg + 1);
		float ClearMax = -FLT_MAX;
		for (int32 i = 0; i <= NSeg; ++i)
		{
			Clear[i] = Z[i] - RGZ.At(Axe[i].X, Axe[i].Y);
			ClearMax = FMath::Max(ClearMax, Clear[i]);
		}

		for (int32 i = 0; i < NSeg; ++i)
		{
			const FVector2D AL = Axe[i] - Nrm[i] * (double)HalfW;
			const FVector2D AR = Axe[i] + Nrm[i] * (double)HalfW;
			const FVector2D BL = Axe[i + 1] - Nrm[i + 1] * (double)HalfW;
			const FVector2D BR = Axe[i + 1] + Nrm[i + 1] * (double)HalfW;
			const float ZA = Z[i], ZB = Z[i + 1];
			// SOUS-FACE : le tablier est une DALLE d'epaisseur constante, point.
			// (Premiere version rejetee sur capture : elle clampait la sous-face
			//  SOUS le sol rendu « pour fermer le raccord ». Au-dessus du lit de la
			//  Garonne, 10 m plus bas, cela transformait le pont en MUR PLEIN d'une
			//  rive a l'autre et bouchait la promenade — l'inverse exact du but.
			//  Aux atterrissages, la dalle de 90 cm s'enterre d'elle-meme : le sol y
			//  est a la cote du tablier, donc au-dessus de la sous-face.)
			const float SA = ZA - GBridgeDeckThickCm;
			const float SB = ZB - GBridgeDeckThickCm;

			// (a) LA CHAUSSEE. UV monde metrique, comme la dalle : le revetement reste
			//     en phase avec la rue qui arrive sur le pont.
			{
				const FVector3f Q[4] = { V3(AL, ZA), V3(AR, ZA), V3(BR, ZB), V3(BL, ZB) };
				const FVector2f UV[4] = { WorldUV(Q[0]), WorldUV(Q[1]), WorldUV(Q[2]), WorldUV(Q[3]) };
				QM.AddPoly(DeckGroup, Q, 4, Up, UV, DeckTint);
				++OutQuads;
			}
			// (b) LA SOUS-FACE. C'est elle qu'on voit depuis la promenade : elle est
			//     en pierre, pas en asphalte.
			{
				const FVector3f Q[4] = { V3(BL, SB), V3(BR, SB), V3(AR, SA), V3(AL, SA) };
				const FVector2f UV[4] = { WorldUV(Q[0]), WorldUV(Q[1]), WorldUV(Q[2]), WorldUV(Q[3]) };
				QM.AddPoly(StoneGroup, Q, 4, Down, UV, StoneTint);
				++OutQuads;
			}
			// (c) LES DEUX BANDEAUX (les flancs du tablier).
			{
				const FVector3f NG(-(float)Nrm[i].X, -(float)Nrm[i].Y, 0.f);
				const FVector3f G[4] = { V3(AL, ZA), V3(BL, ZB), V3(BL, SB), V3(AL, SA) };
				const FVector2f UVG[4] = {
					FVector2f(S[0], 0.f), FVector2f((float)(BL - AL).Size() * 0.01f, 0.f),
					FVector2f((float)(BL - AL).Size() * 0.01f, (ZB - SB) * 0.01f),
					FVector2f(0.f, (ZA - SA) * 0.01f) };
				AddStoneTwoSided(G, NG.GetSafeNormal(), UVG);
				const FVector3f ND((float)Nrm[i].X, (float)Nrm[i].Y, 0.f);
				const FVector3f D[4] = { V3(BR, ZB), V3(AR, ZA), V3(AR, SA), V3(BR, SB) };
				AddStoneTwoSided(D, ND.GetSafeNormal(), UVG);
				OutQuads += 2;
			}
			// (d) LES PARAPETS, seulement la ou l'ouvrage est REELLEMENT en l'air.
			if (bParapets && Clear[i] > GBridgeMinClearCm && Clear[i + 1] > GBridgeMinClearCm)
			{
				for (int32 Cote = 0; Cote < 2; ++Cote)
				{
					const double Sgn = (Cote == 0) ? -1.0 : 1.0;
					const FVector2D EA = Axe[i] + Nrm[i] * (Sgn * (double)HalfW);
					const FVector2D EB = Axe[i + 1] + Nrm[i + 1] * (Sgn * (double)HalfW);
					const FVector2D IA = Axe[i] + Nrm[i] * (Sgn * (double)(HalfW - GBridgeParapetWCm));
					const FVector2D IB = Axe[i + 1] + Nrm[i + 1] * (Sgn * (double)(HalfW - GBridgeParapetWCm));
					const float TA = ZA + GBridgeParapetHCm, TB = ZB + GBridgeParapetHCm;
					const FVector3f NExt((float)(Nrm[i].X * Sgn), (float)(Nrm[i].Y * Sgn), 0.f);
					// face EXTERIEURE — l'ordre des sommets s'inverse avec le cote,
					// sinon la face du cote droit sort a l'envers.
					FVector3f Ext[4];
					if (Sgn < 0.0)
					{
						Ext[0] = V3(EA, ZA); Ext[1] = V3(EB, ZB);
						Ext[2] = V3(EB, TB); Ext[3] = V3(EA, TA);
					}
					else
					{
						Ext[0] = V3(EB, ZB); Ext[1] = V3(EA, ZA);
						Ext[2] = V3(EA, TA); Ext[3] = V3(EB, TB);
					}
					const FVector2f UVE[4] = {
						FVector2f(0.f, 0.f), FVector2f((float)(EB - EA).Size() * 0.01f, 0.f),
						FVector2f((float)(EB - EA).Size() * 0.01f, GBridgeParapetHCm * 0.01f),
						FVector2f(0.f, GBridgeParapetHCm * 0.01f) };
					AddStoneTwoSided(Ext, NExt.GetSafeNormal(), UVE);
					// COURONNEMENT
					const FVector3f Cour[4] = { V3(IA, TA), V3(IB, TB), V3(EB, TB), V3(EA, TA) };
					const FVector2f UVC[4] = { WorldUV(Cour[0]), WorldUV(Cour[1]),
						WorldUV(Cour[2]), WorldUV(Cour[3]) };
					QM.AddPoly(StoneGroup, Cour, 4, Up, UVC, StoneTint);
					// face INTERIEURE (celle que voit l'automobiliste)
					const FVector3f NInt(-(float)(Nrm[i].X * Sgn), -(float)(Nrm[i].Y * Sgn), 0.f);
					const FVector3f Int[4] = { V3(IB, ZB), V3(IA, ZA), V3(IA, TA), V3(IB, TB) };
					AddStoneTwoSided(Int, NInt.GetSafeNormal(), UVE);
					OutQuads += 3;
				}
			}
		}

		// (e) BOUCHONS DE BOUT : le tablier est une piece fermee. Meme quand le bout
		//     n'est pas « libre », le bouchon ne coute rien et supprime tout jour au
		//     raccord (deux ouvrages voisins se recouvrent exactement au noeud).
		for (int32 Bout = 0; Bout < 2; ++Bout)
		{
			const int32 i = (Bout == 0) ? 0 : NSeg;
			const FVector2D L = Axe[i] - Nrm[i] * (double)HalfW;
			const FVector2D R = Axe[i] + Nrm[i] * (double)HalfW;
			const float Zt = Z[i];
			// Meme dalle d'epaisseur constante que les sections (cf. la note ci-dessus) ;
			// le bouchon s'enfonce d'un cran de plus pour ne laisser aucun jour au
			// raccord de deux ouvrages voisins.
			const float Zs = Zt - GBridgeDeckThickCm - GBridgeSinkCm;
			FVector2D Dir = (Bout == 0) ? (Axe[0] - Axe[1]) : (Axe[NSeg] - Axe[NSeg - 1]);
			Dir = Dir.IsNearlyZero() ? FVector2D(1.0, 0.0) : Dir.GetSafeNormal();
			const FVector3f N((float)Dir.X, (float)Dir.Y, 0.f);
			FVector3f Q[4];
			if (Bout == 0)
			{
				Q[0] = V3(L, Zt); Q[1] = V3(R, Zt); Q[2] = V3(R, Zs); Q[3] = V3(L, Zs);
			}
			else
			{
				Q[0] = V3(R, Zt); Q[1] = V3(L, Zt); Q[2] = V3(L, Zs); Q[3] = V3(R, Zs);
			}
			const FVector2f UV[4] = { FVector2f(0.f, 0.f), FVector2f(B.WidthCm * 0.01f, 0.f),
				FVector2f(B.WidthCm * 0.01f, (Zt - Zs) * 0.01f), FVector2f(0.f, (Zt - Zs) * 0.01f) };
			AddStoneTwoSided(Q, N, UV);
			++OutQuads;
		}

		// Le tablier POSE se nomme — comme la volee d'escalier. Sans cette ligne,
		// aucune enquete n'est possible (lecon payee par une enquete entiere au lot
		// QUAIS : un compte agrege ne nomme personne).
		UE_LOG(LogCityImport, Display,
			TEXT("PONTS C2 tablier POSE id=%s nom='%s' pos=%+d long=%.1f m larg=%.1f m z=%.2f..%.2f m hauteur_libre_max=%.2f m z_source=%s quads=%d parement_pose_des_DEUX_COTES=%d."),
			*B.Id, *B.Nom, B.Pos, RunCm * 0.01f, B.WidthCm * 0.01f,
			FMath::Min(Z[0], Z[NSeg]) * 0.01f, FMath::Max(Z[0], Z[NSeg]) * 0.01f,
			ClearMax * 0.01f, B.bHasZ ? TEXT("bdtopo3d") : TEXT("REPLI_RIVES"), OutQuads,
			TwoSidedQuads);
		return RunCm * 0.01f;
	}

	// TIRET de ligne axiale : un quad de 15 cm de large. Le debitage (3 m plein /
	// 1,5 m vide, 8 m d'ecart aux carrefours, dans la chaussee seulement) est fait
	// au prep — le C++ ne decide de rien ici.
	// v4 : Z du sol RENDU (le tiret n'est souleve que de 6 cm — sous la dalle il
	// disparait exactement comme la pierre).
	void BuildAxialDash(FCityMeshBuilder& QM, const FVector2D& ACm, const FVector2D& BCm,
		const FRenderedGroundZ& RGZ, const FResolvedSurface* Surf, const FVector3f& Tint)
	{
		const FVector2D D = BCm - ACm;
		const float Len = (float)D.Size();
		if (!Surf || Len < 1.f)
		{
			return;
		}
		const FVector2D Dir = D / Len;
		const FVector2D Lat(-Dir.Y, Dir.X);
		const float H = GAxialWidthCm * 0.5f;
		auto At = [&](const FVector2D& P, float Side)
		{
			const FVector2D Q = P + Lat * (Side * H);
			return FVector3f((float)Q.X, (float)Q.Y,
				RGZ.At(Q.X, Q.Y) + GMaskDashLiftCm);
		};
		const FVector3f P[4] = { At(ACm, -1.f), At(BCm, -1.f), At(BCm, 1.f), At(ACm, 1.f) };
		const FVector2f UV[4] = {
			FVector2f(0.f, 0.f), FVector2f(Len * 0.01f, 0.f),
			FVector2f(Len * 0.01f, GAxialWidthCm * 0.01f), FVector2f(0.f, GAxialWidthCm * 0.01f) };
		QM.AddPoly(QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material), P, 4,
			FVector3f(0, 0, 1), UV, Tint);
	}

	// Ruban de route TEXTURE : UN SEUL quad par segment (chaussee + trottoirs + bordures
	// + marquage peints dans T_RoadStrip, slot Glass des cellules de sol). L'ancienne
	// version geometrique (asphalte + 2 trottoirs + tirets, ~500-700 k tris de tirets
	// sub-pixel au-dela de 300 m) plafonnait le panorama a 45 fps. En prime : plus de
	// z-fight de marquage (aucune geometrie superposee). Les sentiers pietons restent
	// un ruban uni sur le slot Wall (ils n'avaient deja ni trottoir ni marquage).
	// Empilement decimetrique (Adreno/GLES sans reversed-Z) : dalle 0 < parc < eau <
	// rail < route 55+. TerrainZ (desktop) : Z terrain par sommet, l'empilement
	// devient RELATIF au terrain ; nul = plat historique (mobile).
	// bBakedShade=false (desktop PBR) : teinte brute, Lumen eclaire (plus de Shade cuit).
	// Surf (J3c point 2) : classe de revetement resolue — le ruban part dans un groupe
	// de polygones dedie avec une UV0 EN METRES (U = abscisse curviligne, V = position
	// transversale) au lieu du slot Glass + T_RoadStrip. Nul = chemin historique.
	// SurfPlain + NearJunction (v2) : sur les segments dont un sommet touche un
	// carrefour, la classe bascule vers l'asphalte NU — les tirets axiaux ne
	// traversent plus les croisements. Le Z reste celui de la classe D'ORIGINE sur
	// toute la longueur : pas de marche au raccord.
	// SurfCurb + SurfSlab + Junctions (v5 « voirie ») : la CHAUSSEE AUTO n'est plus un
	// quad unique — bande centrale de classe chaussee, deux bordures en relief de
	// 12 cm et deux rives de 1,70 m en classe DALLE (cf. GSidewalkWidthCm). Le tout
	// nul = chemin d'avant, a l'octet pres (mobile, gravier, sentiers).
	void BuildRoad(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float WidthCm,
		const FString& Type, int32 RoadIndex, const TArray<float>* TerrainZ = nullptr,
		bool bBakedShade = true, const FResolvedSurface* Surf = nullptr,
		const FResolvedSurface* SurfPlain = nullptr, const TArray<uint8>* NearJunction = nullptr,
		const FResolvedSurface* SurfCurb = nullptr, const FResolvedSurface* SurfSlab = nullptr,
		const FJunctionMap* Junctions = nullptr, int32* OutCurbQuads = nullptr,
		// PARTITION : le SOCLE du ruban. 55 cm = la valeur historique de la voirie
		// (elle survolait une dalle plate) ; tout appel qui ne le precise pas garde
		// donc exactement le meme Z, bit pour bit. Les rubans de BANDE, eux, se
		// posent sur une dalle DRAPEE : il n'y a plus rien a survoler.
		float ZBaseCm = 55.f,
		// PARTITION : ECHELLE DE DEMI-LARGEUR PAR SOMMET, entre 0 et 1. Un
		// decalage constant applique a des normales de SOMMET se CROISE des que la
		// ligne tourne serre — le quad s'inverse et on obtient un noeud papillon
		// (c'est le meme defaut que les 73 murs sur 239 du lot BERGES, corrige la
		// par le meme rabotage). Nul = comportement historique, bit pour bit.
		const TArray<float>* HalfScale = nullptr)
	{
		const int32 N = PtsCm.Num();
		if (N < 2)
		{
			return;
		}
		// v1 (mobile) : jitter par ordre d'arrivee — a 0,8 cm de pas, deux routes qui se
		// croisent scintillaient au-dela de ~1 km (precision depth GLES).
		// v2 (revetements) : offset DETERMINISTE PAR CLASSE (gravier bas ... dalles
		// haut) + un micro-jitter de 0 a 1,2 cm pour departager deux rubans COPLANAIRES
		// de la meme classe, dix fois plus petit que le pas entre classes : l'ordre
		// entre classes ne s'inverse jamais.
		const float ZRoad = Surf
			? ZBaseCm + Surf->Class->ZClassCm + (RoadIndex % 4) * 0.4f
			: ZBaseCm + (RoadIndex % 7) * 4.f;
		const bool bWalkway = Type == TEXT("footway") || Type == TEXT("path") || Type == TEXT("cycleway");
		const bool bMarking = !bWalkway && WidthCm >= 550.f;
		const bool bSolid = Type == TEXT("primary") || Type == TEXT("secondary");
		const float WalkW = bWalkway ? 0.f : 170.f;
		// v5 — RUE COMPLETE : reserve aux CHAUSSEES AUTO. Une allee de gravier ou une
		// piste cyclable n'a pas de bordure ; elle garde son ruban d'un seul tenant.
		const bool bStreet = Surf && SurfCurb && SurfSlab && Surf->Class->bAuto;
		const float RoadHalf = WidthCm * 0.5f;
		// La demi-largeur TOTALE du ruban : chaussee + chant + rive en rue complete
		// (RibbonHalfCm cote appelant applique la MEME regle — c'est elle qui fixe le
		// rayon des patchs de carrefour).
		const float Half = bStreet ? RoadHalf + GCurbTopWidthCm + GSidewalkWidthCm
			: WidthCm * 0.5f + WalkW;
		const FVector3f Up(0, 0, 1);
		// Teinte de base CLAIRE : les bandes de la texture assombrissent (asphalte 0,2x,
		// trottoir 0,59x, marquage 1x) pour retrouver les couleurs historiques.
		const FVector3f Base = bWalkway ? FVector3f(0.48f, 0.45f, 0.42f) : FVector3f(0.85f, 0.85f, 0.80f);
		const FVector3f Shaded = bBakedShade ? Shade(Base, Up, ZRoad) : Base;

		// Normales par sommet (moyenne des segments adjacents).
		TArray<FVector2D> Nrm;
		Nrm.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			FVector2D D(0, 0);
			if (i > 0) { D += (PtsCm[i] - PtsCm[i - 1]).GetSafeNormal(); }
			if (i < N - 1) { D += (PtsCm[i + 1] - PtsCm[i]).GetSafeNormal(); }
			D = D.GetSafeNormal();
			Nrm[i] = FVector2D(-D.Y, D.X);
		}

		float Arc = 0.f;
		for (int32 i = 0; i + 1 < N; ++i)
		{
			const FVector2D A = PtsCm[i], B = PtsCm[i + 1];
			// v5 : en rue complete, le quad de CLASSE ne couvre plus que la chaussee —
			// les rives partent dans le groupe de la dalle et la bordure les separe.
			const float QuadHalf = bStreet ? RoadHalf : Half;
			// Rabotage anti-retournement, sommet par sommet (cf. HalfScale).
			const float SA = HalfScale && HalfScale->IsValidIndex(i) ? (*HalfScale)[i] : 1.f;
			const float SB = HalfScale && HalfScale->IsValidIndex(i + 1) ? (*HalfScale)[i + 1] : 1.f;
			const FVector2D NA = Nrm[i] * (QuadHalf * SA), NB = Nrm[i + 1] * (QuadHalf * SB);
			const float SegLen = (B - A).Size();
			const float ZA = ZRoad + (TerrainZ ? (*TerrainZ)[i] : 0.f);
			const float ZB = ZRoad + (TerrainZ ? (*TerrainZ)[i + 1] : 0.f);
			const FVector3f P[4] = {
				FVector3f(A.X - NA.X, A.Y - NA.Y, ZA), FVector3f(B.X - NB.X, B.Y - NB.Y, ZB),
				FVector3f(B.X + NB.X, B.Y + NB.Y, ZB), FVector3f(A.X + NA.X, A.Y + NA.Y, ZA) };
			if (bStreet)
			{
				BuildStreetSides(QM, A, B, Nrm[i], Nrm[i + 1], ZA, ZB, Arc, SegLen, RoadHalf,
					SurfCurb, SurfSlab, Shaded, Junctions, OutCurbQuads);
			}
			if (Surf)
			{
				// Segment au contact d'un carrefour : classe de remplacement NUE
				// (asphalte tuile) pour ne pas tirer un tiret axial en travers du
				// croisement. Seules les classes MARQUEES (bFullWidth) sont concernees.
				const FResolvedSurface* SegSurf = Surf;
				if (SurfPlain && NearJunction && Surf->Class->bFullWidth &&
					((*NearJunction)[i] || (*NearJunction)[i + 1]))
				{
					SegSurf = SurfPlain;
				}
				// UV0 en METRES : U = abscisse curviligne, V = position transversale.
				// bFullWidth : V couvre exactement AcrossM sur TOUTE la largeur du
				// quad (la ligne axiale integree au scan tombe au milieu) ; sinon V
				// est la distance transversale reelle et la texture tuile aussi.
				// v5 : en rue complete le quad EST la chaussee — la ligne axiale peinte
				// tombe donc au milieu de la CHAUSSEE et non plus au milieu de
				// chaussee + trottoirs (elle etait decalee de toute la largeur d'un
				// trottoir sur les rues etroites).
				const float Along0 = Arc * 0.01f;
				const float Along1 = (Arc + SegLen) * 0.01f;
				const float AcrossMax = SegSurf->Class->bFullWidth
					? SegSurf->Class->AcrossM : (2.f * QuadHalf * 0.01f);
				const float AlongAt[4] = { Along0, Along1, Along1, Along0 };
				const float AcrossAt[4] = { 0.f, 0.f, AcrossMax, AcrossMax };
				FVector2f UV[4];
				for (int32 c = 0; c < 4; ++c)
				{
					UV[c] = SegSurf->Class->bSwapUV
						? FVector2f(AcrossAt[c], AlongAt[c])
						: FVector2f(AlongAt[c], AcrossAt[c]);
				}
				QM.AddPoly(QM.GetOrCreateGroup(SegSurf->SlotName(), SegSurf->Material), P, 4, Up, UV, Shaded);
			}
			else if (bWalkway)
			{
				QM.AddQuad(QM.WallGroup, P[0], P[1], P[2], P[3], Up, Shaded);
			}
			else
			{
				// V : tirets qui defilent avec l'abscisse ; ligne continue = V fixe en
				// zone peinte ; pas de marquage = V fixe en zone vide de la texture.
				float V0, V1;
				if (!bMarking) { V0 = V1 = 0.75f; }
				else if (bSolid) { V0 = V1 = 0.25f; }
				else { V0 = Arc / 600.f; V1 = (Arc + SegLen) / 600.f; }
				const FVector2f UV[4] = {
					FVector2f(0, V0), FVector2f(0, V1), FVector2f(1, V1), FVector2f(1, V0) };
				QM.AddPoly(QM.GlassGroup, P, 4, Up, UV, Shaded);
			}
			Arc += SegLen;
		}
	}

	// Arbre low-poly : tronc 6 faces + 2 etages de feuillage coniques.
	void BuildTree(FCityMeshBuilder& QM)
	{
		const FVector3f Trunk(0.32f, 0.22f, 0.14f);
		const FVector3f Leaf(0.28f, 0.45f, 0.22f);
		const int32 Sides = 6;
		auto Ring = [&](float R, float Z, int32 i) {
			const float Ang = 2.f * PI * i / Sides;
			return FVector3f(R * FMath::Cos(Ang), R * FMath::Sin(Ang), Z);
		};
		for (int32 i = 0; i < Sides; ++i)
		{
			const FVector3f N = (Ring(1.f, 0.f, i) + Ring(1.f, 0.f, i + 1)).GetSafeNormal();
			// Tronc
			QM.AddQuad(QM.WallGroup, Ring(14.f, 0.f, i), Ring(14.f, 0.f, i + 1), Ring(11.f, 260.f, i + 1),
				Ring(11.f, 260.f, i), N, Trunk);
			// Feuillage : cone bas + cone haut
			QM.AddTri(QM.WallGroup, Ring(170.f, 220.f, i), Ring(170.f, 220.f, i + 1), FVector3f(0, 0, 460.f),
				(N + FVector3f(0, 0, 0.5f)).GetSafeNormal(), Shade(Leaf, (N + FVector3f(0, 0, 0.5f)).GetSafeNormal(), 300.f));
			QM.AddTri(QM.WallGroup, Ring(170.f, 220.f, i + 1), Ring(170.f, 220.f, i), FVector3f(0, 0, 160.f),
				FVector3f(0, 0, -1), Shade(Leaf * 0.6f, FVector3f(0, 0, -1), 200.f));
		}
	}

	// Fosses de plantation : cote REEL de voirie, quasi constant (0,9 a 1,2 m en
	// France ; on retient 1,1-1,3 m). Le mesh est genere au cote de reference et
	// chaque instance ne recoit qu'une echelle de +-8 %.
	//
	// v2 (LOT3) — VERDICT UTILISATEUR sur la v1 : « les fosses sont MOCHES ». Elles
	// etaient un DISQUE plat de gravier beige clair pose sur du pave gris : un
	// autocollant, pas du mobilier urbain. Trois defauts, trois corrections :
	//   1. FORME : un CARRE a cadre fin, comme une vraie fosse d'arbre de rue
	//      (entourage beton/pierre + terre en creux). Le cadre affleure le pave,
	//      l'interieur est EN CREUX derriere un biseau : c'est ce decrochement qui
	//      fait lire un objet et non un decalque.
	//   2. MATIERE : de la TERRE SOMBRE (scan dirt_ground) pour l'interieur, la
	//      matiere de bordure pour le cadre. Plus de gravier beige.
	//   3. LUMIERE : la terre s'assombrit vers le tronc (vertex color lue par le
	//      materiau de la fosse), comme l'ombre portee du houppier.
	// v3 (LOT4) — VERDICT UTILISATEUR sur la v2, captures a l'appui. Deux defauts :
	//   1. TAILLE UNIQUE : l'empattement des gros arbres (le disque de litiere/racines
	//      du mesh Megascans, jusqu'a 1,2 m de rayon a pleine echelle) DEBORDAIT d'une
	//      fosse de 1,2 m, et 1 256 fosses identiques faisaient un motif repete.
	//   2. FOSSE PLATE SUR SOL NON PLAT : la dalle rendue est grossiere (~7,8 m par
	//      triangle) et pentue par endroits, si bien qu'un carre HORIZONTAL pose au Z
	//      du seul centre avait des coins immerges, des coins flottants, et surtout un
	//      FOND a -1,5 cm sous le pave : la terre etait MASQUEE et on voyait du pave
	//      dans le cadre.
	// v3 : cote PROPORTIONNEL a l'empattement mesure sur le mesh, et pose sur le PLAN
	// LOCAL du sol (9 traces sous l'emprise) avec le fond garanti au-dessus de la dalle.
	constexpr float GPitRefM = 1.2f;      // cote du carre de REFERENCE (celui du mesh)
	constexpr float GPitMinM = 1.0f;      // plus petite fosse admise
	constexpr float GPitMaxM = 2.2f;      // plus grande (au-dela ce n'est plus une fosse)
	constexpr float GPitRootMarginM = 0.30f;  // marge demandee autour de l'empattement
	constexpr float GPitRootClearCm = 5.f;    // jeu entre l'empattement et le cadre
	// Empattement = rayon XY des sommets du mesh sous cette hauteur LOCALE. Le disque
	// de litiere des arbres Megascans y est inclus : c'est precisement lui qui debordait.
	constexpr float GPitBasalHeightCm = 40.f;
	constexpr float GPitBasalPct = 0.98f;     // centile : un sommet aberrant ne dicte rien
	// v2b — MESURE PAR CAPTURE de la v2a : le cadre etait INVISIBLE. Pose a plat
	// 1 cm au-dessus du pave et vetu de M_Surf_curb (la dalle a peine assombrie de
	// 8 %), il ne dessinait rien : la fosse se lisait comme une tache sombre, pas
	// comme du mobilier. Trois corrections, toutes geometriques ou de matiere :
	//   - entourage plus LARGE (12 cm) : au-dela de l'empattement des racines ;
	//   - entourage plus HAUT (2,5 cm) avec une JUPE verticale qui rejoint le pave :
	//     c'est l'arete eclairee et son ombre qui font lire un objet pose ;
	//   - matiere DEDIEE (pack treepit_frame, la dalle assombrie de moitie) au lieu
	//     de la bordure de trottoir.
	constexpr float GPitFrameCm = 12.f;   // largeur du cadre (entourage)
	constexpr float GPitBevelCm = 3.f;    // course horizontale du biseau
	constexpr int32 GPitSegPerSide = 4;   // subdivision du bord interieur (degrade)
	// v3 — LE defaut visuel de la v2 : le fond de terre etait a -1,5 cm SOUS le pave,
	// donc masque par lui. Le creux existe toujours, mais il se mesure desormais par
	// rapport au CADRE, et le fond reste au-dessus de la dalle EN TOUT POINT :
	//   fond = haut du cadre - GPitSinkCm, et haut du cadre = plan local + GPitLiftCm
	//   avec GPitLiftCm = GPitSinkCm + GPitClearCm  =>  fond = plan local + Clear.
	constexpr float GPitSinkCm = 3.f;     // fond de terre SOUS le haut du cadre
	constexpr float GPitClearCm = 1.f;    // garde du fond AU-DESSUS de la dalle
	constexpr float GPitLiftCm = GPitSinkCm + GPitClearCm;  // haut du cadre / dalle
	// La jupe SCELLE visuellement le cadre au pave. Sur un sol non plan, la fosse est
	// calee sur le point le plus HAUT de son emprise : la jupe doit descendre d'autant
	// que la gite locale, sinon un jour s'ouvre du cote bas. 16 cm couvrent une
	// non-planeite de 12 cm sous l'emprise ; au-dela on lit une jardiniere sur pente,
	// ce qui est le comportement voulu (et non un carre qui flotte).
	constexpr float GPitSkirtCm = 16.f;
	// Gite maximale reprise par la pose : au-dela, la pente vient d'une donnee suspecte
	// (bord de dalle, triangle degenere) et on prefere l'horizontale a une fosse folle.
	constexpr float GPitMaxSlope = 0.36f;   // tan(20 deg)
	// v3b — MESURE de la v3a : caler la fosse sur le point le PLUS HAUT de son emprise
	// la met en l'air des qu'un sondage tombe sur autre chose que la rue (marche de
	// bordure, dalle superposee, pied de mur) : 0,96 % des fosses planaient de plus de
	// 15 cm, jusqu'a 1,04 m. Deux garde-fous, dans cet ordre :
	//   - les points a plus de GPitOutlierCm du plan ajuste sont des points d'UNE AUTRE
	//     SURFACE : on les jette et on rajuste le plan sur les survivants ;
	//   - le relevement restant est borne : mieux vaut un cadre qui mord une marche
	//     (la jupe de 16 cm le scelle) qu'une fosse qui plane au-dessus de son arbre.
	constexpr float GPitOutlierCm = 20.f;
	constexpr float GPitMaxLiftCm = 8.f;
	constexpr float GPitShadeCenter = 0.45f;  // teinte au pied du tronc
	constexpr float GPitShadeEdge = 1.f;      // teinte au bord de la fosse
	// Alignement : une fosse reelle est alignee sur sa rue. On cherche la bordure la
	// plus proche dans les masques de sol ; au-dela de cette portee, on retombe sur un
	// lacet QUASI NUL (un carre a 45 deg au milieu d'un trottoir ferait faux).
	constexpr float GPitAlignRangeCm = 3000.f;
	constexpr float GPitJitterDeg = 3.f;      // jeu de pose autour de l'axe de la rue
	constexpr float GPitFreeJitterDeg = 10.f; // sans rue identifiee

	// Touffes d'herbe : PLUS AUCUN FONDU (V6, decision utilisateur), et toujours pas
	// d'ombre portee.
	//
	// Le fondu 45-60 m datait du contexte MOBILE, ou les touffes etaient des maillages
	// a UN SEUL LOD de 6 271 a 30 767 triangles : les tenir a distance etait le seul
	// levier existant. Son prix, mesure en v4 : ZERO touffe dessinee des 100 m
	// d'altitude, et une pelouse qui « repousse » en descendant sous 60 m (c'etait le
	// SEUL cull du niveau — l'effet « sous-marin » signale par l'utilisateur).
	//
	// V6 : les 12 SM_KikuyuGrass_* sont passes en NANITE (repli 476 a 3 031 triangles
	// contre 771 a 30 767 en source). La distance n'est plus une charge — Nanite choisit
	// lui-meme la finesse selon la taille a l'ecran. Mesure ProfileGPU (editeur AU
	// PREMIER PLAN, sinon la frame profilee est une frame d'interface), sur la pelouse
	// la plus dense du proto, 5 460 touffes dans 60 m = 69,8 M triangles source :
	//     pose        avant (1 LOD, cull 45/60)   Nanite meme cull   Nanite SANS cull
	//     ras du sol           5,30 ms                 3,42 ms            3,42 ms
	//     50 m                 2,74 ms                 2,75 ms            3,01 ms
	//     150 m                2,32 ms                 2,57 ms            2,50 ms
	//     temoin ciel          1,59 ms                 1,80 ms            1,49 ms
	// (le temoin ciel donne le plancher de bruit : +-0,3 ms). Rendre l'herbe A TOUTE
	// ALTITUDE coute donc MOINS que ce que coutait l'ancienne herbe au ras du sol.
	// Zero = aucun fondu : un plafond serait une depense sans contrepartie mesuree.
	constexpr float GClumpCullStartCm = 0.f;
	constexpr float GClumpCullEndCm = 0.f;

	// -----------------------------------------------------------------------------
	// LOT6 point A/D — RETRACTION DES PLANTATIONS QUI MORDENT LA CHAUSSEE.
	//
	// Le semis vient du LiDAR, qui donne l'APEX DE COURONNE et non le pied : un arbre
	// d'alignement penche naturellement vers la rue, si bien que sa position tombe trop
	// pres du bord — sa fosse se retrouve a cheval sur la chaussee (defaut signale,
	// captures a l'appui). Ces alignements de boulevard sont REELS et precieux : on ne
	// les supprime pas, ON LES RECULE. Meme regle pour les haies (point D) : leur filtre
	// d'epoque utilisait les buffers OSM alors que la voirie VISIBLE est la chaussee
	// peinte BD TOPO, d'ou des haies au milieu de la route.
	//
	// Le masque de sol PORTE DEJA le champ de distance qu'il faut : son canal G est la
	// distance SIGNEE a la chaussee. Il donne donc a la fois la DIRECTION (son gradient)
	// et la DISTANCE (sa valeur) — rien a re-deriver.
	// Regle : reculer jusqu'a ce que le degagement demande (demi-emprise + marge) soit
	// atteint ; au-dela de la course maximale (rue etroite bordee des deux cotes), on ne
	// pose PAS l'instance et on la compte. Jamais de deplacement devine, jamais de
	// suppression silencieuse.
	constexpr float GRetraitMargeCm = 20.f;   // degagement demande au-dela de l'emprise
	constexpr float GRetraitMaxCm = 250.f;    // course maximale d'une retraction
	constexpr float GRetraitPasCm = 10.f;     // pas de la marche le long du gradient
	// Le gradient se lit par differences finies : le pas doit depasser LA TAILLE D'UN
	// PIXEL de masque (500 m / 1024 = 48,8 cm), sinon les deux echantillons tombent dans
	// le meme pixel et le gradient est nul par construction.
	constexpr float GRetraitGradPasCm = 60.f;
	constexpr int32 GRetraitDirs = 32;        // directions du balayage de secours

	// -----------------------------------------------------------------------------
	// LOT6 point C — FOSSE RONDE DES ARBUSTES ET DES HAIES SUR SOL MINERAL.
	//
	// Verdict utilisateur : les haies Elderberry posees sur du pave n'ont ni socle ni
	// racines, ce sont des tiges fines plantees dans la pierre. Meme remede que pour les
	// arbres, a l'echelle de l'arbuste : la voirie decoupe une petite fosse RONDE (les
	// fosses d'arbustes reelles le sont), avec un fin anneau de cadre et de la terre en
	// leger MONTICULE vers la tige — c'est le monticule qui fait lire un pied enracine,
	// la ou le creux de la fosse d'arbre fait lire une cuvette d'arrosage.
	// Les haies plantees DANS L'HERBE n'en ont pas : il n'y a rien a decouper.
	constexpr float GShrubPitRefM = 0.9f;     // diametre du disque de REFERENCE (mesh)
	constexpr float GShrubPitMinM = 0.6f;     // plus petite fosse d'arbuste
	constexpr float GShrubPitMaxM = 0.9f;     // plus grande
	constexpr float GShrubPitMarginCm = 10.f; // marge autour de la touffe de tiges
	// MESURE (run du 31/07) : l'empattement rendu par ComputeBasalRadiusCm sur un
	// Elderberry vaut 87 a 202 cm A SON ECHELLE — c'est l'etalement du BUISSON ENTIER,
	// ses basses branches comprises, et non la touffe de tiges. Le prendre tel quel
	// saturait 100 % des fosses au plafond de 0,9 m : 268 disques identiques, exactement
	// le motif repete deja reproche aux fosses carrees v2. La decoupe du revetement suit
	// LES TIGES, soit environ un cinquieme de cet etalement — ce qui redonne toute la
	// plage 0,6-0,9 m demandee, proportionnelle a la taille de l'arbuste.
	constexpr float GShrubPitBasalFrac = 0.20f;
	// Une fosse est une decoupe HORIZONTALE dans un revetement : couchee a 25 deg sur un
	// talus, elle ne se lit plus comme telle (meme verdict que pour les fosses carrees au
	// lot5). Au-dela du seuil, l'ARBUSTE RESTE — un buisson sur talus est credible — mais
	// il n'a PAS de fosse. On ne supprime pas la plante, on renonce a la decoupe.
	constexpr float GShrubPitMaxSlopeDeg = 15.f;
	constexpr float GShrubFrameCm = 5.f;      // FIN anneau de cadre (vs 12 cm pour l'arbre)
	constexpr float GShrubMoundCm = 5.f;      // hauteur du monticule de terre au centre
	constexpr int32 GShrubSegs = 20;          // segments du disque
	constexpr float GShrubSkirtCm = 10.f;     // jupe qui scelle l'anneau au pave

	// FOSSE RONDE : meme convention de repere que la fosse carree — Z = 0 est le HAUT DE
	// L'ANNEAU, l'instance est posee sur le plan local du sol a GPitLiftCm au-dessus du
	// point le plus haut de son emprise, si bien que le point le plus BAS de la terre
	// (Z local = -GPitSinkCm, au pourtour) reste GPitClearCm AU-DESSUS de la dalle.
	// Le centre monte a -GPitSinkCm + GShrubMoundCm : de la terre AMASSEE contre la tige.
	//   WallGroup  = la TERRE (seule a porter une vertex color utile) ;
	//   GlassGroup = l'ANNEAU et sa JUPE (matiere de bordure).
	void BuildRoundPit(FCityMeshBuilder& QM, float RadiusCm)
	{
		const FVector3f Up(0.f, 0.f, 1.f);
		const FVector3f Blanc(1.f, 1.f, 1.f);
		const float InnerCm = FMath::Max(RadiusCm - GShrubFrameCm, 1.f);
		const float ZSoilBord = -GPitSinkCm;
		const float ZSoilCentre = ZSoilBord + GShrubMoundCm;
		auto UVof = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };

		for (int32 k = 0; k < GShrubSegs; ++k)
		{
			const float A0 = (float)(2.0 * PI * k / GShrubSegs);
			const float A1 = (float)(2.0 * PI * (k + 1) / GShrubSegs);
			const FVector2f D0(FMath::Cos(A0), FMath::Sin(A0));
			const FVector2f D1(FMath::Cos(A1), FMath::Sin(A1));

			// --- l'ANNEAU : couronne plate au niveau du pave -------------------------
			{
				const FVector3f P[4] = {
					FVector3f(D0.X * RadiusCm, D0.Y * RadiusCm, 0.f),
					FVector3f(D1.X * RadiusCm, D1.Y * RadiusCm, 0.f),
					FVector3f(D1.X * InnerCm, D1.Y * InnerCm, 0.f),
					FVector3f(D0.X * InnerCm, D0.Y * InnerCm, 0.f) };
				const FVector2f UV[4] = { UVof(P[0]), UVof(P[1]), UVof(P[2]), UVof(P[3]) };
				QM.AddPoly(QM.GlassGroup, P, 4, Up, UV, Blanc);
			}
			// --- la JUPE : la face verticale, du haut de l'anneau jusque sous le pave --
			{
				const FVector2f Sortante = (D0 + D1) * 0.5f;
				const FVector3f N(Sortante.X, Sortante.Y, 0.f);
				const FVector3f P[4] = {
					FVector3f(D0.X * RadiusCm, D0.Y * RadiusCm, 0.f),
					FVector3f(D1.X * RadiusCm, D1.Y * RadiusCm, 0.f),
					FVector3f(D1.X * RadiusCm, D1.Y * RadiusCm, -GShrubSkirtCm),
					FVector3f(D0.X * RadiusCm, D0.Y * RadiusCm, -GShrubSkirtCm) };
				const FVector2f UV[4] = {
					FVector2f(P[0].X * 0.01f, P[0].Y * 0.01f),
					FVector2f(P[1].X * 0.01f, P[1].Y * 0.01f),
					FVector2f(P[2].X * 0.01f, (P[2].Y - GShrubSkirtCm) * 0.01f),
					FVector2f(P[3].X * 0.01f, (P[3].Y - GShrubSkirtCm) * 0.01f) };
				QM.AddPoly(QM.GlassGroup, P, 4, N.GetSafeNormal(), UV, Blanc);
			}
			// --- la TERRE : eventail du centre (haut) vers le bord interieur (bas) -----
			// Le degrade de vertex color assombrit le pied de la tige, comme la fosse
			// carree assombrit le pied du tronc.
			{
				const FVector3f P[3] = {
					FVector3f(0.f, 0.f, ZSoilCentre),
					FVector3f(D0.X * InnerCm, D0.Y * InnerCm, ZSoilBord),
					FVector3f(D1.X * InnerCm, D1.Y * InnerCm, ZSoilBord) };
				const FVector2f UV[3] = { UVof(P[0]), UVof(P[1]), UVof(P[2]) };
				const FVector3f Cols[3] = {
					FVector3f(GPitShadeCenter, GPitShadeCenter, GPitShadeCenter),
					FVector3f(GPitShadeEdge, GPitShadeEdge, GPitShadeEdge),
					FVector3f(GPitShadeEdge, GPitShadeEdge, GPitShadeEdge) };
				// Normale du cone : penchee vers l'exterieur, sinon le monticule prend la
				// lumiere exactement comme le pave et ne se lit pas en relief.
				const FVector2f Sortante = (D0 + D1) * 0.5f;
				const FVector3f N = FVector3f(Sortante.X * GShrubMoundCm,
					Sortante.Y * GShrubMoundCm, InnerCm).GetSafeNormal();
				QM.AddPolyPerVertexColors(QM.WallGroup, P, 3, N, UV, Cols);
			}
		}
	}

	// FOSSE DE PLANTATION v2 : le carre de terre a cadre, au pied d'un arbre de rue.
	// Objet urbain REEL — la voirie decoupe le revetement autour du tronc et borde la
	// coupe d'un entourage — et non un disque pose dessus.
	//
	// Repere local : Z = 0 est le HAUT DU CADRE. v3 : l'instance est posee sur le PLAN
	// LOCAL du sol (pitch/roll compris), a GPitLiftCm au-dessus du point le plus haut
	// de son emprise, si bien que le fond de terre (Z local = -GPitSinkCm) reste
	// GPitClearCm AU-DESSUS de la dalle partout. Une seule face vers le haut partout
	// (une fosse ne se voit jamais par en dessous).
	//
	// Deux groupes de polygones :
	//   WallGroup  = l'INTERIEUR (terre), le seul a porter une vertex color utile —
	//                son materiau la lit et s'assombrit vers le tronc ;
	//   GlassGroup = le CADRE et son BISEAU (matiere de bordure).
	//
	// Les UV sont EN METRES : les materiaux M_Surf_* divisent l'UV0 par la taille
	// physique du scan, donc un UV metrique donne l'echelle reelle quel que soit le
	// pack.
	void BuildPlantingPit(FCityMeshBuilder& QM, float HalfCm)
	{
		const FVector3f Up(0.f, 0.f, 1.f);
		const FVector3f Blanc(1.f, 1.f, 1.f);   // cadre : materiau sans vertex color
		const float InnerCm = HalfCm - GPitFrameCm;              // bord interieur du cadre
		const float SoilCm = FMath::Max(InnerCm - GPitBevelCm, 1.f);  // bord du fond
		const float ZSoil = -GPitSinkCm;

		// Les 4 coins unitaires, dans le sens trigonometrique vu du dessus.
		const FVector2f C[4] = { FVector2f(-1.f, -1.f), FVector2f(1.f, -1.f),
			FVector2f(1.f, 1.f), FVector2f(-1.f, 1.f) };
		auto Pt = [](const FVector2f& U, float R, float Z)
		{
			return FVector3f(U.X * R, U.Y * R, Z);
		};
		auto UVof = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };

		for (int32 i = 0; i < 4; ++i)
		{
			const FVector2f& U0 = C[i];
			const FVector2f& U1 = C[(i + 1) % 4];

			// --- le CADRE : couronne carree a onglet, a plat au niveau du pave ---
			{
				const FVector3f P[4] = { Pt(U0, HalfCm, 0.f), Pt(U1, HalfCm, 0.f),
					Pt(U1, InnerCm, 0.f), Pt(U0, InnerCm, 0.f) };
				const FVector2f UV[4] = { UVof(P[0]), UVof(P[1]), UVof(P[2]), UVof(P[3]) };
				QM.AddPoly(QM.GlassGroup, P, 4, Up, UV, Blanc);
			}

			// --- la JUPE : la face verticale de l'entourage, du haut du cadre jusque
			// SOUS le pave. C'est elle qui fait exister l'objet — une arete verticale
			// prend la lumiere autrement que le sol et pose une ombre au pied.
			{
				const FVector2f Sortante = (U0 + U1) * 0.5f;
				const FVector3f N(Sortante.X, Sortante.Y, 0.f);
				const FVector3f P[4] = { Pt(U0, HalfCm, 0.f), Pt(U1, HalfCm, 0.f),
					Pt(U1, HalfCm, -GPitSkirtCm), Pt(U0, HalfCm, -GPitSkirtCm) };
				const FVector2f UV[4] = {
					FVector2f(P[0].X * 0.01f, P[0].Y * 0.01f),
					FVector2f(P[1].X * 0.01f, P[1].Y * 0.01f),
					FVector2f(P[2].X * 0.01f, (P[2].Y - GPitSkirtCm) * 0.01f),
					FVector2f(P[3].X * 0.01f, (P[3].Y - GPitSkirtCm) * 0.01f) };
				QM.AddPoly(QM.GlassGroup, P, 4, N.GetSafeNormal(), UV, Blanc);
			}

			// --- le BISEAU : du bord interieur du cadre vers le fond, en pente ---
			// Normale dans le plan vertical du cote : vers le HAUT et vers l'INTERIEUR
			// (paroi de cuvette). Sans elle, la pente prendrait la lumiere comme le
			// pave et le decrochement ne se lirait pas.
			{
				const FVector2f Sortante = (U0 + U1) * 0.5f;   // (0,-1), (1,0), (0,1), (-1,0)
				const FVector3f N = FVector3f(-Sortante.X * GPitSinkCm,
					-Sortante.Y * GPitSinkCm, GPitBevelCm).GetSafeNormal();
				const FVector3f P[4] = { Pt(U0, InnerCm, 0.f), Pt(U1, InnerCm, 0.f),
					Pt(U1, SoilCm, ZSoil), Pt(U0, SoilCm, ZSoil) };
				const FVector2f UV[4] = { UVof(P[0]), UVof(P[1]), UVof(P[2]), UVof(P[3]) };
				QM.AddPoly(QM.GlassGroup, P, 4, N, UV, Blanc);
			}
		}

		// --- le FOND DE TERRE : eventail depuis le centre, subdivise sur le pourtour.
		// L'eventail interpole lineairement la couleur du centre vers le bord : c'est
		// lui qui porte l'assombrissement au pied du tronc, sans texture ni decal.
		const int32 NSeg = 4 * GPitSegPerSide;
		auto Bord = [&](int32 k)
		{
			const int32 Cote = (k / GPitSegPerSide) % 4;
			const float T = (float)(k % GPitSegPerSide) / (float)GPitSegPerSide;
			const FVector2f U = C[Cote] + (C[(Cote + 1) % 4] - C[Cote]) * T;
			return Pt(U, SoilCm, ZSoil);
		};
		const FVector3f Centre(0.f, 0.f, ZSoil);
		const FVector3f ColCentre(GPitShadeCenter, GPitShadeCenter, GPitShadeCenter);
		const FVector3f ColBord(GPitShadeEdge, GPitShadeEdge, GPitShadeEdge);
		for (int32 k = 0; k < NSeg; ++k)
		{
			const FVector3f P[3] = { Centre, Bord(k), Bord((k + 1) % NSeg) };
			const FVector2f UV[3] = { UVof(P[0]), UVof(P[1]), UVof(P[2]) };
			const FVector3f Cols[3] = { ColCentre, ColBord, ColBord };
			QM.AddPolyPerVertexColors(QM.WallGroup, P, 3, Up, UV, Cols);
		}
	}

	// EMPATTEMENT d'un mesh de vegetation : le rayon XY qu'il occupe AU SOL, c'est-a-dire
	// le rayon maximal de ses sommets situes sous GPitBasalHeightCm de hauteur locale.
	// Sur les arbres Megascans cela englobe le disque de litiere/racines pose au pied,
	// qui est exactement ce qui debordait des fosses v2.
	//
	// Centile 98 et non maximum : un unique sommet egare (brindille couchee, feuille
	// tombee modelisee) ne doit pas dicter la taille d'une fosse.
	//
	// Repli en cascade — jamais d'echec silencieux : donnees de rendu, puis description
	// de maillage (sources non cuites), puis fraction de l'emprise. Le repli est TRACE.
	float ComputeBasalRadiusCm(UStaticMesh* Mesh, FString& OutMethode)
	{
		auto Centile = [](TArray<float>& R) -> float
		{
			R.Sort();
			const int32 Idx = FMath::Clamp((int32)(GPitBasalPct * (R.Num() - 1)),
				0, R.Num() - 1);
			return R[Idx];
		};
		if (!Mesh)
		{
			OutMethode = TEXT("mesh nul");
			return 0.f;
		}
		// --- 1) donnees de rendu (deja en memoire : le mesh est affiche) -------------
		if (const FStaticMeshRenderData* RD = Mesh->GetRenderData())
		{
			if (RD->LODResources.Num() > 0)
			{
				const FPositionVertexBuffer& PVB =
					RD->LODResources[0].VertexBuffers.PositionVertexBuffer;
				const uint32 N = PVB.GetNumVertices();
				if (N >= 8)
				{
					float MinZ = TNumericLimits<float>::Max();
					for (uint32 i = 0; i < N; ++i)
					{
						MinZ = FMath::Min(MinZ, PVB.VertexPosition(i).Z);
					}
					TArray<float> Rayons;
					Rayons.Reserve(N / 4 + 8);
					for (uint32 i = 0; i < N; ++i)
					{
						const FVector3f P = PVB.VertexPosition(i);
						if (P.Z <= MinZ + GPitBasalHeightCm)
						{
							Rayons.Add(FMath::Sqrt(P.X * P.X + P.Y * P.Y));
						}
					}
					if (Rayons.Num() >= 4)
					{
						OutMethode = FString::Printf(TEXT("rendu LOD0 (%d/%u sommets)"),
							Rayons.Num(), N);
						return Centile(Rayons);
					}
				}
			}
		}
		// --- 2) description de maillage (source) -------------------------------------
		if (const FMeshDescription* MD = Mesh->GetMeshDescription(0))
		{
			FStaticMeshConstAttributes Attrs(*MD);
			TVertexAttributesConstRef<FVector3f> Pos = Attrs.GetVertexPositions();
			if (Pos.IsValid() && MD->Vertices().Num() >= 8)
			{
				float MinZ = TNumericLimits<float>::Max();
				for (const FVertexID V : MD->Vertices().GetElementIDs())
				{
					MinZ = FMath::Min(MinZ, Pos[V].Z);
				}
				TArray<float> Rayons;
				for (const FVertexID V : MD->Vertices().GetElementIDs())
				{
					const FVector3f P = Pos[V];
					if (P.Z <= MinZ + GPitBasalHeightCm)
					{
						Rayons.Add(FMath::Sqrt(P.X * P.X + P.Y * P.Y));
					}
				}
				if (Rayons.Num() >= 4)
				{
					OutMethode = FString::Printf(TEXT("description (%d sommets bas)"),
						Rayons.Num());
					return Centile(Rayons);
				}
			}
		}
		// --- 3) repli : fraction de l'emprise (mesure impossible, valeur plausible) ---
		const FBoxSphereBounds B = Mesh->GetBounds();
		OutMethode = TEXT("REPLI emprise (sommets illisibles)");
		return 0.12f * (float)(B.BoxExtent.X + B.BoxExtent.Y) * 0.5f;
	}

	UMaterialInterface* LoadMaterialOrDefault(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return UMaterial::GetDefaultMaterial(MD_Surface);
		}
		UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Path);
		if (!M)
		{
			RaiseError(FString::Printf(TEXT("Material '%s' not found."), *Path));
		}
		return M;
	}

	// bNanite (Lot B, desktop) : NaniteSettings.bEnabled sur les meshes generes —
	// murs, sol, routes, proxys ET vitres (J2e, 25/07 : le verre est OPAQUE ;
	// vitres non-Nanite devant murs Nanite = fenetres qui « flottent » aux
	// transitions LOD). Jamais mobile. Le flag est REPOSE a chaque generation :
	// une regeneration mobile par-dessus des assets desktop les remet a false
	// (golden path).
	UStaticMesh* CreateMeshAsset(const FString& AssetPath, FCityMeshBuilder& QM,
		UMaterialInterface* WallMat, UMaterialInterface* GlassMat, bool bWithCollision = true,
		bool bBoxCollision = false, float BoxTopCm = 0.f, UStaticMesh* ComplexCollisionMesh = nullptr,
		bool bNanite = false)
	{
		// bBoxCollision : une simple boite englobante remplace le trimesh — reserve aux
		// cellules de sol PLATES (dalles+routes), ou le trimesh coutait ~90 Mo de RAM
		// device pour un gain de precision negligeable (< 20 cm en Z).
		FBox SimpleBox(ForceInit);
		if (bWithCollision && bBoxCollision)
		{
			TVertexAttributesRef<FVector3f> Pos = FStaticMeshAttributes(QM.MeshDesc).GetVertexPositions();
			for (const FVertexID V : QM.MeshDesc.Vertices().GetElementIDs())
			{
				SimpleBox += FVector(Pos[V]);
			}
		}
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *(AssetPath + TEXT(".") + ObjectName), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		if (!Mesh)
		{
			UPackage* Package = CreatePackage(*AssetPath);
			Mesh = NewObject<UStaticMesh>(Package, *ObjectName, RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Mesh);
		}
		Mesh->Modify();
		Mesh->GetStaticMaterials().Empty();
		Mesh->GetStaticMaterials().Add(FStaticMaterial(WallMat, FName(TEXT("Wall"))));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(GlassMat, FName(TEXT("Glass"))));
		// J3c point 2 : un slot par classe de revetement, MEME ORDRE que les groupes
		// de polygones du builder (le nom de slot est la cle de correspondance a la
		// construction). Materiau du pack absent -> repli sur GlassMat, qui est le
		// materiau HISTORIQUE des rubans sur les cellules de sol (M_CityRoad_PBR) et
		// vaut WallMat sur les cellules de surfaces : un import sans les assets
		// Megascans rend donc exactement comme avant.
		for (int32 e = 0; e < QM.ExtraSlotNames.Num(); ++e)
		{
			UMaterialInterface* ExtraMat = QM.ExtraSlotMaterials[e] ? QM.ExtraSlotMaterials[e] : GlassMat;
			Mesh->GetStaticMaterials().Add(FStaticMaterial(ExtraMat, QM.ExtraSlotNames[e]));
		}
		Mesh->SetNumSourceModels(1);
		FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(0);
		SourceModel.BuildSettings.bRecomputeNormals = true;
		SourceModel.BuildSettings.bRecomputeTangents = true;
		SourceModel.BuildSettings.bGenerateLightmapUVs = false;
		SourceModel.BuildSettings.bRemoveDegenerates = true;
		Mesh->CreateMeshDescription(0, MoveTemp(QM.MeshDesc));
		Mesh->CommitMeshDescription(0);
		Mesh->SetImportVersion(EImportStaticMeshVersion::LastVersion);
		Mesh->GetNaniteSettings().bEnabled = bNanite;
		// Collision : le maillage de rendu sert de collision (drone vs ville). Pas de
		// primitives simples generees ; les meshes sont statiques, cout memoire accepte.
		// bWithCollision=false (proxy lointain) : UseSimpleAsComplex sans primitive
		// simple = aucun trimesh cuit, zero data de collision dans l'APK.
		// ComplexCollisionMesh (sol desktop) : le trimesh cuit vient d'un mesh basse
		// resolution dedie (grille 16x16), le rendu garde sa pleine densite 64x64.
		// Attendre la fin de sa compilation async AVANT le build de ce mesh : le cook
		// physique lit sa SectionInfoMap depuis un worker (ensure StaticMesh.cpp:4790).
		Mesh->ComplexCollisionMesh = ComplexCollisionMesh;
		if (ComplexCollisionMesh)
		{
			FStaticMeshCompilingManager::Get().FinishCompilation(
				TArrayView<UStaticMesh* const>(&ComplexCollisionMesh, 1));
		}
		Mesh->CreateBodySetup();
		if (UBodySetup* Body = Mesh->GetBodySetup())
		{
			Body->CollisionTraceFlag = (bWithCollision && !bBoxCollision)
				? CTF_UseComplexAsSimple : CTF_UseSimpleAsComplex;
			// Regeneration en place : purger les boites de la generation precedente
			// (elles s'ACCUMULAIENT a chaque re-import — bug latent expose par le
			// test de non-regression du profil mobile).
			Body->AggGeom.BoxElems.Reset();
			if (bWithCollision && bBoxCollision && SimpleBox.IsValid)
			{
				// BoxTopCm > 0 : plafond de boite force (ex. dalles de sol dont la boite
				// doit englober les rubans de route poses au-dessus, ~55-80 cm).
				const float ZSize = BoxTopCm > 0.f
					? BoxTopCm : FMath::Max<float>(SimpleBox.GetSize().Z, 8.f);
				FKBoxElem BoxElem(SimpleBox.GetSize().X, SimpleBox.GetSize().Y, ZSize);
				BoxElem.Center = BoxTopCm > 0.f
					? FVector(SimpleBox.GetCenter().X, SimpleBox.GetCenter().Y, ZSize * 0.5)
					: SimpleBox.GetCenter();
				Body->AggGeom.BoxElems.Add(BoxElem);
			}
			Body->InvalidatePhysicsData();
		}
		Mesh->Build(false);
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
		return Mesh;
	}
}

FCityGenProfile FCityGenProfile::Desktop()
{
	FCityGenProfile Profile;
	Profile.bDesktop = true;
	Profile.GroundGridN = 64;
	Profile.GroundCollisionGridN = 16;
	Profile.RoadResampleStepCm = 1500.f;
	Profile.bDrapeToTerrain = true;
	Profile.bWindowReveals = true;
	Profile.bSplitWallGlass = true;
	Profile.bNanite = true;
	Profile.bPBRMaterials = true;
	Profile.bSurfaceMaterials = true;
	return Profile;
}

FCityGenProfile FCityGenProfile::Resolved() const
{
	if (!bDesktop)
	{
		return *this;
	}
	// bDesktop seul (appel MCP minimal) : tout champ laisse a sa valeur mobile
	// bascule vers le prereglage desktop ; un champ renseigne est conserve.
	const FCityGenProfile Mobile;
	FCityGenProfile Out = *this;
	if (Out.GroundGridN == Mobile.GroundGridN) { Out.GroundGridN = 64; }
	if (Out.GroundCollisionGridN == Mobile.GroundCollisionGridN) { Out.GroundCollisionGridN = 16; }
	if (Out.RoadResampleStepCm == Mobile.RoadResampleStepCm) { Out.RoadResampleStepCm = 1500.f; }
	Out.bDrapeToTerrain = true;
	Out.bWindowReveals = true;
	Out.bSplitWallGlass = true;
	Out.bNanite = true;
	Out.bPBRMaterials = true;
	Out.bSurfaceMaterials = true;
	return Out;
}

// ============================================================================
// ⭐ LOT PIE (02/08) — LA COLLISION DE LA VEGETATION EST LE PRIX DU « PLAY ».
//
// LE FAIT MESURE. Un `UInstancedStaticMeshComponent` cree UN CORPS PHYSIQUE PAR
// INSTANCE ; et comme nos HISM sont `Movable`, le moteur les cree UN PAR UN
// (`FBodyInstance::InitBody`, `InstancedMeshComponentBodies.cpp` l. 111) au lieu du
// chemin par lot `InitStaticBodies` reserve aux composants statiques. Sur le proto
// 3x3 : **1 253 686 corps Chaos** a chaque « Play » et a chaque « Stop ».
// Cycle PIE mesure AVANT ce correctif : demarrage 10,3 s (dont 8,83 s de
// `InitializeActorsForPlay`), arret **604,5 s** dont **602 392 ms (99,97 %)** dans
// `DestroyGarbage` pour 862 objets (analytics `gc.DumpAnalyticsToLog`). C'est le gel
// que l'utilisateur reglait au gestionnaire des taches.
//
// LA REGLE N'EST PAS CHOISIE, ELLE EST LUE DANS LA DONNEE. Un mesh sans AUCUNE
// primitive de collision SIMPLE (`AggGeom` vide) n'a pas de collision voulue : son
// `CTF_USE_DEFAULT` retombe sur la collision COMPLEXE, donc sur le repli decime du
// Nanite — ni voulue, ni utilisable, et payee 1,2 million de fois. Catalogue mesure
// du proto : 12 herbes (1 180 237 touffes), fosses carrees et rondes (25 054) et
// sureaux = **0 primitive** ; erables et hetres = **2 a 3 convexes**, ils GARDENT
// leur collision (aucun arbitrage de gameplay n'est demande ici).
//
// Et rien de la generation n'en depend : la passe de vegetation IGNORE deja les
// acteurs `CityVeg` dans ses traces (voir `TraceParams.AddIgnoredActor` plus bas) —
// la collision des touffes n'a jamais servi qu'a se faire detruire.
//
// `FCityGenProfile::bVegCollisionHistorique = true` restitue l'ancien comportement
// SANS rebuild.
// ============================================================================
namespace
{
	bool VegSansCollisionVoulue(const UStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return true;
		}
		const UBodySetup* BS = Mesh->GetBodySetup();
		if (!BS)
		{
			return true;
		}
		return BS->AggGeom.GetElementCount() == 0;
	}

	// A APPELER AVANT `RegisterComponent()` : posee a la creation, la propriete ne
	// peut pas etre perdue par une regeneration ni par une sauvegarde faite avant
	// elle (doctrine du 01/08, lot V5). Posee apres, aucun corps n'aurait ete evite.
	void PoserCollisionVegetation(UHierarchicalInstancedStaticMeshComponent* Hism,
		const FCityGenProfile& Gen, int32& OutSansCollision)
	{
		if (!Hism || Gen.bVegCollisionHistorique)
		{
			return;
		}
		if (!VegSansCollisionVoulue(Hism->GetStaticMesh()))
		{
			return;   // vrai volume de collision (arbres) : on ne touche a rien.
		}
		Hism->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Hism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Hism->SetGenerateOverlapEvents(false);
		++OutSansCollision;
	}
}

FCityImportSummary UCityImportTools::ImportCityDistrict(const FString& JsonFilePath, const FString& AssetFolder,
	const FString& WallMaterialPath, const FString& GlassMaterialPath, float CellSizeM, FVector Location)
{
	FCityImportSummary Summary;

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read district file '%s'."), *JsonFilePath));
		return Summary;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("District file is not valid JSON."));
		return Summary;
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder must be a package path such as /Game/City/Capitole."));
		return Summary;
	}
	if (CellSizeM < 20.f)
	{
		RaiseError(TEXT("CellSizeM must be at least 20."));
		return Summary;
	}

	UMaterialInterface* WallMat = LoadMaterialOrDefault(WallMaterialPath);
	UMaterialInterface* GlassMat = LoadMaterialOrDefault(GlassMaterialPath);
	if (!WallMat || !GlassMat)
	{
		return Summary;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		RaiseError(TEXT("No editor world is loaded."));
		return Summary;
	}

	// Idempotence : un re-import remplace la ville existante (memes labels d'acteurs).
	TArray<AActor*> ToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		if (L.StartsWith(TEXT("SM_City_")) || L == TEXT("CityTrees"))
		{
			ToDestroy.Add(*It);
		}
	}
	for (AActor* A : ToDestroy)
	{
		World->DestroyActor(A);
	}

	const float Cell = CellSizeM * 100.f;
	TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>> Cells;
	TMap<FIntPoint, FBox2D> CellBounds;
	auto GetCell = [&](const FVector2D& P) -> FCityMeshBuilder&
	{
		const FIntPoint Key(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell));
		TUniquePtr<FCityMeshBuilder>& B = Cells.FindOrAdd(Key);
		if (!B)
		{
			B = MakeUnique<FCityMeshBuilder>();
			CellBounds.Add(Key, FBox2D(FVector2D(Key.X * Cell, Key.Y * Cell),
				FVector2D((Key.X + 1) * Cell, (Key.Y + 1) * Cell)));
		}
		return *B;
	};
	auto ReadPts = [](const TArray<TSharedPtr<FJsonValue>>& In, TArray<FVector2D>& Out)
	{
		for (const TSharedPtr<FJsonValue>& V : In)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() >= 2)
			{
				Out.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
			}
		}
	};

	// --- Batiments ---
	const TArray<TSharedPtr<FJsonValue>>* BuildingsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("buildings"), BuildingsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *BuildingsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 3)
			{
				continue;
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			FVector2D Centroid(0, 0);
			for (const FVector2D& P : Pts) { Centroid += P; }
			Centroid /= Pts.Num();
			const float Hcm = O->GetNumberField(TEXT("h")) * 100.f;
			BuildPolygonBuilding(GetCell(Centroid), Pts, Hcm,
				UsageTint(O->GetStringField(TEXT("u")), Index));
			++Summary.Buildings;
			++Index;
		}
	}

	// --- Routes ---
	const TArray<TSharedPtr<FJsonValue>>* RoadsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("roads"), RoadsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *RoadsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 2)
			{
				continue;
			}
			BuildRoad(GetCell(Pts[0]), Pts, O->GetNumberField(TEXT("w")) * 100.f,
				O->GetStringField(TEXT("t")), Index);
			++Summary.Roads;
			++Index;
		}
	}

	// --- Sols par cellule ---
	for (auto& Pair : CellBounds)
	{
		FCityMeshBuilder& B = *Cells[Pair.Key];
		const FBox2D& Bx = Pair.Value;
		B.AddQuad(B.WallGroup,
			FVector3f(Bx.Min.X, Bx.Min.Y, 0), FVector3f(Bx.Max.X, Bx.Min.Y, 0),
			FVector3f(Bx.Max.X, Bx.Max.Y, 0), FVector3f(Bx.Min.X, Bx.Max.Y, 0),
			FVector3f(0, 0, 1), FVector3f(0.33f, 0.31f, 0.28f));
	}

	// --- Assets + acteurs par cellule ---
	for (auto& Pair : Cells)
	{
		const FString Name = FString::Printf(TEXT("SM_City_%d_%d"), Pair.Key.X, Pair.Key.Y);
		UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / Name, *Pair.Value, WallMat, GlassMat);
		++Summary.Meshes;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->SetActorLabel(Name);
	}

	// --- Arbres : un mesh + un HISM ---
	const TArray<TSharedPtr<FJsonValue>>* TreesJson = nullptr;
	if (Root->TryGetArrayField(TEXT("trees"), TreesJson) && TreesJson->Num() > 0)
	{
		FCityMeshBuilder TreeBuilder;
		BuildTree(TreeBuilder);
		UStaticMesh* TreeMesh = CreateMeshAsset(AssetFolder / TEXT("SM_CityTree"), TreeBuilder, WallMat, GlassMat);
		++Summary.Meshes;
		// Piege F.39 : usage InstancedStaticMeshes persiste sur le materiau de base.
		WallMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		GlassMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);

		AActor* TreeActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		USceneComponent* Root2 = NewObject<USceneComponent>(TreeActor, TEXT("Root"), RF_Transactional);
		TreeActor->SetRootComponent(Root2);
		TreeActor->AddInstanceComponent(Root2);
		Root2->RegisterComponent();
		Root2->SetWorldLocation(Location);
		UHierarchicalInstancedStaticMeshComponent* Hism =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(TreeActor, TEXT("Trees"), RF_Transactional);
		Hism->SetStaticMesh(TreeMesh);
		Hism->SetupAttachment(Root2);
		TreeActor->AddInstanceComponent(Hism);
		Hism->RegisterComponent();
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *TreesJson)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() < 2)
			{
				continue;
			}
			const float Yaw = FMath::Frac(FMath::Sin(Index * 78.233f) * 12543.21f) * 360.f;
			const float Scale = 0.8f + 0.5f * FMath::Frac(FMath::Sin(Index * 39.11f) * 6543.87f);
			FTransform Xf(FRotator(0, Yaw, 0),
				FVector(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0, 0),
				FVector(Scale));
			Hism->AddInstance(Xf);
			++Summary.Trees;
			++Index;
		}
		TreeActor->SetActorLabel(TEXT("CityTrees"));
	}

	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCityImport, Display, TEXT("District importe : %d batiments, %d routes, %d arbres, %d meshes."),
		Summary.Buildings, Summary.Roads, Summary.Trees, Summary.Meshes);
	return Summary;
}

namespace
{
	struct FMarkerKind
	{
		float HeightCm;
		FVector3f Color;
		bool bLabel;
		float LabelSizeCm = 320.f;
	};

	const TMap<FString, FMarkerKind>& MarkerKinds()
	{
		static const TMap<FString, FMarkerKind> Kinds = {
			{ TEXT("metro"),    { 900.f,  FVector3f(0.90f, 0.10f, 0.10f), true } },
			{ TEXT("metro_e"),  { 400.f,  FVector3f(0.90f, 0.10f, 0.10f), false } },
			{ TEXT("church"),   { 1500.f, FVector3f(0.55f, 0.30f, 0.85f), true } },
			{ TEXT("townhall"), { 1900.f, FVector3f(0.95f, 0.72f, 0.15f), true } },
			// Indications de zones (place=suburb) et quartiers (place=quarter|neighbourhood) :
			// totems hauts et noms geants, lisibles en vol a l'echelle de la ville.
			{ TEXT("district"), { 3500.f, FVector3f(0.10f, 0.80f, 0.75f), true, 1100.f } },
			{ TEXT("quarter"),  { 2200.f, FVector3f(0.25f, 0.50f, 0.95f), true, 650.f } },
		};
		return Kinds;
	}

	// Totem : socle + mat sombre + tete cubique de la couleur du type.
	void BuildTotem(FCityMeshBuilder& QM, const FMarkerKind& Kind)
	{
		auto Box = [&](float HalfX, float HalfY, float Z0, float Z1, const FVector3f& C)
		{
			const FVector3f Mn(-HalfX, -HalfY, Z0), Mx(HalfX, HalfY, Z1);
			const FVector3f v000(Mn.X, Mn.Y, Mn.Z), v100(Mx.X, Mn.Y, Mn.Z), v010(Mn.X, Mx.Y, Mn.Z),
				v110(Mx.X, Mx.Y, Mn.Z), v001(Mn.X, Mn.Y, Mx.Z), v101(Mx.X, Mn.Y, Mx.Z),
				v011(Mn.X, Mx.Y, Mx.Z), v111(Mx.X, Mx.Y, Mx.Z);
			QM.AddQuad(QM.WallGroup, v000, v100, v101, v001, FVector3f(0, -1, 0), Shade(C, FVector3f(0, -1, 0), Z1));
			QM.AddQuad(QM.WallGroup, v110, v010, v011, v111, FVector3f(0, 1, 0), Shade(C, FVector3f(0, 1, 0), Z1));
			QM.AddQuad(QM.WallGroup, v010, v000, v001, v011, FVector3f(-1, 0, 0), Shade(C, FVector3f(-1, 0, 0), Z1));
			QM.AddQuad(QM.WallGroup, v100, v110, v111, v101, FVector3f(1, 0, 0), Shade(C, FVector3f(1, 0, 0), Z1));
			QM.AddQuad(QM.WallGroup, v001, v101, v111, v011, FVector3f(0, 0, 1), Shade(C, FVector3f(0, 0, 1), Z1));
			QM.AddQuad(QM.WallGroup, v010, v110, v100, v000, FVector3f(0, 0, -1), C * 0.4f);
		};
		const float HeadHalf = FMath::Clamp(Kind.HeightCm * 0.12f, 80.f, 180.f);
		Box(70.f, 70.f, 0.f, 25.f, FVector3f(0.22f, 0.22f, 0.24f));
		Box(20.f, 20.f, 25.f, Kind.HeightCm - HeadHalf * 2.f, Kind.Color * 0.55f);
		Box(HeadHalf, HeadHalf, Kind.HeightCm - HeadHalf * 2.f, Kind.HeightCm, Kind.Color);
	}
}

int32 UCityImportTools::ImportCityMarkers(const FString& JsonFilePath, const FString& AssetFolder,
	const FString& WallMaterialPath, FVector Location, const FCityGenProfile& Profile)
{
	// Profil effectif + MNT charge UNE fois pour tout l'import (jalon J2).
	const FCityGenProfile Gen = Profile.Resolved();
	FDrapeContext Drape;
	if (!MakeDrapeContext(Gen, Drape))
	{
		return 0;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read markers file '%s'."), *JsonFilePath));
		return 0;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("Markers file is not valid JSON."));
		return 0;
	}
	const TArray<TSharedPtr<FJsonValue>>* MarkersJson = nullptr;
	if (!Root->TryGetArrayField(TEXT("markers"), MarkersJson) || MarkersJson->Num() == 0)
	{
		RaiseError(TEXT("Markers file has no 'markers' array."));
		return 0;
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder must be a package path such as /Game/City/Capitole."));
		return 0;
	}
	UMaterialInterface* WallMat = LoadMaterialOrDefault(WallMaterialPath);
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!WallMat || !World)
	{
		if (!World)
		{
			RaiseError(TEXT("No editor world is loaded."));
		}
		return 0;
	}
	WallMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);

	// Idempotence : un re-import remplace les marqueurs existants.
	TArray<AActor*> ToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		if (L == TEXT("CityMarkers") || L.StartsWith(TEXT("Label_")))
		{
			ToDestroy.Add(*It);
		}
	}
	for (AActor* A : ToDestroy)
	{
		World->DestroyActor(A);
	}

	// Un mesh + un HISM par type present dans le fichier.
	TMap<FString, UHierarchicalInstancedStaticMeshComponent*> HismByKind;
	AActor* MarkerActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
	USceneComponent* Root2 = NewObject<USceneComponent>(MarkerActor, TEXT("Root"), RF_Transactional);
	MarkerActor->SetRootComponent(Root2);
	MarkerActor->AddInstanceComponent(Root2);
	Root2->RegisterComponent();
	Root2->SetWorldLocation(Location);
	MarkerActor->SetActorLabel(TEXT("CityMarkers"));

	int32 Placed = 0;
	for (const TSharedPtr<FJsonValue>& V : *MarkersJson)
	{
		const TSharedPtr<FJsonObject>& O = V->AsObject();
		const FString KindName = O->GetStringField(TEXT("k"));
		const FMarkerKind* Kind = MarkerKinds().Find(KindName);
		if (!Kind)
		{
			continue;
		}
		// Desktop : totem et labels poses a l'altitude du terrain (rebase Capitole).
		const double Mx = O->GetNumberField(TEXT("x")) * 100.0;
		const double My = O->GetNumberField(TEXT("y")) * 100.0;
		const FVector Pos = Location + FVector(Mx, My, Drape.GroundZ(Mx, My));

		UHierarchicalInstancedStaticMeshComponent*& Hism = HismByKind.FindOrAdd(KindName);
		if (!Hism)
		{
			FCityMeshBuilder Builder;
			BuildTotem(Builder, *Kind);
			UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / (TEXT("SM_Marker_") + KindName), Builder,
				WallMat, WallMat);
			Hism = NewObject<UHierarchicalInstancedStaticMeshComponent>(MarkerActor,
				*(TEXT("Markers_") + KindName), RF_Transactional);
			Hism->SetStaticMesh(Mesh);
			Hism->SetupAttachment(Root2);
			MarkerActor->AddInstanceComponent(Hism);
			Hism->RegisterComponent();
		}
		Hism->AddInstance(FTransform(Pos - Location));

		// Nom flottant : QUATRE orientations empilees (0/90/180/270 — le materiau texte
		// est double-face avec revers en miroir : superposes les glyphes se melangent,
		// empiles il y a toujours un exemplaire lisible NON-miroir de n'importe quelle
		// direction au sol) + UN exemplaire A PLAT au sommet pour la vue aerienne.
		const FString Label = O->GetStringField(TEXT("n"));
		if (Kind->bLabel && !Label.IsEmpty())
		{
			const FColor LabelColor(
				FMath::RoundToInt(Kind->Color.X * 255.f),
				FMath::RoundToInt(Kind->Color.Y * 255.f),
				FMath::RoundToInt(Kind->Color.Z * 255.f));
			auto MakeText = [&](const FVector& P, const FRotator& Rot, int32 Idx)
			{
				ATextRenderActor* Text = World->SpawnActor<ATextRenderActor>(P, Rot);
				UTextRenderComponent* Comp = Text->GetTextRender();
				Comp->SetText(FText::FromString(Label));
				Comp->SetWorldSize(Kind->LabelSizeCm);
				Comp->SetHorizontalAlignment(EHTA_Center);
				Comp->SetTextRenderColor(LabelColor);
				Text->SetActorLabel(FString::Printf(TEXT("Label_%s_%d"), *Label, Idx));
			};
			for (int32 i = 0; i < 4; ++i)
			{
				MakeText(Pos + FVector(0, 0, Kind->HeightCm + Kind->LabelSizeCm * (0.5625 + i * 1.125)),
					FRotator(0.f, i * 90.f, 0.f), i);
			}
			// Paire A PLAT dos-a-dos (30 cm d'ecart) : le materiau texte double-face a un
			// revers en miroir — une copie seule se lisait a l'envers vue du ciel.
			MakeText(Pos + FVector(0, 0, Kind->HeightCm + Kind->LabelSizeCm * 5.2),
				FRotator(-90.f, 0.f, 0.f), 4);
			MakeText(Pos + FVector(0, 0, Kind->HeightCm + Kind->LabelSizeCm * 5.2 + 30.0),
				FRotator(90.f, 180.f, 0.f), 5);
		}
		++Placed;
	}
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCityImport, Display, TEXT("%d marqueurs places."), Placed);
	return Placed;
}

namespace
{
	// Polygone plat de reperage (eau, parc, bois) : triangulation de l'anneau, teinte
	// cuite. TerrainZ (desktop) : Z terrain PAR SOMMET ajoute a l'offset d'empilement
	// Zcm (verts drapes) ; nul = film plat historique (mobile, et l'eau desktop qui
	// reste plane — son niveau est alors porte par Zcm).
	// Surf (J3c point 2) : classe de revetement resolue — le polygone part dans son
	// groupe dedie avec une UV0 MONDE EN METRES (pelouses, bois). Nul = historique.
	// PreTris (FRONTIERE-Z) : triangulation LIVREE PAR LA CUISSON. Non vide = on
	// pose ces triangles tels quels ; vide = `TriangulateRing`, comportement
	// historique bit pour bit.
	void BuildFlatPolygon(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float Zcm,
		const FVector3f& Tint, const TArray<float>* TerrainZ = nullptr,
		const FResolvedSurface* Surf = nullptr,
		const TArray<FVector3f>* VertexTints = nullptr,
		const TArray<int32>* PreTris = nullptr)
	{
		TArray<int32> Tris;
		if (PreTris && PreTris->Num() >= 3)
		{
			Tris = *PreTris;
		}
		else
		{
			TriangulateRing(PtsCm, Tris);
		}
		const FVector3f Shaded = Shade(Tint, FVector3f(0, 0, 1), Zcm);
		auto VertexZ = [&](int32 Index)
		{
			return Zcm + (TerrainZ ? (*TerrainZ)[Index] : 0.f);
		};
		const FPolygonGroupID Group = Surf
			? QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material) : QM.WallGroup;
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			const FVector3f P[3] = {
				FVector3f(PtsCm[Tris[t]].X, PtsCm[Tris[t]].Y, VertexZ(Tris[t])),
				FVector3f(PtsCm[Tris[t + 1]].X, PtsCm[Tris[t + 1]].Y, VertexZ(Tris[t + 1])),
				FVector3f(PtsCm[Tris[t + 2]].X, PtsCm[Tris[t + 2]].Y, VertexZ(Tris[t + 2])) };
			if (!Surf)
			{
				QM.AddTri(QM.WallGroup, P[0], P[1], P[2], FVector3f(0, 0, 1), Shaded);
				continue;
			}
			if ((P[0] - P[1]).IsNearlyZero(0.01f) || (P[1] - P[2]).IsNearlyZero(0.01f) ||
				(P[2] - P[0]).IsNearlyZero(0.01f))
			{
				continue;
			}
			const FVector2f UV[3] = {
				FVector2f(P[0].X * 0.01f, P[0].Y * 0.01f),
				FVector2f(P[1].X * 0.01f, P[1].Y * 0.01f),
				FVector2f(P[2].X * 0.01f, P[2].Y * 0.01f) };
			if (VertexTints && VertexTints->Num() == PtsCm.Num())
			{
				// BERGES : la couleur de sommet PORTE UNE DONNEE (l'ecoulement),
				// elle ne teinte rien — donc aucun Shade() ici, ce serait la falsifier.
				const FVector3f Cols[3] = {
					(*VertexTints)[Tris[t]], (*VertexTints)[Tris[t + 1]],
					(*VertexTints)[Tris[t + 2]] };
				QM.AddPolyPerVertexColors(Group, P, 3, FVector3f(0, 0, 1), UV, Cols);
				continue;
			}
			QM.AddPoly(Group, P, 3, FVector3f(0, 0, 1), UV, Shaded);
		}
	}

	// Ruban de voie ferree : ballast sombre, sans trottoir ni marquage.
	// TerrainZ : cf. BuildRoad (drapage desktop, nul = plat mobile).
	void BuildRail(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float WidthCm, int32 RailIndex,
		const TArray<float>* TerrainZ = nullptr)
	{
		const int32 N = PtsCm.Num();
		if (N < 2)
		{
			return;
		}
		// Empilement decimetrique (cf. BuildRoad) + decalage par voie aux croisements.
		const float Z = 45.f + (RailIndex % 4) * 2.f;
		const FVector3f Ballast(0.24f, 0.22f, 0.21f);
		const FVector3f Up(0, 0, 1);
		TArray<FVector2D> Nrm;
		Nrm.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			FVector2D D(0, 0);
			if (i > 0) { D += (PtsCm[i] - PtsCm[i - 1]).GetSafeNormal(); }
			if (i < N - 1) { D += (PtsCm[i + 1] - PtsCm[i]).GetSafeNormal(); }
			D = D.GetSafeNormal();
			Nrm[i] = FVector2D(-D.Y, D.X);
		}
		const float Half = WidthCm * 0.5f;
		for (int32 i = 0; i + 1 < N; ++i)
		{
			const FVector2D A = PtsCm[i], B = PtsCm[i + 1];
			const FVector2D NA = Nrm[i] * Half, NB = Nrm[i + 1] * Half;
			const float ZA = Z + (TerrainZ ? (*TerrainZ)[i] : 0.f);
			const float ZB = Z + (TerrainZ ? (*TerrainZ)[i + 1] : 0.f);
			QM.AddQuad(QM.WallGroup,
				FVector3f(A.X - NA.X, A.Y - NA.Y, ZA), FVector3f(B.X - NB.X, B.Y - NB.Y, ZB),
				FVector3f(B.X + NB.X, B.Y + NB.Y, ZB), FVector3f(A.X + NA.X, A.Y + NA.Y, ZA),
				Up, Ballast);
		}
	}

	// Polygone de peinture du sol (echantillonnage des dalles).
	struct FPaintPoly
	{
		FBox2D Bounds;
		TArray<FVector2D> Pts;
		FVector3f Tint;
		int32 Priority = 0;
	};

	bool PointInRing(const TArray<FVector2D>& P, const FVector2D& Q)
	{
		bool bIn = false;
		for (int32 i = 0, j = P.Num() - 1; i < P.Num(); j = i++)
		{
			if (((P[i].Y > Q.Y) != (P[j].Y > Q.Y)) &&
				(Q.X < (P[j].X - P[i].X) * (Q.Y - P[i].Y) / (P[j].Y - P[i].Y) + P[i].X))
			{
				bIn = !bIn;
			}
		}
		return bIn;
	}

	// =========================================================================
	// ⭐ PARTITION DU SOL — ① LE CONTRAT DE Z, ③ LE RUBAN DE BANDE,
	//                       ④ LA LOI D'INTERFACE.
	//
	// « La carte dit QUI possede ; le contrat de Z dit A QUELLE COTE ; la
	// couture dit COMMENT deux proprietaires se rencontrent. »
	// =========================================================================

	/**
	 * ① LE CONTRAT DE Z. Le profil d'une ligne : ses points re-echantillonnes a
	 * pas fixe et, sous chacun, le Z de la surface RENDUE lu par `At()` — LE
	 * lecteur canonique du projet (celui des escaliers, des gradins, des murs et
	 * de la vegetation). Lire ne change rien : le contrat est ADDITIF par
	 * construction, et c'est ce qui rend l'acceptation ① vraie a priori.
	 */
	struct FProfilZ
	{
		TArray<FVector2D> Pts;   // cm monde
		TArray<float> Z;         // cm monde, surface RENDUE
	};

	void EchantillonnerProfil(const TArray<FVector2D>& Ligne, float PasCm,
		const FRenderedGroundZ& RGZ, FProfilZ& Out)
	{
		Out.Pts = (PasCm > 0.f) ? ResamplePolyline(Ligne, PasCm) : Ligne;
		Out.Z.SetNumUninitialized(Out.Pts.Num());
		for (int32 i = 0; i < Out.Pts.Num(); ++i)
		{
			Out.Z[i] = RGZ.At(Out.Pts[i].X, Out.Pts[i].Y);
		}
	}

	/** Normales par sommet d'une polyligne — meme calcul que BuildRoad. */
	void NormalesSommet(const TArray<FVector2D>& Pts, TArray<FVector2D>& Out)
	{
		const int32 N = Pts.Num();
		Out.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			FVector2D D(0, 0);
			if (i > 0) { D += (Pts[i] - Pts[i - 1]).GetSafeNormal(); }
			if (i < N - 1) { D += (Pts[i + 1] - Pts[i]).GetSafeNormal(); }
			D = D.GetSafeNormal();
			Out[i] = FVector2D(-D.Y, D.X);
		}
	}

	float LongueurCm(const TArray<FVector2D>& Pts)
	{
		float L = 0.f;
		for (int32 i = 0; i + 1 < Pts.Num(); ++i)
		{
			L += (float)(Pts[i + 1] - Pts[i]).Size();
		}
		return L;
	}

	/**
	 * ③ LA CLASSE DU PROPRIETAIRE — et rien qu'elle. AUCUNE classe ni matiere
	 * nouvelle n'est creee par ce chantier : une bande annexee prend le
	 * revetement de celui qui l'a annexee, tel qu'il existe deja.
	 *   voirie / batiment -> la DALLE. C'est la doctrine v5 mot pour mot : « le
	 *     trottoir n'est PAS un revetement de plus, c'est le sol de la ville ».
	 *     Une bande le long d'une rue ou au pied d'un immeuble EST cette rive.
	 *   ouvrage -> la PIERRE DE QUAI (GSurfCurb), deja le materiau des faces et
	 *     des couronnements de l'ouvrage de berge.
	 *   zone -> l'HERBE COUPEE, la seule herbe de tous les espaces verts depuis
	 *     l'assainissement v5.
	 */
	const FSurfaceClass* ClasseDeProprio(FCityPartition::EProprio P)
	{
		switch (P)
		{
		case FCityPartition::EProprio::Ouvrage:  return &GSurfCurb;
		case FCityPartition::EProprio::Voirie:   return &GSurfSlab;
		case FCityPartition::EProprio::Batiment: return &GSurfSlab;
		case FCityPartition::EProprio::Zone:     return &GSurfGrassCut;
		default:                                 return nullptr;
		}
	}

	/**
	 * ③ LA GEOMETRIE DU RUBAN DE BANDE.
	 *
	 * La bande est un polygone mince (≤ 3 m par la regle de la carte) accroche a
	 * son proprietaire par sa LIGNE PORTEUSE. Le ruban se pose SUR cette ligne
	 * et se pousse vers l'INTERIEUR de la bande :
	 *   * largeur = aire / longueur de ligne — la largeur MOYENNE mesuree, un
	 *     nombre, pas une interpolation ; le collier l'elargit un peu pour que
	 *     le ruban couvre la bande ENTIERE (13.2 : le recouvrement est benin,
	 *     le vide est fatal) ;
	 *   * le COTE se decide par POINT-DANS-POLYGONE (loi 13.3), jamais par le
	 *     signe d'une normale : on sonde les deux cotes et on garde celui qui
	 *     tombe dans l'anneau de la bande, a la majorite des sommets ;
	 *   * le Z vient du profil lu SUR LA LIGNE PORTEUSE, c'est-a-dire AU
	 *     CONTACT DU PROPRIETAIRE : c'est en cela que la bande cesse d'etre
	 *     drapee comme de l'organique. Il est CONSTANT en travers de la bande
	 *     (une constante n'est pas une nappe, 13.1) et suit la surface le long.
	 *
	 * Rend false quand il n'y a rien a poser (ligne trop courte, aire nulle).
	 */
	// Raisons d'ecart d'un ruban de bande. Nommees : « compte, jamais tu en silence ».
	enum class ERubanRaison : uint8 { Pose, TropFine, NonRuban };

	/**
	 * ⛔ LE RABOTAGE ANTI-RETOURNEMENT — mecanisme du projet, repris tel quel du
	 * constructeur de murs (`Deretourne`, lot BERGES) et non reinvente.
	 *
	 * Un decalage CONSTANT applique a des normales de SOMMET se croise des que la
	 * ligne tourne plus serre que le decalage : le quad s'inverse et l'on obtient
	 * un NOEUD PAPILLON — le coin sombre plie en deux vu sur la capture. On rabote
	 * le decalage aux SEULS sommets fautifs (moitie a chaque passe) jusqu'a ce que
	 * la ligne decalee AVANCE partout, des DEUX cotes. La convergence est acquise :
	 * a decalage nul, la ligne decalee EST l'axe, qui avance toujours.
	 *
	 * Rend le nombre de sommets rabotes.
	 */
	int32 EchelleAntiRetournement(const TArray<FVector2D>& Pts, const TArray<FVector2D>& Nrm,
		float OffCm, TArray<float>& Echelle)
	{
		const int32 N = Pts.Num();
		Echelle.Init(1.f, N);
		if (OffCm <= 0.f || N < 2)
		{
			return 0;
		}
		for (int32 Iter = 0; Iter < 12; ++Iter)
		{
			bool bPropre = true;
			for (int32 i = 0; i + 1 < N; ++i)
			{
				FVector2D A = Pts[i + 1] - Pts[i];
				if (A.IsNearlyZero())
				{
					continue;
				}
				A.Normalize();
				bool bFaute = false;
				for (double Signe : { 1.0, -1.0 })
				{
					const FVector2D O0 = Pts[i] + Nrm[i] * (Signe * (double)(OffCm * Echelle[i]));
					const FVector2D O1 = Pts[i + 1]
						+ Nrm[i + 1] * (Signe * (double)(OffCm * Echelle[i + 1]));
					if (FVector2D::DotProduct(O1 - O0, A) < 0.0)
					{
						bFaute = true;
					}
				}
				if (!bFaute)
				{
					continue;
				}
				bPropre = false;
				Echelle[i] *= 0.5f;
				Echelle[i + 1] *= 0.5f;
			}
			if (bPropre)
			{
				break;
			}
		}
		int32 Rabotes = 0;
		for (int32 i = 0; i < N; ++i)
		{
			if (Echelle[i] < 1.f) { ++Rabotes; }
		}
		return Rabotes;
	}

	bool RubanDeBande(const FCityPartition::FBande& B, const TArray<FVector2D>& Ligne,
		float PasCm, float CollierCm, float LargeurMaxCm, float SondeProprioCm,
		const FRenderedGroundZ& RGZ,
		TArray<FVector2D>& OutAxe, TArray<float>& OutZ, float& OutLargeurCm,
		TArray<FVector2D>& OutBordLibre, ERubanRaison& OutRaison,
		TArray<float>& OutEchelle, int32& OutRabotes)
	{
		OutRaison = ERubanRaison::TropFine;
		FProfilZ Profil;
		EchantillonnerProfil(Ligne, PasCm, RGZ, Profil);
		const int32 N = Profil.Pts.Num();
		if (N < 2 || B.Ext.Num() < 3 || B.AireM2 <= 0.0)
		{
			return false;
		}
		const float LigneCm = LongueurCm(Profil.Pts);
		if (LigneCm < 1.f)
		{
			return false;
		}
		// Aire en cm2 (m2 x 10 000) rapportee a la longueur de la ligne.
		const float LargeurCm = (float)(B.AireM2 * 10000.0) / LigneCm;
		if (LargeurCm < 1.f)
		{
			return false;
		}
		// ⛔ LE MODELE RUBAN NE DECRIT PAS TOUTE BANDE. La carte publie sa propre
		// borne (`BANDE_MAX_M`) : une bande est au plus large de cela. Quand la
		// largeur DEDUITE la depasse, ce n'est pas la bande qui est large — c'est
		// que la ligne porteuse ne longe pas la bande : elle l'EFFLEURE (mesure sur
		// le district : jusqu'a 598 m2 pour 4 cm de contact, soit un ruban de
		// 14 km de large). Poser serait poser FAUX ; on ecarte et on COMPTE.
		if (LargeurMaxCm > 0.f && LargeurCm > LargeurMaxCm)
		{
			OutRaison = ERubanRaison::NonRuban;
			return false;
		}
		TArray<FVector2D> Nrm;
		NormalesSommet(Profil.Pts, Nrm);
		// LE COTE, par point-dans-polygone. La sonde vaut la moitie de la largeur
		// (bornee) : assez loin pour sortir du bruit du contour, assez pres pour
		// rester dans une bande mince.
		const double Sonde = (double)FMath::Clamp(LargeurCm * 0.5f, 3.f, 150.f);
		int32 Plus = 0, Moins = 0;
		for (int32 i = 0; i < N; ++i)
		{
			if (PointInRing(B.Ext, Profil.Pts[i] + Nrm[i] * Sonde)) { ++Plus; }
			if (PointInRing(B.Ext, Profil.Pts[i] - Nrm[i] * Sonde)) { ++Moins; }
		}
		if (Plus == 0 && Moins == 0)
		{
			return false;   // la ligne ne borde pas cette bande : on n'invente rien
		}
		const double Signe = (Plus >= Moins) ? 1.0 : -1.0;
		OutRaison = ERubanRaison::Pose;
		OutLargeurCm = LargeurCm + 2.f * CollierCm;
		// Le point le plus EXTERIEUR du ruban est a (largeur + collier) de la ligne
		// porteuse : c'est ce decalage-la qu'on rabote, et l'echelle obtenue sert
		// AUSSI a BuildRoad pour sa propre demi-largeur — sans quoi le noeud
		// papillon reapparaitrait a l'interieur du constructeur.
		TArray<float> EchLigne;
		OutRabotes = EchelleAntiRetournement(Profil.Pts, Nrm, LargeurCm + CollierCm, EchLigne);
		OutAxe.SetNumUninitialized(N);
		OutBordLibre.SetNumUninitialized(N);
		OutZ.SetNumUninitialized(N);
		for (int32 i = 0; i < N; ++i)
		{
			const double Ech = (double)EchLigne[i];
			// ⭐ LE Z DU PROPRIETAIRE SE LIT CHEZ LE PROPRIETAIRE, PAS SUR LA
			// FRONTIERE. Sur la ligne meme, `At()` est AMBIGU par definition : elle
			// longe une discontinuite, et deux echantillons voisins tombent de part
			// et d'autre (mesure sur les bandes d'ouvrage : 3,7 m d'ecart d'un
			// echantillon au suivant — un ruban en dents de scie). On sonde donc
			// LEGEREMENT A L'INTERIEUR du proprietaire, ou la surface est definie.
			// Ce n'est PAS un lissage (13.1 : aucune nappe) : c'est un echantillon
			// deplace, chaque valeur reste une cote MESUREE de la surface rendue.
			const FVector2D Chez = Profil.Pts[i] - Nrm[i] * (Signe * (double)SondeProprioCm);
			const float ZP = RGZ.At(Chez.X, Chez.Y);
			OutZ[i] = (ZP > TNumericLimits<float>::Lowest()) ? ZP : Profil.Z[i];
			// L'axe du ruban : au milieu de la bande. Son BORD LIBRE (celui qui
			// donne sur l'organique) est a l'autre bout — c'est lui que la loi
			// d'interface viendra coudre.
			OutAxe[i] = Profil.Pts[i] + Nrm[i] * (Signe * (double)LargeurCm * 0.5 * Ech);
			OutBordLibre[i] = Profil.Pts[i] + Nrm[i] * (Signe * (double)OutLargeurCm * Ech);
		}
		// ⚠️ ET C'EST ICI QUE SE JOUE LE NOEUD PAPILLON, pas plus haut : le
		// constructeur de ruban recalcule SES PROPRES normales sur l'AXE qu'on lui
		// donne, et decale de +-demi-largeur DEPUIS CET AXE. Un rabotage calcule
		// sur la ligne porteuse ne garde donc pas ce que BuildRoad fabrique
		// vraiment (erreur payee : le pli etait toujours la apres le premier
		// correctif). L'echelle rendue est celle de l'AXE, avec SES normales et
		// SON decalage — exactement la geometrie que le constructeur va poser.
		TArray<FVector2D> NrmAxe;
		NormalesSommet(OutAxe, NrmAxe);
		OutRabotes += EchelleAntiRetournement(OutAxe, NrmAxe, OutLargeurCm * 0.5f, OutEchelle);
		return true;
	}

	/**
	 * ④ LA LOI D'INTERFACE — LA COUTURE.
	 *
	 * Une face VERTICALE cousue SOMMET POUR SOMMET entre deux cotes d'une
	 * frontiere : le haut suit la surface du proprietaire, le bas suit la
	 * surface voisine. Il n'y a aucun test de normale directionnelle (13.3) :
	 * l'ORIENTATION est celle que donnent les deux surfaces elles-memes — la
	 * face regarde toujours vers le BAS-COTE, celui d'ou on la voit.
	 *
	 * Rend le nombre de quads poses et cumule la longueur cousue (en cm).
	 */
	int32 CoudreFace(FCityMeshBuilder& QM, const TArray<FVector2D>& BordIn,
		const TArray<float>& ZHautIn, const TArray<float>& ZBasIn, float SeuilCm,
		const FResolvedSurface* Pierre, const FVector3f& Teinte, float& OutLongueurCm,
		// Le PLAFOND : au-dela, ce n'est plus une couture mais un ouvrage deja
		// bati — on ne coud pas, et on COMPTE. 0 = pas de plafond.
		float HauteurMaxCm, int32& OutTropHaut, float& OutMarcheMaxCm,
		// La direction, en plan, du COTE BAS — celui d'ou la face se voit. Elle est
		// MESUREE (comparaison des deux surfaces voisines), jamais devinee. Nulle =
		// on ne verifie pas. Le sens d'enroulement est corrige NUMERIQUEMENT ici :
		// aucun raisonnement de signe ne survit a un contour qui tourne (13.3).
		const FVector2D& DirBas = FVector2D::ZeroVector)
	{
		if (BordIn.Num() < 2)
		{
			return 0;
		}
		TArray<FVector2D> Bord = BordIn;
		TArray<float> ZHaut = ZHautIn;
		TArray<float> ZBas = ZBasIn;
		if (!DirBas.IsNearlyZero() && Bord.Num() >= 2 && ZHaut.Num() == Bord.Num())
		{
			// Normale du quad telle que l'enroulement la produira, sur le segment
			// median. Si elle tourne le dos au bas-cote, on retourne le contour.
			const int32 M = Bord.Num() / 2;
			const int32 A = FMath::Clamp(M - 1, 0, Bord.Num() - 2);
			const FVector2D T = Bord[A + 1] - Bord[A];
			const FVector2D NorPlan(T.Y, -T.X);
			if (FVector2D::DotProduct(NorPlan, DirBas) < 0.0)
			{
				Algo::Reverse(Bord);
				Algo::Reverse(ZHaut);
				Algo::Reverse(ZBas);
			}
		}
		const int32 N = Bord.Num();
		if (N < 2 || ZHaut.Num() != N || ZBas.Num() != N)
		{
			return 0;
		}
		const FPolygonGroupID Groupe = Pierre
			? QM.GetOrCreateGroup(Pierre->SlotName(), Pierre->Material) : QM.WallGroup;
		int32 Quads = 0;
		float Arc = 0.f;
		for (int32 i = 0; i + 1 < N; ++i)
		{
			const float SegCm = (float)(Bord[i + 1] - Bord[i]).Size();
			const float HA = ZHaut[i] - ZBas[i];
			const float HB = ZHaut[i + 1] - ZBas[i + 1];
			OutMarcheMaxCm = FMath::Max(OutMarcheMaxCm, FMath::Max(HA, HB));
			// Les deux surfaces se touchent deja (ou le haut est ENTERRE sous le
			// bas : l'objet est noye, ce qui est benin) — rien a coudre.
			if (HA <= SeuilCm && HB <= SeuilCm)
			{
				Arc += SegCm;
				continue;
			}
			// ⛔ AU-DELA D'UN NIVEAU, CE N'EST PLUS UNE COUTURE. C'est un mur de
			// quai, un flanc de pont, un mur de soutenement : un OUVRAGE, qui a
			// deja sa face. Coudre ici doublerait un mur existant.
			if (HauteurMaxCm > 0.f && (HA > HauteurMaxCm || HB > HauteurMaxCm))
			{
				++OutTropHaut;
				Arc += SegCm;
				continue;
			}
			const float BasA = ZBas[i], BasB = ZBas[i + 1];
			const float HautA = FMath::Max(ZHaut[i], BasA), HautB = FMath::Max(ZHaut[i + 1], BasB);
			const FVector3f P[4] = {
				FVector3f((float)Bord[i].X, (float)Bord[i].Y, BasA),
				FVector3f((float)Bord[i + 1].X, (float)Bord[i + 1].Y, BasB),
				FVector3f((float)Bord[i + 1].X, (float)Bord[i + 1].Y, HautB),
				FVector3f((float)Bord[i].X, (float)Bord[i].Y, HautA) };
			// La normale sort de l'ENROULEMENT des sommets, comme partout dans le
			// projet : elle est une propriete geometrique du quad, pas un choix.
			FVector3f Nor = FVector3f::CrossProduct(P[1] - P[0], P[3] - P[0]);
			if (Nor.SizeSquared() < 1e-6f)
			{
				Arc += SegCm;
				continue;
			}
			Nor.Normalize();
			// UV0 en METRES, comme tous les revetements : U = abscisse curviligne,
			// V = hauteur. La pierre de quai se lit donc a sa vraie echelle.
			const FVector2f UV[4] = {
				FVector2f(Arc * 0.01f, 0.f),
				FVector2f((Arc + SegCm) * 0.01f, 0.f),
				FVector2f((Arc + SegCm) * 0.01f, (HautB - BasB) * 0.01f),
				FVector2f(Arc * 0.01f, (HautA - BasA) * 0.01f) };
			QM.AddPoly(Groupe, P, 4, Nor, UV, Teinte);
			++Quads;
			OutLongueurCm += SegCm;
			Arc += SegCm;
		}
		return Quads;
	}
}

FCitySurfacesSummary UCityImportTools::ImportCitySurfaces(const FString& JsonFilePath,
	const FString& AssetFolder, const FString& WallMaterialPath, float CellSizeM, FVector Location,
	const FCityGenProfile& Profile)
{
	FCitySurfacesSummary Summary;

	// Profil effectif + MNT charge UNE fois pour tout l'import (jalon J2).
	const FCityGenProfile Gen = Profile.Resolved();
	FDrapeContext Drape;
	if (!MakeDrapeContext(Gen, Drape))
	{
		return Summary;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read surfaces file '%s'."), *JsonFilePath));
		return Summary;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("Surfaces file is not valid JSON."));
		return Summary;
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder must be a package path such as /Game/City/Capitole."));
		return Summary;
	}
	if (CellSizeM < 20.f)
	{
		RaiseError(TEXT("CellSizeM must be at least 20."));
		return Summary;
	}
	UMaterialInterface* WallMat = LoadMaterialOrDefault(WallMaterialPath);
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!WallMat || !World)
	{
		if (!World)
		{
			RaiseError(TEXT("No editor world is loaded."));
		}
		return Summary;
	}

	// --- LOT VELOCITE : MODE DISTRICT (voir FCityGenProfile::CellFilter) ---------
	TSet<FIntPoint> CellSet;
	const bool bCellFilter = ParseCellFilter(Gen.CellFilter, CellSet);
	const float FilterCellM = (Gen.CellFilterSizeM > 0.f) ? Gen.CellFilterSizeM : CellSizeM;
	if (bCellFilter && !FMath::IsNearlyEqual(FilterCellM, CellSizeM))
	{
		RaiseError(FString::Printf(
			TEXT("CellFilter est exprime pour des cellules de %.0f m et cet import travaille a %.0f m."),
			FilterCellM, CellSizeM));
		return Summary;
	}

	// Idempotence : un re-import remplace les surfaces existantes.
	// Mode district : purge BORNEE aux cellules visees ; CitySurfaceTrees (acteur
	// unique, non decoupe par cellule) est conserve, et donc pas reconstruit non plus.
	TArray<AActor*> ToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		if (L.StartsWith(TEXT("SM_Surface_")))
		{
			FIntPoint K;
			if (!bCellFilter || (CellFromLabel(L, TEXT("SM_Surface_"), K) && CellSet.Contains(K)))
			{
				ToDestroy.Add(*It);
			}
		}
		else if (L == TEXT("CitySurfaceTrees") && !bCellFilter)
		{
			ToDestroy.Add(*It);
		}
	}
	for (AActor* A : ToDestroy)
	{
		World->DestroyActor(A);
	}

	const float Cell = CellSizeM * 100.f;
	auto CelluleVisee = [&CellSet, bCellFilter, Cell](const FVector2D& P)
	{
		return !bCellFilter || CellSet.Contains(
			FIntPoint(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell)));
	};
	TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>> Cells;
	auto GetCellKey = [&Cells](const FIntPoint& Key) -> FCityMeshBuilder&
	{
		TUniquePtr<FCityMeshBuilder>& B = Cells.FindOrAdd(Key);
		if (!B)
		{
			B = MakeUnique<FCityMeshBuilder>();
		}
		return *B;
	};
	auto GetCell = [&](const FVector2D& P) -> FCityMeshBuilder&
	{
		return GetCellKey(FIntPoint(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell)));
	};
	auto ReadPts = [](const TArray<TSharedPtr<FJsonValue>>& In, TArray<FVector2D>& Out)
	{
		for (const TSharedPtr<FJsonValue>& V : In)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() >= 2)
			{
				Out.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
			}
		}
	};
	auto Centroid = [](const TArray<FVector2D>& Pts)
	{
		FVector2D C(0, 0);
		for (const FVector2D& P : Pts) { C += P; }
		return C / Pts.Num();
	};

	// Empilement decimetrique sous les routes (55+) : parc 15 < bois 20 < eau 30 <
	// rail 45 (cf. BuildRoad — ecarts centimetriques = z-fight garanti a 2 km en GLES).
	const FVector3f WaterTint(0.16f, 0.30f, 0.38f);
	const FVector3f ForestTint(0.20f, 0.34f, 0.16f);
	const FVector3f ParkTint(0.35f, 0.48f, 0.22f);
	TArray<FTransform> ScatterXf;

	// J3c point 2 : revetements Megascans des surfaces vertes (l'eau et les rails
	// gardent leur film teinte en v1). Desactive = surfaces historiques a l'octet pres.
	FSurfaceLibrary Surfaces;
	Surfaces.Init(Gen.bSurfaceMaterials, Gen.SurfacesFolder);

	// LOT EAU : quand la vraie surface en eau est active, le FILM teinte historique
	// n'est plus construit — c'est la surface a sa cote mesuree qui le remplace, et
	// deux plans a la meme place, c'est du z-fight garanti. `bWaterFilmsHistorique`
	// les restitue pour refaire l'A/B ; `bWater=false` aussi (rollback total).
	const bool bFilmsEauHistoriques = (!Gen.bWater) || Gen.bWaterFilmsHistorique;
	const TArray<TSharedPtr<FJsonValue>>* WaterJson = nullptr;
	if (bFilmsEauHistoriques && Root->TryGetArrayField(TEXT("water"), WaterJson))
	{
		for (const TSharedPtr<FJsonValue>& V : *WaterJson)
		{
			TArray<FVector2D> Pts;
			ReadPts(V->AsObject()->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 3)
			{
				continue;
			}
			if (!CelluleVisee(Centroid(Pts)))
			{
				continue;   // mode district : polygone hors des cellules visees
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			// Desktop (J2 §3.4) : l'eau n'est PAS drapee — plan horizontal au
			// percentile bas (p10) du MNT sous le polygone (le MNT sol nu descend
			// dans le lit ; la Garonne reste plane par troncon), offset conserve.
			float ZWater = 30.f;
			if (Drape.IsActive())
			{
				ZWater += Drape.Sampler->PercentileAltCmInPolygon(Pts, 0.10f) - Drape.AltCapCm;
			}
			BuildFlatPolygon(GetCell(Centroid(Pts)), Pts, ZWater, WaterTint);
			++Summary.Water;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* GreenJson = nullptr;
	if (Root->TryGetArrayField(TEXT("green"), GreenJson))
	{
		int32 GreenIndex = 0;
		for (const TSharedPtr<FJsonValue>& V : *GreenJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 3)
			{
				continue;
			}
			// Mode district : l'INDEX AVANCE quand meme (il commande l'etagement Z du
			// polygone et, si bVariedGrass, sa classe d'herbe).
			if (!CelluleVisee(Centroid(Pts)))
			{
				++GreenIndex;
				continue;
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			const bool bForest = O->GetStringField(TEXT("k")) == TEXT("forest");
			// Etagement PAR POLYGONE : les verts se chevauchent entre eux (parc sur
			// pelouse, bois sur parc) — a hauteur egale ils z-fightaient encore.
			// Desktop : drapes sur le MNT par sommet, l'etagement reste RELATIF.
			const float ZJitter = (GreenIndex % 5) * 1.5f;
			TArray<float> GreenTerrainZ;
			const TArray<float>* GreenTerrainZPtr = nullptr;
			if (Drape.IsActive())
			{
				ComputePolylineZ(Pts, Drape, /*bBridge=*/false, GreenTerrainZ);
				GreenTerrainZPtr = &GreenTerrainZ;
			}
			// v5 point 3 — ASSAINISSEMENT DES ESPACES VERTS. Verdict utilisateur sur la
			// v4b : « spaghetti des espaces verts ». Les polygones verts d'OSM se
			// CHEVAUCHENT largement (un parc porte souvent 3 ou 4 anneaux empiles :
			// leisure=park, landuse=grass, natural=wood...) ; y faire alterner trois
			// herbes differentes transformait chaque chevauchement en frontiere visible.
			// Une seule herbe (grass_cut) pour TOUS les verts : les chevauchements
			// deviennent invisibles, il n'y a plus rien a harmoniser.
			// bVariedGrass (defaut false) : l'alternance historique reste en code pour
			// un usage futur — berges, friches — la ou la variete se justifiera.
			const FSurfaceClass* GreenClass = &GSurfGrassCut;
			if (Gen.bVariedGrass && bForest)
			{
				GreenClass = (((uint32)GreenIndex * 2654435761u) >> 16) % 2u == 0u
					? &GSurfGrassUncut : &GSurfGrassWild;
			}
			BuildFlatPolygon(GetCell(Centroid(Pts)), Pts, (bForest ? 20.f : 12.f) + ZJitter,
				bForest ? ForestTint : ParkTint, GreenTerrainZPtr, Surfaces.Resolve(GreenClass));
			++GreenIndex;
			++Summary.Green;

			// Bois : arbres proceduraux disperses sur une grille de 28 m avec jitter,
			// plafond global — reperage leger, pas une foret dense.
			if (bForest && ScatterXf.Num() < 80000)
			{
				FBox2D Box(ForceInit);
				for (const FVector2D& P : Pts) { Box += P; }
				constexpr float Step = 2800.f;
				for (float Y = Box.Min.Y; Y <= Box.Max.Y; Y += Step)
				{
					for (float X = Box.Min.X; X <= Box.Max.X; X += Step)
					{
						const int32 Seed = FMath::RoundToInt32(X * 0.001f + Y * 0.017f);
						const float Jx = (FMath::Frac(FMath::Sin(Seed * 12.9898f) * 43758.5453f) - 0.5f) * Step * 0.6f;
						const float Jy = (FMath::Frac(FMath::Sin(Seed * 78.233f) * 12543.21f) - 0.5f) * Step * 0.6f;
						const FVector2D Q(X + Jx, Y + Jy);
						if (!PointInRing(Pts, Q))
						{
							continue;
						}
						const float Yaw = FMath::Frac(FMath::Sin(Seed * 39.11f) * 6543.87f) * 360.f;
						const float Scale = 0.9f + 0.6f * FMath::Frac(FMath::Sin(Seed * 3.7f) * 971.3f);
						ScatterXf.Emplace(FRotator(0, Yaw, 0),
							FVector(Q.X, Q.Y, Drape.GroundZ(Q.X, Q.Y)), FVector(Scale));
						++Summary.ScatterTrees;
						if (ScatterXf.Num() >= 80000)
						{
							break;
						}
					}
					if (ScatterXf.Num() >= 80000)
					{
						break;
					}
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RailsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("rails"), RailsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *RailsJson)
		{
			TArray<FVector2D> Pts;
			ReadPts(V->AsObject()->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 2)
			{
				continue;
			}
			// Mode district : l'INDEX AVANCE (il ensemence la variation de BuildRail).
			if (!CelluleVisee(Pts[0]))
			{
				++Index;
				continue;
			}
			// Desktop : meme traitement que les routes (re-echantillonnage + drapage).
			const TArray<FVector2D>* RailPts = &Pts;
			TArray<FVector2D> Resampled;
			TArray<float> TerrainZ;
			const TArray<float>* TerrainZPtr = nullptr;
			if (Drape.IsActive())
			{
				if (Gen.RoadResampleStepCm > 0.f)
				{
					Resampled = ResamplePolyline(Pts, Gen.RoadResampleStepCm);
					RailPts = &Resampled;
				}
				ComputePolylineZ(*RailPts, Drape, /*bBridge=*/false, TerrainZ);
				TerrainZPtr = &TerrainZ;
			}
			BuildRail(GetCell(Pts[0]), *RailPts, 400.f, Index, TerrainZPtr);
			++Summary.Rails;
			++Index;
		}
	}

	// -----------------------------------------------------------------------------
	// LOT EAU — LA SURFACE EN EAU, A SA COTE MESUREE (side-car SourceData/Eau).
	//
	// Le side-car porte, PAR SOMMET, l'altitude NGF de la surface, mesuree sur le
	// MNT LiDAR (qui ne penetre pas l'eau : ses retours SONT la surface). Un plan
	// UNIQUE par polygone ne peut pas marcher — mesure du proto : le polygone de la
	// Garonne traverse la chaussee du Bazacle, 6 m de chute, et son p10 posait le
	// plan 6 m SOUS le lit toulousain (fleuve a sec sous tous les ponts).
	//
	// La geometrie va dans la cellule PROPRIETAIRE (celle du fichier), jamais dans
	// celle du centroide : c'est la garde du Playbook S11.6 contre la fuite de
	// geometrie vers la cellule voisine en mode district.
	// AUCUNE collision : comme tous les films de surface, le mesh est cree sans.
	// -----------------------------------------------------------------------------
	if (Gen.bWater)
	{
		const FString EauDir = WaterDir(Gen);
		TArray<FString> Fichiers;
		IFileManager::Get().FindFiles(Fichiers, *(EauDir / TEXT("eau_*.json")), true, false);
		const FString MatEau = Gen.WaterMaterialPath.IsEmpty()
			? FString(TEXT("/Game/Dev/MI_CityWater.MI_CityWater")) : Gen.WaterMaterialPath;
		FResolvedSurface SurfEau;
		SurfEau.Class = &GSurfWater;
		SurfEau.Material = LoadObject<UMaterialInterface>(nullptr, *MatEau, nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		if (!SurfEau.Material)
		{
			// Display et NON Warning (l'automation eleve les warnings en erreurs) :
			// sans materiau la surface est quand meme posee, avec le repli du mesh.
			UE_LOG(LogCityImport, Display,
				TEXT("LOT EAU : materiau '%s' introuvable — la surface en eau prend le materiau de repli du mesh."),
				*MatEau);
		}
		// Repli quand le side-car ne porte pas d'ecoulement : (0,5 ; 0,5) = derive
		// nulle. Ce n'est plus « le materiau ne lit pas la VertexColor » : il la lit
		// desormais, et elle porte la DIRECTION DE L'ECOULEMENT (cf. FCityWaterBody).
		const FVector3f TeinteEau(0.5f, 0.5f, 1.f);
		int32 CellulesEau = 0, EauTailleKo = 0, WaterFlowBodies = 0;
		int32 WaterBodiesPreTri = 0;
		double EauTailleCuiteM = 0.0, AireEau = 0.0;
		for (const FString& Fichier : Fichiers)
		{
			FString Reste = FPaths::GetBaseFilename(Fichier);
			Reste.RemoveFromStart(TEXT("eau_"));
			FString Sx, Sy;
			if (!Reste.Split(TEXT("_"), &Sx, &Sy, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				continue;
			}
			const FIntPoint CelluleEau(FCString::Atoi(*Sx), FCString::Atoi(*Sy));
			if (bCellFilter && !CellSet.Contains(CelluleEau))
			{
				continue;
			}
			TArray<FCityWaterBody> Corps;
			if (!LoadWaterCell(EauDir, CelluleEau.X, CelluleEau.Y, CellSizeM,
				Drape.IsActive() ? Drape.AltCapCm : 0.f, Corps, EauTailleKo, EauTailleCuiteM) ||
				Corps.Num() == 0)
			{
				continue;
			}
			++CellulesEau;
			for (FCityWaterBody& W : Corps)
			{
				if (W.ZCm.Num() != W.PtsCm.Num() || W.PtsCm.Num() < 3)
				{
					++Summary.WaterSkipped;
					UE_LOG(LogCityImport, Display,
						TEXT("LOT EAU : piece ECARTEE cellule %d_%d, cleabs=%s, %.0f m2 — cote absente sur au moins un sommet."),
						CelluleEau.X, CelluleEau.Y, *W.Cleabs, W.AreaM2);
					continue;
				}
				if (SignedArea(W.PtsCm) < 0)
				{
					const int32 NPts = W.PtsCm.Num();
					Algo::Reverse(W.PtsCm);
					Algo::Reverse(W.ZCm);
					if (W.Flux.Num() == W.PtsCm.Num())
					{
						Algo::Reverse(W.Flux);
					}
					// FRONTIERE-Z : les indices cuits designent l'ordre D'AVANT le
					// retournement. On les renumerote (i -> N-1-i) ; le SENS de
					// chaque triangle est une propriete geometrique de ses trois
					// sommets, il ne change pas.
					for (int32& I : W.Tris)
					{
						I = NPts - 1 - I;
					}
				}
				// BERGES : l'ecoulement voyage dans la COULEUR DE SOMMET
				// (R = 0,5 + 0,5 dx, G = 0,5 + 0,5 dy, B = 1). Sans donnee de flux,
				// on n'invente rien : (0,5 ; 0,5) = derive nulle, et le materiau
				// retombe sur son ondulation sur place.
				TArray<FVector3f> Couleurs;
				if (W.Flux.Num() == W.PtsCm.Num())
				{
					Couleurs.Reserve(W.Flux.Num());
					for (const FVector2D& F : W.Flux)
					{
						Couleurs.Add(FVector3f(0.5f + 0.5f * (float)F.X,
							0.5f + 0.5f * (float)F.Y, 1.f));
					}
					++WaterFlowBodies;
				}
				FCityMeshBuilder& B = GetCellKey(CelluleEau);
				const int32 Avant = B.MeshDesc.Triangles().Num();
				BuildFlatPolygon(B, W.PtsCm, 0.f, TeinteEau, &W.ZCm,
					SurfEau.Material ? &SurfEau : nullptr,
					Couleurs.Num() ? &Couleurs : nullptr,
					W.Tris.Num() >= 3 ? &W.Tris : nullptr);
				if (W.Tris.Num() >= 3)
				{
					++WaterBodiesPreTri;
				}
				const int32 Tris = B.MeshDesc.Triangles().Num() - Avant;
				if (Tris <= 0)
				{
					++Summary.WaterSkipped;
					UE_LOG(LogCityImport, Display,
						TEXT("LOT EAU : piece ECARTEE cellule %d_%d, cleabs=%s, %.0f m2 — triangulation vide."),
						CelluleEau.X, CelluleEau.Y, *W.Cleabs, W.AreaM2);
					continue;
				}
				++Summary.WaterBodies;
				Summary.WaterTris += Tris;
				AireEau += W.AreaM2;
			}
		}
		Summary.WaterCells = CellulesEau;
		Summary.WaterAreaM2 = FMath::RoundToInt(AireEau);
		UE_LOG(LogCityImport, Display,
			TEXT("BERGES eau : %d surfaces sur %d portent un ECOULEMENT lu dans la donnee (couleur de sommet)."),
			WaterFlowBodies, Summary.WaterBodies);
		UE_LOG(LogCityImport, Display,
			TEXT("FRONTIERE-Z eau : %d surfaces sur %d posent la TRIANGULATION CUITE "
				 "(Delaunay contrainte du side-car) ; les autres retombent sur "
				 "TriangulateRing (comportement historique)."),
			WaterBodiesPreTri, Summary.WaterBodies);
		UE_LOG(LogCityImport, Display,
			TEXT("LOT EAU : %d cellules lues, %d surfaces posees (%d m2, %d triangles), %d ecartees ; materiau '%s' %s — dossier '%s'."),
			CellulesEau, Summary.WaterBodies, Summary.WaterAreaM2, Summary.WaterTris,
			Summary.WaterSkipped, *MatEau, SurfEau.Material ? TEXT("charge") : TEXT("ABSENT"),
			*EauDir);
		if (EauTailleKo > 0)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("LOT EAU : %d cellules IGNOREES — side-car cuit pour des cellules de %.0f m, import a %.0f m."),
				EauTailleKo, EauTailleCuiteM, CellSizeM);
		}
	}

	// -----------------------------------------------------------------------------
	// ⭐ PARTITION ① — LE CONTRAT DE Z, PUBLIE.
	//
	// « La carte dit QUI possede ; ce side-car dit A QUELLE COTE. »
	// La passe de surfaces echantillonne la surface RENDUE (`FRenderedGroundZ::At`,
	// le lecteur canonique) le long des LIGNES PORTEUSES des bandes et des
	// FRONTIERES de la carte, et publie le resultat, versionne et empreinte.
	//
	// C'est une LECTURE : aucune geometrie n'est creee, deplacee ni supprimee ici.
	// L'acceptation ① (« rien ne bouge la ou rien ne devait bouger ») est donc
	// vraie PAR CONSTRUCTION pour cette etape — ce qui n'empeche pas de la
	// verifier par diff, et c'est fait.
	// -----------------------------------------------------------------------------
	if (Gen.bPublierProfilZ && PartitionSingleton().bActive)
	{
		const double T0 = FPlatformTime::Seconds();
		const FCityPartition& Part = PartitionSingleton();
		FRenderedGroundZ RGZ;
		RGZ.Init(Drape, Gen.GroundGridN, Cell);
		const float PasCm = FMath::Max(Gen.PartitionStepM, 0.05f) * 100.f;

		// Payload construit a la main : ConvertTo-Json/serializer explosent sur ce
		// volume, et le format doit rester diffable ligne a ligne.
		FString Corps;
		Corps.Reserve(1 << 20);
		const double SondeCm = (double)FMath::Max(Gen.CoutureSondeCm, 1.f);
		auto EcrireProfil = [&Corps, PasCm, SondeCm, &RGZ, &Summary](const TCHAR* Genre,
			int32 Index, const TCHAR* Etiquette, const TArray<FVector2D>& Ligne)
		{
			FProfilZ P;
			EchantillonnerProfil(Ligne, PasCm, RGZ, P);
			if (P.Pts.Num() < 2)
			{
				return;
			}
			// ⭐ LE CONTRAT PORTE LES TROIS COTES, et c'est ce qui en fait un
			// CONTRAT plutot qu'une mesure : `z` sur la ligne (ambigue par nature,
			// on la publie telle quelle), `za` et `zb` DE PART ET D'AUTRE, la ou
			// chaque surface est definie. La MARCHE — donc la couture — se derive
			// du fichier seul, sans rejouer le moteur.
			TArray<FVector2D> Nrm;
			NormalesSommet(P.Pts, Nrm);
			auto Ecrire = [&Corps](const TCHAR* Cle, const TArray<float>& Z)
			{
				Corps += FString::Printf(TEXT(",\"%s\":["), Cle);
				for (int32 i = 0; i < Z.Num(); ++i)
				{
					// Millimetres entiers : le contrat est une COTE, pas un flottant
					// a 7 chiffres dont les derniers seraient du bruit de mesure.
					const int64 Mm = (Z[i] > TNumericLimits<float>::Lowest())
						? (int64)FMath::RoundToDouble((double)Z[i] * 10.0) : (int64)0;
					Corps += (i ? TEXT(",") : TEXT(""));
					Corps += FString::Printf(TEXT("%lld"), Mm);
				}
				Corps += TEXT("]");
			};
			TArray<float> ZA, ZB;
			ZA.SetNumUninitialized(P.Pts.Num());
			ZB.SetNumUninitialized(P.Pts.Num());
			for (int32 i = 0; i < P.Pts.Num(); ++i)
			{
				const FVector2D Pa = P.Pts[i] + Nrm[i] * SondeCm;
				const FVector2D Pb = P.Pts[i] - Nrm[i] * SondeCm;
				ZA[i] = RGZ.At(Pa.X, Pa.Y);
				ZB[i] = RGZ.At(Pb.X, Pb.Y);
			}
			Corps += FString::Printf(TEXT("{\"g\":\"%s\",\"i\":%d,\"p\":\"%s\",\"n\":%d"),
				Genre, Index, Etiquette, P.Pts.Num());
			Ecrire(TEXT("z"), P.Z);
			Ecrire(TEXT("za"), ZA);
			Ecrire(TEXT("zb"), ZB);
			Corps += TEXT("}\n");
			Summary.ProfilPoints += P.Z.Num();
		};
		for (int32 bi = 0; bi < Part.Bandes.Num(); ++bi)
		{
			const FCityPartition::FBande& B = Part.Bandes[bi];
			if (!CelluleVisee(FVector2D(B.Cellule.X * (double)Cell + 1.0,
					B.Cellule.Y * (double)Cell + 1.0)))
			{
				continue;
			}
			for (const TArray<FVector2D>& L : B.Lignes)
			{
				EcrireProfil(TEXT("b"), bi, FCityPartition::NomDe(B.Proprio), L);
			}
			++Summary.ProfilBandes;
		}
		for (int32 fi = 0; fi < Part.Frontieres.Num(); ++fi)
		{
			const FCityPartition::FFrontiere& F = Part.Frontieres[fi];
			if (!CelluleVisee(FVector2D(F.Cellule.X * (double)Cell + 1.0,
					F.Cellule.Y * (double)Cell + 1.0)))
			{
				continue;
			}
			EcrireProfil(TEXT("f"), fi, F.bEngagee ? TEXT("engagee") : TEXT("libre"), F.Poly);
			++Summary.ProfilFrontieres;
		}
		// L'EMPREINTE du contrat : md5 du corps seul (l'en-tete porte des chronos
		// et le filtre de district — il n'a rien a faire dans une empreinte).
		FTCHARToUTF8 Utf8(*Corps);
		FMD5 Md5;
		Md5.Update((const uint8*)Utf8.Get(), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);
		FString Empreinte;
		for (int32 i = 0; i < 16; ++i)
		{
			Empreinte += FString::Printf(TEXT("%02x"), Digest[i]);
		}
		FString Sortie = FString::Printf(
			TEXT("{\"version\":\"profil_z/v1\",\"produit_par\":\"ImportCitySurfaces\",")
			TEXT("\"carte_version\":\"%s\",\"carte_md5\":\"%s\",\"carte_geometries_md5\":\"%s\",")
			TEXT("\"pas_m\":%.3f,\"cellule_m\":%.1f,\"unite_z\":\"mm\",\"repere_z\":\"monde Unreal (Capitole = 0)\",")
			TEXT("\"sonde_cm\":%.1f,\"cotes\":\"z = sur la ligne (ambigue), za/zb = de part et d'autre a sonde_cm\",")
			TEXT("\"lecteur\":\"FRenderedGroundZ::At\",\"district\":\"%s\",")
			TEXT("\"bandes\":%d,\"frontieres\":%d,\"points\":%d,\"empreinte\":\"%s\"}\n"),
			*Part.Version, *Part.Md5Fichier, *Part.Md5Geometries,
			Gen.PartitionStepM, CellSizeM, Gen.CoutureSondeCm, *Gen.CellFilter,
			Summary.ProfilBandes, Summary.ProfilFrontieres, Summary.ProfilPoints, *Empreinte);
		Sortie += Corps;
		const FString PDir = Gen.PartitionPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Partition"))
			: Gen.PartitionPath;
		const FString Chemin = FPaths::Combine(PDir,
			Gen.CellFilter.IsEmpty() ? TEXT("profil_z_v1.json")
									 : TEXT("profil_z_v1_district.json"));
		// ForceUTF8WithoutBOM : SaveStringToFile bascule en UTF-16 des qu'un
		// caractere sort de l'ASCII (piege paye au lot A-ter).
		const bool bEcrit = FFileHelper::SaveStringToFile(Sortie, *Chemin,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogCityImport, Display,
			TEXT("PARTITION ① CONTRAT DE Z %s : '%s' — %d bandes, %d frontieres, %d points, "
				 "pas %.2f m, empreinte %s (%.1f s). Lecture seule : AUCUNE geometrie touchee."),
			bEcrit ? TEXT("PUBLIE") : TEXT("NON ECRIT"), *Chemin,
			Summary.ProfilBandes, Summary.ProfilFrontieres, Summary.ProfilPoints,
			Gen.PartitionStepM, *Empreinte, FPlatformTime::Seconds() - T0);
	}

	// --- Assets + acteurs par cellule ---
	// SANS collision : ces surfaces sont des films de 1-3 cm poses sur la dalle de sol
	// qui porte deja la collision — le trimesh cuit etait ~200 Mo de RAM device pour rien.
	for (auto& Pair : Cells)
	{
		const FString Name = FString::Printf(TEXT("SM_Surface_%d_%d"), Pair.Key.X, Pair.Key.Y);
		UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / Name, *Pair.Value, WallMat, WallMat, false);
		++Summary.Meshes;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ApplyGroundTextureStreaming(Actor->GetStaticMeshComponent());
		Actor->SetActorLabel(Name);
	}

	// --- Arbres disperses : HISM dedie, mesh SM_CityTree reutilise s'il existe ---
	// Mode district : CitySurfaceTrees est un acteur UNIQUE (pas de decoupage par
	// cellule) — il n'a pas ete detruit, on ne le reconstruit donc pas.
	if (ScatterXf.Num() > 0 && bCellFilter)
	{
		UE_LOG(LogCityImport, Display,
			TEXT("Mode district : CitySurfaceTrees conserve tel quel (%d arbres disperses non reposes)."),
			ScatterXf.Num());
	}
	if (ScatterXf.Num() > 0 && !bCellFilter)
	{
		const FString TreePath = AssetFolder / TEXT("SM_CityTree");
		UStaticMesh* TreeMesh = LoadObject<UStaticMesh>(nullptr,
			*(TreePath + TEXT(".SM_CityTree")), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!TreeMesh)
		{
			FCityMeshBuilder TreeBuilder;
			BuildTree(TreeBuilder);
			TreeMesh = CreateMeshAsset(TreePath, TreeBuilder, WallMat, WallMat);
			++Summary.Meshes;
		}
		WallMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);

		AActor* TreeActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		USceneComponent* Root2 = NewObject<USceneComponent>(TreeActor, TEXT("Root"), RF_Transactional);
		TreeActor->SetRootComponent(Root2);
		TreeActor->AddInstanceComponent(Root2);
		Root2->RegisterComponent();
		Root2->SetWorldLocation(Location);
		UHierarchicalInstancedStaticMeshComponent* Hism =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(TreeActor, TEXT("Trees"), RF_Transactional);
		Hism->SetStaticMesh(TreeMesh);
		// LOT PIE : AVANT RegisterComponent — sinon les corps sont deja crees.
		int32 NbVegSansCollision = 0;
		PoserCollisionVegetation(Hism, Gen, NbVegSansCollision);
		Hism->SetupAttachment(Root2);
		TreeActor->AddInstanceComponent(Hism);
		Hism->RegisterComponent();
		UE_LOG(LogCityImport, Display,
			TEXT("LOT PIE : arbres disperses — %d composant(s) pose(s) SANS COLLISION."),
			NbVegSansCollision);
		// LOT VELOCITE (L3) : un seul AddInstances (un seul arbre spatial construit).
		Hism->AddInstances(ScatterXf, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
		TreeActor->SetActorLabel(TEXT("CitySurfaceTrees"));
	}

	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCityImport, Display,
		TEXT("Surfaces importees : %d eau, %d vert, %d rails, %d arbres disperses, %d meshes."),
		Summary.Water, Summary.Green, Summary.Rails, Summary.ScatterTrees, Summary.Meshes);
	return Summary;
}

// Placement de la vegetation EN C++ (comme les batiments) : le Z vient du
// FTerrainSampler autoritaire (Drape.GroundZ), pas d'un modele MNT re-invente en
// Python — d'ou plus d'arbres flottants. La logique data (x, y, scale, yaw) reste
// preparee en JSON ; ici on ne fait que poser, base-a-0, sur le vrai sol.
FCityVegSummary UCityImportTools::ImportVegetation(const FString& VegJsonPath,
	const FString& AssetFolder, FVector Location, const FCityGenProfile& Profile)
{
	FCityVegSummary Summary;

	// LOT PIE : compteur de sortie — combien de composants sont poses SANS COLLISION
	// (meshes sans primitive simple). Doit valoir 15 sur le proto (12 herbes + sureaux
	// + 2 acteurs de fosses) et JAMAIS 0 tant que `bVegCollisionHistorique` est false.
	int32 NbVegSansCollision = 0;

	// Profil effectif + MNT charge une fois : GroundZ identique a la pose des batiments.
	const FCityGenProfile Gen = Profile.Resolved();
	FDrapeContext Drape;
	if (!MakeDrapeContext(Gen, Drape))
	{
		return Summary;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *VegJsonPath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read vegetation file '%s'."), *VegJsonPath));
		return Summary;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("Vegetation file is not valid JSON."));
		return Summary;
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder must be a package path such as /Game/Dev/ProtoE2Sol2/Veg."));
		return Summary;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		RaiseError(TEXT("No editor world is loaded."));
		return Summary;
	}

	// --- LOT VELOCITE : MODE DISTRICT -------------------------------------------
	// La vegetation est le SEUL cas ou le filtre ne peut pas se contenter de « ne pas
	// regenerer » : ses acteurs sont groupes PAR MESH (contrainte HISM), pas par
	// cellule. Detruire CityVeg_SM_KikuyuGrass_03 pour re-semer un quartier effacerait
	// les 98 000 touffes de toute la ville.
	// La strategie retenue garde l'AUTORITE UNIQUE de cette passe : c'est toujours ce
	// meme code qui trace et qui pose ; on RETIRE de chaque HISM les seules instances
	// tombant dans l'emprise visee, puis on y ajoute celles qu'on vient de semer.
	TSet<FIntPoint> CellSet;
	bool bCellFilter = ParseCellFilter(Gen.CellFilter, CellSet);
	if (bCellFilter && Gen.CellFilterSizeM <= 0.f)
	{
		// Display (pas Warning : l'automation eleve les Warning en echec). Le refus est
		// EXPLICITE : une passe qui se croirait filtree et re-semerait toute la ville
		// coute 30 minutes et un diagnostic faux.
		UE_LOG(LogCityImport, Display,
			TEXT("Vegetation : CellFilter renseigne mais CellFilterSizeM = 0 — FILTRE IGNORE, passe complete. ")
			TEXT("ImportVegetation n'a pas de parametre CellSizeM : la taille de maille doit venir du profil."));
		bCellFilter = false;
		CellSet.Reset();
	}
	const float FilterCellCm = Gen.CellFilterSizeM * 100.f;
	auto DansLeFiltre = [&CellSet, bCellFilter, FilterCellCm](double Xcm, double Ycm)
	{
		return !bCellFilter || CellSet.Contains(FIntPoint(
			FMath::FloorToInt(Xcm / FilterCellCm), FMath::FloorToInt(Ycm / FilterCellCm)));
	};

	// Idempotence (passe vege-seule rejouable) : efface la vege precedente.
	// Mode district : on ne detruit RIEN, on VIDE l'emprise visee de chaque HISM.
	TMap<FString, AActor*> ActeursVeg;
	TMap<FString, UHierarchicalInstancedStaticMeshComponent*> HismVeg;
	// ⭐ Instances CONSERVEES (hors emprise) des SEULS acteurs qui en perdent. On ne
	// RETIRE pas : on RELIT, on garde ce qui reste, et on repose tout d'un coup.
	// MESURE qui a impose ce choix (premier run filtre, 4 cellules) :
	// RemoveInstances(indices) sur 99 580 instances reparties dans 31 HISM a coute
	// 264 s — soit 2,65 ms par instance retiree, la meme signature quadratique que
	// l'AddInstance unitaire du levier L3 ; le reste de la passe (trace + pose de
	// 98 297 instances) tenait dans 3,0 s. ClearInstances + UN AddInstances ramene
	// tout le paquet au regime lineaire deja mesure a la regeneration complete.
	TMap<FString, TArray<FTransform>> GardeesParActeur;
	int32 NumInstancesRetirees = 0;
	{
		TArray<AActor*> ToDestroy;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const FString L = It->GetActorLabel();
			if (!L.StartsWith(TEXT("CityVeg")))
			{
				continue;
			}
			if (!bCellFilter)
			{
				ToDestroy.Add(*It);
				continue;
			}
			// Indexation PAR LABEL D'ACTEUR (piege du Playbook §4 : les noms de
			// COMPOSANT ne sont uniques que dans un acteur — « Veg » les designe tous).
			TArray<UHierarchicalInstancedStaticMeshComponent*> Comps;
			It->GetComponents(Comps);
			if (Comps.Num() != 1)
			{
				continue;
			}
			ActeursVeg.Add(L, *It);
			HismVeg.Add(L, Comps[0]);
			const int32 N = Comps[0]->GetInstanceCount();
			TArray<FTransform> Gardees;
			Gardees.Reserve(N);
			int32 NRetire = 0;
			for (int32 i = 0; i < N; ++i)
			{
				FTransform Xf;
				if (!Comps[0]->GetInstanceTransform(i, Xf, /*bWorldSpace=*/true))
				{
					continue;
				}
				if (DansLeFiltre(Xf.GetTranslation().X, Xf.GetTranslation().Y))
				{
					++NRetire;
				}
				else
				{
					Gardees.Add(Xf);
				}
			}
			// Un acteur qui ne perd RIEN n'est pas touche du tout : ni vide, ni repose.
			if (NRetire > 0)
			{
				NumInstancesRetirees += NRetire;
				GardeesParActeur.Add(L, MoveTemp(Gardees));
			}
		}
		for (AActor* A : ToDestroy)
		{
			World->DestroyActor(A);
		}
	}
	if (bCellFilter)
	{
		UE_LOG(LogCityImport, Display,
			TEXT("Vegetation MODE DISTRICT : %d cellule(s) de %.0f m — %d acteurs CityVeg conserves, ")
			TEXT("%d instances retirees de l'emprise visee."),
			CellSet.Num(), Gen.CellFilterSizeM, ActeursVeg.Num(), NumInstancesRetirees);
	}

	const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
	if (!Root->TryGetArrayField(TEXT("instances"), Instances) || Instances->Num() == 0)
	{
		UE_LOG(LogCityImport, Display, TEXT("Vegetation : aucune instance dans '%s'."), *VegJsonPath);
		return Summary;
	}

	// -------------------------------------------------------------------------
	// POSE SUR LA SURFACE VISIBLE (films SM_Surface + dalle SM_Slab), pas GroundZ.
	// Verdict utilisateur (iteration 6) : les arbres FLOTTENT car ils etaient poses
	// sur GroundZ analytique. Verdict iteration 7 (30/07) : les arbres sont ENTERRES.
	// Mesure sunk_measure2 (2 873 arbres x 9 traces, collision film reelle) :
	//  - la passe precedente a pose 100 % des arbres sur la DALLE (z_central - z_base
	//    = 0,0 exactement pour 1380/1380 arbres au trace central dalle) : donner une
	//    collision aux films par CollisionTraceFlag + InvalidatePhysicsData +
	//    CreatePhysicsMeshes NE CUIT AUCUN trimesh (echec silencieux : le cook est lie
	//    au Build du mesh, Invalidate+Create seuls retombent sur le cache vide) ;
	//  - resultat visuel : base sous le film d'herbe (lift ~12 cm + drapage fin) —
	//    sunk mediane +11,6 cm, P90 +35,4 cm, max +5,8 m (parc en pente, cellule -1_0).
	// LA VOIE PROUVEE (diag 30/07, 209/246 hits) : DUPLIQUER chaque mesh de sol dans
	// le paquet transient, poser CTF_UseComplexAsSimple AVANT tout usage physique puis
	// Build() -> le trimesh cuit ; on trace contre des acteurs PROXY jetables.
	//  - proxys aussi pour les DALLES : leur collision d'origine est le mesh dedie
	//    16x16 (~28 m/sommet) alors que l'oeil voit le rendu 64x64 (~7 m) — le proxy
	//    (Nanite off, ComplexCollisionMesh nul) rend le Z du RENDU visible ;
	//  - les assets d'origine ne sont PAS touches : rien de dirty, rien a restaurer,
	//    cleanup = destruction des proxys (les duplicatas transient partent au GC).
	// La vegetation existante (Sol2Veg/Sol2Grass) a une collision DENSE de canopee
	// -> on l'IGNORE dans le trace, sinon un arbre se poserait sur un autre arbre.
	// Le trace ECC_Visibility descend TOUTE la colonne (multi-hit : on traverse les
	// toits et le mobilier) et rend le hit SOL (SM_Surface_/SM_Slab_) le plus HAUT.
	// AUCUN SOL DANS LA COLONNE = AUCUNE INSTANCE (plus de repli GroundZ analytique,
	// dernier vestige du modele MNT dans la pose : il faisait flotter les bordures).

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(VegPlaceSurface), /*bTraceComplex=*/true);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		if (L.StartsWith(TEXT("Sol2Veg")) || L.StartsWith(TEXT("Sol2Grass")) ||
			L.StartsWith(TEXT("CityVeg")) || L == TEXT("CitySurfaceTrees"))
		{
			TraceParams.AddIgnoredActor(*It);
		}
	}

	// --- Proxys de trace : duplicatas transient (collision qui cuit VRAIMENT) ---
	TArray<AStaticMeshActor*> TraceProxies;
	// Acteurs de sol d'ORIGINE effectivement doubles par un proxy : leurs hits sont
	// collectes pour le diagnostic mais N'ENTRENT PLUS dans la decision de pose.
	TSet<const AActor*> ProxySources;
	{
		// Mode district : seules les surfaces qui RECOUVRENT l'emprise visee sont
		// doublees. Le critere est la boite du mesh en monde et non son indice de
		// cellule : un polygone vert est range dans la cellule de son CENTROIDE mais
		// peut deborder largement sur ses voisines — la boite, elle, ne ment pas.
		// Duplication + Build() d'un mesh de sol 64x64 coute ~0,3 s : 75 -> 6 doubles,
		// c'est une part majeure du cout fixe de la passe.
		const FBox2D Emprise = bCellFilter
			? CellFilterBounds(CellSet, FilterCellCm, 0.f) : FBox2D(ForceInit);
		TArray<AStaticMeshActor*> SolActors;
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			const FString L = It->GetActorLabel();
			if (L.StartsWith(TEXT("SM_Surface_")) ||
				(L.StartsWith(TEXT("SM_Slab_")) && !L.EndsWith(TEXT("_Col"))))
			{
				if (bCellFilter)
				{
					FVector Origine, Etendue;
					It->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origine, Etendue);
					const FBox2D Boite(FVector2D(Origine.X - Etendue.X, Origine.Y - Etendue.Y),
						FVector2D(Origine.X + Etendue.X, Origine.Y + Etendue.Y));
					if (!Emprise.Intersect(Boite))
					{
						continue;
					}
				}
				SolActors.Add(*It);
			}
		}
		int32 ProxyIdx = 0;
		for (AStaticMeshActor* Src : SolActors)
		{
			UStaticMeshComponent* SrcComp = Src->GetStaticMeshComponent();
			UStaticMesh* SrcMesh = SrcComp ? SrcComp->GetStaticMesh() : nullptr;
			if (!SrcMesh)
			{
				continue;
			}
			UStaticMesh* Dup = DuplicateObject<UStaticMesh>(SrcMesh, GetTransientPackage());
			Dup->SetFlags(RF_Transient);
			Dup->GetNaniteSettings().bEnabled = false; // trimesh depuis le rendu plein res
			Dup->ComplexCollisionMesh = nullptr;       // dalle : pas le 16x16 dedie
			Dup->CreateBodySetup();
			Dup->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
			Dup->Build(/*bSilent=*/true);
			AStaticMeshActor* P = World->SpawnActor<AStaticMeshActor>(
				Src->GetActorLocation(), Src->GetActorRotation());
			P->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			P->SetActorTransform(Src->GetActorTransform());
			P->GetStaticMeshComponent()->SetStaticMesh(Dup);
			P->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
			P->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			P->SetActorHiddenInGame(true);
			P->SetIsTemporarilyHiddenInEditor(true);
			// Le prefixe SOL est conserve : le classement film/dalle du trace lit ce label.
			P->SetActorLabel(FString::Printf(TEXT("%s_TraceProxy%d"),
				Src->GetActorLabel().StartsWith(TEXT("SM_Surface_"))
					? TEXT("SM_Surface") : TEXT("SM_Slab"), ProxyIdx++));
			TraceProxies.Add(P);
			// LOT5 / defaut « arbre qui flotte AVEC sa fosse » : la source est DESORMAIS
			// retiree de la DECISION de pose. Sans cela, la colonne offrait DEUX surfaces
			// de sol — le proxy (geometrie RENDUE) et l'original, dont la collision est le
			// mesh DEDIE SM_Slab_*_Col (grille 16x16, ~32 m de pas) — et la regle « hit
			// sol le plus HAUT » retenait le MAXIMUM des deux. Sur toute bosse ou pente,
			// la collision grossiere passe AU-DESSUS du rendu : l'arbre montait dessus, et
			// ses 9 sondages de fosse aussi (ils voient la meme surface, coherente entre
			// eux) — d'ou un arbre ET sa fosse en l'air au-dessus d'un pave visible. Les
			// proxys existent justement pour porter le Z du RENDU : l'original n'a plus
			// voix au chapitre. Il n'est ecarte QUE parce que SON proxy existe : aucune
			// surface ne disparait du trace. Son hit reste NEANMOINS collecte, pour le
			// DIAGNOSTIC (ampleur exacte de l'ancien defaut, publiee dans .floating.json).
			ProxySources.Add(Src);
		}
		// Le Build des duplicatas peut etre asynchrone : tout finir AVANT de tracer.
		FStaticMeshCompilingManager::Get().FinishAllCompilation();
	}

	int32 TraceMiss = 0, NumFilmCentral = 0, NumDalleCentral = 0;
	int32 NumFosseCorrige = 0, NumMultiHitRecup = 0;
	// Compteurs du garde-fou « hit isole qui domine » (voir plus bas).
	int32 NumHitIsoleEcarte = 0, NumHitIsoleGarde = 0;
	// Sondages retombes sur une surface d'origine faute de proxy (doit rester a 0).
	int32 NumSansProxy = 0;

	// --- COLONNE COMPLETE --------------------------------------------------------
	// On ne s'arrete plus au PREMIER sol rencontre : on descend toute la colonne et on
	// COLLECTE chaque surface de sol, en distinguant le PROXY (geometrie RENDUE, la
	// seule que l'oeil voit) de l'ORIGINAL (sa collision dediee, grossiere). La regle
	// de pose ne lit que les proxys ; les originaux ne servent plus qu'a mesurer
	// l'ancien defaut. 24 etages : un original + son proxy par cellule, plus les toits
	// et le mobilier traverses.
	struct FSolHit
	{
		float Z = 0.f;
		bool bFilm = false;
		bool bProxy = false;
		FString Acteur;
	};
	auto TraceColumnAll = [&](double Xcm, double Ycm,
		TArray<FSolHit, TInlineAllocator<16>>& Out, bool& bOutTraversee)
	{
		Out.Reset();
		FVector Start(Xcm, Ycm, 50000.0);
		const FVector End(Xcm, Ycm, -50000.0);
		bOutTraversee = false;
		// Plancher d'arret : des qu'un sol est vu, on ne descend plus que de 20 m. Tout
		// ce qui compte (l'autre surface de la meme cellule, la dalle sous son film,
		// l'ecart rendu/collision) tient tres largement dedans ; au-dela on ne ferait
		// que payer les faces inferieures et les jupes.
		double Plancher = -50000.0;
		for (int32 Step = 0; Step < 24; ++Step)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, TraceParams))
			{
				return;
			}
			const AActor* HitActor = Hit.GetActor();
			const FString HL = HitActor ? HitActor->GetActorLabel() : FString();
			if (HL.StartsWith(TEXT("SM_Surface_")) || HL.StartsWith(TEXT("SM_Slab_")))
			{
				FSolHit H;
				H.Z = (float)Hit.ImpactPoint.Z;
				H.bFilm = HL.StartsWith(TEXT("SM_Surface_"));
				H.bProxy = HL.Contains(TEXT("TraceProxy"));
				H.Acteur = HL;
				Out.Add(MoveTemp(H));
				Plancher = FMath::Max(Plancher, Hit.ImpactPoint.Z - 2000.0);
			}
			else if (Out.Num() == 0)
			{
				// « Traversee » garde son sens d'origine : un toit ou du mobilier
				// FRANCHI avant d'atteindre le sol.
				bOutTraversee = true;
			}
			Start.Z = Hit.ImpactPoint.Z - 1.0;
			if (Start.Z <= End.Z + 1.0 || Start.Z <= Plancher)
			{
				return;
			}
		}
	};

	// Z de pose = surface RENDUE la plus haute de la colonne (proxys uniquement).
	//
	// GARDE-FOU « hit isole qui domine » : si la surface rendue la plus haute domine la
	// SUIVANTE de plus de 50 cm, elle est suspecte (couture entre deux cellules en
	// pente qui divergent, chant de dalle, marche). On demande alors leur avis a
	// QUATRE colonnes voisines a 1 m : si la majorite d'entre elles se range du cote du
	// hit BAS, c'est le hit haut qui est l'accident et il est ecarte. Sinon on le
	// garde : une marche ou un muret REELS sont vus par les voisins aussi. Le garde-fou
	// ne se declenche que sur un ecart franc, il est donc sans effet sur le cas normal
	// (film d'herbe drape quelques cm au-dessus de sa dalle).
	constexpr float GSolIsoleCm = 50.f;   // ecart a partir duquel on demande les voisins
	constexpr float GSolVoisinCm = 100.f; // portee de la consultation
	auto ChoisitSol = [&](double Xcm, double Ycm,
		const TArray<FSolHit, TInlineAllocator<16>>& Hits, float& OutZ, bool& bOutFilm) -> bool
	{
		int32 IdxHaut = INDEX_NONE, IdxSuiv = INDEX_NONE;
		for (int32 i = 0; i < Hits.Num(); ++i)
		{
			if (!Hits[i].bProxy)
			{
				continue;
			}
			if (IdxHaut == INDEX_NONE || Hits[i].Z > Hits[IdxHaut].Z)
			{
				IdxSuiv = IdxHaut;
				IdxHaut = i;
			}
			else if (IdxSuiv == INDEX_NONE || Hits[i].Z > Hits[IdxSuiv].Z)
			{
				IdxSuiv = i;
			}
		}
		if (IdxHaut == INDEX_NONE)
		{
			// FILET DE SECURITE : aucune surface RENDUE dans la colonne alors qu'une
			// surface d'origine y est. Ecarter l'original serait alors supprimer de la
			// vegetation pour cause d'outil, pas de terrain : on retombe sur lui et on
			// COMPTE le cas. Un compteur non nul en fin de passe signale un proxy qui
			// n'a pas cuit sa collision — c'est un defaut a corriger, pas a subir.
			for (int32 i = 0; i < Hits.Num(); ++i)
			{
				if (IdxHaut == INDEX_NONE || Hits[i].Z > Hits[IdxHaut].Z) { IdxHaut = i; }
			}
			if (IdxHaut == INDEX_NONE)
			{
				return false;
			}
			++NumSansProxy;
			OutZ = Hits[IdxHaut].Z;
			bOutFilm = Hits[IdxHaut].bFilm;
			return true;
		}
		int32 Retenu = IdxHaut;
		if (IdxSuiv != INDEX_NONE && Hits[IdxHaut].Z - Hits[IdxSuiv].Z > GSolIsoleCm)
		{
			const float ZHaut = Hits[IdxHaut].Z;
			const float ZBas = Hits[IdxSuiv].Z;
			int32 PourBas = 0, PourHaut = 0;
			static const float DX[4] = { 1.f, -1.f, 0.f, 0.f };
			static const float DY[4] = { 0.f, 0.f, 1.f, -1.f };
			TArray<FSolHit, TInlineAllocator<16>> HV;
			for (int32 K = 0; K < 4; ++K)
			{
				bool bTv = false;
				TraceColumnAll(Xcm + DX[K] * GSolVoisinCm, Ycm + DY[K] * GSolVoisinCm, HV, bTv);
				float ZV = 0.f;
				bool bVu = false;
				for (const FSolHit& H : HV)
				{
					if (H.bProxy && (!bVu || H.Z > ZV)) { ZV = H.Z; bVu = true; }
				}
				if (!bVu)
				{
					continue;
				}
				if (FMath::Abs(ZV - ZBas) < FMath::Abs(ZV - ZHaut)) { ++PourBas; }
				else { ++PourHaut; }
			}
			if (PourBas > PourHaut)
			{
				Retenu = IdxSuiv;
				++NumHitIsoleEcarte;
			}
			else
			{
				++NumHitIsoleGarde;
			}
		}
		OutZ = Hits[Retenu].Z;
		bOutFilm = Hits[Retenu].bFilm;
		return true;
	};

	// Interface historique, inchangee pour tous les appelants (couronne, 9 sondages).
	auto TraceColumnSol = [&](double Xcm, double Ycm, float& OutZ, bool& bOutFilm,
		bool& bOutTraversee) -> bool
	{
		TArray<FSolHit, TInlineAllocator<16>> Hits;
		TraceColumnAll(Xcm, Ycm, Hits, bOutTraversee);
		return ChoisitSol(Xcm, Ycm, Hits, OutZ, bOutFilm);
	};

	// --- DIAGNOSTIC DU DEFAUT « FLOTTANT » ---------------------------------------
	// Pour chaque instance on compare, sur la MEME colonne : l'ANCIENNE regle (hit sol
	// le plus haut, proxys ET originaux confondus) et la NOUVELLE (rendu seul). L'ecart
	// est exactement la hauteur a laquelle l'instance flottait. Publie en JSON.
	struct FFlottant
	{
		FString Mesh;
		double Xm = 0.0, Ym = 0.0;
		float ZAncien = 0.f, ZNouveau = 0.f;
		FString Coupable;
		FString Colonne;
	};
	TArray<FFlottant> Flottants;
	int32 NumFlottant30 = 0, NumFlottant100 = 0;
	float MaxFlottantCm = 0.f;
	TMap<FString, int32> CoupablesParActeur;

	// Instances rejetees (aucun sol dans la colonne) : tracees, jamais devinees.
	struct FSkippedInst
	{
		FString Mesh;
		double Xm = 0.0;
		double Ym = 0.0;
	};
	TArray<FSkippedInst> Skipped;
	// LOT5 point B — ARBRES SUR PENTE MINERALE : un arbre plante dans un talus pave
	// n'existe pas, et sa fosse carree posee de biais sur la pente etait l'incoherence
	// la plus visible du lot precedent. Decision utilisateur : « coherence globale
	// plutot qu'incoherences visuelles » -> on supprime l'INSTANCE ENTIERE (arbre +
	// fosse), on ne la rattrape pas. Les haies et les touffes ne sont pas concernees :
	// elles n'ont pas de fosse et un buisson sur talus est parfaitement credible.
	// Le plan local des 9 sondages de la fosse donne deja la pente : rien a mesurer en
	// plus. Seuil sur la pente BRUTE (avant l'ecretage a 20 deg du plan de pose).
	constexpr float GPenteMaxArbreMineralDeg = 15.f;
	TArray<FSkippedInst> SkippedPente;
	TArray<float> SkippedPenteDeg;

	// Fosses de plantation a poser (un seul HISM, construit apres la boucle).
	// v3 : rotation COMPLETE (la fosse epouse le plan local du sol) et echelle
	// individuelle (le cote suit l'empattement de SON arbre).
	struct FPitInst
	{
		double Xcm = 0.0;
		double Ycm = 0.0;
		float Zcm = 0.f;
		FRotator Rot = FRotator::ZeroRotator;
		float Scale = 1.f;
	};
	TArray<FPitInst> Pits;
	// Diagnostic de pose, publie dans le log de fin : sans lui, « ca a l'air mieux »
	// resterait une impression.
	int32 NumPitPlan = 0, NumPitPlat = 0, NumPitClampMin = 0, NumPitClampMax = 0;
	int32 NumPitOutliers = 0, NumPitLiftBorne = 0;
	double SumPitCoteCm = 0.0;
	float MaxPitGiteCm = 0.f;
	// COMPARATIF EXACT v2 / v3, sur LA MEME dalle rendue et les memes traces. Il est
	// gratuit (les 9 sondages de l'emprise servent deja a la pose) et il repond aux
	// trois defauts signales sans dependre d'une mesure exterieure, qui ne peut tracer
	// que la collision GROSSIERE des dalles (maillage de collision dedie, bien plus
	// large que le triangle rendu) et sous-estime donc les deux premiers.
	//   regle v2 = carre horizontal de 1,2 m cale au Z du centre, fond a -1,5 cm ;
	//   regle v3 = carre dimensionne, pose sur le plan local, fond a +1 cm.
	int32 NumCmpPits = 0;
	int32 N2Fond = 0, N2Coin = 0, N2Debord = 0, N2PtsOcc = 0;
	int32 N3Fond = 0, N3Coin = 0, N3Debord = 0, N3PtsOcc = 0;
	int32 NumCmpPts = 0;
	// Constantes de la v2, figees ici pour que le comparatif reste lisible meme quand
	// les constantes v3 bougeront.
	constexpr float GV2HalfCm = 60.f, GV2LiftCm = 2.5f, GV2SinkCm = 4.f,
		GV2InnerCm = 48.f;
	FRandomStream PitRand(20260730);  // deterministe : deux runs donnent la meme ville
	FGrassMaskSampler MaskSampler(GroundMasksDir(Gen));
	// Compteur de diagnostic : arbres que la NOUVELLE regle (sol rendu) classe
	// autrement que l'ancienne (canal R brut). Publie dans le log de fin.
	int32 NumFosseGagnee = 0, NumFossePerdue = 0;

	// --- LOT6 : retraction hors chaussee (points A et D) et fosses rondes (point C) ---
	// Fosses RONDES d'arbustes : meme mecanique que les carrees, un HISM dedie.
	TArray<FPitInst> ShrubPits;
	// Trace de CHAQUE deplacement : position d'origine, position retenue, course.
	struct FRetrait
	{
		FString Mesh;
		bool bArbre = false;
		double X0m = 0.0, Y0m = 0.0, X1m = 0.0, Y1m = 0.0;
		float CourseCm = 0.f;
		float DAvantCm = 0.f, DApresCm = 0.f, RequisCm = 0.f;
	};
	TArray<FRetrait> Retraits;
	TArray<FSkippedInst> RetraitImpossible;
	int32 NumRetractArbre = 0, NumRetractHaie = 0;
	int32 NumRetractRateArbre = 0, NumRetractRateHaie = 0;
	int32 NumRetractInchangeArbre = 0, NumRetractInchangeHaie = 0;
	int32 NumRetractParGradient = 0, NumRetractParBalayage = 0;
	double SumCourseCm = 0.0;
	float MaxCourseCm = 0.f;
	// Fosses rondes : diagnostic aligne sur celui des carrees.
	int32 NumShrubPitPlan = 0, NumShrubPitPlat = 0, NumShrubLiftBorne = 0;
	// Arbustes sur pente : l'arbuste reste, la fosse tombe (mesure du premier run).
	int32 NumShrubPitPente = 0;
	double SumShrubRayonCm = 0.0;

	// --- ALIGNEMENT DES FOSSES SUR LA VOIRIE ---------------------------------------
	// Une fosse d'arbre reelle est alignee sur sa rue, jamais posee de biais. Les
	// polylignes de BORDURE deja cuites par cellule (sols_<x>_<y>.json, les memes qui
	// servent a mailler le relief de la rue) donnent cette orientation gratuitement :
	// on prend la direction du segment de bordure le plus proche. Au-dela de la
	// portee, plus de rue identifiable -> lacet quasi nul plutot qu'un carre de biais.
	TArray<FVector4> CurbSegs;   // (ax, ay, bx, by) en cm
	{
		const FString MaskDir = GroundMasksDir(Gen);
		const float CellM = MaskSampler.GetCellSizeM();
		// Cellules ENUMEREES sur disque (meme patron que la passe de relief) : aucune
		// etendue devinee, la zone cuite est la seule verite.
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(MaskDir / TEXT("sols_*.json")), true, false);
		for (const FString& File : Files)
		{
			FString Rest = FPaths::GetBaseFilename(File);
			Rest.RemoveFromStart(TEXT("sols_"));
			FString Sx, Sy;
			if (!Rest.Split(TEXT("_"), &Sx, &Sy, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				continue;
			}
			FGroundMaskCell Cell;
			if (!LoadGroundMaskCell(MaskDir, FCString::Atoi(*Sx), FCString::Atoi(*Sy),
				CellM, Cell))
			{
				continue;
			}
			for (const TArray<FVector2D>& Line : Cell.Curbs)
			{
				for (int32 i = 0; i + 1 < Line.Num(); ++i)
				{
					CurbSegs.Add(FVector4(Line[i].X, Line[i].Y,
						Line[i + 1].X, Line[i + 1].Y));
				}
			}
		}
	}
	auto YawFosse = [&](double Xcm, double Ycm) -> float
	{
		double Best = (double)GPitAlignRangeCm * (double)GPitAlignRangeCm;
		float BestYaw = 0.f;
		bool bFound = false;
		for (const FVector4& S : CurbSegs)
		{
			const double Ax = S.X, Ay = S.Y, Bx = S.Z, By = S.W;
			const double Dx = Bx - Ax, Dy = By - Ay;
			const double L2 = Dx * Dx + Dy * Dy;
			if (L2 < 1.0)
			{
				continue;
			}
			double T = ((Xcm - Ax) * Dx + (Ycm - Ay) * Dy) / L2;
			T = FMath::Clamp(T, 0.0, 1.0);
			const double Px = Ax + T * Dx - Xcm;
			const double Py = Ay + T * Dy - Ycm;
			const double D2 = Px * Px + Py * Py;
			if (D2 < Best)
			{
				Best = D2;
				BestYaw = (float)FMath::RadiansToDegrees(FMath::Atan2(Dy, Dx));
				bFound = true;
			}
		}
		if (!bFound)
		{
			return (float)PitRand.FRandRange(-GPitFreeJitterDeg, GPitFreeJitterDeg);
		}
		// Un carre a la symetrie d'ordre 4 : seul le reste modulo 90 deg compte.
		BestYaw = FMath::Fmod(FMath::Fmod(BestYaw, 90.f) + 90.f, 90.f);
		return BestYaw + (float)PitRand.FRandRange(-GPitJitterDeg, GPitJitterDeg);
	};

	// --- LOT6 points A et D : RETRACTION HORS DE LA CHAUSSEE -----------------------
	// Rend  1 = position deplacee (X/Y modifies), 0 = deja degagee (rien touche),
	//      -1 = impossible dans la course maximale -> l'appelant NE POSE PAS l'instance.
	//
	// Deux etages, du moins cher au plus sur :
	//   1. LE GRADIENT du champ de distance, exactement ce que le brief demande : il
	//      pointe vers l'interieur de la chaussee, on recule donc dans son oppose. Pour
	//      un vrai champ de distance |grad| = 1, si bien que la course a parcourir vaut
	//      (d + requis) — on marche quand meme par pas de 10 cm et on RELIT le champ a
	//      chaque pas plutot que de faire confiance a cette estimation (le champ est
	//      quantifie sur 8 bits et sature a 2 m).
	//   2. UN BALAYAGE de 32 directions si le gradient est degenere (au fond d'une rue
	//      large, le champ sature : deux echantillons voisins lisent la meme valeur et
	//      leur difference est nulle) ou si la marche n'a rien trouve. On garde la
	//      direction dont la course est la PLUS COURTE : reculer d'un metre a l'oppose
	//      de la rue est juste, traverser la rue ne le serait pas.
	// Une position n'est acceptee que si elle est REELLEMENT degagee (relecture du
	// champ) : jamais de deplacement au juge.
	auto DegageChaussee = [&](double& Xcm, double& Ycm, float RequisCm,
		float& OutD0, float& OutD1, float& OutCourse) -> int32
	{
		OutD0 = MaskSampler.RoadDistCm(Xcm, Ycm);
		OutD1 = OutD0;
		OutCourse = 0.f;
		if (OutD0 <= -RequisCm)
		{
			return 0;   // deja assez loin du bord : on ne touche a rien
		}
		const int32 NPas = FMath::CeilToInt32(GRetraitMaxCm / GRetraitPasCm);

		// --- 1) le gradient ---------------------------------------------------------
		const float H = GRetraitGradPasCm;
		const float Gx = MaskSampler.RoadDistCm(Xcm + H, Ycm)
			- MaskSampler.RoadDistCm(Xcm - H, Ycm);
		const float Gy = MaskSampler.RoadDistCm(Xcm, Ycm + H)
			- MaskSampler.RoadDistCm(Xcm, Ycm - H);
		const FVector2D G(Gx, Gy);
		if (G.SizeSquared() > 1.f)
		{
			const FVector2D Dir = -G.GetSafeNormal();   // vers l'EXTERIEUR de la chaussee
			for (int32 K = 1; K <= NPas; ++K)
			{
				const float L = K * GRetraitPasCm;
				const double Nx = Xcm + Dir.X * L;
				const double Ny = Ycm + Dir.Y * L;
				const float D = MaskSampler.RoadDistCm(Nx, Ny);
				if (D <= -RequisCm)
				{
					Xcm = Nx;
					Ycm = Ny;
					OutD1 = D;
					OutCourse = L;
					++NumRetractParGradient;
					return 1;
				}
			}
		}

		// --- 2) le balayage de secours ----------------------------------------------
		float BestL = TNumericLimits<float>::Max();
		double BestX = Xcm, BestY = Ycm;
		float BestD = OutD0;
		for (int32 A = 0; A < GRetraitDirs; ++A)
		{
			const double Ang = 2.0 * PI * A / GRetraitDirs;
			const double Cx = FMath::Cos(Ang), Cy = FMath::Sin(Ang);
			for (int32 K = 1; K <= NPas; ++K)
			{
				const float L = K * GRetraitPasCm;
				if (L >= BestL)
				{
					break;   // cette direction ne peut plus faire mieux
				}
				const double Nx = Xcm + Cx * L;
				const double Ny = Ycm + Cy * L;
				const float D = MaskSampler.RoadDistCm(Nx, Ny);
				if (D <= -RequisCm)
				{
					BestL = L;
					BestX = Nx;
					BestY = Ny;
					BestD = D;
					break;
				}
			}
		}
		if (BestL < TNumericLimits<float>::Max())
		{
			Xcm = BestX;
			Ycm = BestY;
			OutD1 = BestD;
			OutCourse = BestL;
			++NumRetractParBalayage;
			return 1;
		}
		return -1;   // rue etroite bordee des deux cotes : instance non posee, comptee
	};

	// Groupe les instances par mesh (contrainte HISM : un composant = un seul mesh).
	// Le Z est calcule plus bas, une fois le mesh CHARGE : le rayon de fosse de la
	// couronne depend de sa canopee. La cle preserve l'ordre de premiere apparition.
	struct FVegInst
	{
		double X = 0.0;
		double Y = 0.0;
		float Scale = 1.f;
		float Yaw = 0.f;
	};
	TMap<FString, TArray<FVegInst>> ByMesh;
	// « kind » facultatif par instance : tree / hedge / clump. Le generateur de
	// donnees le pose (une seule autorite) ; en son absence on retombe sur une
	// heuristique de taille, plus bas, pour ne jamais dependre d'un nom d'asset.
	TMap<FString, FString> KindByMesh;
	for (const TSharedPtr<FJsonValue>& V : *Instances)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid())
		{
			continue;
		}
		const FString MeshPath = O->GetStringField(TEXT("mesh"));
		if (MeshPath.IsEmpty())
		{
			continue;
		}
		FVegInst Inst;
		Inst.X = O->GetNumberField(TEXT("x")) * 100.0;
		Inst.Y = O->GetNumberField(TEXT("y")) * 100.0;
		// Mode district : ecarte AVANT tout travail. C'est ici que se gagne l'essentiel
		// du temps — chaque instance retenue coute une colonne de traces complete.
		if (!DansLeFiltre(Inst.X, Inst.Y))
		{
			continue;
		}
		double Scale = 1.0; O->TryGetNumberField(TEXT("scale"), Scale);
		double Yaw = 0.0;   O->TryGetNumberField(TEXT("yaw"), Yaw);
		Inst.Scale = (float)Scale;
		Inst.Yaw = (float)Yaw;
		FString Kind;
		if (O->TryGetStringField(TEXT("kind"), Kind) && !Kind.IsEmpty())
		{
			KindByMesh.FindOrAdd(MeshPath) = Kind.ToLower();
		}
		ByMesh.FindOrAdd(MeshPath).Add(Inst);
	}

	// Un acteur + un HISM par mesh distinct. Aucun mesh recree, aucun materiau touche.
	for (auto& Pair : ByMesh)
	{
		const FString& MeshPath = Pair.Key;
		// Accepte le chemin de package nu (/Game/.../SM_x) ou l'objet (/Game/.../SM_x.SM_x).
		FString ObjectPath = MeshPath;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath += TEXT(".") + FPackageName::GetLongPackageAssetName(MeshPath);
		}
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Mesh)
		{
			UE_LOG(LogCityImport, Warning,
				TEXT("Vegetation : mesh introuvable '%s' (%d instances ignorees)."),
				*MeshPath, Pair.Value.Num());
			continue;
		}
		// Piege F.39 : sans l'usage ISM sur ses materiaux, les instances rendent en defaut.
		// V6 : meme regle pour l'usage NANITE, depuis que les touffes SM_KikuyuGrass_*
		// sont Nanite — sans ce flag, « missing usage flag Nanite » et le moteur
		// substitue le Materiau par Defaut (piege deja paye le 25/07 sur les toits).
		// Il est pose ICI, par le generateur, et non par un script joue apres coup :
		// c'est la doctrine V5 (une propriete rejouee a la main ne survit pas).
		const bool bMeshNanite = Mesh->IsNaniteEnabled();
		for (const FStaticMaterial& SM : Mesh->GetStaticMaterials())
		{
			if (SM.MaterialInterface)
			{
				SM.MaterialInterface->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
				if (bMeshNanite)
				{
					SM.MaterialInterface->CheckMaterialUsage(MATUSAGE_Nanite);
				}
			}
		}
		// VENT DES ARBRES : rien a faire ici, et c'est VOULU. Les meshes gardent leurs
		// materiaux d'origine, et le reglage "vent minimum mais vivant" valide par
		// l'utilisateur vit desormais DANS CES ASSETS MATERIAUX eux-memes (sauves le
		// 30/07), pas dans un override de composant ni dans un MIC intermediaire :
		//   /Game/NorwayMaple/Materials/{SimpleWind,Impostor}/MI_*
		//   /Game/EuropeanBeech/Materials/{Simplewind,Impostor}/MI_*
		//   -> Local Wind Strength 1 = 0,05  (tronc : c'est lui qui "etire" l'arbre entier)
		//      Local Wind Strength 2/3/4     = 0,10
		//      Wind Animation Strength       = 0,10
		//      Wind Noise Strength           = 0,10
		//      Additional Wind Animation Noise Strength = 0,10
		// (recette d'origine : work/SOL2/fix_drape.py "FIX C", 111 parametres, verifiee
		//  par verify_drape.log ; elle etait alors posee sur des MIC MI_WZ_* assignes aux
		//  anciens acteurs Sol2Veg_*, d'ou sa PERTE quand la vege est passee en C++ ici.)
		// Consequence : toute regeneration, sur n'importe quelle ville, herite du vent
		// reduit d'office. Ne PAS reintroduire de MIC vent ici, et ne pas toucher
		// EvaluateWorldPositionOffset : son defaut (true) est justement ce que l'etape
		// finale de la recette d'epoque avait retabli (fix_wpo_persist.py) — un WPO coupe
		// donne un arbre MORT, ce qui avait ete refuse.
		// Mesure de validation (30/07, meme camera, series a intervalles irreguliers,
		// aire de ciel decoupee par la couronne) : amplitude du mouvement 7856 -> 2212 px
		// (-72 %), ecart-type 2969 -> 647 (-78 %), pour un plancher de bruit de 7 a vent
		// nul : le mouvement est fortement reduit mais bien VIVANT.
		// Rayon de fosse : replique gen_surfaces_v5.py — rayon de canopee reel
		// (demi-somme des demi-emprises X/Y du mesh) x scale, clamp [4;6] m, +50 cm de
		// marge. Les touffes d'herbe (Clump) n'ont pas de fosse : pas de couronne (les
		// relever a l'herbe distante ferait flotter une touffe posee sur dalle visible).
		const FBoxSphereBounds MeshBounds = Mesh->GetBounds();
		const float CanopyRUnitCm = (float)(MeshBounds.BoxExtent.X + MeshBounds.BoxExtent.Y) * 0.5f;

		// --- Classe de vegetation : touffe / haie / arbre ---------------------------
		// Priorite au champ « kind » des donnees (une seule autorite). Sans lui,
		// heuristique de TAILLE, jamais de nom d'asset : une touffe fait quelques
		// dizaines de cm, un arbuste de haie moins de 3 m, un arbre davantage.
		const float UnitHeightCm = (float)MeshBounds.BoxExtent.Z * 2.f;
		float MedScale = 1.f;
		{
			TArray<float> Scales;
			Scales.Reserve(Pair.Value.Num());
			for (const FVegInst& I : Pair.Value)
			{
				Scales.Add(I.Scale);
			}
			Scales.Sort();
			MedScale = Scales.Num() ? Scales[Scales.Num() / 2] : 1.f;
		}
		const FString* KindPtr = KindByMesh.Find(MeshPath);
		const FString Kind = KindPtr ? *KindPtr : FString();
		const bool bClump = Kind.IsEmpty()
			? Mesh->GetName().Contains(TEXT("Clump"))
			: Kind == TEXT("clump");
		const bool bHedge = Kind.IsEmpty()
			? (!bClump && UnitHeightCm * MedScale < 300.f)
			: Kind == TEXT("hedge");
		const bool bTree = !bClump && !bHedge;

		// EMPATTEMENT du mesh, mesure UNE FOIS ici (et non par instance : c'est une
		// propriete du maillage). Il commande la taille des fosses plus bas.
		// LOT6 : mesure aussi pour les HAIES — elles ont desormais une fosse RONDE sur
		// sol mineral (point C), et leur empattement sert de degagement minimal a la
		// retraction hors chaussee (point D) quand elles poussent dans l'herbe.
		float BasalRUnitCm = 0.f;
		FString BasalMethode;
		if (bTree || bHedge)
		{
			BasalRUnitCm = ComputeBasalRadiusCm(Mesh, BasalMethode);
		}
		// Cote de la fosse CARREE d'un arbre : deux contraintes, on garde la plus
		// exigeante — la marge racinaire demandee, et la CONTENANCE de l'interieur du
		// cadre (qui s'echelonne avec la fosse). Sorti de la boucle d'instances au
		// lot6 : la RETRACTION doit connaitre l'emprise de la fosse AVANT de decider ou
		// poser l'arbre, une fosse ne peut pas etre dimensionnee apres coup sur une
		// position qu'elle aurait du commander.
		const float RatioInterieur = (GPitRefM * 50.f - GPitFrameCm) / (GPitRefM * 100.f);
		auto CoteFosseCm = [&](float Scale) -> float
		{
			const float RbCm = BasalRUnitCm * Scale;
			return FMath::Clamp(FMath::Max(2.f * RbCm + GPitRootMarginM * 100.f,
				(RbCm + GPitRootClearCm) / RatioInterieur),
				GPitMinM * 100.f, GPitMaxM * 100.f);
		};
		// Rayon de la fosse RONDE d'un arbuste (lot6 point C) : diametre 0,6 a 0,9 m,
		// proportionnel a la taille REELLE de l'arbuste — une fraction de son etalement
		// basal (voir GShrubPitBasalFrac : l'empattement mesure est celui du buisson
		// entier, la decoupe ne suit que la touffe de tiges).
		auto RayonFosseArbusteCm = [&](float Scale) -> float
		{
			return FMath::Clamp(
				GShrubPitBasalFrac * BasalRUnitCm * Scale + GShrubPitMarginCm,
				GShrubPitMinM * 50.f, GShrubPitMaxM * 50.f);
		};
		// Le REPLI de la mesure d'empattement reste trace (donnees de rendu -> description
		// de maillage -> fraction d'emprise) : un empattement obtenu par repli explique a
		// lui seul une fosse trop grande ou trop petite.
		if (bTree)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("Vegetation : empattement de %s = %.1f cm a l'echelle 1 (%s) — ")
				TEXT("echelle mediane %.2f -> fosse carree ~%.2f m."),
				*Mesh->GetName(), BasalRUnitCm, *BasalMethode, MedScale,
				CoteFosseCm(MedScale) * 0.01f);
		}
		else if (bHedge)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("Vegetation : empattement de %s = %.1f cm a l'echelle 1 (%s) — ")
				TEXT("echelle mediane %.2f -> fosse RONDE ~%.2f m de diametre."),
				*Mesh->GetName(), BasalRUnitCm, *BasalMethode, MedScale,
				2.f * RayonFosseArbusteCm(MedScale) * 0.01f);
		}

		// Mode district : on REPREND l'acteur existant de ce mesh (ses instances hors
		// emprise ont ete conservees, celles de l'emprise viennent d'etre retirees).
		const FString VegLabel = FString::Printf(TEXT("CityVeg_%s"), *Mesh->GetName());
		AActor* VegActor = bCellFilter ? ActeursVeg.FindRef(VegLabel) : nullptr;
		UHierarchicalInstancedStaticMeshComponent* Hism =
			bCellFilter ? HismVeg.FindRef(VegLabel) : nullptr;
		const bool bNouvelActeur = (VegActor == nullptr || Hism == nullptr);
		if (bNouvelActeur)
		{
			VegActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
			// Les instances deja posees ont une collision de canopee : sans cette ligne, les
			// traces des meshes suivants s'epuisent dans les feuillages des arbres precedents
			// (constate au run du 30/07 : 5 670 replis GroundZ au lieu de ~0).
			// (Les acteurs REPRIS, eux, sont deja dans la liste d'ignores : le balayage
			// initial de TraceParams ignore tout « CityVeg ».)
			TraceParams.AddIgnoredActor(VegActor);
			USceneComponent* Root2 = NewObject<USceneComponent>(VegActor, TEXT("Root"), RF_Transactional);
			VegActor->SetRootComponent(Root2);
			VegActor->AddInstanceComponent(Root2);
			Root2->RegisterComponent();
			Root2->SetWorldLocation(Location);
			Hism = NewObject<UHierarchicalInstancedStaticMeshComponent>(VegActor, TEXT("Veg"), RF_Transactional);
			Hism->SetStaticMesh(Mesh);
			// LOT PIE : AVANT RegisterComponent — sinon les corps sont deja crees.
			PoserCollisionVegetation(Hism, Gen, NbVegSansCollision);
			Hism->SetupAttachment(Root2);
			VegActor->AddInstanceComponent(Hism);
			Hism->RegisterComponent();
		}
		// ⭐ LOT VELOCITE (L3) — LA POSE SE FAIT PAR LOT, ET C'EST TOUT LE SUJET.
		// Mesure V6 : 530 991 instances en 331 s, 1 228 632 en 1 762 s — x2,3
		// d'instances pour x5,3 de temps. La cause est dans le moteur, pas chez nous :
		// UHierarchicalInstancedStaticMeshComponent::AddInstance appelle
		// BuildTreeIfOutdated() a CHAQUE appel, donc reconstruit l'arbre spatial du
		// composant a chaque touffe (HierarchicalInstancedStaticMesh.cpp). AddInstances
		// (pluriel) fait le meme travail utile et ne rebatit l'arbre QU'UNE FOIS.
		// On accumule donc les transforms et on pose tout a la fin de la boucle.
		// C'est SANS effet sur la geometrie : la pose est deja le dernier geste de
		// chaque instance (« plus aucun rejet possible passe ce point »), et les traces
		// ignorent de toute facon les acteurs CityVeg — une instance posee n'a jamais
		// influence la pose de la suivante.
		TArray<FTransform> APoser;
		// Mode district : les instances CONSERVEES de cet acteur ouvrent le lot ; le
		// HISM est vide puis repose EN UNE FOIS (conservees + nouvelles).
		if (TArray<FTransform>* Gardees = GardeesParActeur.Find(VegLabel))
		{
			Hism->ClearInstances();
			APoser = MoveTemp(*Gardees);
			GardeesParActeur.Remove(VegLabel);
		}
		APoser.Reserve(APoser.Num() + Pair.Value.Num());
		for (const FVegInst& InstSrc : Pair.Value)
		{
			// Copie MUTABLE : la retraction hors chaussee (lot6) DEPLACE X/Y, et tout ce
			// qui suit — trace du sol, couronne, plan de fosse, pose — doit travailler
			// sur la position RETENUE. Deplacer apres coup laisserait l'instance posee
			// au Z de son ancienne colonne et sa fosse ajustee sur l'ancienne emprise.
			FVegInst Inst = InstSrc;

			// --- LOT6 points A et D : LA PLANTATION NE DOIT PAS MORDRE LA CHAUSSEE ---
			// Le degagement demande est la demi-emprise de LA FOSSE, plus une marge.
			//
			// PERIMETRE VOLONTAIREMENT ETROIT : seules les plantations sur sol MINERAL
			// rendu sont concernees, c'est-a-dire exactement celles qui recoivent une
			// fosse (carree pour un arbre, ronde pour un arbuste depuis le point C) —
			// et c'est aussi, par construction, l'ensemble qui contient toute plantation
			// posee SUR la chaussee, puisque le peint gagne toujours sur l'herbe. Un
			// arbre plante dans une pelouse dont le feuillage, ou meme le disque de
			// litiere, surplombe un peu la rue n'est pas un defaut : c'est un arbre
			// d'alignement normal, et le deplacer serait retoucher un semis valide.
			if (bTree || bHedge)
			{
				const bool bMinRetrait = MaskSampler.IsRenderedMineral(Inst.X, Inst.Y);
				// L'emprise a degager est le rayon CIRCONSCRIT, pas le demi-cote : une
				// fosse carree est alignee sur SA rue, ses coins pointent donc en biais
				// et ce sont eux qui mordaient encore le bitume de quelques centimetres
				// (mesure du premier run : 9 fosses, jusqu'a 9 cm) quand le degagement
				// n'etait demande que sur le demi-cote. Le disque circonscrit garantit
				// qu'aucun coin ne depasse, quelle que soit l'orientation retenue.
				const float EmpriseCm = bTree
					? CoteFosseCm(Inst.Scale) * 0.5f * UE_SQRT_2
					: RayonFosseArbusteCm(Inst.Scale);
				const float RequisCm = bMinRetrait ? EmpriseCm + GRetraitMargeCm : 0.f;
				const double X0 = Inst.X, Y0 = Inst.Y;
				float D0 = 0.f, D1 = 0.f, Course = 0.f;
				const int32 Verdict =
					DegageChaussee(Inst.X, Inst.Y, RequisCm, D0, D1, Course);
				if (Verdict > 0)
				{
					if (bTree) { ++NumRetractArbre; } else { ++NumRetractHaie; }
					SumCourseCm += Course;
					MaxCourseCm = FMath::Max(MaxCourseCm, Course);
					if (Retraits.Num() < 500)
					{
						Retraits.Add(FRetrait{ MeshPath, bTree, X0 / 100.0, Y0 / 100.0,
							Inst.X / 100.0, Inst.Y / 100.0, Course, D0, D1, RequisCm });
					}
				}
				else if (Verdict < 0)
				{
					// Rue etroite bordee des deux cotes : aucune position degagee dans
					// la course maximale. On NE POSE PAS plutot que de laisser un arbre
					// et sa fosse au milieu de la chaussee, et on le COMPTE.
					if (bTree) { ++NumRetractRateArbre; } else { ++NumRetractRateHaie; }
					RetraitImpossible.Add(FSkippedInst{ MeshPath, X0 / 100.0, Y0 / 100.0 });
					continue;
				}
				else
				{
					if (bTree) { ++NumRetractInchangeArbre; }
					else { ++NumRetractInchangeHaie; }
				}
			}

			// Base-a-0, ZERO offset min_Z (pivot Megascans au pied) : le sol EST la
			// surface RENDUE la plus haute de la colonne (proxys films + dalles).
			float SolZ = 0.f;
			bool bFilm = false;
			bool bTraversee = false;
			TArray<FSolHit, TInlineAllocator<16>> Colonne;
			TraceColumnAll(Inst.X, Inst.Y, Colonne, bTraversee);
			const bool bSol = ChoisitSol(Inst.X, Inst.Y, Colonne, SolZ, bFilm);
			// Mesure de l'ANCIEN defaut sur la MEME colonne : l'ancienne regle prenait le
			// hit sol le plus haut TOUS acteurs confondus, donc la collision grossiere des
			// dalles des qu'elle depassait le rendu. L'ecart EST la hauteur de flottement.
			if (bSol)
			{
				int32 IdxAnc = INDEX_NONE;
				for (int32 i = 0; i < Colonne.Num(); ++i)
				{
					if (IdxAnc == INDEX_NONE || Colonne[i].Z > Colonne[IdxAnc].Z) { IdxAnc = i; }
				}
				const float Ecart = Colonne[IdxAnc].Z - SolZ;
				if (Ecart > 30.f)
				{
					++NumFlottant30;
					if (Ecart > 100.f) { ++NumFlottant100; }
					MaxFlottantCm = FMath::Max(MaxFlottantCm, Ecart);
					CoupablesParActeur.FindOrAdd(Colonne[IdxAnc].Acteur) += 1;
					if (Flottants.Num() < 500)
					{
						FString Col;
						for (const FSolHit& H : Colonne)
						{
							Col += FString::Printf(TEXT("%s%s@%.1f%s"), Col.IsEmpty() ? TEXT("") : TEXT(" | "),
								*H.Acteur, H.Z, H.bProxy ? TEXT(" (rendu)") : TEXT(" (collision d'origine)"));
						}
						Flottants.Add(FFlottant{ MeshPath, Inst.X / 100.0, Inst.Y / 100.0,
							Colonne[IdxAnc].Z, SolZ, Colonne[IdxAnc].Acteur, Col });
					}
				}
			}
			if (bSol && bTraversee)
			{
				++NumMultiHitRecup;
			}
			if (bSol && bFilm)
			{
				++NumFilmCentral;
			}
			else if (bSol)
			{
				++NumDalleCentral;
				// Arbre en fosse : le trace central passe par le TROU du film et tape la
				// dalle, alors que l'herbe VISIBLE autour est drapee plus haut. Couronne
				// de 8 traces au rayon de fosse : >= 3 films -> base a la MEDIANE des Z
				// film, bornee par la dalle (max) pour ne jamais enfoncer l'arbre sous un
				// fond de fosse visible plus haut que l'herbe (creux du MNT).
				if (!bClump)
				{
					const float RayonCm =
						FMath::Clamp(CanopyRUnitCm * Inst.Scale, 400.f, 600.f) + 50.f;
					TArray<float> FilmZs;
					for (int32 K = 0; K < 8; ++K)
					{
						const double Ang = 2.0 * PI * K / 8.0;
						float Zc = 0.f;
						bool bF = false;
						bool bT = false;
						if (TraceColumnSol(Inst.X + RayonCm * FMath::Cos(Ang),
							Inst.Y + RayonCm * FMath::Sin(Ang), Zc, bF, bT) && bF)
						{
							FilmZs.Add(Zc);
						}
					}
					if (FilmZs.Num() >= 3)
					{
						FilmZs.Sort();
						const int32 N = FilmZs.Num();
						const float MedianFilm = (N % 2) ? FilmZs[N / 2]
							: 0.5f * (FilmZs[N / 2 - 1] + FilmZs[N / 2]);
						if (MedianFilm > SolZ)
						{
							SolZ = MedianFilm;
							++NumFosseCorrige;
						}
					}
				}
			}
			else
			{
				// PAS DE SOL DANS LA COLONNE -> INSTANCE NON POSEE.
				// L'ancien repli sur Drape.GroundZ etait le DERNIER vestige du modele
				// MNT analytique dans la pose de la vegetation : il fabriquait un sol
				// la ou il n'y en a pas (typiquement les haies semees au-dela du km2
				// de dalle du proto), d'ou des arbres et des haies FLOTTANT dans le
				// vide en bordure. Regle : pas de sol trouve, pas d'instance. Les
				// positions rejetees partent dans un JSON de trace, jamais dans le
				// silence.
				++TraceMiss;
				Skipped.Add(FSkippedInst{ MeshPath, Inst.X / 100.0, Inst.Y / 100.0 });
				continue;
			}
			// LA POSE EST DIFFEREE apres le bloc « fosse » : depuis le lot5, la fosse peut
			// prononcer le REJET de l'instance (arbre sur pente minerale), et une instance
			// rejetee ne doit pas avoir ete ajoutee au HISM entre-temps.

			// FOSSE DE PLANTATION : un ARBRE dont le sol est la dalle ET dont le sol
			// RENDU est mineral pousse a travers un revetement — la voirie y decoupe
			// une fosse. « Rendu » et non « canal R brut » : le peint gagne sur
			// l'herbe et la frontiere d'herbe est bruitee, si bien qu'un arbre de
			// LISIERE se retrouvait sur du pave SANS fosse (defaut signale).
			// v3 : taille PROPORTIONNELLE a l empattement de SON arbre, alignee sur la
			// rue, et posee sur le PLAN LOCAL du sol (pitch/roll) — fond de terre garanti
			// AU-DESSUS de la dalle en tout point de l emprise.
			if (bTree && bSol && !bFilm)
			{
				const bool bRendu = MaskSampler.IsRenderedMineral(Inst.X, Inst.Y);
				const bool bBrut = MaskSampler.IsMineral(Inst.X, Inst.Y);
				if (bRendu && !bBrut)
				{
					++NumFosseGagnee;
				}
				else if (bBrut && !bRendu)
				{
					++NumFossePerdue;
				}
				if (bRendu)
				{
					// --- 1) TAILLE : proportionnelle a l'empattement de CET arbre ---
					// Deux contraintes, on garde la plus exigeante :
					//   - la marge demandee : cote >= 2 x rayon + 30 cm ;
					//   - la CONTENANCE : l'empattement doit tenir dans l'INTERIEUR du
					//     cadre, dont le demi-cote vaut RatioInterieur x cote (le cadre
					//     s'echelonne avec la fosse, il n'est pas de largeur fixe).
					// RatioInterieur et le calcul du cote sont remontes avant la boucle
					// d'instances (lambda CoteFosseCm) : la retraction du lot6 a besoin
					// de l'emprise de la fosse AVANT de choisir la position. Meme
					// formule, meme resultat.
					const float RbCm = BasalRUnitCm * Inst.Scale;
					const float CoteVoulu = FMath::Max(
						2.f * RbCm + GPitRootMarginM * 100.f,
						(RbCm + GPitRootClearCm) / RatioInterieur);
					const float CoteCm = CoteFosseCm(Inst.Scale);
					if (CoteVoulu < GPitMinM * 100.f) { ++NumPitClampMin; }
					else if (CoteVoulu > GPitMaxM * 100.f) { ++NumPitClampMax; }
					SumPitCoteCm += CoteCm;

					// --- 2) POSE : sur le PLAN LOCAL du sol -------------------------
					// La dalle rendue est grossiere (~7,8 m par triangle) : un carre
					// horizontal cale sur le seul Z du centre plonge ses coins bas dans
					// le pave et fait flotter les autres. On echantillonne l'emprise en
					// 9 traces (centre, coins, milieux de cote), on ajuste un plan par
					// moindres carres, et on cale la fosse sur le point le PLUS HAUT.
					const float YawDeg = YawFosse(Inst.X, Inst.Y);
					const float HalfCm = CoteCm * 0.5f;
					const float CosY = FMath::Cos(FMath::DegreesToRadians(YawDeg));
					const float SinY = FMath::Sin(FMath::DegreesToRadians(YawDeg));
					static const float PU[9] = { 0.f, -1.f, 1.f, 1.f, -1.f, 0.f, 1.f, 0.f, -1.f };
					static const float PV[9] = { 0.f, -1.f, -1.f, 1.f, 1.f, -1.f, 0.f, 1.f, 0.f };
					float Us[9], Vs[9], Zs[9];
					int32 Ks[9];
					int32 NPts = 0;
					for (int32 K = 0; K < 9; ++K)
					{
						const float Lu = PU[K] * HalfCm;
						const float Lv = PV[K] * HalfCm;
						float Zc = 0.f;
						bool bFc = false, bTc = false;
						if (TraceColumnSol(Inst.X + Lu * CosY - Lv * SinY,
							Inst.Y + Lu * SinY + Lv * CosY, Zc, bFc, bTc))
						{
							Us[NPts] = Lu;
							Vs[NPts] = Lv;
							Zs[NPts] = Zc;
							Ks[NPts] = K;
							++NPts;
						}
					}
					// 4 sondages de plus AU CONTOUR DE LA v2 (1,2 m fixe) : sans eux, la
					// regle v2 serait jugee sur une emprise qui n'est pas la sienne.
					float Z2Coin[4];
					int32 N2Coins = 0;
					for (int32 K = 1; K <= 4; ++K)
					{
						const float Lu = PU[K] * GV2HalfCm;
						const float Lv = PV[K] * GV2HalfCm;
						float Zc = 0.f;
						bool bFc = false, bTc = false;
						if (TraceColumnSol(Inst.X + Lu * CosY - Lv * SinY,
							Inst.Y + Lu * SinY + Lv * CosY, Zc, bFc, bTc))
						{
							Z2Coin[N2Coins++] = Zc;
						}
					}
					// Plan z = A.u + B.v + C dans le repere de la fosse. Les 9 points
					// sont symetriques, mais une trace manquante casse la symetrie :
					// on centre explicitement avant de resoudre. DEUX passes : la
					// seconde ignore les points qui n'appartiennent visiblement pas a
					// la meme surface (marche, mur, dalle superposee).
					float A = 0.f, B = 0.f, C = SolZ;
					// Pente BRUTE du plan local, avant l'ecretage a GPitMaxSlope : c'est
					// elle qui juge la pente reelle du terrain (point B). L'ecretee, elle,
					// ne sert qu'a poser la fosse sans la mettre de travers.
					float ARaw = 0.f, BRaw = 0.f;
					bool bGarde[9];
					for (int32 K = 0; K < 9; ++K) { bGarde[K] = true; }
					int32 NGarde = NPts;
					auto Ajuste = [&]() -> bool
					{
						if (NGarde < 4) { return false; }
						float Mu = 0.f, Mv = 0.f, Mz = 0.f;
						for (int32 K = 0; K < NPts; ++K)
						{
							if (!bGarde[K]) { continue; }
							Mu += Us[K]; Mv += Vs[K]; Mz += Zs[K];
						}
						Mu /= NGarde; Mv /= NGarde; Mz /= NGarde;
						float Su2 = 0.f, Sv2 = 0.f, Suz = 0.f, Svz = 0.f;
						for (int32 K = 0; K < NPts; ++K)
						{
							if (!bGarde[K]) { continue; }
							const float Du = Us[K] - Mu, Dv = Vs[K] - Mv, Dz = Zs[K] - Mz;
							Su2 += Du * Du; Sv2 += Dv * Dv;
							Suz += Du * Dz; Svz += Dv * Dz;
						}
						A = Su2 > 1.f ? Suz / Su2 : 0.f;
						B = Sv2 > 1.f ? Svz / Sv2 : 0.f;
						ARaw = A; BRaw = B;
						A = FMath::Clamp(A, -GPitMaxSlope, GPitMaxSlope);
						B = FMath::Clamp(B, -GPitMaxSlope, GPitMaxSlope);
						C = Mz - A * Mu - B * Mv;
						return true;
					};
					if (Ajuste())
					{
						// Purge des points d'une AUTRE surface, puis rajustement.
						int32 NRejet = 0;
						for (int32 K = 0; K < NPts; ++K)
						{
							if (FMath::Abs(Zs[K] - (A * Us[K] + B * Vs[K] + C))
								> GPitOutlierCm)
							{
								bGarde[K] = false;
								++NRejet;
							}
						}
						if (NRejet > 0 && NPts - NRejet >= 4)
						{
							NGarde = NPts - NRejet;
							NumPitOutliers += NRejet;
							Ajuste();
						}
						else
						{
							for (int32 K = 0; K < NPts; ++K) { bGarde[K] = true; }
							NGarde = NPts;
						}
						++NumPitPlan;
					}
					else
					{
						++NumPitPlat;   // emprise mal echantillonnee : horizontale
					}

					// --- POINT B : ARBRE SUR PENTE MINERALE -> INSTANCE SUPPRIMEE ---
					// On est ici sur un arbre pose sur du sol MINERAL rendu (bRendu) : la
					// pente du plan local dit si ce mineral est un talus. Au-dela du
					// seuil, ni arbre ni fosse : un tronc plante dans un talus pave et
					// un cadre carre couche sur la pente sont deux incoherences que
					// l'utilisateur a demande de trancher par la suppression. Le rejet
					// est trace (compteur + positions) comme tout rejet de cette passe.
					{
						const float PenteDeg = FMath::RadiansToDegrees(
							FMath::Atan(FMath::Sqrt(ARaw * ARaw + BRaw * BRaw)));
						if (PenteDeg > GPenteMaxArbreMineralDeg)
						{
							SkippedPente.Add(FSkippedInst{ MeshPath,
								Inst.X / 100.0, Inst.Y / 100.0 });
							SkippedPenteDeg.Add(PenteDeg);
							continue;   // ni instance, ni fosse
						}
					}

					// Residu MAXIMAL sur les points RETENUS : c'est lui qui garantit
					// qu'aucun point de la rue ne depasse le plan de la fosse. Borne,
					// pour qu'un relief non filtre ne mette jamais la fosse en l'air.
					float MaxRes = 0.f, MinRes = 0.f;
					for (int32 K = 0; K < NPts; ++K)
					{
						if (!bGarde[K]) { continue; }
						const float R = Zs[K] - (A * Us[K] + B * Vs[K] + C);
						MaxRes = FMath::Max(MaxRes, R);
						MinRes = FMath::Min(MinRes, R);
					}
					MaxPitGiteCm = FMath::Max(MaxPitGiteCm, MaxRes - MinRes);
					if (MaxRes > GPitMaxLiftCm)
					{
						++NumPitLiftBorne;
						MaxRes = GPitMaxLiftCm;
					}

					// Rotation : l'axe Z local devient la normale du plan local. Le lacet
					// de rue est conserve comme direction d'avant (MakeFromZX projette).
					const FVector NLoc(-A, -B, 1.0);
					const FVector NW(NLoc.X * CosY - NLoc.Y * SinY,
						NLoc.X * SinY + NLoc.Y * CosY, 1.0);
					const FRotator PitRot = FRotationMatrix::MakeFromZX(
						NW.GetSafeNormal(), FVector(CosY, SinY, 0.0)).Rotator();
					const float ZFrame = C + MaxRes + GPitLiftCm;
					Pits.Add(FPitInst{ Inst.X, Inst.Y, ZFrame,
						PitRot, CoteCm / (GPitRefM * 100.f) });

					// --- 3) COMPARATIF v2 / v3 sur les memes sondages ----------------
					if (NPts == 9 && N2Coins == 4)
					{
						++NumCmpPits;
						// v2 : tout est plat, cale sur le seul Z du centre.
						const float Z2Sol = SolZ + GV2LiftCm - GV2SinkCm;   // -1,5 cm
						if (Z2Sol < Zs[0]) { ++N2Fond; }
						for (int32 K = 0; K < NPts; ++K)
						{
							++NumCmpPts;
							if (Z2Sol < Zs[K]) { ++N2PtsOcc; }
							// v3 : le fond suit le plan de la fosse.
							const float Z3Sol = ZFrame + A * Us[K] + B * Vs[K] - GPitSinkCm;
							if (Z3Sol < Zs[K]) { ++N3PtsOcc; }
							if (K == 0 && Z3Sol < Zs[K]) { ++N3Fond; }
						}
						bool b2Coin = false;
						for (int32 K = 0; K < N2Coins; ++K)
						{
							if (SolZ + GV2LiftCm < Z2Coin[K]) { b2Coin = true; }
						}
						if (b2Coin) { ++N2Coin; }
						bool b3Coin = false;
						for (int32 K = 0; K < NPts; ++K)
						{
							if (Ks[K] < 1 || Ks[K] > 4) { continue; }
							if (ZFrame + A * Us[K] + B * Vs[K] < Zs[K]) { b3Coin = true; }
						}
						if (b3Coin) { ++N3Coin; }
						// Debordement : empattement vs demi-cote INTERIEUR du cadre.
						if (RbCm > GV2InnerCm) { ++N2Debord; }
						if (RbCm > RatioInterieur * CoteCm) { ++N3Debord; }
					}
				}
			}

			// --- LOT6 point C : FOSSE RONDE DE L'ARBUSTE SUR SOL MINERAL -------------
			// Verdict utilisateur : les haies posees sur du pave n'ont ni socle ni
			// racines — des tiges fines qui sortent de la pierre. La voirie decoupe la
			// aussi, mais en ROND et en petit (les fosses d'arbustes reelles le sont),
			// avec un fin anneau et de la terre en monticule contre la tige.
			// Les haies dans l'HERBE n'en ont pas : il n'y a rien a decouper.
			//
			// La mecanique est celle de la fosse carree — plan local echantillonne,
			// purge des sondages tombes sur une autre surface, relevement borne, fond
			// garanti au-dessus de la dalle. Elle est ECRITE A PART et non factorisee :
			// la fosse carree est validee par l'utilisateur et instrumentee par son
			// comparatif v2/v3, on ne la refactorise pas dans le meme lot que l'ajout.
			// Les sondages sont pris dans les axes du MONDE (un disque n'a pas de lacet
			// a respecter), si bien que le plan ajuste donne directement la normale.
			if (bHedge && bSol && !bFilm &&
				MaskSampler.IsRenderedMineral(Inst.X, Inst.Y))
			{
				const float RCm = RayonFosseArbusteCm(Inst.Scale);
				float Us[9], Vs[9], Zs[9];
				int32 NPts = 0;
				for (int32 K = 0; K < 9; ++K)
				{
					float Lu = 0.f, Lv = 0.f;
					if (K > 0)
					{
						const double Ang = 2.0 * PI * (K - 1) / 8.0;
						Lu = RCm * (float)FMath::Cos(Ang);
						Lv = RCm * (float)FMath::Sin(Ang);
					}
					float Zc = 0.f;
					bool bFc = false, bTc = false;
					if (TraceColumnSol(Inst.X + Lu, Inst.Y + Lv, Zc, bFc, bTc))
					{
						Us[NPts] = Lu;
						Vs[NPts] = Lv;
						Zs[NPts] = Zc;
						++NPts;
					}
				}
				float A = 0.f, B = 0.f, C = SolZ;
				float ARaw = 0.f, BRaw = 0.f;   // pente BRUTE, avant ecretage
				bool bGarde[9];
				for (int32 K = 0; K < 9; ++K) { bGarde[K] = true; }
				int32 NGarde = NPts;
				auto AjusteRond = [&]() -> bool
				{
					if (NGarde < 4) { return false; }
					float Mu = 0.f, Mv = 0.f, Mz = 0.f;
					for (int32 K = 0; K < NPts; ++K)
					{
						if (!bGarde[K]) { continue; }
						Mu += Us[K]; Mv += Vs[K]; Mz += Zs[K];
					}
					Mu /= NGarde; Mv /= NGarde; Mz /= NGarde;
					float Su2 = 0.f, Sv2 = 0.f, Suz = 0.f, Svz = 0.f;
					for (int32 K = 0; K < NPts; ++K)
					{
						if (!bGarde[K]) { continue; }
						const float Du = Us[K] - Mu, Dv = Vs[K] - Mv, Dz = Zs[K] - Mz;
						Su2 += Du * Du; Sv2 += Dv * Dv;
						Suz += Du * Dz; Svz += Dv * Dz;
					}
					A = Su2 > 1.f ? Suz / Su2 : 0.f;
					B = Sv2 > 1.f ? Svz / Sv2 : 0.f;
					ARaw = A; BRaw = B;
					A = FMath::Clamp(A, -GPitMaxSlope, GPitMaxSlope);
					B = FMath::Clamp(B, -GPitMaxSlope, GPitMaxSlope);
					C = Mz - A * Mu - B * Mv;
					return true;
				};
				if (AjusteRond())
				{
					int32 NRejet = 0;
					for (int32 K = 0; K < NPts; ++K)
					{
						if (FMath::Abs(Zs[K] - (A * Us[K] + B * Vs[K] + C)) > GPitOutlierCm)
						{
							bGarde[K] = false;
							++NRejet;
						}
					}
					if (NRejet > 0 && NPts - NRejet >= 4)
					{
						NGarde = NPts - NRejet;
						AjusteRond();
					}
					else
					{
						for (int32 K = 0; K < NPts; ++K) { bGarde[K] = true; }
						NGarde = NPts;
					}
					++NumShrubPitPlan;
				}
				else
				{
					++NumShrubPitPlat;
				}
				// PENTE : une fosse est une decoupe HORIZONTALE dans un revetement.
				// MESURE du premier run : 113 des 268 disques sortaient couches a
				// 20-27 deg, plantes dans les talus paves — exactement le faux des cadres
				// carres couches, tranche au lot5. Ici l'ARBUSTE RESTE (un buisson sur
				// talus est credible, c'est la regle du lot5) : seule la fosse tombe.
				// Le juge est la pente BRUTE, avant l'ecretage qui sert a poser.
				const float PenteRondeDeg = FMath::RadiansToDegrees(
					FMath::Atan(FMath::Sqrt(ARaw * ARaw + BRaw * BRaw)));
				if (PenteRondeDeg > GShrubPitMaxSlopeDeg)
				{
					++NumShrubPitPente;
				}
				else
				{
					float MaxRes = 0.f;
					for (int32 K = 0; K < NPts; ++K)
					{
						if (!bGarde[K]) { continue; }
						MaxRes = FMath::Max(MaxRes, Zs[K] - (A * Us[K] + B * Vs[K] + C));
					}
					if (MaxRes > GPitMaxLiftCm)
					{
						++NumShrubLiftBorne;
						MaxRes = GPitMaxLiftCm;
					}
					const float YawR = FMath::DegreesToRadians(Inst.Yaw);
					const FRotator ShrubRot = FRotationMatrix::MakeFromZX(
						FVector(-A, -B, 1.0).GetSafeNormal(),
						FVector(FMath::Cos(YawR), FMath::Sin(YawR), 0.0)).Rotator();
					SumShrubRayonCm += RCm;
					ShrubPits.Add(FPitInst{ Inst.X, Inst.Y, C + MaxRes + GPitLiftCm,
						ShrubRot, RCm / (GShrubPitRefM * 50.f) });
				}
			}

			// POSE EFFECTIVE : plus aucun rejet possible passe ce point.
			APoser.Add(FTransform(FRotator(0, Inst.Yaw, 0),
				FVector(Inst.X, Inst.Y, SolZ), FVector(Inst.Scale)));
			++Summary.Instances;
		}
		if (APoser.Num() > 0)
		{
			Hism->AddInstances(APoser, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
		}
		// Touffes d'herbe : reglages de RENDU poses A LA CREATION, dans l'unique
		// autorite de pose (V6 : plus aucun fondu — cf. GClumpCullStartCm ; toujours
		// pas d'ombre portee, reglage utilisateur d'origine). L'ecriture est
		// INCONDITIONNELLE : elle efface aussi le cull d'une generation anterieure.
		if (bClump)
		{
			Hism->InstanceStartCullDistance = GClumpCullStartCm;
			Hism->InstanceEndCullDistance = GClumpCullEndCm;
			Hism->SetCastShadow(false);
		}
		if (bNouvelActeur)
		{
			VegActor->SetActorLabel(VegLabel);
			++Summary.Actors;
		}
		++Summary.Meshes;
	}

	// --- FOSSES DE PLANTATION : un mesh, un HISM, toutes les fosses --------------
	if (Pits.Num() > 0)
	{
		// Deux matieres : la TERRE (pack dedie treepit_soil, seul materiau du set a
		// lire la VertexColor) pour l'interieur, la BORDURE pour le cadre. Repli sur
		// le gravier puis sur la terre elle-meme : jamais de materiau /Engine/.
		FSurfaceLibrary SurfLib;
		SurfLib.Init(true, Gen.SurfacesFolder);
		const FResolvedSurface* Soil = SurfLib.Resolve(&GSurfPitSoil);
		UMaterialInterface* PitMat = Soil ? Soil->Material : nullptr;
		if (!PitMat)
		{
			const FResolvedSurface* Repli = SurfLib.Resolve(&GSurfGravel);
			PitMat = Repli ? Repli->Material : nullptr;
			UE_LOG(LogCityImport, Display,
				TEXT("Vegetation : pack de terre '%s' absent — repli sur '%s'."),
				GSurfPitSoil.Slug, GSurfGravel.Slug);
		}
		const FResolvedSurface* Frame = SurfLib.Resolve(&GSurfPitFrame);
		UMaterialInterface* FrameMat = Frame ? Frame->Material : nullptr;
		if (!FrameMat)
		{
			const FResolvedSurface* Curb = SurfLib.Resolve(&GSurfCurb);
			FrameMat = Curb && Curb->Material ? Curb->Material : PitMat;
		}
		if (!PitMat)
		{
			// Pas de materiau terre = PAS de fosses. Des carres en materiau par
			// defaut (gris vif) seraient un defaut visible, pire que l'absence.
			UE_LOG(LogCityImport, Warning,
				TEXT("Vegetation : materiau de fosse absent (%s / %s) — %d fosses non generees."),
				GSurfPitSoil.Slug, GSurfGravel.Slug, Pits.Num());
			Pits.Reset();
		}

		FCityMeshBuilder PitBuilder;
		PitBuilder.bLinearColors = Gen.bDesktop;
		BuildPlantingPit(PitBuilder, GPitRefM * 50.f /*demi-cote cm*/);
		// Pas de collision : une fosse est une decoration de sol, elle ne doit ni
		// gener le drone ni polluer les traces de la prochaine passe.
		// Slot 0 (Wall) = la terre, slot 1 (Glass) = le cadre : c'est le decoupage en
		// groupes de polygones de BuildPlantingPit.
		UStaticMesh* PitMesh = PitMat
			? CreateMeshAsset(AssetFolder / TEXT("SM_TreePit"), PitBuilder,
				PitMat, FrameMat, /*bWithCollision=*/false)
			: nullptr;
		if (PitMesh)
		{
			for (const FStaticMaterial& SM : PitMesh->GetStaticMaterials())
			{
				if (SM.MaterialInterface)
				{
					SM.MaterialInterface->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
				}
			}
			// Mode district : meme regle que les meshes de vegetation — l'acteur des
			// fosses est REPRIS, ses fosses hors emprise sont deja conservees.
			const FString PitLabel(TEXT("CityVeg_TreePits"));
			AActor* PitActor = bCellFilter ? ActeursVeg.FindRef(PitLabel) : nullptr;
			UHierarchicalInstancedStaticMeshComponent* PitHism =
				bCellFilter ? HismVeg.FindRef(PitLabel) : nullptr;
			const bool bNouvellesFosses = (PitActor == nullptr || PitHism == nullptr);
			if (bNouvellesFosses)
			{
				PitActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
				USceneComponent* PitRoot =
					NewObject<USceneComponent>(PitActor, TEXT("Root"), RF_Transactional);
				PitActor->SetRootComponent(PitRoot);
				PitActor->AddInstanceComponent(PitRoot);
				PitRoot->RegisterComponent();
				PitRoot->SetWorldLocation(Location);
				PitHism = NewObject<UHierarchicalInstancedStaticMeshComponent>(PitActor, TEXT("Pits"),
					RF_Transactional);
				PitHism->SetStaticMesh(PitMesh);
				// LOT PIE : AVANT RegisterComponent — sinon les corps sont deja crees.
				PoserCollisionVegetation(PitHism, Gen, NbVegSansCollision);
				PitHism->SetupAttachment(PitRoot);
				PitActor->AddInstanceComponent(PitHism);
				PitHism->RegisterComponent();
			}
			else if (PitHism->GetStaticMesh() != PitMesh)
			{
				PitHism->SetStaticMesh(PitMesh);
			}
			PitHism->SetCastShadow(false);   // un disque plat au sol n'ombre rien
			// LOT VELOCITE (L3) : un seul AddInstances.
			// Echelle (S,S,1) : le carre grandit, mais le relief VERTICAL du cadre
			// (hauteur, creux, jupe) reste absolu — une grande fosse n'a pas un cadre
			// deux fois plus haut.
			TArray<FTransform> PitXf;
			if (TArray<FTransform>* Gardees = GardeesParActeur.Find(PitLabel))
			{
				PitHism->ClearInstances();
				PitXf = MoveTemp(*Gardees);
				GardeesParActeur.Remove(PitLabel);
			}
			PitXf.Reserve(PitXf.Num() + Pits.Num());
			for (const FPitInst& P : Pits)
			{
				PitXf.Add(FTransform(P.Rot,
					FVector(P.Xcm, P.Ycm, P.Zcm), FVector(P.Scale, P.Scale, 1.f)));
			}
			PitHism->AddInstances(PitXf, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
			// Le prefixe CityVeg est VOULU : la passe vege-seule suivante detruit ces
			// acteurs comme les autres (idempotence), et le trace les ignore.
			Summary.Pits = Pits.Num();
			if (bNouvellesFosses)
			{
				PitActor->SetActorLabel(PitLabel);
				++Summary.Actors;
			}
			++Summary.Meshes;
		}
	}

	// --- LOT6 point C : FOSSES RONDES DES ARBUSTES — un mesh, un HISM -------------
	// Meme matieres que la fosse carree (terre treepit_soil qui lit la vertex color,
	// cadre treepit_frame) : c'est le meme objet de voirie, a une autre echelle.
	// Meme regle sans appel : pas de materiau de terre, pas de fosses — des disques en
	// materiau par defaut seraient un defaut visible, pire que l'absence.
	int32 NumShrubPitsPoses = 0;
	if (ShrubPits.Num() > 0)
	{
		FSurfaceLibrary SurfLib;
		SurfLib.Init(true, Gen.SurfacesFolder);
		const FResolvedSurface* Soil = SurfLib.Resolve(&GSurfPitSoil);
		UMaterialInterface* PitMat = Soil ? Soil->Material : nullptr;
		if (!PitMat)
		{
			const FResolvedSurface* Repli = SurfLib.Resolve(&GSurfGravel);
			PitMat = Repli ? Repli->Material : nullptr;
		}
		const FResolvedSurface* Frame = SurfLib.Resolve(&GSurfPitFrame);
		UMaterialInterface* FrameMat = Frame ? Frame->Material : nullptr;
		if (!FrameMat)
		{
			const FResolvedSurface* Curb = SurfLib.Resolve(&GSurfCurb);
			FrameMat = Curb && Curb->Material ? Curb->Material : PitMat;
		}
		if (!PitMat)
		{
			UE_LOG(LogCityImport, Warning,
				TEXT("Vegetation : materiau de fosse absent — %d fosses rondes non generees."),
				ShrubPits.Num());
			ShrubPits.Reset();
		}
		else
		{
			FCityMeshBuilder ShrubBuilder;
			ShrubBuilder.bLinearColors = Gen.bDesktop;
			BuildRoundPit(ShrubBuilder, GShrubPitRefM * 50.f /*rayon de reference cm*/);
			UStaticMesh* ShrubMesh = CreateMeshAsset(AssetFolder / TEXT("SM_ShrubPit"),
				ShrubBuilder, PitMat, FrameMat, /*bWithCollision=*/false);
			if (ShrubMesh)
			{
				for (const FStaticMaterial& SM : ShrubMesh->GetStaticMaterials())
				{
					if (SM.MaterialInterface)
					{
						SM.MaterialInterface->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
					}
				}
				// Mode district : acteur REPRIS s'il existe (cf. fosses carrees).
				const FString SPLabel(TEXT("CityVeg_ShrubPits"));
				AActor* SPActor = bCellFilter ? ActeursVeg.FindRef(SPLabel) : nullptr;
				UHierarchicalInstancedStaticMeshComponent* SPHism =
					bCellFilter ? HismVeg.FindRef(SPLabel) : nullptr;
				const bool bNouvellesRondes = (SPActor == nullptr || SPHism == nullptr);
				if (bNouvellesRondes)
				{
					SPActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
					USceneComponent* SPRoot =
						NewObject<USceneComponent>(SPActor, TEXT("Root"), RF_Transactional);
					SPActor->SetRootComponent(SPRoot);
					SPActor->AddInstanceComponent(SPRoot);
					SPRoot->RegisterComponent();
					SPRoot->SetWorldLocation(Location);
					SPHism = NewObject<UHierarchicalInstancedStaticMeshComponent>(SPActor,
						TEXT("ShrubPits"), RF_Transactional);
					SPHism->SetStaticMesh(ShrubMesh);
					// LOT PIE : AVANT RegisterComponent — sinon les corps sont deja crees.
					PoserCollisionVegetation(SPHism, Gen, NbVegSansCollision);
					SPHism->SetupAttachment(SPRoot);
					SPActor->AddInstanceComponent(SPHism);
					SPHism->RegisterComponent();
				}
				else if (SPHism->GetStaticMesh() != ShrubMesh)
				{
					SPHism->SetStaticMesh(ShrubMesh);
				}
				SPHism->SetCastShadow(false);
				// LOT VELOCITE (L3) : un seul AddInstances.
				// Echelle (S,S,1) : le disque grandit, le relief VERTICAL (anneau,
				// monticule, jupe) reste absolu.
				TArray<FTransform> SPXf;
				if (TArray<FTransform>* Gardees = GardeesParActeur.Find(SPLabel))
				{
					SPHism->ClearInstances();
					SPXf = MoveTemp(*Gardees);
					GardeesParActeur.Remove(SPLabel);
				}
				SPXf.Reserve(SPXf.Num() + ShrubPits.Num());
				for (const FPitInst& P : ShrubPits)
				{
					SPXf.Add(FTransform(P.Rot,
						FVector(P.Xcm, P.Ycm, P.Zcm), FVector(P.Scale, P.Scale, 1.f)));
				}
				SPHism->AddInstances(SPXf, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
				// Prefixe CityVeg VOULU : detruit comme les autres au prochain re-import
				// (idempotence) et ignore par le trace.
				NumShrubPitsPoses = ShrubPits.Num();
				if (bNouvellesRondes)
				{
					SPActor->SetActorLabel(SPLabel);
					++Summary.Actors;
				}
				++Summary.Meshes;
			}
		}
	}

	// Mode district : les acteurs qui ONT PERDU des instances dans l'emprise mais n'en
	// recoivent AUCUNE nouvelle (une essence qui a disparu du quartier). Ils n'ont pas
	// ete vides plus haut — on le fait ici, sinon leurs anciennes instances resteraient
	// dans l'emprise qu'on vient de re-semer.
	int32 NumActeursVides = 0;
	for (TPair<FString, TArray<FTransform>>& P : GardeesParActeur)
	{
		UHierarchicalInstancedStaticMeshComponent* H = HismVeg.FindRef(P.Key);
		if (!H)
		{
			continue;
		}
		H->ClearInstances();
		if (P.Value.Num() > 0)
		{
			H->AddInstances(P.Value, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
		}
		++NumActeursVides;
	}
	GardeesParActeur.Reset();

	// Cleanup des proxys de trace : acteurs detruits, duplicatas transient au GC.
	// Les films/dalles d'ORIGINE n'ont jamais ete touches (rien de dirty).
	for (AStaticMeshActor* P : TraceProxies)
	{
		World->DestroyActor(P);
	}

	// --- TRACE DES INSTANCES REJETEES -------------------------------------------
	// Le repli GroundZ ayant disparu, un « sol introuvable » se solde par une
	// instance en moins : elle doit rester VERIFIABLE, pas silencieuse.
	Summary.Skipped = Skipped.Num();
	{
		const FString SkipPath = VegJsonPath + TEXT(".skipped.json");
		FString Out = FString::Printf(
			TEXT("{\"skipped\":%d,\"raison\":\"aucun sol (SM_Surface_/SM_Slab_) dans la colonne")
			TEXT(" — instance NON posee, plus de repli GroundZ\",\"positions\":["), Skipped.Num());
		for (int32 i = 0; i < Skipped.Num(); ++i)
		{
			Out += FString::Printf(TEXT("%s{\"mesh\":\"%s\",\"x\":%.2f,\"y\":%.2f}"),
				i ? TEXT(",") : TEXT(""), *Skipped[i].Mesh, Skipped[i].Xm, Skipped[i].Ym);
		}
		Out += TEXT("]}");
		// UTF-8 FORCE : par defaut FFileHelper bascule tout le fichier en UTF-16 des
		// qu'un caractere sort de l'ASCII (ici le tiret cadratin du message), ce qui
		// donnait un JSON illisible pour les outils qui le relisent.
		FFileHelper::SaveStringToFile(Out, *SkipPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// --- TRACE DES ARBRES SUPPRIMES POUR PENTE MINERALE (point B) ----------------
	{
		const FString P = VegJsonPath + TEXT(".slope_skipped.json");
		float MaxDeg = 0.f;
		double SomDeg = 0.0;
		for (float D : SkippedPenteDeg) { MaxDeg = FMath::Max(MaxDeg, D); SomDeg += D; }
		FString Out = FString::Printf(
			TEXT("{\"supprimes\":%d,\"seuil_deg\":%.1f,\"pente_moyenne_deg\":%.1f,")
			TEXT("\"pente_max_deg\":%.1f,\"raison\":\"arbre sur sol MINERAL rendu dont la ")
			TEXT("pente locale depasse le seuil : arbre ET fosse supprimes\",\"positions\":["),
			SkippedPente.Num(), GPenteMaxArbreMineralDeg,
			SkippedPente.Num() ? (float)(SomDeg / SkippedPente.Num()) : 0.f, MaxDeg);
		for (int32 i = 0; i < SkippedPente.Num(); ++i)
		{
			Out += FString::Printf(TEXT("%s{\"mesh\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"pente_deg\":%.1f}"),
				i ? TEXT(",") : TEXT(""), *SkippedPente[i].Mesh,
				SkippedPente[i].Xm, SkippedPente[i].Ym, SkippedPenteDeg[i]);
		}
		Out += TEXT("]}");
		FFileHelper::SaveStringToFile(Out, *P, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// --- MESURE DU DEFAUT « FLOTTANT » CORRIGE (point A) -------------------------
	// Chaque ligne dit, pour une instance : ou l'ANCIENNE regle l'aurait posee, ou la
	// NOUVELLE la pose, et la colonne complete (chaque surface avec son Z et sa nature)
	// — la preuve est dans le fichier, pas dans le commentaire.
	{
		const FString P = VegJsonPath + TEXT(".floating.json");
		FString Coupables;
		for (const TPair<FString, int32>& Pr : CoupablesParActeur)
		{
			Coupables += FString::Printf(TEXT("%s\"%s\":%d"),
				Coupables.IsEmpty() ? TEXT("") : TEXT(","), *Pr.Key, Pr.Value);
		}
		FString Out = FString::Printf(
			TEXT("{\"instances_corrigees_sup_30cm\":%d,\"dont_sup_1m\":%d,\"flottement_max_cm\":%.1f,")
			TEXT("\"hits_isoles_ecartes\":%d,\"hits_isoles_gardes\":%d,\"sans_proxy\":%d,")
			TEXT("\"regle\":\"pose sur la surface RENDUE (proxys) la plus haute ; la collision ")
			TEXT("d'origine des dalles (SM_Slab_*_Col, grille 16x16) ne decide plus\",")
			TEXT("\"coupables\":{%s},\"exemples\":["),
			NumFlottant30, NumFlottant100, MaxFlottantCm,
			NumHitIsoleEcarte, NumHitIsoleGarde, NumSansProxy, *Coupables);
		for (int32 i = 0; i < Flottants.Num(); ++i)
		{
			const FFlottant& F = Flottants[i];
			Out += FString::Printf(
				TEXT("%s{\"mesh\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"z_ancienne_regle\":%.1f,")
				TEXT("\"z_nouvelle_regle\":%.1f,\"flottement_cm\":%.1f,\"coupable\":\"%s\",")
				TEXT("\"colonne\":\"%s\"}"),
				i ? TEXT(",") : TEXT(""), *F.Mesh, F.Xm, F.Ym, F.ZAncien, F.ZNouveau,
				F.ZAncien - F.ZNouveau, *F.Coupable, *F.Colonne);
		}
		Out += TEXT("]}");
		FFileHelper::SaveStringToFile(Out, *P, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// --- LOT6 : RETRACTIONS ET FOSSES RONDES, CHIFFREES ET TRACEES ---------------
	// Tout ce que ce lot a deplace ou ajoute tient dans ce fichier : position de
	// depart, position retenue, course, distances a la chaussee avant/apres et
	// degagement demande. Un deplacement qui n'y figure pas n'a pas eu lieu.
	{
		const FString P = VegJsonPath + TEXT(".lot6.json");
		FString Out = FString::Printf(
			TEXT("{\"retraction\":{\"regle\":\"une plantation dont l'emprise (fosse ou ")
			TEXT("pied) mord la chaussee peinte est RECULEE le long du gradient du canal G ")
			TEXT("du masque jusqu'a un degagement de demi-emprise + %.0f cm ; course bornee ")
			TEXT("a %.0f cm ; sans position degagee l'instance n'est PAS posee\",")
			TEXT("\"arbres_retractes\":%d,\"haies_retractees\":%d,")
			TEXT("\"arbres_non_poses\":%d,\"haies_non_posees\":%d,")
			TEXT("\"arbres_inchanges\":%d,\"haies_inchangees\":%d,")
			TEXT("\"par_gradient\":%d,\"par_balayage\":%d,")
			TEXT("\"course_moyenne_cm\":%.1f,\"course_max_cm\":%.1f,\"deplacements\":["),
			GRetraitMargeCm, GRetraitMaxCm,
			NumRetractArbre, NumRetractHaie, NumRetractRateArbre, NumRetractRateHaie,
			NumRetractInchangeArbre, NumRetractInchangeHaie,
			NumRetractParGradient, NumRetractParBalayage,
			(NumRetractArbre + NumRetractHaie)
				? (float)(SumCourseCm / (NumRetractArbre + NumRetractHaie)) : 0.f,
			MaxCourseCm);
		for (int32 i = 0; i < Retraits.Num(); ++i)
		{
			const FRetrait& R = Retraits[i];
			Out += FString::Printf(
				TEXT("%s{\"mesh\":\"%s\",\"type\":\"%s\",\"x0\":%.2f,\"y0\":%.2f,")
				TEXT("\"x1\":%.2f,\"y1\":%.2f,\"course_cm\":%.1f,\"d_chaussee_avant_cm\":%.1f,")
				TEXT("\"d_chaussee_apres_cm\":%.1f,\"degagement_requis_cm\":%.1f}"),
				i ? TEXT(",") : TEXT(""), *R.Mesh, R.bArbre ? TEXT("arbre") : TEXT("haie"),
				R.X0m, R.Y0m, R.X1m, R.Y1m, R.CourseCm, R.DAvantCm, R.DApresCm, R.RequisCm);
		}
		Out += FString::Printf(
			TEXT("],\"non_poses\":["));
		for (int32 i = 0; i < RetraitImpossible.Num(); ++i)
		{
			Out += FString::Printf(TEXT("%s{\"mesh\":\"%s\",\"x\":%.2f,\"y\":%.2f}"),
				i ? TEXT(",") : TEXT(""), *RetraitImpossible[i].Mesh,
				RetraitImpossible[i].Xm, RetraitImpossible[i].Ym);
		}
		Out += FString::Printf(
			TEXT("]},\"fosses_rondes\":{\"regle\":\"un ARBUSTE dont le sol rendu est ")
			TEXT("mineral recoit une fosse RONDE (diametre %.2f a %.2f m, proportionnel a ")
			TEXT("son etalement), terre en monticule vers la tige, fin anneau de %.0f cm, ")
			TEXT("fond garanti au-dessus de la dalle ; dans l'herbe, aucune fosse ; ")
			TEXT("au-dela de %.0f deg de pente locale l'arbuste RESTE mais sa fosse tombe ")
			TEXT("(une decoupe de revetement couchee sur un talus ne se lit plus)\",")
			TEXT("\"posees\":%d,\"rayon_moyen_cm\":%.1f,\"plan_local\":%d,")
			TEXT("\"pose_horizontale\":%d,\"relevements_bornes\":%d,")
			TEXT("\"abandonnees_pente\":%d}}"),
			GShrubPitMinM, GShrubPitMaxM, GShrubFrameCm, GShrubPitMaxSlopeDeg,
			NumShrubPitsPoses,
			NumShrubPitsPoses ? (float)(SumShrubRayonCm / NumShrubPitsPoses) : 0.f,
			NumShrubPitPlan, NumShrubPitPlat, NumShrubLiftBorne, NumShrubPitPente);
		FFileHelper::SaveStringToFile(Out, *P, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCityImport, Display,
		TEXT("Retraction hors chaussee (lot6) : %d arbres et %d haies RECULES (course ")
		TEXT("moyenne %.0f cm, maximum %.0f cm — %d par le gradient, %d par balayage), ")
		TEXT("%d arbres et %d haies NON POSES faute de position degagee, %d arbres et ")
		TEXT("%d haies inchanges. Fosses rondes d'arbustes : %d posees (rayon moyen ")
		TEXT("%.0f cm, plage %.2f-%.2f m de diametre), %d abandonnees pour pente > ")
		TEXT("%.0f deg (l'arbuste reste). Detail : %s.lot6.json"),
		NumRetractArbre, NumRetractHaie,
		(NumRetractArbre + NumRetractHaie)
			? (float)(SumCourseCm / (NumRetractArbre + NumRetractHaie)) : 0.f,
		MaxCourseCm, NumRetractParGradient, NumRetractParBalayage,
		NumRetractRateArbre, NumRetractRateHaie,
		NumRetractInchangeArbre, NumRetractInchangeHaie, NumShrubPitsPoses,
		NumShrubPitsPoses ? (float)(SumShrubRayonCm / NumShrubPitsPoses) : 0.f,
		GShrubPitMinM, GShrubPitMaxM, NumShrubPitPente, GShrubPitMaxSlopeDeg,
		*VegJsonPath);
	UE_LOG(LogCityImport, Display,
		TEXT("Vegetation importee : %d instances, %d meshes, %d acteurs — central film=%d, ")
		TEXT("central dalle=%d, fosses corrigees (couronne)=%d, multi-hit recuperes=%d, ")
		TEXT("REJETEES (sans sol)=%d, fosses de plantation=%d, cellules de masque=%d, ")
		TEXT("proxys sol=%d — regle SOL RENDU : +%d fosses gagnees, -%d perdues vs ")
		TEXT("canal R brut, bruit de bord %s, segments de bordure pour l'alignement=%d."),
		Summary.Instances, Summary.Meshes, Summary.Actors, NumFilmCentral, NumDalleCentral,
		NumFosseCorrige, NumMultiHitRecup, TraceMiss, Summary.Pits,
		MaskSampler.NumCellsLoaded(), TraceProxies.Num(),
		NumFosseGagnee, NumFossePerdue,
		MaskSampler.HasNoise() ? TEXT("CHARGE") : TEXT("ABSENT"), CurbSegs.Num());
	// LOT PIE — le compteur qui PROUVE que la collision de vegetation est bien tombee
	// a la creation. 0 avec `bVegCollisionHistorique=false` serait une regression.
	UE_LOG(LogCityImport, Display,
		TEXT("LOT PIE : %d composant(s) de vegetation pose(s) SANS COLLISION (mesh sans ")
		TEXT("primitive simple) ; les meshes qui portent un vrai volume (arbres) gardent ")
		TEXT("la leur. bVegCollisionHistorique=%s."),
		NbVegSansCollision, Gen.bVegCollisionHistorique ? TEXT("true") : TEXT("false"));
	if (bCellFilter)
	{
		// En mode district les compteurs ci-dessus portent sur les CELLULES VISEES,
		// jamais sur la ville : une comparaison de non-regression se fait sur une
		// regeneration COMPLETE. La ligne ci-dessous donne le solde reel des HISM.
		int32 TotalApres = 0;
		for (const TPair<FString, UHierarchicalInstancedStaticMeshComponent*>& P : HismVeg)
		{
			if (P.Value)
			{
				TotalApres += P.Value->GetInstanceCount();
			}
		}
		UE_LOG(LogCityImport, Display,
			TEXT("Vegetation MODE DISTRICT : -%d instances retirees / +%d reposees ; ")
			TEXT("%d acteur(s) vide(s) de leur part de district sans nouvelle instance ; ")
			TEXT("total dans les %d acteurs CityVeg repris = %d."),
			NumInstancesRetirees, Summary.Instances + Summary.Pits + NumShrubPitsPoses,
			NumActeursVides, HismVeg.Num(), TotalApres);
	}
	UE_LOG(LogCityImport, Display,
		TEXT("Pose sur SURFACE RENDUE (lot5) : %d instances que l'ancienne regle aurait ")
		TEXT("posees plus de 30 cm trop haut (dont %d de plus d'1 m, maximum %.0f cm) — la ")
		TEXT("collision d'origine des dalles ne decide plus. Garde-fou hit isole : %d ")
		TEXT("ecartes, %d gardes (avis des 4 colonnes voisines). Sondages sans surface ")
		TEXT("rendue (repli sur la collision d'origine, DOIT valoir 0)=%d. Detail : ")
		TEXT("%s.floating.json"),
		NumFlottant30, NumFlottant100, MaxFlottantCm, NumHitIsoleEcarte, NumHitIsoleGarde,
		NumSansProxy, *VegJsonPath);
	UE_LOG(LogCityImport, Display,
		TEXT("Arbres sur pente minerale (lot5) : %d instances SUPPRIMEES (arbre + fosse) ")
		TEXT("au-dela de %.0f deg de pente locale. Detail : %s.slope_skipped.json"),
		SkippedPente.Num(), GPenteMaxArbreMineralDeg, *VegJsonPath);
	UE_LOG(LogCityImport, Display,
		TEXT("Fosses v3 : cote moyen %.2f m (clamp bas=%d, clamp haut=%d), pose sur plan ")
		TEXT("local=%d, pose horizontale (emprise mal echantillonnee)=%d, non-planeite ")
		TEXT("max sous une emprise=%.1f cm, sondages rejetes (autre surface)=%d, ")
		TEXT("relevements bornes a %.0f cm=%d — fond de terre a +%.1f cm de la dalle, ")
		TEXT("haut du cadre a +%.1f cm, jupe %.0f cm."),
		Summary.Pits ? (float)(SumPitCoteCm / Summary.Pits * 0.01) : 0.f,
		NumPitClampMin, NumPitClampMax, NumPitPlan, NumPitPlat, MaxPitGiteCm,
		NumPitOutliers, GPitMaxLiftCm, NumPitLiftBorne,
		GPitClearCm, GPitLiftCm, GPitSkirtCm);
	if (NumCmpPits > 0)
	{
		auto Pc = [](int32 N, int32 D) { return D ? 100.f * N / D : 0.f; };
		UE_LOG(LogCityImport, Display,
			TEXT("Fosses AVANT/APRES sur %d fosses et %d sondages de la MEME dalle rendue — ")
			TEXT("fond occlus au centre : v2 %.1f %% -> v3 %.1f %% ; points de fond occlus : ")
			TEXT("v2 %.1f %% -> v3 %.1f %% ; coin immerge : v2 %.1f %% -> v3 %.1f %% ; ")
			TEXT("debordement racinaire : v2 %.1f %% -> v3 %.1f %%."),
			NumCmpPits, NumCmpPts,
			Pc(N2Fond, NumCmpPits), Pc(N3Fond, NumCmpPits),
			Pc(N2PtsOcc, NumCmpPts), Pc(N3PtsOcc, NumCmpPts),
			Pc(N2Coin, NumCmpPits), Pc(N3Coin, NumCmpPits),
			Pc(N2Debord, NumCmpPits), Pc(N3Debord, NumCmpPits));
	}
	return Summary;
}

namespace
{
	// Texture de fenetres tuilee (1 tuile = 1 fenetre centree sur fond mur blanc, le
	// blanc etant teinte par les vertex colors de la facade dans le materiau).
	UTexture2D* GetOrCreateFacadeTexture(const FString& AssetFolder)
	{
		const FString AssetPath = AssetFolder / TEXT("T_FacadeWindow");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		constexpr int32 Size = 128;
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		// Fenetre : 50 % de largeur, 45 % de hauteur, centree (52 % vertical — appui bas).
		constexpr int32 X0 = 32, X1 = 96, Y0 = 33, Y1 = 91;
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const bool bIn = X >= X0 && X < X1 && Y >= Y0 && Y < Y1;
				const bool bFrame = bIn && (X < X0 + 3 || X >= X1 - 3 || Y < Y0 + 3 || Y >= Y1 - 3);
				FColor C(255, 255, 255);                       // mur : blanc -> teinte facade
				if (bFrame) { C = FColor(210, 208, 200); }     // cadre clair
				else if (bIn) { C = FColor(58, 64, 74); }      // vitre sombre bleutee
				// Croisillon central vertical (2 px) pour casser l'aplat.
				if (bIn && !bFrame && FMath::Abs(X - (X0 + X1) / 2) < 1) { C = FColor(96, 100, 106); }
				Pixels[Y * Size + X] = C;
			}
		}
		UPackage* Package = CreatePackage(*AssetPath);
		UTexture2D* Tex = NewObject<UTexture2D>(Package, *ObjectName, RF_Public | RF_Standalone);
		Tex->Source.Init(Size, Size, 1, 1, TSF_BGRA8, (const uint8*)Pixels.GetData());
		Tex->SRGB = true;
		Tex->LODGroup = TEXTUREGROUP_World;
		Tex->AddressX = TA_Wrap;
		Tex->AddressY = TA_Wrap;
		Tex->UpdateResource();
		Tex->PostEditChange();
		Tex->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Tex);
		return Tex;
	}

	// Materiau facade : unlit, Emissive = texture fenetres x vertex color (meme
	// convention que M_BldgWall : les vertex colors sont deja compensees pow 2.2).
	UMaterialInterface* GetOrCreateFacadeMaterial(const FString& AssetFolder)
	{
		const FString AssetPath = AssetFolder / TEXT("M_BldgFacade");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		UTexture2D* Tex = GetOrCreateFacadeTexture(AssetFolder);
		UPackage* Package = CreatePackage(*AssetPath);
		UMaterial* M = NewObject<UMaterial>(Package, *ObjectName, RF_Public | RF_Standalone);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_Unlit);
		UMaterialExpressionTextureSample* Sample = NewObject<UMaterialExpressionTextureSample>(M);
		Sample->Texture = Tex;
		Sample->SamplerType = SAMPLERTYPE_Color;
		M->GetExpressionCollection().AddExpression(Sample);
		UMaterialExpressionVertexColor* VColor = NewObject<UMaterialExpressionVertexColor>(M);
		M->GetExpressionCollection().AddExpression(VColor);
		UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(M);
		Mul->A.Connect(0, Sample);
		Mul->B.Connect(0, VColor);
		M->GetExpressionCollection().AddExpression(Mul);
		M->GetEditorOnlyData()->EmissiveColor.Connect(0, Mul);
		M->PostEditChange();
		M->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(M);
		return M;
	}

	// Texture de ruban routier : U = travers (trottoir|bordure|asphalte|marquage|...),
	// V = longueur (tirets 3 m peints sur V<0,5, periode 6 m). Multipliee par la
	// teinte claire du vertex color : asphalte 0.2x, trottoir 0.59x, marquage 1x.
	UTexture2D* GetOrCreateRoadTexture(const FString& AssetFolder)
	{
		const FString AssetPath = AssetFolder / TEXT("T_RoadStrip");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		constexpr int32 Size = 128;
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				uint8 V = 50;                                    // asphalte
				if (X < 15 || X >= 113) { V = 150; }             // trottoirs
				else if (X < 17 || X >= 111) { V = 35; }         // bordures (lisibilite)
				else if (X >= 58 && X < 70 && Y < 64) { V = 255; } // marquage central (tiret)
				Pixels[Y * Size + X] = FColor(V, V, V);
			}
		}
		UPackage* Package = CreatePackage(*AssetPath);
		UTexture2D* Tex = NewObject<UTexture2D>(Package, *ObjectName, RF_Public | RF_Standalone);
		Tex->Source.Init(Size, Size, 1, 1, TSF_BGRA8, (const uint8*)Pixels.GetData());
		Tex->SRGB = true;
		Tex->LODGroup = TEXTUREGROUP_World;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Wrap;
		Tex->UpdateResource();
		Tex->PostEditChange();
		Tex->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Tex);
		return Tex;
	}

	// Materiau ruban routier : unlit, texture x vertex color (meme patron que la facade).
	UMaterialInterface* GetOrCreateRoadMaterial(const FString& AssetFolder)
	{
		const FString AssetPath = AssetFolder / TEXT("M_RoadStrip");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		UTexture2D* Tex = GetOrCreateRoadTexture(AssetFolder);
		UPackage* Package = CreatePackage(*AssetPath);
		UMaterial* M = NewObject<UMaterial>(Package, *ObjectName, RF_Public | RF_Standalone);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_Unlit);
		UMaterialExpressionTextureSample* Sample = NewObject<UMaterialExpressionTextureSample>(M);
		Sample->Texture = Tex;
		Sample->SamplerType = SAMPLERTYPE_Color;
		M->GetExpressionCollection().AddExpression(Sample);
		UMaterialExpressionVertexColor* VColor = NewObject<UMaterialExpressionVertexColor>(M);
		M->GetExpressionCollection().AddExpression(VColor);
		UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(M);
		Mul->A.Connect(0, Sample);
		Mul->B.Connect(0, VColor);
		M->GetExpressionCollection().AddExpression(Mul);
		M->GetEditorOnlyData()->EmissiveColor.Connect(0, Mul);
		M->PostEditChange();
		M->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(M);
		return M;
	}

	// ---- Lot B (J2 §3.3-3.4) : matiere desktop — atlas de facades + materiaux PBR ----

	// Hash 2D stable (meme famille que les jitters d'arbres).
	float TileHash(int32 A, int32 B)
	{
		return FMath::Frac(FMath::Sin(A * 12.9898f + B * 78.233f) * 43758.5453f);
	}

	// Atlas de facades 2048² (parametrable) en grille 4x4 de sous-tuiles generees
	// procedureralement : 0-3 brique toulousaine (4 variantes), 4-7 enduit (4),
	// 8 moderne, 9 industriel, 10 toit tuiles terre cuite, 11 toit ardoise,
	// 12 pierre claire (modenature), 13 beton, 14 enduit blanc, 15 neutre.
	// Tuiles quasi neutres (valeur/motif) : la COULEUR vient du VertexColor
	// (UsageTint) dans M_CityWall_PBR — sauf les toits qui portent leur teinte.
	UTexture2D* GetOrCreateCityAtlasTexture(const FString& AssetFolder, int32 SizePx)
	{
		const FString AssetPath = AssetFolder / TEXT("T_CityAtlas");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		const int32 Size = FMath::Clamp(SizePx, 512, 4096);
		const int32 T = Size / 4;
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const int32 Tile = (Y / T) * 4 + (X / T);
				const int32 LX = X % T, LY = Y % T;
				FColor C(200, 200, 200);
				if (Tile <= 3)
				{
					// Brique : rangees decalees d'une demi-brique, joints de mortier.
					const int32 BH = FMath::Max(T / 18, 4), BW = FMath::Max(T / 8, 8);
					const int32 Row = LY / BH;
					const int32 LXo = LX + ((Row % 2) ? BW / 2 : 0);
					const int32 Col2 = LXo / BW;
					if (LY % BH < 2 || LXo % BW < 2)
					{
						C = FColor(168, 165, 160);                    // mortier
					}
					else
					{
						const uint8 V = (uint8)(196 + 28.f * (TileHash(Col2, Row * 7 + Tile) - 0.5f));
						C = FColor(V, (uint8)(V * 0.95f), (uint8)(V * 0.90f));
					}
				}
				else if (Tile <= 7)
				{
					// Enduit : grain leger, 4 graines de variation.
					const float N = TileHash(LX / 6 + Tile * 31, LY / 6)
						+ 0.5f * TileHash(LX, LY + Tile);
					const uint8 V = (uint8)FMath::Clamp(206.f + 14.f * (N - 0.75f), 0.f, 255.f);
					C = FColor(V, V, (uint8)(V * 0.97f));
				}
				else if (Tile == 8)
				{
					// Moderne : panneaux avec joints creux.
					const int32 P = T / 4;
					C = (LX % P < 2 || LY % P < 2) ? FColor(140, 140, 142) : FColor(212, 212, 214);
				}
				else if (Tile == 9)
				{
					// Industriel : bardage ondule vertical.
					C = ((LX / FMath::Max(T / 64, 2)) % 2) ? FColor(180, 181, 184) : FColor(204, 205, 208);
				}
				else if (Tile == 10 || Tile == 11)
				{
					// Toits : rangees de tuiles ombrees (terre cuite / ardoise).
					const int32 RH = FMath::Max(T / 12, 6);
					const float G = (float)(LY % RH) / RH;
					const int32 Row = LY / RH;
					const int32 Sep = FMath::Max(T / 16, 4);
					float V01 = 0.55f + 0.45f * G;
					if ((LX + Row * Sep / 2) % Sep < 1) { V01 *= 0.8f; }
					if (Tile == 10)
					{
						C = FColor((uint8)(212 * V01), (uint8)(124 * V01), (uint8)(88 * V01));
					}
					else
					{
						const uint8 V = (uint8)(150 * V01 + 20.f * TileHash(LX / Sep, Row));
						C = FColor(V, V, (uint8)(V * 1.05f));
					}
				}
				else if (Tile == 12)
				{
					// Pierre claire : modenature (tableaux, appuis, linteaux).
					const uint8 V = (uint8)(220 + 8.f * (TileHash(LX / 8, LY / 8) - 0.5f));
					C = FColor(V, (uint8)(V * 0.98f), (uint8)(V * 0.93f));
				}
				else if (Tile == 13)
				{
					const uint8 V = (uint8)(184 + 20.f * (TileHash(LX / 12 + 5, LY / 12) - 0.5f));
					C = FColor(V, V, V);
				}
				else if (Tile == 14)
				{
					const uint8 V = (uint8)(233 + 6.f * (TileHash(LX, LY + 9) - 0.5f));
					C = FColor(V, V, V);
				}
				Pixels[Y * Size + X] = C;
			}
		}
		UPackage* Package = CreatePackage(*AssetPath);
		UTexture2D* Tex = NewObject<UTexture2D>(Package, *ObjectName, RF_Public | RF_Standalone);
		Tex->Source.Init(Size, Size, 1, 1, TSF_BGRA8, (const uint8*)Pixels.GetData());
		Tex->SRGB = true;
		Tex->LODGroup = TEXTUREGROUP_World;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		Tex->UpdateResource();
		Tex->PostEditChange();
		Tex->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Tex);
		return Tex;
	}

	// UV dans une sous-tuile de l'atlas (grille 4x4), marge anti-bleed 3 %.
	FVector2f AtlasUV(int32 Tile, float U01, float V01)
	{
		const int32 TX = Tile % 4, TY = Tile / 4;
		const float M = 0.03f;
		return FVector2f((TX + M + FMath::Clamp(U01, 0.f, 1.f) * (1.f - 2.f * M)) * 0.25f,
			(TY + M + FMath::Clamp(V01, 0.f, 1.f) * (1.f - 2.f * M)) * 0.25f);
	}

	// Sous-tuile de facade par usage/graine : meme logique de variation qu'UsageTint.
	int32 UsageTile(const FString& Usage, int32 Seed)
	{
		if (Usage == TEXT("res")) { return Seed % 4; }         // brique toulousaine x4
		if (Usage == TEXT("com")) { return 4 + Seed % 4; }     // enduit x4
		if (Usage == TEXT("ind")) { return 9; }                // industriel
		return 8;                                              // moderne
	}

	// -----------------------------------------------------------------------------
	// J3b — toits en pente ANCRES. Le squelette droit est PRECALCULE par
	// Tools/j3b_prep_toits.py (bpypolyskel) et livre dans le JSON batiments
	// (bloc "roof") : le C++ ne fait que mailler, z = egout + delta * d / maxd
	// (la pente est lineaire en d par construction du squelette). Tout bloc
	// incoherent -> toit plat historique, jamais d'erreur fatale.
	struct FRoofData
	{
		float EaveCm = 0.f;          // egout - sol (IGN alt_min_toit - alt_min_sol)
		float DeltaCm = 0.f;         // faitage - egout (borne [0,3 ; 30] m au prep)
		int32 Tile = 10;             // sous-tuile atlas (10 tuile, 11 ardoise/zinc, 13 beton)
		TArray<FVector3f> Skel;      // noeuds du squelette : x,y en cm, Z = retrait d en cm
		TArray<TArray<int32>> Faces; // versants ; idx < N = anneau, sinon Skel[idx - N]
		float MaxDcm = 0.f;
		// J3c — teinte de vertex du toit : couleur REELLE echantillonnee dans la BD
		// ORTHO par Tools/j3c_tint_toits.py (mediane robuste / gris de reference),
		// deja multipliee par le quasi-blanc historique. Defaut = ancien RoofTint.
		FVector3f Tint = FVector3f(0.95f, 0.95f, 0.95f);
		// PoC LiDAR (Roofer/3DBAG) : maillage de toit ABSOLU auto-porte. Quand present
		// (champs "rv"/"rf" du batiment), il remplace le squelette droit : les faces
		// indexent AbsVerts SEULS (aucun contrat d'arete de bord), z = hauteur au-dessus
		// du sol du batiment (cm). Le squelette (Skel/Faces) reste vide.
		bool bAbs = false;
		TArray<FVector3f> AbsVerts;      // x,y,z en cm ; z = hauteur / sol du batiment
		TArray<TArray<int32>> AbsFaces;  // versants LiDAR, indices dans AbsVerts
		// PoC LiDAR — MURS Roofer (WallSurface du Solid LoD2.2). Present (champ "wf")
		// => la coque abs est COMPLETE (toit + murs soudes au meme egout, watertight) :
		// on rend ces murs et on SAUTE l'anneau de mur procedural (qui montait a une
		// hauteur plate unique -> trous a l'egout LiDAR variable). Indices dans AbsVerts
		// (memes sommets que le toit a l'egout). Vide => murs proceduraux (compat).
		TArray<TArray<int32>> AbsWallFaces;
	};

	int32 RoofTileFromMat(const FString& Mat)
	{
		if (Mat == TEXT("ardoise") || Mat == TEXT("zinc")) { return 11; }
		if (Mat == TEXT("beton")) { return 13; }
		return 10; // tuile terre cuite — defaut toulousain (verrou 1 : ~79 % tuile)
	}

	// Lit et VALIDE le bloc "roof" d'un batiment (N = nombre de points du contour).
	// Holes = anneaux de cour (peut etre vide) : l'espace d'indices des faces est alors
	// contour(N) ++ trous(HTotal) ++ squelette(Skel), et la 1re arete d'un versant peut
	// etre une arete du contour OU d'un trou (avant-toit de cour). Vide = comportement
	// historique bit-a-bit. false = pas de toit en pente (l'appelant retombe sur plat).
	bool ParseRoof(const TSharedPtr<FJsonObject>& Bldg, int32 N,
		const TArray<TArray<FVector2D>>& Holes, FRoofData& Out)
	{
		const TSharedPtr<FJsonObject>* RoofObj = nullptr;
		if (!Bldg->TryGetObjectField(TEXT("roof"), RoofObj))
		{
			return false;
		}
		// PoC LiDAR : maillage de toit ABSOLU (Roofer). Prioritaire sur le squelette.
		// "rv" = sommets [x,y,z] en metres locaux (z = hauteur / sol du batiment) ;
		// "rf" = faces indexant rv. Ne suit PAS le contrat d'arete de bord du squelette.
		{
			const TArray<TSharedPtr<FJsonValue>>* Rv = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Rf = nullptr;
			if ((*RoofObj)->TryGetArrayField(TEXT("rv"), Rv) &&
				(*RoofObj)->TryGetArrayField(TEXT("rf"), Rf) &&
				Rv->Num() >= 3 && Rf->Num() >= 1)
			{
				Out.AbsVerts.Reset(Rv->Num());
				float ZMin = FLT_MAX, ZMax = -FLT_MAX;
				for (const TSharedPtr<FJsonValue>& V : *Rv)
				{
					const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
					if (C.Num() < 3) { return false; }
					const FVector3f P((float)(C[0]->AsNumber() * 100.0),
						(float)(C[1]->AsNumber() * 100.0), (float)(C[2]->AsNumber() * 100.0));
					if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z))
					{
						return false;
					}
					ZMin = FMath::Min(ZMin, P.Z);
					ZMax = FMath::Max(ZMax, P.Z);
					Out.AbsVerts.Add(P);
				}
				const int32 NV = Out.AbsVerts.Num();
				Out.AbsFaces.Reset(Rf->Num());
				for (const TSharedPtr<FJsonValue>& FV : *Rf)
				{
					TArray<int32> Face;
					for (const TSharedPtr<FJsonValue>& IV : FV->AsArray())
					{
						const int32 Idx = (int32)IV->AsNumber();
						if (Idx < 0 || Idx >= NV) { return false; }
						Face.Add(Idx);
					}
					if (Face.Num() >= 3) { Out.AbsFaces.Add(MoveTemp(Face)); }
				}
				if (Out.AbsFaces.Num() == 0) { return false; }
				// MURS Roofer optionnels (WallSurface) : memes indices AbsVerts que le toit
				// -> coque complete soudee a l'egout. Absent = repli murs proceduraux.
				const TArray<TSharedPtr<FJsonValue>>* Wf = nullptr;
				if ((*RoofObj)->TryGetArrayField(TEXT("wf"), Wf))
				{
					Out.AbsWallFaces.Reset(Wf->Num());
					for (const TSharedPtr<FJsonValue>& FV : *Wf)
					{
						TArray<int32> Face;
						bool bBad = false;
						for (const TSharedPtr<FJsonValue>& IV : FV->AsArray())
						{
							const int32 Idx = (int32)IV->AsNumber();
							if (Idx < 0 || Idx >= NV) { bBad = true; break; }
							Face.Add(Idx);
						}
						if (!bBad && Face.Num() >= 3) { Out.AbsWallFaces.Add(MoveTemp(Face)); }
					}
				}
				Out.bAbs = true;
				Out.EaveCm = FMath::Max(ZMin, 0.f);      // egout = point de toit le plus bas
				Out.DeltaCm = FMath::Max(ZMax - ZMin, 0.f);
				Out.MaxDcm = 1.f;
				FString AbsMat;
				(*RoofObj)->TryGetStringField(TEXT("mat"), AbsMat);
				Out.Tile = RoofTileFromMat(AbsMat);
				// J3f LiDAR combine — teinte ORTHO par batiment AUSSI sur la coque abs.
				// Le squelette lisait "tint" en fin de fonction (apres ce return) : les
				// toits Roofer restaient donc au quasi-blanc par defaut (terracotta
				// uniforme). On lit "tint" ICI, avant le return, pour que chaque toit
				// LiDAR recoive sa vraie teinte ortho (meme validation/clamp que plus bas).
				const TArray<TSharedPtr<FJsonValue>>* AbsTintArr = nullptr;
				if (Bldg->TryGetArrayField(TEXT("tint"), AbsTintArr) && AbsTintArr->Num() >= 3)
				{
					FVector3f T(0.f, 0.f, 0.f);
					bool bOk = true;
					for (int32 i = 0; i < 3; ++i)
					{
						const float V = (float)(*AbsTintArr)[i]->AsNumber();
						if (!FMath::IsFinite(V)) { bOk = false; break; }
						T[i] = FMath::Clamp(V, 0.3f, 2.0f) * 0.95f;
					}
					if (bOk) { Out.Tint = T; }
				}
				return true;
			}
		}
		double Eave = 0.0, Delta = 0.0;
		if (!(*RoofObj)->TryGetNumberField(TEXT("eave"), Eave) ||
			!(*RoofObj)->TryGetNumberField(TEXT("delta"), Delta) ||
			Eave < 2.0 || Delta < 0.3 || Delta > 30.0)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Sv = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Fa = nullptr;
		if (!(*RoofObj)->TryGetArrayField(TEXT("sv"), Sv) || Sv->Num() == 0 ||
			!(*RoofObj)->TryGetArrayField(TEXT("f"), Fa) || Fa->Num() < 3)
		{
			return false;
		}
		Out.EaveCm = (float)(Eave * 100.0);
		Out.DeltaCm = (float)(Delta * 100.0);
		FString Mat;
		(*RoofObj)->TryGetStringField(TEXT("mat"), Mat);
		Out.Tile = RoofTileFromMat(Mat);
		Out.Skel.Reset(Sv->Num());
		Out.MaxDcm = 0.f;
		for (const TSharedPtr<FJsonValue>& V : *Sv)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() < 3)
			{
				return false;
			}
			const FVector3f P((float)(C[0]->AsNumber() * 100.0),
				(float)(C[1]->AsNumber() * 100.0), (float)(C[2]->AsNumber() * 100.0));
			if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || P.Z < 0.f)
			{
				return false;
			}
			Out.MaxDcm = FMath::Max(Out.MaxDcm, P.Z);
			Out.Skel.Add(P);
		}
		if (Out.MaxDcm < 1.f)
		{
			return false;
		}
		// Sommets de BORD (contour + trous), tous a d = 0 ; le squelette vient apres.
		int32 HTotal = 0;
		for (const TArray<FVector2D>& H : Holes) { HTotal += H.Num(); }
		const int32 NB = N + HTotal;
		// Aretes de BORD admissibles comme 1re arete d'un versant (egout exterieur OU
		// avant-toit de cour) — contrat du prep (skeleton_faces valide l'identique).
		// Sans trou : Bnd = les seules aretes (i, i+1) du contour, donc ce test est
		// STRICTEMENT equivalent a l'ancien Face[0]<N && Face[1]<N && voisines.
		TSet<TPair<int32, int32>> Bnd;
		for (int32 i = 0; i < N; ++i) { Bnd.Add(TPair<int32, int32>(i, (i + 1) % N)); }
		{
			int32 Base = N;
			for (const TArray<FVector2D>& H : Holes)
			{
				const int32 M = H.Num();
				for (int32 i = 0; i < M; ++i) { Bnd.Add(TPair<int32, int32>(Base + i, Base + (i + 1) % M)); }
				Base += M;
			}
		}
		Out.Faces.Reset(Fa->Num());
		for (const TSharedPtr<FJsonValue>& FV : *Fa)
		{
			TArray<int32> Face;
			for (const TSharedPtr<FJsonValue>& IV : FV->AsArray())
			{
				const int32 Idx = (int32)IV->AsNumber();
				if (Idx < 0 || Idx >= NB + Out.Skel.Num())
				{
					return false;
				}
				Face.Add(Idx);
			}
			// Contrat du prep : la 1re arete de chaque versant = une arete de bord.
			if (Face.Num() < 3 || !Bnd.Contains(TPair<int32, int32>(Face[0], Face[1])))
			{
				return false;
			}
			Out.Faces.Add(MoveTemp(Face));
		}
		// J3c — "tint":[r,g,b] OPTIONNEL au niveau du batiment (l'ortho couvre aussi
		// les toits plats, qui n'ont pas de bloc roof). Absent ou aberrant -> quasi
		// blanc historique : aucune regression si le champ n'est pas la.
		const TArray<TSharedPtr<FJsonValue>>* TintArr = nullptr;
		if (Bldg->TryGetArrayField(TEXT("tint"), TintArr) && TintArr->Num() >= 3)
		{
			FVector3f T(0.f, 0.f, 0.f);
			bool bOk = true;
			for (int32 i = 0; i < 3; ++i)
			{
				const float V = (float)(*TintArr)[i]->AsNumber();
				if (!FMath::IsFinite(V))
				{
					bOk = false;
					break;
				}
				T[i] = FMath::Clamp(V, 0.3f, 2.0f) * 0.95f;
			}
			if (bOk)
			{
				Out.Tint = T;
			}
		}
		return true;
	}

	// Fabrique commune des materiaux PBR Lot B : DefaultLit (PAS unlit — Lumen
	// eclaire, spec §3.3), BaseColor = [Texture x] VertexColor ou constante,
	// Roughness/Metallic constants. Meme patron code que GetOrCreateFacadeMaterial.
	UMaterialInterface* GetOrCreatePBRMaterial(const FString& AssetFolder, const TCHAR* Name,
		UTexture2D* TexOrNull, bool bVertexColor, const FLinearColor& ConstantBase,
		float Roughness, float Metallic)
	{
		const FString AssetPath = AssetFolder / Name;
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(*AssetPath);
		UMaterial* M = NewObject<UMaterial>(Package, *ObjectName, RF_Public | RF_Standalone);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_DefaultLit);
		// Flag d'usage Nanite : les meshes Lot B opaques (et le verre opaque J2e)
		// sont Nanite ; sans ce flag, -game refuse le materiau (« missing usage
		// flag Nanite ») et rend le fallback decime 0,1 % en Default Material
		// (toits fondus). L'acces direct a bUsedWithNanite est deprecie en 5.8 :
		// SetUsageByFlag pose le flag sans recompiler, le PostEditChange final
		// compile avec l'usage inclus.
		M->SetUsageByFlag(MATUSAGE_Nanite, true);
		UMaterialExpression* Base = nullptr;
		if (TexOrNull)
		{
			UMaterialExpressionTextureSample* Sample = NewObject<UMaterialExpressionTextureSample>(M);
			Sample->Texture = TexOrNull;
			Sample->SamplerType = SAMPLERTYPE_Color;
			M->GetExpressionCollection().AddExpression(Sample);
			Base = Sample;
		}
		if (bVertexColor)
		{
			UMaterialExpressionVertexColor* VColor = NewObject<UMaterialExpressionVertexColor>(M);
			M->GetExpressionCollection().AddExpression(VColor);
			if (Base)
			{
				UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(M);
				Mul->A.Connect(0, Base);
				Mul->B.Connect(0, VColor);
				M->GetExpressionCollection().AddExpression(Mul);
				Base = Mul;
			}
			else
			{
				Base = VColor;
			}
		}
		if (!Base)
		{
			UMaterialExpressionConstant3Vector* CB = NewObject<UMaterialExpressionConstant3Vector>(M);
			CB->Constant = ConstantBase;
			M->GetExpressionCollection().AddExpression(CB);
			Base = CB;
		}
		M->GetEditorOnlyData()->BaseColor.Connect(0, Base);
		UMaterialExpressionConstant* R = NewObject<UMaterialExpressionConstant>(M);
		R->R = Roughness;
		M->GetExpressionCollection().AddExpression(R);
		M->GetEditorOnlyData()->Roughness.Connect(0, R);
		if (Metallic > 0.f)
		{
			UMaterialExpressionConstant* Met = NewObject<UMaterialExpressionConstant>(M);
			Met->R = Metallic;
			M->GetExpressionCollection().AddExpression(Met);
			M->GetEditorOnlyData()->Metallic.Connect(0, Met);
		}
		M->PostEditChange();
		M->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(M);
		return M;
	}

	// Murs : atlas facades x VertexColor (teinte UsageTint LINEAIRE = variation).
	UMaterialInterface* GetOrCreateWallPBRMaterial(const FString& AssetFolder, int32 AtlasSizePx)
	{
		return GetOrCreatePBRMaterial(AssetFolder, TEXT("M_CityWall_PBR"),
			GetOrCreateCityAtlasTexture(AssetFolder, AtlasSizePx), true,
			FLinearColor::White, 0.8f, 0.f);
	}

	// Vitres : OPAQUE tres lisse (choix Q3, le plus robuste Lumen : le translucide
	// n'a ni reflets Lumen fiables ni depth propre, l'opaque roughness 0,05 recoit
	// reflexions ecran/Lumen sans cout ni tri). Sombre bleute, leger metallic.
	UMaterialInterface* GetOrCreateGlassPBRMaterial(const FString& AssetFolder)
	{
		return GetOrCreatePBRMaterial(AssetFolder, TEXT("M_CityGlass_PBR"),
			nullptr, false, FLinearColor(0.02f, 0.035f, 0.05f), 0.05f, 0.4f);
	}

	// Sol : version lit du vertex-color unlit historique (dalles peintes, sentiers).
	UMaterialInterface* GetOrCreateGroundPBRMaterial(const FString& AssetFolder)
	{
		return GetOrCreatePBRMaterial(AssetFolder, TEXT("M_CityGround_PBR"),
			nullptr, true, FLinearColor::White, 0.9f, 0.f);
	}

	// Routes : version lit du ruban texture (meme T_RoadStrip x VertexColor).
	UMaterialInterface* GetOrCreateRoadPBRMaterial(const FString& AssetFolder)
	{
		return GetOrCreatePBRMaterial(AssetFolder, TEXT("M_CityRoad_PBR"),
			GetOrCreateRoadTexture(AssetFolder), true, FLinearColor::White, 0.85f, 0.f);
	}

	// Batiment desktop Lot B : fenetres GEOMETRIQUES en creux. Wall recoit murs,
	// socle, toit et modenature (opaque -> atlas PBR, Nanite) ; Glass les vitres
	// (Nanite AUSSI depuis J2e — verre opaque). En mode non-split, passer le
	// MEME builder aux deux.
	// bReveals : par fenetre, +9 quads (+18 tris) EXACTEMENT vs la vitre en simple
	// retrait — 4 tableaux (retours 18 cm) + appui saillant 3 quads (dessus/face/
	// dessous, saillie 8 cm) + linteau saillant 2 quads (face avant/sous-face,
	// saillie 6 cm). C'est la base du test « delta de tris par fenetre = +18 ».
	// bBakedShade=false (PBR) : teintes brutes, Lumen eclaire (plus de Shade cuit).
	void BuildPolygonBuildingDesktop(FCityMeshBuilder& Wall, FCityMeshBuilder& Glass,
		const TArray<FVector2D>& PtsCm, float Hcm, const FVector3f& TintIn, int32 WallTileIn,
		float ZBaseCm, float SocleDepthCm, bool bReveals, bool bBakedShade,
		const TArray<TArray<FVector2D>>& Holes, const FRoofData* Roof = nullptr,
		int32 WindowMode = 0, bool bMarbleWhite = false, bool bMarbleWalls = false)
	{
		const int32 Floors = FMath::Clamp(FMath::RoundToInt32(Hcm / 290.f), 1, 40);
		const float FloorH = Hcm / Floors;
		const FVector3f StoneTint(0.82f, 0.79f, 0.72f);
		// J3f DA marbre : murs blancs sur la pierre claire (12), UsageTint/UsageTile ignores.
		// bMarbleWalls (flag E) : marbre AUX MURS SEULEMENT (le toit garde sa tuile reelle) ;
		// bMarbleWhite reste tout-ou-rien (murs ET toits). Les murs suivent donc l'un OU l'autre.
		const bool bWallsMarble = bMarbleWhite || bMarbleWalls;
		const FVector3f Tint = bWallsMarble ? MarbleTint() : TintIn;
		const int32 WallTile = bWallsMarble ? 12 : WallTileIn;
		// PoC LiDAR : coque Roofer complete -> murs abs rendus ici, anneau procedural
		// reduit au SOCLE enterre (le reste du mur = faces Roofer soudees a l'egout).
		const bool bAbsWalls = (Roof != nullptr) && Roof->bAbs && Roof->AbsWallFaces.Num() > 0;
		// Modes fenetres : 1=aucune (pas de travees), 2=discretes (niche marbre), sinon
		// geometrie complete (3=forcee, 0=historique via bReveals).
		const bool bNoWindows = (WindowMode == 1);
		const bool bDiscreetWindows = (WindowMode == 2);
		auto Col = [&](const FVector3f& C, const FVector3f& Nrm, float Zrel)
		{
			return bBakedShade ? Shade(C, Nrm, Zrel) : C;
		};
		// Un anneau de murs. Appele sur le CONTOUR (exterieur, CCW -> normale sortante)
		// puis, si cour, sur chaque TROU (CW -> la meme formule Nout(dy,-dx) pointe vers
		// l'interieur de la cour = la bonne face). Le corps est l'ancienne boucle d'aretes,
		// inchangee : sans cour, seul le contour est monte -> geometrie bit-a-bit identique.
		auto BuildWallRing = [&](const TArray<FVector2D>& Ring, bool bExterior)
		{
		const int32 N = Ring.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = Ring[e];
			const FVector2D B2 = Ring[(e + 1) % N];
			const FVector2D Dir2 = B2 - A2;
			const float Len = Dir2.Size();
			// Seuil a 1 cm (pas 30) : au-dela de ~1 cm on MONTE le mur de l'arete,
			// meme courte. L'ancien saut a 30 cm laissait une FENTE verticale pleine
			// hauteur partout ou le contour a un petit redan / pan coupe de coin
			// (mesure : 46 778 aretes, 8 644 batiments = 6,58 %, dont le Capitole).
			// Les murs sont bout a bout aux sommets partages : monter chaque arete
			// rend l'anneau etanche. La garde U1-U0<1 de WallQuad couvre deja les
			// quads degeneres ; 1 cm evite juste le NaN de Dir2/Len sur un doublon.
			if (Len < 1.f)
			{
				continue;
			}
			const FVector2D T2 = Dir2 / Len;
			const FVector3f Nout(T2.Y, -T2.X, 0.f);
			const FVector3f A(A2.X, A2.Y, 0.f);
			const FVector3f T(T2.X, T2.Y, 0.f);
			// Point de facade : U le long du mur, Z relatif au rez-de-chaussee,
			// D profondeur vers l'interieur (creux) — negatif = saillie.
			auto Pt = [&](float U, float Z, float D)
			{
				return A + T * U + FVector3f(0, 0, ZBaseCm + Z) - Nout * D;
			};
			// Quad de mur : UV0 dans la sous-tuile atlas, echelle ~5,12 m / tuile
			// (clampee : un mur aveugle geant etire sa tuile, assume Lot B).
			auto WallQuad = [&](float U0, float U1, float Z0, float Z1)
			{
				if (U1 - U0 < 1.f || Z1 - Z0 < 1.f)
				{
					return;
				}
				const FVector3f P[4] = { Pt(U0, Z0, 0), Pt(U1, Z0, 0), Pt(U1, Z1, 0), Pt(U0, Z1, 0) };
				const float SU = (U1 - U0) / 512.f;
				const float SV = (Z1 - Z0) / 512.f;
				const FVector2f UV[4] = { AtlasUV(WallTile, 0, 0), AtlasUV(WallTile, SU, 0),
					AtlasUV(WallTile, SU, SV), AtlasUV(WallTile, 0, SV) };
				Wall.AddPoly(Wall.WallGroup, P, 4, Nout, UV, Col(Tint, Nout, (Z0 + Z1) * 0.5f));
			};
			// Quad de modenature en pierre claire (sous-tuile 12).
			auto StoneQuad = [&](const FVector3f& P0, const FVector3f& P1, const FVector3f& P2,
				const FVector3f& P3, const FVector3f& Nrm)
			{
				const FVector3f P[4] = { P0, P1, P2, P3 };
				const FVector2f UV[4] = { AtlasUV(12, 0.1f, 0.1f), AtlasUV(12, 0.4f, 0.1f),
					AtlasUV(12, 0.4f, 0.4f), AtlasUV(12, 0.1f, 0.4f) };
				Wall.AddPoly(Wall.WallGroup, P, 4, Nrm, UV,
					Col(StoneTint, Nrm, P0.Z - ZBaseCm));
			};
			// Bandeau horizontal saillant de pierre claire (modenature J3f) : 3 quads
			// (dessus / face avant / dessous), centre a Zc, demi-hauteur Half, saillie
			// Proj vers l'exterieur (D<0, cf. -Nout*D). Meme enroulement que l'appui de
			// fenetre plus haut (rendu prouve). Court le long de TOUTE l'arete ; les
			// bandeaux d'aretes voisines se recouvrent au coin convexe mais dans le
			// MEME materiau (StoneTint) -> pas de scintillement lisible. Garde-fous :
			// arete courte / bande plate -> rien (aucun quad degenere).
			auto Band = [&](float Zc, float Half, float Proj)
			{
				if (Len < 60.f || Half < 1.f || Proj < 1.f)
				{
					return;
				}
				const float Zb = Zc - Half, Zt = Zc + Half;
				StoneQuad(Pt(0.f, Zt, -Proj), Pt(Len, Zt, -Proj),
					Pt(Len, Zt, 0.f), Pt(0.f, Zt, 0.f), FVector3f(0, 0, 1));   // dessus
				StoneQuad(Pt(0.f, Zb, -Proj), Pt(Len, Zb, -Proj),
					Pt(Len, Zt, -Proj), Pt(0.f, Zt, -Proj), Nout);            // face avant
				StoneQuad(Pt(0.f, Zb, -Proj), Pt(Len, Zb, -Proj),
					Pt(Len, Zb, 0.f), Pt(0.f, Zb, 0.f), FVector3f(0, 0, -1)); // dessous
			};
			// Fenetre : vitre en retrait D, puis modenature (+9 quads si bReveals).
			auto Window = [&](float WU0, float WU1, float WZ0, float WZ1)
			{
				const float D = 18.f;
				const FVector3f G[4] = { Pt(WU0, WZ0, D), Pt(WU1, WZ0, D),
					Pt(WU1, WZ1, D), Pt(WU0, WZ1, D) };
				const FVector2f GUV[4] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
				Glass.AddPoly(Glass.GlassGroup, G, 4, Nout, GUV,
					bBakedShade ? Shade(FVector3f(0.35f, 0.35f, 0.35f), Nout, (WZ0 + WZ1) * 0.5f)
						: FVector3f(1, 1, 1));
				if (!bReveals)
				{
					return;
				}
				// 4 tableaux (retours d'embrasure de la facade vers la vitre).
				StoneQuad(Pt(WU0, WZ0, 0), Pt(WU0, WZ1, 0), Pt(WU0, WZ1, D), Pt(WU0, WZ0, D), T);
				StoneQuad(Pt(WU1, WZ0, 0), Pt(WU1, WZ1, 0), Pt(WU1, WZ1, D), Pt(WU1, WZ0, D), -T);
				StoneQuad(Pt(WU0, WZ1, 0), Pt(WU1, WZ1, 0), Pt(WU1, WZ1, D), Pt(WU0, WZ1, D),
					FVector3f(0, 0, -1));
				StoneQuad(Pt(WU0, WZ0, 0), Pt(WU1, WZ0, 0), Pt(WU1, WZ0, D), Pt(WU0, WZ0, D),
					FVector3f(0, 0, 1));
				// Appui saillant : dessus, face avant, dessous (visible en vol drone).
				const float S = 8.f, E = 6.f;
				StoneQuad(Pt(WU0, WZ0, -S), Pt(WU1, WZ0, -S), Pt(WU1, WZ0, 0), Pt(WU0, WZ0, 0),
					FVector3f(0, 0, 1));
				StoneQuad(Pt(WU0, WZ0 - E, -S), Pt(WU1, WZ0 - E, -S), Pt(WU1, WZ0, -S), Pt(WU0, WZ0, -S),
					Nout);
				StoneQuad(Pt(WU0, WZ0 - E, -S), Pt(WU1, WZ0 - E, -S), Pt(WU1, WZ0 - E, 0), Pt(WU0, WZ0 - E, 0),
					FVector3f(0, 0, -1));
				// Linteau saillant : face avant + sous-face.
				const float L = 6.f, LH = 12.f;
				StoneQuad(Pt(WU0, WZ1, -L), Pt(WU1, WZ1, -L), Pt(WU1, WZ1 + LH, -L), Pt(WU0, WZ1 + LH, -L),
					Nout);
				StoneQuad(Pt(WU0, WZ1, -L), Pt(WU1, WZ1, -L), Pt(WU1, WZ1, 0), Pt(WU0, WZ1, 0),
					FVector3f(0, 0, -1));
			};
			// Fenetre DISCRETE (J3f DA option C) : le mur reste PLEIN (le quad plein
			// couvre deja la travee) ; on creuse juste une niche marbre peu profonde
			// (6 cm) — fond en materiau MUR + 4 tableaux pierre. Ni trou traversant, ni
			// vitre sombre : un simple renfoncement qui accroche l'ombre en vol.
			auto BlindWindow = [&](float WU0, float WU1, float WZ0, float WZ1)
			{
				const float Dd = 6.f;
				const FVector3f B[4] = { Pt(WU0, WZ0, Dd), Pt(WU1, WZ0, Dd),
					Pt(WU1, WZ1, Dd), Pt(WU0, WZ1, Dd) };
				const float SU = (WU1 - WU0) / 512.f;
				const float SV = (WZ1 - WZ0) / 512.f;
				const FVector2f BUV[4] = { AtlasUV(WallTile, 0, 0), AtlasUV(WallTile, SU, 0),
					AtlasUV(WallTile, SU, SV), AtlasUV(WallTile, 0, SV) };
				Wall.AddPoly(Wall.WallGroup, B, 4, Nout, BUV, Col(Tint, Nout, (WZ0 + WZ1) * 0.5f));
				StoneQuad(Pt(WU0, WZ0, 0), Pt(WU0, WZ1, 0), Pt(WU0, WZ1, Dd), Pt(WU0, WZ0, Dd), T);
				StoneQuad(Pt(WU1, WZ0, 0), Pt(WU1, WZ1, 0), Pt(WU1, WZ1, Dd), Pt(WU1, WZ0, Dd), -T);
				StoneQuad(Pt(WU0, WZ1, 0), Pt(WU1, WZ1, 0), Pt(WU1, WZ1, Dd), Pt(WU0, WZ1, Dd),
					FVector3f(0, 0, -1));
				StoneQuad(Pt(WU0, WZ0, 0), Pt(WU1, WZ0, 0), Pt(WU1, WZ0, Dd), Pt(WU0, WZ0, Dd),
					FVector3f(0, 0, 1));
			};

			const float Margin = 40.f;
			const float Usable = Len - 2.f * Margin;
			// Mode « aucune fenetre » (J3f DA option A/B) : 0 travee -> mur plein par etage.
			const int32 Bays = (!bNoWindows && Usable > 200.f)
				? FMath::Max(1, FMath::RoundToInt32(Usable / 280.f)) : 0;
			// Socle enterre (desktop drape) : mur aveugle sous le rez-de-chaussee.
			if (SocleDepthCm >= 1.f)
			{
				WallQuad(0.f, Len, -SocleDepthCm, 0.f);
			}
			// PoC LiDAR (coque Roofer) : on ne monte QUE le socle enterre ; les murs
			// visibles (0..egout) sont les faces Roofer, soudees au toit -> zero trou.
			// (Le socle reste sous le terrain draine, donc l'ecart d'emprise BD/Roofer
			// au ras du sol est enterre et invisible.)
			if (bAbsWalls)
			{
				continue;
			}
			// Mur marbre LISSE (couture zero) : la subdivision par etage + le tuilage
			// d'atlas ne servent qu'a poser les fenetres et ne produisent que des COUTURES
			// (l'UV redemarre 0->SU/0->SV a chaque etage ET a chaque arete -> rayures
			// verticales+horizontales) ; la MODENATURE J3f (bandeaux saillants) ajoutait
			// des rayures HORIZONTALES a chaque etage. DA « bloc lisse comme le proxy A » :
			// on ignore Bays/travees et on monte TOUJOURS UN seul quad pleine hauteur a UV
			// CONSTANT (centre de la sous-tuile pierre 12), et on SAUTE toute la modenature
			// (bloc bExterior plus bas) -> mur parfaitement lisse, identique au proxy.
			if (bWallsMarble)
			{
				const FVector3f MP[4] = { Pt(0.f, 0.f, 0.f), Pt(Len, 0.f, 0.f),
					Pt(Len, Hcm, 0.f), Pt(0.f, Hcm, 0.f) };
				const FVector2f MC = AtlasUV(WallTile, 0.5f, 0.5f);
				const FVector2f MUV[4] = { MC, MC, MC, MC };
				Wall.AddPoly(Wall.WallGroup, MP, 4, Nout, MUV, Col(Tint, Nout, Hcm * 0.5f));
			}
			else
			{
			float Z0 = 0.f;
			for (int32 F = 0; F < Floors; ++F)
			{
				const float Z1 = Z0 + FloorH;
				if (Bays == 0 || FloorH < 220.f)
				{
					WallQuad(0.f, Len, Z0, Z1);
					Z0 = Z1;
					continue;
				}
				WallQuad(0.f, Margin, Z0, Z1);
				WallQuad(Len - Margin, Len, Z0, Z1);
				const float BayW = Usable / Bays;
				for (int32 B = 0; B < Bays; ++B)
				{
					const float BU0 = Margin + B * BayW;
					const float BU1 = BU0 + BayW;
					const float WinW = BayW * 0.5f;
					const float WinH = FloorH * (F == 0 ? 0.60f : 0.45f);
					const float WU0 = (BU0 + BU1 - WinW) * 0.5f;
					const float WU1 = WU0 + WinW;
					const float WZ0 = F == 0 ? Z0 + 25.f : Z0 + (FloorH - WinH) * 0.5f;
					const float WZ1 = WZ0 + WinH;
					WallQuad(BU0, WU0, Z0, Z1);
					WallQuad(WU1, BU1, Z0, Z1);
					WallQuad(WU0, WU1, Z0, WZ0);
					WallQuad(WU0, WU1, WZ1, Z1);
					// Discretes = niche marbre peu profonde ; sinon vitre en creux.
					if (bDiscreetWindows)
					{
						BlindWindow(WU0, WU1, WZ0, WZ1);
					}
					else
					{
						Window(WU0, WU1, WZ0, WZ1);
					}
				}
				Z0 = Z1;
			}
			}
			// --- Modenature de facade (J3f) : bandeaux de pierre saillants, LISIBLES en
			//     vol sur un batiment blanc. CONTOUR uniquement (bExterior) ; purement
			//     ADDITIF -- ne touche ni au contour, ni au toit, ni aux cours.
			//     Marbre (bWallsMarble) : AUCUNE modenature (socle/bandeaux/corniche/debord)
			//     -> ce sont justement les rayures horizontales que la DA « bloc lisse »
			//     refuse. Le mur marbre reste le seul quad plein pose au-dessus.
			if (bExterior && !bWallsMarble)
			{
				// Socle : plinthe saillante 5 cm sur les ~50 premiers cm au sol.
				Band(25.f, 25.f, 5.f);
				// Bandeaux d'etage : fine bande 3 cm a chaque ligne de plancher
				// intermediaire (dans l'allege, entre les fenetres -> pas de conflit).
				if (Floors >= 2 && FloorH >= 200.f)
				{
					for (int32 F = 1; F < Floors; ++F)
					{
						Band((float)F * FloorH, 4.f, 3.f);
					}
				}
				// Corniche : bandeau saillant 10 cm sous l'egout (au-dessus des fenetres
				// du dernier etage), avec un jour avant le debord.
				if (Hcm > 120.f)
				{
					Band(Hcm - 30.f, 6.f, 10.f);
				}
				// Debord de toiture : l'egout deborde 30 cm (sous-face + chant visibles
				// en vol). Le dessus AFFLEURE l'egout (Zt = Hcm) et prolonge le toit vers
				// l'exterieur -- hors contour, donc aucun recouvrement avec le toit.
				Band(Hcm - 8.f, 8.f, 30.f);
			}
		}

		};
		BuildWallRing(PtsCm, true);
		// Cours : murs interieurs (face tournee vers la cour) montes UNIQUEMENT quand le
		// toit en pente a trous existe ; sinon le toit plat couvre la cour et on garde le
		// batiment plein (repli documente). Sans cour, seul le contour est monte : la
		// geometrie est bit-a-bit identique a l'historique.
		const bool bUseHoles = (Roof != nullptr) && (Holes.Num() > 0);
		if (bUseHoles)
		{
			for (const TArray<FVector2D>& H : Holes)
			{
				BuildWallRing(H, false);
			}
		}

		// N = nombre de sommets du CONTOUR (base de l'espace d'indices du toit).
		const int32 N = PtsCm.Num();
		// Toit : versants du squelette droit si fournis (J3b), sinon plat historique.
		// Quasi blanc en vertex color (la sous-tuile porte la couleur du materiau).
		// J3c : les VERSANTS prennent Roof->Tint (couleur ortho reelle du toit) ; le
		// toit plat garde ce quasi blanc — non-regression mobile intouchable.
		// J3f DA marbre : toit (pente ET plat) force au marbre blanc, sous-tuile pierre (12).
		const FVector3f RoofTint = bMarbleWhite ? MarbleTint() : FVector3f(0.95f, 0.95f, 0.95f);
		if (Roof && Roof->bAbs)
		{
			// PoC LiDAR : coque Roofer ABSOLUE. z deja au-dessus du sol (AbsVerts.Z en cm)
			// -> Z monde = ZBase + Z. Quand la coque fournit ses MURS (AbsWallFaces), ils
			// sont soudes au meme egout que le toit (memes sommets) -> aucun trou ; l'anneau
			// procedural est alors reduit au socle (cf. bAbsWalls). Sinon les murs procd.
			// montent a l'egout plat via WallHcm.
			// --- MURS Roofer (WallSurface) : faces VERTICALES -> la projection XY est
			// degeneree, on triangule dans le PLAN du mur (repere tangente/hauteur). Marbre
			// (sous-tuile 12) si DA marbre OU flag E, sinon tuile mur BD. UV : U le long du
			// mur, V = hauteur monde (texture verticale). Winding force vers la normale
			// sortante (Newell) pour un rendu monoface propre.
			if (bAbsWalls)
			{
				for (const TArray<int32>& WFace : Roof->AbsWallFaces)
				{
					const int32 Nw = WFace.Num();
					if (Nw < 3) { continue; }
					TArray<FVector3f> W;
					W.Reserve(Nw);
					for (const int32 Idx : WFace)
					{
						const FVector3f& V = Roof->AbsVerts[Idx];
						W.Add(FVector3f(V.X, V.Y, ZBaseCm + V.Z));
					}
					// Normale robuste (Newell) sur le polygone 3D.
					FVector3f Newell(0.f, 0.f, 0.f);
					for (int32 i = 0; i < Nw; ++i)
					{
						const FVector3f& A = W[i];
						const FVector3f& B = W[(i + 1) % Nw];
						Newell.X += (A.Y - B.Y) * (A.Z + B.Z);
						Newell.Y += (A.Z - B.Z) * (A.X + B.X);
						Newell.Z += (A.X - B.X) * (A.Y + B.Y);
					}
					const FVector3f WN = Newell.GetSafeNormal();
					if (WN.IsNearlyZero()) { continue; }
					// Repere 2D dans le plan du mur : Tg = tangente horizontale, Vt = "haut".
					FVector3f Tg = FVector3f::CrossProduct(FVector3f(0, 0, 1), WN);
					if (Tg.IsNearlyZero()) { Tg = FVector3f(1, 0, 0); }
					Tg = Tg.GetSafeNormal();
					const FVector3f Vt = FVector3f::CrossProduct(WN, Tg).GetSafeNormal();
					const FVector3f Origin = W[0];
					TArray<FVector2D> Q;
					TArray<int32> Order;
					Q.Reserve(Nw);
					Order.Reserve(Nw);
					for (int32 i = 0; i < Nw; ++i)
					{
						const FVector3f R = W[i] - Origin;
						Q.Add(FVector2D(FVector3f::DotProduct(R, Tg), FVector3f::DotProduct(R, Vt)));
						Order.Add(i);
					}
					// TriangulateRing exige un anneau CCW : inverser Q ET l'index map si CW.
					if (SignedArea(Q) < 0.0)
					{
						Algo::Reverse(Q);
						Algo::Reverse(Order);
					}
					TArray<int32> WTris;
					TriangulateRing(Q, WTris);
					for (int32 t = 0; t + 2 < WTris.Num(); t += 3)
					{
						FVector3f P[3] = { W[Order[WTris[t]]], W[Order[WTris[t + 1]]], W[Order[WTris[t + 2]]] };
						FVector2f TUV[3];
						if (bWallsMarble)
						{
							// Coque Roofer = beaucoup de petites faces -> le tuilage d'atlas
							// multiplierait les coutures. Marbre => UV CONSTANT (centre de la
							// sous-tuile pierre 12) : mur LiDAR lisse, coherent avec le proxy
							// et les murs proceduraux marbre.
							TUV[0] = TUV[1] = TUV[2] = AtlasUV(WallTile, 0.5f, 0.5f);
						}
						else
						{
							for (int32 k = 0; k < 3; ++k)
							{
								const FVector3f R = P[k] - Origin;
								TUV[k] = AtlasUV(WallTile, (float)FVector3f::DotProduct(R, Tg) / 512.f,
									(P[k].Z - ZBaseCm) / 512.f);
							}
						}
						// Winding : la normale du tri doit suivre WN (Newell de la face source).
						FVector3f TN = FVector3f::CrossProduct(P[1] - P[0], P[2] - P[0]);
						if (FVector3f::DotProduct(TN, WN) < 0.f)
						{
							Swap(P[1], P[2]);
							Swap(TUV[1], TUV[2]);
						}
						const float Zc = (P[0].Z + P[1].Z + P[2].Z) / 3.f - ZBaseCm;
						Wall.AddPoly(Wall.WallGroup, P, 3, WN, TUV, Col(Tint, WN, Zc));
						// DOUBLE-FACE (fix trous LiDAR mur<->toit) : le winding des faces
						// WallSurface Roofer est INCOHERENT dans la donnee source (mesure :
						// 65 % des faces "wf" ont leur normale Newell tournee vers l'INTERIEUR
						// du batiment). Rendues monoface elles sont back-face-culled -> le mur
						// disparait de l'exterieur = les trous vus au rendu, alors que la
						// donnee est watertight. On emet donc la face MIROIR (winding inverse
						// + normale opposee) : la coque est visible des DEUX cotes, quel que
						// soit le winding source. Cout : x2 tris de mur sur ~186 bat LiDAR
						// (negligeable) ; depuis l'exterieur seule la face tournee vers la
						// camera est dessinee (materiau monoface) -> eclairage correct, zero
						// z-fighting.
						FVector3f Pb[3] = { P[0], P[2], P[1] };
						FVector2f Ub[3] = { TUV[0], TUV[2], TUV[1] };
						Wall.AddPoly(Wall.WallGroup, Pb, 3, -WN, Ub, Col(Tint, -WN, Zc));
					}
				}
			}
			const int32 AbsTile = bMarbleWhite ? 12 : Roof->Tile;
			const FVector3f AbsTint = bMarbleWhite ? MarbleTint() : Roof->Tint;
			for (const TArray<int32>& Face : Roof->AbsFaces)
			{
				const int32 Nf = Face.Num();
				TArray<FVector3f> C;
				TArray<FVector2D> C2;
				C.Reserve(Nf);
				C2.Reserve(Nf);
				for (const int32 Idx : Face)
				{
					const FVector3f& V = Roof->AbsVerts[Idx];
					C.Add(FVector3f(V.X, V.Y, ZBaseCm + V.Z));
					C2.Add(FVector2D(V.X, V.Y));
				}
				FVector3f Nrm = FVector3f::CrossProduct(C[1] - C[0], C[2] - C[0]).GetSafeNormal();
				if (Nrm.Z < 0.f) { Nrm = -Nrm; }
				if (Nrm.IsNearlyZero()) { Nrm = FVector3f(0, 0, 1); }
				FBox2D Box(ForceInit);
				for (const FVector2D& P : C2) { Box += P; }
				const FVector2D Sz = Box.GetSize();
				TArray<FVector2f> UV;
				UV.Reserve(Nf);
				for (const FVector2D& P : C2)
				{
					UV.Add(AtlasUV(AbsTile,
						(float)((P.X - Box.Min.X) / FMath::Max(Sz.X, 1.0)),
						(float)((P.Y - Box.Min.Y) / FMath::Max(Sz.Y, 1.0))));
				}
				TArray<int32> Tris;
				TriangulateRing(C2, Tris);
				for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
				{
					const FVector3f P[3] = { C[Tris[t]], C[Tris[t + 1]], C[Tris[t + 2]] };
					const FVector2f TUV[3] = { UV[Tris[t]], UV[Tris[t + 1]], UV[Tris[t + 2]] };
					Wall.AddPoly(Wall.WallGroup, P, 3, Nrm, TUV, Col(AbsTint, Nrm, Hcm));
				}
			}
			return;
		}
		if (Roof)
		{
			// Ici Hcm = hauteur d'EGOUT (les murs s'arretent a l'egout), le versant
			// monte de delta * d / maxd. UV0 : U le long de l'arete d'egout (1re
			// arete du versant, contrat du prep), V le long du rampant (longueur
			// reelle) — les rangees de tuiles restent paralleles a l'egout.
			const float SlopeLen = FMath::Sqrt(1.f + FMath::Square(Roof->DeltaCm / Roof->MaxDcm));
			// J3f DA marbre : versants blancs sur pierre claire (12), teinte ortho ignoree.
			const int32 RoofTile = bMarbleWhite ? 12 : Roof->Tile;
			const FVector3f RoofFaceTint = bMarbleWhite ? MarbleTint() : Roof->Tint;
			// Espace de sommets du toit = contour(N) ++ trous(Holes, a plat) ++ squelette.
			// C'est le contrat EXACT du prep (j3b_prep_toits.py) : le C++ reconstruit ici
			// le meme espace pour poser les faces. Sans cour (Holes vide), Idx>=N tombe
			// directement sur le squelette -> mapping identique a l'historique.
			auto RoofVert = [&](int32 Idx, FVector2D& OutXY, float& OutD)
			{
				if (Idx < N) { OutXY = PtsCm[Idx]; OutD = 0.f; return; }
				int32 j = Idx - N;
				for (const TArray<FVector2D>& H : Holes)
				{
					if (j < H.Num()) { OutXY = H[j]; OutD = 0.f; return; }
					j -= H.Num();
				}
				OutXY = FVector2D(Roof->Skel[j].X, Roof->Skel[j].Y);
				OutD = Roof->Skel[j].Z;
			};
			for (const TArray<int32>& Face : Roof->Faces)
			{
				const int32 Nf = Face.Num();
				TArray<FVector3f> C;
				TArray<FVector2D> C2;
				TArray<float> D;
				C.Reserve(Nf);
				C2.Reserve(Nf);
				D.Reserve(Nf);
				for (const int32 Idx : Face)
				{
					FVector2D XY;
					float d;
					RoofVert(Idx, XY, d);
					C.Add(FVector3f((float)XY.X, (float)XY.Y,
						ZBaseCm + Hcm + Roof->DeltaCm * d / Roof->MaxDcm));
					C2.Add(XY);
					D.Add(d);
				}
				const FVector2D EaveDir = (C2[1] - C2[0]).GetSafeNormal();
				float U0 = FLT_MAX, U1 = -FLT_MAX, VMax = 1.f;
				TArray<FVector2f> UV;
				UV.Reserve(Nf);
				for (int32 i = 0; i < Nf; ++i)
				{
					const float U = (float)FVector2D::DotProduct(C2[i] - C2[0], EaveDir);
					U0 = FMath::Min(U0, U);
					U1 = FMath::Max(U1, U);
					VMax = FMath::Max(VMax, D[i] * SlopeLen);
					UV.Add(FVector2f(U, D[i] * SlopeLen));
				}
				const float USpan = FMath::Max(U1 - U0, 1.f);
				for (FVector2f& T : UV)
				{
					T = AtlasUV(RoofTile, (T.X - U0) / USpan, T.Y / VMax);
				}
				// Normale vraie du versant (Z force vers le haut). Tris par
				// ear-clipping de la projection XY : un toit est un champ de hauteur,
				// la projection d'un versant est donc un polygone simple (et le fan
				// des n-gons ne gere pas les versants non convexes).
				FVector3f Nrm = FVector3f::CrossProduct(C[1] - C[0], C[2] - C[0]).GetSafeNormal();
				if (Nrm.Z < 0.f)
				{
					Nrm = -Nrm;
				}
				if (Nrm.IsNearlyZero())
				{
					Nrm = FVector3f(0, 0, 1);
				}
				TArray<int32> Tris;
				TriangulateRing(C2, Tris);
				for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
				{
					const FVector3f P[3] = { C[Tris[t]], C[Tris[t + 1]], C[Tris[t + 2]] };
					const FVector2f TUV[3] = { UV[Tris[t]], UV[Tris[t + 1]], UV[Tris[t + 2]] };
					// J3c : teinte ortho du toit (Roof->Tint vaut RoofTint si absente).
					// J3f DA marbre : RoofFaceTint = MarbleTint quand bMarbleWhite.
					Wall.AddPoly(Wall.WallGroup, P, 3, Nrm, TUV, Col(RoofFaceTint, Nrm, Hcm));
				}
			}
			return;
		}
		// Toit plat : sous-tuile toit terre cuite (10), UV0 = emprise normalisee.
		// J3f DA marbre : pierre claire (12) a la place de la terre cuite.
		const int32 FlatRoofTile = bMarbleWhite ? 12 : 10;
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		FBox2D RoofBox(ForceInit);
		for (const FVector2D& P : PtsCm)
		{
			RoofBox += P;
		}
		const FVector2D RoofSize = RoofBox.GetSize();
		const FVector3f Up(0, 0, 1);
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			FVector3f R[3];
			FVector2f RUV[3];
			for (int32 i = 0; i < 3; ++i)
			{
				const FVector2D& P2 = PtsCm[Tris[t + i]];
				R[i] = FVector3f(P2.X, P2.Y, ZBaseCm + Hcm);
				RUV[i] = AtlasUV(FlatRoofTile,
					(float)((P2.X - RoofBox.Min.X) / FMath::Max(RoofSize.X, 1.0)),
					(float)((P2.Y - RoofBox.Min.Y) / FMath::Max(RoofSize.Y, 1.0)));
			}
			Wall.AddPoly(Wall.WallGroup, R, 3, Up, RUV, Col(RoofTint, Up, Hcm));
		}
	}

	// Batiment a facades TEXTUREES : un seul quad par mur, la grille de fenetres est
	// dans la texture (UV = travees x etages). ~x8-10 moins de triangles que la
	// version geometrique — budget Adreno 512 ~1,5 M tris/image. Les toits vont sur
	// le slot Glass (materiau uni) pour ne pas recevoir la texture de fenetres.
	// Desktop (J2 §3.3) : ZBaseCm pose le rez-de-chaussee a MinAlt du terrain sous
	// l'emprise (rebase Capitole) et SocleDepthCm prolonge le mur en socle aveugle
	// enterre jusqu'a ZBase - (MaxAlt - MinAlt) - SocleCm : aucun coin ne flotte en
	// pente. Mobile : 0/0, geometrie bit-a-bit identique a l'historique.
	void BuildPolygonBuildingTextured(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm,
		float Hcm, const FVector3f& Tint, float ZBaseCm = 0.f, float SocleDepthCm = 0.f)
	{
		const int32 Floors = FMath::Clamp(FMath::RoundToInt32(Hcm / 290.f), 1, 40);
		const float FloorH = Hcm / Floors;
		const int32 N = PtsCm.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = PtsCm[e];
			const FVector2D B2 = PtsCm[(e + 1) % N];
			const FVector2D Dir2 = B2 - A2;
			const float Len = Dir2.Size();
			if (Len < 30.f)
			{
				continue;
			}
			const FVector2D T2 = Dir2 / Len;
			const FVector3f Nout(T2.Y, -T2.X, 0.f);
			const float Usable = Len - 80.f;
			const int32 Bays = (Usable > 200.f && FloorH >= 220.f)
				? FMath::Max(1, FMath::RoundToInt32(Usable / 280.f)) : 0;
			const FVector3f P[4] = {
				FVector3f(A2.X, A2.Y, ZBaseCm), FVector3f(B2.X, B2.Y, ZBaseCm),
				FVector3f(B2.X, B2.Y, ZBaseCm + Hcm), FVector3f(A2.X, A2.Y, ZBaseCm + Hcm) };
			// Mur aveugle : UV constante dans un coin 100 % mur de la tuile.
			FVector2f UV[4] = { FVector2f(0.03f, 0.03f), FVector2f(0.03f, 0.03f),
				FVector2f(0.03f, 0.03f), FVector2f(0.03f, 0.03f) };
			if (Bays > 0)
			{
				UV[0] = FVector2f(0, 0);
				UV[1] = FVector2f(Bays, 0);
				UV[2] = FVector2f(Bays, Floors);
				UV[3] = FVector2f(0, Floors);
			}
			QM.AddPoly(QM.WallGroup, P, 4, Nout, UV, Shade(Tint, Nout, Hcm * 0.5f));
			// Socle enterre (desktop) : mur aveugle sous le rez-de-chaussee.
			if (SocleDepthCm >= 1.f)
			{
				const FVector3f S[4] = {
					FVector3f(A2.X, A2.Y, ZBaseCm - SocleDepthCm), FVector3f(B2.X, B2.Y, ZBaseCm - SocleDepthCm),
					FVector3f(B2.X, B2.Y, ZBaseCm), FVector3f(A2.X, A2.Y, ZBaseCm) };
				const FVector2f SocleUV[4] = { FVector2f(0.03f, 0.03f), FVector2f(0.03f, 0.03f),
					FVector2f(0.03f, 0.03f), FVector2f(0.03f, 0.03f) };
				QM.AddPoly(QM.WallGroup, S, 4, Nout, SocleUV, Shade(Tint * 0.85f, Nout, 0.f));
			}
		}
		// Toit plat : slot Glass = materiau uni (M_BldgWall), pas de texture fenetres.
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		const FVector3f RoofShaded = Shade(FVector3f(0.42f, 0.40f, 0.38f), FVector3f(0, 0, 1), Hcm);
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			const FVector3f R[3] = {
				FVector3f(PtsCm[Tris[t]].X, PtsCm[Tris[t]].Y, ZBaseCm + Hcm),
				FVector3f(PtsCm[Tris[t + 1]].X, PtsCm[Tris[t + 1]].Y, ZBaseCm + Hcm),
				FVector3f(PtsCm[Tris[t + 2]].X, PtsCm[Tris[t + 2]].Y, ZBaseCm + Hcm) };
			const FVector2f RUV[3] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1) };
			QM.AddPoly(QM.GlassGroup, R, 3, FVector3f(0, 0, 1), RUV, RoofShaded);
		}
	}

	// Boite proxy : contour simplifie (>= 3 m entre points, plafond 12) et retracte de
	// 2 m vers le centroide, murs pleins + toit plat abaisse de 2,5 m. La version
	// detaillee du batiment recouvre le proxy -> il disparait dedans quand le bloc de
	// detail est charge. Retrait 30 cm au depart : z-fight VISIBLE sur device (toits
	// qui clignotent vus d'altitude a 1-2 km, la precision depth ne separe plus 30 cm).
	// ZBaseCm / SocleDepthCm : meme pose desktop que le batiment detaille (le proxy
	// doit rester CONTENU dans le detail pour disparaitre dedans) ; 0/0 en mobile.
	// bBakedShade=false (desktop PBR) : teinte brute, Lumen eclaire (cf. BuildRoad).
	void BuildProxyBuilding(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float Hcm,
		const FVector3f& Tint, float ZBaseCm = 0.f, float SocleDepthCm = 0.f,
		bool bBakedShade = true, bool bWhite = false)
	{
		// RECTANGLE ORIENTE (10 tris par batiment contre ~28 pour le contour simplifie,
		// « tout garder, moins cher ») : axe = plus longue arete de l'emprise,
		// projection min/max sur (axe, perpendiculaire), puis retrait de 2 m.
		if (PtsCm.Num() < 3)
		{
			return;
		}
		FVector2D Axis(1, 0);
		float BestLen = 0.f;
		for (int32 i = 0; i < PtsCm.Num(); ++i)
		{
			const FVector2D E = PtsCm[(i + 1) % PtsCm.Num()] - PtsCm[i];
			const float L = E.SquaredLength();
			if (L > BestLen)
			{
				BestLen = L;
				Axis = E.GetSafeNormal();
			}
		}
		const FVector2D Perp(-Axis.Y, Axis.X);
		float MinU = FLT_MAX, MaxU = -FLT_MAX, MinV = FLT_MAX, MaxV = -FLT_MAX;
		for (const FVector2D& Q : PtsCm)
		{
			const float U = FVector2D::DotProduct(Q, Axis);
			const float V = FVector2D::DotProduct(Q, Perp);
			MinU = FMath::Min(MinU, U); MaxU = FMath::Max(MaxU, U);
			MinV = FMath::Min(MinV, V); MaxV = FMath::Max(MaxV, V);
		}
		TArray<FVector2D> P;
		const float BoxArea = (MaxU - MinU) * (MaxV - MinV);
		const float FootprintArea = FMath::Abs(float(SignedArea(PtsCm)));
		if (BoxArea < 1.f || FootprintArea / BoxArea >= 0.68f)
		{
			// Emprise compacte : rectangle oriente, retrait 2 m par cote (plafonne au
			// tiers de la dimension pour les petites emprises).
			const float InsetU = FMath::Min(200.f, (MaxU - MinU) / 3.f);
			const float InsetV = FMath::Min(200.f, (MaxV - MinV) / 3.f);
			P.Add(Axis * (MinU + InsetU) + Perp * (MinV + InsetV));
			P.Add(Axis * (MaxU - InsetU) + Perp * (MinV + InsetV));
			P.Add(Axis * (MaxU - InsetU) + Perp * (MaxV - InsetV));
			P.Add(Axis * (MinU + InsetU) + Perp * (MaxV - InsetV));
		}
		else
		{
			// Emprise concave/composite (< 68 % de sa boite) : le rectangle creait des
			// DALLES GEANTES qui debordaient et clignotaient contre le detail (vu en
			// v12) -> contour simplifie (>= 3 m, plafond 12 pts) retracte de 2 m.
			for (const FVector2D& Pt : PtsCm)
			{
				if (P.Num() == 0 || FVector2D::Distance(P.Last(), Pt) >= 300.f)
				{
					P.Add(Pt);
				}
			}
			if (P.Num() > 12)
			{
				TArray<FVector2D> Thin;
				const int32 Step = FMath::DivideAndRoundUp(P.Num(), 12);
				for (int32 i = 0; i < P.Num(); i += Step)
				{
					Thin.Add(P[i]);
				}
				P = MoveTemp(Thin);
			}
			if (P.Num() < 3)
			{
				P = PtsCm;
			}
			FVector2D Ctr(0, 0);
			for (const FVector2D& Q : P) { Ctr += Q; }
			Ctr /= P.Num();
			for (FVector2D& Q : P)
			{
				const FVector2D D = Ctr - Q;
				const float L = D.Size();
				if (L > 60.f)
				{
					Q += D / L * FMath::Min(200.f, L * 0.5f);
				}
			}
		}
		if (SignedArea(P) < 0)
		{
			Algo::Reverse(P);
		}
		const float H = FMath::Max(Hcm - 250.f, 100.f);
		const float Z0 = ZBaseCm - SocleDepthCm;
		const float Z1 = ZBaseCm + H;
		const int32 N = P.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = P[e], B2 = P[(e + 1) % N];
			const FVector2D Dir2 = B2 - A2;
			const float Len = Dir2.Size();
			if (Len < 30.f)
			{
				continue;
			}
			const FVector2D T2 = Dir2 / Len;
			const FVector3f Nout(T2.Y, -T2.X, 0.f);
			QM.AddQuad(QM.WallGroup, FVector3f(A2.X, A2.Y, Z0), FVector3f(B2.X, B2.Y, Z0),
				FVector3f(B2.X, B2.Y, Z1), FVector3f(A2.X, A2.Y, Z1), Nout,
				bBakedShade ? Shade(Tint, Nout, H * 0.5f) : Tint);
		}
		TArray<int32> Tris;
		TriangulateRing(P, Tris);
		// J3f DA marbre : toit plat du proxy BLANC comme les murs (A = boite marbre nue).
		const FVector3f RoofBase = bWhite ? MarbleTint() : FVector3f(0.42f, 0.40f, 0.38f);
		const FVector3f RoofShaded = bBakedShade ? Shade(RoofBase, FVector3f(0, 0, 1), H) : RoofBase;
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			QM.AddTri(QM.WallGroup,
				FVector3f(P[Tris[t]].X, P[Tris[t]].Y, Z1),
				FVector3f(P[Tris[t + 1]].X, P[Tris[t + 1]].Y, Z1),
				FVector3f(P[Tris[t + 2]].X, P[Tris[t + 2]].Y, Z1),
				FVector3f(0, 0, 1), RoofShaded);
		}
	}

	// Prisme de collision FERME d'un batiment (Verrou 2) : emprise triangulee en
	// plancher ET toit + un quad lateral par arete, aux Z EXACTS de la pose Lot A
	// (toit = ZBase + h, pied = ZBase - socle). ~30 tris par batiment. Raison
	// d'etre : les murs Nanite ne doivent JAMAIS servir de collision — leur
	// fallback decime (~0,1 %) laisse les facades traversables (sonde 2026-07-25).
	// Collision pure : ni teinte utile, ni UV, jamais rendu (pattern _Col du sol).
	// Holes / Roof (cours J3b) : quand un batiment a un toit en pente a trous, la
	// collision devient un PUITS — murs interieurs de cour en plus, et planchers/toits
	// AJOURES (tessellation du toit = contour ++ trous ++ squelette, mise a plat) au lieu
	// de l'emprise pleine, pour que le sol de la cour reste ouvert (pas de "beton"). Sans
	// cour (Holes vide OU Roof nul), on garde EXACTEMENT le prisme plein historique.
	void BuildCollisionPrism(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm,
		float TopZCm, float BottomZCm,
		const TArray<TArray<FVector2D>>& Holes = TArray<TArray<FVector2D>>(),
		const FRoofData* Roof = nullptr)
	{
		const FVector3f White(1.f, 1.f, 1.f);
		// Un anneau de murs lateraux (contour ou cour). Meme quad des deux cotes de la
		// convention : la collision est double-face, l'orientation de la normale n'importe pas.
		auto SideWalls = [&](const TArray<FVector2D>& Ring)
		{
			const int32 M = Ring.Num();
			for (int32 e = 0; e < M; ++e)
			{
				const FVector2D A2 = Ring[e];
				const FVector2D B2 = Ring[(e + 1) % M];
				if ((B2 - A2).Size() < 1.0)
				{
					continue;
				}
				const FVector2D T2 = (B2 - A2).GetSafeNormal();
				const FVector3f Nout(T2.Y, -T2.X, 0.f);
				QM.AddQuad(QM.WallGroup,
					FVector3f(A2.X, A2.Y, BottomZCm), FVector3f(B2.X, B2.Y, BottomZCm),
					FVector3f(B2.X, B2.Y, TopZCm), FVector3f(A2.X, A2.Y, TopZCm), Nout, White);
			}
		};
		SideWalls(PtsCm);
		const bool bUseHoles = (Roof != nullptr) && (Holes.Num() > 0);
		if (bUseHoles)
		{
			const int32 N = PtsCm.Num();
			for (const TArray<FVector2D>& H : Holes)
			{
				SideWalls(H);
			}
			// Espace de sommets = contour ++ trous ++ squelette (identique au toit).
			auto CapVert = [&](int32 Idx) -> FVector2D
			{
				if (Idx < N) { return PtsCm[Idx]; }
				int32 j = Idx - N;
				for (const TArray<FVector2D>& H : Holes)
				{
					if (j < H.Num()) { return H[j]; }
					j -= H.Num();
				}
				return FVector2D(Roof->Skel[j].X, Roof->Skel[j].Y);
			};
			// Planchers ajoures : chaque versant (polygone simple) triangule par ear-clip
			// (comme le rendu du toit) puis pose a plat, en haut ET en bas.
			for (const TArray<int32>& Face : Roof->Faces)
			{
				TArray<FVector2D> C2;
				C2.Reserve(Face.Num());
				for (const int32 Idx : Face)
				{
					C2.Add(CapVert(Idx));
				}
				TArray<int32> Tris;
				TriangulateRing(C2, Tris);
				for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
				{
					const FVector2D& P0 = C2[Tris[t]];
					const FVector2D& P1 = C2[Tris[t + 1]];
					const FVector2D& P2 = C2[Tris[t + 2]];
					QM.AddTri(QM.WallGroup, FVector3f(P0.X, P0.Y, TopZCm), FVector3f(P1.X, P1.Y, TopZCm),
						FVector3f(P2.X, P2.Y, TopZCm), FVector3f(0, 0, 1), White);
					QM.AddTri(QM.WallGroup, FVector3f(P0.X, P0.Y, BottomZCm), FVector3f(P1.X, P1.Y, BottomZCm),
						FVector3f(P2.X, P2.Y, BottomZCm), FVector3f(0, 0, -1), White);
				}
			}
			return;
		}
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			const FVector2D& P0 = PtsCm[Tris[t]];
			const FVector2D& P1 = PtsCm[Tris[t + 1]];
			const FVector2D& P2 = PtsCm[Tris[t + 2]];
			QM.AddTri(QM.WallGroup, FVector3f(P0.X, P0.Y, TopZCm), FVector3f(P1.X, P1.Y, TopZCm),
				FVector3f(P2.X, P2.Y, TopZCm), FVector3f(0, 0, 1), White);
			QM.AddTri(QM.WallGroup, FVector3f(P0.X, P0.Y, BottomZCm), FVector3f(P1.X, P1.Y, BottomZCm),
				FVector3f(P2.X, P2.Y, BottomZCm), FVector3f(0, 0, -1), White);
		}
	}

	// Coeur commun des deux outils de collision batiments : parse le JSON UNE fois,
	// construit les prismes par cellule (memes conventions que ImportCityStreamed :
	// pts x100, CCW, cellule du centroide, ZBase/socle drapes), cree chaque
	// SM_Bldg_<x>_<y>_Col (complex-as-simple, sans Nanite, materiau par defaut),
	// le cable en ComplexCollisionMesh du SM_Bldg_<x>_<y>_Wall s'il existe, et
	// sauvegarde les packages AU FIL DE L'EAU (un kill ne perd que la cellule en
	// cours). OnlyCell non nul = une seule cellule.
	bool GenerateBuildingCollisionForCells(const FString& JsonFilePath, const FString& AssetFolder,
		float CellSizeM, const FCityGenProfile& Profile, const FIntPoint* OnlyCell,
		TMap<FIntPoint, FCityBldgColSummary>& OutCells)
	{
		if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
		{
			RaiseError(TEXT("AssetFolder must be a package path such as /Game/City."));
			return false;
		}
		if (CellSizeM < 20.f)
		{
			RaiseError(TEXT("CellSizeM must be >= 20."));
			return false;
		}
		const FCityGenProfile Gen = Profile.Resolved();
		FDrapeContext Drape;
		if (!MakeDrapeContext(Gen, Drape))
		{
			return false;
		}
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
		{
			RaiseError(FString::Printf(TEXT("Cannot read district file '%s'."), *JsonFilePath));
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			RaiseError(TEXT("District file is not valid JSON."));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* BuildingsJson = nullptr;
		if (!Root->TryGetArrayField(TEXT("buildings"), BuildingsJson) || BuildingsJson->Num() == 0)
		{
			RaiseError(TEXT("District file has no 'buildings' array."));
			return false;
		}

		const float Cell = CellSizeM * 100.f;
		TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>> ColCells;
		TMap<FIntPoint, int32> CellBuildings;
		for (const TSharedPtr<FJsonValue>& V : *BuildingsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			for (const TSharedPtr<FJsonValue>& PV : O->GetArrayField(TEXT("pts")))
			{
				const TArray<TSharedPtr<FJsonValue>>& C = PV->AsArray();
				if (C.Num() >= 2)
				{
					Pts.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
				}
			}
			if (Pts.Num() < 3)
			{
				continue;
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			FVector2D Centroid(0, 0);
			for (const FVector2D& P : Pts) { Centroid += P; }
			Centroid /= Pts.Num();
			const FIntPoint Key(FMath::FloorToInt(Centroid.X / Cell), FMath::FloorToInt(Centroid.Y / Cell));
			if (OnlyCell && Key != *OnlyCell)
			{
				continue;
			}
			const float Hcm = O->GetNumberField(TEXT("h")) * 100.f;
			float ZBase = 0.f, SocleDepth = 0.f;
			if (Drape.IsActive())
			{
				const float MinAlt = Drape.Sampler->MinAltCmInPolygon(Pts);
				const float MaxAlt = Drape.Sampler->MaxAltCmInPolygon(Pts);
				ZBase = MinAlt - Drape.AltCapCm;
				SocleDepth = (MaxAlt - MinAlt) + Gen.SocleCm;
			}
			TUniquePtr<FCityMeshBuilder>& Builder = ColCells.FindOrAdd(Key);
			if (!Builder)
			{
				Builder = MakeUnique<FCityMeshBuilder>();
			}
			BuildCollisionPrism(*Builder, Pts, ZBase + Hcm, ZBase - SocleDepth);
			++CellBuildings.FindOrAdd(Key);
		}
		if (ColCells.Num() == 0)
		{
			RaiseError(OnlyCell
				? FString::Printf(TEXT("No building centroid in cell (%d, %d)."), OnlyCell->X, OnlyCell->Y)
				: TEXT("No usable building footprint in the district file."));
			return false;
		}

		UMaterialInterface* DefaultMat = UMaterial::GetDefaultMaterial(MD_Surface);
		int32 Done = 0;
		for (auto& Pair : ColCells)
		{
			const FString Name = FString::Printf(TEXT("SM_Bldg_%d_%d"), Pair.Key.X, Pair.Key.Y);
			UStaticMesh* ColMesh = CreateMeshAsset(AssetFolder / (Name + TEXT("_Col")), *Pair.Value,
				DefaultMat, DefaultMat, true);
			// Attendre la compilation du _Col avant cablage/sauvegarde (meme piege que
			// le sol : le cook physique lit sa SectionInfoMap depuis un worker).
			FStaticMeshCompilingManager::Get().FinishCompilation(
				TArrayView<UStaticMesh* const>(&ColMesh, 1));
			FCityBldgColSummary& Out = OutCells.FindOrAdd(Pair.Key);
			Out.Buildings = CellBuildings[Pair.Key];
			const FMeshDescription* ColDesc = ColMesh->GetMeshDescription(0);
			Out.Triangles = ColDesc ? ColDesc->Triangles().Num() : 0;

			TArray<UPackage*> ToSave = { ColMesh->GetPackage() };
			const FString WallName = Name + TEXT("_Wall");
			UStaticMesh* Wall = LoadObject<UStaticMesh>(nullptr,
				*(AssetFolder / WallName + TEXT(".") + WallName), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Wall)
			{
				Wall->Modify();
				Wall->ComplexCollisionMesh = ColMesh;
				if (!Wall->GetBodySetup())
				{
					Wall->CreateBodySetup();
				}
				Wall->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
				// GUID physique regenere : le trimesh cuit (DDC) doit repartir du _Col,
				// pas du fallback Nanite decime. Pas de Build() complet : la geometrie de
				// rendu du Wall ne change pas, seule la source de collision change.
				Wall->GetBodySetup()->InvalidatePhysicsData();
				Wall->MarkPackageDirty();
				ToSave.Add(Wall->GetPackage());
				Out.bWallWired = true;
			}
			// Sauvegarde FORCEE (bOnlyDirty=false) au fil de l'eau.
			Out.bSaved = UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty=*/false);
			++Done;
			UE_LOG(LogCityImport, Display,
				TEXT("Collision batiments %d/%d : %s_Col — %d batiments, %d tris, wall %s, save %s"),
				Done, ColCells.Num(), *Name, Out.Buildings, Out.Triangles,
				Out.bWallWired ? TEXT("cable") : TEXT("ABSENT"), Out.bSaved ? TEXT("OK") : TEXT("ECHEC"));
		}
		return true;
	}
}

FCityStreamedSummary UCityImportTools::ImportCityStreamed(const FString& JsonFilePath,
	const FString& SurfacesJsonFilePath, const FString& AssetFolder, const FString& BlocksFolder,
	const FString& WallMaterialPath, const FString& GlassMaterialPath, float CellSizeM,
	float BlockSizeM, float ProxyCellSizeM, FVector Location, const FCityGenProfile& Profile)
{
	FCityStreamedSummary Summary;

	// Profil effectif + MNT charge UNE fois pour tout l'import (jalon J2).
	const FCityGenProfile Gen = Profile.Resolved();
	FDrapeContext Drape;
	if (!MakeDrapeContext(Gen, Drape))
	{
		return Summary;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read district file '%s'."), *JsonFilePath));
		return Summary;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("District file is not valid JSON."));
		return Summary;
	}
	// J3b : source batiments dediee (anneaux nettoyes CCW + toits precalcules du
	// prep). Routes, arbres et surfaces restent dans le JSON principal.
	TSharedPtr<FJsonObject> BldRoot = Root;
	if (!Gen.BuildingsJsonPath.IsEmpty())
	{
		FString BldJson;
		if (!FFileHelper::LoadFileToString(BldJson, *Gen.BuildingsJsonPath))
		{
			RaiseError(FString::Printf(TEXT("Cannot read buildings file '%s'."), *Gen.BuildingsJsonPath));
			return Summary;
		}
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BldJson), BldRoot) || !BldRoot.IsValid())
		{
			RaiseError(TEXT("Buildings file is not valid JSON."));
			return Summary;
		}
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")) ||
		BlocksFolder.IsEmpty() || !BlocksFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder and BlocksFolder must be package paths such as /Game/City."));
		return Summary;
	}
	if (CellSizeM < 20.f || ProxyCellSizeM < 20.f || BlockSizeM < CellSizeM)
	{
		RaiseError(TEXT("Cell sizes must be >= 20 and BlockSizeM >= CellSizeM."));
		return Summary;
	}
	UMaterialInterface* WallMat = LoadMaterialOrDefault(WallMaterialPath);
	UMaterialInterface* GlassMat = LoadMaterialOrDefault(GlassMaterialPath);
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!WallMat || !GlassMat || !World)
	{
		if (!World)
		{
			RaiseError(TEXT("No editor world is loaded."));
		}
		return Summary;
	}
	UEditorLevelUtils::MakeLevelCurrent(World->PersistentLevel, true);

	// --- LOT VELOCITE : MODE DISTRICT -------------------------------------------
	// CellFilter vide = tout le code qui suit est celui d'avant, a l'octet pres.
	TSet<FIntPoint> CellSet;
	const bool bCellFilter = ParseCellFilter(Gen.CellFilter, CellSet);
	const float FilterCellM = (Gen.CellFilterSizeM > 0.f) ? Gen.CellFilterSizeM : CellSizeM;
	if (bCellFilter && !FMath::IsNearlyEqual(FilterCellM, CellSizeM))
	{
		// Un filtre exprime dans une AUTRE maille designerait d'autres cellules que
		// celles que cette passe fabrique : on refuse plutot que de purger a cote.
		RaiseError(FString::Printf(
			TEXT("CellFilter est exprime pour des cellules de %.0f m et cet import travaille a %.0f m."),
			FilterCellM, CellSizeM));
		return Summary;
	}
	// Une cellule est « visee » quand elle est dans le filtre — ou quand il n'y a pas
	// de filtre du tout (la ville entiere).
	auto CelluleVisee = [&CellSet, bCellFilter](const FIntPoint& K)
	{
		return !bCellFilter || CellSet.Contains(K);
	};
	auto CelluleDe = [](const FVector2D& P, float SizeCm)
	{
		return FIntPoint(FMath::FloorToInt(P.X / SizeCm), FMath::FloorToInt(P.Y / SizeCm));
	};
	const bool bProxyLayer = Gen.bProxyLayer && !bCellFilter;
	if (bCellFilter && Gen.bProxyLayer)
	{
		UE_LOG(LogCityImport, Display,
			TEXT("Mode district : bProxyLayer ignore — la couche proxy a sa propre maille (%.0f m)."),
			ProxyCellSizeM);
	}
	if (bCellFilter)
	{
		UE_LOG(LogCityImport, Display,
			TEXT("MODE DISTRICT : %d cellule(s) de %.0f m regenerees, le reste de la ville n'est pas touche."),
			CellSet.Num(), CellSizeM);
	}

	// Lot B : bascule matiere desktop. Le chemin batiments GEOMETRIQUES est pris des
	// qu'un flag Lot B batiments est actif ; bPBRMaterials commande les materiaux
	// DefaultLit, l'encodage LINEAIRE des vertex colors, la fin de l'ombrage cuit
	// Shade() (Lumen eclaire) et l'UV1 monde (sol, routes, toits — ortho-ready J3).
	const bool bDesktopBldg = Gen.bWindowReveals || Gen.bSplitWallGlass || Gen.bPBRMaterials;
	const bool bLinearColors = Gen.bPBRMaterials;
	const bool bBakedShade = !Gen.bPBRMaterials;
	const bool bWorldUVs = Gen.bPBRMaterials;
	const bool bNanite = Gen.bNanite;

	// J3b : sous-niveaux de blocs VISIBLES en editeur AVANT la purge — DestroyActor
	// echoue en silence sur un niveau invisible (constate sur le proto : generations
	// empilees 8 -> 16), et un niveau sauve invisible rend la ville « proxys seuls »
	// dans l'editeur CityLab (3 sessions de diagnostic payees le 25/07).
	// Mode district : SEULS les blocs qui portent une cellule visee sont rendus
	// visibles (donc charges, puis re-sauves plus bas). C'est la moitie de l'economie
	// de la passe filtree : a 3x3 km, 16 sous-niveaux de 1 km rendus visibles puis
	// resauves coutent bien plus cher que la geometrie d'un quartier.
	TSet<FIntPoint> BlocsVises;
	if (bCellFilter)
	{
		for (const FIntPoint& K : CellSet)
		{
			BlocsVises.Add(FIntPoint(
				FMath::FloorToInt(K.X * CellSizeM / BlockSizeM),
				FMath::FloorToInt(K.Y * CellSizeM / BlockSizeM)));
		}
	}
	auto BlocVise = [&](const FString& PackageName) -> bool
	{
		if (!bCellFilter)
		{
			return true;
		}
		FIntPoint B;
		return CellFromLabel(FPackageName::GetShortName(PackageName), TEXT("L_T10_B_"), B)
			&& BlocsVises.Contains(B);
	};
	for (ULevelStreaming* S : World->GetStreamingLevels())
	{
		if (S && S->GetWorldAssetPackageName().StartsWith(BlocksFolder) &&
			BlocVise(S->GetWorldAssetPackageName()))
		{
			S->SetShouldBeVisibleInEditor(true);
			if (ULevel* Lvl = S->GetLoadedLevel())
			{
				UEditorLevelUtils::SetLevelVisibility(Lvl, true, false);
			}
		}
	}

	// Idempotence : couches precedentes + heritage monolithique SM_City_*.
	// EN MODE DISTRICT la purge est BORNEE aux cellules visees — c'est le point le
	// plus dangereux du filtre : une purge restee globale effacerait la ville et la
	// passe ne reconstruirait que le quartier. Un acteur dont le label ne rend pas de
	// cellule lisible (heritage SM_City_*, CityTrees) est donc CONSERVE.
	TArray<AActor*> ToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		const bool bCandidat =
			L.StartsWith(TEXT("SM_Ground_")) || L.StartsWith(TEXT("SM_Slab_")) ||
			L.StartsWith(TEXT("SM_Proxy_")) ||
			L.StartsWith(TEXT("SM_City_")) || L.StartsWith(TEXT("SM_Bldg_")) || L == TEXT("CityTrees");
		if (!bCandidat)
		{
			continue;
		}
		if (bCellFilter)
		{
			FIntPoint K;
			const bool bLu =
				CellFromLabel(L, TEXT("SM_Ground_"), K) || CellFromLabel(L, TEXT("SM_Slab_"), K) ||
				CellFromLabel(L, TEXT("SM_Bldg_"), K);
			if (!bLu || !CellSet.Contains(K))
			{
				continue;
			}
		}
		ToDestroy.Add(*It);
	}
	for (AActor* A : ToDestroy)
	{
		World->DestroyActor(A);
	}

	// Sous-niveaux existants du dossier : reutilises (vides par la purge ci-dessus si
	// charges — les acteurs SM_Bldg_* d'un niveau charge sont vus par TActorIterator).
	TMap<FIntPoint, ULevelStreaming*> Blocks;
	for (ULevelStreaming* S : World->GetStreamingLevels())
	{
		const FString Pkg = S->GetWorldAssetPackageName();
		if (!Pkg.StartsWith(BlocksFolder))
		{
			continue;
		}
		FString Tail = FPackageName::GetShortName(Pkg);
		FString Sx, Sy;
		if (Tail.RemoveFromStart(TEXT("L_T10_B_")) && Tail.Split(TEXT("_"), &Sx, &Sy))
		{
			Blocks.Add(FIntPoint(FCString::Atoi(*Sx), FCString::Atoi(*Sy)), S);
		}
	}

	const float Cell = CellSizeM * 100.f;
	const float ProxyCell = ProxyCellSizeM * 100.f;
	const float Block = BlockSizeM * 100.f;
	TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>> BldgCells, BldgGlassCells, BldgColCells, GroundCells, ProxyCells;
	TSet<FIntPoint> SlabKeys;
	// bLinear / bUV1 (Lot B) : appliques a la CREATION du builder de cellule —
	// encodage lineaire des couleurs et canal UV1 monde. Defauts = mobile inchange.
	auto GetIn = [](TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>>& Map, const FVector2D& P,
		float Size, bool bLinear = false, bool bUV1 = false) -> FCityMeshBuilder&
	{
		const FIntPoint Key(FMath::FloorToInt(P.X / Size), FMath::FloorToInt(P.Y / Size));
		TUniquePtr<FCityMeshBuilder>& B = Map.FindOrAdd(Key);
		if (!B)
		{
			B = MakeUnique<FCityMeshBuilder>();
			B->bLinearColors = bLinear;
			if (bUV1)
			{
				B->EnableWorldUV1();
			}
		}
		return *B;
	};
	// Meme fabrique, adressee par CLE et non par point (mode district, ci-dessous).
	auto GetInKey = [](TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>>& Map, const FIntPoint& Key,
		bool bLinear = false, bool bUV1 = false) -> FCityMeshBuilder&
	{
		TUniquePtr<FCityMeshBuilder>& B = Map.FindOrAdd(Key);
		if (!B)
		{
			B = MakeUnique<FCityMeshBuilder>();
			B->bLinearColors = bLinear;
			if (bUV1)
			{
				B->EnableWorldUV1();
			}
		}
		return *B;
	};
	// ⭐ MODE DISTRICT — LA FUITE DE GEOMETRIE VERS LA CELLULE VOISINE, ET SA GARDE.
	//
	// Defaut MESURE au premier run filtre (cellules -2_0/-2_1/-1_0/-1_1) : quatre
	// SM_Ground_ de plus dans le monde, exactement les voisines EST et SUD du district
	// (0_0, 0_1, -2_2, -1_2), en DOUBLE — et l'asset du voisin reecrit avec le seul
	// fragment qui avait deborde.
	// Cause : bordures, bordurettes, tirets et murs sont deja decoupes A LA CELLULE au
	// prep, mais le premier sommet d'une polyligne peut tomber EXACTEMENT sur la
	// frontiere est ou sud ; `floor` le range alors dans la cellule suivante. En
	// generation complete c'est sans consequence (toutes les cellules sont refaites) ;
	// en generation filtree, cette cellule-la n'est ni purgee ni reconstruite — on
	// creait donc un acteur en double et on ECRASAIT l'asset du voisin.
	// Garde : en mode district, une geometrie qui sortirait du filtre est rangee dans sa
	// cellule PROPRIETAIRE (celle du fichier dont elle vient). Rien n'est perdu, rien ne
	// deborde. Hors mode district, la cle est celle d'avant, au bit pres.
	auto CleSol = [&](const FVector2D& P, const FIntPoint& Proprio) -> FIntPoint
	{
		const FIntPoint K(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell));
		return (!bCellFilter || CellSet.Contains(K)) ? K : Proprio;
	};
	auto ReadPts = [](const TArray<TSharedPtr<FJsonValue>>& In, TArray<FVector2D>& Out)
	{
		for (const TSharedPtr<FJsonValue>& V : In)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() >= 2)
			{
				Out.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
			}
		}
	};

	// --- Batiments : detail (cellules) + proxy (grandes cellules), meme teinte ---
	const TArray<TSharedPtr<FJsonValue>>* BuildingsJson = nullptr;
	if (BldRoot->TryGetArrayField(TEXT("buildings"), BuildingsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *BuildingsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 3)
			{
				continue;
			}
			bool bReversed = false;
			if (SignedArea(Pts) < 0)
			{
				// Un JSON du prep J3b est deja CCW ; un anneau legacy peut ne pas
				// l'etre — dans ce cas les indices de toit seraient invalides.
				Algo::Reverse(Pts);
				bReversed = true;
			}
			FVector2D Centroid(0, 0);
			for (const FVector2D& P : Pts) { Centroid += P; }
			Centroid /= Pts.Num();
			// Mode district : hors des cellules visees on n'entre pas dans la fabrique,
			// mais L'INDEX AVANCE QUAND MEME — c'est lui qui donne a chaque batiment sa
			// teinte et sa sous-tuile (UsageTint/UsageTile, deterministes par indice).
			// Le sauter re-colorierait tous les batiments suivants, et la cellule
			// regeneree ne ressemblerait plus a ses voisines.
			if (bCellFilter && !CellSet.Contains(CelluleDe(Centroid, Cell)))
			{
				++Index;
				continue;
			}
			// Cours interieures (J3b cours) : anneaux CW (metres) livres par
			// j3b_ajoute_cours.py. Chaque trou -> mur de cour + toit qui retombe vers
			// l'avant-toit interieur + collision ajouree. Ignore si le contour a du etre
			// reoriente (bReversed) : l'alignement des indices toit/trous exige le meme
			// contour CCW que le prep. Absent = batiment plein (compat totale).
			TArray<TArray<FVector2D>> Holes;
			const TArray<TSharedPtr<FJsonValue>>* HolesJson = nullptr;
			if (!bReversed && O->TryGetArrayField(TEXT("holes"), HolesJson))
			{
				for (const TSharedPtr<FJsonValue>& HV : *HolesJson)
				{
					TArray<FVector2D> Hole;
					ReadPts(HV->AsArray(), Hole);
					if (Hole.Num() >= 3)
					{
						Holes.Add(MoveTemp(Hole));
					}
				}
			}
			const float Hcm = O->GetNumberField(TEXT("h")) * 100.f;
			const FString Usage = O->GetStringField(TEXT("u"));
			const FVector3f Tint = UsageTint(Usage, Index);
			// Pose desktop (J2 §3.3) : rez-de-chaussee a MinAlt sous l'emprise
			// (rebase Capitole), socle aveugle jusqu'a ZBase - (Max - Min) - SocleCm.
			float ZBase = 0.f, SocleDepth = 0.f;
			if (Drape.IsActive())
			{
				const float MinAlt = Drape.Sampler->MinAltCmInPolygon(Pts);
				const float MaxAlt = Drape.Sampler->MaxAltCmInPolygon(Pts);
				ZBase = MinAlt - Drape.AltCapCm;
				SocleDepth = (MaxAlt - MinAlt) + Gen.SocleCm;
			}
			if (bDesktopBldg)
			{
				// J3b : toit en pente si le bloc "roof" est present et coherent — les
				// murs s'arretent alors a l'EGOUT et le versant monte au faitage
				// (coherent avec h = faitage - sol de la BD TOPO). Le prisme de
				// collision reste a ZBase + h (plan du faitage).
				FRoofData Roof;
				const bool bPitched = !bReversed && ParseRoof(O, Pts.Num(), Holes, Roof)
					&& Roof.EaveCm <= Hcm + 50.f;
				const float WallHcm = bPitched ? FMath::Min(Roof.EaveCm, Hcm) : Hcm;
				// Lot B : fenetres geometriques (en creux si bWindowReveals) ; les
				// vitres partent dans un builder SEPARE si bSplitWallGlass (Q3).
				FCityMeshBuilder& WallB = GetIn(BldgCells, Centroid, Cell, bLinearColors, bWorldUVs);
				FCityMeshBuilder& GlassB = Gen.bSplitWallGlass
					? GetIn(BldgGlassCells, Centroid, Cell, bLinearColors, false) : WallB;
				BuildPolygonBuildingDesktop(WallB, GlassB, Pts, WallHcm, Tint, UsageTile(Usage, Index),
					ZBase, SocleDepth, Gen.bWindowReveals, bBakedShade, Holes, bPitched ? &Roof : nullptr,
					Gen.WindowMode, Gen.bMarbleWhite, Gen.bMarbleWalls);
				if (bPitched)
				{
					++Summary.RoofsPitched;
				}
				// Verrou 2 : prisme de collision dedie, meme pose — les murs Nanite ne
				// servent JAMAIS de collision (fallback decime = facades traversables).
				// PoC LiDAR : toit absolu -> prisme plein a plat (cap au faitage = ZBase+Hcm) ;
				// pas de cap ajoure (Roof.Faces vide). Envelope de collision suffisante (vol).
				BuildCollisionPrism(GetIn(BldgColCells, Centroid, Cell), Pts,
					ZBase + Hcm, ZBase - SocleDepth, Holes,
					(bPitched && !Roof.bAbs) ? &Roof : nullptr);
			}
			else
			{
				BuildPolygonBuildingTextured(GetIn(BldgCells, Centroid, Cell), Pts, Hcm, Tint,
					ZBase, SocleDepth);
			}
			// V6 : la couche proxy est supprimee (cf. FCityGenProfile::bProxyLayer).
			// On ne la CONSTRUIT plus du tout : a l'echelle 3x3 km c'est autant de
			// geometrie, d'assets et d'acteurs en moins, pas seulement une visibilite.
			if (bProxyLayer)
			{
				BuildProxyBuilding(GetIn(ProxyCells, Centroid, ProxyCell, bLinearColors), Pts, Hcm,
					Gen.bMarbleWhite ? MarbleTint() : Tint,
					ZBase, SocleDepth, bBakedShade, Gen.bMarbleWhite);
			}
			SlabKeys.Add(FIntPoint(FMath::FloorToInt(Centroid.X / Cell), FMath::FloorToInt(Centroid.Y / Cell)));
			++Summary.Buildings;
			++Index;
		}
	}

	// --- Routes : couche sol residente ---
	// J3c point 2 : classe de revetement par DONNEE (tag OSM "surface"), a defaut par
	// type + nombre de voies. Desactive (mobile) = ruban T_RoadStrip historique.
	FSurfaceLibrary Surfaces;
	Surfaces.Init(Gen.bSurfaceMaterials, Gen.SurfacesFolder);

	// --- J3c maquette du sol : releve des cellules qui ont un masque cuit.
	// La liste vient du DISQUE et non des routes : avec la chaussee peinte, une
	// cellule peut n'avoir plus aucun ruban et devoir quand meme sa dalle.
	// bMaskedGround exige bSurfaceMaterials (le melange se fait avec les memes
	// scans) — sans lui, on ne bascule pas et le comportement actuel tient.
	const bool bMaskedGround = Gen.bMaskedGround && Gen.bSurfaceMaterials;
	TMap<FIntPoint, FGroundMaskCell> MaskCells;
	if (bMaskedGround)
	{
		const FString Dir = GroundMasksDir(Gen);
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("sols_*.json")), true, false);
		for (const FString& File : Files)
		{
			FString Rest = FPaths::GetBaseFilename(File);
			Rest.RemoveFromStart(TEXT("sols_"));
			FString Sx, Sy;
			if (!Rest.Split(TEXT("_"), &Sx, &Sy, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				continue;
			}
			const FIntPoint Key(FCString::Atoi(*Sx), FCString::Atoi(*Sy));
			// Mode district : on ne LIT meme pas le JSON des cellules hors filtre —
			// c'est la passe la plus lourde a l'entree (bordures, bordurettes, tirets
			// de 36 cellules), et elle n'a rien a dire sur un quartier qu'on ne
			// regenere pas.
			if (!CelluleVisee(Key))
			{
				continue;
			}
			FGroundMaskCell Data;
			if (LoadGroundMaskCell(Dir, Key.X, Key.Y, CellSizeM, Data))
			{
				MaskCells.Add(Key, MoveTemp(Data));
				SlabKeys.Add(Key);
			}
		}
		if (MaskCells.Num() == 0)
		{
			UE_LOG(LogCityImport, Warning,
				TEXT("Maquette du sol demandee mais aucun masque dans '%s' : les rubans de chaussee restent generes."),
				*Dir);
		}
	}
	// Une cellule N'A de dalle masquee que si elle a A LA FOIS son JSON et son
	// instance de materiau : sans le materiau, la peindre reviendrait a effacer la
	// chaussee sans rien mettre a la place.
	const FString MaskAssetDir = GroundMasksAssetDir(Gen);
	auto LoadCellMaskMaterial = [&MaskAssetDir](const FIntPoint& Key) -> UMaterialInterface*
	{
		const FString Path = FString::Printf(TEXT("%s/MI_CityGround_%d_%d.MI_CityGround_%d_%d"),
			*MaskAssetDir, Key.X, Key.Y, Key.X, Key.Y);
		return LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	};
	// Le basculement est GLOBAL et non par cellule : supprimer les rubans sur une
	// cellule peinte et les garder sur sa voisine ferait une couture visible en
	// plein milieu d'une rue. Si un seul materiau de cellule manque, on ne bascule
	// pas du tout et on le dit.
	bool bMaskedActive = bMaskedGround && MaskCells.Num() > 0;
	if (bMaskedActive)
	{
		int32 Missing = 0;
		for (const TPair<FIntPoint, FGroundMaskCell>& Pair : MaskCells)
		{
			if (!LoadCellMaskMaterial(Pair.Key))
			{
				++Missing;
			}
		}
		if (Missing > 0)
		{
			bMaskedActive = false;
			UE_LOG(LogCityImport, Warning,
				TEXT("Maquette du sol : %d/%d instances MI_CityGround_* absentes sous '%s' — bascule ANNULEE (lancer Tools/import_ground_masks.py)."),
				Missing, MaskCells.Num(), *MaskAssetDir);
		}
	}

	// Largeur de ruban : la MEME regle que BuildRoad — elle sert au rayon des patchs
	// de carrefour, donc a l'emprise ou la bordure s'interrompt. v5 : une chaussee
	// auto emporte desormais chant + rive (chaussee/2 + 15 + 170) ; le reste garde la
	// regle historique (chaussee/2 + 170, ou + 0 pour une voie pietonne).
	auto RibbonHalfCm = [](const FString& Type, float WidthCm, const FSurfaceClass* Class)
	{
		if (Class && Class->bAuto)
		{
			return WidthCm * 0.5f + GCurbTopWidthCm + GSidewalkWidthCm;
		}
		const bool bWalkway = Type == TEXT("footway") || Type == TEXT("path") || Type == TEXT("cycleway");
		return WidthCm * 0.5f + (bWalkway ? 0.f : 170.f);
	};
	auto ReadRoadTags = [](const TSharedPtr<FJsonObject>& O, FString& OutSurface, int32& OutLanes)
	{
		OutSurface.Empty();
		OutLanes = 0;
		O->TryGetStringField(TEXT("surface"), OutSurface);
		O->TryGetNumberField(TEXT("lanes"), OutLanes);
	};
	// v5 point 2 — SITE DE PASSAGE PIETON. Un noeud partage entre une CHAUSSEE AUTO et
	// une voie PIETONNE : la voie pietonne ne produit plus de ruban depuis la v4, mais
	// elle existe toujours dans la donnee — c'est elle qui dit ou la vraie ville avait
	// un passage. On retient, par noeud, la chaussee la plus LARGE (deterministe) avec
	// sa tangente au noeud : le quad se posera dans l'axe de cette rue.
	struct FCrossingSite
	{
		FVector2D PosCm = FVector2D::ZeroVector;
		FVector2D DirCm = FVector2D::ZeroVector;
		float RoadHalfCm = -1.f;
		float ZClassCm = 0.f;
	};
	TMap<FIntPoint, FCrossingSite> CrossingSites;
	TSet<FIntPoint> PedestrianNodes;

	// v2 — PASSE 1 : releve des noeuds partages (sur les points D'ORIGINE du JSON,
	// pas les points re-echantillonnes : seuls les noeuds OSM sont partages).
	FJunctionMap Junctions;
	const TArray<TSharedPtr<FJsonValue>>* RoadsJson = nullptr;
	if (Gen.bSurfaceMaterials && Root->TryGetArrayField(TEXT("roads"), RoadsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *RoadsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			const FString Type = O->GetStringField(TEXT("t"));
			// v4 : une voie pietonne ne produit plus de ruban — elle ne doit donc plus
			// peser sur les carrefours, ni par son compte de voies, ni comme dominante
			// (une place pietonne large aurait sinon interdit le patch d'un vrai
			// croisement de voitures qu'elle traverse).
			if (Pts.Num() >= 2 && IsPedestrianRibbon(Type))
			{
				for (const FVector2D& P : Pts)
				{
					PedestrianNodes.Add(FJunctionMap::Key(P));
				}
			}
			else if (Pts.Num() >= 2)
			{
				const float WidthCm = O->GetNumberField(TEXT("w")) * 100.f;
				FString SurfaceTag;
				int32 Lanes = 0;
				ReadRoadTags(O, SurfaceTag, Lanes);
				const FSurfaceClass* Class = SurfaceClassForRoad(SurfaceTag, Type, Lanes, WidthCm);
				Junctions.Add(Index, Pts, RibbonHalfCm(Type, WidthCm, Class), Class);
				if (Class && Class->bAuto)
				{
					for (int32 i = 0; i < Pts.Num(); ++i)
					{
						FCrossingSite& Site = CrossingSites.FindOrAdd(FJunctionMap::Key(Pts[i]));
						if (WidthCm * 0.5f <= Site.RoadHalfCm)
						{
							continue;
						}
						// Tangente au noeud : moyenne des segments adjacents, comme les
						// normales de BuildRoad — le passage reste dans l'axe meme sur un
						// sommet de virage.
						FVector2D D(0, 0);
						if (i > 0) { D += (Pts[i] - Pts[i - 1]).GetSafeNormal(); }
						if (i + 1 < Pts.Num()) { D += (Pts[i + 1] - Pts[i]).GetSafeNormal(); }
						if (D.IsNearlyZero())
						{
							continue;
						}
						Site.PosCm = Pts[i];
						Site.DirCm = D.GetSafeNormal();
						Site.RoadHalfCm = WidthCm * 0.5f;
						Site.ZClassCm = Class->ZClassCm;
					}
				}
			}
			++Index;
		}
		Junctions.Build();
		UE_LOG(LogCityImport, Display,
			TEXT("Carrefours : %d noeuds partages sur %d releves, dont %d a patcher (>= 2 chaussees auto)."),
			Junctions.NumJunctions, Junctions.Nodes.Num(), Junctions.NumAutoJunctions);
	}
	RoadsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("roads"), RoadsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *RoadsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 2)
			{
				continue;
			}
			// Mode district. Un ruban entier part dans la cellule de son PREMIER point
			// (regle de fusion de GetIn) : filtrer sur cette meme cellule est donc exact
			// — un ruban qui deborde sur la voisine appartient au mesh de la cellule
			// d'origine, qui n'est pas touche tant qu'elle n'est pas visee.
			// L'INDEX avance : il ensemence la variation de BuildRoad.
			if (bCellFilter && !CellSet.Contains(CelluleDe(Pts[0], Cell)))
			{
				++Index;
				continue;
			}
			// v4 — LE PIETON EST LA DALLE : en profil revetements, aucune voie pietonne
			// ne produit de ruban. C'est LA simplification structurelle de la v4 (le
			// lacis pieton du centre faisait a lui seul l'effet « grand puzzle »). Le
			// golden path mobile n'est pas concerne : sans revetements, les sentiers
			// gardent leur ruban uni historique, a l'octet pres.
			if (Gen.bSurfaceMaterials && IsPedestrianRibbon(O->GetStringField(TEXT("t"))))
			{
				++Index;
				continue;
			}
			// C2 (03/08) — LE RUBAN DE PONT EST REMPLACE PAR LE TABLIER A SA COTE.
			// Le ruban OSM n'a AUCUNE cote : ComputePolylineZ l'interpolait entre les
			// deux bouts drapes au MNT, ce qui l'enfoncait de 7,62 m en moyenne sous
			// le vrai tablier du Pont Saint-Pierre (mesure du 03/08) — il traversait
			// la promenade au niveau du sable. Des que la passe ponts est active,
			// c'est elle qui pose le franchissement, depuis BD TOPO 3D.
			// `bBridgeRibbonsHistorique` restitue l'ancien comportement pour l'A/B.
			if (Gen.bBridges && Drape.IsActive() && !Gen.bBridgeRibbonsHistorique)
			{
				bool bBridgeRibbon = false;
				O->TryGetBoolField(TEXT("bridge"), bBridgeRibbon);
				if (bBridgeRibbon)
				{
					++Summary.BridgeRibbonsReplaced;
					++Index;
					continue;
				}
			}
			// J3c maquette — LA CHAUSSEE N'EST PLUS UN FILM POSE SUR LA DALLE. Une
			// fois le sol peint, un ruban au niveau du sol ne ferait que doubler la
			// peinture (et se battre avec elle en profondeur). Seuls survivent les
			// PONTS : leur tablier est au-dessus du terrain, aucun masque de sol ne
			// peut le rendre.
			if (bMaskedActive)
			{
				bool bBridgeRibbon = false;
				O->TryGetBoolField(TEXT("bridge"), bBridgeRibbon);
				if (!bBridgeRibbon)
				{
					++Summary.GroundRibbonsSkipped;
					++Index;
					continue;
				}
				++Summary.BridgeRibbons;
			}
			// v5 point 4 — FRAGMENT ORPHELIN. Court (< 25 m) ET sans le moindre noeud
			// partage avec le reste du reseau : ce n'est pas une rue, c'est un bout de
			// voie coupe par la fenetre d'extraction, pose seul sur la dalle (« morceaux
			// perdus », verdict v4b). La connexite se lit dans la carte des noeuds deja
			// relevee en passe 1, sur les points D'ORIGINE (les seuls partages).
			if (Gen.bSurfaceMaterials && Junctions.Nodes.Num() > 0)
			{
				float LenCm = 0.f;
				for (int32 i = 0; i + 1 < Pts.Num(); ++i)
				{
					LenCm += (float)(Pts[i + 1] - Pts[i]).Size();
				}
				if (LenCm < GOrphanMaxLenCm)
				{
					bool bConnected = false;
					for (const FVector2D& P : Pts)
					{
						const FJunctionNode* Node = Junctions.Nodes.Find(FJunctionMap::Key(P));
						if (Node && Node->NumRoads >= 2)
						{
							bConnected = true;
							break;
						}
					}
					if (!bConnected)
					{
						++Summary.OrphanRibbons;
						++Index;
						continue;
					}
				}
			}
			// Desktop (J2 §3.2) : re-echantillonnage a pas fixe puis drapage MNT,
			// l'empilement 55+ devient relatif au terrain. Champ optionnel bridge
			// (lu TOLERANT, absent = false) : tablier interpole entre les culees.
			const TArray<FVector2D>* RoadPts = &Pts;
			TArray<FVector2D> Resampled;
			TArray<float> TerrainZ;
			const TArray<float>* TerrainZPtr = nullptr;
			if (Drape.IsActive())
			{
				if (Gen.RoadResampleStepCm > 0.f)
				{
					Resampled = ResamplePolyline(Pts, Gen.RoadResampleStepCm);
					RoadPts = &Resampled;
				}
				bool bBridge = false;
				O->TryGetBoolField(TEXT("bridge"), bBridge);
				ComputePolylineZ(*RoadPts, Drape, bBridge, TerrainZ);
				TerrainZPtr = &TerrainZ;
			}
			// Champs OSM optionnels, lus TOLERANT (absents sur les JSON historiques).
			const float RoadWidthCm = O->GetNumberField(TEXT("w")) * 100.f;
			const FString RoadType = O->GetStringField(TEXT("t"));
			FString RoadSurface;
			int32 RoadLanes = 0;
			ReadRoadTags(O, RoadSurface, RoadLanes);
			const FResolvedSurface* RoadSurf =
				Surfaces.Resolve(SurfaceClassForRoad(RoadSurface, RoadType, RoadLanes, RoadWidthCm));
			// v2 : drapeau « sommet au contact d'un carrefour » (les tirets axiaux y
			// cedent la place a l'asphalte nu). Calcule seulement pour les classes
			// marquees — inutile de sonder la grille pour un trottoir.
			const FResolvedSurface* PlainSurf = nullptr;
			TArray<uint8> NearJunction;
			if (RoadSurf && RoadSurf->Class->bFullWidth && Junctions.NumJunctions > 0)
			{
				PlainSurf = Surfaces.Resolve(&GSurfAsphalt);
				NearJunction.SetNumUninitialized(RoadPts->Num());
				for (int32 i = 0; i < RoadPts->Num(); ++i)
				{
					NearJunction[i] = Junctions.IsNear((*RoadPts)[i], GJunctionPlainCm) ? 1 : 0;
				}
			}
			// v5 : rives + bordures (chaussees auto uniquement). CurbSurf/SlabSurf nuls
			// en profil mobile -> BuildRoad reprend son chemin d'un seul quad.
			const FResolvedSurface* CurbSurf = Surfaces.Resolve(&GSurfCurb);
			const FResolvedSurface* RibbonSlabSurf = Surfaces.Resolve(&GSurfSlab);
			BuildRoad(GetIn(GroundCells, Pts[0], Cell, bLinearColors, bWorldUVs), *RoadPts,
				RoadWidthCm, RoadType, Index, TerrainZPtr, bBakedShade, RoadSurf,
				PlainSurf, PlainSurf ? &NearJunction : nullptr,
				CurbSurf, RibbonSlabSurf, &Junctions, &Summary.CurbQuads);
			SlabKeys.Add(FIntPoint(FMath::FloorToInt(Pts[0].X / Cell), FMath::FloorToInt(Pts[0].Y / Cell)));
			++Summary.Roads;
			++Index;
		}
		// v2 — PASSE 3 : patchs de carrefour. Un disque du revetement DOMINANT
		// (classe de la voie la plus large) recouvre le disque de rencontre, pose
		// au-dessus du ruban le plus haut du noeud. Si la dominante est une classe
		// MARQUEE, le patch prend son equivalent NU : un croisement n'a jamais de
		// ligne axiale en son milieu.
		// v3 : WantsPatch() — reserve aux noeuds ou au moins DEUX chaussees auto se
		// rencontrent (zero patch dans le lacis pieton, cf. « peau de leopard »).
		for (const TPair<FIntPoint, FJunctionNode>& Pair : Junctions.Nodes)
		{
			const FJunctionNode& Node = Pair.Value;
			// J3c maquette : un carrefour PEINT n'a pas besoin d'un disque de
			// rattrapage — il n'y a plus de rubans a raccorder.
			if (bMaskedActive || !Node.WantsPatch() || Node.MaxHalfCm < 150.f ||
				!CelluleVisee(CelluleDe(Node.PosCm, Cell)))
			{
				continue;
			}
			const FSurfaceClass* PatchClass = Node.Dominant->bFullWidth ? &GSurfAsphalt : Node.Dominant;
			const FResolvedSurface* PatchSurf = Surfaces.Resolve(PatchClass);
			if (!PatchSurf)
			{
				continue;
			}
			const float Zcm = 55.f + Node.MaxZClassCm + GJunctionPatchLiftCm
				+ Drape.GroundZ(Node.PosCm.X, Node.PosCm.Y);
			BuildJunctionPatch(GetIn(GroundCells, Node.PosCm, Cell, bLinearColors, bWorldUVs),
				Node.PosCm, Node.MaxHalfCm + GJunctionPatchMarginCm, Zcm, PatchSurf,
				FVector3f(0.85f, 0.85f, 0.80f));
			SlabKeys.Add(FIntPoint(FMath::FloorToInt(Node.PosCm.X / Cell),
				FMath::FloorToInt(Node.PosCm.Y / Cell)));
			++Summary.JunctionPatches;
		}

		// v5 — PASSE 4 : PASSAGES PIETONS. Un quad par noeud partage entre une chaussee
		// auto et une voie pietonne, en travers de la CHAUSSEE SEULE, au-dessus du
		// ruban (+9 cm) et donc au-dessus du patch de ce noeud (+5 cm), mais SOUS le
		// chant des bordures (+12 cm) : le passage s'arrete au pied du trottoir.
		// Noeud deja couvert par un disque de patch : passage REPORTE (compte a part).
		// Le raccord propre bord-de-patch (passage pose en amont de l'entree du
		// carrefour) est au backlog — ici, un carrefour reste une zone de roulement nue.
		if (Gen.bSurfaceMaterials && !bMaskedActive && CrossingSites.Num() > 0)
		{
			const FResolvedSurface* CrossSurf = Surfaces.Resolve(&GSurfCrossing);
			for (const TPair<FIntPoint, FCrossingSite>& Pair : CrossingSites)
			{
				const FCrossingSite& Site = Pair.Value;
				if (Site.RoadHalfCm <= 0.f || !PedestrianNodes.Contains(Pair.Key) ||
					!CelluleVisee(CelluleDe(Site.PosCm, Cell)))
				{
					continue;
				}
				if (Junctions.IsInPatch(Site.PosCm))
				{
					++Summary.CrossingsDeferred;
					continue;
				}
				if (!CrossSurf)
				{
					continue;
				}
				const float Zcm = 55.f + Site.ZClassCm + GCrossingLiftCm
					+ Drape.GroundZ(Site.PosCm.X, Site.PosCm.Y);
				BuildCrossing(GetIn(GroundCells, Site.PosCm, Cell, bLinearColors, bWorldUVs),
					Site.PosCm, Site.DirCm, Site.RoadHalfCm, Zcm, CrossSurf,
					FVector3f(0.85f, 0.85f, 0.80f));
				SlabKeys.Add(FIntPoint(FMath::FloorToInt(Site.PosCm.X / Cell),
					FMath::FloorToInt(Site.PosCm.Y / Cell)));
				++Summary.Crossings;
			}
		}
		if (Gen.bSurfaceMaterials)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("Voirie : %d quads de bordure, %d passages pietons poses, %d reportes (patch), %d rubans orphelins ecartes."),
				Summary.CurbQuads, Summary.Crossings, Summary.CrossingsDeferred, Summary.OrphanRibbons);
		}
	}

	// --- J3c maquette : LE RELIEF, depuis le masque. Trois passes tres courtes —
	// tout le decoupage a ete fait au prep, il ne reste qu'a poser des quads. Elles
	// vivent dans les cellules de RUBANS (SM_Ground_*, sans collision, cullables) :
	// une bordure ou un tiret n'a rien a faire dans la dalle porteuse.
	if (bMaskedActive)
	{
		const FResolvedSurface* CurbSurf = Surfaces.Resolve(&GSurfCurb);
		const FResolvedSurface* CrossSurf = Surfaces.Resolve(&GSurfCrossing);
		const FResolvedSurface* MarkSurf = Surfaces.Resolve(&GSurfMarking);
		const FVector3f Tint(0.85f, 0.85f, 0.80f);
		// v4 — LE Z DU SOL RENDU, un seul objet pour TOUT ce qui se pose sur la dalle
		// (bordure de chaussee, bordurette d'herbe, passages, tirets axiaux).
		FRenderedGroundZ RGZ;
		RGZ.Init(Drape, Gen.GroundGridN, Cell);
		FCurbSinkStats SinkRoad;
		FCurbSinkStats SinkGrass;
		for (const TPair<FIntPoint, FGroundMaskCell>& Pair : MaskCells)
		{
			const FGroundMaskCell& Data = Pair.Value;
			for (const TArray<FVector2D>& Line : Data.Curbs)
			{
				BuildMaskCurb(GetInKey(GroundCells, CleSol(Line[0], Pair.Key),
						bLinearColors, bWorldUVs),
					Line, RGZ, CurbSurf, Tint, &Summary.CurbQuads,
					GCurbHeightCm, GCurbTopWidthCm, &SinkRoad);
			}
			// V3 : LA BORDURETTE D'HERBE. Meme mecanique, MEME materiau de bordure,
			// profil reduit (7 / 14 cm au lieu de 12 / 15). Elle vit dans les memes
			// cellules de rubans que la bordure de chaussee — une pierre n'a rien a
			// faire dans la dalle porteuse. Compteur SEPARE : c'est ce qui permet de
			// juger le volume pose sans le confondre avec celui de la voirie.
			for (const TArray<FVector2D>& Line : Data.GrassEdges)
			{
				BuildMaskCurb(GetInKey(GroundCells, CleSol(Line[0], Pair.Key),
						bLinearColors, bWorldUVs),
					Line, RGZ, CurbSurf, Tint, &Summary.GrassCurbQuads,
					GGrassCurbHeightCm, GGrassCurbTopWidthCm, &SinkGrass);
			}
			for (const FMaskCrossing& Site : Data.Crossings)
			{
				const float Zcm = RGZ.At(Site.PosCm.X, Site.PosCm.Y) + GMaskCrossLiftCm;
				BuildCrossing(GetInKey(GroundCells, CleSol(Site.PosCm, Pair.Key),
						bLinearColors, bWorldUVs),
					Site.PosCm, Site.DirCm, Site.HalfWCm, Zcm, CrossSurf, Tint);
				++Summary.Crossings;
			}
			for (const FVector4& Seg : Data.Axial)
			{
				const FVector2D A(Seg.X, Seg.Y);
				BuildAxialDash(GetInKey(GroundCells, CleSol(A, Pair.Key),
						bLinearColors, bWorldUVs),
					A, FVector2D(Seg.Z, Seg.W), RGZ, MarkSurf, Tint);
				++Summary.AxialDashes;
			}
		}
		UE_LOG(LogCityImport, Display,
			TEXT("Maquette du sol : %d cellules masquees, %d rubans de chaussee supprimes, %d ponts conserves, %d quads de bordure, %d quads de bordurette d'herbe, %d passages, %d tirets axiaux."),
			MaskCells.Num(), Summary.GroundRibbonsSkipped, Summary.BridgeRibbons,
			Summary.CurbQuads, Summary.GrassCurbQuads, Summary.Crossings, Summary.AxialDashes);
		// v4 — CE QUE LE CORRECTIF A RATTRAPE, mesure sur la vraie donnee : ecart entre
		// la dalle RENDUE et le MNT continu aux sommets de bordure. Chaque sommet
		// au-dela de la hauteur du profil etait une pierre INVISIBLE avant la v4.
		auto LogSink = [](const TCHAR* Nom, const FCurbSinkStats& S, float HauteurCm)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("Z du sol rendu (%s) : %d sommets, enterrement moyen %.2f cm, max %.2f cm ; "
					 "%d sommets > 7 cm, %d > 12 cm (avant v4 : autant de pierres noyees pour un profil de %.0f cm) ; "
					 "%d sommets ajoutes par le decoupage adaptatif."),
				Nom, S.Vertices, S.Vertices > 0 ? (float)(S.SumCm / S.Vertices) : 0.f,
				S.MaxCm, S.Over7cm, S.Over12cm, HauteurCm, S.AddedVertices);
		};
		LogSink(TEXT("bordure de chaussee"), SinkRoad, GCurbHeightCm);
		LogSink(TEXT("bordurette d'herbe"), SinkGrass, GGrassCurbHeightCm);
	}

	// --- C1 « DISCONTINUITES » : LES MURS DE SOUTENEMENT.
	// Passe INDEPENDANTE de la maquette du sol (une berge de canal n'a pas besoin
	// d'un masque de chaussee), mais elle exige le DRAPE : sans relief, il n'y a
	// aucune rampe a masquer. Elle vit, comme les bordures, dans les cellules de
	// RUBANS (SM_Ground_*) : un mur n'a rien a faire dans la dalle porteuse.
	if (Gen.bRetainingWalls && Drape.IsActive())
	{
		const FString WallDir = RetainingWallsDir(Gen);
		TSet<FString> Classes;
		if (!Gen.RetainingWallClasses.IsEmpty())
		{
			TArray<FString> Parts;
			Gen.RetainingWallClasses.ToLower().ParseIntoArray(Parts, TEXT(","), true);
			for (FString& P : Parts)
			{
				Classes.Add(P.TrimStartAndEnd());
			}
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(WallDir / TEXT("murs_*.json")), true, false);
		// Le mur reprend LE MATERIAU DES BORDURES (dalle assombrie x0,92) : aucun
		// materiau nouveau, et la meme grammaire minerale que la pierre de la rue.
		// FSurfaceLibrary::Resolve rend nullptr quand bSurfaceMaterials est faux —
		// on le DIT plutot que de ne rien poser en silence.
		const FResolvedSurface* WallSurf = Surfaces.Resolve(&GSurfCurb);
		if (!WallSurf)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("Murs de soutenement demandes mais bSurfaceMaterials est faux : aucun mur pose."));
		}
		const FVector3f WallTint(0.85f, 0.85f, 0.80f);
		FRenderedGroundZ WallRGZ;
		WallRGZ.Init(Drape, Gen.GroundGridN, Cell);
		const float QuadCm = (Gen.GroundGridN > 0) ? Cell / (float)Gen.GroundGridN : Cell;
		int32 CellsWithWalls = 0;
		int32 CellsWrongSize = 0;
		int32 GradinsWrongSize = 0;
		int32 MursCedes = 0;      // BUILDQUAY : murs remplaces par la piece
		double BakedCellM = 0.0;
		const FString GradinDir = GradinsDir(Gen);
		for (const FString& File : Files)
		{
			FString Rest = FPaths::GetBaseFilename(File);
			Rest.RemoveFromStart(TEXT("murs_"));
			FString Sx, Sy;
			if (!Rest.Split(TEXT("_"), &Sx, &Sy, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				continue;
			}
			// Mode district : le side-car est deja decoupe a la cellule, le filtre se
			// lit donc directement sur le nom du fichier.
			if (!CelluleVisee(FIntPoint(FCString::Atoi(*Sx), FCString::Atoi(*Sy))))
			{
				continue;
			}
			TArray<FRetainingWall> Walls;
			if (!LoadRetainingWallCell(WallDir, FCString::Atoi(*Sx), FCString::Atoi(*Sy),
				CellSizeM, Classes, Walls, CellsWrongSize, BakedCellM) || Walls.Num() == 0)
			{
				continue;
			}
			++CellsWithWalls;
			const FIntPoint CelluleMur(FCString::Atoi(*Sx), FCString::Atoi(*Sy));
			// FINITION QUAIS : les EMPRISES de gradins de CETTE cellule. Contrat des
			// autres side-cars : dossier absent ou cellule sans fichier = aucune
			// emprise, sans erreur — et alors pas un gradin nulle part.
			TArray<FGradinEmprise> Emprises;
			LoadGradinsCell(GradinDir, CelluleMur.X, CelluleMur.Y, CellSizeM,
				Emprises, GradinsWrongSize);
			Summary.QuayTierEmprises += Emprises.Num();
			const FCityQuay& QuayEx = QuaySingleton();
			for (const FRetainingWall& W : Walls)
			{
				// ⭐ BUILDQUAY — EXCLUSION PAR TYPE. Un mur de classe `quai` a
				// portee d'une chaine de frontiere typee `quai_dur` est DEJA
				// construit par la piece (face, couronnement, gradins) : le
				// poser une seconde fois par ruptures de sol, c'est la couture
				// qui a fait le chaos. Le generateur C1 garde TOUT le reste de
				// la ville — l'exclusion est nominative et vient de la cuisson.
				if (QuayEx.bActive && QuayEx.MursExclus.Contains(
					FString::Printf(TEXT("%d_%d#%d"), CelluleMur.X, CelluleMur.Y, W.Rang)))
				{
					++MursCedes;
					UE_LOG(LogCityImport, Display,
						TEXT("MUR CEDE AU CONSTRUCTEUR cellule=%d_%d classe=%s rang=%d — la piece de quai le porte."),
						CelluleMur.X, CelluleMur.Y, *W.Classe, W.Rang);
					continue;
				}
				// BERGES — PLANCHER DE LONGUEUR, ecarte AVEC CAUSE (jamais en silence).
				// Defaut 0 : la mesure dit que le side-car n'a aucun mur sous son
				// propre plancher de detection (min 12,04 m pour un plancher de 12 m).
				if (Gen.WallMinLengthM > 0.f)
				{
					double LongCm = 0.0;
					for (int32 k = 0; k + 1 < W.PtsCm.Num(); ++k)
					{
						LongCm += (W.PtsCm[k + 1] - W.PtsCm[k]).Size();
					}
					if (LongCm * 0.01 < (double)Gen.WallMinLengthM)
					{
						++Summary.RetainingWallsTooShort;
						UE_LOG(LogCityImport, Display,
							TEXT("MUR ECARTE cellule=%d_%d classe=%s longueur=%.2f m — sous le plancher de %.2f m."),
							CelluleMur.X, CelluleMur.Y, *W.Classe,
							(float)(LongCm * 0.01), Gen.WallMinLengthM);
						continue;
					}
				}
				int32 Tiers = 0;
				float TierM = 0.f;
				FWallGeom Geom;
				const int32 Q = BuildRetainingWall(
					GetInKey(GroundCells, CleSol(W.PtsCm[0], CelluleMur),
						bLinearColors, bWorldUVs),
					W, WallRGZ, WallSurf, WallTint, QuadCm, Gen.bQuayTiers, Tiers,
					&Emprises, &TierM, Gen.bWallCrestOnPlateau, &Geom,
					Gen.bWallNoFlip, Gen.bWallCapsOnRealEndsOnly,
					&Summary.RetainingWallCaps, &Summary.RetainingWallFlipsFixed,
					Gen.bWallFootOnPlateau, Gen.WallFootMinM * 100.f);
				Summary.QuayTierDm += FMath::RoundToInt32(TierM * 10.0f);
				// BERGES — LA LIGNE NOMINATIVE DE GEOMETRIE (cf. FWallGeom).
				UE_LOG(LogCityImport, Display,
					TEXT("MUR GEOM cellule=%d_%d classe=%s longueur=%.1f m pied=%.2f m crete=%.2f m emprise=%.2f m h=%.2f m pose=%d"),
					CelluleMur.X, CelluleMur.Y, *W.Classe, Geom.LenM,
					Geom.OffFootCm * 0.01f, Geom.OffCrestCm * 0.01f,
					(Geom.OffFootCm + Geom.OffCrestCm) * 0.01f, Geom.HMedCm * 0.01f,
					Q > 0 ? 1 : 0);
				Summary.RetainingWallSpanDm +=
					(Q > 0) ? FMath::RoundToInt32((Geom.OffFootCm + Geom.OffCrestCm) * 0.1f) : 0;
				if (Q > 0)
				{
					Summary.RetainingWallQuads += Q;
					++Summary.RetainingWalls;
					if (Tiers >= 2)
					{
						++Summary.QuayTierWalls;
						Summary.QuayTiers += Tiers;
						// NOMINATIF, une ligne par mur gradine : c'est ce que lit le
						// verrou (work/FINQUAIS/f_verrou_gradins.py). Un compte agrege
						// ne dirait pas QUEL mur, ni sur COMBIEN de metres.
						double LongCm = 0.0;
						for (int32 k = 0; k + 1 < W.PtsCm.Num(); ++k)
						{
							LongCm += (W.PtsCm[k + 1] - W.PtsCm[k]).Size();
						}
						UE_LOG(LogCityImport, Display,
							TEXT("GRADIN POSE cellule=%d_%d classe=%s longueur_mur=%.1f m gradines=%.1f m gradins=%d h_med_side_car=%.2f m borde_pieton=%d"),
							CelluleMur.X, CelluleMur.Y, *W.Classe,
							(float)(LongCm * 0.01), TierM, Tiers,
							W.HMedCm * 0.01f, W.bBordePieton ? 1 : 0);
					}
				}
				else
				{
					++Summary.RetainingWallsSkipped;
				}
			}
		}
		UE_LOG(LogCityImport, Display,
			TEXT("BERGES murs : emprise cumulee %.1f m (somme pied+crete), %d bouchons, %d sommets deretournes, %d ecartes trop courts ; crete_sur_palier=%d anti_retournement=%d bouchons_vrais_bouts=%d | MUR35 pied_sur_palier=%d plancher_pied=%.2f m."),
			Summary.RetainingWallSpanDm * 0.1f, Summary.RetainingWallCaps,
			Summary.RetainingWallFlipsFixed, Summary.RetainingWallsTooShort,
			Gen.bWallCrestOnPlateau ? 1 : 0, Gen.bWallNoFlip ? 1 : 0,
			Gen.bWallCapsOnRealEndsOnly ? 1 : 0,
			Gen.bWallFootOnPlateau ? 1 : 0, Gen.WallFootMinM);
		UE_LOG(LogCityImport, Display,
			TEXT("Murs de soutenement : %d cellules, %d murs poses (%d quads), %d ecartes (aucune rampe a masquer) — dossier '%s'."),
			CellsWithWalls, Summary.RetainingWalls, Summary.RetainingWallQuads,
			Summary.RetainingWallsSkipped, *WallDir);
		UE_LOG(LogCityImport, Display,
			TEXT("Gradins de quai : %d emprises OSM lues dans '%s', %d murs gradines (%d gradins, %.1f m de mur) — %s. Hors emprise, AUCUN gradin : c'est la donnee qui borne, pas un reglage."),
			Summary.QuayTierEmprises, *GradinDir, Summary.QuayTierWalls, Summary.QuayTiers,
			Summary.QuayTierDm * 0.1f,
			Gen.bQuayTiers ? TEXT("profil actif") : TEXT("PROFIL DESACTIVE (bQuayTiers=false)"));
		if (CellsWrongSize > 0)
		{
			// UNE ligne pour toute la passe, en Display : le side-car trouve n'est pas
			// pour cette maille, ce n'est pas une faute (voir LoadRetainingWallCell).
			UE_LOG(LogCityImport, Display,
				TEXT("Murs de soutenement : %d cellules IGNOREES — leur side-car est cuit pour des cellules de %.0f m et cet import travaille a %.0f m."),
				CellsWrongSize, BakedCellM, CellSizeM);
		}
		if (MursCedes > 0)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("BUILDQUAY : %d murs de classe quai CEDES au constructeur (exclusion par TYPE ; le generateur C1 garde le reste de la ville)."),
				MursCedes);
		}
	}

	// =========================================================================
	// ⭐ BUILDQUAY — LA POSE DE LA PIECE. Le moteur ne fait que POSER : la
	// cuisson a deja balaye le profil le long de la chaine de frontiere. Meme
	// materiau que les murs de quai (bordure, `GSurfCurb`) et meme teinte —
	// AUCUNE classe ni matiere nouvelle. La piece va dans le mesh `SM_Ground_`
	// de sa cellule PROPRIETAIRE (la cuisson a range chaque quad par le centre,
	// garde `CleSol` du mode district).
	if (Drape.IsActive())
	{
		FCityQuay& Quay = QuaySingleton();
		const FResolvedSurface* QuaySurf = Surfaces.Resolve(&GSurfCurb);
		if (Quay.bActive && QuaySurf)
		{
			const FVector3f QuayTint(0.85f, 0.85f, 0.80f);
			int32 PoseQuads = 0, PoseCells = 0, TierDm = 0, TierN = 0, TierPieces = 0;
			TArray<FString> QFiles;
			IFileManager::Get().FindFiles(QFiles, *(Quay.Dir / TEXT("quai_*.json")), true, false);
			{
				for (const FString& QFile : QFiles)
				{
					FString QRest = FPaths::GetBaseFilename(QFile);
					QRest.RemoveFromStart(TEXT("quai_"));
					FString QSx, QSy;
					if (!QRest.Split(TEXT("_"), &QSx, &QSy, ESearchCase::CaseSensitive,
						ESearchDir::FromEnd))
					{
						continue;
					}
					const FIntPoint Key(FCString::Atoi(*QSx), FCString::Atoi(*QSy));
					// Mode district : le side-car est deja decoupe a la cellule.
					if (!CelluleVisee(Key))
					{
						continue;
					}
					const FCityQuay::FCell* QC = Quay.Cellule(Key);
					if (!QC || QC->Objets.Num() < 3)
					{
						continue;
					}
					++PoseCells;
					FCityMeshBuilder& QB = GetInKey(GroundCells, Key, bLinearColors, bWorldUVs);
					const FPolygonGroupID Grp = QB.GetOrCreateGroup(QuaySurf->SlotName(),
						QuaySurf->Material);
					// ⭐ SOL DE BERGE — GRADINS ET VOLEES SONT DES OBJETS POSES.
					// Ils ne decoupent plus le sol : le sol est sain, ils s'y
					// appuient, exactement comme un batiment sur son terrain. La
					// promenade s'est elargie sous eux (le mur a recule de
					// nu x 0,70 m) et le bloc remplit la difference.
					for (int32 t = 0; t + 2 < QC->Objets.Num(); t += 3)
					{
						const FVector3f C[3] = { QC->Objets[t], QC->Objets[t + 1],
												 QC->Objets[t + 2] };
						FVector3f Nor = FVector3f::CrossProduct(C[1] - C[0], C[2] - C[0]);
						if (Nor.SizeSquared() < 1e-6f)
						{
							continue;    // marche degeneree : rien a poser
						}
						Nor.Normalize();
						const FVector2f UV[3] = { QC->ObjUVs[t], QC->ObjUVs[t + 1],
												  QC->ObjUVs[t + 2] };
						QB.AddPoly(Grp, C, 3, Nor, UV, QuayTint);
						++PoseQuads;
					}
					TierDm += QC->GradinsDm;
					if (QC->GradinsDm > 0)
					{
						++TierPieces;
						TierN = FMath::Max(TierN, QC->GradinsN);
					}
				}
			}
			// LES GRADINS SONT DESORMAIS TAILLES DANS LA PIECE : ce sont ces
			// compteurs-la que lit le verrou. Ils REMPLACENT ceux des murs de
			// soutenement dans l'emprise (les murs y sont cedes) ; hors emprise,
			// le mecanisme historique reste seul maitre.
			if (TierDm > 0)
			{
				Summary.QuayTierDm += TierDm;
				Summary.QuayTiers += TierN;
				Summary.QuayTierWalls += TierPieces;
			}
			UE_LOG(LogCityImport, Display,
				TEXT("SOL DE BERGE : %d triangles d'OBJET poses sur %d cellules (gradins et volees, %.1f m, %d assises, %d pieces) ; la BANDE, elle, est le sol : %d quads triangules, %d triangles dont %d de face verticale, %d quads masques (le mecanisme a disparu)."),
				PoseQuads, PoseCells, TierDm * 0.1f, TierN, TierPieces,
				Quay.QuadsBande, Quay.Triangles, Quay.TrisVerticaux, 0);
		}
		else if (Quay.bActive)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("BUILDQUAY demande mais bSurfaceMaterials est faux : aucune piece de quai posee."));
		}
	}

	// --- QUAIS V2 : LES ESCALIERS.
	// Meme place et meme dependance que les murs (elle exige le DRAPE : sans
	// relief il n'y a aucun denivele a franchir) et meme hote : la volee vit dans
	// la cellule de RUBANS (SM_Ground_*), a cote du mur qu'elle dessert.
	if (Gen.bStairs && Drape.IsActive())
	{
		const FString StDir = StairsDir(Gen);
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(StDir / TEXT("escaliers_*.json")), true, false);
		// Meme materiau que les murs et les bordures : aucun materiau nouveau, une
		// seule grammaire minerale. La teinte est celle du mur — un escalier de quai
		// est taille dans la meme pierre que le quai.
		const FResolvedSurface* StSurf = Surfaces.Resolve(&GSurfCurb);
		if (!StSurf)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("Escaliers demandes mais bSurfaceMaterials est faux : aucun escalier pose."));
		}
		const FVector3f StTint(0.85f, 0.85f, 0.80f);
		FRenderedGroundZ StRGZ;
		StRGZ.Init(Drape, Gen.GroundGridN, Cell);
		int32 CellsWithStairs = 0;
		int32 StCellsWrongSize = 0;
		double StBakedCellM = 0.0;
		int32 StairQuads = 0;
		// ⭐ QUAIV2 — LA REGLE DU LOT SIMPLIFICATION EST INVERSEE.
		// CHRONOLOGIE, a lire dans cet ordre (elle explique le code) :
		//   02/08 (lot QUAIS)  : les deux volees du corridor sont posees par
		//       `BuildStairs` et VERROUILLEES nominativement.
		//   04/08 (lot SIMPLE) : « les gradins SONT l'escalier » — une volee qui
		//       traverse une emprise `leisure=bleachers` n'est plus rendue ; le
		//       verrou est retire.
		//   04/08 (lot QUAIV2) : DECISION UTILISATEUR — la regle est INVERSEE.
		//       Les volees reviennent, mais RECONSTRUITES PAR LE CONSTRUCTEUR
		//       DE QUAI : une ENTAILLE au profil 70/45 taillee dans la meme
		//       piece, pied sur la PROMENADE (+1,20 sur l'eau), tete sur le
		//       COURONNEMENT — au lieu d'une rampe DRAPEE de 30 m sur la berge.
		//       Le verrou nominatif est RESTAURE et adapte aux nouvelles cotes.
		// Ce que le moteur fait desormais : il ne DRAPE plus une volee que la
		// piece a deja batie. La liste est nominative et vient de la cuisson
		// (`volees_constructeur`) — la REGLE, elle, est nationale et vit dans la
		// cuisson : « toute volee dont l'emprise tombe dans la piece ». Liste
		// vide (dossier Quai absent) = comportement d'avant ce lot.
		const FString StGradinDir = GradinsDir(Gen);
		int32 StGradinsWrongSize = 0;
		int32 StairsInTiers = 0;
		const FCityQuay& StQuay = QuaySingleton();
		for (const FString& File : Files)
		{
			FString Rest = FPaths::GetBaseFilename(File);
			Rest.RemoveFromStart(TEXT("escaliers_"));
			FString Sx, Sy;
			if (!Rest.Split(TEXT("_"), &Sx, &Sy, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				continue;
			}
			if (!CelluleVisee(FIntPoint(FCString::Atoi(*Sx), FCString::Atoi(*Sy))))
			{
				continue;
			}
			TArray<FCityStairs> Flights;
			if (!LoadStairsCell(StDir, FCString::Atoi(*Sx), FCString::Atoi(*Sy),
				CellSizeM, Flights, StCellsWrongSize, StBakedCellM) || Flights.Num() == 0)
			{
				continue;
			}
			++CellsWithStairs;
			const FIntPoint CelluleEsc(FCString::Atoi(*Sx), FCString::Atoi(*Sy));
			TArray<FGradinEmprise> StEmprises;
			LoadGradinsCell(StGradinDir, CelluleEsc.X, CelluleEsc.Y, CellSizeM,
				StEmprises, StGradinsWrongSize);
			for (const FCityStairs& St : Flights)
			{
				// QUAIV2 — LA VOLEE EST-ELLE DEJA BATIE PAR LA PIECE ?
				// (cf. l'en-tete de la passe : la regle SIMPLE est inversee.)
				if (StQuay.bActive && StQuay.VoleesConstructeur.Contains(St.Id))
				{
					++StairsInTiers;
					// NOMINATIF : on dit QUELLE volee, et par qui elle est batie.
					UE_LOG(LogCityImport, Display,
						TEXT("VOLEE RECONSTRUITE PAR LA PIECE cellule=%d_%d source=%s id=%s — entaille 70/45 taillee entre la promenade et le couronnement ; le drapage est donc supprime."),
						CelluleEsc.X, CelluleEsc.Y, *St.Source, *St.Id);
					continue;
				}
				int32 Q = 0;
				const int32 M = BuildStairs(
					GetInKey(GroundCells, CleSol(St.PtsCm[0], CelluleEsc),
						bLinearColors, bWorldUVs),
					St, StRGZ, StSurf, StTint, Q);
				if (M > 0)
				{
					Summary.StairSteps += M;
					StairQuads += Q;
					++Summary.Stairs;
				}
				else
				{
					++Summary.StairsSkipped;
				}
			}
		}
		UE_LOG(LogCityImport, Display,
			TEXT("QUAIS V2 escaliers : %d cellules, %d volees posees (%d marches, %d quads), %d ecartees (pas de denivele exploitable) — dossier '%s'."),
			CellsWithStairs, Summary.Stairs, Summary.StairSteps, StairQuads,
			Summary.StairsSkipped, *StDir);
		UE_LOG(LogCityImport, Display,
			TEXT("QUAIV2 escaliers : %d volees RECONSTRUITES PAR LA PIECE (entaille 70/45 entre promenade et couronnement) et donc non drapees — la regle du lot SIMPLE est inversee (dossier gradins '%s')."),
			StairsInTiers, *StGradinDir);
		if (StCellsWrongSize > 0)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("QUAIS V2 escaliers : %d cellules IGNOREES — leur side-car est cuit pour des cellules de %.0f m et cet import travaille a %.0f m."),
				StCellsWrongSize, StBakedCellM, CellSizeM);
		}
	}

	// --- C2 : LES PONTS. Meme place, meme hote et meme dependance que les murs et
	// les escaliers (le drapage : sans relief il n'y a rien a franchir). Le tablier
	// vit dans la cellule de RUBANS de son PREMIER point — la meme regle que tout le
	// reste, `CleSol`, celle qui evite d'ecraser l'asset de la cellule voisine en
	// mode district (garde du §11.6 du Playbook).
	if (Gen.bBridges && Drape.IsActive())
	{
		const FString BrDir = BridgesDir(Gen);
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(BrDir / TEXT("ponts_*.json")), true, false);
		// Chaussee = l'asphalte des rubans ; sous-face, bandeaux et parapets = la
		// pierre des bordures et des murs. AUCUN materiau nouveau.
		const FResolvedSurface* BrDeck = Surfaces.Resolve(&GSurfAsphalt);
		const FResolvedSurface* BrStone = Surfaces.Resolve(&GSurfCurb);
		if (!BrDeck || !BrStone)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("Ponts demandes mais bSurfaceMaterials est faux : aucun tablier pose."));
		}
		const FVector3f BrTint(0.85f, 0.85f, 0.80f);
		FRenderedGroundZ BrRGZ;
		BrRGZ.Init(Drape, Gen.GroundGridN, Cell);
		int32 CellsWithBridges = 0;
		int32 BrCellsWrongSize = 0;
		double BrBakedCellM = 0.0;
		float DeckM = 0.f;
		for (const FString& File : Files)
		{
			FString Rest = FPaths::GetBaseFilename(File);
			Rest.RemoveFromStart(TEXT("ponts_"));
			FString Sx, Sy;
			if (!Rest.Split(TEXT("_"), &Sx, &Sy, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				continue;
			}
			if (!CelluleVisee(FIntPoint(FCString::Atoi(*Sx), FCString::Atoi(*Sy))))
			{
				continue;
			}
			TArray<FCityBridge> Decks;
			if (!LoadBridgesCell(BrDir, FCString::Atoi(*Sx), FCString::Atoi(*Sy),
				CellSizeM, Drape.AltCapCm, Decks, BrCellsWrongSize, BrBakedCellM) ||
				Decks.Num() == 0)
			{
				continue;
			}
			++CellsWithBridges;
			const FIntPoint CellulePont(FCString::Atoi(*Sx), FCString::Atoi(*Sy));
			for (const FCityBridge& Bd : Decks)
			{
				int32 Q = 0;
				const float M = BuildBridge(
					GetInKey(GroundCells, CleSol(Bd.PtsCm[0], CellulePont),
						bLinearColors, bWorldUVs),
					Bd, BrRGZ, BrDeck, BrStone, BrTint, Gen.bBridgeParapets, Q);
				if (M > 0.f)
				{
					++Summary.Bridges;
					Summary.BridgeQuads += Q;
					DeckM += M;
					if (!Bd.bHasZ)
					{
						++Summary.BridgesZFallback;
					}
				}
				else
				{
					++Summary.BridgesSkipped;
				}
			}
		}
		Summary.BridgeDeckM = FMath::RoundToInt(DeckM);
		UE_LOG(LogCityImport, Display,
			TEXT("PONTS C2 : %d cellules, %d tabliers poses (%d m de tablier, %d quads), %d ecartes, %d au repli de cote ; %d rubans OSM de pont remplaces — dossier '%s'."),
			CellsWithBridges, Summary.Bridges, Summary.BridgeDeckM, Summary.BridgeQuads,
			Summary.BridgesSkipped, Summary.BridgesZFallback,
			Summary.BridgeRibbonsReplaced, *BrDir);
		if (BrCellsWrongSize > 0)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("PONTS C2 : %d cellules IGNOREES — leur side-car est cuit pour des cellules de %.0f m et cet import travaille a %.0f m."),
				BrCellsWrongSize, BrBakedCellM, CellSizeM);
		}
	}

	// --- Dalles de sol PEINTES : grilles 12x12 par cellule, sommets teintes par
	// echantillonnage des surfaces (eau > bois > parc). Toujours residentes, elles
	// portent l'apparence de la map au-dela des distances de cull des films 3D —
	// une seule couche au loin = zero z-fight possible (precision depth GLES).
	TArray<FPaintPoly> PaintPolys;
	if (!SurfacesJsonFilePath.IsEmpty())
	{
		FString SurfJson;
		if (FFileHelper::LoadFileToString(SurfJson, *SurfacesJsonFilePath))
		{
			TSharedPtr<FJsonObject> SurfRoot;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SurfJson), SurfRoot) &&
				SurfRoot.IsValid())
			{
				auto LoadPolys = [&](const TCHAR* Field, auto TintForKind)
				{
					const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
					if (!SurfRoot->TryGetArrayField(Field, Arr))
					{
						return;
					}
					for (const TSharedPtr<FJsonValue>& V : *Arr)
					{
						const TSharedPtr<FJsonObject>& O = V->AsObject();
						FPaintPoly Poly;
						ReadPts(O->GetArrayField(TEXT("pts")), Poly.Pts);
						if (Poly.Pts.Num() < 3)
						{
							continue;
						}
						Poly.Bounds = FBox2D(ForceInit);
						for (const FVector2D& P : Poly.Pts) { Poly.Bounds += P; }
						TintForKind(O, Poly);
						PaintPolys.Add(MoveTemp(Poly));
					}
				};
				LoadPolys(TEXT("water"), [](const TSharedPtr<FJsonObject>&, FPaintPoly& P)
					{ P.Tint = FVector3f(0.16f, 0.30f, 0.38f); P.Priority = 3; });
				LoadPolys(TEXT("green"), [](const TSharedPtr<FJsonObject>& O, FPaintPoly& P)
					{
						const bool bForest = O->GetStringField(TEXT("k")) == TEXT("forest");
						P.Tint = bForest ? FVector3f(0.20f, 0.34f, 0.16f) : FVector3f(0.35f, 0.48f, 0.22f);
						P.Priority = bForest ? 2 : 1;
					});
			}
		}
	}
	const FVector3f SlabBase(0.33f, 0.31f, 0.28f);
	auto SampleGround = [&](const FVector2D& P) -> FVector3f
	{
		int32 BestPrio = 0;
		FVector3f Tint = SlabBase;
		for (const FPaintPoly& Poly : PaintPolys)
		{
			if (Poly.Priority > BestPrio && Poly.Bounds.IsInside(P) && PointInRing(Poly.Pts, P))
			{
				BestPrio = Poly.Priority;
				Tint = Poly.Tint;
			}
		}
		return Tint;
	};
	// Lot B : materiaux PBR DefaultLit en desktop — sol/sentiers/proxys en
	// M_CityGround_PBR (VertexColor lit), rubans en M_CityRoad_PBR (meme
	// T_RoadStrip). Mobile : materiaux unlit historiques, inchanges.
	UMaterialInterface* RoadMat = Gen.bPBRMaterials
		? GetOrCreateRoadPBRMaterial(AssetFolder) : GetOrCreateRoadMaterial(AssetFolder);
	UMaterialInterface* SlabMat = Gen.bPBRMaterials
		? GetOrCreateGroundPBRMaterial(AssetFolder) : WallMat;
	// Grille de sol parametree par le profil : 12x12 plat (mobile) ou 64x64 drape
	// MNT (desktop, sommets Z = AltCmAt - AltCapitole). Fabrique commune : la teinte
	// peinte reste echantillonnee en (X, Y), le Z ne change pas les couleurs.
	const int32 SlabGrid = FMath::Clamp(Gen.GroundGridN, 1, 256);
	// v4 — LA DALLE PORTE LA MATIERE. Resolue une fois : nulle en profil mobile (la
	// dalle reste la grille peinte historique, a l'octet pres) et nulle pour le mesh
	// de COLLISION (jamais rendu — lui donner un slot de revetement serait du gachis).
	const FResolvedSurface* SlabSurf = Surfaces.Resolve(&GSurfSlab);
	// LOT SIMPLIFICATION : combien de quads de dalle ont cede la place a l'ouvrage
	// de berge, et combien de sous-quads l'ouvrage a poses. Une ligne de journal
	// suffit (aucun champ ajoute au resume : pas de changement de layout).
	int32 OuvrageQuadsCedes = 0;
	int32 OuvrageSousQuads = 0;
	// ⭐ SOL DE BERGE : la PIERRE DE QUAI des faces verticales. C'est le materiau
	// des bordures et des murs (`GSurfCurb`) — aucune matiere nouvelle, une seule
	// grammaire minerale. Nul (bSurfaceMaterials faux) = les faces retombent dans
	// le groupe de la dalle plutot que de disparaitre.
	const FResolvedSurface* QuayStone = Surfaces.Resolve(&GSurfCurb);
	auto BuildGroundGrid = [&](FCityMeshBuilder& Builder, const FIntPoint& Key, int32 GridN, bool bPaint,
		const FResolvedSurface* Surf)
	{
		const float Step = Cell / GridN;
		// Groupe de revetement cree UNE fois pour toute la dalle (pas par quad).
		const FPolygonGroupID Group = Surf
			? Builder.GetOrCreateGroup(Surf->SlotName(), Surf->Material) : Builder.WallGroup;
		// ⭐ LOT SIMPLIFICATION — LES QUADS QUI ONT CEDE LA PLACE A L'OUVRAGE.
		// Uniquement sur la grille de RENDU (celle a laquelle la cuisson est
		// accordee) : le mesh de COLLISION, cuit a une autre maille, garde le
		// drapage historique — il n'est jamais rendu.
		const FCityBedCeiling::FGrilles* Ouv = nullptr;
		if (Drape.Bed && Drape.Bed->bProfil && Drape.Bed->bActive
			&& GridN == Drape.Bed->GridN)
		{
			const FCityBedCeiling::FGrilles* G = Drape.Bed->Grille(Key);
			if (G && G->OuvrageK > 0 && G->OuvrageQuads.Num() > 0)
			{
				Ouv = G;
			}
		}
		// ⭐ SOL DE BERGE — DANS LA BANDE, LE QUAD N'EST PLUS UNE GRILLE : C'EST
		// UNE TRIANGULATION CONTRAINTE. Ce n'est PAS un masquage — il n'y a rien
		// derriere : la triangulation couvre le quad ENTIER (la cuisson mesure
		// 0,00e+00 m2 d'ecart a l'aire du quad), coins compris, et ses coins SONT
		// les noeuds de la grille au meme Z. La couture n'est pas propre : elle
		// n'existe pas.
		// Uniquement sur la grille de RENDU (celle a laquelle la cuisson est
		// accordee) : le mesh de COLLISION, cuit a une autre maille, garde le
		// drapage historique — il n'est jamais rendu.
		const FCityQuay& Quay = QuaySingleton();
		const FCityQuay::FCell* QCell =
			(Quay.bActive && GridN == Quay.GridN) ? Quay.Cellule(Key) : nullptr;
		for (int32 GY = 0; GY < GridN; ++GY)
		{
			for (int32 GX = 0; GX < GridN; ++GX)
			{
				if (QCell && QCell->Bande.Contains(GY * GridN + GX))
				{
					continue;    // la triangulation contrainte rend ce quad
				}
				if (Ouv && Ouv->OuvrageQuads.Contains(GY * GridN + GX))
				{
					continue;    // l'ouvrage le rend a sa place, au pas fin
				}
				const float X0 = Key.X * Cell + GX * Step, Y0 = Key.Y * Cell + GY * Step;
				const FVector3f C[4] = {
					FVector3f(X0, Y0, Drape.GroundZ(X0, Y0)),
					FVector3f(X0 + Step, Y0, Drape.GroundZ(X0 + Step, Y0)),
					FVector3f(X0 + Step, Y0 + Step, Drape.GroundZ(X0 + Step, Y0 + Step)),
					FVector3f(X0, Y0 + Step, Drape.GroundZ(X0, Y0 + Step)) };
				// UV0 : historiquement [0,1] PAR QUAD (le materiau de dalle ne lisait
				// que la VertexColor). v4 : UV0 EN METRES MONDE, comme les rubans —
				// c'est ce qui donne au scan sa vraie echelle et, surtout, ce qui fait
				// que la dalle et le ruban qui la recouvre parlent la meme langue.
				FVector2f UV[4] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
				if (Surf)
				{
					for (int32 c = 0; c < 4; ++c)
					{
						UV[c] = FVector2f(C[c].X * 0.01f, C[c].Y * 0.01f);
					}
				}
				FVector3f Cols[4];
				for (int32 c = 0; c < 4; ++c)
				{
					const FVector3f Base = bPaint ? SampleGround(FVector2D(C[c].X, C[c].Y)) : SlabBase;
					Cols[c] = bBakedShade ? Shade(Base, FVector3f(0, 0, 1), 0.f) : Base;
				}
				Builder.AddPolyPerVertexColors(Group, C, 4, FVector3f(0, 0, 1), UV, Cols);
			}
		}
		// ⭐ SOL DE BERGE — LA TRIANGULATION CONTRAINTE, dans le MEME maillage de
		// dalle. Les pans HORIZONTAUX vont dans le groupe de la cellule (meme
		// materiau, meme UV0 metrique monde, meme peinture de sommet : la
		// maquette du sol continue de s'appliquer mot pour mot) ; les pans
		// VERTICAUX — face de quai et face de mur — vont dans le groupe de la
		// PIERRE DE QUAI, celui des bordures et des murs. Aucune classe, aucune
		// matiere nouvelle, et UNE SEULE SURFACE.
		if (QCell && QCell->TriSlot.Num() > 0)
		{
			const FPolygonGroupID Pierre = QuayStone
				? Builder.GetOrCreateGroup(QuayStone->SlotName(), QuayStone->Material)
				: Group;
			const FVector3f PierreTeinte(0.85f, 0.85f, 0.80f);
			for (int32 t = 0; t < QCell->TriSlot.Num(); ++t)
			{
				const bool bVertical = (QCell->TriSlot[t] != 0);
				const FVector3f C[3] = { QCell->Tris[t * 3], QCell->Tris[t * 3 + 1],
										 QCell->Tris[t * 3 + 2] };
				// La normale vient de l'ENROULEMENT (la cuisson l'a ordonne) :
				// aucun test de cote dans le moteur.
				FVector3f Nor = FVector3f::CrossProduct(C[1] - C[0], C[2] - C[0]);
				if (Nor.SizeSquared() < 1e-6f)
				{
					continue;    // triangle degenere : rien a poser
				}
				Nor.Normalize();
				FVector2f UV[3];
				FVector3f Cols[3];
				for (int32 c = 0; c < 3; ++c)
				{
					UV[c] = Surf || bVertical ? QCell->TriUVs[t * 3 + c]
											  : FVector2f((float)c, 0.f);
					const FVector3f Base2 = bVertical
						? PierreTeinte
						: (bPaint ? SampleGround(FVector2D(C[c].X, C[c].Y)) : SlabBase);
					Cols[c] = bBakedShade ? Shade(Base2, Nor, 0.f) : Base2;
				}
				Builder.AddPolyPerVertexColors(bVertical ? Pierre : Group, C, 3, Nor,
					UV, Cols);
				++OuvrageSousQuads;
			}
			OuvrageQuadsCedes += QCell->Bande.Num();
		}
		if (!Ouv)
		{
			return;
		}
		// L'OUVRAGE, pose tel que la cuisson l'a livre : le moteur ne fait que
		// POSER (meme doctrine que les murs, les ponts et l'eau). Meme groupe,
		// meme materiau, meme UV0 metrique monde et meme peinture de sommet que
		// la dalle : aucune classe ni matiere nouvelle, et la maquette du sol
		// (masques) continue de s'appliquer mot pour mot.
		const int32 K = Ouv->OuvrageK;
		const int32 NV = (K + 1) * (K + 1);
		const float Fine = Step / (float)K;
		for (const TPair<int32, int32>& QP : Ouv->OuvrageQuads)
		{
			const int32 GX = QP.Key % GridN, GY = QP.Key / GridN;
			if (GX < 0 || GY < 0 || GX >= GridN || GY >= GridN)
			{
				continue;
			}
			const int32 Base = QP.Value * NV;
			if (!Ouv->OuvrageZ.IsValidIndex(Base + NV - 1))
			{
				continue;
			}
			const float X0 = Key.X * Cell + GX * Step, Y0 = Key.Y * Cell + GY * Step;
			auto ZAt = [&](int32 sx, int32 sy) -> float
			{
				return (float)Ouv->OuvrageZ[Base + sy * (K + 1) + sx] - Drape.AltCapCm;
			};
			for (int32 sy = 0; sy < K; ++sy)
			{
				for (int32 sx = 0; sx < K; ++sx)
				{
					const float XA = X0 + sx * Fine, YA = Y0 + sy * Fine;
					const FVector3f C[4] = {
						FVector3f(XA, YA, ZAt(sx, sy)),
						FVector3f(XA + Fine, YA, ZAt(sx + 1, sy)),
						FVector3f(XA + Fine, YA + Fine, ZAt(sx + 1, sy + 1)),
						FVector3f(XA, YA + Fine, ZAt(sx, sy + 1)) };
					FVector2f UV[4] = { FVector2f(0, 0), FVector2f(1, 0),
										FVector2f(1, 1), FVector2f(0, 1) };
					if (Surf)
					{
						for (int32 c = 0; c < 4; ++c)
						{
							UV[c] = FVector2f(C[c].X * 0.01f, C[c].Y * 0.01f);
						}
					}
					FVector3f Cols[4];
					for (int32 c = 0; c < 4; ++c)
					{
						const FVector3f Base2 = bPaint
							? SampleGround(FVector2D(C[c].X, C[c].Y)) : SlabBase;
						Cols[c] = bBakedShade ? Shade(Base2, FVector3f(0, 0, 1), 0.f) : Base2;
					}
					Builder.AddPolyPerVertexColors(Group, C, 4, FVector3f(0, 0, 1), UV, Cols);
					++OuvrageSousQuads;
				}
			}
			++OuvrageQuadsCedes;
		}
	};
	// J3c maquette : l'instance de la CELLULE, resolue comme une classe de
	// revetement de plus (meme mecanique de slot, meme UV0 metrique monde — c'est
	// exactement ce que le master attend). Une seule difference : son materiau
	// change d'une cellule a l'autre, donc elle se resout dans la boucle.
	FResolvedSurface MaskedSurf;
	MaskedSurf.Class = &GSurfMaskedGround;

	// =========================================================================
	// E2-1 — LE SOL DU DISTRICT CONSTRUIT DEPUIS LE PLAN (`bPlan`).
	//
	// `Doc/Chantier-Plan-de-Ville.md` S8. Quand l'interrupteur est allume, la
	// dalle d'une cellule VISEE n'est plus la grille drapee : ce sont les
	// PARCELLES du plan, chacune a la cote de SA loi de Z. Le reste de la passe
	// (nom d'asset, acteur, collision, streaming) ne change pas d'une virgule —
	// c'est ce qui rend l'A/B lisible.
	//
	// Le plan FAIT FOI : rien n'est infere ici. Un plan absent, incomplet ou a
	// l'empreinte fausse ARRETE la passe (garde E2-0), il ne la degrade pas.
	// =========================================================================
	FPlanVille Plan;
	bool bPlanActif = false;
	FPlanSolStats PlanTotal;
	if (Gen.bPlan)
	{
		FPlanRapport R;
		if (!Plan.Ouvrir(FPlanVille::DossierParDefaut(), R))
		{
			RaiseError(FString::Printf(
				TEXT("bPlan=true mais le plan de ville est REFUSE : %s"), *R.Texte(8)));
			return Summary;
		}
		bPlanActif = true;
		UE_LOG(LogCityImport, Display,
			TEXT("PLAN : sol du district construit DEPUIS LE PLAN (%d cellules au domaine, ")
			TEXT("%d parcelles distinctes au manifeste)."),
			Plan.Index().Cellules.Num(), Plan.Index().ParcellesDistinctes);
	}
	// LES TROIS SOLS — ceux qui existent deja, aucun de plus. La MATIERE du plan
	// choisit le revetement ; l'OUVRAGE garde la PIERRE DE QUAI, exactement la
	// table `ClasseDeProprio` de ce fichier. Les teintes sont celles que la passe
	// peint deja (SlabBase, le vert de `green`, le bleu de `water`).
	const FResolvedSurface* SurfPlanMineral = SlabSurf;
	const FResolvedSurface* SurfPlanVegetal = Surfaces.Resolve(&GSurfGrassCut);
	const FResolvedSurface* SurfPlanEau = Surfaces.Resolve(&GSurfWater);
	const FResolvedSurface* SurfPlanOuvrage = Surfaces.Resolve(&GSurfCurb);
	const FVector3f TintPlanMineral = SlabBase;
	const FVector3f TintPlanVegetal(0.35f, 0.48f, 0.22f);
	const FVector3f TintPlanEau(0.16f, 0.30f, 0.38f);
	auto ClassePlanDe = [](const FPlanParcelle& P) -> int32
	{
		if (P.Proprietaire == EPlanProprio::Ouvrage) { return 3; }
		switch (P.Matiere)
		{
		case EPlanMatiere::Vegetal: return 1;
		case EPlanMatiere::Eau:     return 2;
		default:                    return 0;
		}
	};
	auto PosePlan = [&](FCityMeshBuilder& Builder, const TArray<FPlanSolLot>& Lots)
	{
		for (const FPlanSolLot& Lot : Lots)
		{
			const FResolvedSurface* Surf = SurfPlanMineral;
			FVector3f Tint = TintPlanMineral;
			if (Lot.Classe == 1) { Surf = SurfPlanVegetal; Tint = TintPlanVegetal; }
			else if (Lot.Classe == 2) { Surf = SurfPlanEau; Tint = TintPlanEau; }
			else if (Lot.Classe == 3) { Surf = SurfPlanOuvrage; Tint = TintPlanMineral; }
			const FPolygonGroupID Group = Surf
				? Builder.GetOrCreateGroup(Surf->SlotName(), Surf->Material) : Builder.WallGroup;
			const FVector3f Col = bBakedShade ? Shade(Tint, FVector3f(0, 0, 1), 0.f) : Tint;
			for (const FPlanSolTri& T : Lot.Tris)
			{
				const FVector3f P3[3] = { T.A, T.B, T.C };
				FVector2f UV[3];
				for (int32 i = 0; i < 3; ++i)
				{
					// Meme langue que la dalle actuelle : UV0 en METRES MONDE des
					// que le revetement est resolu, sinon l'echelle historique.
					UV[i] = Surf ? FVector2f(P3[i].X * 0.01f, P3[i].Y * 0.01f)
								 : FVector2f(P3[i].X * 0.0025f, P3[i].Y * 0.0025f);
				}
				Builder.AddPoly(Group, P3, 3, FVector3f(0, 0, 1), UV, Col);
			}
		}
	};

	// Garde de sortie du mode district (doit toujours valoir 0 grace a CleSol) : on ne
	// cree ni n'ecrase JAMAIS l'asset d'une cellule qu'on n'a pas purgee.
	int32 CellulesHorsFiltre = 0;
	for (const FIntPoint& Key : SlabKeys)
	{
		if (!CelluleVisee(Key))
		{
			++CellulesHorsFiltre;
			continue;
		}
		FCityMeshBuilder SlabBuilder;
		SlabBuilder.bLinearColors = bLinearColors;
		if (bWorldUVs)
		{
			SlabBuilder.EnableWorldUV1();
		}
		const FResolvedSurface* CellSurf = SlabSurf;
		if (bMaskedActive && MaskCells.Contains(Key))
		{
			MaskedSurf.Material = LoadCellMaskMaterial(Key);
			if (MaskedSurf.Material)
			{
				CellSurf = &MaskedSurf;
				++Summary.MaskedCells;
			}
		}
		bool bSolDuPlan = false;
		if (bPlanActif)
		{
			FPlanCellule PC;
			FPlanRapport RC;
			if (!Plan.ChargerCellule(Key, PC, RC))
			{
				RaiseError(FString::Printf(
					TEXT("bPlan=true : la cellule (%d, %d) est REFUSEE par le plan — %s"),
					Key.X, Key.Y, *RC.Texte(8)));
				return Summary;
			}
			if (!PC.bConstructible)
			{
				// SECTEUR NON TRANCHE (S12) : on ne batit pas a la place de
				// l'utilisateur. La cellule garde son sol actuel, et on le DIT.
				++Summary.PlanCellulesRefusees;
				UE_LOG(LogCityImport, Display,
					TEXT("PLAN : cellule (%d, %d) NON CONSTRUCTIBLE (%d interface(s) en ")
					TEXT("attente d'arbitrage) — son sol reste celui d'aujourd'hui."),
					Key.X, Key.Y, PC.ArbitragesN);
			}
			else
			{
				// Le pas de subdivision vient de la MAILLE DE SOL EXISTANTE
				// (cote de cellule / GroundGridN) : aucune constante neuve.
				const double PasM = (double)CellSizeM / FMath::Max(SlabGrid, 1);
				TArray<FPlanSolLot> Lots;
				FPlanSolStats St;
				auto ZDrape = [&Drape](double Xcm, double Ycm) -> float
				{
					return Drape.GroundZ(Xcm, Ycm);
				};
				ConstruirePlanSol(PC, Drape.IsActive() ? Drape.AltCapCm : 0.f,
					ZDrape, PasM, FPlanSolPerimetre(), ClassePlanDe, Lots, St);
				PosePlan(SlabBuilder, Lots);
				bSolDuPlan = true;

				++Summary.PlanCellules;
				Summary.PlanParcelles += St.Parcelles;
				Summary.PlanTriangles += St.Triangles;
				Summary.PlanAireM2 += (int32)FMath::RoundToInt(St.AireM2);
				Summary.PlanConstante += St.Constante;
				Summary.PlanProfilTroncon += St.ProfilTroncon;
				Summary.PlanDrapage += St.Drapage;
				Summary.PlanVoirie += St.ParProprio[(int32)EPlanProprio::Voirie];
				Summary.PlanZone += St.ParProprio[(int32)EPlanProprio::Zone];
				Summary.PlanOrganique += St.ParProprio[(int32)EPlanProprio::Organique];
				Summary.PlanBatiment += St.ParProprio[(int32)EPlanProprio::Batiment];
				Summary.PlanOuvrage += St.ParProprio[(int32)EPlanProprio::Ouvrage];
				Summary.PlanTrous += St.AvecTrous;
				Summary.PlanRefusees += St.Refusees;

				// COPLANARITE : l'ecart entre le Z du plan et la surface RENDUE
				// d'aujourd'hui, agrege par proprietaire. C'est la mesure du risque
				// de z-fighting — chiffree, jamais supposee.
				for (int32 K = 0; K < 6; ++K)
				{
					PlanTotal.EcartN[K] += St.EcartN[K];
					PlanTotal.EcartSommeCm[K] += St.EcartSommeCm[K];
					PlanTotal.EcartSous2cm[K] += St.EcartSous2cm[K];
					PlanTotal.EcartMaxCm[K] = FMath::Max(PlanTotal.EcartMaxCm[K], St.EcartMaxCm[K]);
				}
				PlanTotal.PointsFondus += St.PointsFondus;

				for (const FString& D : St.IdsRefuses)
				{
					UE_LOG(LogCityImport, Display,
						TEXT("PLAN : cellule (%d, %d) — parcelle NON CONSTRUITE : %s"),
						Key.X, Key.Y, *D);
				}
				UE_LOG(LogCityImport, Display,
					TEXT("PLAN : cellule (%d, %d) — %d parcelles (%d const / %d profil / %d drapage ; ")
					TEXT("voirie %d, zone %d, organique %d, batiment %d, ouvrage %d), %d trous, ")
					TEXT("%d triangles, %.0f m2, %d refusees."),
					Key.X, Key.Y, St.Parcelles, St.Constante, St.ProfilTroncon, St.Drapage,
					St.ParProprio[(int32)EPlanProprio::Voirie], St.ParProprio[(int32)EPlanProprio::Zone],
					St.ParProprio[(int32)EPlanProprio::Organique],
					St.ParProprio[(int32)EPlanProprio::Batiment],
					St.ParProprio[(int32)EPlanProprio::Ouvrage],
					St.AvecTrous, St.Triangles, St.AireM2, St.Refusees);
			}
		}
		if (!bSolDuPlan)
		{
			BuildGroundGrid(SlabBuilder, Key, SlabGrid, /*bPaint=*/true, CellSurf);
		}
		const FString SlabName = FString::Printf(TEXT("SM_Slab_%d_%d"), Key.X, Key.Y);
		UStaticMesh* SlabMesh = nullptr;
		if (Drape.IsActive() && Gen.GroundCollisionGridN > 0)
		{
			// Collision desktop : la boite plate est fausse des que le terrain ondule
			// — trimesh BASSE RESOLUTION dedie (16x16), cuit a la place du rendu 64x64
			// via ComplexCollisionMesh (le trimesh plein serait du gachis memoire).
			// Le mesh de collision n'est jamais rendu : ni Nanite ni materiau PBR.
			FCityMeshBuilder ColBuilder;
			BuildGroundGrid(ColBuilder, Key, FMath::Clamp(Gen.GroundCollisionGridN, 1, 64), /*bPaint=*/false,
				/*Surf=*/nullptr);
			UStaticMesh* ColMesh = CreateMeshAsset(AssetFolder / (SlabName + TEXT("_Col")), ColBuilder,
				WallMat, WallMat, true);
			SlabMesh = CreateMeshAsset(AssetFolder / SlabName, SlabBuilder, SlabMat, SlabMat,
				true, false, 0.f, ColMesh, bNanite);
		}
		else
		{
			SlabMesh = CreateMeshAsset(AssetFolder / SlabName, SlabBuilder, SlabMat, SlabMat,
				true, true, 60.f, nullptr, bNanite);
		}
		++Summary.GroundMeshes;
		AStaticMeshActor* SlabActor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		SlabActor->GetStaticMeshComponent()->SetStaticMesh(SlabMesh);
		ApplyGroundTextureStreaming(SlabActor->GetStaticMeshComponent());
		SlabActor->SetActorLabel(SlabName);
	}
	if (bPlanActif)
	{
		const int32 OI = (int32)EPlanProprio::Ouvrage;
		Summary.PlanOuvrageSommetsN = PlanTotal.EcartN[OI];
		Summary.PlanOuvrageCoplanaireN = PlanTotal.EcartSous2cm[OI];
		Summary.PlanOuvrageEcartMoyenMm = PlanTotal.EcartN[OI] > 0
			? (int32)FMath::RoundToInt(10.0 * PlanTotal.EcartSommeCm[OI] / PlanTotal.EcartN[OI]) : 0;
		Summary.PlanOuvrageEcartMaxMm = (int32)FMath::RoundToInt(10.0 * PlanTotal.EcartMaxCm[OI]);
		Summary.PlanPointsFondus = PlanTotal.PointsFondus;
		static const TCHAR* NomsProprio[6] =
			{ TEXT("inconnu"), TEXT("batiment"), TEXT("voirie"), TEXT("ouvrage"),
			  TEXT("zone"), TEXT("organique") };
		for (int32 K = 0; K < 6; ++K)
		{
			if (PlanTotal.EcartN[K] == 0) { continue; }
			UE_LOG(LogCityImport, Display,
				TEXT("PLAN COPLANARITE %s : %d sommets, ecart au sol rendu moyen %.1f cm, ")
				TEXT("max %.1f cm, dont %d (%.1f %%) sous 2 cm (seuil du socle anti-z-fight)."),
				NomsProprio[K], PlanTotal.EcartN[K],
				PlanTotal.EcartSommeCm[K] / PlanTotal.EcartN[K], PlanTotal.EcartMaxCm[K],
				PlanTotal.EcartSous2cm[K],
				100.0 * PlanTotal.EcartSous2cm[K] / PlanTotal.EcartN[K]);
		}
	}
	UE_LOG(LogCityImport, Display,
		TEXT("SOL DE BERGE : %d quads de dalle sont rendus par la TRIANGULATION CONTRAINTE (%d triangles poses dans le meme maillage de dalle). "
			 "Ce ne sont pas des quads masques : la triangulation couvre le quad ENTIER, coins compris, et ses coins SONT les noeuds de la grille."),
		OuvrageQuadsCedes, OuvrageSousQuads);

	// -----------------------------------------------------------------------------
	// ⭐ PARTITION ③ LES BANDES EN RUBANS  +  ④ LA LOI D'INTERFACE AU SOL DE VILLE.
	//
	// Les 69 241 m2 de bandes annexees cessent d'etre du sol « reste » drape comme
	// de l'organique : chacune est POSEE, en ruban, sur sa LIGNE PORTEUSE, au Z lu
	// AU CONTACT de son proprietaire, avec la classe de revetement de ce
	// proprietaire — une classe qui existe deja (aucune matiere nouvelle).
	//
	// La composition est ADDITIVE (13.2) : la dalle reste COMPLETE dessous. On ne
	// cede aucun quad, on ne decoupe rien — c'est precisement ce qui a echoue dix
	// fois. Le ruban se pose, et son BORD LIBRE recoit sa couture : une face
	// verticale cousue sommet pour sommet jusqu'a la surface voisine, orientee par
	// les deux surfaces elles-memes.
	//
	// Les rubans vivent dans les cellules de RUBANS (SM_Ground_*, sans collision) :
	// une bande n'a rien a faire dans la dalle porteuse, exactement comme une
	// bordure ou un tiret.
	// -----------------------------------------------------------------------------
	if (PartitionSingleton().bActive)
	{
		const double TPart = FPlatformTime::Seconds();
		const FCityPartition& Part = PartitionSingleton();
		FRenderedGroundZ PartRGZ;
		PartRGZ.Init(Drape, Gen.GroundGridN, Cell);
		const float PasCm = FMath::Max(Gen.PartitionStepM, 0.05f) * 100.f;
		const float CollierCm = FMath::Max(Gen.PartitionCollarM, 0.f) * 100.f;
		const FResolvedSurface* Pierre = Surfaces.Resolve(&GSurfCurb);
		const FVector3f TeinteMinerale(0.85f, 0.85f, 0.80f);
		float CoutureCm = 0.f;
		float MarcheMaxCm = 0.f;
		int32 ParProprio[5] = { 0, 0, 0, 0, 0 };
		double AireM2 = 0.0, AireNonRuban = 0.0;
		// La borne de largeur vient de la CARTE, pas du moteur. Carte muette =
		// aucune borne (on ne devine pas une regle qui n'a pas ete publiee).
		const float LargeurMaxCm = (Part.BandeMaxM > 0.f) ? Part.BandeMaxM * 100.f : 0.f;

		for (const FCityPartition::FBande& B : Part.Bandes)
		{
			if (!CelluleVisee(B.Cellule))
			{
				continue;
			}
			const FSurfaceClass* Classe = ClasseDeProprio(B.Proprio);
			if (!Classe || B.Lignes.Num() == 0)
			{
				++Summary.PartitionBandesSkipped;
				continue;
			}
			const FResolvedSurface* Surf = Surfaces.Resolve(Classe);
			bool bPosee = false;
			bool bNonRuban = false;
			for (const TArray<FVector2D>& Ligne : B.Lignes)
			{
				TArray<FVector2D> Axe, BordLibre;
				TArray<float> ZLigne;
				float LargeurCm = 0.f;
				ERubanRaison Raison = ERubanRaison::TropFine;
				TArray<float> Echelle;
				int32 Rabotes = 0;
				if (!RubanDeBande(B, Ligne, PasCm, CollierCm, LargeurMaxCm,
						Gen.CoutureSondeCm, PartRGZ,
						Axe, ZLigne, LargeurCm, BordLibre, Raison, Echelle, Rabotes))
				{
					bNonRuban = bNonRuban || (Raison == ERubanRaison::NonRuban);
					continue;
				}
				// Le socle : `PartitionLiftCm` au lieu des 55 cm historiques (la
				// dalle est DRAPEE, il n'y a plus rien a survoler), et l'index 0 —
				// deux bandes ne se recouvrent jamais (c'est une PARTITION), le
				// micro-jitter anti-coplanarite n'a personne a departager.
				const float ZRuban = Gen.PartitionLiftCm + (Surf ? Surf->Class->ZClassCm : 0.f);
				Summary.PartitionFlipsFixed += Rabotes;

				// ⛔ LOI DES NAPPES (13.1) APPLIQUEE AU RUBAN : on COUPE sur les
				// MARCHES du proprietaire au lieu de tendre un quad entre deux
				// niveaux. Un quad tendu sur 3 m de denivele est une surface
				// reglee — a l'image, une voile oblique plantee dans le sol.
				// Chaque troncon est alors PLAT ou en pente douce, et la couture
				// ferme le pas verticalement.
				TArray<int32> Coupures;
				Coupures.Add(0);
				if (Gen.PartitionMarcheCm > 0.f)
				{
					for (int32 i = 0; i + 1 < ZLigne.Num(); ++i)
					{
						if (FMath::Abs(ZLigne[i + 1] - ZLigne[i]) > Gen.PartitionMarcheCm)
						{
							Coupures.Add(i + 1);
							++Summary.PartitionRubansCoupes;
						}
					}
				}
				Coupures.Add(ZLigne.Num());
				for (int32 c = 0; c + 1 < Coupures.Num(); ++c)
				{
					const int32 D = Coupures[c], F2 = Coupures[c + 1];
					if (F2 - D < 2)
					{
						continue;   // un troncon d'un seul sommet n'est pas un ruban
					}
					TArray<FVector2D> AxeT, BordT;
					TArray<float> ZT, EchT;
					for (int32 i = D; i < F2; ++i)
					{
						AxeT.Add(Axe[i]);
						BordT.Add(BordLibre[i]);
						ZT.Add(ZLigne[i]);
						EchT.Add(Echelle.IsValidIndex(i) ? Echelle[i] : 1.f);
					}
					BuildRoad(GetInKey(GroundCells, CleSol(AxeT[0], B.Cellule),
							bLinearColors, bWorldUVs),
						// ⚠️ LE TYPE N'EST PAS DECORATIF. `BuildRoad` ajoute 1,70 m de
						// RIVE de chaque cote a tout ce qui n'est pas une voie
						// pietonne (`WalkW`) : une bande de 0,50 m se retrouvait
						// alors posee sur 3,90 m — d'ou des nappes qui debordaient
						// sur la pelouse et, la ligne porteuse tournant serre, des
						// quads RETOURNES (mesure : normale geometrique nz = -1,00
						// sur la plupart des grands triangles neufs, donc dos
						// tourne, donc invisibles : le « coin sombre »).
						// `path` est le type EXISTANT qui dit « pas de rive » —
						// c'est exactement ce qu'est une bande.
						AxeT, LargeurCm, TEXT("path"), /*RoadIndex=*/0, &ZT,
						bBakedShade, Surf, nullptr, nullptr, nullptr, nullptr, nullptr,
						nullptr, Gen.PartitionLiftCm, &EchT);
					bPosee = true;
					++Summary.PartitionTroncons;

					// ④ LA COUTURE DU BORD LIBRE. Haut = la surface du ruban ; bas
					// = la surface voisine, MESUREE sous le bord. La face regarde
					// vers l'exterieur de la bande, direction donnee par la
					// geometrie — jamais par un test de normale (13.3).
					if (Gen.bPartitionCoutures)
					{
						TArray<float> ZHaut, ZBas;
						ZHaut.SetNumUninitialized(BordT.Num());
						ZBas.SetNumUninitialized(BordT.Num());
						for (int32 i = 0; i < BordT.Num(); ++i)
						{
							ZHaut[i] = ZT[i] + ZRuban;
							ZBas[i] = PartRGZ.At(BordT[i].X, BordT[i].Y);
						}
						const FVector2D DirBas =
							(BordT[BordT.Num() / 2] - AxeT[AxeT.Num() / 2]).GetSafeNormal();
						Summary.CoutureQuads += CoudreFace(
							GetInKey(GroundCells, CleSol(BordT[0], B.Cellule),
								bLinearColors, bWorldUVs),
							BordT, ZHaut, ZBas, Gen.CoutureSeuilCm, Pierre,
							TeinteMinerale, CoutureCm, Gen.CoutureHauteurMaxCm,
							Summary.CoutureTropHaute, MarcheMaxCm, DirBas);
					}
				}
			}
			if (bPosee)
			{
				++Summary.PartitionBandes;
				AireM2 += B.AireM2;
				ParProprio[(int32)B.Proprio] += 1;
			}
			else
			{
				++Summary.PartitionBandesSkipped;
				if (bNonRuban)
				{
					++Summary.PartitionBandesNonRuban;
					AireNonRuban += B.AireM2;
				}
				else
				{
					++Summary.PartitionBandesTropFines;
				}
			}
		}
		Summary.PartitionBandesM2 = FMath::RoundToInt(AireM2);
		Summary.PartitionBandesNonRubanM2 = FMath::RoundToInt(AireNonRuban);

		// ④ LES FRONTIERES DE LA CARTE. Chaque run zone|organique est SONDE de part
		// et d'autre sur la surface rendue : la ou les deux surfaces presentent une
		// MARCHE, elle est cousue ; la ou elles se touchent deja, il n'y a rien a
		// coudre et c'est un RESULTAT, pas un echec — il se compte a part.
		if (Gen.bPartitionCoutures)
		{
			const double SondeCm = (double)FMath::Max(Gen.CoutureSondeCm, 1.f);
			for (const FCityPartition::FFrontiere& F : Part.Frontieres)
			{
				if (!CelluleVisee(F.Cellule) || F.Poly.Num() < 2)
				{
					continue;
				}
				++Summary.CoutureRuns;
				FProfilZ P;
				EchantillonnerProfil(F.Poly, PasCm, PartRGZ, P);
				if (P.Pts.Num() < 2)
				{
					continue;
				}
				TArray<FVector2D> Nrm;
				NormalesSommet(P.Pts, Nrm);
				TArray<float> ZA, ZB;
				ZA.SetNumUninitialized(P.Pts.Num());
				ZB.SetNumUninitialized(P.Pts.Num());
				double Somme = 0.0;
				for (int32 i = 0; i < P.Pts.Num(); ++i)
				{
					const FVector2D Pa = P.Pts[i] + Nrm[i] * SondeCm;
					const FVector2D Pb = P.Pts[i] - Nrm[i] * SondeCm;
					ZA[i] = PartRGZ.At(Pa.X, Pa.Y);
					ZB[i] = PartRGZ.At(Pb.X, Pb.Y);
					Somme += (double)(ZA[i] - ZB[i]);
				}
				// Le HAUT est le cote qui domine EN MOYENNE sur tout le run : une
				// couture ne change pas de sens au milieu d'elle-meme.
				const bool bAEnHaut = (Somme >= 0.0);
				TArray<float> ZHaut = bAEnHaut ? ZA : ZB;
				TArray<float> ZBas = bAEnHaut ? ZB : ZA;
				float Marche = 0.f;
				for (int32 i = 0; i < ZHaut.Num(); ++i)
				{
					Marche = FMath::Max(Marche, ZHaut[i] - ZBas[i]);
				}
				if (Marche <= Gen.CoutureSeuilCm)
				{
					++Summary.CoutureSansMarche;
					continue;
				}
				const FVector2D DirBas = (bAEnHaut ? -1.0 : 1.0)
					* Nrm[Nrm.Num() / 2].GetSafeNormal();
				Summary.CoutureQuads += CoudreFace(
					GetInKey(GroundCells, CleSol(P.Pts[0], F.Cellule),
						bLinearColors, bWorldUVs),
					P.Pts, ZHaut, ZBas, Gen.CoutureSeuilCm, Pierre,
					TeinteMinerale, CoutureCm, Gen.CoutureHauteurMaxCm,
					Summary.CoutureTropHaute, MarcheMaxCm, DirBas);
			}
		}
		Summary.CoutureDm = FMath::RoundToInt(CoutureCm * 0.1f);
		Summary.CoutureMarcheMaxCm = FMath::RoundToInt(MarcheMaxCm);
		UE_LOG(LogCityImport, Display,
			TEXT("PARTITION ③ BANDES : %d posees en ruban (%d m2) — ouvrage %d, voirie %d, "
				 "batiment %d, zone %d ; %d ECARTEES = %d SUB-CENTIMETRIQUES (pas de donnee, "
				 "pas d'objet) + %d NON-RUBAN (%d m2 : plaques compactes qui effleurent leur "
				 "proprietaire, largeur deduite > BANDE_MAX_M = %.1f m publie par la carte — "
				 "DETTE NOMMEE pour le chemin POLYGONE). Composition ADDITIVE : la dalle reste "
				 "complete dessous, aucun quad cede."),
			Summary.PartitionBandes, Summary.PartitionBandesM2,
			ParProprio[(int32)FCityPartition::EProprio::Ouvrage],
			ParProprio[(int32)FCityPartition::EProprio::Voirie],
			ParProprio[(int32)FCityPartition::EProprio::Batiment],
			ParProprio[(int32)FCityPartition::EProprio::Zone],
			Summary.PartitionBandesSkipped, Summary.PartitionBandesTropFines,
			Summary.PartitionBandesNonRuban, Summary.PartitionBandesNonRubanM2,
			Part.BandeMaxM);
		UE_LOG(LogCityImport, Display,
			TEXT("PARTITION ③ ANTI-RETOURNEMENT : %d sommets rabotes (meme mecanisme que "
				 "RetainingWallFlipsFixed) — sans lui, le ruban se plie en NOEUD PAPILLON "
				 "des que la ligne porteuse tourne plus serre que sa largeur."),
			Summary.PartitionFlipsFixed);
		UE_LOG(LogCityImport, Display,
			TEXT("PARTITION ③ LOI DES NAPPES : %d coupures sur MARCHE (> %.0f cm par "
				 "echantillon de %.2f m) — %d troncons poses. Aucun quad n'est tendu entre "
				 "deux niveaux : la ou le proprietaire change de niveau, le ruban est COUPE "
				 "et la couture ferme le pas."),
			Summary.PartitionRubansCoupes, Gen.PartitionMarcheCm, Gen.PartitionStepM,
			Summary.PartitionTroncons);
		UE_LOG(LogCityImport, Display,
			TEXT("PARTITION ④ LOI D'INTERFACE : %d quads de couture, %.1f m cousus ; "
				 "%d frontieres examinees dont %d SANS MARCHE mesurable (les deux surfaces "
				 "se touchent deja sous %.0f cm) ; %d segments NON cousus car au-dela d'UN "
				 "NIVEAU (%.0f cm) — ce sont des OUVRAGES deja batis, pas des coutures ; "
				 "marche la plus haute rencontree %.2f m — %.1f s."),
			Summary.CoutureQuads, CoutureCm * 0.01f, Summary.CoutureRuns,
			Summary.CoutureSansMarche, Gen.CoutureSeuilCm,
			Summary.CoutureTropHaute, Gen.CoutureHauteurMaxCm, MarcheMaxCm * 0.01f,
			FPlatformTime::Seconds() - TPart);
	}

	// --- Rubans routiers : SANS collision (films visuels 55-80 cm au-dessus de la
	// dalle porteuse, dont la boite monte a 60 cm) et cullables a ~2 km cote runtime.
	// Slot Wall = sentiers (vertex color) ; slot Glass = rubans textures.
	for (auto& Pair : GroundCells)
	{
		if (!CelluleVisee(Pair.Key))
		{
			++CellulesHorsFiltre;
			continue;
		}
		// E2-1 — EN MODE PLAN, LE RUBAN ROUTIER NE SE POSE PAS.
		// Le ruban est un film pose 55-80 cm au-dessus de la DALLE DRAPEE ; le
		// plan, lui, publie la chaussee comme une PARCELLE `voirie` a la cote de
		// son profil regularise. Les deux ensemble donneraient, selon l endroit,
		// un ruban COLLE (la contre-preuve Z du plan mesure une mediane de
		// 0,3 mm entre profil et MNT : z-fighting garanti) ou un ruban EN L AIR
		// (p95 = 2,05 m, max 9,56 m). Ce n est pas une fonction supprimee :
		// c est la meme chaussee, DITE par le plan au lieu d etre deduite.
		// Hors mode plan, RIEN ne change.
		if (bPlanActif)
		{
			++Summary.PlanRubansOmis;
			continue;
		}
		const FString Name = FString::Printf(TEXT("SM_Ground_%d_%d"), Pair.Key.X, Pair.Key.Y);
		UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / Name, *Pair.Value, SlabMat, RoadMat, false,
			false, 0.f, nullptr, bNanite);
		++Summary.GroundMeshes;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ApplyGroundTextureStreaming(Actor->GetStaticMeshComponent());
		Actor->SetActorLabel(Name);
	}
	if (CellulesHorsFiltre > 0)
	{
		// Display : ce n'est pas une erreur de spec, c'est un compteur de non-regression.
		// S'il devient non nul, c'est qu'un chemin de geometrie echappe encore a CleSol.
		UE_LOG(LogCityImport, Display,
			TEXT("MODE DISTRICT : %d cellule(s) HORS FILTRE ecartees a la sortie (doit valoir 0)."),
			CellulesHorsFiltre);
	}
	for (auto& Pair : ProxyCells)
	{
		const FString Name = FString::Printf(TEXT("SM_Proxy_%d_%d"), Pair.Key.X, Pair.Key.Y);
		UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / Name, *Pair.Value, SlabMat,
			Gen.bPBRMaterials ? SlabMat : GlassMat, false, false, 0.f, nullptr, bNanite);
		++Summary.ProxyMeshes;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ApplyGroundTextureStreaming(Actor->GetStaticMeshComponent());
		Actor->SetActorLabel(Name);
	}

	// --- Blocs de streaming : un sous-niveau par BlockSizeM, batiments detailles ---
	TMap<FIntPoint, TArray<FIntPoint>> CellsByBlock;
	for (auto& Pair : BldgCells)
	{
		const FIntPoint BlockKey(
			FMath::FloorToInt(Pair.Key.X * Cell / Block),
			FMath::FloorToInt(Pair.Key.Y * Cell / Block));
		CellsByBlock.FindOrAdd(BlockKey).Add(Pair.Key);
	}
	for (auto& Pair : CellsByBlock)
	{
		ULevelStreaming* Streaming = Blocks.FindRef(Pair.Key);
		if (!Streaming)
		{
			// CreateNewStreamingLevel attend un chemin de PACKAGE (il convertit lui-meme
			// en nom de fichier) — lui passer un fichier deja converti echoue en silence.
			const FString Pkg = FString::Printf(TEXT("%s/L_T10_B_%d_%d"), *BlocksFolder, Pair.Key.X, Pair.Key.Y);
			Streaming = UEditorLevelUtils::CreateNewStreamingLevel(
				ULevelStreamingDynamic::StaticClass(), Pkg, false);
			if (!Streaming)
			{
				RaiseError(FString::Printf(TEXT("Cannot create streaming level '%s'."), *Pkg));
				return Summary;
			}
			Blocks.Add(Pair.Key, Streaming);
		}
		if (ULevelStreamingDynamic* Dyn = Cast<ULevelStreamingDynamic>(Streaming))
		{
			// LOT A-ter : CHARGE et VISIBLE au runtime DES LA CREATION. Les valeurs par
			// defaut (false/false) rendaient la ville INVISIBLE en Play a chaque regeneration
			// streamee — piege paye TROIS fois le 31/07 et rattrape a chaque coup par un
			// script de post-traitement (SOL2/fix_pie_sol2.py). Ca se decide ICI, pas dans
			// une checklist : une propriete par sous-niveau ne survit pas a sa recreation.
			Dyn->bInitiallyLoaded = true;
			Dyn->bInitiallyVisible = true;
		}
		else
		{
			// V4 : un sous-niveau d'une AUTRE classe (heritage) ne recevait rien du
			// tout, et le verificateur ne le voyait pas — il ne relisait que les flags
			// qu'il venait d'ecrire. On le DIT au lieu de le taire.
			UE_LOG(LogCityImport, Warning,
				TEXT("Sous-niveau '%s' de classe %s (pas ULevelStreamingDynamic) : bInitiallyLoaded/Visible NON poses."),
				*Streaming->GetWorldAssetPackageName(), *Streaming->GetClass()->GetName());
		}
		// ---------------------------------------------------------------------
		// V4 — LA VRAIE CAUSE DE LA RECIDIVE « batiments invisibles en Play ».
		//
		// bInitiallyLoaded / bInitiallyVisible ne sont PAS l'etat : ce sont des
		// GRAINES, recopiees dans bShouldBeLoaded / bShouldBeVisible par
		// ULevelStreamingDynamic::PostLoad() — et seulement dans un monde de JEU. Or
		// le monde PIE n'est pas charge depuis le disque : c'est un DUPLICATA du monde
		// editeur. PostLoad n'y rejoue pas, et le duplicata herite donc de
		// bShouldBeLoaded / bShouldBeVisible tels quels, c'est-a-dire FALSE, la valeur
		// par defaut d'un sous-niveau fraichement cree. Poser les seules graines
		// « marchait » apres un redemarrage d'editeur et ratait dans la session qui
		// venait de generer : exactement le symptome intermittent paye trois fois.
		//
		// On pose donc AUSSI l'etat effectif. Il est serialise avec la map, ce qui rend
		// le correctif verifiable DEPUIS LE .umap SAUVE — le verificateur d'avant etait
		// tautologique : il relisait, dans la meme session, ce qu'il venait d'ecrire.
		Streaming->SetShouldBeLoaded(true);
		Streaming->SetShouldBeVisible(true);
		// J3b : visibilite EDITEUR distincte des flags runtime ci-dessus — elle se
		// sauve avec la map ; invisible, la ville n'affiche que ses proxys.
		Streaming->SetShouldBeVisibleInEditor(true);
		ULevel* BlockLevel = Streaming->GetLoadedLevel();
		if (!BlockLevel)
		{
			World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
			BlockLevel = Streaming->GetLoadedLevel();
		}
		if (BlockLevel)
		{
			UEditorLevelUtils::SetLevelVisibility(BlockLevel, true, false);
		}
		if (!BlockLevel)
		{
			RaiseError(FString::Printf(TEXT("Streaming level '%s' is not loaded in editor — open the map with all sublevels loaded and retry."),
				*Streaming->GetWorldAssetPackageName()));
			return Summary;
		}
		for (const FIntPoint& CellKey : Pair.Value)
		{
			const FString Name = FString::Printf(TEXT("SM_Bldg_%d_%d"), CellKey.X, CellKey.Y);
			auto SpawnBldg = [&](const FString& ActorName, UStaticMesh* Mesh)
			{
				++Summary.BuildingMeshes;
				FActorSpawnParameters SpawnParams;
				SpawnParams.OverrideLevel = BlockLevel;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
					AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				Actor->SetActorLabel(ActorName);
			};
			if (bDesktopBldg)
			{
				// Lot B : murs geometriques (atlas PBR, Nanite si demande) ; vitres
				// dans un mesh SEPARE si bSplitWallGlass — Nanite AUSSI (J2e,
				// retours utilisateur du 25/07 : le verre est opaque, et des vitres
				// non-Nanite devant des murs Nanite « flottent » aux transitions
				// LOD), et AVEC collision (l'embrasure est un vrai trou, la vitre
				// doit arreter le drone). Cellule sans fenetre : pas de mesh Glass.
				UMaterialInterface* BldgWallMat = Gen.bPBRMaterials
					? GetOrCreateWallPBRMaterial(AssetFolder, Gen.AtlasSizePx) : WallMat;
				UMaterialInterface* BldgGlassMat = Gen.bPBRMaterials
					? GetOrCreateGlassPBRMaterial(AssetFolder) : GlassMat;
				// Verrou 2 : le mesh de collision dedie (prismes) est cree d'abord puis
				// cable en ComplexCollisionMesh du mesh opaque — pattern _Col du sol.
				// Sans lui, la collision du Wall Nanite viendrait du fallback decime.
				UStaticMesh* ColMesh = nullptr;
				if (TUniquePtr<FCityMeshBuilder>* ColB = BldgColCells.Find(CellKey))
				{
					UMaterialInterface* DefaultMat = UMaterial::GetDefaultMaterial(MD_Surface);
					ColMesh = CreateMeshAsset(AssetFolder / (Name + TEXT("_Col")), **ColB,
						DefaultMat, DefaultMat, true);
					// Finir sa compilation TOUT DE SUITE : a la regeneration en place, le
					// chargement du Wall deja cable sur disque peut survenir pendant un
					// flush de chargement — finir la compilation du _Col DEPUIS ce contexte
					// declenche l'ensure « Overriding GIsEditorLoadingPackage » (attrape
					// par le test sentinelle au 2e run).
					FStaticMeshCompilingManager::Get().FinishCompilation(
						TArrayView<UStaticMesh* const>(&ColMesh, 1));
					++Summary.BuildingColMeshes;
				}
				if (Gen.bSplitWallGlass)
				{
					SpawnBldg(Name + TEXT("_Wall"), CreateMeshAsset(AssetFolder / (Name + TEXT("_Wall")),
						*BldgCells[CellKey], BldgWallMat, BldgWallMat, true, false, 0.f, ColMesh, bNanite));
					TUniquePtr<FCityMeshBuilder>* GlassB = BldgGlassCells.Find(CellKey);
					if (GlassB && (*GlassB)->QuadCount > 0)
					{
						SpawnBldg(Name + TEXT("_Glass"), CreateMeshAsset(AssetFolder / (Name + TEXT("_Glass")),
							**GlassB, BldgGlassMat, BldgGlassMat, true, false, 0.f, nullptr, bNanite));
					}
				}
				else
				{
					SpawnBldg(Name, CreateMeshAsset(AssetFolder / Name, *BldgCells[CellKey],
						BldgWallMat, BldgGlassMat, true, false, 0.f, ColMesh, bNanite));
				}
			}
			else
			{
				// Facades texturees (slot Wall) + toits unis en vertex color (slot Glass).
				SpawnBldg(Name, CreateMeshAsset(AssetFolder / Name, *BldgCells[CellKey],
					GetOrCreateFacadeMaterial(AssetFolder), WallMat, true));
			}
		}
		BlockLevel->MarkPackageDirty();
		// Sauver le bloc TOUT DE SUITE puis le masquer : l'editeur ne cumule jamais la
		// ville entiere en VRAM (2 crashs GPU TerminateOnGPUCrash payes juste apres
		// l'import, fenetre minimisee comprise — l'upload des ressources + le premier
		// tick rendu suffisaient a tuer le device D3D12).
		FEditorFileUtils::SaveLevel(BlockLevel);
		UEditorLevelUtils::SetLevelVisibility(BlockLevel, false, false, ELevelVisibilityDirtyMode::DontModify);
		++Summary.StreamingBlocks;
	}
	UEditorLevelUtils::MakeLevelCurrent(World->PersistentLevel, true);

	// --- Arbres : residents, un mesh + un HISM (identique a ImportCityDistrict) ---
	// Mode district : l'acteur CityTrees est UNIQUE pour toute la ville (un seul HISM,
	// pas de decoupage par cellule) — il n'a donc pas ete detruit par la purge bornee,
	// et le reconstruire ici DOUBLERAIT les arbres. On le laisse tel quel et on le dit.
	// (Sans consequence sur le pipeline Sol2 : sa section "trees" est vide, toute la
	// vegetation passe par ImportVegetation.)
	const TArray<TSharedPtr<FJsonValue>>* TreesJson = nullptr;
	if (bCellFilter && Root->TryGetArrayField(TEXT("trees"), TreesJson) && TreesJson->Num() > 0)
	{
		UE_LOG(LogCityImport, Display,
			TEXT("Mode district : les %d arbres residents (CityTrees) sont CONSERVES tels quels — ")
			TEXT("cet acteur n'est pas decoupe par cellule."), TreesJson->Num());
	}
	TreesJson = nullptr;
	if (!bCellFilter && Root->TryGetArrayField(TEXT("trees"), TreesJson) && TreesJson->Num() > 0)
	{
		FCityMeshBuilder TreeBuilder;
		BuildTree(TreeBuilder);
		UStaticMesh* TreeMesh = CreateMeshAsset(AssetFolder / TEXT("SM_CityTree"), TreeBuilder, WallMat, GlassMat);
		WallMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		GlassMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);

		AActor* TreeActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		USceneComponent* Root2 = NewObject<USceneComponent>(TreeActor, TEXT("Root"), RF_Transactional);
		TreeActor->SetRootComponent(Root2);
		TreeActor->AddInstanceComponent(Root2);
		Root2->RegisterComponent();
		Root2->SetWorldLocation(Location);
		UHierarchicalInstancedStaticMeshComponent* Hism =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(TreeActor, TEXT("Trees"), RF_Transactional);
		Hism->SetStaticMesh(TreeMesh);
		// LOT PIE : AVANT RegisterComponent — sinon les corps sont deja crees.
		int32 NbVegSansCollision = 0;
		PoserCollisionVegetation(Hism, Gen, NbVegSansCollision);
		Hism->SetupAttachment(Root2);
		TreeActor->AddInstanceComponent(Hism);
		Hism->RegisterComponent();
		UE_LOG(LogCityImport, Display,
			TEXT("LOT PIE : arbres residents — %d composant(s) pose(s) SANS COLLISION."),
			NbVegSansCollision);
		int32 Index = 0;
		// LOT VELOCITE (L3) : les transforms sont accumulees puis posees en UN SEUL
		// AddInstances. Voir le commentaire de fond dans ImportVegetation : un
		// AddInstance unitaire reconstruit l'arbre spatial du HISM a CHAQUE instance.
		TArray<FTransform> TreeXf;
		TreeXf.Reserve(TreesJson->Num());
		for (const TSharedPtr<FJsonValue>& V : *TreesJson)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() < 2)
			{
				continue;
			}
			const float Yaw = FMath::Frac(FMath::Sin(Index * 78.233f) * 12543.21f) * 360.f;
			const float Scale = 0.8f + 0.5f * FMath::Frac(FMath::Sin(Index * 39.11f) * 6543.87f);
			const double Tx = C[0]->AsNumber() * 100.0, Ty = C[1]->AsNumber() * 100.0;
			TreeXf.Add(FTransform(FRotator(0, Yaw, 0),
				FVector(Tx, Ty, Drape.GroundZ(Tx, Ty)), FVector(Scale)));
			++Summary.Trees;
			++Index;
		}
		if (TreeXf.Num() > 0)
		{
			Hism->AddInstances(TreeXf, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
		}
		TreeActor->SetActorLabel(TEXT("CityTrees"));
	}

	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	// Sauvegarde generale AVANT de rendre la main : si l'editeur crashe au premier
	// tick rendu apres l'outil, tout est deja sur disque.
	FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave=*/false, /*bSaveMapPackages=*/true,
		/*bSaveContentPackages=*/true);
	UE_LOG(LogCityImport, Display,
		TEXT("Ville streamee : %d batiments, %d routes, %d arbres — %d sols, %d proxys, %d meshes detail, %d collisions batiments, %d blocs, %d patchs de carrefour, %d quads de bordure, %d passages pietons (%d reportes), %d orphelins ecartes, %d murs de soutenement (%d quads). Tout est sauve."),
		Summary.Buildings, Summary.Roads, Summary.Trees, Summary.GroundMeshes, Summary.ProxyMeshes,
		Summary.BuildingMeshes, Summary.BuildingColMeshes, Summary.StreamingBlocks,
		Summary.JunctionPatches, Summary.CurbQuads, Summary.Crossings, Summary.CrossingsDeferred,
		Summary.OrphanRibbons, Summary.RetainingWalls, Summary.RetainingWallQuads);
	if (Summary.MaskedCells > 0)
	{
		UE_LOG(LogCityImport, Display,
			TEXT("Maquette du sol : %d dalles peintes, %d rubans de chaussee supprimes, %d ponts, %d tirets axiaux."),
			Summary.MaskedCells, Summary.GroundRibbonsSkipped, Summary.BridgeRibbons,
			Summary.AxialDashes);
	}
	// FRONTIERE : ce que le plafond du lit a reellement lu. Un compteur, pas un
	// recit — c'est lui qui distingue « ecrasement actif » de « side-car absent ».
	{
		const FCityBedCeiling& Bed = BedCeilingSingleton();
		Summary.WaterBedCells = Bed.Charges;
		Summary.WaterBedRejects = Bed.TailleKo;
		Summary.QuayProfileNodes = Bed.NoeudsProfil;
		if (Bed.bActive)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("FRONTIERE : plafond du lit lu sur %d cellule(s) (pas %.4f m, grille %d) ; %d refusee(s), %d sans fichier."),
				Bed.Charges, Bed.StepCm / 100.f, Bed.GridN, Bed.TailleKo, Bed.Manquants);
			UE_LOG(LogCityImport, Display,
				TEXT("PROFIL : profil de berge %s — %d noeud(s) FORCE(S) (plateforme a +%.2f m "
					 "sur la cote d'eau locale, ou esplanade)."),
				Bed.bProfil ? TEXT("ACTIF") : TEXT("ETEINT"), Bed.NoeudsProfil,
				Bed.PlatHeightCm / 100.f);
		}
	}
	return Summary;
}

FCityBldgColSummary UCityImportTools::GenerateBuildingCollisionCell(const FString& JsonFilePath,
	const FString& AssetFolder, float CellSizeM, int32 CellX, int32 CellY,
	const FCityGenProfile& Profile)
{
	FCityBldgColSummary Summary;
	const FIntPoint Cell(CellX, CellY);
	TMap<FIntPoint, FCityBldgColSummary> Cells;
	if (GenerateBuildingCollisionForCells(JsonFilePath, AssetFolder, CellSizeM, Profile, &Cell, Cells))
	{
		Summary = Cells.FindRef(Cell);
	}
	return Summary;
}

FCityBldgColBatchSummary UCityImportTools::GenerateBuildingCollisionAll(const FString& JsonFilePath,
	const FString& AssetFolder, float CellSizeM, const FCityGenProfile& Profile)
{
	FCityBldgColBatchSummary Summary;
	TMap<FIntPoint, FCityBldgColSummary> Cells;
	if (!GenerateBuildingCollisionForCells(JsonFilePath, AssetFolder, CellSizeM, Profile, nullptr, Cells))
	{
		return Summary;
	}
	for (const auto& Pair : Cells)
	{
		++Summary.Cells;
		Summary.Buildings += Pair.Value.Buildings;
		Summary.WiredWalls += Pair.Value.bWallWired ? 1 : 0;
		Summary.MissingWalls += Pair.Value.bWallWired ? 0 : 1;
	}
	UE_LOG(LogCityImport, Display,
		TEXT("Collision batiments : %d cellules, %d batiments, %d walls cables, %d walls absents."),
		Summary.Cells, Summary.Buildings, Summary.WiredWalls, Summary.MissingWalls);
	return Summary;
}
