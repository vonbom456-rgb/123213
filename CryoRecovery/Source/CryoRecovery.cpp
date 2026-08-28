#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../DamageAlerts/Source/MiniJson.h"

namespace CryoRecovery {

struct Config {
    bool enabled = true;
    float heal_percent_per_minute = 5.0f;
    float pvp_heal_percent_per_minute = 4.0f;
    int minimum_stored_seconds = 5;
    float maximum_counted_hours = 24.0f;
    float post_spawn_apply_delay_seconds = 3.0f;
    bool daeodon_enabled = true;
    float daeodon_heal_multiplier = 5.0f;
    float daeodon_pvp_heal_multiplier = 4.0f;
    std::vector<std::string> daeodon_buff_name_tokens{
        "daeodon", "daedon", "pig_healing", "pig healing"};
    bool notify_player = true;
    std::string status_command = "/cryoheal";
    std::string healed_message = "Cryopod recovery: +{0} HP ({1}% total).";
    bool release_limit_enabled = true;
    int release_window_seconds = 30;
    int max_releases = 10;
    int max_large_releases = 5;
    std::vector<std::string> large_dino_tokens{
        "giganoto", "carcha", "titanosaur", "bronto", "paracer", "rockgolem",
        "rock elemental", "rock_elemental", "magmasaur", "stego", "mek"};
    std::string release_blocked_message = "RAID/PvP cryo limit reached. Wait a few seconds.";
};

struct DinoKey {
    unsigned int id1 = 0;
    unsigned int id2 = 0;

    bool operator==(const DinoKey& other) const noexcept {
        return id1 == other.id1 && id2 == other.id2;
    }
};

struct DinoKeyHash {
    size_t operator()(const DinoKey& key) const noexcept {
        return (static_cast<size_t>(key.id1) << 32) ^ static_cast<size_t>(key.id2);
    }
};

Config g_config;
std::unordered_map<DinoKey, std::int64_t, DinoKeyHash> g_stored_at;

struct PendingRecovery {
    TWeakObjectPtr<APrimalDinoCharacter> dino;
    TWeakObjectPtr<AShooterPlayerController> controller;
    DinoKey key;
    std::int64_t elapsed_seconds = 0;
    float rate = 0.0f;
    float health_after_spawn = 0.0f;
    bool pvp = false;
    std::chrono::steady_clock::time_point apply_at{};
};

std::vector<PendingRecovery> g_pending_recoveries;
bool g_capture_hook_installed = false;
bool g_spawn_hook_installed = false;
bool g_set_health_hook_installed = false;
bool g_can_use_hook_installed = false;
std::uint64_t g_daeodon_boosts = 0;

struct ReleaseEvent { std::chrono::steady_clock::time_point at; bool large = false; };
std::unordered_map<int, std::deque<ReleaseEvent>> g_release_events;

using PvpCooldownQuery = bool(__fastcall*)(AShooterPlayerController*);

std::int64_t UnixNow() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string PluginDir() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/CryoRecovery";
}

std::string ConfigPath() { return PluginDir() + "/config.json"; }
std::string StatePath() { return PluginDir() + "/state.csv"; }

std::string ReplaceToken(std::string text, const std::string& token, const std::string& value) {
    const size_t pos = text.find(token);
    if (pos != std::string::npos) text.replace(pos, token.size(), value);
    return text;
}

void ReadConfig() {
    std::ifstream file(ConfigPath(), std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Can't open " + ConfigPath());
    std::ostringstream stream;
    stream << file.rdbuf();
    const minijson::Value root = minijson::parse(stream.str());
    if (!root.is_object()) throw std::runtime_error("config root must be a JSON object");

    Config value;
    value.enabled = minijson::boolean(root, "CryopodHealing", "Enabled", value.enabled);
    value.heal_percent_per_minute = minijson::number(
        root, "CryopodHealing", "HealPercentPerMinute", value.heal_percent_per_minute);
    value.pvp_heal_percent_per_minute = minijson::number(
        root, "CryopodHealing", "PvpHealPercentPerMinute", value.pvp_heal_percent_per_minute);
    value.minimum_stored_seconds = minijson::integer(
        root, "CryopodHealing", "MinimumStoredSeconds", value.minimum_stored_seconds);
    value.maximum_counted_hours = minijson::number(
        root, "CryopodHealing", "MaximumCountedHours", value.maximum_counted_hours);
    value.post_spawn_apply_delay_seconds = minijson::number(
        root, "CryopodHealing", "PostSpawnApplyDelaySeconds",
        value.post_spawn_apply_delay_seconds);
    value.daeodon_enabled = minijson::boolean(
        root, "DaeodonHealing", "Enabled", value.daeodon_enabled);
    value.daeodon_heal_multiplier = minijson::number(
        root, "DaeodonHealing", "HealMultiplier", value.daeodon_heal_multiplier);
    value.daeodon_pvp_heal_multiplier = minijson::number(
        root, "DaeodonHealing", "PvpHealMultiplier", value.daeodon_pvp_heal_multiplier);
    const auto daeodon_tokens = minijson::strings(root, "DaeodonHealing", "BuffNameTokens");
    if (!daeodon_tokens.empty()) value.daeodon_buff_name_tokens = daeodon_tokens;
    value.notify_player = minijson::boolean(root, "Messages", "NotifyPlayer", value.notify_player);
    value.status_command = minijson::str(root, "Messages", "StatusCommand", value.status_command);
    value.healed_message = minijson::str(root, "Messages", "Healed", value.healed_message);
    value.release_limit_enabled = minijson::boolean(root, "PvpReleaseLimit", "Enabled", value.release_limit_enabled);
    value.release_window_seconds = minijson::integer(root, "PvpReleaseLimit", "WindowSeconds", value.release_window_seconds);
    value.max_releases = minijson::integer(root, "PvpReleaseLimit", "MaxTotalReleases", value.max_releases);
    value.max_large_releases = minijson::integer(root, "PvpReleaseLimit", "MaxLargeReleases", value.max_large_releases);
    const auto large_tokens = minijson::strings(root, "PvpReleaseLimit", "LargeDinoTokens");
    if (!large_tokens.empty()) value.large_dino_tokens = large_tokens;
    value.release_blocked_message = minijson::str(root, "Messages", "ReleaseLimitReached", value.release_blocked_message);

    value.heal_percent_per_minute = std::max(0.0f, value.heal_percent_per_minute);
    value.pvp_heal_percent_per_minute = std::max(0.0f, value.pvp_heal_percent_per_minute);
    value.minimum_stored_seconds = std::max(0, value.minimum_stored_seconds);
    value.maximum_counted_hours = std::max(0.0f, value.maximum_counted_hours);
    value.post_spawn_apply_delay_seconds = std::clamp(
        value.post_spawn_apply_delay_seconds, 0.5f, 10.0f);
    value.daeodon_heal_multiplier = std::max(1.0f, value.daeodon_heal_multiplier);
    value.daeodon_pvp_heal_multiplier = std::max(1.0f, value.daeodon_pvp_heal_multiplier);
    value.release_window_seconds = std::clamp(value.release_window_seconds, 5, 300);
    value.max_releases = std::clamp(value.max_releases, 1, 100);
    value.max_large_releases = std::clamp(value.max_large_releases, 1, value.max_releases);
    g_config = value;
}

void LoadState() {
    g_stored_at.clear();
    std::ifstream file(StatePath());
    if (!file.is_open()) return;

    unsigned int id1 = 0;
    unsigned int id2 = 0;
    std::int64_t stored_at = 0;
    char comma1 = 0;
    char comma2 = 0;
    while (file >> id1 >> comma1 >> id2 >> comma2 >> stored_at) {
        if (comma1 == ',' && comma2 == ',' && id1 != 0 && id2 != 0 && stored_at > 0)
            g_stored_at[{id1, id2}] = stored_at;
    }
}

void SaveState() {
    const std::string temp_path = StatePath() + ".tmp";
    std::ofstream file(temp_path, std::ios::trunc);
    if (!file.is_open()) {
        Log::GetLog()->error("CryoRecovery: cannot write state file");
        return;
    }
    for (const auto& entry : g_stored_at)
        file << entry.first.id1 << ',' << entry.first.id2 << ',' << entry.second << '\n';
    file.close();
    if (!MoveFileExA(temp_path.c_str(), StatePath().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log::GetLog()->error("CryoRecovery: cannot replace state file (Windows error {})", GetLastError());
    }
}

bool IsOnPvpCooldown(AShooterPlayerController* pc) {
    if (!pc) return false;
    HMODULE module = GetModuleHandleA("PvPCooldowns.dll");
    if (!module) return false;
    const auto query = reinterpret_cast<PvpCooldownQuery>(
        GetProcAddress(module, "PvpCooldowns_IsOnCooldown"));
    return query ? query(pc) : false;
}

bool IsTeamOnPvpCooldown(int team) {
    if (team <= 0) return false;
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return false;
    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        auto* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        if (ArkApi::GetApiUtils().GetTribeID(pc) == team && IsOnPvpCooldown(pc)) return true;
    }
    return false;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool HasDaeodonHealingBuff(APrimalCharacter* character) {
    if (!character) return false;
    for (APrimalBuff* buff : character->BuffsField()) {
        if (!buff) continue;
        FString full_name;
        buff->GetFullName(&full_name, nullptr);
        const std::string name = ToLower(full_name.ToString());
        for (const std::string& configured_token : g_config.daeodon_buff_name_tokens) {
            const std::string token = ToLower(configured_token);
            if (!token.empty() && name.find(token) != std::string::npos) return true;
        }
    }
    return false;
}

void Send(AShooterPlayerController* pc, const std::string& message) {
    if (!pc || message.empty()) return;
    const FString text(ArkApi::Tools::Utf8Decode(message).c_str());
    ArkApi::GetApiUtils().SendServerMessage(pc, FColorList::Green, *text);
}

DECLARE_HOOK(APrimalDinoCharacter_GetDinoData, void,
             APrimalDinoCharacter*, FARKDinoData*);

DECLARE_HOOK(APrimalDinoCharacter_SpawnFromDinoDataEx, APrimalDinoCharacter*,
             FARKDinoData*, UWorld*, FVector*, FRotator*, bool*, int, bool,
             AShooterPlayerController*, bool);

DECLARE_HOOK(APrimalCharacter_SetHealth, float, APrimalCharacter*, float);
DECLARE_HOOK(UPrimalItem_CanUse, bool, UPrimalItem*, bool);

void PruneReleaseEvents(int team) {
    auto it = g_release_events.find(team);
    if (it == g_release_events.end()) return;
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(g_config.release_window_seconds);
    while (!it->second.empty() && it->second.front().at < cutoff) it->second.pop_front();
    if (it->second.empty()) g_release_events.erase(it);
}

bool HasLargeToken(const std::string& text) {
    const std::string lower = ToLower(text);
    for (const auto& configured : g_config.large_dino_tokens) {
        const std::string token = ToLower(configured);
        if (!token.empty() && lower.find(token) != std::string::npos) return true;
    }
    return false;
}

AShooterPlayerController* ItemController(UPrimalItem* item) {
    if (!item) return nullptr;
    UPrimalInventoryComponent* inventory = item->OwnerInventoryField().Get();
    AActor* owner = inventory ? inventory->GetOwner() : nullptr;
    if (!owner || !owner->IsA(AShooterCharacter::GetPrivateStaticClass())) return nullptr;
    AController* controller = static_cast<AShooterCharacter*>(owner)->ControllerField();
    return controller && controller->IsA(AShooterPlayerController::GetPrivateStaticClass())
        ? static_cast<AShooterPlayerController*>(controller) : nullptr;
}

bool IsFilledCryopod(UPrimalItem* item) {
    if (!item) return false;
    FString full_name;
    item->GetFullName(&full_name, nullptr);
    const std::string name = ToLower(full_name.ToString());
    return name.find("filledcryopod") != std::string::npos ||
        (name.find("cryopod") != std::string::npos && name.find("emptycryopod") == std::string::npos &&
         !item->CustomItemDescriptionField().IsEmpty());
}

bool PodLooksLarge(UPrimalItem* item, AShooterPlayerController* pc) {
    FString display, full_name;
    item->GetItemName(&display, false, false, pc);
    item->GetFullName(&full_name, nullptr);
    return HasLargeToken(display.ToString() + " " + item->CustomItemNameField().ToString() + " " +
        item->CustomItemDescriptionField().ToString() + " " + full_name.ToString());
}

bool Hook_UPrimalItem_CanUse(UPrimalItem* item, bool ignore_cooldown) {
    const bool vanilla_allowed = UPrimalItem_CanUse_original(item, ignore_cooldown);
    if (!vanilla_allowed || !g_config.release_limit_enabled || !IsFilledCryopod(item)) return vanilla_allowed;
    AShooterPlayerController* pc = ItemController(item);
    if (!pc || !IsOnPvpCooldown(pc)) return vanilla_allowed;
    const int team = ArkApi::GetApiUtils().GetTribeID(pc);
    PruneReleaseEvents(team);
    int total = 0, large = 0;
    const auto it = g_release_events.find(team);
    if (it != g_release_events.end()) {
        total = static_cast<int>(it->second.size());
        for (const auto& event : it->second) if (event.large) ++large;
    }
    if (total >= g_config.max_releases || (PodLooksLarge(item, pc) && large >= g_config.max_large_releases)) {
        Send(pc, g_config.release_blocked_message);
        return false;
    }
    return true;
}

float Hook_APrimalCharacter_SetHealth(APrimalCharacter* character, float new_health) {
    if (g_config.daeodon_enabled && character &&
        character->IsA(APrimalDinoCharacter::GetPrivateStaticClass())) {
        const float old_health = character->GetHealth();
        if (new_health > old_health && HasDaeodonHealingBuff(character)) {
            auto* dino = static_cast<APrimalDinoCharacter*>(character);
            const bool pvp = IsTeamOnPvpCooldown(dino->TargetingTeamField());
            const float multiplier = pvp ? g_config.daeodon_pvp_heal_multiplier
                                         : g_config.daeodon_heal_multiplier;
            const float max_health = character->GetMaxHealth();
            new_health = std::min(max_health, old_health + (new_health - old_health) * multiplier);
            ++g_daeodon_boosts;
        }
    }
    return APrimalCharacter_SetHealth_original(character, new_health);
}

void Hook_APrimalDinoCharacter_GetDinoData(
    APrimalDinoCharacter* dino, FARKDinoData* out_data) {
    APrimalDinoCharacter_GetDinoData_original(dino, out_data);
    if (!g_config.enabled || !dino || !dino->BPIsTamed()) return;

    const DinoKey key{dino->DinoID1Field(), dino->DinoID2Field()};
    if (key.id1 == 0 || key.id2 == 0) return;
    g_stored_at[key] = UnixNow();
    SaveState();
    Log::GetLog()->debug("CryoRecovery: captured dino id={}:{}", key.id1, key.id2);
}

void RecoveryTimer() {
    if (g_pending_recoveries.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    auto it = g_pending_recoveries.begin();
    while (it != g_pending_recoveries.end()) {
        if (now < it->apply_at) {
            ++it;
            continue;
        }

        APrimalDinoCharacter* dino = it->dino.Get();
        AShooterPlayerController* controller = it->controller.Get();
        if (!dino || !dino->BPIsTamed()) {
            Log::GetLog()->warn(
                "CryoRecovery: delayed recovery skipped for missing dino id={}:{}",
                it->key.id1, it->key.id2);
            it = g_pending_recoveries.erase(it);
            continue;
        }

        const float max_health = dino->GetMaxHealth();
        const float current_health = dino->GetHealth();
        const float base_health = std::max(current_health, it->health_after_spawn);
        const double max_seconds = static_cast<double>(g_config.maximum_counted_hours) * 3600.0;
        const double counted_seconds = g_config.maximum_counted_hours > 0.0f
            ? std::min<double>(it->elapsed_seconds, max_seconds)
            : static_cast<double>(it->elapsed_seconds);
        const double healed_percent = counted_seconds / 60.0 * static_cast<double>(it->rate);
        const float heal_amount = static_cast<float>(max_health * healed_percent / 100.0);
        const float new_health = std::min(max_health, base_health + heal_amount);
        const float applied = std::max(0.0f, new_health - current_health);

        if (max_health > 0.0f && applied > 0.0f) {
            // SpawnFromDinoDataEx returns before ARK/S+ has finished recalculating
            // the dino's status component. Applying health here prevents that
            // post-spawn recalculation from overwriting cryopod recovery.
            dino->SetHealth(new_health);
            Log::GetLog()->info(
                "CryoRecovery: delayed release heal id={}:{} stored={}s pvp={} rate={}%%/min health={} -> {} / {}",
                it->key.id1, it->key.id2, it->elapsed_seconds, it->pvp, it->rate,
                current_health, new_health, max_health);

            if (g_config.notify_player && controller) {
                const int total_percent = static_cast<int>(
                    std::lround((new_health / max_health) * 100.0f));
                std::string message = ReplaceToken(
                    g_config.healed_message, "{0}",
                    std::to_string(static_cast<long long>(std::llround(applied))));
                message = ReplaceToken(message, "{1}", std::to_string(total_percent));
                Send(controller, message);
            }
        } else {
            Log::GetLog()->info(
                "CryoRecovery: no delayed heal needed id={}:{} health={} / {}",
                it->key.id1, it->key.id2, current_health, max_health);
        }

        it = g_pending_recoveries.erase(it);
    }
}

APrimalDinoCharacter* Hook_APrimalDinoCharacter_SpawnFromDinoDataEx(
    FARKDinoData* in_data, UWorld* world, FVector* location, FRotator* rotation,
    bool* duped_dino, int for_team, bool generate_new_id,
    AShooterPlayerController* tamer_controller, bool begin_play) {
    APrimalDinoCharacter* dino = APrimalDinoCharacter_SpawnFromDinoDataEx_original(
        in_data, world, location, rotation, duped_dino, for_team, generate_new_id,
        tamer_controller, begin_play);
    if (!dino) return dino;

    if (g_config.release_limit_enabled && tamer_controller && IsOnPvpCooldown(tamer_controller)) {
        const int team = ArkApi::GetApiUtils().GetTribeID(tamer_controller);
        FString dino_name;
        dino->GetFullName(&dino_name, nullptr);
        PruneReleaseEvents(team);
        g_release_events[team].push_back({std::chrono::steady_clock::now(), HasLargeToken(dino_name.ToString())});
    }

    if (!g_config.enabled) return dino;

    const DinoKey key{dino->DinoID1Field(), dino->DinoID2Field()};
    const auto it = g_stored_at.find(key);
    if (it == g_stored_at.end()) return dino;

    const std::int64_t now = UnixNow();
    const std::int64_t elapsed_seconds = std::max<std::int64_t>(0, now - it->second);
    g_stored_at.erase(it);
    SaveState();

    if (elapsed_seconds < g_config.minimum_stored_seconds) return dino;

    const bool pvp = IsOnPvpCooldown(tamer_controller);
    const float rate = pvp ? g_config.pvp_heal_percent_per_minute
                           : g_config.heal_percent_per_minute;
    const float old_health = dino->GetHealth();
    if (rate <= 0.0f) return dino;

    PendingRecovery pending;
    pending.dino = GetWeakReference(dino);
    pending.controller = GetWeakReference(tamer_controller);
    pending.key = key;
    pending.elapsed_seconds = elapsed_seconds;
    pending.rate = rate;
    pending.health_after_spawn = old_health;
    pending.pvp = pvp;
    pending.apply_at = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<int>(
            std::lround(g_config.post_spawn_apply_delay_seconds * 1000.0f)));
    g_pending_recoveries.push_back(pending);
    Log::GetLog()->info(
        "CryoRecovery: queued release heal id={}:{} stored={}s pvp={} rate={}%%/min initial_health={} delay={}s",
        key.id1, key.id2, elapsed_seconds, pvp, rate, old_health,
        g_config.post_spawn_apply_delay_seconds);
    return dino;
}

void StatusCommand(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!pc) return;
    std::ostringstream message;
    message << "CryoRecovery: " << (g_config.enabled ? "ON" : "OFF")
            << " | normal=" << g_config.heal_percent_per_minute << "%/min"
            << " | RAID/PvP=" << g_config.pvp_heal_percent_per_minute << "%/min"
            << " | Daeodon=" << (g_config.daeodon_enabled ? "ON" : "OFF")
            << " x" << g_config.daeodon_heal_multiplier
            << " (PvP x" << g_config.daeodon_pvp_heal_multiplier << ')'
            << " | boosts=" << g_daeodon_boosts
            << " | tracked=" << g_stored_at.size()
            << " | pending=" << g_pending_recoveries.size();
    Send(pc, message.str());
    const std::int64_t now = UnixNow();
    int shown = 0;
    for (const auto& entry : g_stored_at) {
        if (shown >= 5) break;
        const std::int64_t seconds = std::max<std::int64_t>(0, now - entry.second);
        const float rate = IsOnPvpCooldown(pc) ? g_config.pvp_heal_percent_per_minute
                                               : g_config.heal_percent_per_minute;
        const int approx = std::clamp(static_cast<int>(std::floor(seconds / 60.0 * rate)), 0, 100);
        std::ostringstream line;
        line << "Dino " << entry.first.id1 << ':' << entry.first.id2
             << " stored " << seconds / 60 << "m " << seconds % 60 << "s"
             << " | approx recovery +" << approx << "% max HP";
        Send(pc, line.str());
        ++shown;
    }
    if (g_stored_at.size() > 5)
        Send(pc, "Showing 5 of " + std::to_string(g_stored_at.size()) + " stored dinos.");
}

} // namespace CryoRecovery

void Load() {
    Log::Get().Init("CryoRecovery");
    Log::GetLog()->info("Loading plugin - CryoRecovery v1.3 StatusAndPvpLimits");
    try { CryoRecovery::ReadConfig(); }
    catch (const std::exception& error) {
        Log::GetLog()->error("CryoRecovery: config error ({}), using defaults", error.what());
        CryoRecovery::g_config = CryoRecovery::Config();
    }
    CryoRecovery::LoadState();
    ArkApi::GetCommands().AddOnTimerCallback(
        "CryoRecovery.DeferredHeal", &CryoRecovery::RecoveryTimer);

    CryoRecovery::g_capture_hook_installed = ArkApi::GetHooks().SetHook(
        "APrimalDinoCharacter.GetDinoData",
        &CryoRecovery::Hook_APrimalDinoCharacter_GetDinoData,
        &CryoRecovery::APrimalDinoCharacter_GetDinoData_original);
    CryoRecovery::g_spawn_hook_installed = ArkApi::GetHooks().SetHook(
        "APrimalDinoCharacter.SpawnFromDinoDataEx",
        &CryoRecovery::Hook_APrimalDinoCharacter_SpawnFromDinoDataEx,
        &CryoRecovery::APrimalDinoCharacter_SpawnFromDinoDataEx_original);
    CryoRecovery::g_set_health_hook_installed = ArkApi::GetHooks().SetHook(
        "APrimalCharacter.SetHealth",
        &CryoRecovery::Hook_APrimalCharacter_SetHealth,
        &CryoRecovery::APrimalCharacter_SetHealth_original);
    CryoRecovery::g_can_use_hook_installed = ArkApi::GetHooks().SetHook(
        "UPrimalItem.CanUse", &CryoRecovery::Hook_UPrimalItem_CanUse,
        &CryoRecovery::UPrimalItem_CanUse_original);

    if (!CryoRecovery::g_capture_hook_installed)
        Log::GetLog()->error("CryoRecovery: GetDinoData hook installation failed");
    if (!CryoRecovery::g_spawn_hook_installed)
        Log::GetLog()->error("CryoRecovery: SpawnFromDinoDataEx hook installation failed");
    if (!CryoRecovery::g_set_health_hook_installed)
        Log::GetLog()->error("CryoRecovery: SetHealth hook installation failed; Daeodon boost unavailable");
    if (!CryoRecovery::g_can_use_hook_installed)
        Log::GetLog()->error("CryoRecovery: CanUse hook failed; RAID/PvP release limit unavailable");

    if (!CryoRecovery::g_config.status_command.empty()) {
        ArkApi::GetCommands().AddChatCommand(
            FString(CryoRecovery::g_config.status_command.c_str()),
            &CryoRecovery::StatusCommand);
    }
    Log::GetLog()->info(
        "CryoRecovery v1.3 ready (cryo={}%%/min, PvP cryo={}%%/min, apply_delay={}s, Daeodon=x{}, PvP Daeodon=x{}, release_limit={}/{}, capture_hook={}, spawn_hook={}, health_hook={}, can_use_hook={})",
        CryoRecovery::g_config.heal_percent_per_minute,
        CryoRecovery::g_config.pvp_heal_percent_per_minute,
        CryoRecovery::g_config.post_spawn_apply_delay_seconds,
        CryoRecovery::g_config.daeodon_heal_multiplier,
        CryoRecovery::g_config.daeodon_pvp_heal_multiplier,
        CryoRecovery::g_config.max_releases,
        CryoRecovery::g_config.max_large_releases,
        CryoRecovery::g_capture_hook_installed,
        CryoRecovery::g_spawn_hook_installed,
        CryoRecovery::g_set_health_hook_installed,
        CryoRecovery::g_can_use_hook_installed);
}

void Unload() {
    CryoRecovery::SaveState();
    ArkApi::GetCommands().RemoveOnTimerCallback("CryoRecovery.DeferredHeal");
    if (!CryoRecovery::g_config.status_command.empty())
        ArkApi::GetCommands().RemoveChatCommand(FString(CryoRecovery::g_config.status_command.c_str()));
    if (CryoRecovery::g_capture_hook_installed)
        ArkApi::GetHooks().DisableHook(
            "APrimalDinoCharacter.GetDinoData",
            &CryoRecovery::Hook_APrimalDinoCharacter_GetDinoData);
    if (CryoRecovery::g_spawn_hook_installed)
        ArkApi::GetHooks().DisableHook(
            "APrimalDinoCharacter.SpawnFromDinoDataEx",
            &CryoRecovery::Hook_APrimalDinoCharacter_SpawnFromDinoDataEx);
    if (CryoRecovery::g_set_health_hook_installed)
        ArkApi::GetHooks().DisableHook(
            "APrimalCharacter.SetHealth",
            &CryoRecovery::Hook_APrimalCharacter_SetHealth);
    if (CryoRecovery::g_can_use_hook_installed)
        ArkApi::GetHooks().DisableHook(
            "UPrimalItem.CanUse", &CryoRecovery::Hook_UPrimalItem_CanUse);
    CryoRecovery::g_stored_at.clear();
    CryoRecovery::g_pending_recoveries.clear();
    CryoRecovery::g_release_events.clear();
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try { Load(); }
    catch (const std::exception& error) {
        Log::Get().Init("CryoRecovery");
        Log::GetLog()->error("CryoRecovery failed to initialize: {}", error.what());
    }
    catch (...) {
        Log::Get().Init("CryoRecovery");
        Log::GetLog()->error("CryoRecovery failed to initialize with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try { Unload(); }
    catch (const std::exception& error) {
        Log::GetLog()->error("CryoRecovery unload exception: {}", error.what());
    }
    catch (...) {
        Log::GetLog()->error("CryoRecovery unload unknown exception");
    }
}
