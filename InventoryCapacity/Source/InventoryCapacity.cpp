#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace InventoryCapacity {

struct Config {
    bool enabled = false;
    int player_inventory_slots = 300;
    int update_interval_seconds = 1;
};

Config g_config;
std::chrono::steady_clock::time_point g_next_update{};

std::string PluginDir() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/InventoryCapacity";
}

void ReadConfig() {
    std::ifstream file(PluginDir() + "/config.json", std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("cannot open InventoryCapacity/config.json");
    std::ostringstream stream;
    stream << file.rdbuf();
    const minijson::Value root = minijson::parse(stream.str());
    if (!root.is_object()) throw std::runtime_error("config root must be a JSON object");

    Config value;
    value.enabled = minijson::boolean(root, "PlayerInventory", "Enabled", value.enabled);
    value.player_inventory_slots = minijson::integer(
        root, "PlayerInventory", "MaxSlots", value.player_inventory_slots);
    value.update_interval_seconds = minijson::integer(
        root, "PlayerInventory", "UpdateIntervalSeconds", value.update_interval_seconds);
    // Very large player inventories generate huge reliable inventory updates.
    // Keep a configurable but safe upper bound for ASE clients.
    // 300 is ASE's normal absolute item limit.  Do not turn this bug fix into
    // extra inventory slots; we only repair the stale full-inventory flag.
    value.player_inventory_slots = 300;
    value.update_interval_seconds = std::clamp(value.update_interval_seconds, 1, 30);
    g_config = value;
}

bool RepairPlayer(AShooterPlayerController* pc) {
    if (!pc) return false;
    AShooterCharacter* character = pc->GetPlayerCharacter();
    if (!character) return false;
    UPrimalInventoryComponent* inventory = character->MyInventoryComponentField();
    if (!inventory) return false;

    // The character keeps a cached "inventory full" flag. S+/ASE can leave
    // it set after a failed transfer even when the inventory component itself
    // is no longer full. Trust the inventory's native capacity calculation,
    // clear only that stale cache and never alter slot/weight limits.
    if (character->bIsAtMaxInventoryItems().Get() &&
        !inventory->IsAtMaxInventoryItems()) {
        character->bIsAtMaxInventoryItems().Set(false);
        character->ForceNetUpdate(false, true, false);
        return true;
    }
    return false;
}

void ApplyToPlayers() {
    if (!g_config.enabled) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_update) return;
    g_next_update = now + std::chrono::seconds(g_config.update_interval_seconds);

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return;

    int cleared = 0;
    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        APlayerController* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        if (RepairPlayer(pc)) ++cleared;
    }

    if (cleared > 0) {
        Log::GetLog()->info(
            "InventoryCapacity: cleared {} stale player inventory-full flag(s)",
            cleared);
    }
}

void FixCommand(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    const bool repaired = RepairPlayer(pc);
    const wchar_t* message = repaired
        ? L"Inventory state repaired. Try taking the item again."
        : L"Inventory state is already normal. Check weight and real slot limit.";
    ArkApi::GetApiUtils().SendServerMessage(
        pc, repaired ? FColorList::Green : FColorList::Yellow, message);
}

void ReloadCommand(APlayerController*, FString*, bool) {
    try {
        ReadConfig();
        g_next_update = std::chrono::steady_clock::time_point{};
        ApplyToPlayers();
        Log::GetLog()->info(
            "InventoryCapacity: config reloaded (enabled={}, slots={})",
            g_config.enabled, g_config.player_inventory_slots);
    } catch (const std::exception& error) {
        Log::GetLog()->error("InventoryCapacity: reload failed: {}", error.what());
    }
}

} // namespace InventoryCapacity

void Load() {
    Log::Get().Init("InventoryCapacity");
    try {
        InventoryCapacity::ReadConfig();
    } catch (const std::exception& error) {
        Log::GetLog()->error(
            "InventoryCapacity: config error ({}), using defaults", error.what());
        InventoryCapacity::g_config = InventoryCapacity::Config();
    }
    ArkApi::GetCommands().AddOnTimerCallback(
        "InventoryCapacity.Update", &InventoryCapacity::ApplyToPlayers);
    ArkApi::GetCommands().AddConsoleCommand(
        "InventoryCapacity.Reload", &InventoryCapacity::ReloadCommand);
    ArkApi::GetCommands().AddChatCommand(
        "/invfix", &InventoryCapacity::FixCommand);
    Log::GetLog()->info(
        "Loaded plugin - InventoryCapacity v1.3 safe mode (enabled={}, no inventory limit fields are modified)",
        InventoryCapacity::g_config.enabled);
}

void Unload() {
    ArkApi::GetCommands().RemoveChatCommand("/invfix");
    ArkApi::GetCommands().RemoveConsoleCommand("InventoryCapacity.Reload");
    ArkApi::GetCommands().RemoveOnTimerCallback("InventoryCapacity.Update");
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try { Load(); }
    catch (const std::exception& error) {
        Log::Get().Init("InventoryCapacity");
        Log::GetLog()->error("InventoryCapacity failed to initialize: {}", error.what());
    }
    catch (...) {
        Log::Get().Init("InventoryCapacity");
        Log::GetLog()->error("InventoryCapacity failed with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try { Unload(); }
    catch (...) {}
}
