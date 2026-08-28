// DamageNumbers -- ARK's native floating damage numbers for the attacker,
// plus a red warning to the victim tribe only when a real enemy tribe deals
// damage. PvPCooldowns separately owns the persistent raid timer/state.
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
#include <cctype>
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
    bool notify_victim_tribe = true;
    int min_tribe_team_id = 50000;
    bool floating_damage_enabled = true;
    bool force_native_server_setting = false;
    bool disable_native_server_setting = true;
    bool show_enemy_damage_to_victim_tribe = true;
    bool show_outgoing_player_damage = true;
    bool show_outgoing_dino_damage = true;
    bool show_outgoing_structure_damage = true;
    bool show_self_damage = true;
    bool show_turret_damage_to_players = true;
    bool show_turret_damage_to_dinos = true;
    bool show_turret_damage_to_structures = false;
    bool show_victim_player_damage = true;
    bool show_victim_dino_damage = true;
    bool show_victim_structure_damage = true;
    bool also_send_attacker_chat = false;
    float floating_damage_vertical_offset = 100.0f;
    std::string test_command = "/datest";

    bool combat_balance_enabled = true;
    float enemy_structure_damage_multiplier = 0.5f;
    float tek_rifle_structure_damage_cap = 500.0f;
    float turret_character_damage_multiplier = 3.0f;

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

FVector GetCameraFrontLocation(AShooterPlayerController* pc) {
    FVector location{};
    FRotator rotation{};
    if (!pc) return location;
    pc->GetPlayerViewPoint(&location, &rotation);
    constexpr float degrees_to_radians = 3.14159265358979323846f / 180.0f;
    const float pitch = rotation.Pitch * degrees_to_radians;
    const float yaw = rotation.Yaw * degrees_to_radians;
    const FVector forward(
        std::cos(pitch) * std::cos(yaw),
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch));
    location += forward * 600.0f;
    location.Z += 50.0f;
    return location;
}

bool CategoryEnabled(Category cat, bool player, bool dino, bool structure) {
    if (cat == Category::Player) return player;
    if (cat == Category::Dino) return dino;
    return structure;
}

void SendEnemyFloatingDamage(AActor* target, int victim_team, int attacker_team,
                             float amount, Category cat) {
    if (!g_config.floating_damage_enabled || !g_config.show_enemy_damage_to_victim_tribe ||
        !target || amount < g_config.min_damage_to_report ||
        !CategoryEnabled(cat, g_config.show_victim_player_damage,
                         g_config.show_victim_dino_damage,
                         g_config.show_victim_structure_damage)) return;

    FVector location{};
    FVector extent{};
    target->GetActorBounds(false, &location, &extent);
    location.Z += g_config.floating_damage_vertical_offset;
    const int displayed_damage = std::max(1, static_cast<int>(std::llround(amount)));

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return;
    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        auto* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        if (ArkApi::GetApiUtils().GetTribeID(pc) != victim_team) continue;

        FVector display_location = location;
        if (cat == Category::Player &&
            target == static_cast<AActor*>(pc->GetPlayerCharacter())) {
            display_location = GetCameraFrontLocation(pc);
        }

        // Passing the enemy attacker's team lets ARK apply its native enemy
        // colour (red) for every victim-tribe client. No client mod is needed.
        pc->ClientAddFloatingDamageText(
            FVector_NetQuantize(display_location), displayed_damage, attacker_team);
        ++g_floating_rpc_events;
    }
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

AttackerInfo ResolveAttacker(AController* event_instigator, AActor* damage_causer);

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string GetClassFullName(UObject* object) {
    if (!object || !object->ClassField()) return {};
    UObject* cdo = object->ClassField()->GetDefaultObject(true);
    if (!cdo) return {};
    FString full_name;
    cdo->GetFullName(&full_name, nullptr);
    return ToLower(full_name.ToString());
}

bool NameLooksLikeTekRifle(const std::string& name) {
    if (name.empty()) return false;
    return name.find("tekrifle") != std::string::npos ||
        name.find("weaptek") != std::string::npos ||
        (name.find("tek") != std::string::npos &&
         name.find("rifle") != std::string::npos);
}

bool IsTekRifleDamage(AController* event_instigator, AActor* damage_causer) {
    AActor* current = damage_causer;
    for (int depth = 0; current && depth < 4; ++depth) {
        if (NameLooksLikeTekRifle(GetClassFullName(current))) return true;
        current = current->OwnerField();
    }

    if (event_instigator && event_instigator->IsA(AShooterPlayerController::GetPrivateStaticClass())) {
        auto* pc = static_cast<AShooterPlayerController*>(event_instigator);
        AShooterCharacter* character = pc->GetPlayerCharacter();
        if (character && NameLooksLikeTekRifle(GetClassFullName(character->CurrentWeaponField()))) return true;
    }
    return false;
}

bool IsTurretDamage(AActor* damage_causer) {
    AActor* current = damage_causer;
    for (int depth = 0; current && depth < 5; ++depth) {
        if (current->IsA(APrimalStructureTurret::GetPrivateStaticClass())) return true;
        const std::string name = GetClassFullName(current);
        if (name.find("turret") != std::string::npos &&
            (name.find("projectile") != std::string::npos || name.find("bullet") != std::string::npos)) return true;
        current = current->OwnerField();
    }
    return false;
}

float ApplyCombatBalance(float damage, bool structure_target, int target_team,
                         AController* event_instigator, AActor* damage_causer) {
    if (!g_config.combat_balance_enabled || damage <= 0.0f) return damage;

    float adjusted = damage;
    if (structure_target) {
        const AttackerInfo attacker = ResolveAttacker(event_instigator, damage_causer);
        const bool enemy_pvp_damage = IsPlayerOwnedTeam(target_team) &&
            IsPlayerOwnedTeam(attacker.team) && attacker.team != target_team;
        if (enemy_pvp_damage) {
            adjusted *= std::max(0.0f, g_config.enemy_structure_damage_multiplier);
        }

        // Apply the weapon cap after the PvP multiplier, so a high raw Tek
        // rifle hit is still capped at 500 outside caves. ARK may then apply
        // its own location-specific multipliers (for example cave damage).
        if (IsTekRifleDamage(event_instigator, damage_causer)) {
            const float cap = g_config.tek_rifle_structure_damage_cap;
            if (cap > 0.0f) adjusted = std::min(adjusted, cap);
        }
    } else if (!structure_target && IsPlayerOwnedTeam(target_team) && IsTurretDamage(damage_causer)) {
        adjusted *= std::max(0.0f, g_config.turret_character_damage_multiplier);
    }
    return std::max(0.0f, adjusted);
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
    const bool self_damage = attacker.player &&
        target == static_cast<AActor*>(attacker.player->GetPlayerCharacter());

    // Automated turrets can carry their owner's controller/team as the
    // instigator. Treating that as a personal shot gives the owner a green
    // number at the remote victim and effectively reveals the victim through
    // terrain. Personal weapon hits still use the normal outgoing number.
    const bool automated_turret_hit = IsTurretDamage(damage_causer);
    const bool outgoing_category_enabled = CategoryEnabled(
        cat, g_config.show_outgoing_player_damage,
        g_config.show_outgoing_dino_damage,
        g_config.show_outgoing_structure_damage);
    const bool turret_category_enabled = CategoryEnabled(
        cat, g_config.show_turret_damage_to_players,
        g_config.show_turret_damage_to_dinos,
        g_config.show_turret_damage_to_structures);
    if (g_config.notify_attacker && attacker.player && amount >= g_config.min_damage_to_report &&
        outgoing_category_enabled && (!self_damage || g_config.show_self_damage) &&
        (!automated_turret_hit || turret_category_enabled)) {
        // Always send the native client RPC for the actual attacker. Merely
        // toggling bShowFloatingDamageText at runtime is not reliably
        // replicated to already connected ASE clients, so relying on that
        // setting alone can produce no number at all.
        if (g_config.floating_damage_enabled) {
            FVector location{};
            FVector extent{};
            // Character bounds can lag or expand during falling/ragdoll. Use
            // the live root-component position for moving player/dino targets.
            if (self_damage) {
                location = GetCameraFrontLocation(attacker.player);
            } else if (cat != Category::Structure && target->RootComponentField()) {
                target->RootComponentField()->GetWorldLocation(&location);
            } else {
                target->GetActorBounds(false, &location, &extent);
            }
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
    const bool enemy_tribe_damage = IsPlayerOwnedTeam(target_team) &&
        IsPlayerOwnedTeam(attacker.team) && attacker.team != target_team;
    // Apply the turret category switches to the victim side as well.  The
    // previous implementation suppressed the owner's number but still sent
    // one reliable ClientAddFloatingDamageText RPC to every victim-tribe
    // player for every turret projectile.  A large turret wall could therefore
    // overflow the client's reliable network buffer even though all three
    // ShowTurretDamageTo* settings were false.
    if (enemy_tribe_damage && (!automated_turret_hit || turret_category_enabled)) {
        SendEnemyFloatingDamage(target, target_team, attacker.team, amount, cat);
    }
    if (g_config.notify_victim_tribe && enemy_tribe_damage) {
        g_pending_victim[target_team][static_cast<int>(cat)] += amount;
    }
}

std::chrono::steady_clock::time_point g_next_flush{};

void FlushTimer() {
    // ARK's global floating-damage switch cannot distinguish a hand-held
    // weapon from an automated turret.  When it is enabled, the owner sees a
    // native green number for every turret projectile even if this plugin's
    // turret categories are disabled.  Keep the global stream off and use the
    // filtered ClientAddFloatingDamageText RPCs above instead.
    if (g_config.disable_native_server_setting) {
        AShooterGameMode* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
        if (game_mode) {
            game_mode->bShowFloatingDamageTextField() = false;
            if (!g_native_floating_applied) {
                g_native_floating_applied = true;
                Log::GetLog()->info("DamageNumbers: native ARK floating damage disabled; filtered RPC mode active");
            }
        }
    } else if (g_config.floating_damage_enabled && g_config.force_native_server_setting) {
        AShooterGameMode* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
        if (game_mode) {
            game_mode->bShowFloatingDamageTextField() = true;
            if (!g_native_floating_applied) {
                g_native_floating_applied = true;
                Log::GetLog()->info("DamageNumbers: native ARK floating damage enabled");
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
DECLARE_HOOK(APrimalTargetableActor_AdjustDamage, void, APrimalTargetableActor*, float*, FDamageEvent*, AController*, AActor*);

float Hook_APrimalCharacter_TakeDamage(APrimalCharacter* _this, float damage, FDamageEvent* damage_event,
                                        AController* event_instigator, AActor* damage_causer) {
    const int target_team = _this ? _this->TargetingTeamField() : -1;
    damage = ApplyCombatBalance(damage, false, target_team, event_instigator, damage_causer);
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

void Hook_APrimalTargetableActor_AdjustDamage(APrimalTargetableActor* _this, float* damage,
                                               FDamageEvent* damage_event,
                                               AController* event_instigator,
                                               AActor* damage_causer) {
    // Let ARK and the structure (including modded S+/Tek structures) finish
    // their normal weapon/material calculations first. Applying the cap in
    // TakeDamage was too early: ARK could multiply the already capped 500
    // afterwards and a live Tek foundation received 4620.
    APrimalTargetableActor_AdjustDamage_original(
        _this, damage, damage_event, event_instigator, damage_causer);

    if (!_this || !damage || *damage <= 0.0f ||
        !_this->IsA(APrimalStructure::GetPrivateStaticClass())) return;

    const int target_team = _this->TargetingTeamField();
    *damage = ApplyCombatBalance(
        *damage, true, target_team, event_instigator, damage_causer);
}

// --- Config ---

std::string ConfigPath() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/DamageNumbers/config.json";
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
    c.disable_native_server_setting = minijson::boolean(
        root, "FloatingDamage", "DisableNativeServerSetting", c.disable_native_server_setting);
    // Disabling wins if both legacy options are accidentally enabled.
    if (c.disable_native_server_setting) c.force_native_server_setting = false;
    c.show_enemy_damage_to_victim_tribe = minijson::boolean(
        root, "FloatingDamage", "ShowEnemyDamageToVictimTribe", c.show_enemy_damage_to_victim_tribe);
    c.show_outgoing_player_damage = minijson::boolean(root, "FloatingDamage", "ShowOutgoingPlayerDamage", c.show_outgoing_player_damage);
    c.show_outgoing_dino_damage = minijson::boolean(root, "FloatingDamage", "ShowOutgoingDinoDamage", c.show_outgoing_dino_damage);
    c.show_outgoing_structure_damage = minijson::boolean(root, "FloatingDamage", "ShowOutgoingStructureDamage", c.show_outgoing_structure_damage);
    c.show_self_damage = minijson::boolean(root, "FloatingDamage", "ShowSelfDamage", c.show_self_damage);
    c.show_turret_damage_to_players = minijson::boolean(root, "FloatingDamage", "ShowTurretDamageToPlayers", c.show_turret_damage_to_players);
    c.show_turret_damage_to_dinos = minijson::boolean(root, "FloatingDamage", "ShowTurretDamageToDinos", c.show_turret_damage_to_dinos);
    c.show_turret_damage_to_structures = minijson::boolean(root, "FloatingDamage", "ShowTurretDamageToStructures", c.show_turret_damage_to_structures);
    c.show_victim_player_damage = minijson::boolean(root, "FloatingDamage", "ShowVictimPlayerDamage", c.show_victim_player_damage);
    c.show_victim_dino_damage = minijson::boolean(root, "FloatingDamage", "ShowVictimDinoDamage", c.show_victim_dino_damage);
    c.show_victim_structure_damage = minijson::boolean(root, "FloatingDamage", "ShowVictimStructureDamage", c.show_victim_structure_damage);
    c.also_send_attacker_chat = minijson::boolean(root, "FloatingDamage", "AlsoSendAttackerChat", c.also_send_attacker_chat);
    c.floating_damage_vertical_offset = minijson::number(
        root, "FloatingDamage", "VerticalOffset", c.floating_damage_vertical_offset);
    c.test_command = minijson::str(root, "Diagnostics", "TestCommand", c.test_command);

    c.combat_balance_enabled = minijson::boolean(root, "CombatBalance", "Enabled", c.combat_balance_enabled);
    c.enemy_structure_damage_multiplier = minijson::number(
        root, "CombatBalance", "EnemyStructureDamageMultiplier", c.enemy_structure_damage_multiplier);
    c.tek_rifle_structure_damage_cap = minijson::number(
        root, "CombatBalance", "TekRifleStructureDamageCap", c.tek_rifle_structure_damage_cap);
    c.turret_character_damage_multiplier = minijson::number(
        root, "CombatBalance", "TurretCharacterDamageMultiplier", c.turret_character_damage_multiplier);

    c.attacker_hit = minijson::str(root, "Messages", "AttackerHit", c.attacker_hit);
    c.victim_hit = minijson::str(root, "Messages", "VictimHit", c.victim_hit);

    c.flush_interval_seconds = std::max(1, c.flush_interval_seconds);
    c.min_tribe_team_id = std::max(1, c.min_tribe_team_id);
    c.enemy_structure_damage_multiplier = std::max(0.0f, c.enemy_structure_damage_multiplier);
    c.tek_rifle_structure_damage_cap = std::max(0.0f, c.tek_rifle_structure_damage_cap);
    c.turret_character_damage_multiplier = std::max(0.0f, c.turret_character_damage_multiplier);
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
        // Put the test number in front of the camera. The old test placed it
        // at the player's own body, which is behind the first-person camera.
        FVector location{};
        FRotator rotation{};
        pc->GetPlayerViewPoint(&location, &rotation);
        constexpr float degrees_to_radians = 3.14159265358979323846f / 180.0f;
        const float pitch = rotation.Pitch * degrees_to_radians;
        const float yaw = rotation.Yaw * degrees_to_radians;
        const FVector forward(
            std::cos(pitch) * std::cos(yaw),
            std::cos(pitch) * std::sin(yaw),
            std::sin(pitch));
        location += forward * 600.0f;
        location.Z += 50.0f;
        const int team = ArkApi::GetApiUtils().GetTribeID(pc);
        pc->ClientAddFloatingDamageText(FVector_NetQuantize(location), 12345, team);
        ++g_floating_rpc_events;
    }

    std::ostringstream status;
    status << "DamageNumbers v2.6 TurretRpcOverflowFix OK | native="
           << (g_config.disable_native_server_setting ? "OFF" :
               (g_native_floating_applied ? "ON" : "WAIT"))
           << " | character_hits=" << g_character_damage_events
           << " | structure_hits=" << g_structure_damage_events
           << " | rpc=" << g_floating_rpc_events;
    SendColored(pc, status.str(), FColorList::Cyan);
}

} // namespace DamageAlerts

void Load() {
    Log::Get().Init("DamageNumbers");
    Log::GetLog()->info("Loading plugin - DamageNumbers v2.6 TurretRpcOverflowFix");

    try {
        DamageAlerts::ReadConfig();
    } catch (const std::exception& e) {
        Log::GetLog()->error("DamageNumbers: config error ({}), using defaults", e.what());
        DamageAlerts::g_config = DamageAlerts::Config();
    }

    ArkApi::GetHooks().SetHook("APrimalCharacter.TakeDamage",
        &DamageAlerts::Hook_APrimalCharacter_TakeDamage,
        &DamageAlerts::APrimalCharacter_TakeDamage_original);
    ArkApi::GetHooks().SetHook("APrimalStructure.TakeDamage",
        &DamageAlerts::Hook_APrimalStructure_TakeDamage,
        &DamageAlerts::APrimalStructure_TakeDamage_original);
    ArkApi::GetHooks().SetHook("APrimalTargetableActor.AdjustDamage",
        &DamageAlerts::Hook_APrimalTargetableActor_AdjustDamage,
        &DamageAlerts::APrimalTargetableActor_AdjustDamage_original);

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
    ArkApi::GetHooks().DisableHook("APrimalTargetableActor.AdjustDamage",
        &DamageAlerts::Hook_APrimalTargetableActor_AdjustDamage);
    DamageAlerts::g_pending_attacker.clear();
    DamageAlerts::g_pending_victim.clear();
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try { Load(); }
    catch (const std::exception& e) {
        Log::Get().Init("DamageNumbers");
        Log::GetLog()->error("DamageNumbers failed to initialize: {}", e.what());
    }
    catch (...) {
        Log::Get().Init("DamageNumbers");
        Log::GetLog()->error("DamageNumbers failed to initialize with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try { Unload(); }
    catch (const std::exception& e) {
        Log::GetLog()->error("DamageNumbers unload exception: {}", e.what());
    }
    catch (...) {
        Log::GetLog()->error("DamageNumbers unload unknown exception");
    }
}
