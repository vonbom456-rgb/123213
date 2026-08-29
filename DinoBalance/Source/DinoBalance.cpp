#include <API/ARK/Ark.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace DinoBalance {

struct Config {
    bool enabled = true;
    float max_health = 328000.0f;
    float max_damage_per_hit = 6000.0f;
    int enforcement_interval_seconds = 5;
    std::string class_token = "cherufe";
};

Config g_config;
std::chrono::steady_clock::time_point g_next_scan{};

std::string PluginDir() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/DinoBalance";
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string FullName(UObject* object) {
    if (!object) return {};
    FString name;
    object->GetFullName(&name, nullptr);
    return Lower(name.ToString());
}

void ReadConfig() {
    std::ifstream file(PluginDir() + "/config.json", std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("cannot open DinoBalance/config.json");
    std::ostringstream stream;
    stream << file.rdbuf();
    const auto root = minijson::parse(stream.str());

    Config value;
    value.enabled = minijson::boolean(root, "Magmasaur", "Enabled", value.enabled);
    value.max_health = minijson::number(root, "Magmasaur", "MaxHealth", value.max_health);
    value.max_damage_per_hit = minijson::number(
        root, "Magmasaur", "MaxDamagePerHit", value.max_damage_per_hit);
    value.enforcement_interval_seconds = minijson::integer(
        root, "Magmasaur", "EnforcementIntervalSeconds", value.enforcement_interval_seconds);
    value.class_token = Lower(minijson::str(root, "Magmasaur", "ClassToken", value.class_token));

    value.max_health = std::clamp(value.max_health, 1000.0f, 10000000.0f);
    value.max_damage_per_hit = std::clamp(value.max_damage_per_hit, 100.0f, 1000000.0f);
    value.enforcement_interval_seconds = std::clamp(value.enforcement_interval_seconds, 1, 60);
    if (value.class_token.empty()) value.class_token = "cherufe";
    g_config = value;
}

bool IsMagmasaur(UObject* object) {
    if (!g_config.enabled || !object) return false;
    const std::string name = FullName(object);
    return name.find(g_config.class_token) != std::string::npos ||
           name.find("magmasaur") != std::string::npos;
}

APrimalDinoCharacter* FindMagmasaurAttacker(AController* controller, AActor* causer) {
    auto as_magmasaur = [](AActor* actor) -> APrimalDinoCharacter* {
        if (!actor || !actor->IsA(APrimalDinoCharacter::GetPrivateStaticClass()) ||
            !IsMagmasaur(actor)) return nullptr;
        return static_cast<APrimalDinoCharacter*>(actor);
    };

    if (auto* dino = as_magmasaur(causer)) return dino;
    if (causer) {
        if (auto* dino = as_magmasaur(causer->InstigatorField())) return dino;
        if (auto* dino = as_magmasaur(causer->OwnerField())) return dino;
    }
    if (controller) {
        if (auto* dino = as_magmasaur(controller->PawnField())) return dino;
    }
    return nullptr;
}

bool EnforceHealth(APrimalDinoCharacter* dino) {
    if (!dino || dino->bIsDead()() || !IsMagmasaur(dino)) return false;
    UPrimalCharacterStatusComponent* status = dino->MyCharacterStatusComponentField();
    if (!status) return false;

    const auto health = EPrimalCharacterStatusValue::Health;
    const float before = dino->GetMaxStatusValue(health);
    if (before <= g_config.max_health + 0.5f) return false;

    status->RescaleMaxStat(health, g_config.max_health, false);
    // Some modded character BPs recalculate their max after RescaleMaxStat.
    // Clamp both authoritative and replicated arrays as a hard backstop.
    status->MaxStatusValuesField()()[static_cast<int>(health)] = g_config.max_health;
    status->ReplicatedGlobalMaxStatusValuesField()()[static_cast<int>(health)] = g_config.max_health;
    if (dino->GetHealth() > g_config.max_health)
        dino->SetHealth(g_config.max_health);
    dino->ForceNetUpdate(false, true, false);
    return true;
}

void EnforceAll() {
    if (!g_config.enabled) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_scan) return;
    g_next_scan = now + std::chrono::seconds(g_config.enforcement_interval_seconds);

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return;
    TArray<AActor*> dinos;
    UGameplayStatics::GetAllActorsOfClass(
        world, TSubclassOf<AActor>(APrimalDinoCharacter::GetPrivateStaticClass()), &dinos);
    int changed = 0;
    for (AActor* actor : dinos) {
        if (actor && actor->IsA(APrimalDinoCharacter::GetPrivateStaticClass()) &&
            EnforceHealth(static_cast<APrimalDinoCharacter*>(actor))) ++changed;
    }
    if (changed > 0)
        Log::GetLog()->info("DinoBalance: capped {} Magmasaur health value(s) at {}",
            changed, static_cast<int>(g_config.max_health));
}

void Reload(APlayerController*, FString*, bool) {
    try {
        ReadConfig();
        g_next_scan = std::chrono::steady_clock::time_point{};
        EnforceAll();
        Log::GetLog()->info("DinoBalance config reloaded");
    } catch (const std::exception& error) {
        Log::GetLog()->error("DinoBalance reload failed: {}", error.what());
    }
}

DECLARE_HOOK(APrimalDinoCharacter_BeginPlay, void, APrimalDinoCharacter*);
DECLARE_HOOK(APrimalTargetableActor_AdjustDamage, void, APrimalTargetableActor*, float*,
    FDamageEvent*, AController*, AActor*);

void Hook_APrimalDinoCharacter_BeginPlay(APrimalDinoCharacter* dino) {
    APrimalDinoCharacter_BeginPlay_original(dino);
    EnforceHealth(dino);
}

void Hook_APrimalTargetableActor_AdjustDamage(APrimalTargetableActor* target, float* damage,
    FDamageEvent* event, AController* instigator, AActor* causer) {
    APrimalTargetableActor_AdjustDamage_original(target, damage, event, instigator, causer);
    if (!damage || *damage <= 0.0f) return;
    if (FindMagmasaurAttacker(instigator, causer))
        *damage = std::min(*damage, g_config.max_damage_per_hit);
}

} // namespace DinoBalance

void Load() {
    Log::Get().Init("DinoBalance");
    DinoBalance::ReadConfig();
    ArkApi::GetHooks().SetHook("APrimalDinoCharacter.BeginPlay",
        reinterpret_cast<LPVOID>(&DinoBalance::Hook_APrimalDinoCharacter_BeginPlay),
        &DinoBalance::APrimalDinoCharacter_BeginPlay_original);
    ArkApi::GetHooks().SetHook("APrimalTargetableActor.AdjustDamage",
        reinterpret_cast<LPVOID>(&DinoBalance::Hook_APrimalTargetableActor_AdjustDamage),
        &DinoBalance::APrimalTargetableActor_AdjustDamage_original);
    ArkApi::GetCommands().AddOnTimerCallback("DinoBalance.Enforce", &DinoBalance::EnforceAll);
    ArkApi::GetCommands().AddConsoleCommand("DinoBalance.Reload", &DinoBalance::Reload);
    Log::GetLog()->info("Loaded plugin - DinoBalance v1.0 (Magmasaur HP cap={}, damage cap={})",
        static_cast<int>(DinoBalance::g_config.max_health),
        static_cast<int>(DinoBalance::g_config.max_damage_per_hit));
}

void Unload() {
    ArkApi::GetCommands().RemoveConsoleCommand("DinoBalance.Reload");
    ArkApi::GetCommands().RemoveOnTimerCallback("DinoBalance.Enforce");
    ArkApi::GetHooks().DisableHook("APrimalTargetableActor.AdjustDamage",
        reinterpret_cast<LPVOID>(&DinoBalance::Hook_APrimalTargetableActor_AdjustDamage));
    ArkApi::GetHooks().DisableHook("APrimalDinoCharacter.BeginPlay",
        reinterpret_cast<LPVOID>(&DinoBalance::Hook_APrimalDinoCharacter_BeginPlay));
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try { Load(); }
    catch (const std::exception& error) {
        Log::Get().Init("DinoBalance");
        Log::GetLog()->error("DinoBalance failed to initialize: {}", error.what());
    }
    catch (...) {
        Log::Get().Init("DinoBalance");
        Log::GetLog()->error("DinoBalance failed with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try { Unload(); } catch (...) {}
}
