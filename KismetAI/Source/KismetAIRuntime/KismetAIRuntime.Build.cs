// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using UnrealBuildTool;

// Provider-neutral agent runtime: the turn loop, model client, tool contracts,
// context management, session journal and telemetry.
//
// Depends on Core/CoreUObject/Json/HTTP/Projects ONLY — no Engine, UnrealEd,
// Slate, BlueprintGraph or HTTPServer. That constraint is the point of the
// module: the runtime must not be able to reach an editor type, and a link
// error is a better guard than a code-review convention. Same reasoning as
// KismetLayoutCore, which is Core-only for the same reason.
//
// Editor-side adapters (tool handlers, panels, HTTP routes) live in KismetAI,
// which depends on this module. Never the reverse.
public class KismetAIRuntime : ModuleRules
{
	public KismetAIRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Json"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTP",      // telemetry reporting
			"Projects"   // plugin dir resolution for Config/
		});

		// The agent and the embedded-config loader moved into
		// this module, so the build-time defines they read have to be set
		// here too. These are the same helpers KismetAI.Build.cs uses, keyed off
		// the plugin directory — which is identical for both modules.
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		ConfigureEmbeddedConfig(Target);
		ConfigureDevLogging(Target);
	}

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
