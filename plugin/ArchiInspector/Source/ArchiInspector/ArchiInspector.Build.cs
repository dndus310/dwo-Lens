using UnrealBuildTool;

public class ArchiInspector : ModuleRules
{
	public ArchiInspector(ReadOnlyTargetRules Target) : base(Target)
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
