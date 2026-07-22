using UnrealBuildTool;
using System.Collections.Generic;

public class CityLabTarget : TargetRules
{
	public CityLabTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CityLab");
	}
}
