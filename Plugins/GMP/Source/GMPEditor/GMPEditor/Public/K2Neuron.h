// Copyright K2Neuron, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "EdGraph/EdGraphNodeUtils.h"
#include "Engine/MemberReference.h"
#include "K2Node.h"
#include "Templates/SubclassOf.h"
#include "UObject/ObjectMacros.h"
#include "UnrealCompatibility.h"

#if WITH_EDITOR
#include "KismetNodes/SGraphNodeK2Default.h"
#endif

#include "K2Neuron.generated.h"

class FBlueprintActionDatabaseRegistrar;
class FKismetCompilerContext;
class UEdGraph;
class UEdGraphPin;
class UEdGraphSchema_K2;
class UK2Node_CustomEvent;
class UK2Node_TemporaryVariable;
class UK2Node_BaseMCDelegate;
class UK2Node_Event;
class UK2Node_CallFunction;
struct FEdGraphPinType;

#ifndef K2NEURON_API
#define K2NEURON_API GMPEDITOR_API
#endif

USTRUCT()
struct K2NEURON_API FNeuronBase
{
	GENERATED_BODY()
public:
	static const FName ScopeSelf;
	static const FName ScopeProxy;
	static const FName ScopeObject;

	static const FName TypeProp;
	static const FName TypeParam;
	static const FName TypeEvent;

	static const FName ClassDelim;
	static const FName FunctionDelim;
	static const FName DelegateDelim;
	static const FName MemberDelim;
	static const FName EnumExecDelim;
};

USTRUCT()
struct K2NEURON_API FNeuronPinBag : public FNeuronBase
{
	GENERATED_BODY()
public:
	UPROPERTY()
	TArray<FName> PropChain;

	void Push(FName InProp) { PropChain.Add(InProp); }
	void PushClass(const UClass* InClass)
	{
		PropChain.Add(ClassDelim);
#if UE_5_00_OR_LATER
		PropChain.Add(*GetPathNameSafe(InClass));
#else
		PropChain.Add(GetFNameSafe(InClass));
#endif
	}
	void PushFunction(const UFunction* InFunc)
	{
		PropChain.Add(ClassDelim);
		PropChain.Add(GetFNameSafe(InFunc));
	}

	void PushFiledName(const FField* InField)
	{
#if UE_5_00_OR_LATER
		PropChain.Add(*GetPathNameSafe(InField));
#else
		PropChain.Add(GetFNameSafe(InField));
#endif
	}
	void Pop() { PropChain.Pop(); }

	template<typename... NameTypes>
	FNeuronPinBag& Append(NameTypes&&... InNames)
	{
		const FName Names[] = {FName(InNames)...};
		for (auto& Name : Names)
		{
			Push(Name);
		}
		return *this;
	}

	template<typename... NameTypes>
	FNeuronPinBag Combine(NameTypes&&... InNames) const
	{
		const FName Names[] = {FName(InNames)...};
		FNeuronPinBag Ret = *this;
		for (auto& Name : Names)
		{
			Ret.Push(Name);
		}
		return Ret;
	}
	FNeuronPinBag() = default;
	FNeuronPinBag(FName InProp) { PropChain.Add(InProp); }

	FNeuronPinBag LeftChop(FName InProp) const
	{
		FNeuronPinBag Ret = *this;
		for (auto i = 0; i < Ret.PropChain.Num(); ++i)
		{
			if (Ret.PropChain[i] == InProp)
			{
				Ret.PropChain.RemoveAt(i, Ret.PropChain.Num() - i);
				break;
			}
		}
		return Ret;
	}

	friend bool operator==(const FNeuronPinBag& Lhs, const FNeuronPinBag& Rhs) { return Lhs.PropChain == Rhs.PropChain; }

	// Scope + Type + ClassDelim + ClassName + MemberDelim + MemberName
	// Scope + Type + ClassDelim + ClassName + DelegateDelim + DelegateName
	// Scope + Type + ClassDelim + ClassName + DelegateDelim + DelegateName + MemberDelim + MemberName
	// Scope + Type + ClassDelim + ClassName + DelegateDelim + DelegateName + EnumExecDelim + MemberName
	// Scope + Type + ClassDelim + ClassName + FunctionDelim + FunctionName
	// Scope + Type + ClassDelim + ClassName + FunctionDelim + FunctionName + MemberDelim + MemberName
	// Scope + Type + ClassDelim + ClassName + FunctionDelim + FunctionName + DelegateDelim + DelegateName
	// Scope + Type + ClassDelim + ClassName + FunctionDelim + FunctionName + DelegateDelim + DelegateName + MemberDelim + MemberName
	// Scope + Type + ClassDelim + ClassName + FunctionDelim + FunctionName + DelegateDelim + DelegateName + EnumExecDelim + MemberName

	FString GetClassName() const { return After(ClassDelim); }
	FString GetFunctionName() const { return After(FunctionDelim); }
	FString GetDelegateName() const { return After(DelegateDelim); }
	FString GetMemberName() const { return After(MemberDelim, EnumExecDelim); }
	FString GetEnumExecName() const { return After(EnumExecDelim); }

	FString After(FName Delim) const
	{
		auto Idx = PropChain.IndexOfByKey(Delim);
		return (Idx >= 0 && PropChain.IsValidIndex(Idx + 1)) ? PropChain[Idx + 1].ToString() : FString();
	}
	FString After(FName Delim1, FName Delim2) const
	{
		auto Ret = After(Delim1);
		if (!Ret.Len())
		{
			Ret = After(Delim2);
		}
		return Ret;
	}

	FName GetScope() const
	{
		if (PropChain.Num() > 0)
		{
			return PropChain[0];
		}
		return NAME_None;
	}

	FName GetType() const
	{
		if (PropChain.Num() > 1)
		{
			return PropChain[1];
		}
		return NAME_None;
	}
	bool StartsWith(const FNeuronPinBag& InBag) const
	{
		if (InBag.PropChain.Num() > PropChain.Num())
		{
			return false;
		}
		for (int32 i = 0; i < InBag.PropChain.Num(); ++i)
		{
			if (InBag.PropChain[i] != PropChain[i])
			{
				return false;
			}
		}
		return true;
	}

	bool MatchPrefix(const FNeuronPinBag& InBag) const { return InBag.StartsWith(*this); }

	bool IsValid() const { return PropChain.Num() > 0; }
};

struct FNeuronPinBagScope
{
	FNeuronPinBag& Ref;
	int32 Idx = 0;
	FNeuronPinBagScope(FNeuronPinBag& InRef)
		: Ref(InRef)
		, Idx(Ref.PropChain.Num())
	{
	}
	~FNeuronPinBagScope() { Ref.PropChain.RemoveAt(Idx, Ref.PropChain.Num() - Idx); }

	operator FNeuronPinBag() const { return Ref; }
	template<typename... NameTypes>
	static FNeuronPinBagScope Make(FNeuronPinBag& InRef, NameTypes&&... InNames)
	{
		FNeuronPinBagScope Ret(InRef);
		Ret.Ref.Append(InNames...);
		return Ret;
	}
};

USTRUCT()
struct K2NEURON_API FEdPinExtraMetaBase : public FNeuronBase
{
	GENERATED_BODY()
public:
	UPROPERTY()
	FNeuronPinBag BagInfo;

	FString GetClassName() const { return BagInfo.GetClassName(); }
	FString GetMemberName() const { return BagInfo.GetMemberName(); }
	FString GetDelegateName() const { return BagInfo.GetDelegateName(); }
	FString GetFunctionName() const { return BagInfo.GetFunctionName(); }
	FString GetEnumExecName() const { return BagInfo.GetEnumExecName(); }

public:
	inline bool HasMeta(const FName& InName) const { return Metas.Contains(InName); }
	inline TOptional<FString> GetMeta(const FName& InName) const { return Metas.FindRef(InName); }
	inline TOptional<FString> SetMeta(const FName& InName, const FString& InValue)
	{
		auto Ret = GetMeta(InName);
		Metas.Add(InName, InValue);
		return Ret;
	}
	inline TOptional<FString> ClearMeta(const FName& InName)
	{
		auto Ret = GetMeta(InName);
		Metas.Remove(InName);
		return Ret;
	}

	static FName IsObjClsPin;
	static FName IsImportedPin;
	static FName IsSpawnedPin;
	static FName IsSelfSpawnedPin;
	static FName IsBindedPin;
	static FName IsCustomStructurePin;
	static FName IsAutoCreateRefTermPin;
	static FName IsNeuronCheckablePin;
	static FName EnumExecMeta;
	static FName BoolExecMeta;

protected:
	UPROPERTY()
	TMap<FName, FString> Metas;
};

USTRUCT()
struct K2NEURON_API FEdPinExtraMeta : public FEdPinExtraMetaBase
{
	GENERATED_BODY()
public:
	UClass* OwnerClass = nullptr;
	UFunction* SubFunction = nullptr;
	FDelegateProperty* SubDelegate = nullptr;
};

UCLASS(abstract)
class K2NEURON_API UK2Neuron
	: public UK2Node
	, public FNeuronBase
{
	GENERATED_BODY()
public:
#define K2NEURON_USE_PERSISTENTGUID 1
	static FGuid SetPinGuid(UEdGraphPin* Pin)
	{
		if (Pin->PersistentGuid.IsValid())
		{
			return Pin->PersistentGuid;
		}
		else
		{
			Pin->PersistentGuid = Pin->PinId;
		}
		return Pin->PersistentGuid;
	}

	static FGuid GetPinGuid(UEdGraphPin* Pin)
	{
#if K2NEURON_USE_PERSISTENTGUID
		return SetPinGuid(Pin);
#else
		return Pin->PinId;
#endif
	}
	static FGuid GetPinGuid(const UEdGraphPin* Pin) { return GetPinGuid(const_cast<UEdGraphPin*>(Pin)); }

	static void FindInBlueprint(const FString& InStr, UBlueprint* Blueprint = nullptr);

	static const bool HasVariadicSupport;

	UK2Neuron(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	static void StaticAssignProperty(UK2Node_BaseMCDelegate* InNode, const FProperty* Property, bool bSelfContext = false, UClass* OwnerClass = nullptr);
	static const UEdGraphSchema_K2* GetK2Schema(const FKismetCompilerContext& CompilerContext);
	static const UEdGraphSchema_K2* GetK2Schema(const UK2Node* InNode);
	const UEdGraphSchema_K2* GetK2Schema() const { return GetK2Schema(this); }
	static UClass* GetBlueprintClass(const UK2Node* InNode);
	UClass* GetBlueprintClass() const { return GetBlueprintClass(this); }

	static bool IsInputParameter(const FProperty* FuncParam, bool bEnsure = false);
	bool IsExecPin(UEdGraphPin* Pin, EEdGraphPinDirection Direction = EGPD_MAX);

	bool IsExpandEnumAsExec(UFunction* Function, UEnum** OutEnum = nullptr, FProperty** OutProp = nullptr, FName ExecParamName = NAME_None) const;
	bool IsExpandEnumAsExec(FMulticastDelegateProperty* MCDProp, UEnum** OutEnum = nullptr, FProperty** OutProp = nullptr) const;

	bool IsExpandBoolAsExec(UFunction* Function, FBoolProperty** OutProp = nullptr, FName ExecParamName = NAME_None) const;
	bool IsExpandBoolAsExec(FMulticastDelegateProperty* MCDProp, FBoolProperty** OutProp = nullptr) const;

	static UEdGraphPin* FindThenPin(const UK2Node* InNode, bool bChecked = true);
	UEdGraphPin* FindThenPin(bool bChecked = true) const { return FindThenPin(this, bChecked); }
	static UEdGraphPin* FindDelegatePin(const UK2Node_Event* InNode, bool bChecked = true);

	static UEdGraphPin* FindExecPin(const UK2Node* InNode, EEdGraphPinDirection Dir = EGPD_MAX, bool bChecked = true);
	UEdGraphPin* FindExecPin(EEdGraphPinDirection Dir = EGPD_MAX, bool bChecked = true) const { return FindExecPin(this, Dir, bChecked); }

	static UEdGraphPin* FindValuePin(const UK2Node* InNode, EEdGraphPinDirection Dir = EGPD_MAX, FName PinCategory = NAME_None);
	UEdGraphPin* FindValuePin(FName PinCategory = NAME_None, EEdGraphPinDirection Dir = EGPD_MAX) const { return FindValuePin(this, Dir, PinCategory); }

	static UEdGraphPin* FindObjectPin(const UK2Node* InNode, UClass* ObjClass, EEdGraphPinDirection Dir = EGPD_MAX);
	UEdGraphPin* SearchPin(const FName PinName, const TArray<UEdGraphPin*>* InPinsToSearch = nullptr, const EEdGraphPinDirection Direction = EEdGraphPinDirection::EGPD_MAX) const;
	UEdGraphPin* SearchPin(const FGuid PinGuid, const TArray<UEdGraphPin*>* InPinsToSearch = nullptr, const EEdGraphPinDirection Direction = EEdGraphPinDirection::EGPD_MAX) const;
	UEdGraphPin* SearchPin(const FNeuronPinBag PinBag, const TArray<UEdGraphPin*>* InPinsToSearch = nullptr, const EEdGraphPinDirection Direction = EEdGraphPinDirection::EGPD_MAX) const;

	static UClass* ClassFromPin(UEdGraphPin* ClassPin, bool bFallback = true);
	UEdGraphPin* GetSpecialClassPin(const TArray<UEdGraphPin*>& InPinsToSearch, FName PinName, UClass** OutClass = nullptr) const;
	UEdGraphPin* GetSpecialClassPin(const TArray<UEdGraphPin*>& InPinsToSearch, FGuid PinGuid, UClass** OutClass = nullptr) const;

	UClass* GetSpecialPinClass(const TArray<UEdGraphPin*>& InPinsToSearch, FName PinName, UEdGraphPin** OutPin = nullptr) const;
	UClass* GetSpecialPinClass(const TArray<UEdGraphPin*>& InPinsToSearch, FGuid PinGuid, UEdGraphPin** OutPin = nullptr) const;

	static bool IsTypePickerPin(UEdGraphPin* Pin);

	static bool HasAnyConnections(const UEdGraphPin* InPin);

	static void FillCustomStructureParameterNames(const UFunction* Function, TArray<FString>& OutNames);

	static void UpdateCustomStructurePins(const UFunction* Function, UK2Node* Node, UEdGraphPin* SinglePin = nullptr);

	void UpdateCustomStructurePin(UEdGraphPin* SinglePin);
	void UpdateCustomStructurePin(TArray<UEdGraphPin*>* InOldPins = nullptr);

	using UEdGraphNode::FindPin;
	static UEdGraphPin* FindPin(FGuid InGuid, const TArray<UEdGraphPin*>& InPins);
	UEdGraphPin* FindPin(FGuid InGuid, TArray<UEdGraphPin*>* InOldPins = nullptr) const;
	UEdGraphPin* FindPin(const FNeuronPinBag& InBag, const TArray<UEdGraphPin*>& InPins) const;
	UEdGraphPin* FindPin(const FNeuronPinBag& InBag, TArray<UEdGraphPin*>* InOldPins = nullptr) const;

private:
	static void HandleSinglePinWildStatus(UEdGraphPin* Pin);

public:
	const FName MetaHidePins;
	UPROPERTY()
	FName MetaTagFlag;
	UPROPERTY()
	FString MetaTagPostfix;

	bool HasSpecialTag(const FFieldVariant& Field) const;
	bool HasSpecialImportTag(const FFieldVariant& Field) const;
	bool HasSpecialExportTag(const FFieldVariant& Field) const;
	FString GetPinFriendlyName(FString Name);
	FString GetDisplayString(const FFieldVariant& Field, const TCHAR* Prefix = nullptr);
	FText GetDisplayText(const FFieldVariant& Field, const TCHAR* Prefix = nullptr);
	FText GetCustomDisplayName(const FString& ClassName, const FFieldVariant& Field, const FString& Postfix = {});

	// Tasks can hide spawn parameters by doing meta = (HideSpawnParms="PropertyA,PropertyB")
	// (For example, hide Instigator in situations where instigator is not relevant to your task)
	TSet<FString> GetHideProperties(const FFieldVariant& InFieldVariant, FField* InFieldOptional = nullptr);
	FName NameHideProperties;
	FName NameShowProperties;

public:
	bool DoOnce(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& InOutThenPin, UEdGraphPin* OnceExecPin);
	bool LaterDo(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& InOutThenPin, const TArray<UEdGraphPin*>& ExecPins);
	FORCEINLINE bool LaterDo(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& InOutThenPin, UEdGraphPin* ExecPin) { return LaterDo(CompilerContext, SourceGraph, InOutThenPin, TArray<UEdGraphPin*>{ExecPin}); }
	bool SequenceDo(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& InOutThenPin, const TArray<UEdGraphPin*>& ExecPins);
	FORCEINLINE bool SequenceDo(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& InOutThenPin, UEdGraphPin* Exec) { return SequenceDo(CompilerContext, SourceGraph, InOutThenPin, TArray<UEdGraphPin*>{Exec}); }
	bool ConditionDo(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* ConditionPin, UEdGraphPin*& InOutThenPin, UEdGraphPin* ExecPin, UEdGraphPin* ElsePin = nullptr);
	UEdGraphPin* BranchExec(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* ConditionPin, UEdGraphPin* ExecPin, UEdGraphPin* ElsePin = nullptr);
	bool BranchThen(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& InOutThenPin, UEdGraphPin* ConditionPin, UEdGraphPin*& ElsePin);

	TMap<UEdGraphPin*, UEdGraphPin*> TemporaryVariables;
	bool AssignTempAndGet(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& InOutThenPin, UEdGraphPin*& InOutVarPin, bool bPure = false, UEdGraphPin* ConnectingPin = nullptr);
	UEdGraphPin* AssignValueAndGet(UK2Node_TemporaryVariable* InVarNode, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& LastThenPin, const FString& InValue);

	bool CastAssignAndGet(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& InOutThenPin, UEdGraphPin*& InOutVarPin, UClass* InClass, bool bPure = false);
	UEdGraphPin* PureValidateObjectOrClass(class FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* InObjectPin);
	UEdGraphPin* SpawnPureVariableTo(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* SourceTypePin, const FString& Value = {});
	UEdGraphPin* SpawnPureVariable(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* InVariablePin, const FString& DefaultValue, bool bConst = true);

	UEdGraphPin* MakeTemporaryVariable(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, const FEdGraphPinType& PinType);
	TMap<UEdGraphPin*, UEdGraphPin*> LiteralVariables;
	UEdGraphPin* MakeLiteralVariable(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* SrcPin, const FString& Value = {});
	bool TryCreateConnection(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* InPinA, UEdGraphPin* InPinB, bool bMove = true);
	FORCEINLINE bool TryCreateConnection(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin& InPinA, UEdGraphPin& InPinB, bool bMove = true)
	{
		return TryCreateConnection(CompilerContext, SourceGraph, &InPinA, &InPinB, bMove);
	}

public:
	UEdGraphPin* ParamsToArrayPin(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, const TArray<UEdGraphPin*>& InPins);
	bool ConnectAdditionalPins(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UK2Node_CallFunction* FuncNode, const TArray<UEdGraphPin*>& InPins, bool bWithCnt = false);
	bool ConnectMessagePins(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UK2Node_CallFunction* FuncNode, const TArray<UEdGraphPin*>& InPins);

public:
	TCHAR DelimiterChar;
	FString DelimiterStr;

	FName RequiresConnection;
	FName DisableConnection;
	FName RequiresReference;

	bool bHasAdvancedViewPins = false;
	bool IsAllocWithOldPins() const { return bAllocWithOldPins; }

	UEdGraphPin* CreatePinFromInnerClsProp(const UClass* InDerivedCls, FProperty* Property, FNeuronPinBag InPrefix, const FString& InDisplayPrefix = TEXT("."), EEdGraphPinDirection Direction = EGPD_MAX);
	UEdGraphPin* CreatePinFromInnerFuncProp(FFieldVariant InFuncOrDelegate, FProperty* Property, FNeuronPinBag InPrefix, const FString& InDisplayPrefix = TEXT("."), EEdGraphPinDirection Direction = EGPD_MAX);

	UEdGraphPin* CreatePinFromInnerProp(const UObject* ClsOrFunc, FProperty* Property, FNeuronPinBag InPrefix, const FString& InDisplayPrefix = TEXT("."), EEdGraphPinDirection Direction = EGPD_MAX);

	TCHAR MetaEventPrefix;
	UK2Node_CustomEvent* GetMetaEventForClass(UClass* InClass, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, bool bCreate);

public:
	virtual bool GetInstancedFlag(UClass* InClass) const { return true; }
	// we need this info to create pins
	virtual UFunction* GetAlternativeAction(UClass* InClass) const { return nullptr; }
	TArray<FGuid> CreateImportPinsForClass(UClass* InClass, FName Scope, bool bImportFlag = true, TArray<UEdGraphPin*>* OldPins = nullptr);
	bool ConnectImportPinsForClass(UClass* InClass, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& LastThenPin, UEdGraphPin* InstancePin, bool bOverrideDefault = true, bool bOverrideRemote = false);

	struct FPinMetaInfo : public FEdPinExtraMetaBase
	{
		UClass* OwnerClass = nullptr;

		UFunction* SubFuncion = nullptr;
		FDelegateProperty* FuncDelegate = nullptr;

		FMulticastDelegateProperty* SubDelegate = nullptr;

		FProperty* Prop = nullptr;

		bool HasSubStructure() const { return SubFuncion || SubDelegate; }
		K2NEURON_API UObject* GetObjOrFunc() const;
	};

	FPinMetaInfo GetPinMetaInfo(FNeuronPinBag PinBag, bool bRedirect = false, bool bEnsure = true) const;
	virtual void GetRedirectPinNames(const UEdGraphPin& Pin, TArray<FString>& RedirectPinNames) const override;

	void GetRedirectPinNamesImpl(const UEdGraphPin& Pin, TArray<FString>& RedirectPinNames) const;
	virtual ERedirectType DoPinsMatchForReconstruction(const UEdGraphPin* NewPin, int32 NewPinIndex, const UEdGraphPin* OldPin, int32 OldPinIndex) const override;

public:
	bool CreateOutPinsForDelegate(FNeuronPinBag InPrefix, const FProperty* DelegateProp, bool bAdvanceView, UEdGraphPin** OutParamPin = nullptr);
	bool CreateSubPinsForDelegate(const FString& InPrefix, const FProperty* DelegateProp, bool bAdvanceView, UEdGraphPin** OutParamPin = nullptr);
	bool CreateDelegatesForClass(UClass* InClass, FName Scope, UClass* StopClass = nullptr, TArray<UEdGraphPin*>* OldPins = nullptr);
	bool CreateEventsForClass(UClass* InClass, FName Scope, UClass* StopClass = nullptr, TArray<UEdGraphPin*>* OldPins = nullptr);

	static bool ValidDataPin(const UEdGraphPin* Pin, EEdGraphPinDirection Direction);
	static bool CopyEventSignature(UK2Node_CustomEvent* CENode, UFunction* Function, const UEdGraphSchema_K2* Schema);
	static bool CreateDelegateForNewFunction(UEdGraphPin* DelegateInputPin, FName FunctionName, UK2Node* CurrentNode, UEdGraph* SourceGraph, FKismetCompilerContext& CompilerContext);

	struct FDelegateEventOptions
	{
		FName Scope;

		virtual UK2Node_CustomEvent* MakeEventNode(UK2Node* InNode, UEdGraphPin* InstPin, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, const TCHAR* Prefix, const TCHAR* Postfix = nullptr) = 0;
		virtual void NotifyIfNeeded(UK2Node* InNode, UEdGraphPin* InstPin, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UK2Node_CustomEvent* EventNode, UEdGraphPin*& EventThenPin, const FString& DelegateName) {}

		virtual bool ContainsSpecialAction(const UEdGraphPin* ExecPin) { return false; }
		virtual UEdGraphPin* GetSpecialActionExec(UK2Node* InNode, UEdGraphPin* InstPin, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) { return nullptr; }
	};

	bool BindDelegateEvents(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* InstPin, UEdGraphPin*& LastAddDelegateThenPin, UEdGraphPin*& LastRemoveDelegateThenPin, FDelegateEventOptions& Options);

	bool ConnectLocalFunctions(TSet<FGuid>& SkipPinGuids, FNeuronPinBag InParamPrefix, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* InstPin);
	bool ConnectRemoteFunctions(UFunction* InProxyFunc,
								TSet<FGuid>& SkipPinGuids,
								FNeuronPinBag InParamPrefix,
								FKismetCompilerContext& CompilerContext,
								UEdGraph* SourceGraph,
								UEdGraphPin* InstPin,
								TFunctionRef<void(UK2Node* /*FuncNode*/, UK2Node_CustomEvent* /*EventNode*/)> NodeOpreation);
	UEdGraphPin* PureCastTo(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* InstPin, UEdGraphPin* TargetPin, UEdGraphPin** BoolSuccessPin = nullptr);
	UEdGraphPin* PureCastClassTo(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* InstPin, UEdGraphPin* TargetPin, UEdGraphPin** BoolSuccessPin = nullptr);

	static bool IsPinValueOnPropertyModified(UEdGraphPin* GraphPinObj, UObject* ObjOrFunc, FProperty* Prop, bool bReset = false);
	bool IsPinValueModified(UEdGraphPin* GraphPinObj, bool bReset = false) const;

	TArray<UEdGraphPin*> RemovePinsByName(const TSet<FName>& Names, bool bAffixes = true);

	TArray<UEdGraphPin*> RemovePinsByGuid(const TSet<FGuid>& Guids, bool bAffixes = true);

	bool IsInUbergraph() const;

public:
	FName BeginSpawningFuncName;
	FName PostSpawningFuncName;
	FName SpawnedSucceedName;
	FName SpawnedObjectName;
	FName SpawnedFailedName;

	FName SpawnedObjectPropName;
	FName ObjectClassPropName;
	FName SpawnedDelegatePropName;

	FName UnlinkObjEventName;
	FName MetaSplitStructPin;

	UPROPERTY()
	int32 NodeVersion = 0;

	UPROPERTY()
	FGuid ObjClassPinGuid;
	UPROPERTY()
	TArray<FGuid> ImportedPinGuids;
	UPROPERTY()
	TArray<FGuid> SpawnedPinGuids;
	UPROPERTY()
	TArray<FGuid> BindedPinGuids;
	UPROPERTY()
	TSet<FGuid> CustomStructurePinGuids;
	UPROPERTY()
	TSet<FGuid> AutoCreateRefTermPinGuids;
	UPROPERTY()
	TSet<FGuid> NeuronCheckableGuids;
	UPROPERTY()
	TArray<FNeuronPinBag> NeuronCheckableBags;

	TArray<TPair<UEdGraphPin*, UEdGraphPin*>> ObjectPreparedPins;
	TWeakObjectPtr<UK2Node_CallFunction> ProxySpawnFuncNode;
	bool ConnectObjectPreparedPins(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UK2Node_CallFunction* FuncNode, bool bUseKey = true);

	bool IsObjectClassPin(const UEdGraphPin* Pin, bool bExact = true) const;
	virtual UEdGraphPin* GetObjectFactoryClassPin(const TArray<UEdGraphPin*>* InPinsToSearch = nullptr) const;
	UEdGraphPin* GetSpawnedObjectClassPin(const TArray<UEdGraphPin*>* InPinsToSearch = nullptr, UClass** OutClass = nullptr) const;
	UClass* GetSpawnedObjectClass(const TArray<UEdGraphPin*>* InPinsToSearch = nullptr, UEdGraphPin** OutPin = nullptr) const;
	bool CreateObjectSpawnPins(UClass* OwnerClass, TArray<UEdGraphPin*>* InPinsToSearch = nullptr, UClass* InOverrideClass = nullptr);
	UClass* ValidateObjectSpawning(UClass* OwnerClass, FKismetCompilerContext* CompilerContext, const TArray<UEdGraphPin*>& InPinsToSearch, UClass** OutClass = nullptr);
	FProperty* DetectObjectSpawning(UClass* OwnerClass, FKismetCompilerContext* CompilerContext, FName DetectPinName = NAME_None);
	UEdGraphPin* ConnectObjectSpawnPins(UClass* OwnerClass, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin*& LastThenPin, UEdGraphPin* InstancePin, UFunction* ProxyFunc = nullptr);

	void OnSpawnedObjectClassChanged(UClass* OwnerClass, const TArray<UEdGraphPin*>* InPinsToSearch = nullptr);

	bool BindObjBlueprintCompiledEvent(UClass* InClass);

private:
	TWeakObjectPtr<UClass> LastObjClass;
	FDelegateHandle ObjHandle;

	TWeakObjectPtr<class UK2Node_Self> CtxSelfNode;
	UEdGraphPin* GetCtxPin() const;
	bool ShouldDefaultToCtx(UEdGraphPin* InPin) const;
	bool ShouldDefaultToCtx(FProperty* InProp) const;
	bool bAllocWithOldPins = false;

protected:
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FNodeHandlingFunctor* CreateNodeHandler(FKismetCompilerContext& CompilerContext) const override;
	virtual FName GetCornerIcon() const override;
	virtual FText GetPinDisplayName(const UEdGraphPin* Pin) const override;
	virtual void PostPasteNode() override;
	virtual bool HasExternalDependencies(TArray<class UStruct*>* OptionalOutput) const override;
	virtual bool CanSplitPin(const UEdGraphPin* Pin) const;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual void AllocateDefaultPins() override;
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual bool CanJumpToDefinition() const override;
	virtual void JumpToDefinition() const override;
	void JumpToDefinitionClass(UClass* InClass) const;
	virtual void AllocateDefaultPinsImpl(TArray<UEdGraphPin*>* InOldPins = nullptr) {}
	void CallAllocateDefaultPinsImpl(TArray<UEdGraphPin*>* InOldPins = nullptr);
	virtual void PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void EarlyValidation(class FCompilerResultsLog& MessageLog) const override;
	virtual void ClearCachedBlueprintData(UBlueprint* Blueprint) override;
	virtual void PostReconstructNode() override;
	virtual bool IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const override;
	UK2Node* GetConnectedNode(UEdGraphPin* Pin, TSubclassOf<UK2Node> NodeClass) const;
	UEdGraphPin* CastIfFloatType(UEdGraphPin* TestSelfPin, FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* LinkPin = nullptr);

protected:
	mutable bool bClassChanged = false;
	TWeakObjectPtr<UClass> LastClass;
	FDelegateHandle CompiledHandle;
	FDelegateHandle PreCompilingHandle;
	void BindBlueprintCompiledEvent(UClass* InClass);
	void BindBlueprintPreCompilingEvent();
	virtual void OnAssociatedBPCompiled(UBlueprint* ClassBP, UObject* OldCDO = nullptr) {}

	FDelegateHandle OwnerCompiledHandle;
	virtual void OnOwnerBPCompiled() {}
	void BindOwnerBlueprintCompiledEvent();
	UK2Node_CustomEvent* MakeEventNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, const FString& DescName, uint32* OutID = nullptr);
	UK2Node_CustomEvent* MakeEventNodeRemote(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, const FString& DescName, uint32* OutID = nullptr);
	TArray<FName> RemoteEventNodes;

	uint32 MakeShortEventName(UK2Node_CustomEvent* Event, const FString& DescName);

	// reference to old object when compiling
	TWeakObjectPtr<UObject> DataCDO;

public:
	template<typename T = UObject>
	const T* FindDefaultObject(UClass* InClass) const
	{
		check(InClass && InClass->IsChildOf<T>());
		const T* Ret = Cast<T>(InClass->ClassDefaultObject);
		if (!Ret && IsBeingCompiled())
			Ret = Cast<T>(DataCDO.Get());
		return Ret;
	}
	bool IsBeingCompiled() const;

private:
	virtual bool ShouldInputPropCheckable(const FProperty* Prop) const { return false; }
	virtual bool ShouldEventParamCheckable(const FProperty* Prop) const { return false; }
	UPROPERTY()
	uint8 NodeUniqueID = 0;
	uint8 GenerateUniqueNodeID(TArray<UK2Neuron*>& Neurons, TSubclassOf<UK2Neuron> NeuronClass, bool bCompact = true);

	UPROPERTY()
	TMap<FGuid, FEdPinExtraMeta> PinExtraMetas;

public:
	void RemoveUselessPinMetas();
	FNeuronPinBag GetPinBag(FGuid Guid) const
	{
		auto Find = PinExtraMetas.Find(Guid);
		return Find ? Find->BagInfo : FNeuronPinBag();
	}
	FNeuronPinBag GetPinBag(const UEdGraphPin* InPin) const { return GetPinBag(GetPinGuid(InPin)); }

	const FEdPinExtraMeta* FindPinExtraMeta(const UEdGraphPin* InPin) const { return PinExtraMetas.Find(GetPinGuid(InPin)); }

	FPinMetaInfo GetPinMetaInfo(const UEdGraphPin* InPin, bool bRedirect = false, bool bEnsure = true) const;
	bool MatchAffixes(const UEdGraphPin* InPin, bool bSelf, bool bProxy, bool bObject) const;
	bool MatchAffixesEvent(const UEdGraphPin* InPin, bool bSelf, bool bProxy, bool bObject) const;
	bool MatchAffixesMember(const UEdGraphPin* InPin, bool bSelf, bool bProxy, bool bObject) const;
	bool MatchAffixesInput(const UEdGraphPin* InPin, bool bSelf, bool bProxy, bool bObject) const;

protected:
	auto& SetPinMetaBag(UEdGraphPin* InPin, FNeuronPinBag PinMetaBag)
	{
		check(InPin);
#define K2NEURON_DISABLE_FRIENDLYNAME 1
#if K2NEURON_DISABLE_FRIENDLYNAME
		InPin->bAllowFriendlyName = false;
#endif
		auto& Info = PinExtraMetas.FindOrAdd(GetPinGuid(InPin));
		Info.BagInfo = MoveTemp(PinMetaBag);
		return Info;
	}

	TOptional<FString> GetPinMeta(UEdGraphPin* InPin, FName Meta) const
	{
		check(InPin);
		if (auto* Find = PinExtraMetas.Find(GetPinGuid(InPin)))
			return Find->GetMeta(Meta);
		return {};
	}
	void SetPinMeta(UEdGraphPin* InPin, FName Meta, FString Val = TEXT(""))
	{
		check(InPin);
		auto& Info = PinExtraMetas.FindOrAdd(GetPinGuid(InPin));
		Info.SetMeta(Meta, Val);
	}
	TOptional<FString> ClearPinMeta(UEdGraphPin* InPin, FName Meta)
	{
		check(InPin);
		auto& Info = PinExtraMetas.FindOrAdd(GetPinGuid(InPin));
		return Info.ClearMeta(Meta);
	}

	virtual void Serialize(FArchive& Ar) override;

	TArray<UK2Neuron*> GetOtherNodesOfClass(TSubclassOf<UK2Neuron> NeuronClass = nullptr) const;
	bool VerifyNodeID(FCompilerResultsLog* MessageLog) const;

	virtual uint8 GetNodeUniqueID() const { return NodeUniqueID; }
	template<typename T>
	uint8 GenerateUniqueNodeID(TArray<T*>& Neurons, bool bCompact = true)
	{
		static_assert(TIsDerivedFrom<T, UK2Neuron>::IsDerived, "err");
		return GenerateUniqueNodeID(*reinterpret_cast<TArray<UK2Neuron*>*>(&Neurons), T::StaticClass(), bCompact);
	}
	FNodeTextCache CachedNodeTitle;

	static bool ShouldMarkBlueprintDirtyBeforeOpen();
};

#if WITH_EDITOR
class K2NEURON_API SGraphNeuronBase : public SGraphNodeK2Default
{
public:
	SLATE_BEGIN_ARGS(SGraphNeuronBase) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UK2Neuron* InNode);

protected:
	virtual bool ShouldShowResetButton() const { return true; }
	virtual bool ShouldShowCheckBox(const UEdGraphPin* PinObj, TAttribute<bool> Attr = nullptr) const { return false; }
	EVisibility GetCheckBoxVisibility(const UEdGraphPin* PinObj, TAttribute<bool> Attr = nullptr) const { return ShouldShowCheckBox(PinObj, Attr) ? EVisibility::Visible : EVisibility::Hidden; }
	virtual const FText& GetCheckBoxToolTipText(UEdGraphPin* PinObj) const { return FText::GetEmpty(); }
	virtual void AddPin(const TSharedRef<SGraphPin>& PinToAdd) override;
	EVisibility GetOutputPinVisibility(UEdGraphPin* PinObj) const;
	ECheckBoxState IsDefaultValueChecked(UEdGraphPin* PinObj) const;
	void OnDefaultValueCheckBoxChanged(ECheckBoxState InIsChecked, UEdGraphPin* PinObj);
};
#endif
