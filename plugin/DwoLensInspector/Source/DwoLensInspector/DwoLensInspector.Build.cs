using UnrealBuildTool;

public class DwoLensInspector : ModuleRules
{
	public DwoLensInspector(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"BlueprintGraph",
			"Kismet",
			"AssetRegistry",
			"Projects",
			"Json"
		});
	}
}
