using UnrealBuildTool;

public class DwoStateMonitor : ModuleRules
{
    public DwoStateMonitor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "UMG",
            "Slate",
            "SlateCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Json",
            "ApplicationCore",
            "InputCore",
            "Projects",
            "Sockets"
        });
    }
}
