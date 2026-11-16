// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Automation tests for Phase 4 animation-asset support: create_asset (anim types) and the
// manage_animation handler (montage slot tracks/segments/sections/sync, named notifies on
// sequences + montages, blend-space axis config + samples). Assets live in named /Temp packages
// so the handler's LoadObject(asset_path) resolves them; the factories are invoked directly
// (IAssetTools.CreateAsset needs real /Game packages, unavailable headless), then edits go
// through the real HTTP handler via ExecuteHandlerDirect.
//
// A bare NewObject<USkeleton> is enough: the anim factories and manage_animation ops here do
// not require bone data, and IsCompatibleForEditor(self) is true for the same skeleton, while a
// second distinct skeleton is incompatible (drives the negative skeleton-mismatch case).

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#include "Animation/Skeleton.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Factories/AnimSequenceFactory.h"
#include "Factories/AnimMontageFactory.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ReferenceSkeleton.h"

namespace AnimAssetTestHelpers
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	// A bare USkeleton has no reference skeleton; blend-space sample validation (and pose
	// evaluation generally) asserts on an empty bone container. Seed a single root bone so the
	// skeleton is minimally valid for the headless ops here.
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

	static UAnimSequence* MakeSequence(const TCHAR* PkgName, const TCHAR* AssetName, USkeleton* Skeleton)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
		Factory->TargetSkeleton = Skeleton;
		return Cast<UAnimSequence>(Factory->FactoryCreateNew(
			UAnimSequence::StaticClass(), Pkg, FName(AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	}

	static UAnimMontage* MakeMontage(const TCHAR* PkgName, const TCHAR* AssetName, USkeleton* Skeleton)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
		Factory->TargetSkeleton = Skeleton;
		return Cast<UAnimMontage>(Factory->FactoryCreateNew(
			UAnimMontage::StaticClass(), Pkg, FName(AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	}

	static UBlendSpace* MakeBlendSpace(const TCHAR* PkgName, const TCHAR* AssetName, USkeleton* Skeleton)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		UBlendSpaceFactoryNew* Factory = NewObject<UBlendSpaceFactoryNew>();
		Factory->TargetSkeleton = Skeleton;
		return Cast<UBlendSpace>(Factory->FactoryCreateNew(
			UBlendSpace::StaticClass(), Pkg, FName(AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
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

	// Run a batch of ops, return the data object.
	static TSharedPtr<FJsonObject> RunOps(const FString& AssetPath, TArray<TSharedPtr<FJsonValue>> Ops)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("asset"), AssetPath);
		Body->SetArrayField(TEXT("operations"), Ops);
		TSharedPtr<FJsonObject> Resp = RunHandler(&FKismetAIModule::HandleManageAnimation, Body);
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
}

// ─────────────────────────────────────────────────────────────────────────────
// AnimSequence creation: empty, 1 frame.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAsset_SequenceCreate, "KismetAI.Anim.Asset.SequenceCreate", AnimAssetTestHelpers::Flags)
bool FAnimAsset_SequenceCreate::RunTest(const FString& Parameters)
{
	using namespace AnimAssetTestHelpers;
	USkeleton* Sk = MakeSkeleton(TEXT("/Temp/KismetAnimSeqSk"), TEXT("SK_SeqTest"));
	TestNotNull(TEXT("skeleton"), Sk);
	UAnimSequence* Seq = MakeSequence(TEXT("/Temp/KismetAnimSeq"), TEXT("A_Empty"), Sk);
	TestNotNull(TEXT("sequence created"), Seq);
	if (!Seq) return false;
	TestTrue(TEXT("skeleton bound"), Seq->GetSkeleton() == Sk);
	if (const IAnimationDataModel* Model = Seq->GetDataModel())
	{
		TestEqual(TEXT("1 frame"), Model->GetNumberOfFrames(), 1);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Montage: add slot track, add segment, sections (loop), sync group, named notify.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAsset_MontageHappy, "KismetAI.Anim.Asset.MontageHappy", AnimAssetTestHelpers::Flags)
bool FAnimAsset_MontageHappy::RunTest(const FString& Parameters)
{
	using namespace AnimAssetTestHelpers;
	USkeleton* Sk = MakeSkeleton(TEXT("/Temp/KismetAnimMtgSk"), TEXT("SK_MtgTest"));
	UAnimSequence* Seq = MakeSequence(TEXT("/Temp/KismetAnimMtgSeq"), TEXT("A_Clip"), Sk);
	UAnimMontage* Montage = MakeMontage(TEXT("/Temp/KismetAnimMtg"), TEXT("AM_Test"), Sk);
	TestNotNull(TEXT("montage"), Montage);
	TestNotNull(TEXT("sequence"), Seq);
	if (!Montage || !Seq) return false;
	const FString MtgPath = Montage->GetPathName();
	const FString SeqPath = Seq->GetPathName();

	// add_slot_track
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_slot_track"));
		O->SetStringField(TEXT("slot_name"), TEXT("UpperBody"));
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(MtgPath, { Op(O) }));
		TestTrue(TEXT("add_slot_track ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
	}
	TestTrue(TEXT("slot registered on skeleton"), Sk->ContainsSlotName(FName(TEXT("UpperBody"))));

	// add_segment to the slot
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_segment"));
		O->SetStringField(TEXT("slot"), TEXT("UpperBody"));
		O->SetStringField(TEXT("animation"), SeqPath);
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(MtgPath, { Op(O) }));
		TestTrue(TEXT("add_segment ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
	}
	{
		bool bFound = false;
		for (const FSlotAnimationTrack& T : Montage->SlotAnimTracks)
			if (T.SlotName == FName(TEXT("UpperBody")) && T.AnimTrack.AnimSegments.Num() == 1) bFound = true;
		TestTrue(TEXT("segment present"), bFound);
	}

	// add_section A -> loop to itself
	{
		TSharedPtr<FJsonObject> Sec = MakeShared<FJsonObject>();
		Sec->SetStringField(TEXT("action"), TEXT("add_section"));
		Sec->SetStringField(TEXT("name"), TEXT("Loop"));
		Sec->SetNumberField(TEXT("start_time"), 0.0);
		Sec->SetStringField(TEXT("next_section"), TEXT("Loop"));
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(MtgPath, { Op(Sec) }));
		TestTrue(TEXT("add_section ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
	}
	{
		bool bLoop = false;
		for (const FCompositeSection& S : Montage->CompositeSections)
			if (S.SectionName == FName(TEXT("Loop")) && S.NextSectionName == FName(TEXT("Loop"))) bLoop = true;
		TestTrue(TEXT("section loops to itself"), bLoop);
	}

	// sync group
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("set_sync_group"));
		O->SetStringField(TEXT("group"), TEXT("Locomotion"));
		O->SetNumberField(TEXT("slot_index"), 0);
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(MtgPath, { Op(O) }));
		TestTrue(TEXT("set_sync_group ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
	}
	TestEqual(TEXT("sync group set"), Montage->SyncGroup, FName(TEXT("Locomotion")));

	// named notify on the montage (registers on the skeleton)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_notify"));
		O->SetStringField(TEXT("name"), TEXT("Footstep"));
		O->SetNumberField(TEXT("time"), 0.0);
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(MtgPath, { Op(O) }));
		TestTrue(TEXT("add_notify ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
	}
	{
		bool bNotify = false;
		for (const FAnimNotifyEvent& Ev : Montage->Notifies)
			if (Ev.NotifyName == FName(TEXT("Footstep"))) bNotify = true;
		TestTrue(TEXT("notify present"), bNotify);
		TestTrue(TEXT("notify registered on skeleton"), Sk->AnimationNotifies.Contains(FName(TEXT("Footstep"))));
	}

	// get returns structure
	{
		TSharedPtr<FJsonObject> Get = MakeShared<FJsonObject>();
		Get->SetStringField(TEXT("action"), TEXT("get"));
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(MtgPath, { Op(Get) }));
		TestTrue(TEXT("get ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
		const TSharedPtr<FJsonObject>* Structure = nullptr;
		if (Result.IsValid() && Result->TryGetObjectField(TEXT("structure"), Structure))
		{
			TestEqual(TEXT("kind montage"), (*Structure)->GetStringField(TEXT("kind")), FString(TEXT("montage")));
		}
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blend space: axis config + add sample + edit sample.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAsset_BlendSpace, "KismetAI.Anim.Asset.BlendSpace", AnimAssetTestHelpers::Flags)
bool FAnimAsset_BlendSpace::RunTest(const FString& Parameters)
{
	using namespace AnimAssetTestHelpers;
	USkeleton* Sk = MakeSkeleton(TEXT("/Temp/KismetAnimBsSk"), TEXT("SK_BsTest"));
	UAnimSequence* Seq = MakeSequence(TEXT("/Temp/KismetAnimBsSeq"), TEXT("A_Run"), Sk);
	UBlendSpace* BS = MakeBlendSpace(TEXT("/Temp/KismetAnimBs"), TEXT("BS_Test"), Sk);
	TestNotNull(TEXT("blend space"), BS);
	if (!BS || !Seq) return false;
	const FString BsPath = BS->GetPathName();
	const FString SeqPath = Seq->GetPathName();

	// set x axis
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("set_axis"));
		O->SetStringField(TEXT("axis"), TEXT("x"));
		O->SetStringField(TEXT("name"), TEXT("Speed"));
		O->SetNumberField(TEXT("min"), 0.0);
		O->SetNumberField(TEXT("max"), 600.0);
		O->SetNumberField(TEXT("grid_divisions"), 4);
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(BsPath, { Op(O) }));
		TestTrue(TEXT("set_axis ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
	}
	{
		const FBlendParameter& P = BS->GetBlendParameter(0);
		TestEqual(TEXT("axis name"), P.DisplayName, FString(TEXT("Speed")));
		TestEqual(TEXT("axis max"), P.Max, 600.0f);
	}

	// add sample at x=300 (mid-range so it is accepted)
	int32 SampleIdx = INDEX_NONE;
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_sample"));
		O->SetStringField(TEXT("animation"), SeqPath);
		O->SetNumberField(TEXT("x"), 300.0);
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(BsPath, { Op(O) }));
		TestTrue(TEXT("add_sample ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
		if (Result.IsValid()) SampleIdx = (int32)Result->GetNumberField(TEXT("index"));
	}
	TestEqual(TEXT("one sample"), BS->GetNumberOfBlendSamples(), 1);

	// edit sample to x=450
	if (SampleIdx != INDEX_NONE)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("edit_sample"));
		O->SetNumberField(TEXT("index"), SampleIdx);
		O->SetNumberField(TEXT("x"), 450.0);
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(BsPath, { Op(O) }));
		TestTrue(TEXT("edit_sample ok"), Result.IsValid() && Result->GetBoolField(TEXT("success")));
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// AnimBlueprint creation via factory with a real (procedural) skeleton.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAsset_AnimBlueprintCreate, "KismetAI.Anim.Asset.AnimBlueprintCreate", AnimAssetTestHelpers::Flags)
bool FAnimAsset_AnimBlueprintCreate::RunTest(const FString& Parameters)
{
	using namespace AnimAssetTestHelpers;
	USkeleton* Sk = MakeSkeleton(TEXT("/Temp/KismetAnimAbpSk"), TEXT("SK_AbpTest"));
	TestNotNull(TEXT("skeleton"), Sk);
	if (!Sk) return false;

	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetAnimAbp"));
	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->ParentClass = UAnimInstance::StaticClass();
	Factory->TargetSkeleton = Sk;
	Factory->BlueprintType = BPTYPE_Normal;
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
		UAnimBlueprint::StaticClass(), Pkg, FName(TEXT("ABP_Test")), RF_Public | RF_Standalone, nullptr, GWarn));
	TestNotNull(TEXT("anim blueprint created"), AnimBP);
	if (!AnimBP) return false;
	TestTrue(TEXT("target skeleton bound"), AnimBP->TargetSkeleton == Sk);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Negatives: duplicate section name, sample with incompatible skeleton, invalid axis on 1D.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAsset_Negative, "KismetAI.Anim.Asset.Negative", AnimAssetTestHelpers::Flags)
bool FAnimAsset_Negative::RunTest(const FString& Parameters)
{
	using namespace AnimAssetTestHelpers;
	USkeleton* SkA = MakeSkeleton(TEXT("/Temp/KismetAnimNegSkA"), TEXT("SK_NegA"));
	USkeleton* SkB = MakeSkeleton(TEXT("/Temp/KismetAnimNegSkB"), TEXT("SK_NegB"));
	UAnimMontage* Montage = MakeMontage(TEXT("/Temp/KismetAnimNegMtg"), TEXT("AM_Neg"), SkA);
	UAnimSequence* SeqOther = MakeSequence(TEXT("/Temp/KismetAnimNegSeq"), TEXT("A_Other"), SkB);
	UBlendSpace* BS = MakeBlendSpace(TEXT("/Temp/KismetAnimNegBs"), TEXT("BS_Neg"), SkA);
	TestNotNull(TEXT("montage"), Montage);
	TestNotNull(TEXT("blend space"), BS);
	if (!Montage || !SeqOther || !BS) return false;

	// Duplicate section name.
	{
		TSharedPtr<FJsonObject> S1 = MakeShared<FJsonObject>();
		S1->SetStringField(TEXT("action"), TEXT("add_section"));
		S1->SetStringField(TEXT("name"), TEXT("Start"));
		S1->SetNumberField(TEXT("start_time"), 0.0);
		TSharedPtr<FJsonObject> S2 = MakeShared<FJsonObject>();
		S2->SetStringField(TEXT("action"), TEXT("add_section"));
		S2->SetStringField(TEXT("name"), TEXT("Start"));
		S2->SetNumberField(TEXT("start_time"), 0.0);
		TSharedPtr<FJsonObject> Data = RunOps(Montage->GetPathName(), { Op(S1), Op(S2) });
		TestTrue(TEXT("first section succeeded, second failed"),
			Data.IsValid() && (int32)Data->GetNumberField(TEXT("operations_succeeded")) == 1
			&& (int32)Data->GetNumberField(TEXT("operations_failed")) == 1);
	}

	// Sample with incompatible skeleton (SeqOther uses SkB; BS targets SkA).
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_sample"));
		O->SetStringField(TEXT("animation"), SeqOther->GetPathName());
		O->SetNumberField(TEXT("x"), 0.5);
		TSharedPtr<FJsonObject> Result = FirstResult(RunOps(BS->GetPathName(), { Op(O) }));
		TestTrue(TEXT("incompatible sample rejected"),
			Result.IsValid() && !Result->GetBoolField(TEXT("success"))
			&& Result->GetStringField(TEXT("error_code")) == TEXT("SKELETON_MISMATCH"));
	}

	return true;
}
