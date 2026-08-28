#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace BaseMaintenance {

struct Config {
    bool enabled = true;
    bool prevent_spawn_animation = true;
    std::string repair_command = "/repair";
    int radius_foundations = 10;
    float foundation_units = 300.0f;
    int max_structures = 1000;
    int cooldown_seconds = 10;
    float repair_cost_multiplier = 1.0f;
};

struct Requirement {
    UClass* resource_class = nullptr;
    int quantity = 0;
    bool exact = true;
    std::string name;
};

Config g_config;
std::unordered_map<unsigned long long, std::chrono::steady_clock::time_point> g_cooldowns;

std::string PluginDir() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/BaseMaintenance";
}

void Send(AShooterPlayerController* pc, const std::string& message, const FLinearColor& color = FColorList::Green) {
    if (!pc) return;
    const FString text(ArkApi::Tools::Utf8Decode(message).c_str());
    ArkApi::GetApiUtils().SendServerMessage(pc, color, *text);
}

void ReadConfig() {
    std::ifstream file(PluginDir() + "/config.json", std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("cannot open BaseMaintenance/config.json");
    std::ostringstream stream;
    stream << file.rdbuf();
    const auto root = minijson::parse(stream.str());

    Config value;
    value.enabled = minijson::boolean(root, "General", "Enabled", value.enabled);
    value.prevent_spawn_animation = minijson::boolean(root, "General", "PreventSpawnAnimation", value.prevent_spawn_animation);
    value.repair_command = minijson::str(root, "Commands", "Repair", value.repair_command);
    value.radius_foundations = minijson::integer(root, "Repair", "RadiusFoundations", value.radius_foundations);
    value.foundation_units = minijson::number(root, "Repair", "FoundationSizeUnits", value.foundation_units);
    value.max_structures = minijson::integer(root, "Repair", "MaxStructuresPerCommand", value.max_structures);
    value.cooldown_seconds = minijson::integer(root, "Repair", "CommandCooldownSeconds", value.cooldown_seconds);
    value.repair_cost_multiplier = minijson::number(root, "Repair", "CostMultiplier", value.repair_cost_multiplier);

    value.radius_foundations = std::clamp(value.radius_foundations, 1, 100);
    value.foundation_units = std::clamp(value.foundation_units, 100.0f, 1000.0f);
    value.max_structures = std::clamp(value.max_structures, 1, 10000);
    value.cooldown_seconds = std::clamp(value.cooldown_seconds, 0, 300);
    value.repair_cost_multiplier = std::clamp(value.repair_cost_multiplier, 0.01f, 10.0f);
    if (value.repair_command.empty() || value.repair_command[0] != '/') value.repair_command = "/repair";
    g_config = value;
}

void ApplySpawnAnimationSetting() {
    if (!g_config.prevent_spawn_animation) return;
    AShooterGameMode* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
    AShooterGameState* game_state = ArkApi::GetApiUtils().GetShooterGameState();
    if (game_mode) game_mode->bPreventSpawnAnimationsField() = true;
    if (game_state) {
        game_state->bPreventSpawnAnimationsField() = true;
        game_state->ForceNetUpdate();
    }
}

std::string ResourceName(UClass* cls, AShooterPlayerController* pc) {
    if (!cls) return "Unknown resource";
    UObject* object = cls->GetDefaultObject(true);
    if (!object || !object->IsA(UPrimalItem::GetPrivateStaticClass())) return "Unknown resource";
    auto* item = static_cast<UPrimalItem*>(object);
    FString display;
    item->GetItemName(&display, false, true, pc);
    const std::string result = display.ToString();
    return result.empty() ? "Unknown resource" : result;
}

void AddRequirement(std::vector<Requirement>& requirements, UClass* cls, int quantity,
                    bool exact, AShooterPlayerController* pc) {
    if (!cls || quantity <= 0) return;
    for (auto& requirement : requirements) {
        if (requirement.resource_class == cls && requirement.exact == exact) {
            requirement.quantity += quantity;
            return;
        }
    }
    requirements.push_back({cls, quantity, exact, ResourceName(cls, pc)});
}

bool IsMatchingResource(UPrimalItem* item, const Requirement& requirement) {
    if (!item || item->bIsBlueprint()() || item->bIsEngram()() || item->GetItemQuantity() <= 0) return false;
    if (requirement.exact) return item->ClassField() == requirement.resource_class;
    return item->IsA(requirement.resource_class);
}

int CountResource(UPrimalInventoryComponent* inventory, const Requirement& requirement) {
    if (!inventory) return 0;
    int total = 0;
    const TArray<UPrimalItem*> items = inventory->InventoryItemsField();
    for (UPrimalItem* item : items) {
        if (IsMatchingResource(item, requirement)) total += item->GetItemQuantity();
    }
    return total;
}

int RemoveResource(UPrimalInventoryComponent* inventory, const Requirement& requirement) {
    if (!inventory || requirement.quantity <= 0) return 0;
    int remaining = requirement.quantity;
    const TArray<UPrimalItem*> items = inventory->InventoryItemsField();
    for (UPrimalItem* item : items) {
        if (remaining <= 0) break;
        if (!IsMatchingResource(item, requirement)) continue;
        const int quantity = item->GetItemQuantity();
        const int take = std::min(quantity, remaining);
        if (take < quantity) {
            item->SetQuantity(quantity - take, true);
            inventory->NotifyClientsItemStatus(item, false, false, true, false, false,
                                                nullptr, nullptr, false, false, true);
            remaining -= take;
        } else if (inventory->RemoveItem(&item->ItemIDField(), false, false, true, true)) {
            remaining -= take;
        }
    }
    return requirement.quantity - remaining;
}

bool GetStructureRequirements(APrimalStructure* structure, AShooterPlayerController* pc,
                              std::vector<Requirement>& total) {
    if (!structure) return false;
    const float max_health = structure->GetMaxHealth();
    const float health = structure->GetHealth();
    if (max_health <= 0.0f || health >= max_health - 0.5f) return false;

    UClass* item_class = structure->ConsumesPrimalItemField().uClass;
    UObject* object = item_class ? item_class->GetDefaultObject(true) : nullptr;
    if (!object || !object->IsA(UPrimalItem::GetPrivateStaticClass())) return false;
    auto* item = static_cast<UPrimalItem*>(object);

    const float missing_fraction = std::clamp((max_health - health) / max_health, 0.0f, 1.0f);
    float item_multiplier = item->RepairResourceRequirementMultiplierField();
    if (!std::isfinite(item_multiplier) || item_multiplier <= 0.0f) item_multiplier = 1.0f;

    TArray<FCraftingResourceRequirement>* source = &item->BaseCraftingResourceRequirementsField();
    if (item->bOverrideRepairingRequirements()() && item->OverrideRepairingRequirementsField().Num() > 0) {
        source = &item->OverrideRepairingRequirementsField();
    }

    bool found = false;
    for (const FCraftingResourceRequirement& resource : *source) {
        UClass* resource_class = resource.ResourceItemType.uClass;
        if (!resource_class || resource.BaseResourceRequirement <= 0.0f) continue;
        const float raw = resource.BaseResourceRequirement * missing_fraction * item_multiplier *
                          g_config.repair_cost_multiplier;
        const int quantity = std::max(1, static_cast<int>(std::ceil(raw - 0.0001f)));
        AddRequirement(total, resource_class, quantity,
                       resource.bCraftingRequireExactResourceType, pc);
        found = true;
    }
    return found;
}

std::vector<APrimalStructure*> FindRepairableStructures(AShooterPlayerController* pc,
                                                        int& cooling_down,
                                                        int& unsupported) {
    std::vector<APrimalStructure*> result;
    cooling_down = 0;
    unsupported = 0;
    AShooterCharacter* character = pc ? pc->GetPlayerCharacter() : nullptr;
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!character || !world || !character->RootComponentField()) return result;

    const FVector position = character->RootComponentField()->RelativeLocationField();
    const float radius = g_config.radius_foundations * g_config.foundation_units;
    const int team = ArkApi::GetApiUtils().GetTribeID(pc);
    const long double network_time = UVictoryCore::GetNetworkTimeInSeconds(world);
    TArray<AActor*> actors;
    UVictoryCore::ServerOctreeOverlapActorsClass(&actors, world, position, radius,
        EServerOctreeGroup::STRUCTURES,
        TSubclassOf<AActor>(APrimalStructure::GetPrivateStaticClass()), true);

    for (AActor* actor : actors) {
        if (static_cast<int>(result.size()) >= g_config.max_structures) break;
        if (!actor || !actor->IsA(APrimalStructure::GetPrivateStaticClass())) continue;
        auto* structure = static_cast<APrimalStructure*>(actor);
        if (structure->TargetingTeamField() != team || structure->bIsDead()() ||
            !structure->bCanBeRepaired()() || structure->GetHealth() >= structure->GetMaxHealth() - 0.5f) continue;
        if (!structure->bIgnoreDamageRepairCooldown()() && structure->NextAllowRepairTimeField() > network_time) {
            ++cooling_down;
            continue;
        }
        std::vector<Requirement> probe;
        if (!GetStructureRequirements(structure, pc, probe)) {
            ++unsupported;
            continue;
        }
        result.push_back(structure);
    }
    return result;
}

void RepairCommand(AShooterPlayerController* pc, FString* raw, EChatSendMode::Type) {
    if (!g_config.enabled || !pc || !raw || ArkApi::IApiUtils::IsPlayerDead(pc)) return;
    std::istringstream input(raw->ToString());
    std::string command, argument;
    input >> command >> argument;
    std::transform(argument.begin(), argument.end(), argument.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (argument != "all") {
        Send(pc, "Usage: /repair all", FColorList::Yellow);
        return;
    }

    const auto steam_id = ArkApi::GetApiUtils().GetSteamIdFromController(pc);
    const auto now = std::chrono::steady_clock::now();
    const auto cooldown = g_cooldowns.find(steam_id);
    if (cooldown != g_cooldowns.end() && now < cooldown->second) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(cooldown->second - now).count() + 1;
        Send(pc, "Wait " + std::to_string(seconds) + " second(s) before repairing again.", FColorList::Yellow);
        return;
    }
    g_cooldowns[steam_id] = now + std::chrono::seconds(g_config.cooldown_seconds);

    AShooterCharacter* character = pc->GetPlayerCharacter();
    UPrimalInventoryComponent* inventory = character ? character->MyInventoryComponentField() : nullptr;
    if (!inventory) return;

    int cooling_down = 0;
    int unsupported = 0;
    const auto structures = FindRepairableStructures(pc, cooling_down, unsupported);
    if (structures.empty()) {
        if (cooling_down > 0) {
            Send(pc, "Nothing repaired: " + std::to_string(cooling_down) +
                     " structure(s) are still on the normal post-damage repair cooldown.", FColorList::Yellow);
        } else {
            Send(pc, "No damaged repairable tribe structures found nearby.", FColorList::Yellow);
        }
        return;
    }

    std::vector<Requirement> total;
    for (APrimalStructure* structure : structures) GetStructureRequirements(structure, pc, total);

    std::vector<std::string> missing;
    for (const Requirement& requirement : total) {
        const int available = CountResource(inventory, requirement);
        if (available < requirement.quantity) {
            missing.push_back(requirement.name + " x" + std::to_string(requirement.quantity - available));
        }
    }
    if (!missing.empty()) {
        std::string message = "Not enough resources. Missing: ";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i > 0) message += ", ";
            message += missing[i];
            if (message.size() > 430) { message += "..."; break; }
        }
        Send(pc, message, FColorList::Red);
        return;
    }

    for (const Requirement& requirement : total) {
        const int removed = RemoveResource(inventory, requirement);
        if (removed != requirement.quantity) {
            Log::GetLog()->error("BaseMaintenance unexpected resource removal mismatch wanted={} removed={}",
                                 requirement.quantity, removed);
            Send(pc, "Repair cancelled because resource consumption changed unexpectedly.", FColorList::Red);
            return;
        }
    }

    int repaired = 0;
    for (APrimalStructure* structure : structures) {
        if (!structure || structure->bIsDead()()) continue;
        structure->SetHealth(structure->GetMaxHealth());
        structure->UpdatedHealth(true);
        ++repaired;
    }

    std::string result = "Repaired " + std::to_string(repaired) + " structure(s) within " +
                         std::to_string(g_config.radius_foundations) + " foundation(s).";
    if (cooling_down > 0) result += " Skipped on damage cooldown: " + std::to_string(cooling_down) + ".";
    if (unsupported > 0) result += " Unsupported structures skipped: " + std::to_string(unsupported) + ".";
    Send(pc, result);
}

void Reload(APlayerController*, FString*, bool) {
    try {
        const std::string old_command = g_config.repair_command;
        ReadConfig();
        ApplySpawnAnimationSetting();
        if (old_command != g_config.repair_command) {
            ArkApi::GetCommands().RemoveChatCommand(FString(old_command.c_str()));
            ArkApi::GetCommands().AddChatCommand(FString(g_config.repair_command.c_str()), &RepairCommand);
        }
        Log::GetLog()->info("BaseMaintenance config reloaded");
    } catch (const std::exception& e) {
        Log::GetLog()->error("BaseMaintenance reload failed: {}", e.what());
    }
}

} // namespace BaseMaintenance

DECLARE_HOOK(AShooterGameMode_BeginPlay, void, AShooterGameMode*);

void Hook_AShooterGameMode_BeginPlay(AShooterGameMode* game_mode) {
    AShooterGameMode_BeginPlay_original(game_mode);
    BaseMaintenance::ApplySpawnAnimationSetting();
}

void Load() {
    Log::Get().Init("BaseMaintenance");
    BaseMaintenance::ReadConfig();
    ArkApi::GetHooks().SetHook("AShooterGameMode.BeginPlay", &Hook_AShooterGameMode_BeginPlay,
                               &AShooterGameMode_BeginPlay_original);
    ArkApi::GetCommands().AddChatCommand(FString(BaseMaintenance::g_config.repair_command.c_str()),
                                         &BaseMaintenance::RepairCommand);
    ArkApi::GetCommands().AddConsoleCommand("BaseMaintenance.Reload", &BaseMaintenance::Reload);
    BaseMaintenance::ApplySpawnAnimationSetting();
    Log::GetLog()->info("Loaded plugin - BaseMaintenance v1.0 (repair radius={} foundations, prevent spawn animation={})",
                        BaseMaintenance::g_config.radius_foundations,
                        BaseMaintenance::g_config.prevent_spawn_animation);
}

void Unload() {
    ArkApi::GetCommands().RemoveChatCommand(FString(BaseMaintenance::g_config.repair_command.c_str()));
    ArkApi::GetCommands().RemoveConsoleCommand("BaseMaintenance.Reload");
    ArkApi::GetHooks().DisableHook("AShooterGameMode.BeginPlay", &Hook_AShooterGameMode_BeginPlay);
}

