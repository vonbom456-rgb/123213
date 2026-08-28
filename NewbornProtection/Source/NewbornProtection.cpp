#include <API/ARK/Ark.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace NewbornProtection {
struct Config {
    bool enabled = true;
    int protection_seconds = 300;
    float maximum_baby_age_when_detected = 0.01f;
    int scan_interval_seconds = 5;
    bool disable_during_pvp = true;
};
Config g_config;
struct Entry { TWeakObjectPtr<APrimalDinoCharacter> dino; std::chrono::steady_clock::time_point expires; };
std::unordered_map<APrimalDinoCharacter*, Entry> g_protected;
std::chrono::steady_clock::time_point g_next_scan{};
bool g_damage_hook = false;
using PvpCooldownQuery = bool(__fastcall*)(AShooterPlayerController*);

std::string PluginDir() { return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/NewbornProtection"; }
void ReadConfig() {
    std::ifstream file(PluginDir() + "/config.json", std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("cannot open NewbornProtection/config.json");
    std::ostringstream stream; stream << file.rdbuf(); const auto root = minijson::parse(stream.str());
    Config value;
    value.enabled = minijson::boolean(root, "Protection", "Enabled", value.enabled);
    value.protection_seconds = minijson::integer(root, "Protection", "DurationSeconds", value.protection_seconds);
    value.maximum_baby_age_when_detected = minijson::number(root, "Protection", "MaximumBabyAgeWhenDetected", value.maximum_baby_age_when_detected);
    value.scan_interval_seconds = minijson::integer(root, "Protection", "ScanIntervalSeconds", value.scan_interval_seconds);
    value.disable_during_pvp = minijson::boolean(root, "Protection", "DisableDuringPvp", value.disable_during_pvp);
    value.protection_seconds = std::clamp(value.protection_seconds, 0, 3600);
    value.maximum_baby_age_when_detected = std::clamp(value.maximum_baby_age_when_detected, 0.0001f, 0.05f);
    value.scan_interval_seconds = std::clamp(value.scan_interval_seconds, 2, 60);
    g_config = value;
}

bool TeamInPvp(int team) {
    if (!g_config.disable_during_pvp || team <= 0) return false;
    const HMODULE module = GetModuleHandleA("PvPCooldowns.dll");
    if (!module) return false;
    const auto query = reinterpret_cast<PvpCooldownQuery>(GetProcAddress(module, "PvpCooldowns_IsOnCooldown"));
    if (!query) return false;
    UWorld* world = ArkApi::GetApiUtils().GetWorld(); if (!world) return false;
    for (TWeakObjectPtr<APlayerController> weak : world->PlayerControllerListField()) {
        APlayerController* base = weak.Get();
        if (!base || !base->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base);
        if (ArkApi::GetApiUtils().GetTribeID(pc) == team && query(pc)) return true;
    }
    return false;
}

void Scan() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_scan) return;
    g_next_scan = now + std::chrono::seconds(g_config.scan_interval_seconds);
    for (auto it = g_protected.begin(); it != g_protected.end();) {
        if (!it->second.dino.Get() || now >= it->second.expires) it = g_protected.erase(it); else ++it;
    }
    if (!g_config.enabled || g_config.protection_seconds <= 0) return;
    UWorld* world = ArkApi::GetApiUtils().GetWorld(); if (!world) return;
    TArray<AActor*> actors;
    UGameplayStatics::GetAllActorsOfClass(world,
        TSubclassOf<AActor>(APrimalDinoCharacter::GetPrivateStaticClass()), &actors);
    for (AActor* actor : actors) {
        if (!actor || !actor->IsA(APrimalDinoCharacter::GetPrivateStaticClass())) continue;
        auto* dino = static_cast<APrimalDinoCharacter*>(actor);
        if (!dino->BPIsTamed() || !dino->bIsBaby()() || dino->BabyAgeField() > g_config.maximum_baby_age_when_detected) continue;
        if (g_protected.find(dino) != g_protected.end()) continue;
        g_protected.emplace(dino, Entry{GetWeakReference(dino), now + std::chrono::seconds(g_config.protection_seconds)});
        Log::GetLog()->info("NewbornProtection: protected dino ids={}:{} team={} for {}s",
            dino->DinoID1Field(), dino->DinoID2Field(), dino->TargetingTeamField(), g_config.protection_seconds);
    }
}

DECLARE_HOOK(APrimalDinoCharacter_TakeDamage, float, APrimalDinoCharacter*, float, FDamageEvent*, AController*, AActor*);
float Hook_APrimalDinoCharacter_TakeDamage(APrimalDinoCharacter* dino, float damage,
    FDamageEvent* event, AController* instigator, AActor* causer) {
    if (g_config.enabled && dino && damage > 0.0f) {
        const auto it = g_protected.find(dino);
        if (it != g_protected.end() && std::chrono::steady_clock::now() < it->second.expires &&
            !TeamInPvp(dino->TargetingTeamField())) return 0.0f;
    }
    return APrimalDinoCharacter_TakeDamage_original(dino, damage, event, instigator, causer);
}

void Reload(APlayerController*, FString*, bool) { try { ReadConfig(); } catch (const std::exception& e) { Log::GetLog()->error("NewbornProtection reload: {}", e.what()); } }
}

void Load() {
    Log::Get().Init("NewbornProtection"); NewbornProtection::ReadConfig();
    ArkApi::GetCommands().AddOnTimerCallback("NewbornProtection.Scan", &NewbornProtection::Scan);
    NewbornProtection::g_damage_hook = ArkApi::GetHooks().SetHook("APrimalDinoCharacter.TakeDamage",
        &NewbornProtection::Hook_APrimalDinoCharacter_TakeDamage, &NewbornProtection::APrimalDinoCharacter_TakeDamage_original);
    ArkApi::GetCommands().AddConsoleCommand("NewbornProtection.Reload", &NewbornProtection::Reload);
    Log::GetLog()->info("Loaded plugin - NewbornProtection v1.0 (duration={}s, hook={})", NewbornProtection::g_config.protection_seconds, NewbornProtection::g_damage_hook);
}
void Unload() {
    ArkApi::GetCommands().RemoveOnTimerCallback("NewbornProtection.Scan");
    ArkApi::GetCommands().RemoveConsoleCommand("NewbornProtection.Reload");
    if (NewbornProtection::g_damage_hook) ArkApi::GetHooks().DisableHook("APrimalDinoCharacter.TakeDamage", &NewbornProtection::Hook_APrimalDinoCharacter_TakeDamage);
    NewbornProtection::g_protected.clear();
}
extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept { try { Load(); } catch (...) {} }
extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept { try { Unload(); } catch (...) {} }
