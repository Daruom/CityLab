#include "BuildingTools.h"

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"

BEGIN_DEFINE_SPEC(
	FBuildingToolsSpec,
	"CityLab.BuildingTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FBuildingToolsSpec)

void FBuildingToolsSpec::Define()
{
	Describe("GenerateBuilding", [this]()
	{
		It("builds a mesh with the two material slots and real geometry", [this]()
		{
			FBuildingSpec Spec;
			UStaticMesh* Mesh = UBuildingTools::GenerateBuilding(
				TEXT("/Game/Dev/Test/SM_TestBuilding"), Spec, false, FVector::ZeroVector, 0.f);
			TestNotNull(TEXT("Mesh created"), Mesh);
			if (Mesh)
			{
				TestEqual(TEXT("Material slots"), Mesh->GetStaticMaterials().Num(), 2);
				TestTrue(TEXT("Has vertices"), Mesh->GetNumVertices(0) > 0);
			}
		});

		It("regenerates in place when the asset already exists", [this]()
		{
			FBuildingSpec Spec;
			UStaticMesh* First = UBuildingTools::GenerateBuilding(
				TEXT("/Game/Dev/Test/SM_TestRegen"), Spec, false, FVector::ZeroVector, 0.f);
			Spec.Floors = 8;
			UStaticMesh* Second = UBuildingTools::GenerateBuilding(
				TEXT("/Game/Dev/Test/SM_TestRegen"), Spec, false, FVector::ZeroVector, 0.f);
			TestNotNull(TEXT("First mesh"), First);
			TestEqual(TEXT("Same asset object"), First, Second);
		});

		It("raises on a non-package path", [this]()
		{
			AddExpectedError(TEXT("AssetPath must be a package path"), EAutomationExpectedErrorFlags::Contains);
			FBuildingSpec Spec;
			UBuildingTools::GenerateBuilding(TEXT("not-a-path"), Spec, false, FVector::ZeroVector, 0.f);
		});

		It("raises when Floors is below 1", [this]()
		{
			AddExpectedError(TEXT("Floors must be at least 1"), EAutomationExpectedErrorFlags::Contains);
			FBuildingSpec Spec;
			Spec.Floors = 0;
			UBuildingTools::GenerateBuilding(TEXT("/Game/Dev/Test/SM_TestBad"), Spec, false, FVector::ZeroVector, 0.f);
		});

		It("raises when the footprint cannot fit the corner piers", [this]()
		{
			AddExpectedError(TEXT("Footprint too small"), EAutomationExpectedErrorFlags::Contains);
			FBuildingSpec Spec;
			Spec.WidthM = 4.f;
			Spec.CornerPierM = 3.f;
			UBuildingTools::GenerateBuilding(TEXT("/Game/Dev/Test/SM_TestBad"), Spec, false, FVector::ZeroVector, 0.f);
		});

		It("raises when a named material is missing", [this]()
		{
			AddExpectedError(TEXT("not found"), EAutomationExpectedErrorFlags::Contains);
			FBuildingSpec Spec;
			Spec.WallMaterialPath = TEXT("/Game/DoesNotExist/M_Nope.M_Nope");
			UBuildingTools::GenerateBuilding(TEXT("/Game/Dev/Test/SM_TestBad"), Spec, false, FVector::ZeroVector, 0.f);
		});
	});

	Describe("NewLevel", [this]()
	{
		It("raises on a non-package path", [this]()
		{
			AddExpectedError(TEXT("AssetPath must be a package path"), EAutomationExpectedErrorFlags::Contains);
			UBuildingTools::NewLevel(TEXT("not-a-path"));
		});
	});

	Describe("ExecConsoleCommand", [this]()
	{
		It("raises on an empty command", [this]()
		{
			AddExpectedError(TEXT("Command must not be empty"), EAutomationExpectedErrorFlags::Contains);
			UBuildingTools::ExecConsoleCommand(TEXT("  "));
		});
	});
}
