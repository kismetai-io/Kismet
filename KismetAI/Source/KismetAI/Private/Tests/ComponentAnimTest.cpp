// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Automation tests for the manage_components animation ops: get_animation / set_animation on
// USkeletalMeshComponent templates, covering SCS-added components, inherited C++ components, and
// the skeleton-compatibility guard. Assets (Skeleton, AnimBlueprint, AnimSequence, SkeletalMesh)
// are created in named /Temp packages so the handler's LoadObject(asset_path) resolves them; the
// factories run directly (IAssetTools.CreateAsset needs real /Game packages, unavailable
// headless), and all component edits go through the real HTTP handler via ExecuteHandlerDirect.
//
// A bare USkeleton seeded with one root bone is enough: IsCompatibleForEditor(self) is true for
// the same skeleton, while a second distinct skeleton is incompatible (drives the negative case).

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#include "Animation/Skeleton.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimSequenceFactory.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "ReferenceSkeleton.h"

namespace ComponentAnimTestHelpers
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	static USkeleton* MakeSkeleton(const TCHAR* PkgName, const TCHAR* AssetName)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		USkeleton* Sk = NewObject<USkeleton>(Pkg, FName(AssetName), RF_Public | RF_Standalone);
		{
			FReferenceSkeletonModifier Modifier(Sk);
			Modifier.Add(FMeshBoneInfo(FName(TEXT("Root")), TEXT("Root"), INDEX_NONE), FTransform::Identity);
		}
		return Sk;
	}

	static UAnimBlueprint* MakeAnimBlueprint(const TCHAR* PkgName, const TCHAR* AssetName, USkeleton* Skeleton)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
		Factory->TargetSkeleton = Skeleton;
		UAnimBlueprint* ABP = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
			UAnimBlueprint::StaticClass(), Pkg, FName(AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
		if (ABP)
		{
			FKismetEditorUtilities::CompileBlueprint(ABP);
		}
		return ABP;
	}

	static UAnimSequence* MakeSequence(const TCHAR* PkgName, const TCHAR* AssetName, USkeleton* Skeleton)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
		Factory->TargetSkeleton = Skeleton;
		return Cast<UAnimSequence>(Factory->FactoryCreateNew(
			UAnimSequence::StaticClass(), Pkg, FName(AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	}

	static USkeletalMesh* MakeSkeletalMesh(const TCHAR* PkgName, const TCHAR* AssetName, USkeleton* Skeleton)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Pkg, FName(AssetName), RF_Public | RF_Standalone);
		Mesh->SetSkeleton(Skeleton);
		return Mesh;
	}

	static TSharedPtr<FJsonObject> RunHandler(
		bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
		TSharedPtr<FJsonObject> Body)
	{
		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(Handler, Body);
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static const TSharedPtr<FJsonObject>* GetData(const TSharedPtr<FJsonObject>& Env)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data)) return Data;
		return nullptr;
	}

	static TSharedPtr<FJsonValue> Op(TSharedPtr<FJsonObject> O) { return MakeShared<FJsonValueObject>(O); }

	// Run a batch of manage_components ops, return the data object.
	static TSharedPtr<FJsonObject> RunOps(const FString& BlueprintPath, TArray<TSharedPtr<FJsonValue>> Ops)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), BlueprintPath);
		Body->SetArrayField(TEXT("operations"), Ops);
		TSharedPtr<FJsonObject> Resp = RunHandler(&FKismetAIModule::HandleManageComponents, Body);
		const TSharedPtr<FJsonObject>* Data = GetData(Resp);
		return Data ? *Data : nullptr;
	}

	static TSharedPtr<FJsonObject> FirstResult(const TSharedPtr<FJsonObject>& Data)
	{
		if (!Data) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (Data->TryGetArrayField(TEXT("results"), Results) && Results->Num() > 0)
			return (*Results)[0]->AsObject();
		return nullptr;
	}

	// Convenience builders.
	static TSharedPtr<FJsonObject> OpObj(const FString& Action, const FString& Name)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), Action);
		O->SetStringField(TEXT("name"), Name);
		return O;
	}

	// Set the skeletal mesh on a Blueprint's named SCS component template directly. The
	// editor's SkeletalMeshAsset property is Transient (driven by the setter), so the test
	// fixture seeds the mesh in C++ rather than through generic reflection.
	static bool SetScsMesh(UBlueprint* BP, const FString& CompName, USkeletalMesh* Mesh)
	{
		if (!BP || !BP->SimpleConstructionScript) return false;
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString() == CompName)
			{
				if (USkeletalMeshComponent* Comp = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
				{
					Comp->SetSkeletalMeshAsset(Mesh);
					return Comp->GetSkinnedAsset() == Mesh;
				}
			}
		}
		return false;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// SCS case: add a SkeletalMeshComponent, assign an ABP + mesh, read it back,
// clear it, then set single-node mode with an AnimSequence.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompAnim_SCS, "KismetAI.Components.Anim.SCS", ComponentAnimTestHelpers::Flags)
bool FCompAnim_SCS::RunTest(const FString& Parameters)
{
	using namespace ComponentAnimTestHelpers;

	USkeleton* Sk = MakeSkeleton(TEXT("/Temp/KismetCompAnimScsSk"), TEXT("SK_Scs"));
	UAnimBlueprint* ABP = MakeAnimBlueprint(TEXT("/Temp/KismetCompAnimScsAbp"), TEXT("ABP_Scs"), Sk);
	UAnimSequence* Seq = MakeSequence(TEXT("/Temp/KismetCompAnimScsSeq"), TEXT("A_Scs"), Sk);
	USkeletalMesh* Mesh = MakeSkeletalMesh(TEXT("/Temp/KismetCompAnimScsMesh"), TEXT("SKM_Scs"), Sk);
	TestTrue(TEXT("fixtures created"), Sk && ABP && ABP->GeneratedClass && Seq && Mesh);
	if (!Sk || !ABP || !Seq || !Mesh) return false;

	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetCompAnimScsBP"));
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_CompAnimScs"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	TestTrue(TEXT("blueprint created"), BP != nullptr);
	if (!BP) return false;
	const FString BPPath = BP->GetPathName();

	// Add a skeletal mesh component + give it the mesh (so skeleton-compat can be verified).
	{
		TSharedPtr<FJsonObject> Add = OpObj(TEXT("add_component"), TEXT("MyMesh"));
		Add->SetStringField(TEXT("class"), TEXT("SkeletalMeshComponent"));
		TSharedPtr<FJsonObject> Data = RunOps(BPPath, { Op(Add) });
		TestTrue(TEXT("add component data"), Data.IsValid());
		TestTrue(TEXT("seeded mesh on template"), SetScsMesh(BP, TEXT("MyMesh"), Mesh));
	}

	// Assign the ABP via set_animation (no explicit mode → AnimationBlueprint).
	{
		TSharedPtr<FJsonObject> O = OpObj(TEXT("set_animation"), TEXT("MyMesh"));
		O->SetStringField(TEXT("anim_class"), ABP->GetPathName());
		TSharedPtr<FJsonObject> R = FirstResult(RunOps(BPPath, { Op(O) }));
		TestTrue(TEXT("set_animation success"), R.IsValid() && R->GetBoolField(TEXT("success")));
		if (R.IsValid())
		{
			const TSharedPtr<FJsonObject>* Anim = nullptr;
			TestTrue(TEXT("animation object present"), R->TryGetObjectField(TEXT("animation"), Anim));
			if (Anim)
			{
				TestEqual(TEXT("mode is AnimationBlueprint"), (*Anim)->GetStringField(TEXT("animation_mode")), FString(TEXT("AnimationBlueprint")));
				TestEqual(TEXT("anim_class resolves to source ABP"), (*Anim)->GetStringField(TEXT("anim_class")), ABP->GetPathName());
			}
		}
	}

	// Read it back with get_animation.
	{
		TSharedPtr<FJsonObject> R = FirstResult(RunOps(BPPath, { Op(OpObj(TEXT("get_animation"), TEXT("MyMesh"))) }));
		TestTrue(TEXT("get_animation success"), R.IsValid() && R->GetBoolField(TEXT("success")));
		if (R.IsValid())
		{
			const TSharedPtr<FJsonObject>* Anim = nullptr;
			R->TryGetObjectField(TEXT("animation"), Anim);
			if (Anim)
			{
				TestEqual(TEXT("read anim_class"), (*Anim)->GetStringField(TEXT("anim_class")), ABP->GetPathName());
				TestEqual(TEXT("read skeletal_mesh"), (*Anim)->GetStringField(TEXT("skeletal_mesh")), Mesh->GetPathName());
				TestEqual(TEXT("read skeleton"), (*Anim)->GetStringField(TEXT("skeleton")), Sk->GetPathName());
				TestTrue(TEXT("raw class name kept"), (*Anim)->GetStringField(TEXT("anim_class_raw")).Contains(TEXT("ABP_Scs")));
			}
		}
	}

	// Clear the class, then assign single-node mode with the sequence.
	{
		TSharedPtr<FJsonObject> Clear = OpObj(TEXT("set_animation"), TEXT("MyMesh"));
		Clear->SetStringField(TEXT("anim_class"), TEXT("None"));
		TSharedPtr<FJsonObject> Single = OpObj(TEXT("set_animation"), TEXT("MyMesh"));
		Single->SetStringField(TEXT("anim_to_play"), Seq->GetPathName());
		TSharedPtr<FJsonObject> Data = RunOps(BPPath, { Op(Clear), Op(Single) });
		TSharedPtr<FJsonObject> R = FirstResult(RunOps(BPPath, { Op(OpObj(TEXT("get_animation"), TEXT("MyMesh"))) }));
		TestTrue(TEXT("get after single-node success"), R.IsValid() && R->GetBoolField(TEXT("success")));
		if (R.IsValid())
		{
			const TSharedPtr<FJsonObject>* Anim = nullptr;
			R->TryGetObjectField(TEXT("animation"), Anim);
			if (Anim)
			{
				TestEqual(TEXT("mode is AnimationSingleNode"), (*Anim)->GetStringField(TEXT("animation_mode")), FString(TEXT("AnimationSingleNode")));
				TestEqual(TEXT("anim_to_play set"), (*Anim)->GetStringField(TEXT("anim_to_play")), Seq->GetPathName());
				TestTrue(TEXT("anim_class cleared"), (*Anim)->GetStringField(TEXT("anim_class")).IsEmpty());
			}
		}
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Inherited case: a Blueprint subclass of ASkeletalMeshActor inherits a native C++
// SkeletalMeshComponent. Assign an ABP on it, read back, recompile, re-read
// (persistence through compile).
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompAnim_Inherited, "KismetAI.Components.Anim.Inherited", ComponentAnimTestHelpers::Flags)
bool FCompAnim_Inherited::RunTest(const FString& Parameters)
{
	using namespace ComponentAnimTestHelpers;

	USkeleton* Sk = MakeSkeleton(TEXT("/Temp/KismetCompAnimInhSk"), TEXT("SK_Inh"));
	UAnimBlueprint* ABP = MakeAnimBlueprint(TEXT("/Temp/KismetCompAnimInhAbp"), TEXT("ABP_Inh"), Sk);
	TestTrue(TEXT("fixtures created"), Sk && ABP && ABP->GeneratedClass);
	if (!Sk || !ABP) return false;

	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetCompAnimInhBP"));
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		ASkeletalMeshActor::StaticClass(), Pkg, FName("BP_CompAnimInh"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	TestTrue(TEXT("blueprint created"), BP != nullptr);
	if (!BP) return false;
	const FString BPPath = BP->GetPathName();

	// The inherited component is addressed by its C++ property name. No mesh is set on it,
	// so set_animation should succeed with a warning rather than fail skeleton-compat.
	{
		TSharedPtr<FJsonObject> O = OpObj(TEXT("set_animation"), TEXT("SkeletalMeshComponent"));
		O->SetStringField(TEXT("anim_class"), ABP->GetPathName());
		TSharedPtr<FJsonObject> R = FirstResult(RunOps(BPPath, { Op(O) }));
		TestTrue(TEXT("set on inherited success"), R.IsValid() && R->GetBoolField(TEXT("success")));
		if (R.IsValid())
		{
			TestEqual(TEXT("template_source is native"), R->GetStringField(TEXT("template_source")), FString(TEXT("native")));
			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			TestTrue(TEXT("no-mesh warning present"), R->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings->Num() > 0);
		}
	}

	// Read back.
	{
		TSharedPtr<FJsonObject> R = FirstResult(RunOps(BPPath, { Op(OpObj(TEXT("get_animation"), TEXT("SkeletalMeshComponent"))) }));
		TestTrue(TEXT("get on inherited success"), R.IsValid() && R->GetBoolField(TEXT("success")));
		if (R.IsValid())
		{
			const TSharedPtr<FJsonObject>* Anim = nullptr;
			R->TryGetObjectField(TEXT("animation"), Anim);
			if (Anim)
			{
				TestEqual(TEXT("inherited anim_class"), (*Anim)->GetStringField(TEXT("anim_class")), ABP->GetPathName());
			}
		}
	}

	// Recompile and re-read — the assignment must survive the compile.
	FKismetEditorUtilities::CompileBlueprint(BP);
	{
		TSharedPtr<FJsonObject> R = FirstResult(RunOps(BPPath, { Op(OpObj(TEXT("get_animation"), TEXT("SkeletalMeshComponent"))) }));
		TestTrue(TEXT("get after recompile success"), R.IsValid() && R->GetBoolField(TEXT("success")));
		if (R.IsValid())
		{
			const TSharedPtr<FJsonObject>* Anim = nullptr;
			R->TryGetObjectField(TEXT("animation"), Anim);
			if (Anim)
			{
				TestEqual(TEXT("anim_class persists through compile"), (*Anim)->GetStringField(TEXT("anim_class")), ABP->GetPathName());
			}
		}
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Negative: assigning an ABP built for an incompatible skeleton is rejected with
// SKELETON_MISMATCH (the component's mesh uses a different skeleton).
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompAnim_SkeletonMismatch, "KismetAI.Components.Anim.SkeletonMismatch", ComponentAnimTestHelpers::Flags)
bool FCompAnim_SkeletonMismatch::RunTest(const FString& Parameters)
{
	using namespace ComponentAnimTestHelpers;

	USkeleton* SkA = MakeSkeleton(TEXT("/Temp/KismetCompAnimMisSkA"), TEXT("SK_MisA"));
	USkeleton* SkB = MakeSkeleton(TEXT("/Temp/KismetCompAnimMisSkB"), TEXT("SK_MisB"));
	UAnimBlueprint* AbpB = MakeAnimBlueprint(TEXT("/Temp/KismetCompAnimMisAbp"), TEXT("ABP_MisB"), SkB);
	USkeletalMesh* MeshA = MakeSkeletalMesh(TEXT("/Temp/KismetCompAnimMisMesh"), TEXT("SKM_MisA"), SkA);
	TestTrue(TEXT("fixtures created"), SkA && SkB && AbpB && AbpB->GeneratedClass && MeshA);
	if (!SkA || !SkB || !AbpB || !MeshA) return false;

	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetCompAnimMisBP"));
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_CompAnimMis"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!BP) return false;
	const FString BPPath = BP->GetPathName();

	// Component built around skeleton A.
	{
		TSharedPtr<FJsonObject> Add = OpObj(TEXT("add_component"), TEXT("MyMesh"));
		Add->SetStringField(TEXT("class"), TEXT("SkeletalMeshComponent"));
		RunOps(BPPath, { Op(Add) });
		TestTrue(TEXT("seeded mesh A on template"), SetScsMesh(BP, TEXT("MyMesh"), MeshA));
	}

	// Assign an ABP that targets skeleton B → rejected.
	{
		TSharedPtr<FJsonObject> O = OpObj(TEXT("set_animation"), TEXT("MyMesh"));
		O->SetStringField(TEXT("anim_class"), AbpB->GetPathName());
		TSharedPtr<FJsonObject> R = FirstResult(RunOps(BPPath, { Op(O) }));
		TestTrue(TEXT("mismatch rejected"),
			R.IsValid() && !R->GetBoolField(TEXT("success"))
			&& R->GetStringField(TEXT("error_code")) == TEXT("SKELETON_MISMATCH"));
	}

	return true;
}
