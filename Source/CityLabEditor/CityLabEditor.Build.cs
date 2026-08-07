using UnrealBuildTool;

public class CityLabEditor : ModuleRules
{
	public CityLabEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"LevelEditor",
			"ToolsetRegistry",
			"AssetRegistry",
			"ImageWrapper",
			"Json",
			"MeshDescription",
			"StaticMeshDescription",
			// Lecture des sommets de rendu (empattement des arbres -> taille des fosses).
			"RenderCore",
			// E2-1 (sol du plan) : triangulation contrainte AVEC TROUS (`FDelaunay2`).
			// 145 parcelles de SOL du seul district de test ont des trous (voirie 27,
			// organique 110, zone 8) ; l'ear clipping maison n'en gere aucun.
			"GeometryCore"
		});
	}
}
