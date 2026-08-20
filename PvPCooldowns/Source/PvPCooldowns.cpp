// PvPCooldowns -- puts both tribes on a PvP/raid cooldown when one tribe
// damages another tribe's player, tame or structure, and exposes a
// simple query function so other plugins (e.g. TurretControl) can block
// their own commands while a player is on cooldown.
//
// Detection: hooks character and structure TakeDamage, then starts or extends
// the tag after real enemy damage is applied. Attribution prefers the player
// controller, handles tamed dino instigators explicitly and otherwise falls
// back to the damage causer's player-team id. Wild/environment/friendly damage
// is rejected.
//
// See SOURCE_VERIFICATION.md for exactly which functions this relies on.

#include <API/ARK/Ark.h>
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "PvPCooldownsApi.h"
#include "MiniJson.h"

namespace PvPCooldowns {

struct Config {
    int cooldown_seconds = 45;
    float min_damage_to_trigger = 1.0f;
    bool show_messages = true;
    std::string self_check_command = "/pvpcd";
    std::string test_command = "/pvpcdtest";
    std::string icon_test_command = "/pvpcdicon";
    int reminder_interval_seconds = 5; // 0 disables periodic reminders
    int min_tribe_team_id = 50000;

    bool hud_notification_enabled = true;
    float hud_display_scale = 1.1f;
    float hud_display_time = 4.0f;
    std::string hud_icon = "players";

    // A real custom icon is game content and therefore must come from a tiny
    // client/server mod.  When this class is present the plugin attaches it to
    // the respawned player and drives ARK's native buff countdown.
    bool hud_buff_enabled = false;
    std::string hud_buff_blueprint;

    std::string cooldown_started = "RAID / PVP started: {0}s. /fill and protected commands are blocked.";
    std::string self_check_on_cooldown = "RAID / PVP: {0}s remaining.";
    std::string self_check_none = "RAID / PVP timer is not active.";
    std::string reminder = "RAID / PVP: {0}s remaining.";
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

UTexture2D* GetHudIcon(AShooterPlayerController* pc, std::string name) {
    if (!pc) return nullptr;
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "default") return pc->PingIcon_DefaultField();
    if (name == "dinos") return pc->PingIcon_DinosField();
    if (name == "structures") return pc->PingIcon_StructuresField();
    return pc->PingIcon_PlayersField();
}

void SendHud(AShooterPlayerController* pc, const std::string& text, float display_time = -1.0f,
             const std::string& icon_name = "") {
    if (!pc || text.empty() || !g_config.hud_notification_enabled) return;
    const FString message(ArkApi::Tools::Utf8Decode(text).c_str());
    UTexture2D* icon = GetHudIcon(pc, icon_name.empty() ? g_config.hud_icon : icon_name);
    ArkApi::GetApiUtils().SendNotification(
        pc, FColorList::Red, g_config.hud_display_scale,
        display_time > 0.0f ? display_time : g_config.hud_display_time,
        icon, *message);
}

// steam_id -> cooldown expiry
std::unordered_map<uint64, std::chrono::steady_clock::time_point> g_cooldowns;
UClass* g_hud_buff_class = nullptr;

bool IsPlayerOwnedTeam(int team) {
    return team >= g_config.min_tribe_team_id;
}

// Reject a wild dino explicitly. For projectiles/structures, requiring a
// player-team-range id keeps world/environmental damage out of PvP state.
struct AttackerInfo {
    int team = -1;
    AShooterPlayerController* player = nullptr;
};

AttackerInfo ResolveAttacker(AController* event_instigator, AActor* damage_causer) {
    AttackerInfo result;
    if (event_instigator) {
        if (event_instigator->IsA(AShooterPlayerController::GetPrivateStaticClass())) {
            result.player = static_cast<AShooterPlayerController*>(event_instigator);
            result.team = ArkApi::GetApiUtils().GetTribeID(result.player);
            if (!IsPlayerOwnedTeam(result.team)) result.team = -1;
            return result;
        }

        ACharacter* instigator_character = event_instigator->CharacterField();
        if (instigator_character &&
            instigator_character->IsA(APrimalDinoCharacter::GetPrivateStaticClass())) {
            auto* dino = static_cast<APrimalDinoCharacter*>(instigator_character);
            if (!dino->BPIsTamed()) return result;
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

void StartCooldown(AShooterPlayerController* pc) {
    if (!pc) return;
    const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
    if (steam_id == 0) return;

    const auto now = std::chrono::steady_clock::now();
    const auto existing = g_cooldowns.find(steam_id);
    const bool was_active = existing != g_cooldowns.end() && now < existing->second;
    g_cooldowns[steam_id] = now + std::chrono::seconds(std::max(1, g_config.cooldown_seconds));
    if (!was_active && g_config.show_messages) {
        const std::string message = ReplaceToken(
            g_config.cooldown_started, "{0}", std::to_string(g_config.cooldown_seconds));
        Send(pc, message);
        SendHud(pc, message);
    }
}

void StartCooldownForTribe(int team) {
    if (!IsPlayerOwnedTeam(team)) return;
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return;
    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        auto* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        if (ArkApi::GetApiUtils().GetTribeID(pc) == team) StartCooldown(pc);
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
// cooldown, so both chat and the native HUD notification show the countdown.
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
            const std::string message = ReplaceToken(g_config.reminder, "{0}", std::to_string(remaining));
            Send(pc, message);
            SendHud(pc, message);
        }
    }
}

// --- Enemy damage / raid detection --------------------------------------

DECLARE_HOOK(APrimalCharacter_TakeDamage, float, APrimalCharacter*, float, FDamageEvent*, AController*, AActor*);
DECLARE_HOOK(APrimalStructure_TakeDamage, float, APrimalStructure*, float, FDamageEvent*, AController*, AActor*);

void HandleEnemyDamage(int victim_team, AController* event_instigator, AActor* damage_causer, float applied_damage) {
    if (applied_damage < g_config.min_damage_to_trigger || !IsPlayerOwnedTeam(victim_team)) return;
    const AttackerInfo attacker = ResolveAttacker(event_instigator, damage_causer);
    if (!IsPlayerOwnedTeam(attacker.team) || attacker.team == victim_team) return;

    // Tag every online member of both tribes. This makes structure/turret
    // raids work even when the actual damage causer has no player controller.
    StartCooldownForTribe(victim_team);
    StartCooldownForTribe(attacker.team);
    if (attacker.player) StartCooldown(attacker.player);
}

float Hook_APrimalCharacter_TakeDamage(APrimalCharacter* _this, float damage, FDamageEvent* damage_event,
                                        AController* event_instigator, AActor* damage_causer) {
    const int victim_team = _this ? _this->TargetingTeamField() : -1;

    const float result =
        APrimalCharacter_TakeDamage_original(_this, damage, damage_event, event_instigator, damage_causer);

    HandleEnemyDamage(victim_team, event_instigator, damage_causer, result);

    return result;
}

float Hook_APrimalStructure_TakeDamage(APrimalStructure* _this, float damage, FDamageEvent* damage_event,
                                        AController* event_instigator, AActor* damage_causer) {
    const int victim_team = _this ? _this->TargetingTeamField() : -1;
    const float result =
        APrimalStructure_TakeDamage_original(_this, damage, damage_event, event_instigator, damage_causer);

    HandleEnemyDamage(victim_team, event_instigator, damage_causer, result);

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

void TestCommand(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!pc) return;
    StartCooldown(pc);
    Send(pc, "PvPCooldowns v1.2 test started. Use /pvpcd to inspect the timer.");
}

void IconTestCommand(AShooterPlayerController* pc, FString* message, EChatSendMode::Type) {
    if (!pc || !message) return;
    std::istringstream input(message->ToString());
    std::string command;
    std::string icon;
    input >> command >> icon;
    std::transform(icon.begin(), icon.end(), icon.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (icon != "default" && icon != "players" && icon != "dinos" && icon != "structures") {
        Send(pc, "Usage: /pvpcdicon default|players|dinos|structures");
        return;
    }
    SendHud(pc, "RAID / PVP icon preview: " + icon, 8.0f, icon);
    Send(pc, "Showing built-in ARK icon: " + icon);
}

// --- Config ---

std::string ConfigPath() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/PvPCooldowns/config.json";
}

Config ParseConfig(const minijson::Value& root) {
    Config c;
    c.cooldown_seconds = minijson::integer(root, "General", "CooldownSeconds", c.cooldown_seconds);
    c.min_damage_to_trigger = minijson::number(root, "General", "MinDamageToTrigger", c.min_damage_to_trigger);
    c.show_messages = minijson::boolean(root, "General", "ShowMessages", c.show_messages);
    c.self_check_command = minijson::str(root, "General", "SelfCheckCommand", c.self_check_command);
    c.test_command = minijson::str(root, "General", "TestCommand", c.test_command);
    c.icon_test_command = minijson::str(root, "General", "IconTestCommand", c.icon_test_command);
    c.reminder_interval_seconds = minijson::integer(root, "General", "ReminderIntervalSeconds", c.reminder_interval_seconds);
    c.min_tribe_team_id = minijson::integer(root, "General", "MinTribeTeamId", c.min_tribe_team_id);
    c.hud_notification_enabled = minijson::boolean(root, "HudNotification", "Enabled", c.hud_notification_enabled);
    c.hud_display_scale = minijson::number(root, "HudNotification", "DisplayScale", c.hud_display_scale);
    c.hud_display_time = minijson::number(root, "HudNotification", "DisplayTime", c.hud_display_time);
    c.hud_icon = minijson::str(root, "HudNotification", "Icon", c.hud_icon);
    c.hud_buff_enabled = minijson::boolean(root, "HudBuff", "Enabled", c.hud_buff_enabled);
    c.hud_buff_blueprint = minijson::str(root, "HudBuff", "BlueprintPath", c.hud_buff_blueprint);

    c.cooldown_started = minijson::str(root, "Messages", "CooldownStarted", c.cooldown_started);
    c.self_check_on_cooldown = minijson::str(root, "Messages", "SelfCheckOnCooldown", c.self_check_on_cooldown);
    c.self_check_none = minijson::str(root, "Messages", "SelfCheckNone", c.self_check_none);
    c.reminder = minijson::str(root, "Messages", "Reminder", c.reminder);

    c.cooldown_seconds = std::max(1, c.cooldown_seconds);
    c.min_damage_to_trigger = std::max(0.0f, c.min_damage_to_trigger);
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
    Log::GetLog()->info("Loading plugin - PvPCooldowns v1.2 RaidCombat");

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
    ArkApi::GetHooks().SetHook("APrimalStructure.TakeDamage",
        &PvPCooldowns::Hook_APrimalStructure_TakeDamage,
        &PvPCooldowns::APrimalStructure_TakeDamage_original);

    if (!PvPCooldowns::g_config.self_check_command.empty()) {
        ArkApi::GetCommands().AddChatCommand(
            FString(PvPCooldowns::g_config.self_check_command.c_str()),
            &PvPCooldowns::SelfCheckCommand);
    }
    if (!PvPCooldowns::g_config.test_command.empty()) {
        ArkApi::GetCommands().AddChatCommand(
            FString(PvPCooldowns::g_config.test_command.c_str()),
            &PvPCooldowns::TestCommand);
    }
    if (!PvPCooldowns::g_config.icon_test_command.empty()) {
        ArkApi::GetCommands().AddChatCommand(
            FString(PvPCooldowns::g_config.icon_test_command.c_str()),
            &PvPCooldowns::IconTestCommand);
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
    if (!PvPCooldowns::g_config.test_command.empty()) {
        ArkApi::GetCommands().RemoveChatCommand(FString(PvPCooldowns::g_config.test_command.c_str()));
    }
    if (!PvPCooldowns::g_config.icon_test_command.empty()) {
        ArkApi::GetCommands().RemoveChatCommand(FString(PvPCooldowns::g_config.icon_test_command.c_str()));
    }
    ArkApi::GetHooks().DisableHook("APrimalCharacter.TakeDamage", &PvPCooldowns::Hook_APrimalCharacter_TakeDamage);
    ArkApi::GetHooks().DisableHook("APrimalStructure.TakeDamage", &PvPCooldowns::Hook_APrimalStructure_TakeDamage);
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
