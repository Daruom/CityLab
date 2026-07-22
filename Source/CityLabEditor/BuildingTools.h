#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "BuildingTools.generated.h"

class UStaticMesh;

/**
 * Parameters of a procedurally generated building, consumed by GenerateBuilding.
 * All dimensions are in meters. Shading (sun term, ambient occlusion) is baked
 * into vertex colors at generation time for unlit rendering.
 */
USTRUCT(BlueprintType)
struct FBuildingSpec
{
	GENERATED_BODY()

	/** Footprint size along local X. */
	UPROPERTY(meta = (ClampMin = "4.0", ClampMax = "80.0")) float WidthM = 14.0f;

	/** Footprint size along local Y. */
	UPROPERTY(meta = (ClampMin = "4.0", ClampMax = "80.0")) float DepthM = 12.0f;

	/** Total floor count, ground floor included. */
	UPROPERTY(meta = (ClampMin = "1", ClampMax = "40")) int32 Floors = 5;

	/** Height of the upper floors. */
	UPROPERTY(meta = (ClampMin = "2.4", ClampMax = "5.0")) float FloorHeightM = 3.0f;

	/** Height of the ground floor. */
	UPROPERTY(meta = (ClampMin = "2.4", ClampMax = "8.0")) float GroundFloorHeightM = 4.2f;

	/** Target width of one window bay; the actual width adapts so bays fill the facade exactly. */
	UPROPERTY(meta = (ClampMin = "1.2", ClampMax = "8.0")) float BayWidthM = 2.6f;

	/** Window width as a fraction of the bay width. */
	UPROPERTY(meta = (ClampMin = "0.2", ClampMax = "0.9")) float WindowWidthRatio = 0.55f;

	/** Window height as a fraction of the floor height. */
	UPROPERTY(meta = (ClampMin = "0.2", ClampMax = "0.8")) float WindowHeightRatio = 0.5f;

	/** Depth the windows are recessed behind the wall plane. */
	UPROPERTY(meta = (ClampMin = "0.0", ClampMax = "0.6")) float WindowInsetM = 0.22f;

	/** Solid wall width kept at each facade end. */
	UPROPERTY(meta = (ClampMin = "0.0", ClampMax = "3.0")) float CornerPierM = 0.8f;

	/** How far the cornice band protrudes from the wall. Zero removes the cornice. */
	UPROPERTY(meta = (ClampMin = "0.0", ClampMax = "0.6")) float CorniceDepthM = 0.18f;

	/** Cornice band height. */
	UPROPERTY(meta = (ClampMin = "0.05", ClampMax = "0.5")) float CorniceHeightM = 0.35f;

	/** Roof parapet height above the roof slab. Zero removes the parapet. */
	UPROPERTY(meta = (ClampMin = "0.0", ClampMax = "2.0")) float ParapetHeightM = 0.9f;

	/** Roof parapet thickness. */
	UPROPERTY(meta = (ClampMin = "0.05", ClampMax = "0.5")) float ParapetThicknessM = 0.25f;

	/** Sun direction for the baked shading, in the building's local space. Normalized on use. */
	UPROPERTY() FVector SunDirection = FVector(0.4, -0.6, 0.7);

	/** Base luminance of surfaces facing away from the sun, 0-1. */
	UPROPERTY(meta = (ClampMin = "0.0", ClampMax = "1.0")) float AmbientLuminance = 0.62f;

	/** Luminance added on surfaces fully facing the sun, 0-1. */
	UPROPERTY(meta = (ClampMin = "0.0", ClampMax = "1.0")) float SunLuminance = 0.38f;

	/** Darkening multiplier inside window reveals and on glass. */
	UPROPERTY(meta = (ClampMin = "0.1", ClampMax = "1.0")) float RevealShade = 0.6f;

	/** World meters covered by one tile of the wall texture in UV0. */
	UPROPERTY(meta = (ClampMin = "0.5", ClampMax = "32.0")) float WallTexWorldSizeM = 4.0f;

	/** Wall material asset path. Empty uses the engine default surface material. */
	UPROPERTY() FString WallMaterialPath;

	/** Window glass material asset path. Empty uses the engine default surface material. */
	UPROPERTY() FString GlassMaterialPath;
};

/**
 * Procedural building generation for the CityLab experiments: parametric building
 * meshes with facade detail (recessed windows, cornice, parapet) and shading baked
 * into vertex colors, plus the level lifecycle helpers needed to iterate on them.
 */
UCLASS(MinimalAPI)
class UBuildingTools : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Creates a new empty level asset and opens it in the editor.
	 * @param AssetPath Package path for the level, e.g. "/Game/Maps/L_BuildingTest".
	 */
	UFUNCTION(meta = (AICallable), Category = "BuildingTools")
	static void NewLevel(const FString& AssetPath);

	/** Saves the currently loaded level and every other dirty package (generated meshes included). */
	UFUNCTION(meta = (AICallable), Category = "BuildingTools")
	static void SaveLevel();

	/**
	 * Runs a console command in the editor world, as if typed into the editor console.
	 * Output goes to the editor log.
	 * @param Command The command line, e.g. "stat unit".
	 */
	UFUNCTION(meta = (AICallable), Category = "BuildingTools")
	static void ExecConsoleCommand(const FString& Command);

	/**
	 * Builds a parametric building as a static mesh asset. Two material slots:
	 * "Wall" (facade, cornice, parapet, roof) and "Glass" (window panes).
	 * The package is left dirty; call SaveLevel to persist it.
	 * @param AssetPath Package path for the mesh, e.g. "/Game/Dev/SM_Bldg01".
	 * @param Spec Building parameters.
	 * @param bSpawnActor True also places a StaticMeshActor in the current level.
	 * @param Location World position of the center of the building's base.
	 * @param YawDegrees Rotation of the spawned actor around Z.
	 * @return The generated static mesh asset.
	 */
	UFUNCTION(meta = (AICallable), Category = "BuildingTools")
	static UStaticMesh* GenerateBuilding(const FString& AssetPath, const FBuildingSpec& Spec,
		bool bSpawnActor, FVector Location, float YawDegrees);

	/**
	 * Places instances of a building mesh with a HierarchicalInstancedStaticMeshComponent,
	 * the rendering path the game uses. Instances are laid out along the actor's local X.
	 * @param MeshAssetPath Package path of the mesh, e.g. "/Game/Dev/SM_Bldg01".
	 * @param Count Number of instances.
	 * @param SpacingM Distance between consecutive instance origins, meters.
	 * @param Location World position of the first instance.
	 * @param YawDegrees Rotation of the whole row around Z.
	 * @return The actor holding the instances.
	 */
	UFUNCTION(meta = (AICallable), Category = "BuildingTools")
	static AActor* SpawnBuildingInstances(const FString& MeshAssetPath, int32 Count, float SpacingM,
		FVector Location, float YawDegrees);

private:
	static class UWorld* GetEditorWorldChecked();
};
