//  Copyright GenericMessagePlugin, Inc. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"

#include "GMPValueOneOf.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "GMPJsonUtils.generated.h"

UENUM(BlueprintType, BlueprintInternalUseonly, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EEJsonEncodeMode : uint8
{
	Default = 0 UMETA(Hidden),
	BoolAsBoolean = 1 << 0,
	EnumAsStr = 1 << 1,
	Int64AsStr = 1 << 2,
	UInt64AsStr = 1 << 3,
	OverflowAsStr = 1 << 4,

	LowerStartCase = 1 << 6,
	StandardizeID = 1 << 7,
	All = BoolAsBoolean | EnumAsStr | Int64AsStr | UInt64AsStr | OverflowAsStr | LowerStartCase | StandardizeID,
};
ENUM_CLASS_FLAGS(EEJsonEncodeMode);

UCLASS()
class UGMPJsonUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "GMP|OneOf(Json)", meta = (CallableWithoutWorldContext, CustomStructureParam = "InOut", AdvancedDisplay = "bComsume"))
	static bool AsStruct(const FGMPValueOneOf& InValue, UPARAM(ref) int32& InOut, FName SubKey, bool bConsume = false);
	DECLARE_FUNCTION(execAsStruct);

	UFUNCTION(BlueprintCallable, Category = "GMP|Json|OneOf", meta = (CallableWithoutWorldContext))
	static void ClearOneOf(UPARAM(ref) FGMPValueOneOf& InValue);

protected:
	static bool AsValueImpl(const FGMPValueOneOf& In, FProperty* Prop, void* Out, FName SubKey);
	static int32 IterateKeyValueImpl(const FGMPValueOneOf& In, int32 Idx, FString& OutKey, FGMPValueOneOf& OutValue);

	friend struct FGMPValueOneOf;

	UFUNCTION(BlueprintCallable, CustomThunk, Category = "GMP|Json", meta = (CallableWithoutWorldContext, CustomStructureParam = "InData"))
	static bool EncodeJsonStr(const int32& InData, FString& OutJsonStr, UPARAM(meta = (Bitmask, BitmaskEnum = "/Script/GMP.EEJsonEncodeMode")) int32 EncodeMode);
	DECLARE_FUNCTION(execEncodeJsonStr);

	UFUNCTION(BlueprintCallable, CustomThunk, Category = "GMP|Json", meta = (CallableWithoutWorldContext, CustomStructureParam = "OutData"))
	static bool DecodeJsonStr(const FString& InJsonStr, UPARAM(ref) int32& OutData);
	DECLARE_FUNCTION(execDecodeJsonStr);
};

DECLARE_DYNAMIC_DELEGATE_TwoParams(FGMPJsonResponseDelegate, bool, bSucc, int32, RspCode);
UCLASS(meta = (NeuronAction))
class UGMPJsonHttpUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	UGMPJsonHttpUtils();

	template<typename TReq, typename TRsp>
	static void HttpPostRequestWild(const UObject* InCtx,
									const FString& Url,
									const TMap<FString, FString>& Headers,
									const TReq& Req,
									TDelegate<void(bool, int32, const TRsp&)> Rsp,
									float TimeoutSecs = 60.f,
									EEJsonEncodeMode ConvertFlags = EEJsonEncodeMode::Default)
	{
		return GMPHttpRequestWildImpl(InCtx,
									  Url,
									  Headers,
									  TimeoutSecs,
									  ConvertFlags,
									  GMP::TClass2Prop<TReq>::GetProperty(),
									  std::addressof(Req),
									  GMP::TClass2Prop<TRsp>::GetProperty(),
									  static_cast<TDelegate<void(bool, int32, const uint8* RspData)>&&>(Rsp));
	}

protected:
	static bool GMPHttpRequestWildImpl(const UObject* InCtx,
									   const FString& Url,
									   const TMap<FString, FString>& Headers,
									   float TimeoutSecs,
									   EEJsonEncodeMode ConvertFlags,
									   FProperty* BodyProp,
									   const uint8* BodyData,
									   FProperty* RspProp,
									   TDelegate<void(bool, int32, const uint8* RspData)> OnRsp);

	UFUNCTION(BlueprintCallable,
			  CustomThunk,
			  BlueprintInternalUseOnly,
			  Category = "GMP|Json|HTTP",
			  meta = (DisplayName = "GMPHttpGetRequest", NeuronAction, WorldContext = InCtx, CustomStructureParam = "ResponseStruct", TimeoutSecs = "60", AutoCreateRefTerm = "Headers", AdvancedDisplay = "Headers,TimeoutSecs"))
	static void HttpGetRequestWild(const UObject* InCtx,
								   const FString& Url,
								   const TMap<FString, FString>& Headers,
								   float TimeoutSecs,
								   const FGMPJsonResponseDelegate& OnHttpResponse,
								   UPARAM(Ref, meta = (RequiresReference)) int32& ResponseStruct);
	DECLARE_FUNCTION(execHttpGetRequestWild);

	UFUNCTION(BlueprintCallable,
			  CustomThunk,
			  BlueprintInternalUseOnly,
			  Category = "GMP|Json|HTTP",
			  meta = (DisplayName = "GMPHttpPostRequest",
					  NeuronAction,
					  WorldContext = InCtx,
					  CustomStructureParam = "RequestStruct,ResponseStruct",
					  TimeoutSecs = "60",
					  ConvertFlags = "0",
					  AutoCreateRefTerm = "Headers",
					  AdvancedDisplay = "Headers,TimeoutSecs"))
	static void HttpPostRequestWild(const UObject* InCtx,
									const FString& Url,
									const TMap<FString, FString>& Headers,
									float TimeoutSecs,
									const FGMPJsonResponseDelegate& OnHttpResponse,
									UPARAM(meta = (Bitmask, BitmaskEnum = "/Script/GMP.EEJsonEncodeMode")) int32 ConvertFlags,
									UPARAM(meta = (RequiresReference)) const int32& RequestStruct,
									UPARAM(Ref, meta = (RequiresReference)) int32& ResponseStruct);
	DECLARE_FUNCTION(execHttpPostRequestWild);
};
