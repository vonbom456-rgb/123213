#include <API/ARK/Ark.h>
#include "StorageSafety.h"
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
bool faulted = false;
std::string registered_pull, registered_distribute;
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
    value.max_per_command = std::clamp(value.max_per_command, 1, 10000);
    value.cooldown_seconds = std::clamp(value.cooldown_seconds, 0, 60);
    if (!registered_pull.empty()) { value.pull_command = registered_pull; value.distribute_command = registered_distribute; }
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

int ExactQuantity(UPrimalInventoryComponent* inv, UClass* cls) { return StorageSafety::Quantity(inv,cls); }
int SafeTransfer(UPrimalInventoryComponent* from,UPrimalInventoryComponent* to,UClass* cls,int amount) {
    return StorageSafety::Transfer(from,to,cls,amount);
}

std::vector<APrimalStructureItemContainer*> FindContainers(AShooterPlayerController* pc) {
    return StorageSafety::Nearby(pc ? pc->GetPlayerCharacter() : nullptr,
        pc ? ArkApi::GetApiUtils().GetTribeID(pc) : 0, g_config.radius);
}

bool BeginCommand(AShooterPlayerController* pc) {
    if (!g_config.enabled || !pc || ArkApi::IApiUtils::IsPlayerDead(pc)) return false;
    if (faulted) { Send(pc, "Transfers paused after an error. Check server log."); return false; }
    if (ArkApi::GetApiUtils().GetTribeID(pc) < 50000) { Send(pc,"You must be in a tribe."); return false; }
    if (IsPvp(pc)) { Send(pc, "Resource commands are blocked during RAID/PvP."); return false; }
    const auto steam = ArkApi::GetApiUtils().GetSteamIdFromController(pc);
    const auto now = std::chrono::steady_clock::now();
    auto it = g_cooldowns.find(steam);
    if (it != g_cooldowns.end() && now < it->second) { Send(pc, "Please wait before using this command again."); return false; }
    g_cooldowns[steam] = now + std::chrono::seconds(g_config.cooldown_seconds);
    return true;
}

void PullImpl(AShooterPlayerController* pc, FString* raw) {
    if (!BeginCommand(pc) || !raw) return;
    std::istringstream input(raw->ToString());
    std::string command, query; int requested=0; input>>command>>query>>requested;
    query=Lower(query);
    if(query.empty() || requested<=0) { Send(pc,"Usage: /pull <resource> <quantity>"); return; }
    requested=std::min(requested,g_config.max_per_command);
    auto* character=pc->GetPlayerCharacter();
    auto* inv=character ? character->MyInventoryComponentField() : nullptr;
    if(!inv) return;
    int moved=0, ops=0;
    for(auto* c:FindContainers(pc)) {
        auto* source=c->MyInventoryComponentField();
        for(auto* cls:StorageSafety::Resources(source)) {
            if(moved>=requested || ++ops>64) break;
            // Snapshot classes, never item pointers which a previous transfer can invalidate.
            auto* prototype=static_cast<UPrimalItem*>(cls->ClassDefaultObjectField());
            if(!prototype || ItemSearchText(prototype,pc).find(query)==std::string::npos) continue;
            moved+=SafeTransfer(source,inv,cls,requested-moved);
        }
        if(moved>=requested || ops>64) break;
    }
    Send(pc,"Pulled "+std::to_string(moved)+" item(s). Unsupported Dedicated Storage is skipped.");
}

void PullCommand(AShooterPlayerController* pc,FString* raw,EChatSendMode::Type) {
    try { PullImpl(pc,raw); }
    catch(const std::exception& e) { faulted=true; Log::GetLog()->error("ResourceLogistics /pull stopped: {}",e.what()); Send(pc,"Transfer error: check log and inventory counts."); }
    catch(...) { faulted=true; Log::GetLog()->error("ResourceLogistics /pull unknown exception"); }
}

bool IsDedicated(APrimalStructureItemContainer* container) {
    const std::string name = Lower(FullName(container));
    return name.find("dedicated") != std::string::npos && name.find("storage") != std::string::npos;
}

void DistributeImpl(AShooterPlayerController* pc) {
    if(!BeginCommand(pc)) return;
    auto* character=pc->GetPlayerCharacter();
    auto* inv=character ? character->MyInventoryComponentField() : nullptr;
    if(!inv) return;
    const auto containers=FindContainers(pc);
    int moved=0,ops=0;
    for(auto* cls:StorageSafety::Resources(inv)) {
        if(moved>=g_config.max_per_command || ++ops>64) break;
        for(auto* c:containers) {
            if(!IsDedicated(c) || !StorageSafety::Accepts(c->MyInventoryComponentField(),cls)) continue;
            int added=SafeTransfer(inv,c->MyInventoryComponentField(),cls,g_config.max_per_command-moved);
            moved+=added;
            if(added>0) break;
        }
    }
    Send(pc,"Distributed "+std::to_string(moved)+" item(s). Dedicated must have a matching assigned resource.");
}
void DistributeCommand(AShooterPlayerController* pc,FString*,EChatSendMode::Type) {
    try { DistributeImpl(pc); }
    catch(const std::exception& e) { faulted=true; Log::GetLog()->error("ResourceLogistics /distribute stopped: {}",e.what()); Send(pc,"Transfer error: check log and inventory counts."); }
    catch(...) { faulted=true; Log::GetLog()->error("ResourceLogistics /distribute unknown exception"); }
}

void Reload(APlayerController*, FString*, bool) {
    try { ReadConfig(); Log::GetLog()->info("ResourceLogistics config reloaded"); }
    catch (const std::exception& e) { Log::GetLog()->error("ResourceLogistics reload failed: {}", e.what()); }
}

} // namespace ResourceLogistics

void Load() {
    Log::Get().Init("ResourceLogistics");
    ResourceLogistics::ReadConfig();
    ResourceLogistics::registered_pull=ResourceLogistics::g_config.pull_command;
    ResourceLogistics::registered_distribute=ResourceLogistics::g_config.distribute_command;
    ArkApi::GetCommands().AddChatCommand(FString(ResourceLogistics::g_config.pull_command.c_str()), &ResourceLogistics::PullCommand);
    ArkApi::GetCommands().AddChatCommand(FString(ResourceLogistics::g_config.distribute_command.c_str()), &ResourceLogistics::DistributeCommand);
    ArkApi::GetCommands().AddConsoleCommand("ResourceLogistics.Reload", &ResourceLogistics::Reload);
    Log::GetLog()->info("Loaded plugin - ResourceLogistics v1.1 (checked transfers; no turret access)");
}

void Unload() {
    ArkApi::GetCommands().RemoveChatCommand(FString(ResourceLogistics::g_config.pull_command.c_str()));
    ArkApi::GetCommands().RemoveChatCommand(FString(ResourceLogistics::g_config.distribute_command.c_str()));
    ArkApi::GetCommands().RemoveConsoleCommand("ResourceLogistics.Reload");
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept { try { Load(); } catch (...) {} }
extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept { try { Unload(); } catch (...) {} }
