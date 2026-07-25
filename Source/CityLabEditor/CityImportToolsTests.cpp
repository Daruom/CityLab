#include "CityImportTools.h"
#include "TerrainSampler.h"

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformFileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"

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

			// Le ruban est construit dans la cellule du premier point ; tablier =
			// interpolation lineaire entre les culees + empilement 55 (index 0).
			const int32 CX = FMath::FloorToInt(AXm * 100.f / 10000.f);
			float MinZ = 0.f, MaxZ = 0.f;
			UStaticMesh* Road = LoadTestMesh(FString::Printf(TEXT("SM_Ground_%d_0"), CX));
			if (!TestTrue(TEXT("Ruban du pont lisible"), GetMeshZBounds(Road, MinZ, MaxZ)))
			{
				return;
			}
			TestTrue(FString::Printf(TEXT("Tablier %.1f >= culee basse %.1f"), MinZ, FMath::Min(ZA, ZB) + 55.f - 1.f),
				MinZ >= FMath::Min(ZA, ZB) + 55.f - 1.f);
			TestTrue(FString::Printf(TEXT("Tablier %.1f <= culee haute %.1f"), MaxZ, FMath::Max(ZA, ZB) + 55.f + 1.f),
				MaxZ <= FMath::Max(ZA, ZB) + 55.f + 1.f);
			// Chaque sommet du ruban (X = abscisse re-echantillonnee, la route est
			// alignee sur X) doit etre sur la DROITE entre culees + empilement 55 :
			// un drapage MNT donnerait >= BestDev (>= 3 m) d'ecart au point critique.
			FMeshDescription* RoadDesc = Road->GetMeshDescription(0);
			TVertexAttributesRef<FVector3f> Positions = FStaticMeshAttributes(*RoadDesc).GetVertexPositions();
			float MaxErr = 0.f;
			for (const FVertexID V : RoadDesc->Vertices().GetElementIDs())
			{
				const float T = (Positions[V].X - AXm * 100.f) / ((BXm - AXm) * 100.f);
				const float Expected = FMath::Lerp(ZA, ZB, T) + 55.f;
				MaxErr = FMath::Max(MaxErr, FMath::Abs(Positions[V].Z - Expected));
			}
			TestTrue(FString::Printf(TEXT("Tablier interpole lineairement (ecart max %.2f cm ; drape = %.0f cm)"),
				MaxErr, BestDev), MaxErr <= 1.f);
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
}
