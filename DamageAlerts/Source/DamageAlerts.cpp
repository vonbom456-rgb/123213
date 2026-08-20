// DamageAlerts -- a focused damage-number plugin. ARK's native floating
// number is shown to the attacking player for every hit that actually deals
// damage. Raid state and raid notifications belong to PvPCooldowns.
//
// Detection/attribution reuses the same verified hooks and fields as
// TurretControl and PvPCooldowns: APrimalCharacter.TakeDamage and
// APrimalStructure.TakeDamage (the standard ASE damage hook points), plus
// AActor::TargetingTeamField for tribe attribution. See those plugins'
// SOURCE_VERIFICATION.md / README for where these were confirmed.
//
// Victim alerts and the optional attacker chat fallback are aggregated and
// flushed at most once every FlushIntervalSeconds.

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
#include "MiniJson.h"

namespace DamageAlerts {

enum class Category { Structure = 0, Dino = 1, Player = 2 };

const char* CategoryLabel(Category cat) {
    switch (cat) {
        case Category::Structure: return "структуре";
        case Category::Dino: return "живности";
        case Category::Player: return "игроку";
    }
    return "цели";
}

struct Config {
    float min_damage_to_report = 1.0f;
    int flush_interval_seconds = 1;
    bool notify_attacker = true;
    bool notify_victim_tribe = false;
    int min_tribe_team_id = 50000;
    bool floating_damage_enabled = true;
    bool force_native_server_setting = true;
    bool also_send_attacker_chat = false;
    float floating_damage_vertical_offset = 100.0f;
    std::string test_command = "/datest";

    std::string attacker_hit = "+{0} урона по {1}";
    std::string victim_hit = "-{0} урона по {1} (враг)";
};

Config g_config;
uint64 g_character_damage_events = 0;
uint64 g_structure_damage_events = 0;
uint64 g_floating_rpc_events = 0;
bool g_native_floating_applied = false;

std::string ReplaceToken(std::string text, const std::string& token, const std::string& value) {
    const size_t pos = text.find(token);
    if (pos != std::string::npos) text.replace(pos, token.length(), value);
    return text;
}

std::string FormatMessage(const std::string& tmpl, float amount, Category cat) {
    std::string out = ReplaceToken(tmpl, "{0}", std::to_string(static_cast<long long>(std::llround(amount))));
    out = ReplaceToken(out, "{1}", CategoryLabel(cat));
    return out;
}

void SendColored(AShooterPlayerController* pc, const std::string& text, const FLinearColor& color) {
    if (!pc || text.empty()) return;
    const FString message(ArkApi::Tools::Utf8Decode(text).c_str());
    ArkApi::GetApiUtils().SendServerMessage(pc, color, *message);
}

// Pending aggregation: steam_id / tribe_id -> category index -> summed damage.
std::unordered_map<uint64, std::unordered_map<int, float>> g_pending_attacker;
std::unordered_map<int, std::unordered_map<int, float>> g_pending_victim;

struct AttackerInfo {
    int team = -1;
    AShooterPlayerController* player = nullptr;
};

bool IsPlayerOwnedTeam(int team) {
    return team >= g_config.min_tribe_team_id;
}

AttackerInfo ResolveAttacker(AController* event_instigator, AActor* damage_causer) {
    AttackerInfo result;

    if (event_instigator) {
        if (event_instigator->IsA(AShooterPlayerController::GetPrivateStaticClass())) {
            result.player = static_cast<AShooterPlayerController*>(event_instigator);
            result.team = ArkApi::GetApiUtils().GetTribeID(result.player);
            if (!IsPlayerOwnedTeam(result.team)) result.team = -1;
            return result;
        }

        ACharacter* character = event_instigator->CharacterField();
        if (character && character->IsA(APrimalDinoCharacter::GetPrivateStaticClass())) {
            auto* dino = static_cast<APrimalDinoCharacter*>(character);
            if (!dino->BPIsTamed()) return result; // explicit wild-dino rejection
            result.team = dino->TargetingTeamField();
            if (!IsPlayerOwnedTeam(result.team)) result.team = -1;
            return result;
        }
    }

    if (damage_causer && damage_causer->IsA(APrimalDinoCharacter::GetPrivateStaticClass())) {
        auto* dino = static_cast<APrimalDinoCharacter*>(damage_causer);
        if (!dino->BPIsTamed()) return result;
    }

    result.team = damage_causer ? damage_causer->TargetingTeamField() : -1;
    if (!IsPlayerOwnedTeam(result.team)) result.team = -1;
    return result;
}

void RecordDamage(AActor* target, AController* event_instigator, AActor* damage_causer, float amount, Category cat) {
    if (!target || amount <= 0.f) return;

    const int target_team = target->TargetingTeamField();

    const AttackerInfo attacker = ResolveAttacker(event_instigator, damage_causer);

    if (g_config.notify_attacker && attacker.player && amount >= g_config.min_damage_to_report) {
        // When ForceNativeServerSetting is enabled ARK itself emits the
        // floating number. Sending our RPC too would create duplicate text.
        if (g_config.floating_damage_enabled && !g_config.force_native_server_setting) {
            FVector location{};
            FVector extent{};
            target->GetActorBounds(false, &location, &extent);
            location.Z += g_config.floating_damage_vertical_offset;
            const int displayed_damage = std::max(1, static_cast<int>(std::llround(amount)));

            // This is ASE's native client RPC. Unlike a custom texture it
            // needs no client mod: ARK itself renders and colours the number.
            attacker.player->ClientAddFloatingDamageText(
                FVector_NetQuantize(location), displayed_damage, attacker.team);
            ++g_floating_rpc_events;
        }

        if (g_config.also_send_attacker_chat) {
            const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(attacker.player);
            if (steam_id != 0) g_pending_attacker[steam_id][static_cast<int>(cat)] += amount;
        }
    }

    // Only alert the victim's tribe if the target is actually owned by a
    // tribe and the damage came from a genuinely different tribe.
    if (g_config.notify_victim_tribe && IsPlayerOwnedTeam(target_team) &&
        IsPlayerOwnedTeam(attacker.team) && attacker.team != target_team) {
        g_pending_victim[target_team][static_cast<int>(cat)] += amount;
    }
}

std::chrono::steady_clock::time_point g_next_flush{};

void FlushTimer() {
    if (g_config.floating_damage_enabled && g_config.force_native_server_setting) {
        AShooterGameMode* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
        if (game_mode) {
            game_mode->bShowFloatingDamageTextField() = true;
            if (!g_native_floating_applied) {
                g_native_floating_applied = true;
                Log::GetLog()->info("DamageAlerts: native ARK floating damage enabled");
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_flush) return;
    g_next_flush = now + std::chrono::seconds(std::max(1, g_config.flush_interval_seconds));

    if (g_pending_attacker.empty() && g_pending_victim.empty()) return;

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) {
        g_pending_attacker.clear();
        g_pending_victim.clear();
        return;
    }

    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        auto* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);

        const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
        if (steam_id != 0) {
            const auto it = g_pending_attacker.find(steam_id);
            if (it != g_pending_attacker.end()) {
                for (const auto& [cat_idx, amount] : it->second) {
                    if (amount >= g_config.min_damage_to_report) {
                        SendColored(pc, FormatMessage(g_config.attacker_hit, amount, static_cast<Category>(cat_idx)),
                                    FColorList::Green);
                    }
                }
            }
        }

        const int tribe_id = ArkApi::GetApiUtils().GetTribeID(pc);
        const auto vit = g_pending_victim.find(tribe_id);
        if (vit != g_pending_victim.end()) {
            for (const auto& [cat_idx, amount] : vit->second) {
                if (amount >= g_config.min_damage_to_report) {
                    SendColored(pc, FormatMessage(g_config.victim_hit, amount, static_cast<Category>(cat_idx)),
                                FColorList::Red);
                }
            }
        }
    }

    g_pending_attacker.clear();
    g_pending_victim.clear();
}

// --- Hooks ---

DECLARE_HOOK(APrimalCharacter_TakeDamage, float, APrimalCharacter*, float, FDamageEvent*, AController*, AActor*);
DECLARE_HOOK(APrimalStructure_TakeDamage, float, APrimalStructure*, float, FDamageEvent*, AController*, AActor*);

float Hook_APrimalCharacter_TakeDamage(APrimalCharacter* _this, float damage, FDamageEvent* damage_event,
                                        AController* event_instigator, AActor* damage_causer) {
    const float result =
        APrimalCharacter_TakeDamage_original(_this, damage, damage_event, event_instigator, damage_causer);

    if (result > 0.f && _this) {
        ++g_character_damage_events;
        const Category cat = _this->IsA(AShooterCharacter::GetPrivateStaticClass()) ? Category::Player : Category::Dino;
        RecordDamage(_this, event_instigator, damage_causer, result, cat);
    }
    return result;
}

float Hook_APrimalStructure_TakeDamage(APrimalStructure* _this, float damage, FDamageEvent* damage_event,
                                        AController* event_instigator, AActor* damage_causer) {
    const float result =
        APrimalStructure_TakeDamage_original(_this, damage, damage_event, event_instigator, damage_causer);

    if (result > 0.f && _this) {
        ++g_structure_damage_events;
        RecordDamage(_this, event_instigator, damage_causer, result, Category::Structure);
    }
    return result;
}

// --- Config ---

std::string ConfigPath() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/DamageAlerts/config.json";
}

Config ParseConfig(const minijson::Value& root) {
    Config c;
    c.min_damage_to_report = minijson::number(root, "General", "MinDamageToReport", c.min_damage_to_report);
    c.flush_interval_seconds = minijson::integer(root, "General", "FlushIntervalSeconds", c.flush_interval_seconds);
    c.notify_attacker = minijson::boolean(root, "General", "NotifyAttacker", c.notify_attacker);
    c.notify_victim_tribe = minijson::boolean(root, "General", "NotifyVictimTribeOnEnemyHit", c.notify_victim_tribe);
    c.min_tribe_team_id = minijson::integer(root, "General", "MinTribeTeamId", c.min_tribe_team_id);

    c.floating_damage_enabled = minijson::boolean(root, "FloatingDamage", "Enabled", c.floating_damage_enabled);
    c.force_native_server_setting = minijson::boolean(
        root, "FloatingDamage", "ForceNativeServerSetting", c.force_native_server_setting);
    c.also_send_attacker_chat = minijson::boolean(root, "FloatingDamage", "AlsoSendAttackerChat", c.also_send_attacker_chat);
    c.floating_damage_vertical_offset = minijson::number(
        root, "FloatingDamage", "VerticalOffset", c.floating_damage_vertical_offset);
    c.test_command = minijson::str(root, "Diagnostics", "TestCommand", c.test_command);

    c.attacker_hit = minijson::str(root, "Messages", "AttackerHit", c.attacker_hit);
    c.victim_hit = minijson::str(root, "Messages", "VictimHit", c.victim_hit);

    c.flush_interval_seconds = std::max(1, c.flush_interval_seconds);
    c.min_tribe_team_id = std::max(1, c.min_tribe_team_id);
    return c;
}

void ReadConfig() {
    std::ifstream file(ConfigPath(), std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Can't open " + ConfigPath());
    std::ostringstream ss;
    ss << file.rdbuf();
    const minijson::Value root = minijson::parse(ss.str());
    if (!root.is_object()) throw std::runtime_error("config root must be a JSON object");
    g_config = ParseConfig(root);
}

void SelfTestCommand(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!pc) return;

    AShooterCharacter* character = pc->GetPlayerCharacter();
    if (character) {
        FVector location{};
        FVector extent{};
        character->GetActorBounds(false, &location, &extent);
        location.Z += g_config.floating_damage_vertical_offset;
        const int team = ArkApi::GetApiUtils().GetTribeID(pc);
        pc->ClientAddFloatingDamageText(FVector_NetQuantize(location), 12345, team);
        ++g_floating_rpc_events;
    }

    std::ostringstream status;
    status << "DamageAlerts v1.4 PureDamager OK | native="
           << (g_native_floating_applied ? "ON" : "WAIT")
           << " | character_hits=" << g_character_damage_events
           << " | structure_hits=" << g_structure_damage_events
           << " | rpc=" << g_floating_rpc_events;
    SendColored(pc, status.str(), FColorList::Cyan);
}

} // namespace DamageAlerts

void Load() {
    Log::Get().Init("DamageAlerts");
    Log::GetLog()->info("Loading plugin - DamageAlerts v1.4 PureDamager");

    try {
        DamageAlerts::ReadConfig();
    } catch (const std::exception& e) {
        Log::GetLog()->error("DamageAlerts: config error ({}), using defaults", e.what());
        DamageAlerts::g_config = DamageAlerts::Config();
    }

    ArkApi::GetHooks().SetHook("APrimalCharacter.TakeDamage",
        &DamageAlerts::Hook_APrimalCharacter_TakeDamage,
        &DamageAlerts::APrimalCharacter_TakeDamage_original);
    ArkApi::GetHooks().SetHook("APrimalStructure.TakeDamage",
        &DamageAlerts::Hook_APrimalStructure_TakeDamage,
        &DamageAlerts::APrimalStructure_TakeDamage_original);

    ArkApi::GetCommands().AddOnTimerCallback("DamageAlerts.Flush", &DamageAlerts::FlushTimer);
    if (!DamageAlerts::g_config.test_command.empty()) {
        ArkApi::GetCommands().AddChatCommand(
            FString(DamageAlerts::g_config.test_command.c_str()), &DamageAlerts::SelfTestCommand);
    }
}

void Unload() {
    if (!DamageAlerts::g_config.test_command.empty()) {
        ArkApi::GetCommands().RemoveChatCommand(FString(DamageAlerts::g_config.test_command.c_str()));
    }
    ArkApi::GetCommands().RemoveOnTimerCallback("DamageAlerts.Flush");
    ArkApi::GetHooks().DisableHook("APrimalCharacter.TakeDamage", &DamageAlerts::Hook_APrimalCharacter_TakeDamage);
    ArkApi::GetHooks().DisableHook("APrimalStructure.TakeDamage", &DamageAlerts::Hook_APrimalStructure_TakeDamage);
    DamageAlerts::g_pending_attacker.clear();
    DamageAlerts::g_pending_victim.clear();
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try { Load(); }
    catch (const std::exception& e) {
        Log::Get().Init("DamageAlerts");
        Log::GetLog()->error("DamageAlerts failed to initialize: {}", e.what());
    }
    catch (...) {
        Log::Get().Init("DamageAlerts");
        Log::GetLog()->error("DamageAlerts failed to initialize with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try { Unload(); }
    catch (const std::exception& e) {
        Log::GetLog()->error("DamageAlerts unload exception: {}", e.what());
    }
    catch (...) {
        Log::GetLog()->error("DamageAlerts unload unknown exception");
    }
}
