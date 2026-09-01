#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace DediWithdrawFix {

struct Config {
    bool enabled = true;
    std::string command = "/dediwithdraw";
    int max_per_request = 10000;
    int cooldown_seconds = 1;
};

Config g_config;
std::unordered_map<int, std::chrono::steady_clock::time_point> g_cooldowns;

std::string PluginDir() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/DediWithdrawFix";
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string FullName(UObject* object) {
    if (!object) return {};
    FString value;
    object->GetFullName(&value, nullptr);
    return Lower(value.ToString());
}

void Send(AShooterPlayerController* pc, const std::string& message,
          const FLinearColor& color = FColorList::Green) {
    if (!pc) return;
    FString text(ArkApi::Tools::Utf8Decode(message).c_str());
    ArkApi::GetApiUtils().SendServerMessage(pc, color, *text);
}

void ReadConfig() {
    std::ifstream file(PluginDir() + "/config.json", std::ios::binary);
    if (!file) throw std::runtime_error("cannot open DediWithdrawFix/config.json");
    std::ostringstream stream;
    stream << file.rdbuf();
    const auto root = minijson::parse(stream.str());
    Config value;
    value.enabled = minijson::boolean(root, "General", "Enabled", value.enabled);
    value.command = minijson::str(root, "General", "Command", value.command);
    value.max_per_request = std::clamp(
        minijson::integer(root, "General", "MaxPerRequest", value.max_per_request), 1, 100000);
    value.cooldown_seconds = std::clamp(
        minijson::integer(root, "General", "CooldownSeconds", value.cooldown_seconds), 0, 30);
    g_config = value;
}

APrimalStructureItemContainer* AsOwnedSPlusDedi(AActor* actor, AShooterPlayerController* pc) {
    if (!actor || !pc) return nullptr;
    if (!actor->IsA(APrimalStructureItemContainer::GetPrivateStaticClass())) {
        actor = actor->OwnerField();
        if (!actor || !actor->IsA(APrimalStructureItemContainer::GetPrivateStaticClass())) return nullptr;
    }
    auto* container = static_cast<APrimalStructureItemContainer*>(actor);
    const std::string class_name = FullName(container->ClassField());
    if (class_name.find("bp_dedicatedstoragesp_c") == std::string::npos) return nullptr;
    if (container->TargetingTeamField() != ArkApi::GetApiUtils().GetTribeID(pc)) return nullptr;
    return container;
}

APrimalStructureItemContainer* AimedDedi(AShooterPlayerController* pc) {
    if (!pc) return nullptr;
    int body = 0;
    UActorComponent* component = nullptr;
    AActor* actor = pc->GetAimedUseActor(&component, &body, true);
    if (auto* result = AsOwnedSPlusDedi(actor, pc)) return result;
    if (component) if (auto* result = AsOwnedSPlusDedi(component->GetOwner(), pc)) return result;
    if (auto* result = AsOwnedSPlusDedi(pc->LastHeldUseActorField().Get(), pc)) return result;

    AShooterCharacter* character = pc->GetPlayerCharacter();
    if (character) {
        FHitResult hit{};
        component = nullptr;
        actor = character->GetAimedActor(
            ECollisionChannel::ECC_Visibility, &component, 1000.0f, 20.0f,
            &body, &hit, true, true, true);
        if (auto* result = AsOwnedSPlusDedi(actor, pc)) return result;
        if (auto* result = AsOwnedSPlusDedi(hit.GetActor(), pc)) return result;
        if (component) if (auto* result = AsOwnedSPlusDedi(component->GetOwner(), pc)) return result;
    }
    return nullptr;
}

template <typename T>
bool ReadProperty(UObject* object, const char* name, T* output) {
    if (!object || !output) return false;
    UProperty* property = object->FindProperty(FName(name, EFindName::FNAME_Find));
    if (!property || property->ElementSizeField() != static_cast<int>(sizeof(T))) return false;
    *output = *reinterpret_cast<T*>(
        reinterpret_cast<unsigned char*>(object) + property->Offset_InternalField());
    return true;
}

int PlayerQuantity(UPrimalInventoryComponent* inventory, UClass* item_class) {
    if (!inventory || !item_class) return 0;
    return inventory->GetItemTemplateQuantity(
        TSubclassOf<UPrimalItem>(item_class), nullptr, true, false, true, true);
}

int ChangeDediQuantity(UPrimalInventoryComponent* inventory, UClass* item_class, int amount) {
    if (!inventory || !item_class || amount == 0) return 0;
    return inventory->BPIncrementItemTemplateQuantity(
        TSubclassOf<UPrimalItem>(item_class), amount, true, false, true,
        false, false, false, nullptr, false);
}

int AddToPlayer(UPrimalInventoryComponent* inventory, UClass* item_class, int amount) {
    if (!inventory || !item_class || amount <= 0) return 0;
    const int before = PlayerQuantity(inventory, item_class);
    TSubclassOf<UPrimalItem> no_skin;
    no_skin.uClass = nullptr;
    UPrimalItem::AddNewItem(
        TSubclassOf<UPrimalItem>(item_class), inventory,
        false, false, 0.0f, true, amount, false, 0.0f, false,
        no_skin, 0.0f, false, false);
    return std::clamp(PlayerQuantity(inventory, item_class) - before, 0, amount);
}

void CommandImpl(AShooterPlayerController* pc, FString* raw) {
    if (!g_config.enabled || !pc || !raw || ArkApi::IApiUtils::IsPlayerDead(pc)) return;

    std::istringstream input(raw->ToString());
    std::string command;
    long long requested_raw = 0;
    input >> command >> requested_raw;
    if (requested_raw <= 0) {
        Send(pc, "Usage: /dediwithdraw <amount>", FColorList::Yellow);
        return;
    }

    const int tribe = ArkApi::GetApiUtils().GetTribeID(pc);
    if (tribe < 50000) {
        Send(pc, "You must be in a tribe.", FColorList::Red);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto cooldown = g_cooldowns.find(tribe);
    if (cooldown != g_cooldowns.end() && now < cooldown->second) {
        Send(pc, "Wait before using the command again.", FColorList::Yellow);
        return;
    }

    APrimalStructureItemContainer* dedi = AimedDedi(pc);
    if (!dedi || !dedi->MyInventoryComponentField()) {
        Send(pc, "Look directly at your tribe S+ Dedicated Storage.", FColorList::Red);
        return;
    }

    TSubclassOf<UPrimalItem> resource;
    resource.uClass = nullptr;
    int stored_before = 0;
    if (!ReadProperty(dedi, "SelectedResourceClass", &resource) || !resource.uClass ||
        !ReadProperty(dedi, "ResourceCount", &stored_before)) {
        Send(pc, "This S+ Dedicated Storage exposes no selected resource/count.", FColorList::Red);
        Log::GetLog()->error("DediWithdrawFix: missing S+ fields class={}", FullName(dedi));
        return;
    }
    if (stored_before <= 0) {
        Send(pc, "This S+ Dedicated Storage is empty.", FColorList::Yellow);
        return;
    }

    AShooterCharacter* character = pc->GetPlayerCharacter();
    UPrimalInventoryComponent* player_inventory = character ? character->MyInventoryComponentField() : nullptr;
    if (!player_inventory) return;

    const int requested = static_cast<int>(std::min<long long>(
        {requested_raw, g_config.max_per_request, stored_before}));

    ChangeDediQuantity(dedi->MyInventoryComponentField(), resource.uClass, -requested);
    int stored_after_remove = stored_before;
    ReadProperty(dedi, "ResourceCount", &stored_after_remove);
    const int removed = std::clamp(stored_before - stored_after_remove, 0, requested);
    if (removed <= 0) {
        Send(pc, "S+ refused to remove the resource. Nothing was changed.", FColorList::Red);
        return;
    }

    const int added = AddToPlayer(player_inventory, resource.uClass, removed);
    const int rollback = removed - added;
    if (rollback > 0) ChangeDediQuantity(dedi->MyInventoryComponentField(), resource.uClass, rollback);

    int stored_final = stored_after_remove;
    ReadProperty(dedi, "ResourceCount", &stored_final);
    dedi->ForceNetUpdate(false, true, false);
    character->ForceNetUpdate(false, true, false);
    g_cooldowns[tribe] = now + std::chrono::seconds(g_config.cooldown_seconds);

    Log::GetLog()->info(
        "DediWithdrawFix: tribe={} structure={} item={} requested={} removed={} added={} rollback={} count={}=>{}",
        tribe, dedi->StructureIDField(), FullName(resource.uClass), requested,
        removed, added, rollback, stored_before, stored_final);

    if (added > 0) Send(pc, "Withdrawn " + std::to_string(added) + " item(s).");
    else Send(pc, "Your inventory rejected the item; storage was restored.", FColorList::Red);
}

void Command(AShooterPlayerController* pc, FString* raw, EChatSendMode::Type) noexcept {
    try { CommandImpl(pc, raw); }
    catch (const std::exception& error) {
        Log::GetLog()->error("DediWithdrawFix command error: {}", error.what());
        Send(pc, "Dedi withdraw failed safely; check the server log.", FColorList::Red);
    }
    catch (...) {
        Log::GetLog()->error("DediWithdrawFix command unknown error");
        Send(pc, "Dedi withdraw failed safely; check the server log.", FColorList::Red);
    }
}

void Reload(APlayerController*, FString*, bool) {
    try {
        ReadConfig();
        Log::GetLog()->info("DediWithdrawFix reloaded");
    } catch (const std::exception& error) {
        Log::GetLog()->error("DediWithdrawFix reload failed: {}", error.what());
    }
}

} // namespace DediWithdrawFix

void Load() {
    Log::Get().Init("DediWithdrawFix");
    DediWithdrawFix::ReadConfig();
    ArkApi::GetCommands().AddChatCommand(
        FString(DediWithdrawFix::g_config.command.c_str()), &DediWithdrawFix::Command);
    ArkApi::GetCommands().AddConsoleCommand(
        "DediWithdrawFix.Reload", &DediWithdrawFix::Reload);
    Log::GetLog()->info("Loaded DediWithdrawFix v1.0 (command-only, no global hooks)");
}

void Unload() {
    ArkApi::GetCommands().RemoveChatCommand(FString(DediWithdrawFix::g_config.command.c_str()));
    ArkApi::GetCommands().RemoveConsoleCommand("DediWithdrawFix.Reload");
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try { Load(); }
    catch (const std::exception& error) {
        Log::Get().Init("DediWithdrawFix");
        Log::GetLog()->error("DediWithdrawFix failed to initialize: {}", error.what());
    }
    catch (...) {
        Log::Get().Init("DediWithdrawFix");
        Log::GetLog()->error("DediWithdrawFix failed with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try { Unload(); } catch (...) {}
}
