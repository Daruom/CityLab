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
			"StaticMeshDescription"
		});
	}
}
