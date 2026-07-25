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

	/** Streaming sublevels created or refilled. */
	UPROPERTY() int32 StreamingBlocks = 0;
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
};
