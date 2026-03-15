// FluxDB.Build.cs
using UnrealBuildTool;
using System.IO;

public class FluxDB : ModuleRules
{
    public FluxDB(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

        string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty", "FluxDB");
        string IncPath        = Path.Combine(ThirdPartyPath, "Include");
        string LibPath        = Path.Combine(ThirdPartyPath, "Win64");

        PublicIncludePaths.Add(IncPath);

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string ImportLib = Path.Combine(LibPath, "flux.lib");
            if (File.Exists(ImportLib))
                PublicAdditionalLibraries.Add(ImportLib);

            RuntimeDependencies.Add(
                "$(BinaryOutputDir)/flux.dll",
                Path.Combine(LibPath, "flux.dll")
            );
            PublicDelayLoadDLLs.Add("flux.dll");
        }
    }
}
