// DwoCanvas - editor importer (never packaged)

using UnrealBuildTool;

public class DwoCanvasEditor : ModuleRules
{
	public DwoCanvasEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"UMG"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"DwoCanvasRuntime",
			"UMGEditor",
			"UnrealEd",
			"Kismet",
			"AssetTools",
			"AssetRegistry",
			"ToolMenus",
			"Slate",
			"SlateCore",
			"Json",
			"ImageCore",
			"RenderCore",
			"RHI",
			"DesktopPlatform",
			"Projects"
		});
	}
}
