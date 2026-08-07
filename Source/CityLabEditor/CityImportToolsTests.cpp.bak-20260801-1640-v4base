#include "CityImportTools.h"
#include "TerrainSampler.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/PlatformFileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "StaticMeshCompiler.h"

BEGIN_DEFINE_SPEC(
	FCityImportToolsSpec,
	"CityLab.CityImportTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	// Sampler MNT de CONTROLE des tests desktop (independant de celui de l'outil),
	// charge une seule fois pour toute la spec (~200 Mo decompresses).
	FTerrainSampler Terrain;
	bool EnsureTerrainLoaded();
END_DEFINE_SPEC(FCityImportToolsSpec)

bool FCityImportToolsSpec::EnsureTerrainLoaded()
{
	if (!Terrain.IsLoaded())
	{
		Terrain.Load(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/toulouse10_mnt.png")),
			FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/toulouse10_mnt.json")));
	}
	return Terrain.IsLoaded();
}

namespace
{
	// Charge un mesh genere sous /Game/Dev/Test/City (regenere en place par l'import).
	UStaticMesh* LoadTestMesh(const FString& Name)
	{
		const FString Path = FString::Printf(TEXT("/Game/Dev/Test/City/%s.%s"), *Name, *Name);
		return LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	// Bornes Z reelles du mesh, lues dans la MeshDescription committee.
	bool GetMeshZBounds(UStaticMesh* Mesh, float& OutMinZ, float& OutMaxZ)
	{
		FMeshDescription* MeshDesc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
		if (!MeshDesc || MeshDesc->Vertices().Num() == 0)
		{
			return false;
		}
		TVertexAttributesRef<FVector3f> Positions = FStaticMeshAttributes(*MeshDesc).GetVertexPositions();
		OutMinZ = FLT_MAX;
		OutMaxZ = -FLT_MAX;
		for (const FVertexID V : MeshDesc->Vertices().GetElementIDs())
		{
			OutMinZ = FMath::Min(OutMinZ, Positions[V].Z);
			OutMaxZ = FMath::Max(OutMaxZ, Positions[V].Z);
		}
		return true;
	}

	// Maximum par canal des couleurs d'INSTANCE de sommet du mesh (J3c : la teinte
	// ortho du toit y arrive apres Encode, donc bornee a [0 ; 1]).
	bool GetMeshMaxVertexColor(UStaticMesh* Mesh, FVector3f& OutMax)
	{
		FMeshDescription* MeshDesc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
		if (!MeshDesc || MeshDesc->VertexInstances().Num() == 0)
		{
			return false;
		}
		TVertexInstanceAttributesRef<FVector4f> Colors =
			FStaticMeshAttributes(*MeshDesc).GetVertexInstanceColors();
		if (!Colors.IsValid())
		{
			return false;
		}
		OutMax = FVector3f(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		for (const FVertexInstanceID VI : MeshDesc->VertexInstances().GetElementIDs())
		{
			const FVector4f& C = Colors[VI];
			OutMax = FVector3f(FMath::Max(OutMax.X, C.X), FMath::Max(OutMax.Y, C.Y),
				FMath::Max(OutMax.Z, C.Z));
		}
		return true;
	}
}

void FCityImportToolsSpec::Define()
{
	Describe("ImportCityDistrict", [this]()
	{
		It("imports a minimal district and reports the counts", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_district.json"));
			const FString Json = TEXT(R"({"buildings":[{"pts":[[0,0],[12,0],[12,10],[0,10]],"h":9.5,"u":"res"}],)")
				TEXT(R"("roads":[{"pts":[[-20,-8],[30,-8]],"t":"residential","w":6}],)")
				TEXT(R"("trees":[[5,-15],[8,-15]]})");
			FFileHelper::SaveStringToFile(Json, *Path);

			const FCityImportSummary Summary = UCityImportTools::ImportCityDistrict(
				Path, TEXT("/Game/Dev/Test/City"), FString(), FString(), 100.f, FVector::ZeroVector);
			TestEqual(TEXT("Buildings"), Summary.Buildings, 1);
			TestEqual(TEXT("Roads"), Summary.Roads, 1);
			TestEqual(TEXT("Trees"), Summary.Trees, 2);
			TestTrue(TEXT("At least one cell mesh + tree mesh"), Summary.Meshes >= 2);
		});

		It("raises when the file does not exist", [this]()
		{
			AddExpectedError(TEXT("Cannot read district file"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::ImportCityDistrict(TEXT("Z:/nope.json"), TEXT("/Game/Dev/Test/City"),
				FString(), FString(), 100.f, FVector::ZeroVector);
		});

		It("raises on an invalid asset folder", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_district2.json"));
			FFileHelper::SaveStringToFile(TEXT("{}"), *Path);
			AddExpectedError(TEXT("AssetFolder must be a package path"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::ImportCityDistrict(Path, TEXT("not-a-path"), FString(), FString(), 100.f, FVector::ZeroVector);
		});
	});

	Describe("ImportCityStreamed", [this]()
	{
		It("splits buildings into ground, proxy and streaming blocks", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_streamed.json"));
			const FString Json = TEXT(R"({"buildings":[{"pts":[[0,0],[12,0],[12,10],[0,10]],"h":9.5,"u":"res"},)")
				TEXT(R"({"pts":[[250,0],[262,0],[262,10],[250,10]],"h":15,"u":"com"}],)")
				TEXT(R"("roads":[{"pts":[[-20,-8],[300,-8]],"t":"residential","w":6}],)")
				TEXT(R"("trees":[[5,-15],[8,-15]]})");
			FFileHelper::SaveStringToFile(Json, *Path);

			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, FCityGenProfile());
			TestEqual(TEXT("Buildings"), Summary.Buildings, 2);
			TestEqual(TEXT("Roads"), Summary.Roads, 1);
			TestEqual(TEXT("Trees"), Summary.Trees, 2);
			TestTrue(TEXT("Au moins un mesh de sol"), Summary.GroundMeshes >= 1);
			TestTrue(TEXT("Au moins un mesh proxy"), Summary.ProxyMeshes >= 1);
			TestEqual(TEXT("Deux meshes detail (cellules distinctes)"), Summary.BuildingMeshes, 2);
			TestEqual(TEXT("Deux blocs de streaming"), Summary.StreamingBlocks, 2);
		});

		It("J3b : toit en pente depuis le squelette precalcule, fallback plat si bloc invalide", [this]()
		{
			// Batiment 1 : rectangle 12x10, egout 5,5 m, delta 2 m — squelette du
			// self-test de Tools/j3b_prep_toits.py (2 noeuds a d=5). Faitage attendu
			// a 550 + 200 = 750. Batiment 2 (cellule 2_0) : delta 99 m ABERRANT ->
			// rejete par ParseRoof, toit plat historique a h = 800.
			// J3c : le batiment 1 porte "tint" (teinte ortho) nettement rouge.
			const FString BldPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_bati_toits.json"));
			const FString BldJson = TEXT(R"({"buildings":[)")
				TEXT(R"({"pts":[[0,0],[12,0],[12,10],[0,10]],"h":8,"u":"res","tint":[1.3,0.7,0.7],"roof":{)")
				TEXT(R"("eave":5.5,"delta":2,"mat":"ardoise",)")
				TEXT(R"("sv":[[5,5,5],[7,5,5]],)")
				TEXT(R"("f":[[0,1,5,4],[1,2,5],[2,3,4,5],[3,0,4]]}},)")
				TEXT(R"({"pts":[[250,0],[262,0],[262,10],[250,10]],"h":8,"u":"res","roof":{)")
				TEXT(R"("eave":5.5,"delta":99,"mat":"tuile","sv":[[256,5,5]],"f":[[0,1,4],[1,2,4],[2,3,4]]}}]})");
			FFileHelper::SaveStringToFile(BldJson, *BldPath);
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_toits_main.json"));
			FFileHelper::SaveStringToFile(TEXT(R"({"buildings":[]})"), *Path);

			FCityGenProfile Profile;
			Profile.bWindowReveals = true;   // chemin batiments desktop, sans MNT (ZBase = 0)
			Profile.bSplitWallGlass = true;
			Profile.BuildingsJsonPath = BldPath;
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, Profile);
			TestEqual(TEXT("Batiments (source dediee)"), Summary.Buildings, 2);
			TestEqual(TEXT("Un seul toit en pente (delta aberrant rejete)"), Summary.RoofsPitched, 1);

			float MinZ = 0.f, MaxZ = 0.f;
			UStaticMesh* Pitched = LoadTestMesh(TEXT("SM_Bldg_0_0_Wall"));
			if (TestTrue(TEXT("SM_Bldg_0_0_Wall lisible"), GetMeshZBounds(Pitched, MinZ, MaxZ)))
			{
				TestTrue(FString::Printf(TEXT("Faitage %.1f = egout 550 + delta 200"), MaxZ),
					FMath::Abs(MaxZ - 750.f) <= 1.f);
			}
			// J3c : la teinte [1,3 ; 0,7 ; 0,7] rend le versant nettement rouge. Sans
			// elle le toit est le quasi blanc (0,95 ; 0,95 ; 0,95) -> max R == max G et
			// la marge tombe a 0 : le critere discrimine bien la teinte.
			FVector3f MaxCol;
			if (TestTrue(TEXT("Couleurs de sommet lisibles"), GetMeshMaxVertexColor(Pitched, MaxCol)))
			{
				AddInfo(FString::Printf(TEXT("J3c teinte ortho : max vertex color R=%.3f G=%.3f B=%.3f"),
					MaxCol.X, MaxCol.Y, MaxCol.Z));
				TestTrue(FString::Printf(TEXT("Teinte ortho rouge : max R %.3f > max G %.3f"),
					MaxCol.X, MaxCol.Y), MaxCol.X > MaxCol.Y + 0.1f);
			}
			UStaticMesh* Flat = LoadTestMesh(TEXT("SM_Bldg_2_0_Wall"));
			if (TestTrue(TEXT("SM_Bldg_2_0_Wall lisible"), GetMeshZBounds(Flat, MinZ, MaxZ)))
			{
				TestTrue(FString::Printf(TEXT("Fallback plat %.1f = h 800"), MaxZ),
					FMath::Abs(MaxZ - 800.f) <= 1.f);
			}
		});

		It("raises when the file does not exist", [this]()
		{
			AddExpectedError(TEXT("Cannot read district file"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::ImportCityStreamed(TEXT("Z:/nope.json"), FString(), TEXT("/Game/Dev/Test/City"),
				TEXT("/Game/Dev/Test/Blocks"), FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector,
				FCityGenProfile());
		});
	});

	Describe("ImportCitySurfaces", [this]()
	{
		It("imports water, green and rails, and scatters trees in forests", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_surfaces.json"));
			const FString Json = TEXT(R"({"water":[{"pts":[[0,0],[50,0],[50,30],[0,30]]}],)")
				TEXT(R"("green":[{"k":"forest","pts":[[100,0],[220,0],[220,120],[100,120]]},)")
				TEXT(R"({"k":"park","pts":[[-80,0],[-20,0],[-20,40],[-80,40]]}],)")
				TEXT(R"("rails":[{"pts":[[-50,-50],[300,-50]]}]})");
			FFileHelper::SaveStringToFile(Json, *Path);

			const FCitySurfacesSummary Summary = UCityImportTools::ImportCitySurfaces(
				Path, TEXT("/Game/Dev/Test/City"), FString(), 100.f, FVector::ZeroVector, FCityGenProfile());
			TestEqual(TEXT("Water"), Summary.Water, 1);
			TestEqual(TEXT("Green"), Summary.Green, 2);
			TestEqual(TEXT("Rails"), Summary.Rails, 1);
			TestTrue(TEXT("Des arbres disperses dans le bois de 120 m"), Summary.ScatterTrees > 0);
			TestTrue(TEXT("Au moins un mesh de cellule"), Summary.Meshes >= 1);
		});

		It("raises when the file does not exist", [this]()
		{
			AddExpectedError(TEXT("Cannot read surfaces file"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::ImportCitySurfaces(TEXT("Z:/nope.json"), TEXT("/Game/Dev/Test/City"),
				FString(), 100.f, FVector::ZeroVector, FCityGenProfile());
		});
	});

	Describe("ImportCityMarkers", [this]()
	{
		It("places markers of known kinds and skips unknown ones", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_markers.json"));
			FFileHelper::SaveStringToFile(
				TEXT(R"({"markers":[{"x":0,"y":0,"k":"metro","n":"Test"},{"x":10,"y":0,"k":"metro_e","n":""},{"x":20,"y":0,"k":"inconnu","n":""}]})"),
				*Path);
			const int32 Placed = UCityImportTools::ImportCityMarkers(
				Path, TEXT("/Game/Dev/Test/City"), FString(), FVector::ZeroVector, FCityGenProfile());
			TestEqual(TEXT("Marqueurs places"), Placed, 2);
		});

		It("raises when the file does not exist", [this]()
		{
			AddExpectedError(TEXT("Cannot read markers file"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::ImportCityMarkers(TEXT("Z:/nope.json"), TEXT("/Game/Dev/Test/City"),
				FString(), FVector::ZeroVector, FCityGenProfile());
		});

		It("raises when the markers array is missing", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_markers2.json"));
			FFileHelper::SaveStringToFile(TEXT("{}"), *Path);
			AddExpectedError(TEXT("no 'markers' array"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::ImportCityMarkers(Path, TEXT("/Game/Dev/Test/City"), FString(), FVector::ZeroVector,
				FCityGenProfile());
		});
	});

	// Jalon J2 : profil desktop (relief MNT). Le profil mobile par defaut reste le
	// golden path — premiere verification ci-dessous. Les tests desktop utilisent la
	// VRAIE dalle MNT (comme la spec TerrainSampler) et un sampler de controle
	// independant de celui de l'outil.
	Describe("ProfilDesktop", [this]()
	{
		It("profil mobile par defaut : geometrie a plat inchangee (non-regression)", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_mobile.json"));
			const FString Json = TEXT(R"({"buildings":[{"pts":[[0,0],[12,0],[12,10],[0,10]],"h":9.5,"u":"res"}],)")
				TEXT(R"("roads":[{"pts":[[-20,-8],[30,-8]],"t":"residential","w":6}],)")
				TEXT(R"("trees":[[5,-15],[8,-15]]})");
			FFileHelper::SaveStringToFile(Json, *Path);

			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, FCityGenProfile());
			TestEqual(TEXT("Buildings"), Summary.Buildings, 1);
			TestEqual(TEXT("Roads"), Summary.Roads, 1);
			TestEqual(TEXT("Trees"), Summary.Trees, 2);

			// Dalle historique : grille a plat stricte z=0, collision boite simple.
			float MinZ = 0.f, MaxZ = 0.f;
			UStaticMesh* Slab = LoadTestMesh(TEXT("SM_Slab_0_0"));
			TestTrue(TEXT("SM_Slab_0_0 lisible"), GetMeshZBounds(Slab, MinZ, MaxZ));
			TestEqual(TEXT("Dalle mobile : Z min = 0"), MinZ, 0.f);
			TestEqual(TEXT("Dalle mobile : Z max = 0"), MaxZ, 0.f);
			if (Slab && Slab->GetBodySetup())
			{
				TestEqual(TEXT("Dalle mobile : une boite simple"),
					Slab->GetBodySetup()->AggGeom.BoxElems.Num(), 1);
			}

			// Batiment historique : pied a z=0, toit a h=9,5 m, pas de socle.
			UStaticMesh* Bldg = LoadTestMesh(TEXT("SM_Bldg_0_0"));
			TestTrue(TEXT("SM_Bldg_0_0 lisible"), GetMeshZBounds(Bldg, MinZ, MaxZ));
			TestEqual(TEXT("Batiment mobile : pied a 0"), MinZ, 0.f);
			TestEqual(TEXT("Batiment mobile : toit a 950"), MaxZ, 950.f);

			// Lot B — le golden path mobile ignore la matiere desktop : pas de Nanite,
			// un seul canal UV (pas d'UV1 monde), un seul mesh par cellule batiments.
			if (Slab)
			{
				TestFalse(TEXT("Mobile : dalle sans Nanite"), Slab->GetNaniteSettings().bEnabled);
				if (FMeshDescription* SlabDesc = Slab->GetMeshDescription(0))
				{
					TestEqual(TEXT("Mobile : un seul canal UV sur la dalle"),
						FStaticMeshAttributes(*SlabDesc).GetVertexInstanceUVs().GetNumChannels(), 1);
				}
			}
			if (Bldg)
			{
				TestFalse(TEXT("Mobile : batiment sans Nanite"), Bldg->GetNaniteSettings().bEnabled);
			}
		});

		It("profil desktop : sol drape dans les bornes MNT de la cellule, collision trimesh dediee", [this]()
		{
			if (!TestTrue(TEXT("MNT charge"), EnsureTerrainLoaded()))
			{
				return;
			}
			const float AltCap = Terrain.AltCapitoleCm();

			// Cellule en PENTE cherchee sur la dalle reelle (berges de la Garonne a
			// l'ouest du Capitole) : premiere emprise 20 x 20 m avec >= 2 m de denivele.
			int32 SlopeXm = 0;
			bool bFound = false;
			for (int32 Xm = -3000; Xm <= 3000 && !bFound; Xm += 50)
			{
				const TArray<FVector2D> Foot = {
					FVector2D(Xm * 100.0, -1000.0), FVector2D(Xm * 100.0 + 2000.0, -1000.0),
					FVector2D(Xm * 100.0 + 2000.0, 1000.0), FVector2D(Xm * 100.0, 1000.0) };
				if (Terrain.MaxAltCmInPolygon(Foot) - Terrain.MinAltCmInPolygon(Foot) >= 200.f)
				{
					SlopeXm = Xm;
					bFound = true;
				}
			}
			if (!TestTrue(TEXT("Une pente >= 2 m trouvee sur la dalle"), bFound))
			{
				return;
			}

			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_desktop_sol.json"));
			FFileHelper::SaveStringToFile(FString::Printf(
				TEXT(R"({"buildings":[{"pts":[[%d,-10],[%d,-10],[%d,10],[%d,10]],"h":9.5,"u":"res"}]})"),
				SlopeXm, SlopeXm + 20, SlopeXm + 20, SlopeXm), *Path);
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, FCityGenProfile::Desktop());
			TestEqual(TEXT("Buildings"), Summary.Buildings, 1);

			// Bornes MNT de la zone de la cellule (marge 2 m : le support bilineaire
			// des coins de dalle deborde du rectangle d'un pixel).
			const float Cell = 10000.f;
			const int32 CX = FMath::FloorToInt((SlopeXm * 100.f + 1000.f) / Cell);
			const int32 CY = FMath::FloorToInt(0.f / Cell);
			const TArray<FVector2D> Zone = {
				FVector2D(CX * Cell - 200.0, CY * Cell - 200.0),
				FVector2D((CX + 1) * Cell + 200.0, CY * Cell - 200.0),
				FVector2D((CX + 1) * Cell + 200.0, (CY + 1) * Cell + 200.0),
				FVector2D(CX * Cell - 200.0, (CY + 1) * Cell + 200.0) };
			const float ZoneMinZ = Terrain.MinAltCmInPolygon(Zone) - AltCap;
			const float ZoneMaxZ = Terrain.MaxAltCmInPolygon(Zone) - AltCap;

			float MinZ = 0.f, MaxZ = 0.f;
			UStaticMesh* Slab = LoadTestMesh(FString::Printf(TEXT("SM_Slab_%d_%d"), CX, CY));
			if (!TestTrue(TEXT("Dalle drapee lisible"), GetMeshZBounds(Slab, MinZ, MaxZ)))
			{
				return;
			}
			TestTrue(FString::Printf(TEXT("Z min dalle %.1f >= borne basse zone %.1f"), MinZ, ZoneMinZ - 300.f),
				MinZ >= ZoneMinZ - 300.f);
			TestTrue(FString::Printf(TEXT("Z max dalle %.1f <= borne haute zone %.1f"), MaxZ, ZoneMaxZ + 300.f),
				MaxZ <= ZoneMaxZ + 300.f);
			TestTrue(FString::Printf(TEXT("Z varies sur la cellule en pente (%.1f cm d'amplitude)"), MaxZ - MinZ),
				MaxZ - MinZ >= 50.f);

			// Collision desktop : trimesh basse resolution dedie, plus de boite.
			if (TestNotNull(TEXT("BodySetup"), Slab ? Slab->GetBodySetup() : nullptr))
			{
				TestEqual(TEXT("CTF complex-as-simple (trimesh)"),
					(int32)Slab->GetBodySetup()->CollisionTraceFlag, (int32)CTF_UseComplexAsSimple);
				TestEqual(TEXT("Plus de boite simple"), Slab->GetBodySetup()->AggGeom.BoxElems.Num(), 0);
			}
			UStaticMesh* ColMesh = LoadTestMesh(FString::Printf(TEXT("SM_Slab_%d_%d_Col"), CX, CY));
			TestNotNull(TEXT("Mesh de collision 16x16 dedie cree"), ColMesh);
			if (Slab && ColMesh)
			{
				TestTrue(TEXT("ComplexCollisionMesh pointe le 16x16"),
					Slab->ComplexCollisionMesh.Get() == ColMesh);
			}
		});

		It("profil desktop : batiment en pente pose a MinAlt avec socle enterre", [this]()
		{
			if (!TestTrue(TEXT("MNT charge"), EnsureTerrainLoaded()))
			{
				return;
			}
			const float AltCap = Terrain.AltCapitoleCm();

			// Meme recherche de pente que le test du sol (berges de la Garonne).
			int32 SlopeXm = 0;
			bool bFound = false;
			for (int32 Xm = -3000; Xm <= 3000 && !bFound; Xm += 50)
			{
				const TArray<FVector2D> Foot = {
					FVector2D(Xm * 100.0, -1000.0), FVector2D(Xm * 100.0 + 2000.0, -1000.0),
					FVector2D(Xm * 100.0 + 2000.0, 1000.0), FVector2D(Xm * 100.0, 1000.0) };
				if (Terrain.MaxAltCmInPolygon(Foot) - Terrain.MinAltCmInPolygon(Foot) >= 200.f)
				{
					SlopeXm = Xm;
					bFound = true;
				}
			}
			if (!TestTrue(TEXT("Une pente >= 2 m trouvee sur la dalle"), bFound))
			{
				return;
			}

			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_desktop_bldg.json"));
			FFileHelper::SaveStringToFile(FString::Printf(
				TEXT(R"({"buildings":[{"pts":[[%d,-10],[%d,-10],[%d,10],[%d,10]],"h":9.5,"u":"res"}]})"),
				SlopeXm, SlopeXm + 20, SlopeXm + 20, SlopeXm), *Path);
			UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, FCityGenProfile::Desktop());

			// Attendu (J2 §3.3) : ZBase = MinAlt(emprise) - AltCapitole, mur prolonge
			// jusqu'a ZBase - (MaxAlt - MinAlt) - 50 (socle), toit a ZBase + h.
			const TArray<FVector2D> Foot = {
				FVector2D(SlopeXm * 100.0, -1000.0), FVector2D(SlopeXm * 100.0 + 2000.0, -1000.0),
				FVector2D(SlopeXm * 100.0 + 2000.0, 1000.0), FVector2D(SlopeXm * 100.0, 1000.0) };
			const float MinAlt = Terrain.MinAltCmInPolygon(Foot);
			const float MaxAlt = Terrain.MaxAltCmInPolygon(Foot);
			const float ZBase = MinAlt - AltCap;
			const float ExpectedMinZ = ZBase - (MaxAlt - MinAlt) - 50.f;
			const float ExpectedMaxZ = ZBase + 950.f;

			// Lot B : le profil desktop separe Wall/Glass — le socle et le toit
			// vivent dans le mesh opaque SM_Bldg_*_Wall.
			const int32 CX = FMath::FloorToInt((SlopeXm * 100.f + 1000.f) / 10000.f);
			float MinZ = 0.f, MaxZ = 0.f;
			UStaticMesh* Bldg = LoadTestMesh(FString::Printf(TEXT("SM_Bldg_%d_0_Wall"), CX));
			if (!TestTrue(TEXT("Batiment drape lisible"), GetMeshZBounds(Bldg, MinZ, MaxZ)))
			{
				return;
			}
			TestTrue(FString::Printf(TEXT("Pied du socle %.1f = ZBase - denivele - 50 (%.1f)"), MinZ, ExpectedMinZ),
				FMath::Abs(MinZ - ExpectedMinZ) <= 1.f);
			TestTrue(FString::Printf(TEXT("Toit %.1f = ZBase + 950 (%.1f)"), MaxZ, ExpectedMaxZ),
				FMath::Abs(MaxZ - ExpectedMaxZ) <= 1.f);
			TestTrue(FString::Printf(TEXT("Hauteur de mur %.1f augmentee du socle (denivele %.1f + 50)"),
					MaxZ - MinZ, MaxAlt - MinAlt),
				(MaxZ - MinZ) - 950.f >= (MaxAlt - MinAlt) + 50.f - 1.f);
		});

		It("profil desktop : route bridge=true interpolee entre culees, pas le creux du terrain", [this]()
		{
			if (!TestTrue(TEXT("MNT charge"), EnsureTerrainLoaded()))
			{
				return;
			}
			const float AltCap = Terrain.AltCapitoleCm();

			// Segment de 600 m sur l'axe Y=0 ou le terrain s'ecarte LE PLUS de la
			// corde entre les deux culees (le creux de la Garonne ou une butte) :
			// c'est exactement l'ecart qu'un tablier interpole doit ignorer.
			int32 MidXm = 0;
			float BestDev = 0.f;
			for (int32 Xm = -4500; Xm <= 4500; Xm += 10)
			{
				const float Chord = 0.5f * (Terrain.AltCmAt((Xm - 300) * 100.0, 0.0)
					+ Terrain.AltCmAt((Xm + 300) * 100.0, 0.0));
				const float Dev = FMath::Abs(Terrain.AltCmAt(Xm * 100.0, 0.0) - Chord);
				if (Dev > BestDev)
				{
					BestDev = Dev;
					MidXm = Xm;
				}
			}
			const int32 AXm = MidXm - 300, BXm = MidXm + 300;
			const float ZA = Terrain.AltCmAt(AXm * 100.0, 0.0) - AltCap;
			const float ZB = Terrain.AltCmAt(BXm * 100.0, 0.0) - AltCap;
			if (!TestTrue(FString::Printf(TEXT("Terrain a >= 3 m de la corde des culees (%.1f cm en x=%d m)"),
					BestDev, MidXm),
				BestDev >= 300.f))
			{
				return;
			}

			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_desktop_pont.json"));
			FFileHelper::SaveStringToFile(FString::Printf(
				TEXT(R"({"roads":[{"pts":[[%d,0],[%d,0]],"t":"primary","w":10,"bridge":true,"layer":1}]})"),
				AXm, BXm), *Path);
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, FCityGenProfile::Desktop());
			TestEqual(TEXT("Roads"), Summary.Roads, 1);

			// Le ruban est construit dans la cellule du premier point. Le tablier est
			// l'interpolation lineaire entre culees PLUS un offset d'empilement
			// CONSTANT — depuis J3c v2 cet offset depend de la classe de revetement
			// (55 cm de plancher + 0 a 25 cm selon la classe) : le test ne le code
			// PLUS en dur, il verifie qu'il est constant sur tout le tablier (c'est
			// exactement ce qui distingue une droite d'un drapage) et plausible.
			const float FloorCm = 55.f, CeilCm = 90.f;
			const int32 CX = FMath::FloorToInt(AXm * 100.f / 10000.f);
			float MinZ = 0.f, MaxZ = 0.f;
			UStaticMesh* Road = LoadTestMesh(FString::Printf(TEXT("SM_Ground_%d_0"), CX));
			if (!TestTrue(TEXT("Ruban du pont lisible"), GetMeshZBounds(Road, MinZ, MaxZ)))
			{
				return;
			}
			TestTrue(FString::Printf(TEXT("Tablier %.1f >= culee basse %.1f"), MinZ, FMath::Min(ZA, ZB) + FloorCm - 1.f),
				MinZ >= FMath::Min(ZA, ZB) + FloorCm - 1.f);
			TestTrue(FString::Printf(TEXT("Tablier %.1f <= culee haute %.1f"), MaxZ, FMath::Max(ZA, ZB) + CeilCm + 1.f),
				MaxZ <= FMath::Max(ZA, ZB) + CeilCm + 1.f);
			// Chaque sommet de la CHAUSSEE (X = abscisse re-echantillonnee, la route est
			// alignee sur X) est a la MEME hauteur au-dessus de la droite des culees :
			// un drapage MNT donnerait >= BestDev (>= 3 m) de dispersion.
			// v5 « voirie » : la mesure se limite au slot de CHAUSSEE. Le ruban porte
			// desormais aussi ses bordures et ses rives, posees 12 cm plus haut PAR
			// CONSTRUCTION — les compter ici ferait dire au test « tablier drape » alors
			// qu'il mesurerait la hauteur du trottoir.
			FMeshDescription* RoadDesc = Road->GetMeshDescription(0);
			FStaticMeshAttributes RoadAttr(*RoadDesc);
			TVertexAttributesRef<FVector3f> Positions = RoadAttr.GetVertexPositions();
			TPolygonGroupAttributesRef<FName> RoadSlots = RoadAttr.GetPolygonGroupMaterialSlotNames();
			float MinOff = FLT_MAX, MaxOff = -FLT_MAX;
			for (const FPolygonGroupID G : RoadDesc->PolygonGroups().GetElementIDs())
			{
				if (RoadSlots[G] != FName(TEXT("asphalt_road_tiggcjdo")))
				{
					continue;
				}
				for (const FPolygonID P : RoadDesc->GetPolygonGroupPolygons(G))
				{
					for (const FVertexInstanceID VI : RoadDesc->GetPolygonVertexInstances(P))
					{
						const FVector3f Pos = Positions[RoadDesc->GetVertexInstanceVertex(VI)];
						const float T = (Pos.X - AXm * 100.f) / ((BXm - AXm) * 100.f);
						const float Off = Pos.Z - FMath::Lerp(ZA, ZB, T);
						MinOff = FMath::Min(MinOff, Off);
						MaxOff = FMath::Max(MaxOff, Off);
					}
				}
			}
			if (!TestTrue(TEXT("Slot de chaussee trouve sur le tablier"), MinOff < FLT_MAX))
			{
				return;
			}
			TestTrue(FString::Printf(TEXT("Tablier interpole lineairement (dispersion %.2f cm ; drape = %.0f cm)"),
				MaxOff - MinOff, BestDev), MaxOff - MinOff <= 1.f);
			TestTrue(FString::Printf(TEXT("Offset d'empilement plausible (%.1f cm)"), MinOff),
				MinOff >= FloorCm - 1.f && MaxOff <= CeilCm + 1.f);
		});
	});

	// Lot B « matiere et modenature » : fenetres en creux geometriques, split
	// Wall/Glass, Nanite sur tous les meshes generes (vitres comprises depuis
	// J2e — verre opaque), materiaux PBR DefaultLit, UV1 monde.
	// Tests A PLAT (bDrapeToTerrain=false, profils manuels sans bDesktop) :
	// independants du MNT, ils isolent la matiere du relief.
	Describe("LotB", [this]()
	{
		// Fixture commune : 1 batiment 12x10 m h9,5 (3 etages, travees 4+3+4+3 = 42
		// fenetres) + 1 route, tout en cellule (0,0).
		auto WriteFixture = [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_lotb.json"));
			const FString Json = TEXT(R"({"buildings":[{"pts":[[0,0],[12,0],[12,10],[0,10]],"h":9.5,"u":"res"}],)")
				TEXT(R"("roads":[{"pts":[[5,20],[80,20]],"t":"residential","w":6}]})");
			FFileHelper::SaveStringToFile(Json, *Path);
			return Path;
		};
		auto DesktopFlat = []()
		{
			// Matiere desktop SANS relief : profils explicites, pas de prereglage
			// bDesktop (qui forcerait le drapage MNT).
			FCityGenProfile P;
			P.bWindowReveals = true;
			P.bSplitWallGlass = true;
			P.bNanite = true;
			P.bPBRMaterials = true;
			return P;
		};
		auto TriCount = [](UStaticMesh* Mesh) -> int32
		{
			FMeshDescription* Desc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			return Desc ? Desc->Triangles().Num() : 0;
		};

		It("fenetres en creux : delta de +18 tris par fenetre (spec 3-3)", [this, WriteFixture, DesktopFlat, TriCount]()
		{
			const FString Path = WriteFixture();
			FCityGenProfile POff = DesktopFlat();
			POff.bWindowReveals = false;

			UCityImportTools::ImportCityStreamed(Path, FString(), TEXT("/Game/Dev/Test/City"),
				TEXT("/Game/Dev/Test/Blocks"), FString(), FString(), 100.f, 200.f, 400.f,
				FVector::ZeroVector, POff);
			const int32 WallOff = TriCount(LoadTestMesh(TEXT("SM_Bldg_0_0_Wall")));
			const int32 GlassOff = TriCount(LoadTestMesh(TEXT("SM_Bldg_0_0_Glass")));
			if (!TestTrue(TEXT("Baseline sans tableaux lisible"), WallOff > 0 && GlassOff > 0))
			{
				return;
			}

			UCityImportTools::ImportCityStreamed(Path, FString(), TEXT("/Game/Dev/Test/City"),
				TEXT("/Game/Dev/Test/Blocks"), FString(), FString(), 100.f, 200.f, 400.f,
				FVector::ZeroVector, DesktopFlat());
			const int32 WallOn = TriCount(LoadTestMesh(TEXT("SM_Bldg_0_0_Wall")));
			const int32 GlassOn = TriCount(LoadTestMesh(TEXT("SM_Bldg_0_0_Glass")));

			// La vitre (1 quad/fenetre) compte les fenetres ; elle est identique
			// avec ou sans tableaux — tout le delta est porte par le mesh Wall.
			TestEqual(TEXT("Vitres inchangees par les tableaux"), GlassOn, GlassOff);
			const int32 Windows = GlassOn / 2;
			TestEqual(TEXT("42 fenetres (3 etages x travees 4+3+4+3)"), Windows, 42);
			const int32 Delta = (WallOn + GlassOn) - (WallOff + GlassOff);
			TestEqual(FString::Printf(TEXT("Delta %d tris / %d fenetres = +18 par fenetre"), Delta, Windows),
				Delta, Windows * 18);
		});

		It("split Wall/Glass, Nanite partout (vitres comprises), materiaux PBR DefaultLit, UV1 monde", [this, WriteFixture, DesktopFlat]()
		{
			const FString Path = WriteFixture();
			UCityImportTools::ImportCityStreamed(Path, FString(), TEXT("/Game/Dev/Test/City"),
				TEXT("/Game/Dev/Test/Blocks"), FString(), FString(), 100.f, 200.f, 400.f,
				FVector::ZeroVector, DesktopFlat());

			// DEUX meshes par cellule batiments (Q3) : opaque + vitres.
			UStaticMesh* Wall = LoadTestMesh(TEXT("SM_Bldg_0_0_Wall"));
			UStaticMesh* Glass = LoadTestMesh(TEXT("SM_Bldg_0_0_Glass"));
			UStaticMesh* Slab = LoadTestMesh(TEXT("SM_Slab_0_0"));
			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			UStaticMesh* Proxy = LoadTestMesh(TEXT("SM_Proxy_0_0"));
			if (!TestTrue(TEXT("Les 5 meshes desktop existent (Wall, Glass, Slab, Ground, Proxy)"),
				Wall && Glass && Slab && Ground && Proxy))
			{
				return;
			}

			// Nanite sur TOUS les meshes generes, vitres comprises (J2e, 25/07 :
			// le verre est opaque ; vitres non-Nanite + murs Nanite = fenetres
			// qui « flottent » aux transitions LOD).
			TestTrue(TEXT("Nanite : Wall"), Wall->GetNaniteSettings().bEnabled);
			TestTrue(TEXT("Nanite : Glass aussi (verre opaque, J2e)"), Glass->GetNaniteSettings().bEnabled);
			TestTrue(TEXT("Nanite : sol"), Slab->GetNaniteSettings().bEnabled);
			TestTrue(TEXT("Nanite : routes"), Ground->GetNaniteSettings().bEnabled);
			TestTrue(TEXT("Nanite : proxys"), Proxy->GetNaniteSettings().bEnabled);

			// Materiaux PBR assignes, shading model DefaultLit (PAS unlit — Lumen).
			auto CheckMat = [this](const TCHAR* Label, UStaticMesh* Mesh, int32 Slot, const TCHAR* Expected)
			{
				const TArray<FStaticMaterial>& Mats = Mesh->GetStaticMaterials();
				if (!TestTrue(FString::Printf(TEXT("%s : slot %d present"), Label, Slot), Mats.IsValidIndex(Slot)))
				{
					return;
				}
				UMaterialInterface* MI = Mats[Slot].MaterialInterface;
				if (!TestNotNull(FString::Printf(TEXT("%s : materiau"), Label), MI))
				{
					return;
				}
				TestEqual(FString::Printf(TEXT("%s : %s assigne"), Label, Expected), MI->GetName(), FString(Expected));
				const UMaterial* Mat = MI->GetMaterial();
				TestTrue(FString::Printf(TEXT("%s : DefaultLit"), Label),
					Mat && Mat->GetShadingModels().HasShadingModel(MSM_DefaultLit));
				TestFalse(FString::Printf(TEXT("%s : pas unlit"), Label),
					Mat && Mat->GetShadingModels().HasShadingModel(MSM_Unlit));
			};
			CheckMat(TEXT("Murs"), Wall, 0, TEXT("M_CityWall_PBR"));
			CheckMat(TEXT("Vitres"), Glass, 0, TEXT("M_CityGlass_PBR"));
			CheckMat(TEXT("Sol"), Slab, 0, TEXT("M_CityGround_PBR"));
			CheckMat(TEXT("Routes (rubans)"), Ground, 1, TEXT("M_CityRoad_PBR"));
			CheckMat(TEXT("Proxys"), Proxy, 0, TEXT("M_CityGround_PBR"));

			// Atlas de facades 2048² (grille 4x4 de sous-tuiles).
			UTexture2D* Atlas = LoadObject<UTexture2D>(nullptr,
				TEXT("/Game/Dev/Test/City/T_CityAtlas.T_CityAtlas"), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (TestNotNull(TEXT("T_CityAtlas cree"), Atlas))
			{
				TestEqual(TEXT("Atlas 2048 px"), (int32)Atlas->Source.GetSizeX(), 2048);
				TestEqual(TEXT("Atlas carre"), (int32)Atlas->Source.GetSizeY(), 2048);
			}

			// UV1 monde sur le sol : 2 canaux, valeurs normalisees dalle dans [0,1].
			FMeshDescription* SlabDesc = Slab->GetMeshDescription(0);
			if (TestNotNull(TEXT("MeshDescription dalle"), SlabDesc))
			{
				TVertexInstanceAttributesRef<FVector2f> UVs =
					FStaticMeshAttributes(*SlabDesc).GetVertexInstanceUVs();
				if (TestEqual(TEXT("Dalle : 2 canaux UV (UV1 monde)"), UVs.GetNumChannels(), 2))
				{
					bool bAllIn01 = true;
					for (const FVertexInstanceID I : SlabDesc->VertexInstances().GetElementIDs())
					{
						const FVector2f UV1 = UVs.Get(I, 1);
						if (UV1.X < -0.001f || UV1.X > 1.001f || UV1.Y < -0.001f || UV1.Y > 1.001f)
						{
							bAllIn01 = false;
							break;
						}
					}
					TestTrue(TEXT("Dalle : UV1 dans [0,1]"), bAllIn01);
				}
			}
			// Toits : le mesh Wall des batiments porte aussi l'UV1 monde (ortho J3).
			FMeshDescription* WallDesc = Wall->GetMeshDescription(0);
			if (TestNotNull(TEXT("MeshDescription murs"), WallDesc))
			{
				TestEqual(TEXT("Batiments : 2 canaux UV (toits ortho-ready)"),
					FStaticMeshAttributes(*WallDesc).GetVertexInstanceUVs().GetNumChannels(), 2);
			}
		});
	});

	// J3c point 2 « builder sols » : chaque route et chaque polygone vert part dans un
	// GROUPE DE POLYGONES dedie a sa classe de revetement (un slot de materiau par
	// classe, donc une section de mesh par classe), avec une UV0 EN METRES que le
	// materiau M_Surf_<slug> divise par la taille physique du scan. Les materiaux
	// Megascans ne sont PAS requis ici : absents, le repli garde le materiau
	// historique du slot — c'est le decoupage geometrique qui est teste.
	Describe("RevetementsSols", [this]()
	{
		auto SurfaceProfile = []()
		{
			// Matiere desktop SANS relief (pas de bDesktop : le drapage MNT ferait
			// dependre le test du MNT) + revetements par classe.
			FCityGenProfile P;
			P.bWindowReveals = true;
			P.bSplitWallGlass = true;
			P.bNanite = true;
			P.bPBRMaterials = true;
			P.bSurfaceMaterials = true;
			return P;
		};
		// Trois routes, toutes dans la cellule (0,0) — la cellule d'un ruban est celle
		// de son PREMIER point, d'ou des coordonnees franchement positives : la
		// premiere porte le tag OSM surface="sett" (rue pavee), la deuxieme est une
		// secondaire de 9 m a 2 files (chaussee marquee large), la troisieme un
		// sentier pieton. v4 : la rue pavee est une CHAUSSEE (asphalte) et le sentier
		// ne produit PLUS DE RUBAN DU TOUT : son sol, c'est la dalle desormais.
		auto WriteFixture = [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_surfaces.json"));
			const FString Json =
				TEXT(R"({"roads":[{"pts":[[10,10],[90,10]],"t":"residential","w":6,"surface":"sett"},)")
				TEXT(R"({"pts":[[10,30],[90,30]],"t":"secondary","w":9,"surface":"asphalt","lanes":2},)")
				TEXT(R"({"pts":[[10,50],[90,50]],"t":"footway","w":2.5}]})");
			FFileHelper::SaveStringToFile(Json, *Path);
			return Path;
		};
		auto NonEmptyGroups = [](UStaticMesh* Mesh) -> int32
		{
			FMeshDescription* Desc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Desc)
			{
				return 0;
			}
			int32 Count = 0;
			for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
			{
				if (Desc->GetNumPolygonGroupPolygons(G) > 0)
				{
					++Count;
				}
			}
			return Count;
		};
		auto HasSlot = [](UStaticMesh* Mesh, const TCHAR* SlotName)
		{
			if (!Mesh)
			{
				return false;
			}
			for (const FStaticMaterial& M : Mesh->GetStaticMaterials())
			{
				if (M.MaterialSlotName == FName(SlotName))
				{
					return true;
				}
			}
			return false;
		};

		It("route surface=sett : cellule de sol a >= 2 sections, un slot par classe de revetement",
			[this, WriteFixture, SurfaceProfile, NonEmptyGroups, HasSlot]()
		{
			const FString Path = WriteFixture();
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			// v4 : le sentier ne compte plus — 3 voies au JSON, 2 rubans generes.
			TestEqual(TEXT("Roads : le sentier pieton ne produit plus de ruban"),
				Summary.Roads, 2);

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			TestTrue(FString::Printf(TEXT("Cellule de sol a >= 2 sections (%d groupes non vides)"),
				NonEmptyGroups(Ground)), NonEmptyGroups(Ground) >= 2);
			TestTrue(TEXT("Slot asphalte (rue pavee = chaussee, plus de revetement pave)"),
				HasSlot(Ground, TEXT("asphalt_road_tiggcjdo")));
			TestTrue(TEXT("Slot chaussee marquee large (secondaire 9 m, 2 files -> fine_road)"),
				HasSlot(Ground, TEXT("fine_road_vgdlejpew")));
			// v3/v4 : les scans sortis de la palette ne reviennent JAMAIS sur un ruban.
			TestFalse(TEXT("Plus jamais de paves cobblestone"),
				HasSlot(Ground, TEXT("cobblestone_thjldijbw")));
			TestFalse(TEXT("Plus jamais de marked_rough_road"),
				HasSlot(Ground, TEXT("marked_rough_road_vh1lbhqs")));
			TestFalse(TEXT("Plus de revetement pave herringbone"),
				HasSlot(Ground, TEXT("herringbone_brick_pavement_ue3gbepkw")));
			// v5 : le revetement de DALLE arrive AUSSI sur la cellule de rubans — c'est
			// la matiere des RIVES (les 1,70 m de trottoir de part et d'autre de chaque
			// chaussee). Jusqu'a la v4b il n'avait rien a y faire ; l'y voir est
			// desormais la preuve que les rives sont bien posees.
			TestTrue(TEXT("Cellule de rubans : slot de dalle = les rives des rues"),
				HasSlot(Ground, TEXT("dirty_sidewalk_tiles_ugxjcdpn")));
			TestTrue(TEXT("Cellule de rubans : slot de bordure"), HasSlot(Ground, TEXT("curb")));

			// Sections de rendu : une par classe presente (preuve cote materiau).
			if (const FStaticMeshRenderData* RD = Ground->GetRenderData())
			{
				if (RD->LODResources.Num() > 0)
				{
					TestTrue(FString::Printf(TEXT("Rendu : %d sections"), RD->LODResources[0].Sections.Num()),
						RD->LODResources[0].Sections.Num() >= 2);
				}
			}

			// UV0 EN METRES : le ruban le plus long fait 80 m, donc U depasse
			// largement 1 (une UV normalisee [0,1] serait le bug a attraper).
			FMeshDescription* Desc = Ground->GetMeshDescription(0);
			if (TestNotNull(TEXT("MeshDescription du sol"), Desc))
			{
				TVertexInstanceAttributesRef<FVector2f> UVs =
					FStaticMeshAttributes(*Desc).GetVertexInstanceUVs();
				float MaxU = 0.f;
				for (const FVertexInstanceID I : Desc->VertexInstances().GetElementIDs())
				{
					const FVector2f UV0 = UVs.Get(I, 0);
					MaxU = FMath::Max(MaxU, FMath::Max(FMath::Abs(UV0.X), FMath::Abs(UV0.Y)));
				}
				TestTrue(FString::Printf(TEXT("UV0 en metres le long du ruban (max %.1f >= 70)"), MaxU),
					MaxU >= 70.f);
			}
		});

		// v4 — LA DALLE PORTE LA MATIERE (verdict DA v3 : « grand puzzle », le fond
		// blanc-bleu de J2 faisait de chaque ruban un autocollant sur du papier).
		// Preuve en deux points : le mesh de dalle a bien le slot du revetement
		// mineral, et son UV0 est EN METRES MONDE (une UV [0,1] par quad, l'historique,
		// tuilerait le scan une fois par carreau de grille : le bug a attraper).
		It("la dalle SM_Slab_ porte le revetement mineral en UV0 metrique",
			[this, WriteFixture, SurfaceProfile, HasSlot]()
		{
			const FString Path = WriteFixture();
			UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());

			UStaticMesh* Slab = LoadTestMesh(TEXT("SM_Slab_0_0"));
			if (!TestNotNull(TEXT("SM_Slab_0_0 genere"), Slab))
			{
				return;
			}
			TestTrue(TEXT("Slot de revetement mineral sur la dalle"),
				HasSlot(Slab, TEXT("dirty_sidewalk_tiles_ugxjcdpn")));

			// La cellule fait 100 m de cote : une UV0 metrique monte donc a ~100.
			FMeshDescription* Desc = Slab->GetMeshDescription(0);
			if (TestNotNull(TEXT("MeshDescription de la dalle"), Desc))
			{
				TVertexInstanceAttributesRef<FVector2f> UVs =
					FStaticMeshAttributes(*Desc).GetVertexInstanceUVs();
				float MaxUV0 = 0.f;
				bool bUV1In01 = UVs.GetNumChannels() >= 2;
				for (const FVertexInstanceID I : Desc->VertexInstances().GetElementIDs())
				{
					const FVector2f UV0 = UVs.Get(I, 0);
					MaxUV0 = FMath::Max(MaxUV0, FMath::Max(FMath::Abs(UV0.X), FMath::Abs(UV0.Y)));
					if (UVs.GetNumChannels() >= 2)
					{
						const FVector2f UV1 = UVs.Get(I, 1);
						if (UV1.X < -0.001f || UV1.X > 1.001f || UV1.Y < -0.001f || UV1.Y > 1.001f)
						{
							bUV1In01 = false;
						}
					}
				}
				TestTrue(FString::Printf(TEXT("Dalle : UV0 en metres monde (max %.1f >= 70)"), MaxUV0),
					MaxUV0 >= 70.f);
				// L'UV1 monde (ortho J3) survit a l'ajout de l'UV0 metrique.
				TestTrue(TEXT("Dalle : UV1 monde toujours dans [0,1]"), bUV1In01);
			}
		});

		It("sans le drapeau : la dalle garde son UV0 [0,1] par quad et aucun revetement",
			[this, WriteFixture, HasSlot]()
		{
			const FString Path = WriteFixture();
			UCityImportTools::ImportCityStreamed(Path, FString(), TEXT("/Game/Dev/Test/City"),
				TEXT("/Game/Dev/Test/Blocks"), FString(), FString(), 100.f, 200.f, 400.f,
				FVector::ZeroVector, FCityGenProfile());

			UStaticMesh* Slab = LoadTestMesh(TEXT("SM_Slab_0_0"));
			if (!TestNotNull(TEXT("SM_Slab_0_0 genere"), Slab))
			{
				return;
			}
			TestFalse(TEXT("Mobile : aucun revetement sur la dalle"),
				HasSlot(Slab, TEXT("dirty_sidewalk_tiles_ugxjcdpn")));
			FMeshDescription* Desc = Slab->GetMeshDescription(0);
			if (TestNotNull(TEXT("MeshDescription de la dalle"), Desc))
			{
				TVertexInstanceAttributesRef<FVector2f> UVs =
					FStaticMeshAttributes(*Desc).GetVertexInstanceUVs();
				float MaxUV0 = 0.f;
				for (const FVertexInstanceID I : Desc->VertexInstances().GetElementIDs())
				{
					const FVector2f UV0 = UVs.Get(I, 0);
					MaxUV0 = FMath::Max(MaxUV0, FMath::Max(FMath::Abs(UV0.X), FMath::Abs(UV0.Y)));
				}
				TestTrue(FString::Printf(TEXT("Mobile : UV0 dalle reste dans [0,1] (max %.2f)"), MaxUV0),
					MaxUV0 <= 1.001f);
			}
		});

		It("sans le drapeau : cellule de sol a exactement les 2 slots historiques (non-regression)",
			[this, WriteFixture, HasSlot]()
		{
			const FString Path = WriteFixture();
			UCityImportTools::ImportCityStreamed(Path, FString(), TEXT("/Game/Dev/Test/City"),
				TEXT("/Game/Dev/Test/Blocks"), FString(), FString(), 100.f, 200.f, 400.f,
				FVector::ZeroVector, FCityGenProfile());

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			TestEqual(TEXT("Mobile : 2 slots (Wall, Glass) et rien d'autre"),
				Ground->GetStaticMaterials().Num(), 2);
			TestFalse(TEXT("Mobile : aucun slot de revetement"),
				HasSlot(Ground, TEXT("herringbone_brick_pavement_ue3gbepkw")));
		});

		// v2 (verdict utilisateur : « les revetements se rencontrent sans harmonie »).
		// Z minimal des sommets d'un slot donne : sert a prouver l'ordre d'empilement.
		auto SlotMinZ = [](UStaticMesh* Mesh, const TCHAR* SlotName) -> float
		{
			FMeshDescription* Desc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Desc)
			{
				return FLT_MAX;
			}
			FStaticMeshAttributes Attr(*Desc);
			TPolygonGroupAttributesRef<FName> Names = Attr.GetPolygonGroupMaterialSlotNames();
			TVertexAttributesRef<FVector3f> Pos = Attr.GetVertexPositions();
			float MinZ = FLT_MAX;
			for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
			{
				if (Names[G] != FName(SlotName))
				{
					continue;
				}
				for (const FPolygonID P : Desc->GetPolygonGroupPolygons(G))
				{
					for (const FVertexInstanceID VI : Desc->GetPolygonVertexInstances(P))
					{
						MinZ = FMath::Min(MinZ, Pos[Desc->GetVertexInstanceVertex(VI)].Z);
					}
				}
			}
			return MinZ;
		};

		It("carrefour AUTO : patch du revetement dominant NU pose au-dessus de tous les rubans",
			[this, SurfaceProfile, HasSlot]()
		{
			// v3 : un patch exige DEUX chaussees auto au noeud. Une secondaire de 9 m a
			// 2 files (fine_road large, marquee) croisee par une residentielle de 6 m a
			// 2 files (fine_road medium, marquee) ; noeud commun (50,50) INTERIEUR aux
			// deux polylignes : vrai carrefour, et vraie zone de roulement.
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_carrefour.json"));
			const FString Json =
				TEXT(R"({"roads":[{"pts":[[10,50],[50,50],[90,50]],"t":"secondary","w":9,"lanes":2},)")
				TEXT(R"({"pts":[[50,10],[50,50],[50,90]],"t":"residential","w":6,"lanes":2}]})");
			FFileHelper::SaveStringToFile(Json, *Path);

			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Un seul patch de carrefour"), Summary.JunctionPatches, 1);

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			// La dominante (secondaire) est une classe MARQUEE : le patch prend son
			// equivalent nu — et les segments du ruban au contact du noeud aussi.
			TestTrue(TEXT("Slot asphalte nu present (patch + tirets effaces au carrefour)"),
				HasSlot(Ground, TEXT("asphalt_road_tiggcjdo")));

			// Patch = 55 + Z de classe la plus haute du noeud (fine_road large, 13)
			// + 5 = 73 ; le ruban le plus haut culmine a 55 + 13 + 1,2 de micro-jitter.
			float MinZ = 0.f, MaxZ = 0.f;
			if (TestTrue(TEXT("Bornes Z lisibles"), GetMeshZBounds(Ground, MinZ, MaxZ)))
			{
				TestTrue(FString::Printf(TEXT("Patch au-dessus des rubans (Z max %.1f >= 72)"), MaxZ),
					MaxZ >= 72.f);
			}
		});

		// v3 (verdict DA : « peau de leopard » dans le lacis pieton du centre). Le
		// patch de carrefour est reserve aux rencontres de VOITURES : ni un croisement
		// de sentiers, ni une rue traversee par un sentier n'en recoivent.
		// v4 : le lacis pieton ne produit meme plus de geometrie — la preuve la plus
		// forte est desormais Roads == 0 sur une zone 100 % pietonne.
		It("zero ruban et zero patch sur un noeud pieton ou a une seule chaussee auto",
			[this, SurfaceProfile]()
		{
			const FString PedPath = FPaths::Combine(FPaths::ProjectSavedDir(),
				TEXT("Tests/mini_carrefour_pieton.json"));
			// Deux sentiers qui se croisent + une place pietonne qui passe par le meme
			// noeud : trois voies partagees, un vrai carrefour geometrique... et zero
			// chaussee auto.
			FFileHelper::SaveStringToFile(FString(
				TEXT(R"({"roads":[{"pts":[[10,50],[50,50],[90,50]],"t":"footway","w":3},)")
				TEXT(R"({"pts":[[50,10],[50,50],[50,90]],"t":"path","w":2.5},)")
				TEXT(R"({"pts":[[20,20],[50,50],[80,80]],"t":"pedestrian","w":8}]})")), *PedPath);
			const FCityStreamedSummary PedSummary = UCityImportTools::ImportCityStreamed(
				PedPath, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Zone 100 % pietonne : aucun ruban genere"), PedSummary.Roads, 0);
			TestEqual(TEXT("Carrefour purement pieton : aucun patch"),
				PedSummary.JunctionPatches, 0);

			// Mixte : une secondaire a 2 files traversee par un sentier. La dominante
			// est bien une chaussee auto, mais elle est SEULE : pas de patch non plus.
			const FString MixPath = FPaths::Combine(FPaths::ProjectSavedDir(),
				TEXT("Tests/mini_carrefour_mixte.json"));
			FFileHelper::SaveStringToFile(FString(
				TEXT(R"({"roads":[{"pts":[[10,50],[50,50],[90,50]],"t":"secondary","w":9,"lanes":2},)")
				TEXT(R"({"pts":[[50,10],[50,50],[50,90]],"t":"footway","w":2.5}]})")), *MixPath);
			const FCityStreamedSummary MixSummary = UCityImportTools::ImportCityStreamed(
				MixPath, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Mixte : seule la chaussee auto produit un ruban"),
				MixSummary.Roads, 1);
			TestEqual(TEXT("Une seule chaussee auto au noeud : aucun patch"),
				MixSummary.JunctionPatches, 0);
		});

		It("ordre z : dalle sous gravier sous asphalte, et plus aucun ruban pieton",
			[this, SurfaceProfile, SlotMinZ]()
		{
			// Trois rubans PARALLELES (aucun noeud partage : pas de carrefour, pas de
			// patch) — seul l'empilement par classe est teste. Le ruban du milieu a un
			// sommet INTERIEUR volontaire : c'est le piege qui a coute une generation
			// de proto (un sommet interieur d'une route SEULE passait pour un
			// carrefour — 3 042 faux carrefours sur 3 920 noeuds).
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_ordrez.json"));
			const FString Json =
				TEXT(R"({"roads":[{"pts":[[10,10],[90,10]],"t":"service","w":4,"surface":"gravel"},)")
				TEXT(R"({"pts":[[10,30],[50,30],[90,30]],"t":"service","w":4},)")
				TEXT(R"({"pts":[[10,50],[90,50]],"t":"footway","w":2.5}]})");
			FFileHelper::SaveStringToFile(Json, *Path);

			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Aucun carrefour : rubans paralleles, sommet interieur non partage"),
				Summary.JunctionPatches, 0);

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			const float ZGravel = SlotMinZ(Ground, TEXT("gravel_on_soil_okosdmp0"));
			const float ZAsphalt = SlotMinZ(Ground, TEXT("asphalt_road_tiggcjdo"));
			if (!TestTrue(TEXT("Les deux classes de ruban sont presentes"),
				ZGravel < FLT_MAX && ZAsphalt < FLT_MAX))
			{
				return;
			}
			// v4 : le sentier pieton du JSON ne produit plus rien du tout.
			TestTrue(TEXT("Aucun ruban pieton (le sentier est devenu la dalle)"),
				SlotMinZ(Ground, TEXT("herringbone_brick_pavement_ue3gbepkw")) == FLT_MAX);
			TestTrue(FString::Printf(TEXT("gravier %.1f < asphalte %.1f"), ZGravel, ZAsphalt),
				ZGravel < ZAsphalt);
			// Determinisme : les valeurs sont les offsets de classe (55 + 0/4) au
			// micro-jitter pres (< 1,2 cm), PAS l'ancien ordre d'arrivee.
			TestTrue(FString::Printf(TEXT("gravier a 55 (%.1f)"), ZGravel),
				ZGravel >= 55.f && ZGravel < 56.5f);
			TestTrue(FString::Printf(TEXT("asphalte a 59 (%.1f)"), ZAsphalt),
				ZAsphalt >= 59.f && ZAsphalt < 60.5f);

			// v4 point 3 — JONCTION DALLE / GRAVIER. La dalle porteuse est SOUS tous
			// les rubans : l'allee de gravier se lit par-dessus, jamais l'inverse.
			UStaticMesh* Slab = LoadTestMesh(TEXT("SM_Slab_0_0"));
			if (TestNotNull(TEXT("SM_Slab_0_0 genere"), Slab))
			{
				const float ZSlab = SlotMinZ(Slab, TEXT("dirty_sidewalk_tiles_ugxjcdpn"));
				if (TestTrue(TEXT("La dalle porte bien son revetement"), ZSlab < FLT_MAX))
				{
					TestTrue(FString::Printf(TEXT("dalle %.1f < gravier %.1f"), ZSlab, ZGravel),
						ZSlab < ZGravel);
				}
			}
		});
	});

	// J3c point 3 « VOIRIE » — la structure de la rue. Verdict utilisateur sur la
	// v4b : « il manque la structure des rues (rives) » et « morceaux perdus ». Trois
	// chantiers, tous testes ici :
	//   1. le ruban de chaussee se RE-PARTITIONNE en chaussee + 2 bordures en relief
	//      (face verticale 12 cm + chant 15 cm) + 2 rives de 1,70 m en classe dalle ;
	//   2. un PASSAGE PIETON par noeud partage entre une chaussee auto et une voie
	//      pietonne (reporte si un patch de carrefour couvre deja le noeud) ;
	//   3. les rubans ORPHELINS (< 25 m, aucun noeud partage) ne sont plus generes.
	// Les materiaux Megascans ne sont pas requis : c'est le decoupage geometrique et
	// le comptage qui sont testes, le repli materiau ne change aucune geometrie.
	Describe("Voirie", [this]()
	{
		auto SurfaceProfile = []()
		{
			FCityGenProfile P;
			P.bWindowReveals = true;
			P.bSplitWallGlass = true;
			P.bNanite = true;
			P.bPBRMaterials = true;
			P.bSurfaceMaterials = true;
			return P;
		};
		auto WriteJson = [](const TCHAR* Name, const FString& Json)
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(),
				FString::Printf(TEXT("Tests/%s"), Name));
			FFileHelper::SaveStringToFile(Json, *Path);
			return Path;
		};
		// Polygones d'un slot donne : c'est LE compteur du point 1 (une section de
		// mesh par classe, un quad par bande et par cote).
		auto SlotPolys = [](UStaticMesh* Mesh, const TCHAR* SlotName) -> int32
		{
			FMeshDescription* Desc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Desc)
			{
				return 0;
			}
			TPolygonGroupAttributesRef<FName> Names =
				FStaticMeshAttributes(*Desc).GetPolygonGroupMaterialSlotNames();
			int32 Count = 0;
			for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
			{
				if (Names[G] == FName(SlotName))
				{
					Count += Desc->GetNumPolygonGroupPolygons(G);
				}
			}
			return Count;
		};
		// Bornes Z des sommets d'un slot : preuve du relief de la bordure et de
		// l'altitude du passage pieton (entre la chaussee et le chant).
		auto SlotZBounds = [](UStaticMesh* Mesh, const TCHAR* SlotName, float& OutMin, float& OutMax) -> bool
		{
			FMeshDescription* Desc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Desc)
			{
				return false;
			}
			FStaticMeshAttributes Attr(*Desc);
			TPolygonGroupAttributesRef<FName> Names = Attr.GetPolygonGroupMaterialSlotNames();
			TVertexAttributesRef<FVector3f> Pos = Attr.GetVertexPositions();
			OutMin = FLT_MAX;
			OutMax = -FLT_MAX;
			for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
			{
				if (Names[G] != FName(SlotName))
				{
					continue;
				}
				for (const FPolygonID P : Desc->GetPolygonGroupPolygons(G))
				{
					for (const FVertexInstanceID VI : Desc->GetPolygonVertexInstances(P))
					{
						const float Z = Pos[Desc->GetVertexInstanceVertex(VI)].Z;
						OutMin = FMath::Min(OutMin, Z);
						OutMax = FMath::Max(OutMax, Z);
					}
				}
			}
			return OutMin < FLT_MAX;
		};
		// Distance 2D minimale entre un point et les sommets d'un slot : c'est ainsi
		// qu'on prouve que la bordure S'INTERROMPT sur l'emprise d'un patch.
		auto SlotMinDistTo = [](UStaticMesh* Mesh, const TCHAR* SlotName, const FVector2D& P) -> float
		{
			FMeshDescription* Desc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Desc)
			{
				return -1.f;
			}
			FStaticMeshAttributes Attr(*Desc);
			TPolygonGroupAttributesRef<FName> Names = Attr.GetPolygonGroupMaterialSlotNames();
			TVertexAttributesRef<FVector3f> Pos = Attr.GetVertexPositions();
			float Best = FLT_MAX;
			for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
			{
				if (Names[G] != FName(SlotName))
				{
					continue;
				}
				for (const FPolygonID Poly : Desc->GetPolygonGroupPolygons(G))
				{
					for (const FVertexInstanceID VI : Desc->GetPolygonVertexInstances(Poly))
					{
						const FVector3f V = Pos[Desc->GetVertexInstanceVertex(VI)];
						Best = FMath::Min(Best, (float)FVector2D::Distance(FVector2D(V.X, V.Y), P));
					}
				}
			}
			return Best < FLT_MAX ? Best : -1.f;
		};

		It("bordures : une rue droite donne 7 quads par section (1 chaussee, 4 bordure, 2 rives)",
			[this, SurfaceProfile, WriteJson, SlotPolys, SlotZBounds]()
		{
			// UNE rue, UN segment, aucun noeud partage : le comptage est exact et
			// lisible a la main. Residentielle de 6 m annoncee a 2 files -> classe
			// fine_road_viciaalew (ZClassCm = 10), la seule ou l'ecart chaussee/chant
			// se verifie sans ambiguite.
			const FString Path = WriteJson(TEXT("voirie_rue.json"),
				TEXT(R"({"roads":[{"pts":[[10,50],[90,50]],"t":"residential","w":6,"lanes":2}]})"));
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Un ruban"), Summary.Roads, 1);
			TestEqual(TEXT("4 quads de bordure = 2 cotes x (face + chant)"), Summary.CurbQuads, 4);

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			TestEqual(TEXT("Chaussee : 1 quad"), SlotPolys(Ground, TEXT("fine_road_viciaalew")), 1);
			TestEqual(TEXT("Bordure : 4 quads"), SlotPolys(Ground, TEXT("curb")), 4);
			TestEqual(TEXT("Rives : 2 quads de dalle"),
				SlotPolys(Ground, TEXT("dirty_sidewalk_tiles_ugxjcdpn")), 2);

			// Relief : la bordure part du niveau de la chaussee et monte de 12 cm ;
			// les rives sont de plain-pied avec le chant, donc a son Z maximal.
			float RoadMin = 0.f, RoadMax = 0.f, CurbMin = 0.f, CurbMax = 0.f;
			float WalkMin = 0.f, WalkMax = 0.f;
			if (SlotZBounds(Ground, TEXT("fine_road_viciaalew"), RoadMin, RoadMax) &&
				SlotZBounds(Ground, TEXT("curb"), CurbMin, CurbMax) &&
				SlotZBounds(Ground, TEXT("dirty_sidewalk_tiles_ugxjcdpn"), WalkMin, WalkMax))
			{
				TestTrue(FString::Printf(TEXT("Bordure : pied au niveau de la chaussee (%.1f vs %.1f)"),
					CurbMin, RoadMin), FMath::IsNearlyEqual(CurbMin, RoadMin, 0.05f));
				TestTrue(FString::Printf(TEXT("Bordure : relief de 12 cm (%.2f)"), CurbMax - CurbMin),
					FMath::IsNearlyEqual(CurbMax - CurbMin, 12.f, 0.05f));
				TestTrue(FString::Printf(TEXT("Rives de plain-pied avec le chant (%.1f vs %.1f)"),
					WalkMin, CurbMax), FMath::IsNearlyEqual(WalkMin, CurbMax, 0.05f) &&
					FMath::IsNearlyEqual(WalkMax, CurbMax, 0.05f));
			}

			// Emprise transversale : chaussee 6 m -> ruban total 2 x (300 + 15 + 170).
			float MinY = FLT_MAX, MaxY = -FLT_MAX;
			if (FMeshDescription* Desc = Ground->GetMeshDescription(0))
			{
				TVertexAttributesRef<FVector3f> Pos = FStaticMeshAttributes(*Desc).GetVertexPositions();
				for (const FVertexID V : Desc->Vertices().GetElementIDs())
				{
					MinY = FMath::Min(MinY, Pos[V].Y);
					MaxY = FMath::Max(MaxY, Pos[V].Y);
				}
				TestTrue(FString::Printf(TEXT("Emprise du ruban : %.1f m (attendu 9,70)"),
					(MaxY - MinY) * 0.01f), FMath::IsNearlyEqual(MaxY - MinY, 970.f, 1.f));
			}
		});

		It("gravier et profil mobile : aucune bordure, aucune rive (golden path)",
			[this, SurfaceProfile, WriteJson, SlotPolys]()
		{
			// (a) Allee de gravier : ce n'est pas une chaussee auto — elle garde son
			//     ruban d'un seul tenant, sans bordure (une bordure autour d'un chemin
			//     de terre serait un contresens).
			const FString Path = WriteJson(TEXT("voirie_gravier.json"),
				TEXT(R"({"roads":[{"pts":[[10,50],[90,50]],"t":"service","w":4,"surface":"gravel"},)")
				TEXT(R"({"pts":[[10,20],[90,20]],"t":"service","w":4,"surface":"gravel"}]})"));
			const FCityStreamedSummary Gravel = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Gravier : aucun quad de bordure"), Gravel.CurbQuads, 0);
			UStaticMesh* GravelGround = LoadTestMesh(TEXT("SM_Ground_0_0"));
			TestEqual(TEXT("Gravier : aucun quad de bordure sur le mesh"),
				SlotPolys(GravelGround, TEXT("curb")), 0);

			// (b) GOLDEN PATH MOBILE : la meme rue, profil par defaut. Rien de la v5 ne
			//     doit apparaitre — ni bande, ni bordure, ni passage.
			const FString Street = WriteJson(TEXT("voirie_rue_mobile.json"),
				TEXT(R"({"roads":[{"pts":[[10,50],[90,50]],"t":"residential","w":6,"lanes":2},)")
				TEXT(R"({"pts":[[50,10],[50,90]],"t":"footway","w":2.5}]})"));
			const FCityStreamedSummary Mobile = UCityImportTools::ImportCityStreamed(
				Street, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, FCityGenProfile());
			TestEqual(TEXT("Mobile : aucune bordure"), Mobile.CurbQuads, 0);
			TestEqual(TEXT("Mobile : aucun passage pieton"), Mobile.Crossings, 0);
			TestEqual(TEXT("Mobile : aucun ruban ecarte"), Mobile.OrphanRibbons, 0);
			TestEqual(TEXT("Mobile : le sentier garde son ruban historique"), Mobile.Roads, 2);
			UStaticMesh* MobileGround = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (TestNotNull(TEXT("SM_Ground_0_0 genere"), MobileGround))
			{
				TestEqual(TEXT("Mobile : 2 slots (Wall, Glass) et rien d'autre"),
					MobileGround->GetStaticMaterials().Num(), 2);
			}
		});

		It("la bordure s'interrompt sur l'emprise du patch de carrefour",
			[this, SurfaceProfile, WriteJson, SlotPolys, SlotMinDistTo]()
		{
			// Meme carrefour que la spec des patchs : secondaire 9 m x residentielle
			// 6 m, noeud INTERIEUR aux deux polylignes en (50,50). Rayon du patch =
			// demi-ruban max (450 + 15 + 170 = 635) + 1 m = 735 cm.
			const FString Path = WriteJson(TEXT("voirie_carrefour.json"),
				TEXT(R"({"roads":[{"pts":[[10,50],[50,50],[90,50]],"t":"secondary","w":9,"lanes":2},)")
				TEXT(R"({"pts":[[50,10],[50,50],[50,90]],"t":"residential","w":6,"lanes":2}]})"));
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Un seul patch de carrefour"), Summary.JunctionPatches, 1);
			TestTrue(FString::Printf(TEXT("Des bordures ailleurs qu'au carrefour (%d quads)"),
				Summary.CurbQuads), Summary.CurbQuads > 0);

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			TestTrue(TEXT("Slot de bordure present"), SlotPolys(Ground, TEXT("curb")) > 0);
			// Sans decoupage, la bordure passerait a 450 cm du noeud (sa demi-chaussee).
			// Avec, le sommet de bordure le plus proche est repousse hors du disque.
			const float Dist = SlotMinDistTo(Ground, TEXT("curb"), FVector2D(5000.0, 5000.0));
			TestTrue(FString::Printf(TEXT("Bordure repoussee hors du patch (%.0f cm du noeud, rayon 735)"), Dist),
				Dist > 600.f);
		});

		It("passage pieton : un quad en travers de la chaussee au noeud partage avec une voie pietonne",
			[this, SurfaceProfile, WriteJson, SlotPolys, SlotZBounds]()
		{
			// Une residentielle a 2 files traversee par un sentier au milieu. Une SEULE
			// chaussee auto au noeud : pas de patch (regle v3), donc le passage se pose.
			const FString Path = WriteJson(TEXT("voirie_passage.json"),
				TEXT(R"({"roads":[{"pts":[[10,50],[50,50],[90,50]],"t":"residential","w":6,"lanes":2},)")
				TEXT(R"({"pts":[[50,10],[50,50],[50,90]],"t":"footway","w":2.5}]})"));
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Aucun patch (une seule chaussee auto au noeud)"),
				Summary.JunctionPatches, 0);
			TestEqual(TEXT("Un passage pose"), Summary.Crossings, 1);
			TestEqual(TEXT("Aucun passage reporte"), Summary.CrossingsDeferred, 0);
			TestEqual(TEXT("Le sentier ne produit toujours aucun ruban"), Summary.Roads, 1);

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			TestEqual(TEXT("Un seul quad de passage"),
				SlotPolys(Ground, TEXT("pedestrian_crossing_lines_veggecd")), 1);

			// Altitude : au-dessus de la chaussee, SOUS le chant des bordures — un
			// passage s'arrete au pied du trottoir, il n'y monte pas.
			float RoadMin = 0.f, RoadMax = 0.f, CurbMin = 0.f, CurbMax = 0.f, CrossMin = 0.f, CrossMax = 0.f;
			if (SlotZBounds(Ground, TEXT("fine_road_viciaalew"), RoadMin, RoadMax) &&
				SlotZBounds(Ground, TEXT("curb"), CurbMin, CurbMax) &&
				SlotZBounds(Ground, TEXT("pedestrian_crossing_lines_veggecd"), CrossMin, CrossMax))
			{
				TestTrue(FString::Printf(TEXT("Passage (%.1f) au-dessus de la chaussee (%.1f)"),
					CrossMin, RoadMax), CrossMin > RoadMax);
				TestTrue(FString::Printf(TEXT("Passage (%.1f) sous le chant (%.1f)"), CrossMax, CurbMax),
					CrossMax < CurbMax);
			}

			// Emprise : 4 m dans l'axe de la rue, la largeur de CHAUSSEE en travers
			// (jamais sur les rives) — le quad est centre sur le noeud (50,50).
			FMeshDescription* Desc = Ground->GetMeshDescription(0);
			if (TestNotNull(TEXT("MeshDescription du sol"), Desc))
			{
				FStaticMeshAttributes Attr(*Desc);
				TPolygonGroupAttributesRef<FName> Names = Attr.GetPolygonGroupMaterialSlotNames();
				TVertexAttributesRef<FVector3f> Pos = Attr.GetVertexPositions();
				FBox2D Box(ForceInit);
				for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
				{
					if (Names[G] != FName(TEXT("pedestrian_crossing_lines_veggecd")))
					{
						continue;
					}
					for (const FPolygonID P : Desc->GetPolygonGroupPolygons(G))
					{
						for (const FVertexInstanceID VI : Desc->GetPolygonVertexInstances(P))
						{
							const FVector3f V = Pos[Desc->GetVertexInstanceVertex(VI)];
							Box += FVector2D(V.X, V.Y);
						}
					}
				}
				if (TestTrue(TEXT("Emprise du passage lisible"), Box.bIsValid != 0))
				{
					const FVector2D Size = Box.GetSize();
					TestTrue(FString::Printf(TEXT("4 m dans l'axe de la rue (%.0f cm)"), Size.X),
						FMath::IsNearlyEqual((float)Size.X, 400.f, 1.f));
					TestTrue(FString::Printf(TEXT("Largeur de CHAUSSEE en travers (%.0f cm, attendu 600)"),
						Size.Y), FMath::IsNearlyEqual((float)Size.Y, 600.f, 1.f));
					TestTrue(FString::Printf(TEXT("Centre sur le noeud (%.0f, %.0f)"),
						Box.GetCenter().X, Box.GetCenter().Y),
						FVector2D::Distance(Box.GetCenter(), FVector2D(5000.0, 5000.0)) < 1.0);
				}
			}
		});

		It("passage REPORTE quand un patch de carrefour couvre deja le noeud",
			[this, SurfaceProfile, WriteJson]()
		{
			// Deux chaussees auto + un sentier au meme noeud : le carrefour l'emporte,
			// le passage est compte a part (le raccord bord-de-patch est au backlog).
			const FString Path = WriteJson(TEXT("voirie_passage_patch.json"),
				TEXT(R"({"roads":[{"pts":[[10,50],[50,50],[90,50]],"t":"secondary","w":9,"lanes":2},)")
				TEXT(R"({"pts":[[50,10],[50,50],[50,90]],"t":"residential","w":6,"lanes":2},)")
				TEXT(R"({"pts":[[20,20],[50,50],[80,80]],"t":"footway","w":2.5}]})"));
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Le carrefour est bien patche"), Summary.JunctionPatches, 1);
			TestEqual(TEXT("Aucun passage pose sous le patch"), Summary.Crossings, 0);
			TestEqual(TEXT("Un passage reporte"), Summary.CrossingsDeferred, 1);
		});

		It("fragment orphelin : un ruban court et deconnecte n'est plus genere",
			[this, SurfaceProfile, WriteJson]()
		{
			// Deux rues qui se rejoignent (reseau) + un moignon de 15 m pose a l'ecart,
			// sans le moindre noeud commun : c'est le « morceau perdu » de la v4b.
			const FString Path = WriteJson(TEXT("voirie_orphelin.json"),
				TEXT(R"({"roads":[{"pts":[[10,50],[50,50],[90,50]],"t":"secondary","w":9,"lanes":2},)")
				TEXT(R"({"pts":[[50,10],[50,50],[50,90]],"t":"residential","w":6,"lanes":2},)")
				TEXT(R"({"pts":[[200,200],[215,200]],"t":"service","w":4}]})"));
			const FCityStreamedSummary Summary = UCityImportTools::ImportCityStreamed(
				Path, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Seules les deux rues du reseau sont generees"), Summary.Roads, 2);
			TestEqual(TEXT("Un ruban orphelin ecarte"), Summary.OrphanRibbons, 1);

			// Contre-epreuve : un moignon de la MEME longueur, mais raccroche au
			// reseau par un noeud partage, reste genere (le critere est la solitude,
			// pas la brievete).
			const FString Linked = WriteJson(TEXT("voirie_orphelin_lie.json"),
				TEXT(R"({"roads":[{"pts":[[10,50],[50,50],[90,50]],"t":"secondary","w":9,"lanes":2},)")
				TEXT(R"({"pts":[[50,10],[50,50],[50,90]],"t":"residential","w":6,"lanes":2},)")
				TEXT(R"({"pts":[[90,50],[105,50]],"t":"service","w":4}]})"));
			const FCityStreamedSummary LinkedSummary = UCityImportTools::ImportCityStreamed(
				Linked, FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, SurfaceProfile());
			TestEqual(TEXT("Moignon RACCROCHE : genere"), LinkedSummary.Roads, 3);
			TestEqual(TEXT("Moignon raccroche : aucun ruban ecarte"), LinkedSummary.OrphanRibbons, 0);
		});

		It("espaces verts : UNE seule herbe pour parcs et bois (fin du spaghetti)",
			[this, SurfaceProfile, WriteJson]()
		{
			auto HasSurfaceSlot = [](const TCHAR* SlotName)
			{
				UStaticMesh* Mesh = LoadTestMesh(TEXT("SM_Surface_0_0"));
				if (!Mesh)
				{
					return false;
				}
				for (const FStaticMaterial& M : Mesh->GetStaticMaterials())
				{
					if (M.MaterialSlotName == FName(SlotName))
					{
						return true;
					}
				}
				return false;
			};
			// Quatre verts qui se CHEVAUCHENT dans la meme cellule, dont deux bois :
			// l'alternance historique en aurait fait un patchwork de 3 herbes.
			const FString Path = WriteJson(TEXT("voirie_verts.json"),
				TEXT(R"({"green":[{"k":"park","pts":[[10,10],[90,10],[90,90],[10,90]]},)")
				TEXT(R"({"k":"forest","pts":[[20,20],[80,20],[80,80],[20,80]]},)")
				TEXT(R"({"k":"forest","pts":[[30,30],[70,30],[70,70],[30,70]]},)")
				TEXT(R"({"k":"grass","pts":[[40,40],[60,40],[60,60],[40,60]]}]})"));
			const FCitySurfacesSummary Summary = UCityImportTools::ImportCitySurfaces(
				Path, TEXT("/Game/Dev/Test/City"), FString(), 100.f, FVector::ZeroVector,
				SurfaceProfile());
			TestEqual(TEXT("Les 4 polygones verts sont generes"), Summary.Green, 4);
			TestTrue(TEXT("Herbe tondue : la seule herbe posee"),
				HasSurfaceSlot(TEXT("grass_cut_pjxmz0")));
			TestFalse(TEXT("Plus d'herbe haute"), HasSurfaceSlot(TEXT("uncut_grass_oilpt20")));
			TestFalse(TEXT("Plus d'herbe folle"), HasSurfaceSlot(TEXT("wild_grass_sfknaeoa")));

			// Le flag de profil rend l'alternance (usage futur berges/friches) : la
			// mecanique reste en code, seule sa valeur par defaut a change.
			FCityGenProfile Varied = SurfaceProfile();
			Varied.bVariedGrass = true;
			UCityImportTools::ImportCitySurfaces(Path, TEXT("/Game/Dev/Test/City"), FString(),
				100.f, FVector::ZeroVector, Varied);
			TestTrue(TEXT("bVariedGrass : les herbes de bois reviennent"),
				HasSurfaceSlot(TEXT("uncut_grass_oilpt20")) || HasSurfaceSlot(TEXT("wild_grass_sfknaeoa")));
		});
	});

	// Verrou 2 « collision batiments » : les murs Nanite ne servent JAMAIS de
	// collision (fallback decime ~0,1 % = facades traversables, sonde 2026-07-25) —
	// chaque cellule desktop recoit un SM_Bldg_*_Col en prismes fermes, cable en
	// ComplexCollisionMesh du SM_Bldg_*_Wall. TEST SENTINELLE : preuve par line
	// traces PHYSIQUES dans un monde de jeu dedie (piege connu : les traces sont
	// muettes sans monde initialise avec sa scene physique).
	Describe("CollisionBatiments", [this]()
	{
		// Fixture a plat (profil desktop manuel SANS MNT, Z deterministes) : deux
		// batiments 20x20 m (h 12 et 9 m) separes par une rue de 20 m d'axe Y en
		// x = 40 m, tout en cellule (0,0).
		auto WriteFixture = [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_bldgcol.json"));
			const FString Json = TEXT(R"({"buildings":[{"pts":[[10,10],[30,10],[30,30],[10,30]],"h":12,"u":"res"},)")
				TEXT(R"({"pts":[[50,10],[70,10],[70,30],[50,30]],"h":9,"u":"com"}]})");
			FFileHelper::SaveStringToFile(Json, *Path);
			return Path;
		};
		auto DesktopFlat = []()
		{
			FCityGenProfile P;
			P.bWindowReveals = true;
			P.bSplitWallGlass = true;
			P.bNanite = true;
			P.bPBRMaterials = true;
			return P;
		};

		It("sentinelle : prismes _Col cables — HIT facade a 50 cm pres, MISS rue, HIT toits au Z attendu", [this, WriteFixture, DesktopFlat]()
		{
			const FString Path = WriteFixture();
			// La generation desktop cree ET cable les _Col nativement ; l'outil par
			// cellule repasse par-dessus — les deux chemins doivent converger vers le
			// meme asset cable (regeneration en place).
			UCityImportTools::ImportCityStreamed(Path, FString(), TEXT("/Game/Dev/Test/City"),
				TEXT("/Game/Dev/Test/Blocks"), FString(), FString(), 100.f, 200.f, 400.f,
				FVector::ZeroVector, DesktopFlat());
			const FCityBldgColSummary Summary = UCityImportTools::GenerateBuildingCollisionCell(
				Path, TEXT("/Game/Dev/Test/City"), 100.f, 0, 0, DesktopFlat());
			TestEqual(TEXT("2 batiments dans la cellule"), Summary.Buildings, 2);
			TestTrue(TEXT("Wall cable"), Summary.bWallWired);
			TestTrue(TEXT("Packages sauves"), Summary.bSaved);
			// 2 prismes rectangulaires fermes : (4 quads lateraux + 2 chapeaux) x 2 tris.
			TestEqual(TEXT("24 tris (2 prismes fermes)"), Summary.Triangles, 24);

			UStaticMesh* Col = LoadTestMesh(TEXT("SM_Bldg_0_0_Col"));
			UStaticMesh* Wall = LoadTestMesh(TEXT("SM_Bldg_0_0_Wall"));
			if (!TestNotNull(TEXT("SM_Bldg_0_0_Col"), Col) || !TestNotNull(TEXT("SM_Bldg_0_0_Wall"), Wall))
			{
				return;
			}
			TestFalse(TEXT("_Col sans Nanite"), Col->GetNaniteSettings().bEnabled);
			if (TestNotNull(TEXT("BodySetup _Col"), Col->GetBodySetup()))
			{
				TestEqual(TEXT("_Col complex-as-simple"),
					(int32)Col->GetBodySetup()->CollisionTraceFlag, (int32)CTF_UseComplexAsSimple);
				TestEqual(TEXT("_Col sans boite simple"), Col->GetBodySetup()->AggGeom.BoxElems.Num(), 0);
			}
			TestTrue(TEXT("Wall.ComplexCollisionMesh -> _Col"), Wall->ComplexCollisionMesh.Get() == Col);
			if (Wall->GetBodySetup())
			{
				TestEqual(TEXT("Wall complex-as-simple"),
					(int32)Wall->GetBodySetup()->CollisionTraceFlag, (int32)CTF_UseComplexAsSimple);
			}

			// --- Preuve physique : monde de jeu dedie + scene physique initialisee. ---
			FStaticMeshCompilingManager::Get().FinishAllCompilation();
			UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("BldgColTraceWorld"));
			if (!TestNotNull(TEXT("Monde de test"), World))
			{
				return;
			}
			FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
			Ctx.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();

			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
				FVector::ZeroVector, FRotator::ZeroRotator);
			Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Col);
			Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Actor->GetStaticMeshComponent()->RecreatePhysicsState();
			// Un tick pousse les corps statiques dans la structure d'acceleration SQ.
			World->Tick(LEVELTICK_All, 0.016f);

			FCollisionQueryParams Params(FName(TEXT("BldgColSentinel")), /*bTraceComplex=*/true);
			FHitResult Hit;

			// 1. Facade ouest du batiment A (plan x = 1000 cm), rayon +X a mi-hauteur.
			bool bHit = World->LineTraceSingleByChannel(Hit,
				FVector(-500, 2000, 600), FVector(2500, 2000, 600), ECC_Visibility, Params);
			TestTrue(TEXT("Facade : HIT"), bHit);
			if (bHit)
			{
				TestTrue(FString::Printf(TEXT("Facade touchee a x=%.1f (attendu 1000 +/- 50)"), Hit.Location.X),
					FMath::Abs(Hit.Location.X - 1000.f) <= 50.f);
			}
			// 2. Axe de la rue (x = 40 m) : 70 m sans obstacle.
			bHit = World->LineTraceSingleByChannel(Hit,
				FVector(4000, -2000, 600), FVector(4000, 5000, 600), ECC_Visibility, Params);
			TestFalse(TEXT("Rue : MISS"), bHit);
			// 3. Toits : rayons verticaux, Z attendus 1200 (A) et 900 (B).
			bHit = World->LineTraceSingleByChannel(Hit,
				FVector(2000, 2000, 5000), FVector(2000, 2000, -200), ECC_Visibility, Params);
			TestTrue(TEXT("Toit A : HIT"), bHit);
			if (bHit)
			{
				TestTrue(FString::Printf(TEXT("Toit A a z=%.1f (attendu 1200 +/- 1)"), Hit.Location.Z),
					FMath::Abs(Hit.Location.Z - 1200.f) <= 1.f);
			}
			bHit = World->LineTraceSingleByChannel(Hit,
				FVector(6000, 2000, 5000), FVector(6000, 2000, -200), ECC_Visibility, Params);
			TestTrue(TEXT("Toit B : HIT"), bHit);
			if (bHit)
			{
				TestTrue(FString::Printf(TEXT("Toit B a z=%.1f (attendu 900 +/- 1)"), Hit.Location.Z),
					FMath::Abs(Hit.Location.Z - 900.f) <= 1.f);
			}

			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		});

		It("raises when the cell has no building", [this, WriteFixture, DesktopFlat]()
		{
			const FString Path = WriteFixture();
			AddExpectedError(TEXT("No building centroid in cell"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::GenerateBuildingCollisionCell(
				Path, TEXT("/Game/Dev/Test/City"), 100.f, 9, 9, DesktopFlat());
		});

		It("raises when the file does not exist", [this, DesktopFlat]()
		{
			AddExpectedError(TEXT("Cannot read district file"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::GenerateBuildingCollisionCell(
				TEXT("Z:/nope.json"), TEXT("/Game/Dev/Test/City"), 100.f, 0, 0, DesktopFlat());
		});
	});

	// -------------------------------------------------------------------------
	// J3c « MAQUETTE DU SOL » — LE SOL EST PEINT, LE RELIEF EST MAILLE.
	// Ce qui se verifie ici est la BASCULE, pas le rendu :
	//   - la dalle de la cellule prend son instance de materiau (slot ground_masked) ;
	//   - le ruban de chaussee au niveau du sol DISPARAIT (il ferait doublon avec la
	//     peinture) mais le PONT reste (aucun masque de sol ne peut rendre un tablier) ;
	//   - bordures, passages et tirets arrivent du JSON de la cellule, deja decoupes,
	//     et s'empilent aux bons Z les uns par rapport aux autres ;
	//   - sans masque, sans instance de materiau ou sur une taille de cellule qui ne
	//     correspond pas, RIEN ne bascule : effacer la chaussee sans rien mettre a la
	//     place serait pire que de ne rien faire.
	// -------------------------------------------------------------------------
	Describe("Maquette du sol", [this]()
	{
		auto MaskDir = []()
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/SolsMasques"));
		};
		auto WriteJson = [](const FString& Path, const FString& Json)
		{
			FFileHelper::SaveStringToFile(Json, *Path);
			return Path;
		};
		// Un dossier de masques NEUF a chaque test : un sols_*.json oublie par le test
		// precedent ferait basculer (ou non) le suivant au hasard.
		auto FreshMaskDir = [MaskDir]()
		{
			const FString Dir = MaskDir();
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
			IFileManager::Get().MakeDirectory(*Dir, true);
			return Dir;
		};
		// L'instance de materiau de la cellule, fabriquee EN MEMOIRE au bon chemin de
		// package : le generateur la trouve par LoadObject sans qu'il faille ecrire un
		// uasset ni dependre de Tools/import_ground_masks.py.
		auto MakeCellMaterial = [](int32 CellX, int32 CellY)
		{
			const FString Name = FString::Printf(TEXT("MI_CityGround_%d_%d"), CellX, CellY);
			const FString Pkg = FString::Printf(TEXT("/Game/Dev/Test/Ground/%s"), *Name);
			if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr,
				*FString::Printf(TEXT("%s.%s"), *Pkg, *Name), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				return Existing;
			}
			UPackage* Package = CreatePackage(*Pkg);
			UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(
				Package, *Name, RF_Public | RF_Standalone);
			MIC->Parent = UMaterial::GetDefaultMaterial(MD_Surface);
			return Cast<UMaterialInterface>(MIC);
		};
		auto MaskedProfile = [MaskDir]()
		{
			FCityGenProfile P;
			P.bWindowReveals = true;
			P.bSplitWallGlass = true;
			P.bNanite = true;
			P.bPBRMaterials = true;
			P.bSurfaceMaterials = true;
			P.bMaskedGround = true;
			P.GroundMasksPath = MaskDir();
			P.GroundMasksAssetFolder = TEXT("/Game/Dev/Test/Ground");
			return P;
		};
		auto SlotPolys = [](UStaticMesh* Mesh, const TCHAR* SlotName) -> int32
		{
			FMeshDescription* Desc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Desc)
			{
				return 0;
			}
			TPolygonGroupAttributesRef<FName> Names =
				FStaticMeshAttributes(*Desc).GetPolygonGroupMaterialSlotNames();
			int32 Count = 0;
			for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
			{
				if (Names[G] == FName(SlotName))
				{
					Count += Desc->GetNumPolygonGroupPolygons(G);
				}
			}
			return Count;
		};
		auto SlotZBounds = [](UStaticMesh* Mesh, const TCHAR* SlotName, float& OutMin, float& OutMax) -> bool
		{
			FMeshDescription* Desc = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Desc)
			{
				return false;
			}
			FStaticMeshAttributes Attr(*Desc);
			TPolygonGroupAttributesRef<FName> Names = Attr.GetPolygonGroupMaterialSlotNames();
			TVertexAttributesRef<FVector3f> Pos = Attr.GetVertexPositions();
			OutMin = FLT_MAX;
			OutMax = -FLT_MAX;
			for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
			{
				if (Names[G] != FName(SlotName))
				{
					continue;
				}
				for (const FPolygonID P : Desc->GetPolygonGroupPolygons(G))
				{
					for (const FVertexInstanceID VI : Desc->GetPolygonVertexInstances(P))
					{
						const float Z = Pos[Desc->GetVertexInstanceVertex(VI)].Z;
						OutMin = FMath::Min(OutMin, Z);
						OutMax = FMath::Max(OutMax, Z);
					}
				}
			}
			return OutMin <= OutMax;
		};
		// UNE rue au sol + UN pont, cellule de 100 m : chaque compte du test se relit
		// a la main sur ces deux lignes.
		auto WriteCity = [WriteJson]()
		{
			return WriteJson(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/maquette_ville.json")),
				TEXT(R"({"roads":[{"pts":[[10,50],[90,50]],"t":"residential","w":6,"lanes":2},)")
				TEXT(R"({"pts":[[10,20],[90,20]],"t":"secondary","w":8,"lanes":2,"bridge":true}]})"));
		};
		// La MEME ville sans le pont : le relief du masque s'y compte a l'unite, sans
		// se melanger aux bordures que le tablier du pont porte legitimement.
		auto WriteCityNoBridge = [WriteJson]()
		{
			return WriteJson(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/maquette_ville_sans_pont.json")),
				TEXT(R"({"roads":[{"pts":[[10,50],[90,50]],"t":"residential","w":6,"lanes":2}]})"));
		};
		// Bordure a y = 47 : la chaussee (centree en y = 50, large de 6 m) est donc A
		// GAUCHE du sens de parcours +X — exactement la convention du prep.
		auto WriteMask = [WriteJson](const FString& Dir, float CellSizeM)
		{
			return WriteJson(FPaths::Combine(Dir, TEXT("sols_0_0.json")),
				FString::Printf(TEXT(R"({"cell":[0,0],"cellSizeM":%.1f,"origin":[0,0],)"), CellSizeM) +
				TEXT(R"("curbs":[[[20,47],[80,47]]],)")
				TEXT(R"("crossings":[{"p":[50,50],"d":[1,0],"halfW":3}],)")
				TEXT(R"("axial":[[30,50,33,50]]})"));
		};
		// FINITION_SOL V3 : LE MEME masque, plus une BORDURETTE D'HERBE — un segment
		// `grassEdges` de 40 m pose loin de la chaussee (y = 80). Masque separe : le
		// champ absent du masque ci-dessus verrouille du meme coup la
		// retro-compatibilite (un masque cuit avant la v3 ne pose AUCUNE bordurette).
		auto WriteMaskHerbe = [WriteJson](const FString& Dir, float CellSizeM)
		{
			return WriteJson(FPaths::Combine(Dir, TEXT("sols_0_0.json")),
				FString::Printf(TEXT(R"({"cell":[0,0],"cellSizeM":%.1f,"origin":[0,0],)"), CellSizeM) +
				TEXT(R"("curbs":[[[20,47],[80,47]]],)")
				TEXT(R"("grassEdges":[[[20,80],[60,80]]],)")
				TEXT(R"("crossings":[{"p":[50,50],"d":[1,0],"halfW":3}],)")
				TEXT(R"("axial":[[30,50,33,50]]})"));
		};

		It("sol peint : dalle masquee, ruban de chaussee supprime, pont conserve",
			[this, MaskedProfile, FreshMaskDir, MakeCellMaterial, WriteCity, WriteMask, SlotPolys]()
		{
			const FString Dir = FreshMaskDir();
			WriteMask(Dir, 100.f);
			MakeCellMaterial(0, 0);
			const FCityStreamedSummary S = UCityImportTools::ImportCityStreamed(
				WriteCity(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, MaskedProfile());

			TestEqual(TEXT("Une cellule peinte"), S.MaskedCells, 1);
			TestEqual(TEXT("Le ruban de chaussee au sol est supprime"), S.GroundRibbonsSkipped, 1);
			TestEqual(TEXT("Le pont garde son ruban"), S.BridgeRibbons, 1);
			TestEqual(TEXT("Un seul ruban genere : le pont"), S.Roads, 1);

			UStaticMesh* Slab = LoadTestMesh(TEXT("SM_Slab_0_0"));
			if (TestNotNull(TEXT("SM_Slab_0_0 genere"), Slab))
			{
				TestTrue(TEXT("La dalle porte le slot ground_masked"),
					SlotPolys(Slab, TEXT("ground_masked")) > 0);
				TestEqual(TEXT("La dalle n'a plus le revetement de dalle simple"),
					SlotPolys(Slab, TEXT("dirty_sidewalk_tiles_ugxjcdpn")), 0);
			}

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				TestEqual(TEXT("Un quad de chaussee, celui du pont"),
					SlotPolys(Ground, TEXT("fine_road_viciaalew")), 1);
				TestEqual(TEXT("Aucun quad d'asphalte au sol"),
					SlotPolys(Ground, TEXT("asphalt_road_tiggcjdo")), 0);
			}
			// Le TABLIER du pont garde ses propres bordures et ses rives : c'est un
			// ruban complet, pas un morceau de sol peint. 4 quads (2 cotes x face +
			// chant) s'ajoutent donc aux 3 quads de la bordure du masque.
			TestEqual(TEXT("4 quads de bordure du tablier + 3 du masque"), S.CurbQuads, 7);
			TestEqual(TEXT("Rives du tablier"),
				SlotPolys(LoadTestMesh(TEXT("SM_Ground_0_0")),
					TEXT("dirty_sidewalk_tiles_ugxjcdpn")), 2);
		});

		It("relief : 3 quads de bordure tournes vers la chaussee, passage et tiret sous le chant",
			[this, MaskedProfile, FreshMaskDir, MakeCellMaterial, WriteCityNoBridge, WriteMask,
			 SlotPolys, SlotZBounds]()
		{
			const FString Dir = FreshMaskDir();
			WriteMask(Dir, 100.f);
			MakeCellMaterial(0, 0);
			// Ville SANS pont : tout le relief du mesh vient du masque, les comptes
			// et les Z se lisent donc sans melange avec un tablier.
			const FCityStreamedSummary S = UCityImportTools::ImportCityStreamed(
				WriteCityNoBridge(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, MaskedProfile());

			TestEqual(TEXT("Aucun ruban : tout est peint"), S.Roads, 0);
			TestEqual(TEXT("3 quads pour un segment de bordure"), S.CurbQuads, 3);
			TestEqual(TEXT("Un passage pieton"), S.Crossings, 1);
			TestEqual(TEXT("Un tiret axial"), S.AxialDashes, 1);

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			TestEqual(TEXT("Bordure : 3 quads sur le mesh"), SlotPolys(Ground, TEXT("curb")), 3);
			TestEqual(TEXT("Passage : 1 quad"),
				SlotPolys(Ground, TEXT("pedestrian_crossing_lines_veggecd")), 1);
			TestEqual(TEXT("Tiret : 1 quad"), SlotPolys(Ground, TEXT("marking")), 1);

			float CurbMin = 0.f, CurbMax = 0.f, CrossMin = 0.f, CrossMax = 0.f;
			float DashMin = 0.f, DashMax = 0.f;
			if (SlotZBounds(Ground, TEXT("curb"), CurbMin, CurbMax) &&
				SlotZBounds(Ground, TEXT("pedestrian_crossing_lines_veggecd"), CrossMin, CrossMax) &&
				SlotZBounds(Ground, TEXT("marking"), DashMin, DashMax))
			{
				TestTrue(FString::Printf(TEXT("Bordure : chant a 12 cm (%.2f)"), CurbMax),
					FMath::IsNearlyEqual(CurbMax, 12.f, 0.05f));
				TestTrue(FString::Printf(TEXT("Bordure : pied enterre a -10 cm (%.2f)"), CurbMin),
					FMath::IsNearlyEqual(CurbMin, -10.f, 0.05f));
				TestTrue(FString::Printf(TEXT("Passage a 4 cm (%.2f)"), CrossMax),
					FMath::IsNearlyEqual(CrossMax, 4.f, 0.05f));
				TestTrue(FString::Printf(TEXT("Tiret a 6 cm, sous le chant (%.2f)"), DashMax),
					FMath::IsNearlyEqual(DashMax, 6.f, 0.05f) && DashMax < CurbMax);
			}

			// ORIENTATION : la polyligne va vers +X avec la chaussee A GAUCHE, donc
			// vers +Y. Une face verticale doit regarder +Y (la chaussee), l'autre -Y.
			if (FMeshDescription* Desc = Ground->GetMeshDescription(0))
			{
				FStaticMeshAttributes Attr(*Desc);
				TPolygonGroupAttributesRef<FName> Names = Attr.GetPolygonGroupMaterialSlotNames();
				TVertexInstanceAttributesRef<FVector3f> Normals = Attr.GetVertexInstanceNormals();
				float TowardRoad = 0.f, TowardWalk = 0.f;
				int32 Vertical = 0;
				for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
				{
					if (Names[G] != FName(TEXT("curb")))
					{
						continue;
					}
					for (const FPolygonID P : Desc->GetPolygonGroupPolygons(G))
					{
						const FVector3f N = Normals[Desc->GetPolygonVertexInstances(P)[0]];
						if (FMath::Abs(N.Z) > 0.5f)
						{
							continue; // le chant, horizontal
						}
						++Vertical;
						TowardRoad = FMath::Max(TowardRoad, N.Y);
						TowardWalk = FMath::Min(TowardWalk, N.Y);
					}
				}
				TestEqual(TEXT("Deux faces verticales"), Vertical, 2);
				TestTrue(FString::Printf(TEXT("Une face regarde la chaussee, +Y (%.2f)"), TowardRoad),
					TowardRoad > 0.9f);
				TestTrue(FString::Printf(TEXT("L'autre regarde le trottoir, -Y (%.2f)"), TowardWalk),
					TowardWalk < -0.9f);
			}
		});

		// -------------------------------------------------- FINITION_SOL V3
		It("bordurette d'herbe : 3 quads de profil REDUIT, comptes a part, meme materiau",
			[this, MaskedProfile, FreshMaskDir, MakeCellMaterial, WriteCityNoBridge, WriteMask,
			 WriteMaskHerbe, SlotPolys, SlotZBounds]()
		{
			// 1. Masque SANS grassEdges : aucune bordurette (retro-compatibilite).
			const FString Dir = FreshMaskDir();
			WriteMask(Dir, 100.f);
			MakeCellMaterial(0, 0);
			const FCityStreamedSummary S0 = UCityImportTools::ImportCityStreamed(
				WriteCityNoBridge(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, MaskedProfile());
			TestEqual(TEXT("Masque sans grassEdges : aucune bordurette"), S0.GrassCurbQuads, 0);
			TestEqual(TEXT("Masque sans grassEdges : la bordure de chaussee est intacte"),
				S0.CurbQuads, 3);

			// 2. Le MEME masque avec un segment `grassEdges` de 40 m.
			const FString Dir2 = FreshMaskDir();
			WriteMaskHerbe(Dir2, 100.f);
			MakeCellMaterial(0, 0);
			const FCityStreamedSummary S = UCityImportTools::ImportCityStreamed(
				WriteCityNoBridge(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, MaskedProfile());

			TestEqual(TEXT("3 quads pour un segment de bordurette"), S.GrassCurbQuads, 3);
			TestEqual(TEXT("La bordure de CHAUSSEE n'a pas bouge (compteur separe)"),
				S.CurbQuads, 3);

			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			if (!TestNotNull(TEXT("SM_Ground_0_0 genere"), Ground))
			{
				return;
			}
			// MEME materiau de bordure : les deux pierres partagent le slot `curb`.
			TestEqual(TEXT("Meme materiau : 3 + 3 quads dans le slot curb"),
				SlotPolys(Ground, TEXT("curb")), 6);

			// PROFIL REDUIT : le chant de la bordurette monte a 7 cm, celui de la
			// bordure de chaussee a 12 — donc le Z max du slot reste 12 et le Z min
			// reste le pied enterre a -10 cm, commun aux deux.
			float ZMin = 0.f, ZMax = 0.f;
			if (SlotZBounds(Ground, TEXT("curb"), ZMin, ZMax))
			{
				TestTrue(FString::Printf(TEXT("Chant le plus haut : la chaussee, 12 cm (%.2f)"), ZMax),
					FMath::IsNearlyEqual(ZMax, 12.f, 0.05f));
				TestTrue(FString::Printf(TEXT("Pied enterre a -10 cm (%.2f)"), ZMin),
					FMath::IsNearlyEqual(ZMin, -10.f, 0.05f));
			}
			// Le profil reduit se lit sur les sommets de la bordurette elle-meme :
			// aucune face de la pierre d'herbe ne monte au-dessus de 7 cm.
			if (FMeshDescription* Desc = Ground->GetMeshDescription(0))
			{
				FStaticMeshAttributes Attr(*Desc);
				TPolygonGroupAttributesRef<FName> Names = Attr.GetPolygonGroupMaterialSlotNames();
				TVertexAttributesRef<FVector3f> Pos = Attr.GetVertexPositions();
				float HautHerbe = -1e9f;
				int32 NHerbe = 0;
				for (const FPolygonGroupID G : Desc->PolygonGroups().GetElementIDs())
				{
					if (Names[G] != FName(TEXT("curb")))
					{
						continue;
					}
					for (const FPolygonID P : Desc->GetPolygonGroupPolygons(G))
					{
						bool bHerbe = true;
						float Haut = -1e9f;
						for (const FVertexInstanceID VI : Desc->GetPolygonVertexInstances(P))
						{
							const FVector3f V = Pos[Desc->GetVertexInstanceVertex(VI)];
							// La bordurette est a y = 8 000 cm, la bordure a y = 4 700.
							bHerbe = bHerbe && V.Y > 7000.f;
							Haut = FMath::Max(Haut, V.Z);
						}
						if (bHerbe)
						{
							++NHerbe;
							HautHerbe = FMath::Max(HautHerbe, Haut);
						}
					}
				}
				TestEqual(TEXT("3 polygones appartiennent a la bordurette"), NHerbe, 3);
				TestTrue(FString::Printf(TEXT("Profil REDUIT : la bordurette culmine a 7 cm (%.2f)"),
					HautHerbe), FMath::IsNearlyEqual(HautHerbe, 7.f, 0.05f));
			}
		});

		It("aucun masque cuit : la chaussee reste un ruban (on n'efface jamais sans remplacer)",
			[this, MaskedProfile, FreshMaskDir, MakeCellMaterial, WriteCity, SlotPolys]()
		{
			FreshMaskDir();
			MakeCellMaterial(0, 0);
			AddExpectedMessagePlain(TEXT("aucun masque"), ELogVerbosity::Warning,
				EAutomationExpectedMessageFlags::Contains);
			const FCityStreamedSummary S = UCityImportTools::ImportCityStreamed(
				WriteCity(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, MaskedProfile());
			TestEqual(TEXT("Aucune cellule peinte"), S.MaskedCells, 0);
			TestEqual(TEXT("Les deux rubans sont generes"), S.Roads, 2);
			TestEqual(TEXT("Aucun ruban supprime"), S.GroundRibbonsSkipped, 0);
			TestEqual(TEXT("Aucun tiret"), S.AxialDashes, 0);
			UStaticMesh* Slab = LoadTestMesh(TEXT("SM_Slab_0_0"));
			TestTrue(TEXT("La dalle garde son revetement simple"),
				SlotPolys(Slab, TEXT("dirty_sidewalk_tiles_ugxjcdpn")) > 0);
		});

		It("instance de cellule absente : bascule ANNULEE, la voirie n'est pas perdue",
			[this, MaskedProfile, FreshMaskDir, WriteCity, WriteMask]()
		{
			const FString Dir = FreshMaskDir();
			WriteMask(Dir, 100.f);
			// Cellule 1,0 : masque cuit, mais aucune instance de materiau fabriquee.
			// Le basculement est GLOBAL : une seule cellule orpheline l'annule.
			FFileHelper::SaveStringToFile(
				TEXT(R"({"cell":[1,0],"cellSizeM":100.0,"origin":[100,0],"curbs":[],"crossings":[],"axial":[]})"),
				*FPaths::Combine(Dir, TEXT("sols_1_0.json")));
			AddExpectedMessagePlain(TEXT("bascule ANNULEE"), ELogVerbosity::Warning,
				EAutomationExpectedMessageFlags::Contains);
			const FCityStreamedSummary S = UCityImportTools::ImportCityStreamed(
				WriteCity(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, MaskedProfile());
			TestEqual(TEXT("Aucune cellule peinte"), S.MaskedCells, 0);
			TestEqual(TEXT("Les deux rubans survivent"), S.Roads, 2);
			TestTrue(TEXT("Bordures de RUBAN, pas de masque"), S.CurbQuads > 0);
			TestEqual(TEXT("Aucun tiret axial"), S.AxialDashes, 0);
		});

		It("masque cuit pour une autre taille de cellule : refuse plutot que peindre a cote",
			[this, MaskedProfile, FreshMaskDir, MakeCellMaterial, WriteCity, WriteMask]()
		{
			const FString Dir = FreshMaskDir();
			WriteMask(Dir, 500.f);   // cuit a 500 m, import a 100 m
			MakeCellMaterial(0, 0);
			AddExpectedError(TEXT("was baked for"), EAutomationExpectedErrorFlags::Contains);
			AddExpectedMessagePlain(TEXT("aucun masque"), ELogVerbosity::Warning,
				EAutomationExpectedMessageFlags::Contains);
			const FCityStreamedSummary S = UCityImportTools::ImportCityStreamed(
				WriteCity(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, MaskedProfile());
			TestEqual(TEXT("Aucune cellule peinte"), S.MaskedCells, 0);
			TestEqual(TEXT("Les deux rubans survivent"), S.Roads, 2);
		});

		It("golden path : sans bMaskedGround, rien ne change (non-regression)",
			[this, FreshMaskDir, MakeCellMaterial, WriteCity, WriteMask, SlotPolys]()
		{
			const FString Dir = FreshMaskDir();
			WriteMask(Dir, 100.f);
			MakeCellMaterial(0, 0);
			// Meme masque sur le disque, meme instance de materiau — mais le profil ne
			// demande PAS la maquette : le generateur ne doit meme pas aller regarder.
			FCityGenProfile P;
			P.bWindowReveals = true;
			P.bSplitWallGlass = true;
			P.bNanite = true;
			P.bPBRMaterials = true;
			P.bSurfaceMaterials = true;
			const FCityStreamedSummary S = UCityImportTools::ImportCityStreamed(
				WriteCity(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, P);
			TestEqual(TEXT("Aucune cellule peinte"), S.MaskedCells, 0);
			TestEqual(TEXT("Les deux rubans sont generes"), S.Roads, 2);
			TestEqual(TEXT("Aucun tiret axial"), S.AxialDashes, 0);
			UStaticMesh* Ground = LoadTestMesh(TEXT("SM_Ground_0_0"));
			TestEqual(TEXT("Aucun quad de marquage"), SlotPolys(Ground, TEXT("marking")), 0);

			// Et le profil MOBILE : bMaskedGround exige les revetements, il ne bascule
			// donc jamais — le golden path mobile ne peut pas etre atteint par erreur.
			FCityGenProfile Mobile;
			Mobile.bMaskedGround = true;
			Mobile.GroundMasksPath = Dir;
			Mobile.GroundMasksAssetFolder = TEXT("/Game/Dev/Test/Ground");
			const FCityStreamedSummary M = UCityImportTools::ImportCityStreamed(
				WriteCity(), FString(), TEXT("/Game/Dev/Test/City"), TEXT("/Game/Dev/Test/Blocks"),
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector, Mobile);
			TestEqual(TEXT("Mobile : aucune cellule peinte"), M.MaskedCells, 0);
			TestEqual(TEXT("Mobile : les deux rubans sont generes"), M.Roads, 2);
		});
	});
}
