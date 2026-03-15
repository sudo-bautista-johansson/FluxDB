// VeldraDB.Build.cs
using UnrealBuildTool;
using System.IO;

public class VeldraDB : ModuleRules
{
    public VeldraDB(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

        string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty", "VeldraDB");
        string IncPath        = Path.Combine(ThirdPartyPath, "Include");
        string LibPath        = Path.Combine(ThirdPartyPath, "Win64");

        PublicIncludePaths.Add(IncPath);

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string ImportLib = Path.Combine(LibPath, "veldra.lib");
            if (File.Exists(ImportLib))
                PublicAdditionalLibraries.Add(ImportLib);

            RuntimeDependencies.Add(
                "$(BinaryOutputDir)/veldra.dll",
                Path.Combine(LibPath, "veldra.dll")
            );
            PublicDelayLoadDLLs.Add("veldra.dll");
        }
    }
}
