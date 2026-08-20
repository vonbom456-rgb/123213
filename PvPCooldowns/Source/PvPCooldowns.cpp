// PvPCooldowns -- puts a player on a PvP cooldown when they are killed by
// another tribe (a player, or that tribe's turret/dino), and exposes a
// simple query function so other plugins (e.g. TurretControl) can block
// their own commands while a player is on cooldown.
//
// Detection: hooks APrimalCharacter.TakeDamage (the standard, widely-used
// ASE hook point for player/dino damage -- confirmed against public ArkApi
// 3.56 plugin examples) and checks whether the target went from alive to
// dead across that single call. Attribution uses AActor::TargetingTeamField,
// a base-AActor field (not turret-specific) that ARK sets on players,
// dinos, structures and their projectiles alike, so it covers "a player of
// another tribe killed me" and "that tribe's turret killed me" with the
// same check, without needing to special-case turrets vs. weapons.
//
// See SOURCE_VERIFICATION.md for exactly which functions this relies on.

#include <API/ARK/Ark.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "PvPCooldownsApi.h"
#include "MiniJson.h"

namespace PvPCooldowns {

struct Config {
    int cooldown_seconds = 45;
    bool show_messages = true;
    std::string self_check_command = "/pvpcd";
    int reminder_interval_seconds = 15; // 0 disables periodic reminders
    int min_tribe_team_id = 50000;

    // A real custom icon is game content and therefore must come from a tiny
    // client/server mod.  When this class is present the plugin attaches it to
    // the respawned player and drives ARK's native buff countdown.
    bool hud_buff_enabled = true;
    std::string hud_buff_blueprint = "/Game/Mods/PvPCooldowns/Buff_PvPCooldown.Buff_PvPCooldown_C";

    std::string cooldown_started = "PvP cooldown started: {0}s. Some commands (like /fill) are blocked until it ends.";
    std::string self_check_on_cooldown = "PvP cooldown: {0}s remaining.";
    std::string self_check_none = "You are not on PvP cooldown.";
    std::string reminder = "PvP cooldown: {0}s remaining.";
};

Config g_config;

std::string ReplaceToken(const std::string& text, const std::string& token, const std::string& value) {
    std::string out = text;
    const size_t pos = out.find(token);
    if (pos != std::string::npos) out.replace(pos, token.length(), value);
    return out;
}

void Send(AShooterPlayerController* pc, const std::string& text) {
    if (!pc || text.empty()) return;
    const FString message(ArkApi::Tools::Utf8Decode(text).c_str());
    ArkApi::GetApiUtils().SendServerMessage(pc, FColorList::Yellow, *message);
}

// steam_id -> cooldown expiry
std::unordered_map<uint64, std::chrono::steady_clock::time_point> g_cooldowns;
UClass* g_hud_buff_class = nullptr;

bool IsPlayerOwnedTeam(int team) {
    return team >= g_config.min_tribe_team_id;
}

// Reject a wild dino explicitly. For projectiles/structures, requiring a
// player-team-range id keeps world/environmental damage out of PvP state.
int ResolveAttackerTeam(AController* event_instigator, AActor* damage_causer) {
    if (event_instigator) {
        if (event_instigator->IsA(AShooterPlayerController::GetPrivateStaticClass())) {
            const int team = ArkApi::GetApiUtils().GetTribeID(
                static_cast<AShooterPlayerController*>(event_instigator));
            return IsPlayerOwnedTeam(team) ? team : -1;
        }

        ACharacter* instigator_character = event_instigator->CharacterField();
        if (instigator_character &&
            instigator_character->IsA(APrimalDinoCharacter::GetPrivateStaticClass())) {
            auto* dino = static_cast<APrimalDinoCharacter*>(instigator_character);
            if (!dino->BPIsTamed()) return -1;
            const int team = dino->TargetingTeamField();
            return IsPlayerOwnedTeam(team) ? team : -1;
        }
    }

    if (damage_causer && damage_causer->IsA(APrimalDinoCharacter::GetPrivateStaticClass())) {
        auto* dino = static_cast<APrimalDinoCharacter*>(damage_causer);
        if (!dino->BPIsTamed()) return -1;
    }

    const int team = damage_causer ? damage_causer->TargetingTeamField() : -1;
    return IsPlayerOwnedTeam(team) ? team : -1;
}

void StartCooldown(AShooterPlayerController* pc) {
    if (!pc) return;
    const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
    if (steam_id == 0) return;

    g_cooldowns[steam_id] = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, g_config.cooldown_seconds));
    if (g_config.show_messages) {
        Send(pc, ReplaceToken(g_config.cooldown_started, "{0}", std::to_string(g_config.cooldown_seconds)));
    }
}

// Returns remaining seconds (0 if not on cooldown), and lazily evicts expired entries.
int RemainingSeconds(AShooterPlayerController* pc) {
    if (!pc) return 0;
    const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
    const auto it = g_cooldowns.find(steam_id);
    if (it == g_cooldowns.end()) return 0;

    const auto now = std::chrono::steady_clock::now();
    if (now >= it->second) {
        g_cooldowns.erase(it);
        return 0;
    }
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(it->second - now).count()) + 1;
}

bool IsOnCooldown(AShooterPlayerController* pc) { return RemainingSeconds(pc) > 0; }

std::chrono::steady_clock::time_point g_next_reminder{};

// Periodically re-sends the remaining time to everyone currently on
// cooldown, so it stays visible in chat rather than only appearing once
// when the cooldown started. This is a plain chat message, not an on-screen
// HUD/buff icon -- see README for why a native buff icon isn't included.
void SendReminders(const std::chrono::steady_clock::time_point& now) {
    if (g_config.reminder_interval_seconds <= 0) return;
    if (now < g_next_reminder) return;
    g_next_reminder = now + std::chrono::seconds(g_config.reminder_interval_seconds);
    if (g_cooldowns.empty()) return;

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return;

    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        auto* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);

        const int remaining = RemainingSeconds(pc);
        if (remaining > 0) {
            Send(pc, ReplaceToken(g_config.reminder, "{0}", std::to_string(remaining)));
        }
    }
}

// --- Kill detection -----------------------------------------------------

DECLARE_HOOK(APrimalCharacter_TakeDamage, float, APrimalCharacter*, float, FDamageEvent*, AController*, AActor*);

void HandlePossibleKill(AShooterPlayerController* victim_pc, int victim_team,
                        AController* event_instigator, AActor* damage_causer) {
    if (!victim_pc) return;

    const int killer_team = ResolveAttackerTeam(event_instigator, damage_causer);
    if (!IsPlayerOwnedTeam(killer_team)) return;
    if (killer_team == victim_team) return;  // friendly fire / self-damage -- no cooldown

    StartCooldown(victim_pc);
}

float Hook_APrimalCharacter_TakeDamage(APrimalCharacter* _this, float damage, FDamageEvent* damage_event,
                                        AController* event_instigator, AActor* damage_causer) {
    const bool was_dead_before = _this && _this->IsDead();
    AShooterPlayerController* victim_pc = nullptr;
    int victim_team = -1;

    // ArkApi's FindControllerFromCharacter deliberately refuses dead
    // characters. Capture both values before the original damage call.
    if (!was_dead_before && _this && _this->IsA(AShooterCharacter::GetPrivateStaticClass())) {
        victim_pc = ArkApi::GetApiUtils().FindControllerFromCharacter(
            static_cast<AShooterCharacter*>(_this));
        victim_team = _this->TargetingTeamField();
    }

    const float result =
        APrimalCharacter_TakeDamage_original(_this, damage, damage_event, event_instigator, damage_causer);

    if (!was_dead_before && _this && _this->IsDead()) {
        HandlePossibleKill(victim_pc, victim_team, event_instigator, damage_causer);
    }

    return result;
}

// --- Outbound link to TurretControl (best effort, retried until it works) ---

using TurretControlSetCheckerFn = void(__fastcall*)(PvpCooldownsIsOnCooldownFn);

bool g_turretcontrol_wired = false;

extern "C" __declspec(dllexport) bool __fastcall PvpCooldowns_IsOnCooldown(AShooterPlayerController* pc) {
    return PvPCooldowns::IsOnCooldown(pc);
}

void TryWireTurretControl() {
    HMODULE mod = GetModuleHandleA("TurretControl.dll");
    if (!mod) {
        g_turretcontrol_wired = false;
        return;
    }

    auto setter = reinterpret_cast<TurretControlSetCheckerFn>(GetProcAddress(mod, "TurretControl_SetPvpCooldownChecker"));
    if (!setter) return;

    // Re-apply every upkeep tick. This safely restores the bridge if
    // TurretControl was hot-reloaded while PvPCooldowns stayed loaded.
    setter(&PvpCooldowns_IsOnCooldown);
    if (!g_turretcontrol_wired) {
        g_turretcontrol_wired = true;
        Log::GetLog()->info("PvPCooldowns: linked with TurretControl.dll");
    }
}

void UnwireTurretControl() {
    HMODULE mod = GetModuleHandleA("TurretControl.dll");
    if (mod) {
        auto setter = reinterpret_cast<TurretControlSetCheckerFn>(
            GetProcAddress(mod, "TurretControl_SetPvpCooldownChecker"));
        if (setter) setter(nullptr);
    }
    g_turretcontrol_wired = false;
}

void LoadHudBuffClass() {
    g_hud_buff_class = nullptr;
    if (!g_config.hud_buff_enabled || g_config.hud_buff_blueprint.empty()) return;

    FString path(ArkApi::Tools::Utf8Decode(g_config.hud_buff_blueprint).c_str());
    UClass* cls = UVictoryCore::BPLoadClass(&path);
    if (!cls) {
        Log::GetLog()->warn(
            "PvPCooldowns: HUD buff class not found: '{}'; using chat timer fallback",
            g_config.hud_buff_blueprint);
        return;
    }
    UObject* cdo = cls->GetDefaultObject(true);
    if (!cdo || !cdo->IsA(APrimalBuff::StaticClass())) {
        Log::GetLog()->error(
            "PvPCooldowns: HUD class is not derived from APrimalBuff: '{}'",
            g_config.hud_buff_blueprint);
        return;
    }
    g_hud_buff_class = cls;
}

void SyncHudBuff(AShooterPlayerController* pc, int remaining) {
    if (!pc || !g_hud_buff_class) return;
    AShooterCharacter* character = pc->GetPlayerCharacter();
    if (!character || character->IsDead()) return;

    TSubclassOf<APrimalBuff> buff_class(g_hud_buff_class);
    APrimalBuff* existing = character->GetBuff(buff_class);
    if (remaining <= 0) {
        if (existing) existing->Deactivate();
        return;
    }
    if (existing) return;

    APrimalBuff* buff = APrimalBuff::StaticAddBuff(
        buff_class, character, nullptr, character, false);
    if (!buff) {
        Log::GetLog()->warn("PvPCooldowns: failed to attach HUD buff to player");
        return;
    }

    buff->DeactivateAfterTimeField() = static_cast<float>(remaining);
    buff->bHideBuffFromHUD() = false;
    buff->bHideTimerFromHUD() = false;
    buff->bHUDFormatTimerAsTimecode() = false;
    buff->ResetBuffStart();
}

// --- Periodic upkeep: retry linking, evict stale cooldown entries ---

void UpkeepTimer() {
    TryWireTurretControl();

    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_cooldowns.begin(); it != g_cooldowns.end();) {
        it = (now >= it->second) ? g_cooldowns.erase(it) : std::next(it);
    }

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (world && g_hud_buff_class) {
        for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
            auto* base_pc = weak_pc.Get();
            if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
            auto* pc = static_cast<AShooterPlayerController*>(base_pc);
            SyncHudBuff(pc, RemainingSeconds(pc));
        }
    }

    SendReminders(now);
}

// --- Optional self-check chat command ---

void SelfCheckCommand(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!pc) return;
    const int remaining = RemainingSeconds(pc);
    Send(pc, remaining > 0
        ? ReplaceToken(g_config.self_check_on_cooldown, "{0}", std::to_string(remaining))
        : g_config.self_check_none);
}

// --- Config ---

std::string ConfigPath() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/PvPCooldowns/config.json";
}

Config ParseConfig(const minijson::Value& root) {
    Config c;
    c.cooldown_seconds = minijson::integer(root, "General", "CooldownSeconds", c.cooldown_seconds);
    c.show_messages = minijson::boolean(root, "General", "ShowMessages", c.show_messages);
    c.self_check_command = minijson::str(root, "General", "SelfCheckCommand", c.self_check_command);
    c.reminder_interval_seconds = minijson::integer(root, "General", "ReminderIntervalSeconds", c.reminder_interval_seconds);
    c.min_tribe_team_id = minijson::integer(root, "General", "MinTribeTeamId", c.min_tribe_team_id);
    c.hud_buff_enabled = minijson::boolean(root, "HudBuff", "Enabled", c.hud_buff_enabled);
    c.hud_buff_blueprint = minijson::str(root, "HudBuff", "BlueprintPath", c.hud_buff_blueprint);

    c.cooldown_started = minijson::str(root, "Messages", "CooldownStarted", c.cooldown_started);
    c.self_check_on_cooldown = minijson::str(root, "Messages", "SelfCheckOnCooldown", c.self_check_on_cooldown);
    c.self_check_none = minijson::str(root, "Messages", "SelfCheckNone", c.self_check_none);
    c.reminder = minijson::str(root, "Messages", "Reminder", c.reminder);

    c.cooldown_seconds = std::max(1, c.cooldown_seconds);
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

} // namespace PvPCooldowns

void Load() {
    Log::Get().Init("PvPCooldowns");
    Log::GetLog()->info("Loading plugin - PvPCooldowns v1.1");

    try {
        PvPCooldowns::ReadConfig();
    } catch (const std::exception& e) {
        Log::GetLog()->error("PvPCooldowns: config error ({}), using defaults", e.what());
        PvPCooldowns::g_config = PvPCooldowns::Config();
    }
    PvPCooldowns::LoadHudBuffClass();

    ArkApi::GetHooks().SetHook("APrimalCharacter.TakeDamage",
        &PvPCooldowns::Hook_APrimalCharacter_TakeDamage,
        &PvPCooldowns::APrimalCharacter_TakeDamage_original);

    if (!PvPCooldowns::g_config.self_check_command.empty()) {
        ArkApi::GetCommands().AddChatCommand(
            FString(PvPCooldowns::g_config.self_check_command.c_str()),
            &PvPCooldowns::SelfCheckCommand);
    }

    PvPCooldowns::TryWireTurretControl(); // in case TurretControl already loaded first
    ArkApi::GetCommands().AddOnTimerCallback("PvPCooldowns.Upkeep", &PvPCooldowns::UpkeepTimer);
}

void Unload() {
    PvPCooldowns::UnwireTurretControl();
    ArkApi::GetCommands().RemoveOnTimerCallback("PvPCooldowns.Upkeep");
    if (!PvPCooldowns::g_config.self_check_command.empty()) {
        ArkApi::GetCommands().RemoveChatCommand(FString(PvPCooldowns::g_config.self_check_command.c_str()));
    }
    ArkApi::GetHooks().DisableHook("APrimalCharacter.TakeDamage", &PvPCooldowns::Hook_APrimalCharacter_TakeDamage);
    PvPCooldowns::g_hud_buff_class = nullptr;
    PvPCooldowns::g_cooldowns.clear();
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try { Load(); }
    catch (const std::exception& e) {
        Log::Get().Init("PvPCooldowns");
        Log::GetLog()->error("PvPCooldowns failed to initialize: {}", e.what());
    }
    catch (...) {
        Log::Get().Init("PvPCooldowns");
        Log::GetLog()->error("PvPCooldowns failed to initialize with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try { Unload(); }
    catch (const std::exception& e) {
        Log::GetLog()->error("PvPCooldowns unload exception: {}", e.what());
    }
    catch (...) {
        Log::GetLog()->error("PvPCooldowns unload unknown exception");
    }
}
