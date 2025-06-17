// Copyright Epic Games, Inc. All Rights Reserved.

#include "MessageTagContainer.h"
#include "HAL/IConsoleManager.h"
#include "UObject/CoreNet.h"
#include "UObject/UnrealType.h"
#include "Engine/PackageMapClient.h"
#include "UObject/Package.h"
#include "Engine/NetConnection.h"
#include "MessageTagsManager.h"
#include "MessageTagsModule.h"
#include "Misc/OutputDeviceNull.h"

// For dynamic serialization support
#if UE_WITH_IRIS && UE_5_05_OR_LATER
#include "Net/Core/NetToken/NetTokenExportContext.h"
#include "GameplayTagTokenStore.h"
#endif
#include <Net/Core/Trace/NetTrace.h>

const FMessageTag FMessageTag::EmptyTag;
const FMessageTagContainer FMessageTagContainer::EmptyContainer;

DEFINE_STAT(STAT_FMessageTagContainer_DoesTagContainerMatch);

DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::RemoveTagByExplicitName"), STAT_FMessageTagContainer_RemoveTagByExplicitName, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::FillParentTags"), STAT_FMessageTagContainer_FillParentTags, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::GetMessageTagParents"), STAT_FMessageTagContainer_GetMessageTagParents, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::Filter"), STAT_FMessageTagContainer_Filter, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::AppendTags"), STAT_FMessageTagContainer_AppendTags, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::AppendMatchingTags"), STAT_FMessageTagContainer_AppendMatchingTags, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::AddTag"), STAT_FMessageTagContainer_AddTag, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::RemoveTag"), STAT_FMessageTagContainer_RemoveTag, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTagContainer::RemoveTags"), STAT_FMessageTagContainer_RemoveTags, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTag::GetSingleTagContainer"), STAT_FMessageTag_GetSingleTagContainer, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTag::MatchesTag"), STAT_FMessageTag_MatchesTag, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTag::MatchesAny"), STAT_FMessageTag_MatchesAny, STATGROUP_MessageTags);
DECLARE_CYCLE_STAT(TEXT("FMessageTag::NetSerialize"), STAT_FMessageTag_NetSerialize, STATGROUP_MessageTags);

static bool GEnableMessageTagDetailedStats = false;
static FAutoConsoleVariableRef CVarMessageTagDetailedStats(TEXT("MessageTags.EnableDetailedStats"), GEnableMessageTagDetailedStats, TEXT("Runtime toggle for verbose CPU profiling stats"), ECVF_Default);

static bool GOldReplaysUseMessageTagFastReplication = true;
static FAutoConsoleVariableRef CVarOldReplaysUseMessageTagFastReplication(TEXT("MessageTags.OldReplaysUseFastReplication"), GOldReplaysUseMessageTagFastReplication, TEXT("When loading an outdated replay (before dynamic replication), do we assume it used fast replication?"), ECVF_Default);

/**
 *	Replicates a tag in a packed format:
 *	-A segment of NetIndexFirstBitSegment bits are always replicated.
 *	-Another bit is replicated to indicate "more"
 *	-If "more", then another segment of (MaxBits - NetIndexFirstBitSegment) length is replicated.
 *	
 *	This format is basically the same as SerializeIntPacked, except that there are only 2 segments and they are not the same size.
 *	The message tag system is able to exploit knoweledge in what tags are frequently replicated to ensure they appear in the first segment.
 *	Making frequently replicated tags as cheap as possible. 
 *	
 *	
 *	Setting up your project to take advantage of the packed format.
 *	-Run a normal networked game on non shipping build. 
 *	-After some time, run console command "MessageTags.PrintReport" or set "MessageTags.PrintReportOnShutdown 1" cvar.
 *	-This will generate information on the server log about what tags replicate most frequently.
 *	-Take this list and put it in DefaultMessageTags.ini.
 *	-CommonlyReplicatedTags is the ordered list of tags.
 *	-NetIndexFirstBitSegment is the number of bits (not including the "more" bit) for the first segment.
 *
 */
void SerializeMessageTagNetIndexPacked(FArchive& Ar, FMessageTagNetIndex& Value, const int32 NetIndexFirstBitSegment, const int32 MaxBits)
{
	// Case where we have no segment or the segment is larger than max bits
	if (NetIndexFirstBitSegment <= 0 || NetIndexFirstBitSegment >= MaxBits)
	{
		if (Ar.IsLoading())
		{
			Value = 0;
		}
		Ar.SerializeBits(&Value, MaxBits);
		return;
	}


	const uint32 BitMasks[] = {0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff, 0x1fff, 0x3fff, 0x7fff, 0xffff};
	const uint32 MoreBits[] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000};

	const int32 FirstSegment = NetIndexFirstBitSegment;
	const int32 SecondSegment = MaxBits - NetIndexFirstBitSegment;

	if (Ar.IsSaving())
	{
		uint32 Mask = BitMasks[FirstSegment];
		if (Value > Mask)
		{
			uint32 FirstDataSegment = ((Value & Mask) | MoreBits[FirstSegment+1]);
			uint32 SecondDataSegment = (Value >> FirstSegment);

			uint32 SerializedValue = FirstDataSegment | (SecondDataSegment << (FirstSegment+1));				

			Ar.SerializeBits(&SerializedValue, MaxBits + 1);
		}
		else
		{
			uint32 SerializedValue = Value;
			Ar.SerializeBits(&SerializedValue, NetIndexFirstBitSegment + 1);
		}

	}
	else
	{
		uint32 FirstData = 0;
		Ar.SerializeBits(&FirstData, FirstSegment + 1);
		uint32 More = FirstData & MoreBits[FirstSegment+1];
		if (More)
		{
			uint32 SecondData = 0;
			Ar.SerializeBits(&SecondData, SecondSegment);
			Value = IntCastChecked<uint16, uint32>(SecondData << FirstSegment);
			Value |= (FirstData & BitMasks[FirstSegment]);
		}
		else
		{
			Value = IntCastChecked<uint16, uint32>(FirstData);
		}

	}
}


FMessageTagContainer& FMessageTagContainer::operator=(FMessageTagContainer const& Other)
{
	// Guard against self-assignment
	if (this == &Other)
	{
		return *this;
	}
	MessageTags.Empty(Other.MessageTags.Num());
	MessageTags.Append(Other.MessageTags);

	ParentTags.Empty(Other.ParentTags.Num());
	ParentTags.Append(Other.ParentTags);

	return *this;
}

FMessageTagContainer& FMessageTagContainer::operator=(FMessageTagContainer&& Other)
{
	MessageTags = MoveTemp(Other.MessageTags);
	ParentTags = MoveTemp(Other.ParentTags);
	return *this;
}

bool FMessageTagContainer::operator==(FMessageTagContainer const& Other) const
{
	// This is to handle the case where the two containers are in different orders
	if (MessageTags.Num() != Other.MessageTags.Num())
	{
		return false;
	}

	return HasAllExact(Other);
}

bool FMessageTagContainer::operator!=(FMessageTagContainer const& Other) const
{
	return !operator==(Other);
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS

bool FMessageTagContainer::ComplexHasTag(FMessageTag const& TagToCheck, TEnumAsByte<EMessageTagMatchType::Type> TagMatchType, TEnumAsByte<EMessageTagMatchType::Type> TagToCheckMatchType) const
{
	check(TagMatchType != EMessageTagMatchType::Explicit || TagToCheckMatchType != EMessageTagMatchType::Explicit);

	if (TagMatchType == EMessageTagMatchType::IncludeParentTags)
	{
		FMessageTagContainer ExpandedConatiner = GetMessageTagParents();
		return ExpandedConatiner.HasTagFast(TagToCheck, EMessageTagMatchType::Explicit, TagToCheckMatchType);
	}
	else
	{
		return TagToCheck.GetSingleTagContainer().DoesTagContainerMatch(*this, EMessageTagMatchType::IncludeParentTags, EMessageTagMatchType::Explicit, EMessageContainerMatchType::Any);
	}
}

PRAGMA_ENABLE_DEPRECATION_WARNINGS

bool FMessageTagContainer::RemoveTagByExplicitName(const FName& TagName)
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_RemoveTagByExplicitName);

	// TODO NDarnell Why are we doing this instead of just return RemoveTag(FMessageTag(TagName));
	for (auto MessageTag : this->MessageTags)
	{
		if (MessageTag.GetTagName() == TagName)
		{
			RemoveTag(MessageTag);
			return true;
		}
	}

	return false;
}

FORCEINLINE_DEBUGGABLE void FMessageTagContainer::AddParentsForTag(const FMessageTag& Tag)
{
	UMessageTagsManager::Get().ExtractParentTags(Tag, ParentTags);
}

void FMessageTagContainer::FillParentTags()
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_FillParentTags);

	ParentTags.Reset();

	if (MessageTags.Num() > 0)
	{
		UMessageTagsManager& TagManager = UMessageTagsManager::Get();
		for (const FMessageTag& Tag : MessageTags)
		{
			TagManager.ExtractParentTags(Tag, ParentTags);
		}
	}
}

FMessageTagContainer FMessageTagContainer::GetMessageTagParents() const
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_GetMessageTagParents);

	FMessageTagContainer ResultContainer;
	ResultContainer.MessageTags = MessageTags;

	// Add parent tags to explicit tags, the rest got copied over already
	for (const FMessageTag& Tag : ParentTags)
	{
		ResultContainer.MessageTags.AddUnique(Tag);
	}

	return ResultContainer;
}

FMessageTagContainer FMessageTagContainer::Filter(const FMessageTagContainer& OtherContainer) const
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_Filter);

	FMessageTagContainer ResultContainer;

	for (const FMessageTag& Tag : MessageTags)
	{
		if (Tag.MatchesAny(OtherContainer))
		{
			ResultContainer.AddTagFast(Tag);
		}
	}

	return ResultContainer;
}

FMessageTagContainer FMessageTagContainer::FilterExact(const FMessageTagContainer& OtherContainer) const
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_Filter);

	FMessageTagContainer ResultContainer;

	for (const FMessageTag& Tag : MessageTags)
	{
		if (Tag.MatchesAnyExact(OtherContainer))
		{
			ResultContainer.AddTagFast(Tag);
		}
	}

	return ResultContainer;
}

void FMessageTagContainer::AppendTags(FMessageTagContainer const& Other)
{
	CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_AppendTags, GEnableMessageTagDetailedStats);
	if (Other.IsEmpty())
	{
		return;
	}

	int32 OldTagNum = MessageTags.Num();
	MessageTags.Reserve(OldTagNum + Other.MessageTags.Num());
	// Add other container's tags to our own
	for(const FMessageTag& OtherTag : Other.MessageTags)
	{
		int32 SearchIndex = 0;
		while (true)
		{
			if (SearchIndex >= OldTagNum)
			{
				// Stop searching once we've looked at all existing tags, this is faster when appending large containers
				MessageTags.Add(OtherTag);
				break;
			}
			else if (MessageTags[SearchIndex] == OtherTag)
			{
				// Matching tag found, stop searching
				break;
			}

			SearchIndex++;
		}
	}

	// This function is called enough that the code duplication is faster than a lambda
	OldTagNum = ParentTags.Num();
	ParentTags.Reserve(OldTagNum + Other.ParentTags.Num());
	for (const FMessageTag& OtherTag : Other.ParentTags)
	{
		int32 SearchIndex = 0;
		while (true)
		{
			if (SearchIndex >= OldTagNum)
			{
				ParentTags.Add(OtherTag);
				break;
			}
			else if (ParentTags[SearchIndex] == OtherTag)
			{
				break;
			}

			SearchIndex++;
		}
	}
}

void FMessageTagContainer::AppendMatchingTags(FMessageTagContainer const& OtherA, FMessageTagContainer const& OtherB)
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_AppendMatchingTags);

	for(const FMessageTag& OtherATag : OtherA.MessageTags)
	{
		if (OtherATag.MatchesAny(OtherB))
		{
			AddTag(OtherATag);
		}
	}
}

void FMessageTagContainer::AddTag(const FMessageTag& TagToAdd)
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_AddTag);

	if (TagToAdd.IsValid())
	{
		// Don't want duplicate tags
		MessageTags.AddUnique(TagToAdd);

		AddParentsForTag(TagToAdd);
	}
}

void FMessageTagContainer::AddTagFast(const FMessageTag& TagToAdd)
{
	MessageTags.Add(TagToAdd);
	AddParentsForTag(TagToAdd);
}

bool FMessageTagContainer::AddLeafTag(const FMessageTag& TagToAdd)
{
	// Check tag is not already explicitly in container
	if (HasTagExact(TagToAdd))
	{
		return true;
	}

	// If this tag is parent of explicitly added tag, fail
	if (HasTag(TagToAdd))
	{
		return false;
	}

	TSharedPtr<FMessageTagNode> TagNode = UMessageTagsManager::Get().FindTagNode(TagToAdd);

	if (ensureMsgf(TagNode.IsValid(), TEXT("AddLeafTag passed invalid Message tag %s, only registered tags can be queried"), *TagToAdd.GetTagName().ToString()))
	{
		// Remove any tags in the container that are a parent to TagToAdd
		for (const FMessageTag& ParentTag : TagNode->GetSingleTagContainer().ParentTags)
		{
			if (HasTagExact(ParentTag))
			{
				RemoveTag(ParentTag);
			}
		}
	}

	// Add the tag
	AddTag(TagToAdd);
	return true;
}

bool FMessageTagContainer::RemoveTag(const FMessageTag& TagToRemove, bool bDeferParentTags)
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_RemoveTag);

	int32 NumChanged = MessageTags.RemoveSingle(TagToRemove);

	if (NumChanged > 0)
	{
		if (!bDeferParentTags)
		{
			// Have to recompute parent table from scratch because there could be duplicates providing the same parent tag
			FillParentTags();
		}
		return true;
	}
	return false;
}

void FMessageTagContainer::RemoveTags(const FMessageTagContainer& TagsToRemove)
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTagContainer_RemoveTags);

	int32 NumChanged = 0;

	for (auto Tag : TagsToRemove)
	{
		NumChanged += MessageTags.RemoveSingle(Tag);
	}

	if (NumChanged > 0)
	{
		// Recompute once at the end
		FillParentTags();
	}
}

void FMessageTagContainer::Reset(int32 Slack)
{
	MessageTags.Reset(Slack);

	// ParentTags is usually around size of MessageTags on average
	ParentTags.Reset(Slack);
}

bool FMessageTagContainer::Serialize(FStructuredArchive::FSlot Slot)
{
	FArchive& UnderlyingArchive = Slot.GetUnderlyingArchive();

	const bool bOldTagVer = UnderlyingArchive.UEVer() < VER_UE4_GAMEPLAY_TAG_CONTAINER_TAG_TYPE_CHANGE;

	if (bOldTagVer)
	{
		TArray<FName> Tags_DEPRECATED;
		Slot << Tags_DEPRECATED;
		// Too old to deal with
		UE_LOG(LogMessageTags, Error, TEXT("Failed to load old MessageTag container, too old to migrate correctly"));
	}
	else
	{
		Slot << MessageTags;
	}
	
	// Only do redirects for real loads, not for duplicates or recompiles
	if (UnderlyingArchive.IsLoading() )
	{
		if (UnderlyingArchive.IsPersistent() && !(UnderlyingArchive.GetPortFlags() & PPF_Duplicate) && !(UnderlyingArchive.GetPortFlags() & PPF_DuplicateForPIE))
		{
			// Rename any tags that may have changed by the ini file.  Redirects can happen regardless of version.
			// Regardless of version, want loading to have a chance to handle redirects
			UMessageTagsManager::Get().MessageTagContainerLoaded(*this, UnderlyingArchive.GetSerializedProperty());
		}

		FillParentTags();
	}

	if (UnderlyingArchive.IsSaving())
	{
		// This marks the saved name for later searching
		for (const FMessageTag& Tag : MessageTags)
		{
			UnderlyingArchive.MarkSearchableName(FMessageTag::StaticStruct(), Tag.TagName);
		}

#if WITH_EDITOR && UE_5_06_OR_LATER
		if (UnderlyingArchive.IsCooking())
		{
			check(UnderlyingArchive.GetSavePackageData());
			FObjectSavePackageSerializeContext& SaveContext = UnderlyingArchive.GetSavePackageData()->SavePackageContext;
			if (SaveContext.IsHarvestingCookDependencies())
			{
				SaveContext.AddCookLoadDependency(UMessageTagsManager::Get().CreateCookDependency());
			}
		}
#endif
	}

	return true;
}

FString FMessageTagContainer::ToString() const
{
	FString ExportString;
	FMessageTagContainer::StaticStruct()->ExportText(ExportString, this, this, nullptr, 0, nullptr);

	return ExportString;
}

void FMessageTagContainer::FromExportString(const FString& ExportString, int32 Flags)
{
	Reset();

	FOutputDeviceNull NullOut;
	FMessageTagContainer::StaticStruct()->ImportText(*ExportString, this, nullptr, Flags, &NullOut, TEXT("FMessageTagContainer"), true);
}

bool FMessageTagContainer::ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText)
{
	// Call default import, but skip the native callback to avoid recursion
	Buffer = FMessageTagContainer::StaticStruct()->ImportText(Buffer, this, Parent, PortFlags, ErrorText, TEXT("FMessageTagContainer"), false);

	if (Buffer)
	{
		// Clear out any invalid tags that got stripped
		MessageTags.Remove(FMessageTag());

		// Compute parent tags
		FillParentTags();	
	}
	return true;
}

void FMessageTagContainer::PostScriptConstruct()
{
	FillParentTags();
}

FString FMessageTagContainer::ToStringSimple(bool bQuoted) const
{
	FString RetString;
	for (int i = 0; i < MessageTags.Num(); ++i)
	{
		if (bQuoted)
		{
			RetString += TEXT("\"");
		}
		RetString += MessageTags[i].ToString();
		if (bQuoted)
		{
			RetString += TEXT("\"");
		}
		
		if (i < MessageTags.Num() - 1)
		{
			RetString += TEXT(", ");
		}
	}
	return RetString;
}

TArray<FString> FMessageTagContainer::ToStringsMaxLen(int32 MaxLen) const
{
	// caveat, if MaxLen < than a tag string, full string will be put in array (as a single line in the array)
	// since this is used for debug output.  If need to clamp, it can be added.  Also, strings will end in ", " to 
	// avoid extra complication.
	TArray<FString> RetStrings;
	FString CurLine;
	CurLine.Reserve(MaxLen);
	for (int32 i = 0; i < MessageTags.Num(); ++i)
	{
		FString TagString = MessageTags[i].ToString();
		if (i < MessageTags.Num() - 1)
		{
			TagString += TEXT(",");
		}
		// Add 1 for space
		if (CurLine.Len() + TagString.Len() + 1 >= MaxLen)
		{
			RetStrings.Add(CurLine);
			CurLine = TagString;
		} 
		else
		{
			CurLine += TagString + TEXT(" ");
		}
	}
	if (CurLine.Len() > 0)
	{
		RetStrings.Add(CurLine);
	}
	return RetStrings;
}

bool FMessageTagContainer::NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
{
	// 1st bit to indicate empty tag container or not (empty tag containers are frequently replicated). Early out if empty.
	uint8 IsEmpty = MessageTags.Num() == 0;
	Ar.SerializeBits(&IsEmpty, 1);
	if (IsEmpty)
	{
		if (MessageTags.Num() > 0)
		{
			Reset();
		}
		bOutSuccess = true;
		return true;
	}

	// -------------------------------------------------------

	int32 NumBitsForContainerSize = UMessageTagsManager::Get().NumBitsForContainerSize;

	if (Ar.IsSaving())
	{
		uint8 NumTags = MessageTags.Num();
		uint8 MaxSize = (1 << NumBitsForContainerSize) - 1;
		if (!ensureMsgf(NumTags <= MaxSize, TEXT("TagContainer has %d elements when max is %d! Tags: %s"), NumTags, MaxSize, *ToStringSimple()))
		{
			NumTags = MaxSize;
		}
		
		Ar.SerializeBits(&NumTags, NumBitsForContainerSize);
		for (int32 idx=0; idx < NumTags;++idx)
		{
			FMessageTag& Tag = MessageTags[idx];
			Tag.NetSerialize_Packed(Ar, Map, bOutSuccess);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			UMessageTagsManager::Get().NotifyTagReplicated(Tag, true);
#endif
		}
	}
	else
	{
		// No Common Container tags, just replicate this like normal
		uint8 NumTags = 0;
		Ar.SerializeBits(&NumTags, NumBitsForContainerSize);

		MessageTags.Empty(NumTags);
		MessageTags.AddDefaulted(NumTags);
		for (uint8 idx = 0; idx < NumTags; ++idx)
		{
			MessageTags[idx].NetSerialize_Packed(Ar, Map, bOutSuccess);
		}
		FillParentTags();
	}


	bOutSuccess  = true;
	return true;
}

FText FMessageTagContainer::ToMatchingText(EMessageContainerMatchType MatchType, bool bInvertCondition) const
{
	enum class EMatchingTypes : int8
	{
		Inverted	= 0x01,
		All			= 0x02
	};

#define LOCTEXT_NAMESPACE "FMessageTagContainer"
	const FText MatchingDescription[] =
	{
		LOCTEXT("MatchesAnyMessageTags", "Has any tags in set: {MessageTagSet}"),
		LOCTEXT("NotMatchesAnyMessageTags", "Does not have any tags in set: {MessageTagSet}"),
		LOCTEXT("MatchesAllMessageTags", "Has all tags in set: {MessageTagSet}"),
		LOCTEXT("NotMatchesAllMessageTags", "Does not have all tags in set: {MessageTagSet}")
	};
#undef LOCTEXT_NAMESPACE

	int32 DescriptionIndex = bInvertCondition ? static_cast<int32>(EMatchingTypes::Inverted) : 0;
	switch (MatchType)
	{
		case EMessageContainerMatchType::All:
			DescriptionIndex |= static_cast<int32>(EMatchingTypes::All);
			break;

		case EMessageContainerMatchType::Any:
			break;

		default:
			UE_LOG(LogMessageTags, Warning, TEXT("Invalid value for TagsToMatch (EMessageContainerMatchType) %d.  Should only be Any or All."), static_cast<int32>(MatchType));
			break;
	}

	FFormatNamedArguments Arguments;
	Arguments.Add(TEXT("MessageTagSet"), FText::FromString(*ToString()));
	return FText::Format(MatchingDescription[DescriptionIndex], Arguments);
}

const FMessageTagContainer& FMessageTag::GetSingleTagContainer() const
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTag_GetSingleTagContainer);

	auto TagNode = UMessageTagsManager::Get().FindTagNode(*this);

	if (TagNode.IsValid())
	{
		return TagNode->GetSingleTagContainer();
	}

	// This tag should always be invalid if the node is missing
	ensureMsgf(!IsValid(), TEXT("GetSingleTagContainer passed invalid message tag %s, only registered tags can be queried"), *GetTagName().ToString());

	return FMessageTagContainer::EmptyContainer;
}

FMessageTag FMessageTag::RequestMessageTag(const FName& TagName, bool ErrorIfNotFound)
{
	return UMessageTagsManager::Get().RequestMessageTag(TagName, ErrorIfNotFound);
}

bool FMessageTag::IsValidMessageTagString(const FString& TagString, FText* OutError, FString* OutFixedString)
{
	return UMessageTagsManager::Get().IsValidMessageTagString(TagString, OutError, OutFixedString);
}

FMessageTagContainer FMessageTag::GetMessageTagParents() const
{
	return UMessageTagsManager::Get().RequestMessageTagParents(*this);
}

void FMessageTag::ParseParentTags(TArray<FMessageTag>& UniqueParentTags) const
{
	// This needs to be in the same order as the message tag node ParentTags, which is immediate parent first
	FName RawTag = GetTagName();
	TCHAR TagBuffer[FName::StringBufferSize];
	RawTag.ToString(TagBuffer);
	FStringView TagView = TagBuffer;

	int32 DotIndex;
	TagView.FindLastChar(TEXT('.'), DotIndex);

	while (DotIndex != INDEX_NONE)
	{
		// Remove everything starting with the last dot
		TagView.LeftInline(DotIndex);
		DotIndex = TagView.FindLastChar(TEXT('.'), DotIndex);

		// Add the name to the array
		FMessageTag ParentTag = FMessageTag(FName(TagView));

		UniqueParentTags.AddUnique(MoveTemp(ParentTag));
	}
}

FName FMessageTag::GetTagLeafName() const
{
	FName RawTag = GetTagName();
	if (RawTag.IsNone())
	{
		return RawTag;
	}

	const TStringBuilder<FName::StringBufferSize> TagBuffer(InPlace, RawTag);
	FStringView TagView = TagBuffer.ToView();
	const int32 DotIndex = UE::String::FindLastChar(TagView, TEXT('.'));
	if (DotIndex == INDEX_NONE)
	{
		return RawTag;
	}

	TagView.RightChopInline(DotIndex + 1);
	return FName(TagView);
}

bool FMessageTag::MatchesTag(const FMessageTag& TagToCheck) const
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTag_MatchesTag);

	auto TagNode = UMessageTagsManager::Get().FindTagNode(*this);

	if (TagNode.IsValid())
	{
		return TagNode->GetSingleTagContainer().HasTag(TagToCheck);
	}

	// If a non-empty tag has not been registered, it will not exist in the tag database so this function may return the incorrect value
	// All tags must be registered from code or data before being used in matching functions and this tag may have been deleted with active references
	ensureMsgf(!IsValid(), TEXT("MatchesTag passed invalid message tag %s, only registered tags can be used in containers"), *GetTagName().ToString());

	return false;
}

bool FMessageTag::MatchesAny(const FMessageTagContainer& ContainerToCheck) const
{
	SCOPE_CYCLE_COUNTER(STAT_FMessageTag_MatchesAny);

	auto TagNode = UMessageTagsManager::Get().FindTagNode(*this);

	if (TagNode.IsValid())
	{
		return TagNode->GetSingleTagContainer().HasAny(ContainerToCheck);
	}

	// If a non-empty tag has not been registered, it will not exist in the tag database so this function may return the incorrect value
	// All tags must be registered from code or data before being used in matching functions and this tag may have been deleted with active references
	ensureMsgf(!IsValid(), TEXT("MatchesAny passed invalid message tag %s, only registered tags can be used in containers"), *GetTagName().ToString());

	return false;
}

int32 FMessageTag::MatchesTagDepth(const FMessageTag& TagToCheck) const
{
	return UMessageTagsManager::Get().MessageTagsMatchDepth(*this, TagToCheck);
}

FMessageTag::FMessageTag(const FName& Name)
	: TagName(Name)
{
	// This constructor is used to bypass the table check and is only usable by MessageTagManager
}

bool FMessageTag::SerializeFromMismatchedTag(const FPropertyTag& Tag, FStructuredArchive::FSlot Slot)
{
	if (Tag.Type == NAME_NameProperty)
	{
		Slot << TagName;
		return true;
	}
	return false;
}

FMessageTag FMessageTag::RequestDirectParent() const
{
	return UMessageTagsManager::Get().RequestMessageTagDirectParent(*this);
}

bool FMessageTag::NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (Ar.IsSaving())
	{
		UMessageTagsManager::Get().NotifyTagReplicated(*this, false);
	}
#endif

	NetSerialize_Packed(Ar, Map, bOutSuccess);

	bOutSuccess = true;
	return true;
}

static TSharedPtr<FNetFieldExportGroup> CreateNetfieldExportGroupForNetworkMessageTags(const UMessageTagsManager& TagManager, const TCHAR* NetFieldExportGroupName)
{
	TSharedPtr<FNetFieldExportGroup> NetFieldExportGroup = TSharedPtr<FNetFieldExportGroup>(new FNetFieldExportGroup());

	const TArray<TSharedPtr<FMessageTagNode>>& NetworkMessageTagNodeIndex = TagManager.GetNetworkMessageTagNodeIndex();

	NetFieldExportGroup->PathName = NetFieldExportGroupName;
	NetFieldExportGroup->NetFieldExports.SetNum(NetworkMessageTagNodeIndex.Num());

	for (int32 i = 0; i < NetworkMessageTagNodeIndex.Num(); i++)
	{
		FNetFieldExport NetFieldExport(
			i,
			0,
			NetworkMessageTagNodeIndex[i]->GetCompleteTagName());

		NetFieldExportGroup->NetFieldExports[i] = NetFieldExport;
	}

	return NetFieldExportGroup;
}

bool FMessageTag::NetSerialize_ForReplayUsingFastReplication(FArchive& Ar, UPackageMapClient& PackageMapClient)
{
	UMessageTagsManager& TagManager = UMessageTagsManager::Get();
	FMessageTagNetIndex NetIndex = INVALID_TAGNETINDEX;

	// For replays, use a net field export group to guarantee we can send the name reliably (without having to rely on the client having a deterministic NetworkMessageTagNodeIndex array)
	const TCHAR* NetFieldExportGroupName = TEXT("NetworkMessageTagNodeIndex");

	// Find this net field export group
	TSharedPtr<FNetFieldExportGroup> NetFieldExportGroup = PackageMapClient.GetNetFieldExportGroup(NetFieldExportGroupName);

	if (Ar.IsSaving())
	{
		// If we didn't find it, we need to create it (only when saving though, it should be here on load since it was exported at save time)
		if (!NetFieldExportGroup.IsValid())
		{
			NetFieldExportGroup = CreateNetfieldExportGroupForNetworkMessageTags(TagManager, NetFieldExportGroupName);
			PackageMapClient.AddNetFieldExportGroup(NetFieldExportGroupName, NetFieldExportGroup);
		}

		NetIndex = TagManager.GetNetIndexFromTag(*this);

		if (NetIndex != TagManager.GetInvalidTagNetIndex() && NetIndex != INVALID_TAGNETINDEX)
		{
			PackageMapClient.TrackNetFieldExport(NetFieldExportGroup.Get(), NetIndex);
		}
		else
		{
			NetIndex = INVALID_TAGNETINDEX;		// We can't save InvalidTagNetIndex, since the remote side could have a different value for this
		}
	}

	uint32 NetIndex32 = NetIndex;
	Ar.SerializeIntPacked(NetIndex32);
	NetIndex = IntCastChecked<uint16, uint32>(NetIndex32);

	if (Ar.IsLoading())
	{
		// Get the tag name from the net field export group entry
		if (NetIndex != INVALID_TAGNETINDEX && ensure(NetFieldExportGroup.IsValid()) && ensure(NetIndex < NetFieldExportGroup->NetFieldExports.Num()))
		{
			TagName = NetFieldExportGroup->NetFieldExports[NetIndex].ExportName;

			// Validate the tag name
			const FMessageTag Tag = TagManager.RequestMessageTag(TagName, false);

			// Warn (once) if the tag isn't found
			if (!Tag.IsValid() && !NetFieldExportGroup->NetFieldExports[NetIndex].bIncompatible)
			{ 
				UE_LOG(LogMessageTags, Warning, TEXT( "Message tag not found (marking incompatible): %s"), *TagName.ToString());
				NetFieldExportGroup->NetFieldExports[NetIndex].bIncompatible = true;
			}

			TagName = Tag.TagName;
		}
		else
		{
			TagName = NAME_None;
		}
	}

	return true;
}

// DynamicSerialization currently relies on experimental code only available when compiling with Iris.
namespace UE::MessageTags::MessageTagDynamicSerialization
{
#if UE_WITH_IRIS && UE_5_05_OR_LATER
	// Can we make this generic and handle arbitrary export payloads? Probably something that should be handled in PackageMapClient
	static bool NetSerialize_ForReplay(FMessageTag& MessageTag, FArchive& Ar, UPackageMapClient& PackageMapClient)
	{
		using namespace UE::Net;

		const FNetTokenResolveContext* NetTokenResolveContext = PackageMapClient.GetNetTokenResolveContext();
		FMessageTagTokenStore* TagTokenDataStore = NetTokenResolveContext ? NetTokenResolveContext->NetTokenStore->GetDataStore<UE::Net::FMessageTagTokenStore>() : nullptr;
		if (!TagTokenDataStore)
		{
			return false;
		}

		const TCHAR* NetFieldExportGroupName = TEXT("NetworkMessageTagDynamicIndex");
		TSharedPtr<FNetFieldExportGroup> NetFieldExportGroup = PackageMapClient.GetNetFieldExportGroup(NetFieldExportGroupName);
		FNetToken TagToken;

		if (Ar.IsSaving())
		{
			TagToken = TagTokenDataStore->GetOrCreateToken(MessageTag);

			// Write token
			// Important: As we write it directly through the TagTokenStore we must read it in the same way as we skip serializing the type.
			TagTokenDataStore->WriteNetToken(Ar, TagToken);

			// Register replay export if needed
			if (TagToken.IsValid())
			{
				// If we didn't find it, we need to create it (only when saving though, it should be here on load since it was exported at save time)
				if (!NetFieldExportGroup.IsValid())
				{
					NetFieldExportGroup = TSharedPtr<FNetFieldExportGroup>(new FNetFieldExportGroup());
					NetFieldExportGroup->PathName = NetFieldExportGroupName;
					PackageMapClient.AddNetFieldExportGroup(NetFieldExportGroupName, NetFieldExportGroup);
				}

				// Make sure we have enough room in the NetFieldExports to hold this entry...
				const uint32 TagTokenIndex = TagToken.GetIndex();
				if (!NetFieldExportGroup->NetFieldExports.IsValidIndex(TagTokenIndex))
				{
					NetFieldExportGroup->NetFieldExports.SetNum(TagTokenIndex + 1, EAllowShrinking::No);
					NetFieldExportGroup->bDirtyForReplay = true;
				}
				ensure(NetFieldExportGroup->NetFieldExports.IsValidIndex(TagTokenIndex));
				FNetFieldExport& NetFieldExport = NetFieldExportGroup->NetFieldExports[TagTokenIndex];

				// If it's not yet exported, export it now
				if (!NetFieldExport.bExported)
				{
					NetFieldExport = FNetFieldExport(TagTokenIndex, 0, MessageTag.GetTagName());
					UE_LOG(LogMessageTags, Verbose, TEXT("Replay> Exported Tag %s as NetFieldIndex %u"), *NetFieldExport.ExportName.ToString(), TagTokenIndex);
				}

				// Track the export so that it gets added to the replay index
				PackageMapClient.TrackNetFieldExport(NetFieldExportGroup.Get(), TagTokenIndex);
			}
			return true;
		}
		else if (Ar.IsLoading())
		{
			// Read TagNetToken
			TagToken = TagTokenDataStore->ReadNetToken(Ar);

			if (Ar.IsError())
			{
				return false;
			}

			if (TagToken.IsValid())
			{
				const uint32 TagTokenIndex = TagToken.GetIndex();
				if (ensure(NetFieldExportGroup.IsValid()) && ensure(TagTokenIndex < (uint32)NetFieldExportGroup->NetFieldExports.Num()))
				{
					FName TagName = NetFieldExportGroup->NetFieldExports[TagTokenIndex].ExportName;

					// Validate the tag name
					// TODO: Should we be able to add tags through this?
					UMessageTagsManager& TagManager = UMessageTagsManager::Get();
					MessageTag = TagManager.RequestMessageTag(TagName, false);

					// Warn (once) if the tag isn't found
					if (!MessageTag.IsValid() && !NetFieldExportGroup->NetFieldExports[TagTokenIndex].bIncompatible)
					{ 
						UE_LOG(LogMessageTags, Warning, TEXT( "Message tag not found (marking incompatible): %s"), *TagName.ToString());
						NetFieldExportGroup->NetFieldExports[TagTokenIndex].bIncompatible = true;
					}
					return true;
				}
				else
				{
					MessageTag = FMessageTag();
					return false;
				}
			}
			MessageTag = FMessageTag();
			return true;
		}
		return false;
	}

	bool NetSerialize(FMessageTag& MessageTag, FArchive& Ar, UPackageMap* Map)
	{
		using namespace UE::Net;

		// For now special case replays
		UPackageMapClient* PackageMapClient = Cast<UPackageMapClient>(Map);
		if (const bool bIsReplay = PackageMapClient && PackageMapClient->GetConnection() && PackageMapClient->GetConnection()->IsInternalAck())
		{
			return NetSerialize_ForReplay(MessageTag, Ar, *PackageMapClient);
		}
	
		if (Ar.IsSaving())
		{
			UE::Net::FNetTokenExportContext* ExportContext = FNetTokenExportContext::GetNetTokenExportContext(Ar);
			UE::Net::FNetTokenStore* NetTokenStore = ExportContext ? ExportContext->GetNetTokenStore() : nullptr;
			FMessageTagTokenStore* TagTokenStore = NetTokenStore ? NetTokenStore->GetDataStore<UE::Net::FMessageTagTokenStore>() : nullptr;
			if (ensure(TagTokenStore))
			{
				FNetToken TagToken;
				if (MessageTag.IsValid())
				{
					TagToken = TagTokenStore->GetOrCreateToken(MessageTag);
				}

				UE_NET_TRACE_DYNAMIC_NAME_SCOPE(*TagToken.ToString(), static_cast<FNetBitWriter&>(Ar), GetTraceCollector(static_cast<FNetBitWriter&>(Ar)), ENetTraceVerbosity::VeryVerbose);

				// Write NetToken, 
				// Important: As we write it directly thorugh the TagTokenStore we also need to read it in the samw way as we skip serializing the type.
				TagTokenStore->WriteNetToken(Ar, TagToken);

				// Add export
				ExportContext->AddNetTokenPendingExport(TagToken);

				return true;
			}
			else
			{
				UE_LOG(LogMessageTags, Error, TEXT("FMessageTag::NetSerialize::Could not find required FMessageTagTokenStore"));
				ensure(false);
			}
		}
		else if (Ar.IsLoading())
		{
			// When reading data we always have a PackageMap so we can get the necessary resolve context from here.
			const FNetTokenResolveContext* NetTokenResolveContext = Map ? Map->GetNetTokenResolveContext() : nullptr;
			FMessageTagTokenStore* TagTokenStore = NetTokenResolveContext ? NetTokenResolveContext->NetTokenStore->GetDataStore<UE::Net::FMessageTagTokenStore>() : nullptr;
			if (ensure(TagTokenStore))
			{
				// Read the TagToken using the TagTokenStore
				FNetToken TagToken = TagTokenStore->ReadNetToken(Ar);
				if (Ar.IsError())
				{
					return false;
				}

				// Resolve the TagToken
				MessageTag = TagTokenStore->ResolveToken(TagToken, NetTokenResolveContext->RemoteNetTokenStoreState);
				return true;
			}
			else
			{
				UE_LOG(LogMessageTags, Error, TEXT("FMessageTag::NetSerialize::Could not find required FMessageTagTokenStore"));	
				ensure(false);
				Ar.SetError();
			}
		}
		return false;
	}
#else
	bool NetSerialize(FMessageTag& MessageTag, FArchive& Ar, UPackageMap* Map)
	{
		LowLevelFatalError(TEXT("Cannot use dynamic serialization without compiling with Iris."));
		return false;
	}
#endif
}


bool FMessageTag::NetSerialize_Packed(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess)
{
	CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_FMessageTag_NetSerialize, GEnableMessageTagDetailedStats);
	UMessageTagsManager& TagManager = UMessageTagsManager::Get();

	// We now save off which method we're using (below)
	bool bUseFastReplication = TagManager.ShouldUseFastReplication();
	bool bUseDynamicReplication = TagManager.ShouldUseDynamicReplication();

	Ar.UsingCustomVersion(FEngineNetworkCustomVersion::Guid);
	#if UE_5_06_OR_LATER
	const bool bSerializeReplicationMethod = (Ar.EngineNetVer() >= FEngineNetworkCustomVersion::CustomExports);
	#else
	const bool bSerializeReplicationMethod = (Ar.EngineNetVer() >= 36);
	#endif
	if (bSerializeReplicationMethod)
	{
		Ar.SerializeBits(&bUseFastReplication, 1);
		if (!bUseFastReplication)
		{
			Ar.SerializeBits(&bUseDynamicReplication, 1);
		}
	}
	else
	{
		bUseFastReplication = GOldReplaysUseMessageTagFastReplication;
		bUseDynamicReplication = false; // this didn't exist in prior versions
	}

	if (bUseFastReplication)
	{
		UPackageMapClient* PackageMapClient = Cast<UPackageMapClient>(Map);

		const bool bIsReplay = PackageMapClient && PackageMapClient->GetConnection() && PackageMapClient->GetConnection()->IsInternalAck();
		if (bIsReplay)
		{
			return NetSerialize_ForReplayUsingFastReplication(Ar, *PackageMapClient);
		}

		FMessageTagNetIndex NetIndex = INVALID_TAGNETINDEX;
		if (Ar.IsSaving())
		{
			NetIndex = TagManager.GetNetIndexFromTag(*this);
			SerializeMessageTagNetIndexPacked(Ar, NetIndex, TagManager.GetNetIndexFirstBitSegment(), TagManager.GetNetIndexTrueBitNum());
		}
		else
		{
			SerializeMessageTagNetIndexPacked(Ar, NetIndex, TagManager.GetNetIndexFirstBitSegment(), TagManager.GetNetIndexTrueBitNum());
			TagName = TagManager.GetTagNameFromNetIndex(NetIndex);
		}
	}
	else if (bUseDynamicReplication)
	{
		UE::MessageTags::MessageTagDynamicSerialization::NetSerialize(*this, Ar, Map);
	}
	else
	{
		Ar << TagName;
	}

	bOutSuccess = true;
	return true;
}

void FMessageTag::PostSerialize(const FArchive& Ar)
{
	// This only happens for tags that are not nested inside a container, containers handle redirectors themselves
	// Only do redirects for real loads, not for duplicates or recompiles
	if (Ar.IsLoading() && Ar.IsPersistent() && !(Ar.GetPortFlags() & PPF_Duplicate) && !(Ar.GetPortFlags() & PPF_DuplicateForPIE))
	{
		// Rename any tags that may have changed by the ini file.
		UMessageTagsManager::Get().SingleMessageTagLoaded(*this, Ar.GetSerializedProperty());
	}

	if (Ar.IsSaving())
	{
		if (IsValid())
		{
			// This marks the saved name for later searching
			Ar.MarkSearchableName(FMessageTag::StaticStruct(), TagName);
		}
#if WITH_EDITOR && UE_5_06_OR_LATER
		if (Ar.IsCooking())
		{
			check(const_cast<FArchive&>(Ar).GetSavePackageData());
			FObjectSavePackageSerializeContext& SaveContext =
				const_cast<FArchive&>(Ar).GetSavePackageData()->SavePackageContext;
			if (SaveContext.IsHarvestingCookDependencies())
			{
				SaveContext.AddCookLoadDependency(UMessageTagsManager::Get().CreateCookDependency());
			}
		}
#endif
	}
}

bool FMessageTag::ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText)
{
	FString ImportedTag = TEXT("");
	const TCHAR* NewBuffer = FPropertyHelpers::ReadToken(Buffer, ImportedTag, true);
	if (!NewBuffer)
	{
		// Failed to read buffer. Maybe normal ImportText will work.
		return false;
	}
	
	const TCHAR* OriginalBuffer = Buffer;
	Buffer = NewBuffer;

	if (ImportedTag == TEXT("None") || ImportedTag.IsEmpty())
	{
		// TagName was none
		TagName = NAME_None;
		return true;
	}

	if (ImportedTag[0] == '(')
	{
		// Let normal ImportText handle this before handling fixups
		UScriptStruct* ScriptStruct = FMessageTag::StaticStruct();
		Buffer = ScriptStruct->ImportText(OriginalBuffer, this, Parent, PortFlags, ErrorText, ScriptStruct->GetName(), false);
		UMessageTagsManager::Get().ImportSingleMessageTag(*this, TagName, !!(PortFlags & PPF_SerializedAsImportText));
		return true;
	}

	return UMessageTagsManager::Get().ImportSingleMessageTag(*this, FName(*ImportedTag));
}

void FMessageTag::FromExportString(const FString& ExportString, int32 Flags)
{
	TagName = NAME_None;

	FOutputDeviceNull NullOut;
	FMessageTag::StaticStruct()->ImportText(*ExportString, this, nullptr, Flags, &NullOut, TEXT("FMessageTag"), true);
}

FMessageTagNativeAdder::FMessageTagNativeAdder()
{
	UMessageTagsManager::OnLastChanceToAddNativeTags().AddRaw(this, &FMessageTagNativeAdder::AddTags);
}
