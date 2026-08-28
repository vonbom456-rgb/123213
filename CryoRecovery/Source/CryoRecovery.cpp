#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "../../DamageAlerts/Source/MiniJson.h"

namespace CryoRecovery {

struct Config {
    bool enabled = true;
    float heal_percent_per_minute = 2.0f;
    float pvp_heal_percent_per_minute = 1.5f;
    int minimum_stored_seconds = 5;
    float maximum_counted_hours = 24.0f;
    bool notify_player = true;
    std::string status_command = "/cryoheal";
    std::string healed_message = "Cryopod recovery: +{0} HP ({1}% total).";
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
bool g_capture_hook_installed = false;
bool g_spawn_hook_installed = false;

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
    value.notify_player = minijson::boolean(root, "Messages", "NotifyPlayer", value.notify_player);
    value.status_command = minijson::str(root, "Messages", "StatusCommand", value.status_command);
    value.healed_message = minijson::str(root, "Messages", "Healed", value.healed_message);

    value.heal_percent_per_minute = std::max(0.0f, value.heal_percent_per_minute);
    value.pvp_heal_percent_per_minute = std::max(0.0f, value.pvp_heal_percent_per_minute);
    value.minimum_stored_seconds = std::max(0, value.minimum_stored_seconds);
    value.maximum_counted_hours = std::max(0.0f, value.maximum_counted_hours);
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

APrimalDinoCharacter* Hook_APrimalDinoCharacter_SpawnFromDinoDataEx(
    FARKDinoData* in_data, UWorld* world, FVector* location, FRotator* rotation,
    bool* duped_dino, int for_team, bool generate_new_id,
    AShooterPlayerController* tamer_controller, bool begin_play) {
    APrimalDinoCharacter* dino = APrimalDinoCharacter_SpawnFromDinoDataEx_original(
        in_data, world, location, rotation, duped_dino, for_team, generate_new_id,
        tamer_controller, begin_play);
    if (!g_config.enabled || !dino) return dino;

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
    const double max_seconds = static_cast<double>(g_config.maximum_counted_hours) * 3600.0;
    const double counted_seconds = g_config.maximum_counted_hours > 0.0f
        ? std::min<double>(elapsed_seconds, max_seconds)
        : static_cast<double>(elapsed_seconds);

    const float old_health = dino->GetHealth();
    const float max_health = dino->GetMaxHealth();
    if (rate <= 0.0f || max_health <= 0.0f || old_health >= max_health) return dino;

    const double healed_percent = counted_seconds / 60.0 * static_cast<double>(rate);
    const float heal_amount = static_cast<float>(max_health * healed_percent / 100.0);
    const float new_health = std::min(max_health, old_health + heal_amount);
    const float applied = std::max(0.0f, new_health - old_health);
    if (applied <= 0.0f) return dino;

    dino->SetHealth(new_health);
    Log::GetLog()->info(
        "CryoRecovery: released dino id={}:{} stored={}s pvp={} rate={}%%/min health={} -> {} / {}",
        key.id1, key.id2, elapsed_seconds, pvp, rate, old_health, new_health, max_health);

    if (g_config.notify_player && tamer_controller) {
        const int total_percent = static_cast<int>(std::lround((new_health / max_health) * 100.0f));
        std::string message = ReplaceToken(
            g_config.healed_message, "{0}", std::to_string(static_cast<long long>(std::llround(applied))));
        message = ReplaceToken(message, "{1}", std::to_string(total_percent));
        Send(tamer_controller, message);
    }
    return dino;
}

void StatusCommand(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!pc) return;
    std::ostringstream message;
    message << "CryoRecovery: " << (g_config.enabled ? "ON" : "OFF")
            << " | normal=" << g_config.heal_percent_per_minute << "%/min"
            << " | RAID/PvP=" << g_config.pvp_heal_percent_per_minute << "%/min"
            << " | tracked=" << g_stored_at.size();
    Send(pc, message.str());
}

} // namespace CryoRecovery

void Load() {
    Log::Get().Init("CryoRecovery");
    Log::GetLog()->info("Loading plugin - CryoRecovery v1.0");
    try { CryoRecovery::ReadConfig(); }
    catch (const std::exception& error) {
        Log::GetLog()->error("CryoRecovery: config error ({}), using defaults", error.what());
        CryoRecovery::g_config = CryoRecovery::Config();
    }
    CryoRecovery::LoadState();

    CryoRecovery::g_capture_hook_installed = ArkApi::GetHooks().SetHook(
        "APrimalDinoCharacter.GetDinoData",
        &CryoRecovery::Hook_APrimalDinoCharacter_GetDinoData,
        &CryoRecovery::APrimalDinoCharacter_GetDinoData_original);
    CryoRecovery::g_spawn_hook_installed = ArkApi::GetHooks().SetHook(
        "APrimalDinoCharacter.SpawnFromDinoDataEx",
        &CryoRecovery::Hook_APrimalDinoCharacter_SpawnFromDinoDataEx,
        &CryoRecovery::APrimalDinoCharacter_SpawnFromDinoDataEx_original);

    if (!CryoRecovery::g_capture_hook_installed)
        Log::GetLog()->error("CryoRecovery: GetDinoData hook installation failed");
    if (!CryoRecovery::g_spawn_hook_installed)
        Log::GetLog()->error("CryoRecovery: SpawnFromDinoDataEx hook installation failed");

    if (!CryoRecovery::g_config.status_command.empty()) {
        ArkApi::GetCommands().AddChatCommand(
            FString(CryoRecovery::g_config.status_command.c_str()),
            &CryoRecovery::StatusCommand);
    }
    Log::GetLog()->info(
        "CryoRecovery v1.0 ready (normal={}%%/min, RAID/PvP={}%%/min, capture_hook={}, spawn_hook={})",
        CryoRecovery::g_config.heal_percent_per_minute,
        CryoRecovery::g_config.pvp_heal_percent_per_minute,
        CryoRecovery::g_capture_hook_installed,
        CryoRecovery::g_spawn_hook_installed);
}

void Unload() {
    CryoRecovery::SaveState();
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
    CryoRecovery::g_stored_at.clear();
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
