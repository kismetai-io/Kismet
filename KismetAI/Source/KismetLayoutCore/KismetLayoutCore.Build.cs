// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

// Engine-agnostic graph-layout core: dummy-lane insertion + Brandes-Köpf
// coordinate assignment. Depends on Core ONLY (no Engine/UnrealEd/BlueprintGraph)
// so it stays unit-testable without an editor context and cannot reach for an
// editor type by construction.
public class KismetLayoutCore : ModuleRules
{
	public KismetLayoutCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});
	}
}
