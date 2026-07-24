#include "CityImportTools.h"

#include "Editor.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

BEGIN_DEFINE_SPEC(
	FCityImportToolsSpec,
	"CityLab.CityImportTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FCityImportToolsSpec)

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
				FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector);
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
				TEXT("/Game/Dev/Test/Blocks"), FString(), FString(), 100.f, 200.f, 400.f, FVector::ZeroVector);
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
				Path, TEXT("/Game/Dev/Test/City"), FString(), 100.f, FVector::ZeroVector);
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
				FString(), 100.f, FVector::ZeroVector);
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
				Path, TEXT("/Game/Dev/Test/City"), FString(), FVector::ZeroVector);
			TestEqual(TEXT("Marqueurs places"), Placed, 2);
		});

		It("raises when the file does not exist", [this]()
		{
			AddExpectedError(TEXT("Cannot read markers file"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::ImportCityMarkers(TEXT("Z:/nope.json"), TEXT("/Game/Dev/Test/City"),
				FString(), FVector::ZeroVector);
		});

		It("raises when the markers array is missing", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/mini_markers2.json"));
			FFileHelper::SaveStringToFile(TEXT("{}"), *Path);
			AddExpectedError(TEXT("no 'markers' array"), EAutomationExpectedErrorFlags::Contains);
			UCityImportTools::ImportCityMarkers(Path, TEXT("/Game/Dev/Test/City"), FString(), FVector::ZeroVector);
		});
	});
}
