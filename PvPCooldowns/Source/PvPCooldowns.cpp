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
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "PvPCooldownsApi.h"
#include "MiniJson.h"

namespace PvPCooldowns {

struct Config {
    float min_damage_to_trigger = 1.0f;
    bool show_messages = true;
    std::string self_check_command = "/pvpcd";
    std::string test_command = "/pvpcdtest";
    std::string icon_test_command = "/pvpcdicon";
    int reminder_interval_seconds = 5; // 0 disables periodic reminders
    int min_tribe_team_id = 50000;

    int player_damage_seconds = 80;
    int tame_damage_seconds = 80;
    int structure_damage_seconds = 300;
    int player_kill_seconds = 300;
    int tame_kill_seconds = 300;
    int structure_destroyed_seconds = 300;
    int turret_kill_seconds = 300;
    int test_seconds = 80;

    // DuoRaidCore features.
    bool persistence_enabled = true;
    std::string state_file = "state.json";
    bool tag_offline_tribe_members = true;

    bool block_shop_commands = true;
    std::vector<std::string> blocked_commands = {
        "/shop", "/shopfind", "/buy", "/kit", "/buykit"
    };

    bool combat_logout_enabled = true;
    bool combat_logout_kill_character = true;
    int combat_logout_penalty_seconds = 600;

    bool soft_orp_enabled = true;
    int soft_orp_grace_seconds = 900;
    float soft_orp_damage_multiplier = 0.25f;

    bool hud_notification_enabled = true;
    float hud_display_scale = 1.1f;
    float hud_display_time = 4.0f;
    std::string hud_icon = "players";

    // A real custom icon is game content and therefore must come from a tiny
    // client/server mod.  When this class is present the plugin attaches it to
    // the respawned player and drives ARK's native buff countdown.
    bool hud_buff_enabled = false;
    std::string hud_buff_blueprint;

    std::string cooldown_started = "RAID / PVP: {0}s ({1}). /fill and protected commands are blocked.";
    std::string cooldown_escalated = "RAID / PVP escalated to {0}s ({1}).";
    std::string self_check_on_cooldown = "RAID / PVP: {0}s remaining.";
    std::string self_check_none = "RAID / PVP timer is not active.";
    std::string reminder = "RAID / PVP: {0}s remaining.";
    std::string command_blocked = "RAID / PVP: {0}s remaining. Command is blocked.";
    std::string combat_logout = "Combat logout detected: character killed, RAID / PVP extended to {0}s.";
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

struct CooldownState {
    std::int64_t expiry_unix = 0;
    int severity_seconds = 0;
    int team = -1;
};

// Stable player data id -> cooldown. For the rare pre-spawn case, a Steam ID
// is stored with the high bit set so it cannot collide with ARK player ids.
std::unordered_map<uint64, CooldownState> g_cooldowns;
std::unordered_map<int, std::int64_t> g_tribe_expiry;
std::unordered_map<int, std::int64_t> g_tribe_last_online;
bool g_state_dirty = false;
std::int64_t g_next_state_save = 0;
bool g_unloading = false;
UClass* g_hud_buff_class = nullptr;

std::int64_t UnixNow() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64 PlayerKey(AShooterPlayerController* pc) {
    if (!pc) return 0;
    const uint64 player_id = pc->LinkedPlayerIDField();
    if (player_id != 0) return player_id;
    const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
    return steam_id == 0 ? 0 : (steam_id | (uint64(1) << 63));
}

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

std::string ActorNameLower(AActor* actor) {
    if (!actor) return {};
    FString name;
    actor->GetFullName(&name, nullptr);
    std::string out = name.ToString();
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool ActorOrOwnerIsTurret(AActor* actor) {
    for (int depth = 0; actor && depth < 4; ++depth) {
        if (actor->IsA(APrimalStructureTurret::GetPrivateStaticClass())) return true;
        if (ActorNameLower(actor).find("turret") != std::string::npos) return true;
        actor = actor->OwnerField();
    }
    return false;
}

bool IsTurretDamage(AController* event_instigator, AActor* damage_causer) {
    return ActorOrOwnerIsTurret(damage_causer) || ActorOrOwnerIsTurret(event_instigator);
}

void StartCooldownByKey(uint64 key, int team, int seconds) {
    if (key == 0) return;
    const std::int64_t now = UnixNow();
    const auto existing = g_cooldowns.find(key);
    const bool was_active = existing != g_cooldowns.end() && now < existing->second.expiry_unix;
    const int duration = std::max(1, seconds);
    const bool escalated = was_active && duration > existing->second.severity_seconds;
    CooldownState& state = g_cooldowns[key];
    if (!was_active) state = CooldownState{};
    const std::int64_t new_expiry = std::max(state.expiry_unix, now + duration);
    const int new_severity = std::max(state.severity_seconds, duration);
    const int new_team = IsPlayerOwnedTeam(team) ? team : state.team;
    if (new_expiry != state.expiry_unix || new_severity != state.severity_seconds || new_team != state.team) {
        state.expiry_unix = new_expiry;
        state.severity_seconds = new_severity;
        state.team = new_team;
        g_state_dirty = true;
    }
    (void)escalated;
}

void StartCooldown(AShooterPlayerController* pc, int seconds, const std::string& reason) {
    if (!pc) return;
    const uint64 key = PlayerKey(pc);
    if (key == 0) return;

    const std::int64_t now = UnixNow();
    const auto existing = g_cooldowns.find(key);
    const bool was_active = existing != g_cooldowns.end() && now < existing->second.expiry_unix;
    const int duration = std::max(1, seconds);
    const bool escalated = was_active && duration > existing->second.severity_seconds;
    const int team = ArkApi::GetApiUtils().GetTribeID(pc);
    StartCooldownByKey(key, team, duration);
    if ((!was_active || escalated) && g_config.show_messages) {
        std::string message = ReplaceToken(
            escalated ? g_config.cooldown_escalated : g_config.cooldown_started,
            "{0}", std::to_string(duration));
        message = ReplaceToken(message, "{1}", reason);
        Send(pc, message);
        SendHud(pc, message);
    }
}

FTribeData* FindLoadedTribeData(int team) {
    AShooterGameMode* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
    if (!game_mode) return nullptr;
    for (FTribeData& tribe : game_mode->TribesDataField()) {
        if (tribe.TribeIDField() == team) return &tribe;
    }
    return nullptr;
}

void StartCooldownForTribe(int team, int seconds, const std::string& reason) {
    if (!IsPlayerOwnedTeam(team)) return;
    const std::int64_t expiry = UnixNow() + std::max(1, seconds);
    g_tribe_expiry[team] = std::max(g_tribe_expiry[team], expiry);
    g_state_dirty = true;

    if (g_config.tag_offline_tribe_members) {
        if (FTribeData* tribe = FindLoadedTribeData(team)) {
            for (const unsigned int player_id : tribe->MembersPlayerDataIDField()) {
                StartCooldownByKey(static_cast<uint64>(player_id), team, seconds);
            }
        }
    }

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return;
    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        auto* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        if (ArkApi::GetApiUtils().GetTribeID(pc) == team) StartCooldown(pc, seconds, reason);
    }
}

// Returns remaining seconds (0 if not on cooldown), and lazily evicts expired entries.
int RemainingSeconds(AShooterPlayerController* pc) {
    if (!pc) return 0;
    const uint64 key = PlayerKey(pc);
    const auto it = g_cooldowns.find(key);
    if (it == g_cooldowns.end()) return 0;

    const std::int64_t now = UnixNow();
    if (now >= it->second.expiry_unix) {
        g_cooldowns.erase(it);
        g_state_dirty = true;
        return 0;
    }
    return static_cast<int>(it->second.expiry_unix - now);
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

bool TribeHasOnlineMember(int team) {
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return false;
    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        auto* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        if (ArkApi::GetApiUtils().GetTribeID(pc) == team) return true;
    }
    return false;
}

float ApplySoftOrp(int victim_team, float damage, AController* event_instigator, AActor* damage_causer) {
    if (!g_config.soft_orp_enabled || damage <= 0.0f || !IsPlayerOwnedTeam(victim_team)) return damage;
    const AttackerInfo attacker = ResolveAttacker(event_instigator, damage_causer);
    if (!IsPlayerOwnedTeam(attacker.team) || attacker.team == victim_team) return damage;
    if (TribeHasOnlineMember(victim_team)) return damage;

    const std::int64_t now = UnixNow();
    auto it = g_tribe_last_online.find(victim_team);
    if (it == g_tribe_last_online.end()) {
        g_tribe_last_online[victim_team] = now;
        g_state_dirty = true;
        return damage;
    }
    if (now - it->second < g_config.soft_orp_grace_seconds) return damage;
    return damage * g_config.soft_orp_damage_multiplier;
}

DECLARE_HOOK(APrimalCharacter_TakeDamage, float, APrimalCharacter*, float, FDamageEvent*, AController*, AActor*);
DECLARE_HOOK(APrimalStructure_TakeDamage, float, APrimalStructure*, float, FDamageEvent*, AController*, AActor*);

void HandleEnemyDamage(int victim_team, AController* event_instigator, AActor* damage_causer,
                       float applied_damage, int seconds, const std::string& reason) {
    if (applied_damage < g_config.min_damage_to_trigger || !IsPlayerOwnedTeam(victim_team)) return;
    const AttackerInfo attacker = ResolveAttacker(event_instigator, damage_causer);
    if (!IsPlayerOwnedTeam(attacker.team) || attacker.team == victim_team) return;

    // Tag every online member of both tribes. This makes structure/turret
    // raids work even when the actual damage causer has no player controller.
    StartCooldownForTribe(victim_team, seconds, reason);
    StartCooldownForTribe(attacker.team, seconds, reason);
    if (attacker.player) StartCooldown(attacker.player, seconds, reason);
}

float Hook_APrimalCharacter_TakeDamage(APrimalCharacter* _this, float damage, FDamageEvent* damage_event,
                                        AController* event_instigator, AActor* damage_causer) {
    const int victim_team = _this ? _this->TargetingTeamField() : -1;
    const bool was_dead = !_this || _this->IsDead();
    const bool is_player = _this && _this->IsA(AShooterCharacter::GetPrivateStaticClass());
    const bool is_tame = _this && _this->IsA(APrimalDinoCharacter::GetPrivateStaticClass()) &&
        static_cast<APrimalDinoCharacter*>(_this)->BPIsTamed();

    // ORP protects tames, but intentionally does not make sleeping survivors
    // invulnerable. Combat-log punishment handles players separately.
    const float adjusted_damage = is_tame
        ? ApplySoftOrp(victim_team, damage, event_instigator, damage_causer) : damage;
    const float result = APrimalCharacter_TakeDamage_original(
        _this, adjusted_damage, damage_event, event_instigator, damage_causer);

    const bool killed = _this && !was_dead && _this->IsDead();
    int seconds = is_player ? g_config.player_damage_seconds : g_config.tame_damage_seconds;
    std::string reason = is_player ? "player damage" : "tame damage";
    if (killed) {
        if (IsTurretDamage(event_instigator, damage_causer)) {
            seconds = g_config.turret_kill_seconds;
            reason = "turret kill";
        } else if (is_player) {
            seconds = g_config.player_kill_seconds;
            reason = "player killed";
        } else if (is_tame) {
            seconds = g_config.tame_kill_seconds;
            reason = "tame killed";
        }
    }
    if (is_player || is_tame)
        HandleEnemyDamage(victim_team, event_instigator, damage_causer, result, seconds, reason);

    return result;
}

float Hook_APrimalStructure_TakeDamage(APrimalStructure* _this, float damage, FDamageEvent* damage_event,
                                        AController* event_instigator, AActor* damage_causer) {
    const int victim_team = _this ? _this->TargetingTeamField() : -1;
    const bool was_dead = !_this || _this->IsDead();
    const float result = APrimalStructure_TakeDamage_original(
        _this, ApplySoftOrp(victim_team, damage, event_instigator, damage_causer),
        damage_event, event_instigator, damage_causer);

    const bool destroyed = _this && !was_dead && _this->IsDead();
    HandleEnemyDamage(
        victim_team, event_instigator, damage_causer, result,
        destroyed ? g_config.structure_destroyed_seconds : g_config.structure_damage_seconds,
        destroyed ? "structure destroyed" : "structure damage");

    return result;
}

// --- Persistent raid state, Ark Shop blocking and combat logout ---------

std::string StatePath() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/PvPCooldowns/" + g_config.state_file;
}

void SaveState(bool force = false) {
    if (!g_config.persistence_enabled || (!force && !g_state_dirty)) return;
    const std::int64_t now = UnixNow();
    if (!force && now < g_next_state_save) return;
    g_next_state_save = now + 2;

    const std::string path = StatePath();
    const std::string temp = path + ".tmp";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        Log::GetLog()->error("PvPCooldowns: cannot write state file {}", temp);
        return;
    }

    file << "{\n  \"players\": [\n";
    bool first = true;
    for (const auto& entry : g_cooldowns) {
        if (entry.second.expiry_unix <= now) continue;
        if (!first) file << ",\n";
        first = false;
        file << "    {\"id\":\"" << entry.first << "\",\"expiry\":"
             << entry.second.expiry_unix << ",\"severity\":"
             << entry.second.severity_seconds << ",\"team\":" << entry.second.team << "}";
    }
    file << "\n  ],\n  \"tribes\": [\n";
    first = true;
    std::unordered_set<int> teams;
    for (const auto& entry : g_tribe_expiry) teams.insert(entry.first);
    for (const auto& entry : g_tribe_last_online) teams.insert(entry.first);
    for (const int team : teams) {
        if (!first) file << ",\n";
        first = false;
        file << "    {\"id\":" << team << ",\"expiry\":" << g_tribe_expiry[team]
             << ",\"lastOnline\":" << g_tribe_last_online[team] << "}";
    }
    file << "\n  ]\n}\n";
    file.close();
    if (!file) {
        Log::GetLog()->error("PvPCooldowns: failed while writing state file {}", temp);
        return;
    }
    if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log::GetLog()->error("PvPCooldowns: cannot replace state file {} (Windows error {})", path, GetLastError());
        return;
    }
    g_state_dirty = false;
}

void LoadState() {
    if (!g_config.persistence_enabled) return;
    std::ifstream file(StatePath(), std::ios::binary);
    if (!file.is_open()) {
        Log::GetLog()->info("PvPCooldowns: no previous state file; starting clean");
        return;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const minijson::Value root = minijson::parse(ss.str());
    const std::int64_t now = UnixNow();

    if (const minijson::Value* players = root.find("players"); players && players->is_array()) {
        for (const minijson::Value& item : players->array()) {
            if (!item.is_object()) continue;
            const minijson::Value* id_value = item.find("id");
            const minijson::Value* expiry_value = item.find("expiry");
            if (!id_value || !id_value->is_string() || !expiry_value || !expiry_value->is_number()) continue;
            try {
                const uint64 id = std::stoull(id_value->string());
                CooldownState state;
                state.expiry_unix = static_cast<std::int64_t>(expiry_value->number());
                if (const auto* v = item.find("severity")) state.severity_seconds = v->get_int(0);
                if (const auto* v = item.find("team")) state.team = v->get_int(-1);
                if (id != 0 && state.expiry_unix > now) g_cooldowns[id] = state;
            } catch (...) {}
        }
    }
    if (const minijson::Value* tribes = root.find("tribes"); tribes && tribes->is_array()) {
        for (const minijson::Value& item : tribes->array()) {
            if (!item.is_object()) continue;
            const int team = item.find("id") ? item.find("id")->get_int(-1) : -1;
            if (!IsPlayerOwnedTeam(team)) continue;
            if (const auto* v = item.find("expiry")) {
                const std::int64_t expiry = static_cast<std::int64_t>(v->is_number() ? v->number() : 0);
                if (expiry > now) g_tribe_expiry[team] = expiry;
            }
            if (const auto* v = item.find("lastOnline")) {
                g_tribe_last_online[team] = static_cast<std::int64_t>(v->is_number() ? v->number() : 0);
            }
        }
    }
    Log::GetLog()->info("PvPCooldowns: restored {} player cooldown(s) and {} tribe raid state(s)",
        g_cooldowns.size(), g_tribe_expiry.size());
}

std::string FirstWordLower(FString* message) {
    if (!message) return {};
    std::istringstream input(message->ToString());
    std::string word;
    input >> word;
    std::transform(word.begin(), word.end(), word.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return word;
}

bool IsBlockedCommand(const std::string& command) {
    if (!g_config.block_shop_commands) return false;
    for (std::string configured : g_config.blocked_commands) {
        std::transform(configured.begin(), configured.end(), configured.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (command == configured) return true;
    }
    return false;
}

DECLARE_HOOK(AShooterPlayerController_ServerSendChatMessage_Implementation, void,
    AShooterPlayerController*, FString*, EChatSendMode::Type);
DECLARE_HOOK(AShooterGameMode_Logout, void, AShooterGameMode*, AController*);

void Hook_AShooterPlayerController_ServerSendChatMessage_Implementation(
    AShooterPlayerController* pc, FString* message, EChatSendMode::Type mode) {
    if (pc && IsBlockedCommand(FirstWordLower(message))) {
        const int remaining = RemainingSeconds(pc);
        if (remaining > 0) {
            Send(pc, ReplaceToken(g_config.command_blocked, "{0}", std::to_string(remaining)));
            SendHud(pc, ReplaceToken(g_config.command_blocked, "{0}", std::to_string(remaining)));
            return;
        }
    }
    AShooterPlayerController_ServerSendChatMessage_Implementation_original(pc, message, mode);
}

void Hook_AShooterGameMode_Logout(AShooterGameMode* game_mode, AController* exiting) {
    auto* pc = exiting && exiting->IsA(AShooterPlayerController::GetPrivateStaticClass())
        ? static_cast<AShooterPlayerController*>(exiting) : nullptr;
    if (!g_unloading && g_config.combat_logout_enabled && pc && RemainingSeconds(pc) > 0) {
        const int team = ArkApi::GetApiUtils().GetTribeID(pc);
        StartCooldownByKey(PlayerKey(pc), team, g_config.combat_logout_penalty_seconds);
        if (IsPlayerOwnedTeam(team)) {
            g_tribe_expiry[team] = std::max(g_tribe_expiry[team],
                UnixNow() + g_config.combat_logout_penalty_seconds);
        }
        SaveState(true);
        Log::GetLog()->warn("PvPCooldowns: combat logout punished (steam {}, player id {}, tribe {})",
            ArkApi::IApiUtils::GetSteamIdFromController(pc), pc->LinkedPlayerIDField(), team);
        AShooterCharacter* character = pc->GetPlayerCharacter();
        if (g_config.combat_logout_kill_character && character && !character->IsDead()) {
            character->Suicide();
        }
    }
    AShooterGameMode_Logout_original(game_mode, exiting);
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

    const std::int64_t unix_now = UnixNow();
    for (auto it = g_cooldowns.begin(); it != g_cooldowns.end();) {
        if (unix_now >= it->second.expiry_unix) {
            it = g_cooldowns.erase(it);
            g_state_dirty = true;
        } else {
            ++it;
        }
    }
    for (auto it = g_tribe_expiry.begin(); it != g_tribe_expiry.end();) {
        if (unix_now >= it->second) {
            it = g_tribe_expiry.erase(it);
            g_state_dirty = true;
        } else {
            ++it;
        }
    }

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (world) {
        for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
            auto* base_pc = weak_pc.Get();
            if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
            auto* pc = static_cast<AShooterPlayerController*>(base_pc);
            const int team = ArkApi::GetApiUtils().GetTribeID(pc);
            if (IsPlayerOwnedTeam(team)) {
                const auto seen = g_tribe_last_online.find(team);
                if (seen == g_tribe_last_online.end() || unix_now - seen->second >= 30) {
                    g_tribe_last_online[team] = unix_now;
                    g_state_dirty = true;
                }
                const auto raid = g_tribe_expiry.find(team);
                if (raid != g_tribe_expiry.end() && raid->second > unix_now) {
                    StartCooldown(pc, static_cast<int>(raid->second - unix_now), "offline tribe raid restored");
                }
            }
            if (g_hud_buff_class) SyncHudBuff(pc, RemainingSeconds(pc));
        }
    }

    SendReminders(std::chrono::steady_clock::now());
    SaveState();
}

// --- Optional self-check chat command ---

void SelfCheckCommand(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!pc) return;
    const int remaining = RemainingSeconds(pc);
    Send(pc, remaining > 0
        ? ReplaceToken(g_config.self_check_on_cooldown, "{0}", std::to_string(remaining))
        : g_config.self_check_none);
}

void TestCommand(AShooterPlayerController* pc, FString* message, EChatSendMode::Type) {
    if (!pc) return;
    int seconds = g_config.test_seconds;
    if (message) {
        std::istringstream input(message->ToString());
        std::string command;
        int requested = 0;
        input >> command >> requested;
        if (requested > 0 && requested <= 3600) seconds = requested;
    }
    StartCooldown(pc, seconds, "manual test");
    Send(pc, "PvPCooldowns v2.0 DuoRaidCore test started. Use /pvpcd to inspect the timer.");
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
    c.min_damage_to_trigger = minijson::number(root, "General", "MinDamageToTrigger", c.min_damage_to_trigger);
    c.show_messages = minijson::boolean(root, "General", "ShowMessages", c.show_messages);
    c.self_check_command = minijson::str(root, "General", "SelfCheckCommand", c.self_check_command);
    c.test_command = minijson::str(root, "General", "TestCommand", c.test_command);
    c.icon_test_command = minijson::str(root, "General", "IconTestCommand", c.icon_test_command);
    c.reminder_interval_seconds = minijson::integer(root, "General", "ReminderIntervalSeconds", c.reminder_interval_seconds);
    c.min_tribe_team_id = minijson::integer(root, "General", "MinTribeTeamId", c.min_tribe_team_id);
    c.player_damage_seconds = minijson::integer(root, "Durations", "PlayerDamageSeconds", c.player_damage_seconds);
    c.tame_damage_seconds = minijson::integer(root, "Durations", "TameDamageSeconds", c.tame_damage_seconds);
    c.structure_damage_seconds = minijson::integer(root, "Durations", "StructureDamageSeconds", c.structure_damage_seconds);
    c.player_kill_seconds = minijson::integer(root, "Durations", "PlayerKillSeconds", c.player_kill_seconds);
    c.tame_kill_seconds = minijson::integer(root, "Durations", "TameKillSeconds", c.tame_kill_seconds);
    c.structure_destroyed_seconds = minijson::integer(root, "Durations", "StructureDestroyedSeconds", c.structure_destroyed_seconds);
    c.turret_kill_seconds = minijson::integer(root, "Durations", "TurretKillSeconds", c.turret_kill_seconds);
    c.test_seconds = minijson::integer(root, "Durations", "TestSeconds", c.test_seconds);
    c.persistence_enabled = minijson::boolean(root, "Persistence", "Enabled", c.persistence_enabled);
    c.state_file = minijson::str(root, "Persistence", "StateFile", c.state_file);
    c.tag_offline_tribe_members = minijson::boolean(
        root, "Persistence", "TagOfflineTribeMembers", c.tag_offline_tribe_members);
    c.block_shop_commands = minijson::boolean(root, "ArkShopBlocking", "Enabled", c.block_shop_commands);
    if (const minijson::Value* commands = minijson::path(root, "ArkShopBlocking", "Commands");
        commands && commands->is_array()) {
        c.blocked_commands.clear();
        for (const minijson::Value& value : commands->array()) {
            if (value.is_string() && !value.string().empty()) c.blocked_commands.push_back(value.string());
        }
    }
    c.combat_logout_enabled = minijson::boolean(root, "CombatLogout", "Enabled", c.combat_logout_enabled);
    c.combat_logout_kill_character = minijson::boolean(
        root, "CombatLogout", "KillCharacter", c.combat_logout_kill_character);
    c.combat_logout_penalty_seconds = minijson::integer(
        root, "CombatLogout", "PenaltySeconds", c.combat_logout_penalty_seconds);
    c.soft_orp_enabled = minijson::boolean(root, "SoftORP", "Enabled", c.soft_orp_enabled);
    c.soft_orp_grace_seconds = minijson::integer(root, "SoftORP", "GraceSeconds", c.soft_orp_grace_seconds);
    c.soft_orp_damage_multiplier = minijson::number(
        root, "SoftORP", "DamageMultiplier", c.soft_orp_damage_multiplier);
    c.hud_notification_enabled = minijson::boolean(root, "HudNotification", "Enabled", c.hud_notification_enabled);
    c.hud_display_scale = minijson::number(root, "HudNotification", "DisplayScale", c.hud_display_scale);
    c.hud_display_time = minijson::number(root, "HudNotification", "DisplayTime", c.hud_display_time);
    c.hud_icon = minijson::str(root, "HudNotification", "Icon", c.hud_icon);
    c.hud_buff_enabled = minijson::boolean(root, "HudBuff", "Enabled", c.hud_buff_enabled);
    c.hud_buff_blueprint = minijson::str(root, "HudBuff", "BlueprintPath", c.hud_buff_blueprint);

    c.cooldown_started = minijson::str(root, "Messages", "CooldownStarted", c.cooldown_started);
    c.cooldown_escalated = minijson::str(root, "Messages", "CooldownEscalated", c.cooldown_escalated);
    c.self_check_on_cooldown = minijson::str(root, "Messages", "SelfCheckOnCooldown", c.self_check_on_cooldown);
    c.self_check_none = minijson::str(root, "Messages", "SelfCheckNone", c.self_check_none);
    c.reminder = minijson::str(root, "Messages", "Reminder", c.reminder);
    c.command_blocked = minijson::str(root, "Messages", "CommandBlocked", c.command_blocked);
    c.combat_logout = minijson::str(root, "Messages", "CombatLogout", c.combat_logout);

    c.min_damage_to_trigger = std::max(0.0f, c.min_damage_to_trigger);
    c.min_tribe_team_id = std::max(1, c.min_tribe_team_id);
    c.player_damage_seconds = std::max(1, c.player_damage_seconds);
    c.tame_damage_seconds = std::max(1, c.tame_damage_seconds);
    c.structure_damage_seconds = std::max(1, c.structure_damage_seconds);
    c.player_kill_seconds = std::max(1, c.player_kill_seconds);
    c.tame_kill_seconds = std::max(1, c.tame_kill_seconds);
    c.structure_destroyed_seconds = std::max(1, c.structure_destroyed_seconds);
    c.turret_kill_seconds = std::max(1, c.turret_kill_seconds);
    c.test_seconds = std::max(1, c.test_seconds);
    c.combat_logout_penalty_seconds = std::max(1, c.combat_logout_penalty_seconds);
    c.soft_orp_grace_seconds = std::max(0, c.soft_orp_grace_seconds);
    c.soft_orp_damage_multiplier = std::max(0.0f, std::min(1.0f, c.soft_orp_damage_multiplier));
    if (c.state_file.empty() || c.state_file.find("..") != std::string::npos ||
        c.state_file.find('/') != std::string::npos || c.state_file.find('\\') != std::string::npos) {
        c.state_file = "state.json";
    }
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
    Log::GetLog()->info("Loading plugin - PvPCooldowns v2.0 DuoRaidCore");
    PvPCooldowns::g_unloading = false;

    try {
        PvPCooldowns::ReadConfig();
    } catch (const std::exception& e) {
        Log::GetLog()->error("PvPCooldowns: config error ({}), using defaults", e.what());
        PvPCooldowns::g_config = PvPCooldowns::Config();
    }
    try {
        PvPCooldowns::LoadState();
    } catch (const std::exception& e) {
        Log::GetLog()->error("PvPCooldowns: state restore failed ({}); starting with empty state", e.what());
        PvPCooldowns::g_cooldowns.clear();
        PvPCooldowns::g_tribe_expiry.clear();
        PvPCooldowns::g_tribe_last_online.clear();
    }
    PvPCooldowns::LoadHudBuffClass();

    ArkApi::GetHooks().SetHook("APrimalCharacter.TakeDamage",
        &PvPCooldowns::Hook_APrimalCharacter_TakeDamage,
        &PvPCooldowns::APrimalCharacter_TakeDamage_original);
    ArkApi::GetHooks().SetHook("APrimalStructure.TakeDamage",
        &PvPCooldowns::Hook_APrimalStructure_TakeDamage,
        &PvPCooldowns::APrimalStructure_TakeDamage_original);
    ArkApi::GetHooks().SetHook("AShooterPlayerController.ServerSendChatMessage_Implementation",
        &PvPCooldowns::Hook_AShooterPlayerController_ServerSendChatMessage_Implementation,
        &PvPCooldowns::AShooterPlayerController_ServerSendChatMessage_Implementation_original);
    ArkApi::GetHooks().SetHook("AShooterGameMode.Logout",
        &PvPCooldowns::Hook_AShooterGameMode_Logout,
        &PvPCooldowns::AShooterGameMode_Logout_original);

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
    PvPCooldowns::g_unloading = true;
    PvPCooldowns::SaveState(true);
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
    ArkApi::GetHooks().DisableHook("AShooterPlayerController.ServerSendChatMessage_Implementation",
        &PvPCooldowns::Hook_AShooterPlayerController_ServerSendChatMessage_Implementation);
    ArkApi::GetHooks().DisableHook("AShooterGameMode.Logout", &PvPCooldowns::Hook_AShooterGameMode_Logout);
    PvPCooldowns::g_hud_buff_class = nullptr;
    PvPCooldowns::g_cooldowns.clear();
    PvPCooldowns::g_tribe_expiry.clear();
    PvPCooldowns::g_tribe_last_online.clear();
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
