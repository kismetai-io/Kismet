// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using UnrealBuildTool;

public class KismetAI : ModuleRules
{
	public KismetAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"HTTPServer",
			"KismetAIRuntime"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"KismetLayoutCore",
			"Engine",
			"UnrealEd",
			"BlueprintGraph",
			"KismetCompiler",
			"Kismet",
			"GraphEditor",
			"MaterialEditor",
			"UMG",
			"UMGEditor",
			"AnimGraph",
			"AnimGraphRuntime",
			"RenderCore",
			"RHI",
			"Json",
			"JsonUtilities",
			"HTTP",
			"SSL",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"InputCore",
			"Projects",
			"WebBrowser",
			"ApplicationCore",
			"AssetRegistry",
			"AssetTools",
			"FileUtilities",
			"DeveloperSettings",
			"SourceCodeAccess",
			"Sockets"   // FInternetAddr, for the /mcp loopback peer check
		});

		ConfigureEmbeddedConfig(Target);
		ConfigureDevLogging(Target);
		ConfigureUpdaterEndpoint(Target);
	}

	// KISMETAI_MANIFEST_URL is baked into the binary so shipped plugins
	// can't be redirected at runtime. Shipping/Test always points at prod
	// regardless of the env var. Other configs honor KISMETAI_CHANNEL=dev
	// to point at the dev Supabase project for end-to-end testing.
	//
	// Same UBT gotcha as KISMETAI_DEV_LOGGING: env var changes don't
	// invalidate the build cache. After flipping KISMETAI_CHANNEL, force
	// a rebuild of the module (touch KismetAIUpdater.cpp or wipe
	// Intermediate/) or the old URL stays compiled in.
	private void ConfigureUpdaterEndpoint(ReadOnlyTargetRules Target)
	{
		const string ProdUrl = "https://zsimjwrioguyyythpaud.supabase.co/functions/v1/releases-latest";
		const string DevUrl  = "https://aonkjdwajjldlcdnjyrs.supabase.co/functions/v1/releases-latest";

		bool bAllowOverride =
			Target.Configuration != UnrealTargetConfiguration.Shipping
			&& Target.Configuration != UnrealTargetConfiguration.Test;

		string Channel = "prod";
		if (bAllowOverride)
		{
			string Override = Environment.GetEnvironmentVariable("KISMETAI_CHANNEL");
			if (!string.IsNullOrEmpty(Override))
			{
				string Lower = Override.ToLowerInvariant();
				if (Lower == "dev" || Lower == "prod")
				{
					Channel = Lower;
				}
				else
				{
					throw new BuildException(
						"KISMETAI_CHANNEL must be 'dev' or 'prod' (got '{0}')",
						Override);
				}
			}
		}

		string Url = Channel == "dev" ? DevUrl : ProdUrl;
		PrivateDefinitions.Add(string.Format("KISMETAI_MANIFEST_URL=TEXT(\"{0}\")", Url));
		PrivateDefinitions.Add(string.Format("KISMETAI_RELEASE_CHANNEL=TEXT(\"{0}\")", Channel));
	}

	// Verbose logs can contain conversation content and tool arguments.
	// Development/DebugGame enable them; Shipping/Test omit them at compile time.
	// After changing KISMETAI_DEV_LOGGING, use Scripts/rebuild.sh to make UBT
	// re-evaluate the environment override.
	private void ConfigureDevLogging(ReadOnlyTargetRules Target)
	{
		bool bDevLogging =
			Target.Configuration == UnrealTargetConfiguration.Debug
			|| Target.Configuration == UnrealTargetConfiguration.DebugGame
			|| Target.Configuration == UnrealTargetConfiguration.Development;

		string Override = Environment.GetEnvironmentVariable("KISMETAI_DEV_LOGGING");
		if (!string.IsNullOrEmpty(Override))
		{
			if (Override == "1" || Override.Equals("true", StringComparison.OrdinalIgnoreCase))
			{
				bDevLogging = true;
			}
			else if (Override == "0" || Override.Equals("false", StringComparison.OrdinalIgnoreCase))
			{
				bDevLogging = false;
			}
		}

		PrivateDefinitions.Add("KISMETAI_DEV_LOGGING=" + (bDevLogging ? "1" : "0"));
	}

	// Compile Config/*.{txt,json,html} into the binary for release builds so
	// prompts and UI assets aren't shipped as loose files. Dev builds still
	// read Config/ from disk for fast iteration.
	private void ConfigureEmbeddedConfig(ReadOnlyTargetRules Target)
	{
		string PluginDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string ConfigDir = Path.Combine(PluginDir, "Config");
		string OutDir = Path.Combine(PluginDir, "Intermediate", "Build", "EmbeddedConfig");
		Directory.CreateDirectory(OutDir);

		// Mark the marker file as a build dependency BEFORE branching on
		// bEmbed. UBT tracks file content/presence changes via this list,
		// so flipping the marker (or creating/deleting it) reliably
		// triggers a rebuild — this is what makes the marker-file path
		// gotcha-free, unlike the env-var fallback. The path is always
		// added even when the file does not exist; UBT records the
		// absence and reacts when the file later appears.
		string MarkerPath = Path.Combine(PluginDir, ".embed-config");
		ExternalDependencies.Add(MarkerPath);

		bool bEmbed = ShouldEmbed(Target, MarkerPath);

		if (!bEmbed)
		{
			PrivateDefinitions.Add("KISMETAI_EMBED_CONFIG=0");
			PrivateIncludePaths.Add(OutDir);
			return;
		}

		string Script = Path.Combine(PluginDir, "Scripts", "embed_config.py");
		if (!File.Exists(Script))
		{
			throw new BuildException("KismetAI: embed_config.py missing at {0}", Script);
		}

		RunCodegen(Script, ConfigDir, OutDir);

		PrivateDefinitions.Add("KISMETAI_EMBED_CONFIG=1");
		PrivateIncludePaths.Add(OutDir);

		// Re-run UBT configure whenever a Config file changes so the embedded
		// payload stays in sync with disk during embedded dev builds.
		foreach (string File in Directory.GetFiles(ConfigDir))
		{
			string Ext = Path.GetExtension(File).ToLowerInvariant();
			if (Ext == ".txt" || Ext == ".json" || Ext == ".html")
			{
				ExternalDependencies.Add(File);
			}
		}
		ExternalDependencies.Add(Script);
	}

	// Resolution order (highest priority first):
	//   1. Marker file at <PluginDir>/.embed-config containing "1"/"0".
	//      UBT tracks this file via ExternalDependencies, so flipping its
	//      content is observed and rebuilds happen automatically. This is
	//      the recommended path for incremental development.
	//   2. KISMETAI_EMBED_CONFIG environment variable. UBT does NOT track
	//      env vars, so flipping this between builds without cleaning
	//      Intermediate/ silently produces stale binaries. Safe only for
	//      clean-build flows like UAT BuildPlugin (see package.ps1).
	//   3. Build configuration default — Shipping/Test embed; everything
	//      else does not.
	//
	// Use Scripts/set-embed.sh to manage the marker file.
	private static bool ShouldEmbed(ReadOnlyTargetRules Target, string MarkerPath)
	{
		if (File.Exists(MarkerPath))
		{
			string Content = File.ReadAllText(MarkerPath).Trim();
			if (Content == "1" || Content.Equals("true", StringComparison.OrdinalIgnoreCase))
			{
				return true;
			}
			if (Content == "0" || Content.Equals("false", StringComparison.OrdinalIgnoreCase))
			{
				return false;
			}
			// Unrecognized content falls through to env var / default. We do
			// not throw — the marker file is developer-managed and a typo
			// shouldn't break the build, just degrade to the next layer.
		}

		string Override = Environment.GetEnvironmentVariable("KISMETAI_EMBED_CONFIG");
		if (!string.IsNullOrEmpty(Override))
		{
			if (Override == "1" || Override.Equals("true", StringComparison.OrdinalIgnoreCase))
			{
				return true;
			}
			if (Override == "0" || Override.Equals("false", StringComparison.OrdinalIgnoreCase))
			{
				return false;
			}
		}

		return Target.Configuration == UnrealTargetConfiguration.Shipping
			|| Target.Configuration == UnrealTargetConfiguration.Test;
	}

	private static void RunCodegen(string Script, string ConfigDir, string OutDir)
	{
		string Exe = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "python" : "python3";
		ProcessStartInfo Psi = new ProcessStartInfo
		{
			FileName = Exe,
			Arguments = string.Format("\"{0}\" \"{1}\" \"{2}\"", Script, ConfigDir, OutDir),
			UseShellExecute = false,
			RedirectStandardOutput = true,
			RedirectStandardError = true,
			CreateNoWindow = true,
		};

		using (Process P = Process.Start(Psi))
		{
			string StdOut = P.StandardOutput.ReadToEnd();
			string StdErr = P.StandardError.ReadToEnd();
			P.WaitForExit();
			if (!string.IsNullOrWhiteSpace(StdOut))
			{
				Console.WriteLine(StdOut.TrimEnd());
			}
			if (P.ExitCode != 0)
			{
				throw new BuildException(
					"KismetAI embed_config.py failed (exit {0}): {1}",
					P.ExitCode, StdErr);
			}
		}
	}
}
