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
}
