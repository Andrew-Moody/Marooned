// Fill out your copyright notice in the Description page of Project Settings.


#include "WorldGen/MUResourceChunk.h"
#include "WorldGen/MUResourceInstanceComponent.h"
#include "DataAssets/ResourceDataAsset.h"
#include "AbilitySystem/MaroonedAbilitySystemComponent.h"
#include "AbilitySystem/MUResourceAttributeSet.h"
#include "AbilitySystem/MaroonedAttributeSet.h"
#include <GameplayEffectExtension.h>

#include "Item/ItemTypes.h"
#include "Item/MUItemSubsystem.h"
#include "Kismet/GameplayStatics.h"


AMUResourceChunk::AMUResourceChunk()
{
	PrimaryActorTick.bCanEverTick = false;

	// Set a Root Scene Component
	USceneComponent* SceneComp = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneComp);


	AbilitySystemComponent = CreateDefaultSubobject<UMaroonedAbilitySystemComponent>(TEXT("AbilitySystem"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);

	// Automatically calls GetAbilitySystemComponent to register itself with the ASC
	AttributeSet = CreateDefaultSubobject<UMUResourceAttributeSet>(TEXT("AttributeSet"));

	AttributeSet->OnDamageRecieved.BindUObject(this, &AMUResourceChunk::OnTakeDamage);

	// Too soon to call this here. Nor sure the best place since normally would be done
	// after possession of a pawn
	// May not be needed if the chunk does not activate any abilities
	//AbilitySystemComponent->InitAbilityActorInfo(this, this);



	// Unfortunately spawning components dynamically (Outside of constructor) makes replication difficult
	// HitResults sent via target data from client to server contained null references for the hit component

	// Add a fixed number of ResourceInstanceComponents
	// This allows the components to be static which simplifies replication and referencing

	ResourceInstanceComponents.Reset(NumResourceComponents);

	for (int32 i = 0; i < NumResourceComponents; ++i)
	{
		FString Name = "ResourceComponent_";
		Name.AppendInt(i);

		UMUResourceInstanceComponent* ResourceComp = CreateDefaultSubobject<UMUResourceInstanceComponent>(FName(*Name));

		ResourceComp->SetupAttachment(RootComponent);

		ResourceComp->ComponentIndex = i;

		ResourceComp->NumCustomDataFloats = 1;

		ResourceComp->SetCollisionProfileName(TEXT("Pawn"));

		ResourceInstanceComponents.Add(ResourceComp);
	}
}

void AMUResourceChunk::SpawnResourceGroup(int32 ComponentIndex, UResourceDataAsset* ResourceDataAsset, const TArray<FTransform>& Transforms)
{
	if (ComponentIndex >= NumResourceComponents)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("Exceeded max number of ResourceInstanceComponents"));
		return;
	}

	UMUResourceInstanceComponent* Comp = ResourceInstanceComponents[ComponentIndex];

	Comp->DataAsset = ResourceDataAsset;

	Comp->Populate(Transforms);
}

UAbilitySystemComponent* AMUResourceChunk::GetAbilitySystemComponent() const
{
	// It isn't strictly required to implement this interface but it avoids
	// GetAbilitySystemComponent having to fallback on a component wise search
	return AbilitySystemComponent;
}


void AMUResourceChunk::MulticastTakeDamage_Implementation(int32 InstanceIndex, int32 ComponentIndex, float Damage, FHitResult HitResult)
{
	if (GetNetMode() < ENetMode::NM_Client)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("MulticastTakeDamage called on Server"));
	}
	else
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("MulticastTakeDamage called on Remote Client"));
	}

	// Find the correct component either by sending a reference (if component references can be sent over network)
	// or by Index/Hash

	if (ComponentIndex > -1)
	{
		ResourceInstanceComponents[ComponentIndex]->TakeDamage(InstanceIndex, Damage, HitResult);
	}
}

void AMUResourceChunk::OnTakeDamage(const FGameplayEffectModCallbackData& Data)
{
	// Called on Server when damage is set by an effect
	// Extract required information and pass along to Clients

	if (GetNetMode() < ENetMode::NM_Client)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("OnTakeDamage called on Server"));
	}
	else
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("OnTakeDamage called on Remote Client"));
	}

	float Damage = Data.EvaluatedData.Magnitude;

	const FGameplayEffectContextHandle& EffectContext = Data.EffectSpec.GetEffectContext();

	const FHitResult* HitResult = EffectContext.GetHitResult();

	if (!HitResult)
	{
		// Targeting must produce a HitResult to determine which instance was damaged
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("Targeting did not produce a HitResult for Instanced Resource"));
		return;
	}

	int32 InstanceIndex = HitResult->Item;

	// Test that the hit component was a resource instance component
	UMUResourceInstanceComponent* ResourceComponent = Cast<UMUResourceInstanceComponent>(HitResult->GetComponent());

	if (!ResourceComponent)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("Hit Component was not ResourceInstance"));
		return;
	}

	// Check that the effect and instigator meet all the requirements to damage this resource
	if (!CheckRequirements(Data.EffectSpec, ResourceComponent->DataAsset))
	{
		Damage = 0.0f;
	}


	// Convert the reference into an index or hash
	// that can be used on Clients to identify the correct component
	int32 ComponentIndex = ResourceComponent->ComponentIndex;

	MulticastTakeDamage(InstanceIndex, ComponentIndex, Damage, *HitResult);
}

bool AMUResourceChunk::CheckRequirements(const FGameplayEffectSpec& EffectSpec, const UResourceDataAsset* ResourceData)
{
	// Check that the effect and instigator meet all the requirements to damage this resource

	const FGameplayTagContainer& SourceTags = EffectSpec.CapturedSourceTags.GetSpecTags();
	const FGameplayTagContainer& RequiredTags = ResourceData->RequiredTags;

	if (!SourceTags.HasAll(RequiredTags))
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to meet required tags"));
		return false;
	}

	UAbilitySystemComponent* InstigatorASC = EffectSpec.GetEffectContext().GetInstigatorAbilitySystemComponent();

	if (!InstigatorASC)
	{
		UE_LOG(LogTemp, Warning, TEXT("Instigator AbilitySystemComponent not found"));
		return false;
	}


	const UAttributeSet* AttSet = InstigatorASC->GetAttributeSet(UMaroonedAttributeSet::StaticClass());

	if (!AttSet)
	{
		UE_LOG(LogTemp, Warning, TEXT("GetAttributeSet Failed to find UMaroonedAttributeSet"));

		FString InstName = EffectSpec.GetEffectContext().GetInstigator()->GetFName().ToString();

		UE_LOG(LogTemp, Warning, TEXT("Instigator: %s"), *InstName);

		AttSet = InstigatorASC->GetAttributeSet(UAttributeSet::StaticClass());

		if (!AttSet)
		{
			UE_LOG(LogTemp, Warning, TEXT("GetAttributeSet Failed to find UAttributeSet"));
			return false;
		}
	}

	const UMaroonedAttributeSet* InstigatorAttSet = Cast<UMaroonedAttributeSet>(AttSet);

	if (!InstigatorAttSet)
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to cast to UMaroonedAttributeSet"));
		return false;
	}

	/*const UMaroonedAttributeSet* InstigatorAttSet = Cast<UMaroonedAttributeSet>(
		InstigatorASC->GetAttributeSet(UMaroonedAttributeSet::StaticClass()));

	if (!InstigatorAttSet)
	{
		UE_LOG(LogTemp, Warning, TEXT("Instigator AttributeSet not found"));
		return false;
	}*/

	if (InstigatorAttSet->GetToolStrength() < ResourceData->ToolStrength)
	{
		UE_LOG(LogTemp, Warning, TEXT("ToolStrength insufficient"));
		return false;
	}

	return true;
}


