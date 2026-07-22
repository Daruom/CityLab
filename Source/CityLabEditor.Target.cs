using UnrealBuildTool;
using System.Collections.Generic;

public class CityLabEditorTarget : TargetRules
{
	public CityLabEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CityLab");
		ExtraModuleNames.Add("CityLabEditor");
	}
}
