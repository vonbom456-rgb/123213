#include <API/ARK/Ark.h>
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
        AShooterCharacter* character = pc->GetPlayerCharacter();
        if (!character) continue;
        UPrimalInventoryComponent* inventory = character->MyInventoryComponentField();
        if (!inventory) continue;

        // Never rewrite MaxInventoryItems/AbsoluteMaxInventoryItems. Their
        // zero and Blueprint-defined values have class-specific semantics in
        // ASE and changing them can falsely lock an otherwise empty player
        // inventory. Only clear the cached full flag when the inventory has
        // fewer than the vanilla ceiling of valid item pointers.
        int valid_items = 0;
        for (UPrimalItem* item : inventory->InventoryItemsField())
            if (item) ++valid_items;
        if (valid_items < g_config.player_inventory_slots &&
            character->bIsAtMaxInventoryItems().Get()) {
            character->bIsAtMaxInventoryItems().Set(false);
            ++cleared;
        }
    }

    if (cleared > 0) {
        Log::GetLog()->info(
            "InventoryCapacity: cleared {} stale player inventory-full flag(s)",
            cleared);
    }
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
    Log::GetLog()->info(
        "Loaded plugin - InventoryCapacity v1.2 safe mode (enabled={}, no inventory limit fields are modified)",
        InventoryCapacity::g_config.enabled);
}

void Unload() {
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
