// DwoCanvas - runtime widgets (packaged with the game)

using UnrealBuildTool;

public class DwoCanvasRuntime : ModuleRules
{
	public DwoCanvasRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"Slate",
			"SlateCore"
		});
	}
}
