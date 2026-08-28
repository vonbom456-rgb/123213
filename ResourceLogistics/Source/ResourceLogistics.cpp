#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace ResourceLogistics {

struct Config {
    bool enabled = true;
    float radius = 3000.0f;
    int max_per_command = 50000;
    int cooldown_seconds = 2;
    bool block_during_pvp = true;
    std::string pull_command = "/pull";
    std::string distribute_command = "/distribute";
};

Config g_config;
std::unordered_map<unsigned long long, std::chrono::steady_clock::time_point> g_cooldowns;
using PvpCooldownQuery = bool(__fastcall*)(AShooterPlayerController*);

std::string PluginDir() { return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/ResourceLogistics"; }

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string FullName(UObject* object) {
    if (!object) return {};
    FString value;
    object->GetFullName(&value, nullptr);
    return value.ToString();
}

void Send(AShooterPlayerController* pc, const std::string& message) {
    if (!pc) return;
    const FString text(ArkApi::Tools::Utf8Decode(message).c_str());
    ArkApi::GetApiUtils().SendServerMessage(pc, FColorList::Green, *text);
}

bool IsPvp(AShooterPlayerController* pc) {
    if (!g_config.block_during_pvp || !pc) return false;
    const HMODULE module = GetModuleHandleA("PvPCooldowns.dll");
    if (!module) return false;
    const auto query = reinterpret_cast<PvpCooldownQuery>(GetProcAddress(module, "PvpCooldowns_IsOnCooldown"));
    return query && query(pc);
}

void ReadConfig() {
    std::ifstream file(PluginDir() + "/config.json", std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("cannot open ResourceLogistics/config.json");
    std::ostringstream stream; stream << file.rdbuf();
    const auto root = minijson::parse(stream.str());
    Config value;
    value.enabled = minijson::boolean(root, "General", "Enabled", value.enabled);
    value.radius = minijson::number(root, "General", "Radius", value.radius);
    value.max_per_command = minijson::integer(root, "General", "MaxItemsPerCommand", value.max_per_command);
    value.cooldown_seconds = minijson::integer(root, "General", "CooldownSeconds", value.cooldown_seconds);
    value.block_during_pvp = minijson::boolean(root, "General", "BlockDuringPvp", value.block_during_pvp);
    value.pull_command = minijson::str(root, "Commands", "Pull", value.pull_command);
    value.distribute_command = minijson::str(root, "Commands", "Distribute", value.distribute_command);
    value.radius = std::clamp(value.radius, 500.0f, 15000.0f);
    value.max_per_command = std::clamp(value.max_per_command, 1, 1000000);
    value.cooldown_seconds = std::clamp(value.cooldown_seconds, 0, 60);
    g_config = value;
}

bool IsResource(UPrimalItem* item) {
    return item && !item->bIsBlueprint()() && !item->bIsEngram()() &&
        item->bAllowRemovalFromInventory()() &&
        item->MyItemTypeField().GetValue() == EPrimalItemType::Resource &&
        item->GetItemQuantity() > 0;
}

std::string ItemSearchText(UPrimalItem* item, AShooterPlayerController* pc) {
    if (!item) return {};
    FString display;
    item->GetItemName(&display, false, true, pc);
    return Lower(display.ToString() + " " + FullName(item->ClassField()));
}

int ExactQuantity(UPrimalInventoryComponent* inv, UClass* cls) {
    if (!inv || !cls) return 0;
    return inv->GetItemTemplateQuantity(TSubclassOf<UPrimalItem>(cls), nullptr, true, false, true, true);
}

int RemoveExact(UPrimalInventoryComponent* inv, UClass* cls, int requested) {
    if (!inv || !cls || requested <= 0) return 0;
    const int before = ExactQuantity(inv, cls);
    int remaining = std::min(requested, before);
    const TArray<UPrimalItem*> snapshot = inv->InventoryItemsField();
    for (UPrimalItem* item : snapshot) {
        if (remaining <= 0) break;
        if (!IsResource(item) || item->ClassField() != cls) continue;
        const int qty = item->GetItemQuantity();
        const int take = std::min(qty, remaining);
        if (take < qty) {
            item->SetQuantity(qty - take, true);
            inv->NotifyClientsItemStatus(item, false, false, true, false, false, nullptr, nullptr, false, false, true);
            remaining -= take;
        } else if (inv->RemoveItem(&item->ItemIDField(), false, false, true, true)) {
            remaining -= take;
        }
    }
    return std::clamp(before - ExactQuantity(inv, cls), 0, requested);
}

int AddExact(UPrimalInventoryComponent* inv, UClass* cls, int requested) {
    if (!inv || !cls || requested <= 0) return 0;
    const int before = ExactQuantity(inv, cls);
    TSubclassOf<UPrimalItem> no_skin; no_skin.uClass = nullptr;
    UPrimalItem::AddNewItem(TSubclassOf<UPrimalItem>(cls), inv, false, false, 0.0f, true,
        requested, false, 0.0f, false, no_skin, 0.0f, false, false);
    return std::clamp(ExactQuantity(inv, cls) - before, 0, requested);
}

int SafeTransfer(UPrimalInventoryComponent* from, UPrimalInventoryComponent* to,
                 UClass* cls, int requested) {
    if (!from || !to || !cls || requested <= 0) return 0;
    const int removed = RemoveExact(from, cls, requested);
    if (removed <= 0) return 0;
    const int added = AddExact(to, cls, removed);
    if (added < removed) {
        const int restored = AddExact(from, cls, removed - added);
        if (restored != removed - added) {
            Log::GetLog()->error("ResourceLogistics rollback incomplete class='{}' missing={}",
                FullName(cls), removed - added - restored);
        }
    }
    return added;
}

std::vector<APrimalStructureItemContainer*> FindContainers(AShooterPlayerController* pc) {
    std::vector<APrimalStructureItemContainer*> out;
    AShooterCharacter* character = pc ? pc->GetPlayerCharacter() : nullptr;
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!character || !world || !character->RootComponentField()) return out;
    const FVector pos = character->RootComponentField()->RelativeLocationField();
    const int team = ArkApi::GetApiUtils().GetTribeID(pc);
    TArray<AActor*> actors;
    UVictoryCore::ServerOctreeOverlapActorsClass(&actors, world, pos, g_config.radius,
        EServerOctreeGroup::STRUCTURES,
        TSubclassOf<AActor>(APrimalStructureItemContainer::GetPrivateStaticClass()), true);
    for (AActor* actor : actors) {
        if (!actor || !actor->IsA(APrimalStructureItemContainer::GetPrivateStaticClass())) continue;
        auto* container = static_cast<APrimalStructureItemContainer*>(actor);
        if (container->TargetingTeamField() != team || !container->MyInventoryComponentField()) continue;
        out.push_back(container);
    }
    return out;
}

bool BeginCommand(AShooterPlayerController* pc) {
    if (!g_config.enabled || !pc || ArkApi::IApiUtils::IsPlayerDead(pc)) return false;
    if (IsPvp(pc)) { Send(pc, "Resource commands are blocked during RAID/PvP."); return false; }
    const auto steam = ArkApi::GetApiUtils().GetSteamIdFromController(pc);
    const auto now = std::chrono::steady_clock::now();
    auto it = g_cooldowns.find(steam);
    if (it != g_cooldowns.end() && now < it->second) { Send(pc, "Please wait before using this command again."); return false; }
    g_cooldowns[steam] = now + std::chrono::seconds(g_config.cooldown_seconds);
    return true;
}

void PullCommand(AShooterPlayerController* pc, FString* raw, EChatSendMode::Type) {
    if (!BeginCommand(pc) || !raw) return;
    std::istringstream input(raw->ToString());
    std::string command, query; int requested = 0;
    input >> command >> query >> requested;
    query = Lower(query);
    if (query.empty() || requested <= 0) { Send(pc, "Usage: /pull <resource> <quantity>"); return; }
    requested = std::min(requested, g_config.max_per_command);
    AShooterCharacter* character = pc->GetPlayerCharacter();
    UPrimalInventoryComponent* player_inv = character ? character->MyInventoryComponentField() : nullptr;
    if (!player_inv) return;
    int moved = 0;
    for (auto* container : FindContainers(pc)) {
        if (moved >= requested) break;
        UPrimalInventoryComponent* source = container->MyInventoryComponentField();
        const TArray<UPrimalItem*> items = source->InventoryItemsField();
        for (UPrimalItem* item : items) {
            if (moved >= requested) break;
            if (!IsResource(item) || ItemSearchText(item, pc).find(query) == std::string::npos) continue;
            const float unit_weight = std::max(0.0f, item->GetItemWeight(true, true));
            int can_take = requested - moved;
            if (unit_weight > 0.0f) {
                const float free_weight = std::max(0.0f,
                    character->GetMaxStatusValue(EPrimalCharacterStatusValue::Weight) -
                    character->GetCurrentStatusValue(EPrimalCharacterStatusValue::Weight));
                can_take = std::min(can_take, static_cast<int>(std::floor(free_weight / unit_weight)));
            }
            can_take = std::min(can_take, item->GetItemQuantity());
            if (can_take <= 0) break;
            moved += SafeTransfer(source, player_inv, item->ClassField(), can_take);
        }
    }
    Send(pc, moved > 0 ? "Pulled " + std::to_string(moved) + " resource item(s)." :
                        "Nothing was moved: resource not found, or inventory/weight limit reached.");
}

bool IsDedicated(APrimalStructureItemContainer* container) {
    const std::string name = Lower(FullName(container));
    return name.find("dedicated") != std::string::npos && name.find("storage") != std::string::npos;
}

void DistributeCommand(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!BeginCommand(pc)) return;
    AShooterCharacter* character = pc->GetPlayerCharacter();
    UPrimalInventoryComponent* player_inv = character ? character->MyInventoryComponentField() : nullptr;
    if (!player_inv) return;
    const auto containers = FindContainers(pc);
    int moved = 0;
    const TArray<UPrimalItem*> player_items = player_inv->InventoryItemsField();
    for (UPrimalItem* item : player_items) {
        if (!IsResource(item) || moved >= g_config.max_per_command) continue;
        APrimalStructureItemContainer* target = nullptr;
        for (auto* candidate : containers) {
            if (!IsDedicated(candidate)) continue;
            UPrimalInventoryComponent* inv = candidate->MyInventoryComponentField();
            if (ExactQuantity(inv, item->ClassField()) > 0) { target = candidate; break; }
        }
        if (!target) continue;
        const int amount = std::min(item->GetItemQuantity(), g_config.max_per_command - moved);
        moved += SafeTransfer(player_inv, target->MyInventoryComponentField(), item->ClassField(), amount);
    }
    Send(pc, moved > 0 ? "Distributed " + std::to_string(moved) + " resource item(s)." :
                        "No matching Dedicated Storage was found, or all targets are full.");
}

void Reload(APlayerController*, FString*, bool) {
    try { ReadConfig(); Log::GetLog()->info("ResourceLogistics config reloaded"); }
    catch (const std::exception& e) { Log::GetLog()->error("ResourceLogistics reload failed: {}", e.what()); }
}

} // namespace ResourceLogistics

void Load() {
    Log::Get().Init("ResourceLogistics");
    ResourceLogistics::ReadConfig();
    ArkApi::GetCommands().AddChatCommand(FString(ResourceLogistics::g_config.pull_command.c_str()), &ResourceLogistics::PullCommand);
    ArkApi::GetCommands().AddChatCommand(FString(ResourceLogistics::g_config.distribute_command.c_str()), &ResourceLogistics::DistributeCommand);
    ArkApi::GetCommands().AddConsoleCommand("ResourceLogistics.Reload", &ResourceLogistics::Reload);
    Log::GetLog()->info("Loaded plugin - ResourceLogistics v1.0");
}

void Unload() {
    ArkApi::GetCommands().RemoveChatCommand(FString(ResourceLogistics::g_config.pull_command.c_str()));
    ArkApi::GetCommands().RemoveChatCommand(FString(ResourceLogistics::g_config.distribute_command.c_str()));
    ArkApi::GetCommands().RemoveConsoleCommand("ResourceLogistics.Reload");
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept { try { Load(); } catch (...) {} }
extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept { try { Unload(); } catch (...) {} }
