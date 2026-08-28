#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>

#if defined(TURRETCONTROL_WITH_PERMISSIONS) && TURRETCONTROL_WITH_PERMISSIONS
#include <ArkPermissions.h>
#pragma comment(lib, "Permissions.lib")
#endif

#include "MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace TurretControl {

constexpr const char* kPluginName = "TurretControl";

struct Config {
    std::string fill_command = "/fill";
    std::string turrets_command = "/turrets";
    float fill_radius = 3000.0f;
    int heavy_ammo_limit = 10000;
    int tek_ammo_limit = 5000;
    int auto_ammo_limit = 10000;
    bool require_same_tribe = true;
    bool allow_during_pvp_cooldown = false;
    bool show_messages = true;
    bool inventory_cap_enabled = true;
    bool hard_cap_enabled = true;
    int hard_cap_interval_seconds = 1;
    float hard_cap_scan_radius = 5000.0f;
    int startup_delay_seconds = 60;

    // The server still calculates targeting, damage and ammunition. This
    // switch only stops the cosmetic per-shot projectile/trail request sent
    // to nearby clients, which can overflow ASE's reliable network queue
    // when a large S+ turret wall fires at one target.
    bool disable_client_projectile_effects = true;
    float network_scan_radius = 12000.0f;
    // v2.4 uses measured S+ firing paths: fewer real DoFire cycles, with each
    // cycle carrying the same aggregate damage and ammunition as the grouped
    // original shots. Diagnostics remain active to verify the reduction.
    bool shot_batching_enabled = true;
    int shots_per_network_event = 5;
    bool rpc_diagnostics_enabled = true;
    int rpc_log_interval_seconds = 10;

    bool pvp_placement_cooldown_enabled = true;
    float pvp_placement_cooldown_seconds = 3.0f;
    std::vector<std::string> pvp_placement_class_tokens{
        "turret", "plantspeciesx", "forcefield", "shield", "generator"
    };

    bool use_permissions = false;
    std::string permission = "TurretControl.Default";

    // Purely cosmetic range preview. The native sphere needs no mod or asset
    // path and is sent through the requesting player's controller, so it is
    // visible only to that player. The optional Blueprint actor remains as a
    // fallback for servers that already own a custom visual asset.
    bool fill_radius_hologram_enabled = true;
    bool fill_radius_native_sphere = true;
    float fill_radius_sphere_duration = 6.0f;
    int fill_radius_sphere_segments = 64;
    std::string fill_radius_hologram_blueprint;
    float fill_radius_hologram_spawn_distance = 0.0f;
    float fill_radius_hologram_spawn_y_offset = 0.0f;
    float fill_radius_hologram_spawn_z_offset = 0.0f;

    bool fill_notification_enabled = true;
    float fill_notification_scale = 1.1f;
    float fill_notification_time = 5.0f;

    bool vanilla_heavy = true;
    bool vanilla_tek = true;
    bool vanilla_auto = true;
    std::vector<std::string> custom_heavy_classes;
    std::vector<std::string> custom_tek_classes;
    std::vector<std::string> custom_auto_classes;

    int range_low = 0;
    int range_medium = 1;
    int range_high = 2;

    // Deliberately disabled until the server owner supplies verified values.
    // The 3.56 header exposes AISettingField(), but does not document its numeric mapping.
    int targeting_players_only = -1;
    int targeting_players_and_tames = -1;

    std::string sender = "TurretControl";
    std::string no_permission = "You do not have permission.";
    std::string no_turrets = "No valid turrets found.";
    std::string no_ammo = "No suitable ammunition found.";
    std::string already_full = "All valid turrets are already at their configured ammo limit.";
    std::string fill_success = "Filled {0} turrets | ARB used: {1} | Shards used: {2}";
    std::string fill_failed = "Fill failed. Check ArkApi log for TurretControl details.";
    std::string turret_success = "Updated {0} turrets.";
    std::string targeting_unconfigured = "Targeting command is disabled until TargetingValues are configured.";
    std::string pvp_blocked = "Turret controls and /fill are blocked during PvP cooldown.";
    std::string pvp_placement_blocked = "RAID / PVP: wait {0}s before placing another defensive structure.";
    std::string reload_ok = "TurretControl config reloaded.";
    std::string reload_failed = "TurretControl config reload failed.";
    std::string hard_cap_refund = "Turret ammo cap enforced: {0} overflow ammo returned to your inventory.";
};

Config g_config;
std::string g_registered_fill_command;
std::string g_registered_turrets_command;
std::vector<UClass*> g_custom_heavy;
std::vector<UClass*> g_custom_tek;
std::vector<UClass*> g_custom_auto;
bool g_internal_cap_bypass = false;

using PvpCooldownChecker = bool(__fastcall*)(AShooterPlayerController*);
PvpCooldownChecker g_pvp_checker = nullptr;

std::unordered_map<uint64, std::chrono::steady_clock::time_point> g_last_pvp_defense_placement;

struct TurretBatchState {
    float fire_interval = 0.0f;
    float fire_damage = 0.0f;
    int ammo_per_shot = 0;
};

std::unordered_map<APrimalStructureTurret*, TurretBatchState> g_turret_batch_states;

struct TurretRpcStats {
    std::string class_name;
    std::uint64_t client_rpc_window = 0;
    std::uint64_t client_rpc_total = 0;
    std::uint64_t do_fire_window = 0;
    std::uint64_t do_fire_total = 0;
    std::uint64_t projectile_window = 0;
    std::uint64_t projectile_total = 0;
    std::uint64_t damage_window = 0;
    std::uint64_t damage_total = 0;
};

std::unordered_map<UClass*, TurretRpcStats> g_turret_rpc_stats;
std::unordered_set<UClass*> g_logged_turret_classes;
std::chrono::steady_clock::time_point g_next_rpc_log = std::chrono::steady_clock::now();


DECLARE_HOOK(UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity,
    bool,
    UPrimalInventoryComponent*,
    UPrimalItem*,
    const int*,
    int*);

DECLARE_HOOK(UPrimalInventoryComponent_AllowAddInventoryItem,
    bool,
    UPrimalInventoryComponent*,
    UPrimalItem*,
    int*,
    bool);

DECLARE_HOOK(UPrimalInventoryComponent_RemoteInventoryAllowAddItems,
    bool,
    UPrimalInventoryComponent*,
    AShooterPlayerController*,
    UPrimalItem*,
    int*,
    bool);

DECLARE_HOOK(APrimalStructure_IsAllowedToBuild,
    int,
    APrimalStructure*,
    APlayerController*,
    FVector,
    FRotator,
    FPlacementData*,
    bool,
    FRotator,
    bool);

DECLARE_HOOK(APrimalStructure_PlacedStructure,
    void,
    APrimalStructure*,
    AShooterPlayerController*);

// Exact ASE ArkApi 3.56 signature from API/ARK/PrimalStructure.h.
// This hook is diagnostic-only: it always calls the original implementation.
DECLARE_HOOK(APrimalStructureTurret_ClientsFireProjectile_Implementation,
    void,
    APrimalStructureTurret*,
    FVector,
    FVector_NetQuantizeNormal);

DECLARE_HOOK(APrimalStructureTurret_DoFire,
    void,
    APrimalStructureTurret*,
    int);

DECLARE_HOOK(APrimalStructureTurret_DoFireProjectile,
    void,
    APrimalStructureTurret*,
    FVector,
    FVector);

DECLARE_HOOK(APrimalStructureTurret_DealDamage,
    void,
    APrimalStructureTurret*,
    FHitResult*,
    FVector*,
    int,
    TSubclassOf<UDamageType>,
    float);

std::string ConfigPath() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/TurretControl/config.json";
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ReplaceToken(std::string text, const std::string& token, const std::string& value) {
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
    return text;
}

FString F(const std::string& utf8) {
    return FString(ArkApi::Tools::Utf8Decode(utf8).c_str());
}

void Send(AShooterPlayerController* pc, const std::string& text) {
    if (!pc || !g_config.show_messages) return;
    const FString sender = F(g_config.sender);
    const FString msg = F(text);
    ArkApi::GetApiUtils().SendChatMessage(pc, sender, *msg);
}

void SendFillNotification(AShooterPlayerController* pc, const std::string& text) {
    if (!pc || text.empty() || !g_config.fill_notification_enabled) return;
    const FString msg = F(text);
    ArkApi::GetApiUtils().SendNotification(
        pc, FColorList::Green, g_config.fill_notification_scale,
        g_config.fill_notification_time, pc->PingIcon_StructuresField(), *msg);
}

std::string GetClassFullName(UClass* cls) {
    if (!cls) return {};
    UObject* cdo = cls->GetDefaultObject(true);
    if (!cdo) return {};
    FString name;
    cdo->GetFullName(&name, nullptr);
    return name.ToString();
}

std::string GetClassFullName(UObject* obj) {
    return obj ? GetClassFullName(obj->ClassField()) : std::string{};
}

bool IsValidTurret(APrimalStructureTurret* turret) {
    if (!turret) return false;
    if (turret->IsDead()) return false;
    if (!turret->RootComponentField()) return false;
    if (!turret->MyInventoryComponentField()) return false;
    return true;
}

bool ClassMatches(APrimalStructureTurret* turret, const std::vector<UClass*>& classes) {
    if (!turret) return false;
    for (UClass* cls : classes) {
        if (cls && turret->IsA(cls)) return true;
    }
    return false;
}

enum class TurretKind { Unsupported, Heavy, Tek, Auto };

TurretKind DetectTurretKind(APrimalStructureTurret* turret) {
    if (!turret) return TurretKind::Unsupported;

    if (ClassMatches(turret, g_custom_heavy)) return TurretKind::Heavy;
    if (ClassMatches(turret, g_custom_tek)) return TurretKind::Tek;
    if (ClassMatches(turret, g_custom_auto)) return TurretKind::Auto;

    const std::string class_name = ToLower(GetClassFullName(turret));
    const std::string ammo_name = ToLower(GetClassFullName(turret->AmmoItemTemplateField().uClass));

    // Tek is safest to identify by the turret ammo template; this also covers S+/SS derivatives.
    if (g_config.vanilla_tek &&
        (ammo_name.find("elementshard") != std::string::npos ||
         class_name.find("turrettek") != std::string::npos ||
         class_name.find("autoturrettek") != std::string::npos ||
         class_name.find("tek_turret") != std::string::npos)) {
        return TurretKind::Tek;
    }

    // Heavy and normal auto both use ARB, so class identity is used to separate them.
    if (g_config.vanilla_heavy &&
        (class_name.find("heavy") != std::string::npos) &&
        (class_name.find("turret") != std::string::npos)) {
        return TurretKind::Heavy;
    }

    if (g_config.vanilla_auto &&
        class_name.find("turret") != std::string::npos &&
        class_name.find("rocket") == std::string::npos &&
        class_name.find("minigun") == std::string::npos &&
        (ammo_name.find("advancedriflebullet") != std::string::npos ||
         class_name.find("autoturret") != std::string::npos ||
         class_name.find("turret_character") != std::string::npos)) {
        return TurretKind::Auto;
    }

    return TurretKind::Unsupported;
}

int LimitFor(TurretKind kind) {
    switch (kind) {
    case TurretKind::Heavy: return std::max(0, g_config.heavy_ammo_limit);
    case TurretKind::Tek: return std::max(0, g_config.tek_ammo_limit);
    case TurretKind::Auto: return std::max(0, g_config.auto_ammo_limit);
    default: return 0;
    }
}

struct TurretRef {
    APrimalStructureTurret* turret = nullptr;
    TurretKind kind = TurretKind::Unsupported;
    TSubclassOf<UPrimalItem> ammo;
    int current = 0;
    int capacity = 0;
    float distance = 0.0f;
    int planned = 0;
};


bool ItemMatchesKind(UPrimalItem* item, TurretKind kind) {
    if (!item || item->bIsBlueprint()() || item->bIsEngram()()) return false;
    const std::string name = ToLower(GetClassFullName(item));
    if (kind == TurretKind::Tek) {
        return name.find("elementshard") != std::string::npos;
    }
    if (kind == TurretKind::Heavy || kind == TurretKind::Auto) {
        // Do not match the generic "riflebullet" substring: it also accepts
        // Simple Rifle Ammo and other incompatible mod ammunition.
        return name.find("advancedriflebullet") != std::string::npos;
    }
    return false;
}

int FamilyQuantity(UPrimalInventoryComponent* inventory, TurretKind kind) {
    if (!inventory || kind == TurretKind::Unsupported) return 0;
    int total = 0;
    const TArray<UPrimalItem*> items = inventory->InventoryItemsField();
    for (UPrimalItem* item : items) {
        if (!ItemMatchesKind(item, kind)) continue;
        total += std::max(0, item->GetItemQuantity());
    }
    return total;
}

int ExactQuantity(UPrimalInventoryComponent* inventory, UClass* item_class) {
    if (!inventory || !item_class) return 0;
    TSubclassOf<UPrimalItem> cls(item_class);
    return inventory->GetItemTemplateQuantity(cls, nullptr, true, false, true, true);
}

int RemoveExact(UPrimalInventoryComponent* inventory, UClass* item_class, int requested) {
    if (!inventory || !item_class || requested <= 0) return 0;

    const int before = ExactQuantity(inventory, item_class);
    if (before <= 0) return 0;
    const int target = std::min(requested, before);

    const TArray<UPrimalItem*> items = inventory->InventoryItemsField();
    int attempted = 0;
    for (UPrimalItem* item : items) {
        if (attempted >= target) break;
        if (!item || item->ClassField() != item_class) continue;
        if (!item->bAllowRemovalFromInventory()() || item->bIsBlueprint()() || item->bIsEngram()()) continue;

        const int qty = item->GetItemQuantity();
        if (qty <= 0) continue;
        const int take = std::min(qty, target - attempted);

        if (take < qty) {
            item->SetQuantity(qty - take, true);
            inventory->NotifyClientsItemStatus(item, false, false, true, false, false, nullptr, nullptr, false, false, true);
            attempted += take;
        } else if (inventory->RemoveItem(&item->ItemIDField(), false, false, true, true)) {
            attempted += take;
        }
    }

    const int after = ExactQuantity(inventory, item_class);
    return std::clamp(before - after, 0, target);
}

int AddExact(UPrimalInventoryComponent* inventory, UClass* item_class, TurretKind kind, int requested) {
    if (!inventory || !item_class || requested <= 0) return 0;

    const int before = FamilyQuantity(inventory, kind);

    TSubclassOf<UPrimalItem> item_cls(item_class);
    TSubclassOf<UPrimalItem> no_skin;
    no_skin.uClass = nullptr;

    UPrimalItem::AddNewItem(
        item_cls,
        inventory,
        false,  // bEquipItem
        false,  // bDontStack
        0.0f,   // ItemQuality
        true,   // bForceNoBlueprint
        requested,
        false,  // bForceBlueprint
        0.0f,   // MaxItemDifficultyClamp
        false,  // CreateOnClient
        no_skin,
        0.0f,   // MinRandomQuality
        false,  // clampStats
        true    // bIgnoreAbsolueMaxInventory
    );

    const int after = FamilyQuantity(inventory, kind);
    return std::clamp(after - before, 0, requested);
}

// A stack removed from a turret's inventory, kept around so the caller can
// try to hand it back to a player instead of just deleting it.
struct RemovedStack {
    UClass* item_class = nullptr;
    int quantity = 0;
};

// out_removed is optional: pass it when the caller intends to refund the
// removed ammo (per-exact-class, so the correct item is added back).
int RemoveFamilyOverflow(UPrimalInventoryComponent* inventory, TurretKind kind, int requested,
                          std::vector<RemovedStack>* out_removed = nullptr) {
    if (!inventory || requested <= 0) return 0;
    int remaining = requested;
    int removed_total = 0;

    const TArray<UPrimalItem*> items = inventory->InventoryItemsField();
    for (UPrimalItem* item : items) {
        if (remaining <= 0) break;
        if (!ItemMatchesKind(item, kind)) continue;

        UClass* cls = item->ClassField();
        const int qty = std::max(0, item->GetItemQuantity());
        if (!cls || qty <= 0) continue;

        const int removed = RemoveExact(inventory, cls, std::min(qty, remaining));
        if (removed <= 0) continue;

        removed_total += removed;
        remaining -= removed;
        if (out_removed) out_removed->push_back({cls, removed});
    }
    return removed_total;
}

// Adds previously-removed stacks into another inventory (best-effort refund).
// Returns how much was actually restored; anything short of the original
// removed amount is logged (e.g. the target inventory/weight was full).
int RefundRemovedStacks(UPrimalInventoryComponent* to_inventory, TurretKind kind,
                         const std::vector<RemovedStack>& removed_stacks,
                         UPrimalInventoryComponent* restore_inventory = nullptr) {
    if (!to_inventory) return 0;
    int refunded_total = 0;
    for (const auto& stack : removed_stacks) {
        if (!stack.item_class || stack.quantity <= 0) continue;
        const int added = AddExact(to_inventory, stack.item_class, kind, stack.quantity);
        refunded_total += added;
        if (added < stack.quantity) {
            int restored = 0;
            if (restore_inventory) {
                g_internal_cap_bypass = true;
                restored = AddExact(restore_inventory, stack.item_class, kind, stack.quantity - added);
                g_internal_cap_bypass = false;
            }
            Log::GetLog()->warn(
                "TurretControl overflow refund partial: class='{}' expected={} refunded={} restored={}",
                GetClassFullName(stack.item_class), stack.quantity, added, restored);
        }
    }
    return refunded_total;
}

int TransferFamily(UPrimalInventoryComponent* from, UPrimalInventoryComponent* to,
                   APrimalStructureTurret* turret, TurretKind kind, int requested) {
    if (!from || !to || !turret || requested <= 0) return 0;

    const int before_turret = FamilyQuantity(to, kind);
    int remaining = requested;

    const TArray<UPrimalItem*> items = from->InventoryItemsField();
    for (UPrimalItem* item : items) {
        if (remaining <= 0) break;
        if (!ItemMatchesKind(item, kind)) continue;
        if (!item->bAllowRemovalFromInventory()()) continue;

        UClass* actual_class = item->ClassField();
        const int stack_qty = std::max(0, item->GetItemQuantity());
        if (!actual_class || stack_qty <= 0) continue;

        const int want = std::min(stack_qty, remaining);
        const int player_before = ExactQuantity(from, actual_class);
        const int turret_before = FamilyQuantity(to, kind);

        const int removed = RemoveExact(from, actual_class, want);
        if (removed <= 0) continue;

        const int added = AddExact(to, actual_class, kind, removed);

        if (added < removed) {
            const int refund = removed - added;
            const int refunded = AddExact(from, actual_class, kind, refund);
            if (refunded != refund) {
                Log::GetLog()->error(
                    "TurretControl v1.7: refund failed. class='{}' expected={} refunded={}",
                    GetClassFullName(actual_class), refund, refunded);
            }
        }

        const int turret_after = FamilyQuantity(to, kind);
        Log::GetLog()->info(
            "TurretControl v1.7 fill: turret='{}' template='{}' player_class='{}' player_before={} turret_before={} requested={} removed={} added={} turret_after={}",
            GetClassFullName(turret),
            GetClassFullName(turret->AmmoItemTemplateField().uClass),
            GetClassFullName(actual_class),
            player_before, turret_before, want, removed, added, turret_after);

        remaining -= added;
    }

    const int after_turret = FamilyQuantity(to, kind);
    return std::clamp(after_turret - before_turret, 0, requested);
}


std::vector<TurretRef> FindTurrets(AShooterPlayerController* pc, bool fill_only) {
    std::vector<TurretRef> result;
    if (!pc) return result;

    AShooterCharacter* character = pc->GetPlayerCharacter();
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!character || !world || !character->RootComponentField()) return result;

    const FVector player_pos = character->RootComponentField()->RelativeLocationField();
    const int player_team = ArkApi::GetApiUtils().GetTribeID(pc);

    // ArkApi 3.56 exposes the server spatial octree. Query only structure actors of the
    // turret base class inside the requested radius instead of scanning every actor/turret on the map.
    TArray<AActor*> actors;
    TSubclassOf<AActor> turret_class(APrimalStructureTurret::GetPrivateStaticClass());
    UVictoryCore::ServerOctreeOverlapActorsClass(&actors, world, player_pos, g_config.fill_radius,
        EServerOctreeGroup::STRUCTURES, turret_class, true);

    result.reserve(static_cast<size_t>(actors.Num()));
    for (AActor* actor : actors) {
        if (!actor || !actor->IsA(APrimalStructureTurret::GetPrivateStaticClass())) continue;
        auto* turret = static_cast<APrimalStructureTurret*>(actor);
        if (!IsValidTurret(turret)) continue;

        if (g_config.require_same_tribe && turret->TargetingTeamField() != player_team) continue;

        const FVector turret_pos = turret->RootComponentField()->RelativeLocationField();
        const float distance = FVector::Distance(player_pos, turret_pos);
        if (distance > g_config.fill_radius) continue;

        const TurretKind kind = DetectTurretKind(turret);
        // Filling needs a known ammo/limit profile. Mass power/range control can safely work
        // on any APrimalStructureTurret-derived structure in range, including additional mod turrets.
        if (fill_only && kind == TurretKind::Unsupported) continue;

        TurretRef ref;
        ref.turret = turret;
        ref.kind = kind;
        ref.ammo = turret->AmmoItemTemplateField();
        ref.distance = distance;

        if (fill_only) {
            UPrimalInventoryComponent* inventory = turret->MyInventoryComponentField();
            if (!inventory || !ref.ammo.uClass) continue;
            ref.current = FamilyQuantity(inventory, kind);
            ref.capacity = std::max(0, LimitFor(kind) - ref.current);
        }
        result.emplace_back(ref);
    }

    std::sort(result.begin(), result.end(), [](const TurretRef& a, const TurretRef& b) {
        return a.distance < b.distance;
    });
    return result;
}

int InventoryQuantity(UPrimalInventoryComponent* inventory, TSubclassOf<UPrimalItem> item_class) {
    if (!inventory || !item_class.uClass) return 0;
    return ExactQuantity(inventory, item_class.uClass);
}

int RemoveFromInventory(UPrimalInventoryComponent* inventory, TSubclassOf<UPrimalItem> item_class, int requested) {
    if (!inventory || !item_class.uClass || requested <= 0) return 0;
    return RemoveExact(inventory, item_class.uClass, requested);
}

int AddToInventory(UPrimalInventoryComponent* inventory, TSubclassOf<UPrimalItem> item_class, int requested) {
    if (!inventory || !item_class.uClass || requested <= 0) return 0;
    const std::string n = ToLower(GetClassFullName(item_class.uClass));
    TurretKind kind = n.find("elementshard") != std::string::npos ? TurretKind::Tek : TurretKind::Heavy;
    return AddExact(inventory, item_class.uClass, kind, requested);
}

// Allocates an ammo pool approximately evenly among turrets while respecting each deficit.
void PlanPool(std::vector<TurretRef*>& refs, int available) {
    for (TurretRef* ref : refs) if (ref) ref->planned = 0;
    if (available <= 0 || refs.empty()) return;

    std::vector<TurretRef*> active;
    for (TurretRef* ref : refs) if (ref && ref->capacity > 0) active.push_back(ref);

    while (available > 0 && !active.empty()) {
        const int share = std::max(1, available / static_cast<int>(active.size()));
        bool progress = false;
        for (auto it = active.begin(); it != active.end() && available > 0;) {
            TurretRef* ref = *it;
            const int remaining = std::max(0, ref->capacity - ref->planned);
            if (remaining == 0) {
                it = active.erase(it);
                continue;
            }
            const int give = std::min({remaining, share, available});
            ref->planned += give;
            available -= give;
            progress = progress || give > 0;
            if (ref->planned >= ref->capacity) it = active.erase(it);
            else ++it;
        }
        if (!progress) break;
    }
}

bool HasPermission(AShooterPlayerController* pc) {
    if (!g_config.use_permissions) return true;
#if defined(TURRETCONTROL_WITH_PERMISSIONS) && TURRETCONTROL_WITH_PERMISSIONS
    if (!pc) return false;
    const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
    return Permissions::IsPlayerHasPermission(steam_id, F(g_config.permission));
#else
    // Fail closed when config requests Permissions but the binary was built without it.
    return false;
#endif
}

bool IsPvpBlocked(AShooterPlayerController* pc) {
    if (g_config.allow_during_pvp_cooldown) return false;
    if (!g_pvp_checker) return false; // Core plugin remains independent of PvPCooldowns.
    return g_pvp_checker(pc);
}

uint64 PlayerCooldownKey(AShooterPlayerController* pc) {
    if (!pc) return 0;
    const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
    return steam_id != 0 ? steam_id : static_cast<uint64>(reinterpret_cast<uintptr_t>(pc));
}

bool IsPvpPlacementLimitedStructure(APrimalStructure* structure) {
    if (!structure || !g_config.pvp_placement_cooldown_enabled) return false;
    const std::string class_name = ToLower(GetClassFullName(structure));
    for (const std::string& raw_token : g_config.pvp_placement_class_tokens) {
        const std::string token = ToLower(raw_token);
        if (!token.empty() && class_name.find(token) != std::string::npos) return true;
    }
    return false;
}

int Hook_APrimalStructure_IsAllowedToBuild(
    APrimalStructure* structure, APlayerController* player_controller,
    FVector at_location, FRotator at_rotation, FPlacementData* placement_data,
    bool dont_adjust_for_max_range, FRotator player_view_rotation, bool final_placement)
{
    const int original_result = APrimalStructure_IsAllowedToBuild_original(
        structure, player_controller, at_location, at_rotation, placement_data,
        dont_adjust_for_max_range, player_view_rotation, final_placement);

    // ARK uses zero for an allowed placement. Preserve every native/modded
    // rejection, and only enforce the rate limit on the final server check.
    if (original_result != 0 || !final_placement || !player_controller ||
        !player_controller->IsA(AShooterPlayerController::GetPrivateStaticClass()) ||
        !IsPvpPlacementLimitedStructure(structure)) {
        return original_result;
    }

    auto* pc = static_cast<AShooterPlayerController*>(player_controller);
    if (!IsPvpBlocked(pc)) return original_result;

    const uint64 key = PlayerCooldownKey(pc);
    const auto it = g_last_pvp_defense_placement.find(key);
    if (key == 0 || it == g_last_pvp_defense_placement.end()) return original_result;

    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - it->second).count();
    const float cooldown = std::max(0.0f, g_config.pvp_placement_cooldown_seconds);
    if (elapsed >= cooldown) return original_result;

    const int remaining = std::max(1, static_cast<int>(std::ceil(cooldown - elapsed)));
    const std::string message = ReplaceToken(
        g_config.pvp_placement_blocked, "{0}", std::to_string(remaining));
    Send(pc, message);
    ArkApi::GetApiUtils().SendNotification(
        pc, FColorList::Yellow, 1.0f, 2.0f, pc->PingIcon_StructuresField(), *F(message));
    return 1;
}

void Hook_APrimalStructure_PlacedStructure(
    APrimalStructure* structure, AShooterPlayerController* pc)
{
    APrimalStructure_PlacedStructure_original(structure, pc);
    if (!pc || !IsPvpBlocked(pc) || !IsPvpPlacementLimitedStructure(structure)) return;

    const uint64 key = PlayerCooldownKey(pc);
    if (key != 0) {
        g_last_pvp_defense_placement[key] = std::chrono::steady_clock::now();
        Log::GetLog()->info(
            "TurretControl PvP placement cooldown started: player={} structure='{}' seconds={}",
            key, GetClassFullName(structure), g_config.pvp_placement_cooldown_seconds);
    }
}

// Purely cosmetic. The native debug sphere is a shipping-enabled ARK RPC and
// has no collision or gameplay actor. Calling the multicast RPC on the player
// controller confines it to that controller's owning client in normal network
// relevancy. An optional custom Blueprint actor can also be spawned.
void SpawnFillRadiusHologram(AShooterPlayerController* pc) {
    if (!g_config.fill_radius_hologram_enabled) return;
    if (!pc) return;

    AShooterCharacter* character = pc->GetPlayerCharacter();
    if (g_config.fill_radius_native_sphere && character && character->RootComponentField()) {
        const FVector center = character->RootComponentField()->RelativeLocationField();
        const int segments = std::max(8, std::min(128, g_config.fill_radius_sphere_segments));
        const float duration = std::max(0.5f, g_config.fill_radius_sphere_duration);
        pc->MulticastDrawDebugSphere(
            center, g_config.fill_radius, segments,
            FLinearColor(0.0f, 1.0f, 0.1f, 1.0f), duration, true);
    }

    if (g_config.fill_radius_hologram_blueprint.empty()) return;

    FString blueprint_path(g_config.fill_radius_hologram_blueprint.c_str());
    pc->SpawnActor(
        &blueprint_path,
        g_config.fill_radius_hologram_spawn_distance,
        g_config.fill_radius_hologram_spawn_y_offset,
        g_config.fill_radius_hologram_spawn_z_offset,
        false); // bDoDeferBeginPlay
}

void FillCommandImpl(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!pc || ArkApi::IApiUtils::IsPlayerDead(pc)) return;
    if (!HasPermission(pc)) { Send(pc, g_config.no_permission); return; }
    if (IsPvpBlocked(pc)) { Send(pc, g_config.pvp_blocked); return; }

    SpawnFillRadiusHologram(pc);

    AShooterCharacter* character = pc->GetPlayerCharacter();
    if (!character) return;
    UPrimalInventoryComponent* player_inventory = character->MyInventoryComponentField();
    if (!player_inventory) return;

    std::vector<TurretRef> turrets = FindTurrets(pc, true);
    if (turrets.empty()) { Send(pc, g_config.no_turrets); return; }

    bool has_deficit = false;
    int arb_available = FamilyQuantity(player_inventory, TurretKind::Heavy);
    int shards_available = FamilyQuantity(player_inventory, TurretKind::Tek);

    for (const auto& t : turrets) {
        if (t.capacity > 0) has_deficit = true;
    }
    if (!has_deficit) { Send(pc, g_config.already_full); return; }
    if (arb_available <= 0 && shards_available <= 0) { Send(pc, g_config.no_ammo); return; }

    std::vector<TurretRef*> arb_refs;
    std::vector<TurretRef*> shard_refs;
    for (auto& ref : turrets) {
        if (ref.capacity <= 0) continue;
        if (ref.kind == TurretKind::Tek) shard_refs.push_back(&ref);
        else if (ref.kind == TurretKind::Heavy || ref.kind == TurretKind::Auto) arb_refs.push_back(&ref);
    }

    PlanPool(arb_refs, arb_available);
    PlanPool(shard_refs, shards_available);

    int filled_turrets = 0;
    int arb_used = 0;
    int shards_used = 0;

    for (auto& ref : turrets) {
        if (ref.planned <= 0 || !IsValidTurret(ref.turret)) continue;

        UPrimalInventoryComponent* turret_inventory = ref.turret->MyInventoryComponentField();
        if (!turret_inventory) continue;

        const int live_before = FamilyQuantity(turret_inventory, ref.kind);
        const int limit = LimitFor(ref.kind);
        const int live_deficit = std::max(0, limit - live_before);
        const int want = std::min(ref.planned, live_deficit);
        if (want <= 0) continue;

        const int added = TransferFamily(player_inventory, turret_inventory, ref.turret, ref.kind, want);
        if (added > 0) {
            ++filled_turrets;
            if (ref.kind == TurretKind::Tek) shards_used += added;
            else arb_used += added;
            ref.turret->UpdateNumBullets();
        }

        const int after = FamilyQuantity(turret_inventory, ref.kind);
        if (after > limit) {
            const int overflow = after - limit;
            std::vector<RemovedStack> removed_stacks;
            const int removed = RemoveFamilyOverflow(turret_inventory, ref.kind, overflow, &removed_stacks);
            ref.turret->UpdateNumBullets();

            // The overflow ammo was this player's to begin with (it came out of
            // their own inventory a moment ago), so give it straight back
            // instead of deleting it, and correct the reported "used" totals.
            const int refunded = RefundRemovedStacks(player_inventory, ref.kind, removed_stacks);
            if (ref.kind == TurretKind::Tek) shards_used -= std::min(shards_used, refunded);
            else arb_used -= std::min(arb_used, refunded);

            Log::GetLog()->warn(
                "TurretControl v1.7 /fill safety cap: turret='{}' kind={} before={} after={} limit={} overflow_removed={} refunded_to_player={}",
                GetClassFullName(ref.turret), static_cast<int>(ref.kind), live_before, after, limit, removed, refunded);
        }
    }

    if (filled_turrets <= 0) {
        bool has_live_deficit = false;
        bool has_matching_ammo = false;
        for (const auto& ref : turrets) {
            if (!IsValidTurret(ref.turret)) continue;
            UPrimalInventoryComponent* turret_inventory = ref.turret->MyInventoryComponentField();
            if (!turret_inventory) continue;

            const int live_amount = FamilyQuantity(turret_inventory, ref.kind);
            if (live_amount >= LimitFor(ref.kind)) continue;

            has_live_deficit = true;
            if ((ref.kind == TurretKind::Tek && shards_available > 0) ||
                ((ref.kind == TurretKind::Heavy || ref.kind == TurretKind::Auto) && arb_available > 0)) {
                has_matching_ammo = true;
            }
        }

        if (!has_live_deficit) {
            Send(pc, g_config.already_full);
            return;
        }
        if (!has_matching_ammo) {
            Send(pc, g_config.no_ammo);
            return;
        }

        Log::GetLog()->warn(
            "TurretControl v1.7: /fill found {} valid turrets but transferred nothing. ARB={} Shards={}",
            turrets.size(), arb_available, shards_available);
        Send(pc, g_config.fill_failed);
        return;
    }

    std::string message = g_config.fill_success;
    message = ReplaceToken(message, "{0}", std::to_string(filled_turrets));
    message = ReplaceToken(message, "{1}", std::to_string(arb_used));
    message = ReplaceToken(message, "{2}", std::to_string(shards_used));
    Send(pc, message);
    SendFillNotification(pc, message);
}

void TurretsCommandImpl(AShooterPlayerController* pc, FString* message, EChatSendMode::Type) {
    if (!pc || !message || ArkApi::IApiUtils::IsPlayerDead(pc)) return;
    if (!HasPermission(pc)) { Send(pc, g_config.no_permission); return; }
    if (IsPvpBlocked(pc)) { Send(pc, g_config.pvp_blocked); return; }

    std::istringstream command_stream(message->ToString());
    std::string command_word;
    std::string action;
    command_stream >> command_word >> action;
    if (action.empty()) {
        Send(pc, "Usage: " + g_config.turrets_command + " on|off|low|medium|high|players|tames");
        return;
    }
    action = ToLower(action);
    std::vector<TurretRef> turrets = FindTurrets(pc, false);
    if (turrets.empty()) { Send(pc, g_config.no_turrets); return; }

    int changed = 0;
    for (auto& ref : turrets) {
        APrimalStructureTurret* turret = ref.turret;
        if (!IsValidTurret(turret)) continue;

        if (action == "on") {
            turret->SetContainerActive(true);
            ++changed;
        } else if (action == "off") {
            turret->SetContainerActive(false);
            ++changed;
        } else if (action == "low" || action == "medium" || action == "high") {
            const int value = action == "low" ? g_config.range_low : (action == "medium" ? g_config.range_medium : g_config.range_high);
            turret->RangeSettingField() = static_cast<char>(value);
            turret->UpdatedTargeting();
            ++changed;
        } else if (action == "players" || action == "tames") {
            const int value = action == "players" ? g_config.targeting_players_only : g_config.targeting_players_and_tames;
            if (value < 0 || value > 255) {
                Send(pc, g_config.targeting_unconfigured);
                return;
            }
            turret->AISettingField() = static_cast<char>(value);
            turret->UpdatedTargeting();
            ++changed;
        } else {
            Send(pc, "Usage: " + g_config.turrets_command + " on|off|low|medium|high|players|tames");
            return;
        }
    }

    std::string msg = ReplaceToken(g_config.turret_success, "{0}", std::to_string(changed));
    Send(pc, msg);
}

void FillCommand(AShooterPlayerController* pc, FString* message, EChatSendMode::Type mode) noexcept {
    try {
        FillCommandImpl(pc, message, mode);
    } catch (const std::exception& e) {
        Log::GetLog()->error("TurretControl /fill exception: {}", e.what());
        Send(pc, "TurretControl encountered an internal error. Check the server log.");
    } catch (...) {
        Log::GetLog()->error("TurretControl /fill unknown exception");
        Send(pc, "TurretControl encountered an internal error. Check the server log.");
    }
}

void TurretsCommand(AShooterPlayerController* pc, FString* message, EChatSendMode::Type mode) noexcept {
    try {
        TurretsCommandImpl(pc, message, mode);
    } catch (const std::exception& e) {
        Log::GetLog()->error("TurretControl /turrets exception: {}", e.what());
        Send(pc, "TurretControl encountered an internal error. Check the server log.");
    } catch (...) {
        Log::GetLog()->error("TurretControl /turrets unknown exception");
        Send(pc, "TurretControl encountered an internal error. Check the server log.");
    }
}



APrimalStructureTurret* GetTurretOwner(UPrimalInventoryComponent* inventory) {
    if (!inventory) return nullptr;
    AActor* owner = inventory->GetOwner();
    if (!owner || !owner->IsA(APrimalStructureTurret::GetPrivateStaticClass())) return nullptr;
    return static_cast<APrimalStructureTurret*>(owner);
}

bool ClampTurretAddQuantity(UPrimalInventoryComponent* inventory, UPrimalItem* item,
                            int* requested_quantity, bool require_all) {
    if (g_internal_cap_bypass || !g_config.inventory_cap_enabled || !inventory || !item) return true;

    APrimalStructureTurret* turret = GetTurretOwner(inventory);
    if (!turret || !IsValidTurret(turret)) return true;

    const TurretKind kind = DetectTurretKind(turret);
    if (kind == TurretKind::Unsupported || !ItemMatchesKind(item, kind)) return true;

    const int limit = LimitFor(kind);
    const int current = FamilyQuantity(inventory, kind);
    const int remaining = std::max(0, limit - current);
    const int item_quantity = std::max(0, item->GetItemQuantity());
    const int requested = requested_quantity && *requested_quantity > 0
        ? *requested_quantity
        : item_quantity;
    const int allowed = std::min(requested, remaining);

    if (requested_quantity) *requested_quantity = allowed;

    Log::GetLog()->info(
        "TurretControl v1.7 manual cap: turret='{}' item='{}' current={} limit={} requested={} allowed={}",
        GetClassFullName(turret), GetClassFullName(item), current, limit, requested, allowed);

    if (allowed <= 0) return false;
    if (require_all && allowed < requested) return false;
    return true;
}

bool Hook_UPrimalInventoryComponent_AllowAddInventoryItem(
    UPrimalInventoryComponent* inventory, UPrimalItem* item,
    int* requested_quantity, bool only_add_all)
{
    if (!ClampTurretAddQuantity(inventory, item, requested_quantity, only_add_all)) return false;
    return UPrimalInventoryComponent_AllowAddInventoryItem_original(
        inventory, item, requested_quantity, only_add_all);
}

bool Hook_UPrimalInventoryComponent_RemoteInventoryAllowAddItems(
    UPrimalInventoryComponent* inventory, AShooterPlayerController* pc,
    UPrimalItem* item, int* quantity_override, bool requested_by_player)
{
    if (!ClampTurretAddQuantity(inventory, item, quantity_override, false)) return false;
    return UPrimalInventoryComponent_RemoteInventoryAllowAddItems_original(
        inventory, pc, item, quantity_override, requested_by_player);
}

bool Hook_UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity(
    UPrimalInventoryComponent* inventory,
    UPrimalItem* item,
    const int* requested_quantity_in,
    int* requested_quantity_out)
{
    const bool original_allowed =
        UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity_original(
            inventory, item, requested_quantity_in, requested_quantity_out);

    if (g_internal_cap_bypass || !original_allowed || !g_config.inventory_cap_enabled || !inventory || !item)
        return original_allowed;

    APrimalStructureTurret* turret = GetTurretOwner(inventory);
    if (!turret || !IsValidTurret(turret))
        return original_allowed;

    const TurretKind kind = DetectTurretKind(turret);
    if (kind == TurretKind::Unsupported || !ItemMatchesKind(item, kind))
        return original_allowed;

    const int limit = LimitFor(kind);
    const int current = FamilyQuantity(inventory, kind);
    const int remaining = std::max(0, limit - current);

    int original_max = requested_quantity_out ? *requested_quantity_out
        : (requested_quantity_in ? *requested_quantity_in : 0);
    if (original_max < 0) original_max = 0;

    const int allowed = std::min(original_max, remaining);

    if (requested_quantity_out)
        *requested_quantity_out = allowed;

    Log::GetLog()->debug(
        "TurretControl v1.7 inventory cap: turret='{}' item='{}' current={} limit={} requested={} original_allowed={} final_allowed={}",
        GetClassFullName(turret),
        GetClassFullName(item),
        current,
        limit,
        requested_quantity_in ? *requested_quantity_in : original_max,
        original_max,
        allowed);

    // Returning false when nothing may be inserted keeps the source stack intact.
    return allowed > 0;
}

TurretRpcStats& StatsFor(APrimalStructureTurret* turret) {
    UClass* cls = turret ? turret->ClassField() : nullptr;
    TurretRpcStats& stats = g_turret_rpc_stats[cls];
    if (stats.class_name.empty()) stats.class_name = GetClassFullName(cls);
    return stats;
}

void Hook_APrimalStructureTurret_ClientsFireProjectile_Implementation(
    APrimalStructureTurret* turret,
    FVector origin,
    FVector_NetQuantizeNormal shoot_dir)
{
    if (g_config.rpc_diagnostics_enabled && turret) {
        TurretRpcStats& stats = StatsFor(turret);
        ++stats.client_rpc_window;
        ++stats.client_rpc_total;
    }

    // Read-only diagnostics: never suppress or alter the multicast call.
    APrimalStructureTurret_ClientsFireProjectile_Implementation_original(
        turret, origin, shoot_dir);
}

void Hook_APrimalStructureTurret_DoFire(APrimalStructureTurret* turret, int random_seed) {
    if (g_config.rpc_diagnostics_enabled && turret) {
        TurretRpcStats& stats = StatsFor(turret);
        ++stats.do_fire_window;
        ++stats.do_fire_total;
    }
    APrimalStructureTurret_DoFire_original(turret, random_seed);
}

void Hook_APrimalStructureTurret_DoFireProjectile(
    APrimalStructureTurret* turret, FVector origin, FVector shoot_dir)
{
    if (g_config.rpc_diagnostics_enabled && turret) {
        TurretRpcStats& stats = StatsFor(turret);
        ++stats.projectile_window;
        ++stats.projectile_total;
    }
    APrimalStructureTurret_DoFireProjectile_original(turret, origin, shoot_dir);
}

void Hook_APrimalStructureTurret_DealDamage(
    APrimalStructureTurret* turret, FHitResult* impact, FVector* shoot_dir,
    int damage_amount, TSubclassOf<UDamageType> damage_type, float impulse)
{
    if (g_config.rpc_diagnostics_enabled && turret) {
        TurretRpcStats& stats = StatsFor(turret);
        ++stats.damage_window;
        ++stats.damage_total;
    }
    APrimalStructureTurret_DealDamage_original(
        turret, impact, shoot_dir, damage_amount, damage_type, impulse);
}

bool g_inventory_max_hook_installed = false;
bool g_inventory_add_hook_installed = false;
bool g_inventory_remote_hook_installed = false;
bool g_placement_check_hook_installed = false;
bool g_placed_structure_hook_installed = false;
bool g_rpc_diagnostics_hook_installed = false;
bool g_do_fire_diagnostics_hook_installed = false;
bool g_projectile_diagnostics_hook_installed = false;
bool g_damage_diagnostics_hook_installed = false;

void UninstallRpcDiagnosticsHook();

void ApplyRpcDiagnosticsHookState() {
    if (g_config.rpc_diagnostics_enabled && !g_rpc_diagnostics_hook_installed) {
        g_rpc_diagnostics_hook_installed = ArkApi::GetHooks().SetHook(
            "APrimalStructureTurret.ClientsFireProjectile_Implementation",
            &Hook_APrimalStructureTurret_ClientsFireProjectile_Implementation,
            &APrimalStructureTurret_ClientsFireProjectile_Implementation_original);
        if (g_rpc_diagnostics_hook_installed) {
            Log::GetLog()->info(
            "TurretControl v2.4: read-only turret firing diagnostics enabled");
        } else {
            Log::GetLog()->error(
                "TurretControl v2.4: ClientsFireProjectile diagnostics hook installation failed");
        }
    }

    if (g_config.rpc_diagnostics_enabled && !g_do_fire_diagnostics_hook_installed) {
        g_do_fire_diagnostics_hook_installed = ArkApi::GetHooks().SetHook(
            "APrimalStructureTurret.DoFire", &Hook_APrimalStructureTurret_DoFire,
            &APrimalStructureTurret_DoFire_original);
    }
    if (g_config.rpc_diagnostics_enabled && !g_projectile_diagnostics_hook_installed) {
        g_projectile_diagnostics_hook_installed = ArkApi::GetHooks().SetHook(
            "APrimalStructureTurret.DoFireProjectile",
            &Hook_APrimalStructureTurret_DoFireProjectile,
            &APrimalStructureTurret_DoFireProjectile_original);
    }
    if (g_config.rpc_diagnostics_enabled && !g_damage_diagnostics_hook_installed) {
        g_damage_diagnostics_hook_installed = ArkApi::GetHooks().SetHook(
            "APrimalStructureTurret.DealDamage", &Hook_APrimalStructureTurret_DealDamage,
            &APrimalStructureTurret_DealDamage_original);
    }

    if (g_config.rpc_diagnostics_enabled &&
        (!g_do_fire_diagnostics_hook_installed || !g_projectile_diagnostics_hook_installed ||
         !g_damage_diagnostics_hook_installed)) {
        Log::GetLog()->warn(
            "TurretControl v2.4: one or more server-side firing diagnostic hooks failed (DoFire={}, Projectile={}, Damage={})",
            g_do_fire_diagnostics_hook_installed, g_projectile_diagnostics_hook_installed,
            g_damage_diagnostics_hook_installed);
    }

    if (!g_config.rpc_diagnostics_enabled && g_rpc_diagnostics_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "APrimalStructureTurret.ClientsFireProjectile_Implementation",
            &Hook_APrimalStructureTurret_ClientsFireProjectile_Implementation);
        g_rpc_diagnostics_hook_installed = false;
        g_turret_rpc_stats.clear();
    }
    if (!g_config.rpc_diagnostics_enabled) UninstallRpcDiagnosticsHook();
}

void UninstallRpcDiagnosticsHook() {
    if (g_rpc_diagnostics_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "APrimalStructureTurret.ClientsFireProjectile_Implementation",
            &Hook_APrimalStructureTurret_ClientsFireProjectile_Implementation);
        g_rpc_diagnostics_hook_installed = false;
    }
    if (g_do_fire_diagnostics_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "APrimalStructureTurret.DoFire", &Hook_APrimalStructureTurret_DoFire);
        g_do_fire_diagnostics_hook_installed = false;
    }
    if (g_projectile_diagnostics_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "APrimalStructureTurret.DoFireProjectile",
            &Hook_APrimalStructureTurret_DoFireProjectile);
        g_projectile_diagnostics_hook_installed = false;
    }
    if (g_damage_diagnostics_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "APrimalStructureTurret.DealDamage", &Hook_APrimalStructureTurret_DealDamage);
        g_damage_diagnostics_hook_installed = false;
    }
}

void InstallPlacementHooks() {
    if (!g_placement_check_hook_installed) {
        g_placement_check_hook_installed = ArkApi::GetHooks().SetHook(
            "APrimalStructure.IsAllowedToBuild",
            &Hook_APrimalStructure_IsAllowedToBuild,
            &APrimalStructure_IsAllowedToBuild_original);
        if (!g_placement_check_hook_installed) {
            Log::GetLog()->error("TurretControl: PvP placement-check hook installation failed");
        }
    }
    if (!g_placed_structure_hook_installed) {
        g_placed_structure_hook_installed = ArkApi::GetHooks().SetHook(
            "APrimalStructure.PlacedStructure",
            &Hook_APrimalStructure_PlacedStructure,
            &APrimalStructure_PlacedStructure_original);
        if (!g_placed_structure_hook_installed) {
            Log::GetLog()->error("TurretControl: PvP placed-structure hook installation failed");
        }
    }
}

void UninstallPlacementHooks() {
    if (g_placement_check_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "APrimalStructure.IsAllowedToBuild", &Hook_APrimalStructure_IsAllowedToBuild);
        g_placement_check_hook_installed = false;
    }
    if (g_placed_structure_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "APrimalStructure.PlacedStructure", &Hook_APrimalStructure_PlacedStructure);
        g_placed_structure_hook_installed = false;
    }
}

void ApplyInventoryHookState() {
    if (g_config.inventory_cap_enabled && !g_inventory_max_hook_installed) {
        g_inventory_max_hook_installed = ArkApi::GetHooks().SetHook(
            "UPrimalInventoryComponent.AllowAddInventoryItem_MaxQuantity",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity,
            &UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity_original);
        if (!g_inventory_max_hook_installed) {
            Log::GetLog()->error("TurretControl: max-quantity inventory-cap hook installation failed");
        }
    }
    if (g_config.inventory_cap_enabled && !g_inventory_add_hook_installed) {
        g_inventory_add_hook_installed = ArkApi::GetHooks().SetHook(
            "UPrimalInventoryComponent.AllowAddInventoryItem",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem,
            &UPrimalInventoryComponent_AllowAddInventoryItem_original);
        if (!g_inventory_add_hook_installed) {
            Log::GetLog()->error("TurretControl: generic inventory-cap hook installation failed");
        }
    }
    if (g_config.inventory_cap_enabled && !g_inventory_remote_hook_installed) {
        g_inventory_remote_hook_installed = ArkApi::GetHooks().SetHook(
            "UPrimalInventoryComponent.RemoteInventoryAllowAddItems",
            &Hook_UPrimalInventoryComponent_RemoteInventoryAllowAddItems,
            &UPrimalInventoryComponent_RemoteInventoryAllowAddItems_original);
        if (!g_inventory_remote_hook_installed) {
            Log::GetLog()->error("TurretControl: remote inventory-cap hook installation failed");
        }
    }

    if (!g_config.inventory_cap_enabled && g_inventory_max_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "UPrimalInventoryComponent.AllowAddInventoryItem_MaxQuantity",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity);
        g_inventory_max_hook_installed = false;
    }
    if (!g_config.inventory_cap_enabled && g_inventory_add_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "UPrimalInventoryComponent.AllowAddInventoryItem",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem);
        g_inventory_add_hook_installed = false;
    }
    if (!g_config.inventory_cap_enabled && g_inventory_remote_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "UPrimalInventoryComponent.RemoteInventoryAllowAddItems",
            &Hook_UPrimalInventoryComponent_RemoteInventoryAllowAddItems);
        g_inventory_remote_hook_installed = false;
    }
}


std::chrono::steady_clock::time_point g_next_hard_cap_check = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point g_runtime_enable_at{};
bool g_world_ready_seen = false;
bool g_runtime_ready = false;

bool ApplyShotBatching(APrimalStructureTurret* turret) {
    if (!turret || DetectTurretKind(turret) == TurretKind::Unsupported) return false;

    auto state_it = g_turret_batch_states.find(turret);
    if (state_it == g_turret_batch_states.end()) {
        const TurretBatchState original{
            turret->FireIntervalField(), turret->FireDamageAmountField(),
            turret->NumBulletsPerShotField()
        };
        state_it = g_turret_batch_states.emplace(turret, original).first;
    }

    const TurretBatchState& original = state_it->second;
    const int batch = std::clamp(g_config.shots_per_network_event, 2, 5);
    const bool compatible = turret->bUseInstantDamageShooting().Get() &&
        original.fire_interval > 0.0f && original.fire_damage > 0.0f &&
        original.ammo_per_shot > 0;
    const bool enough_ammo = turret->NumBulletsField() >= original.ammo_per_shot * batch;
    const bool enable = g_config.shot_batching_enabled && compatible && enough_ammo;

    const float desired_interval = enable
        ? original.fire_interval * static_cast<float>(batch) : original.fire_interval;
    const float desired_damage = enable
        ? original.fire_damage * static_cast<float>(batch) : original.fire_damage;
    const int desired_ammo = enable
        ? original.ammo_per_shot * batch : original.ammo_per_shot;

    const bool changed = std::abs(turret->FireIntervalField() - desired_interval) > 0.0001f ||
        std::abs(turret->FireDamageAmountField() - desired_damage) > 0.0001f ||
        turret->NumBulletsPerShotField() != desired_ammo;
    turret->FireIntervalField() = desired_interval;
    turret->FireDamageAmountField() = desired_damage;
    turret->NumBulletsPerShotField() = desired_ammo;
    return enable && changed;
}

// Restore only pointers re-discovered through a fresh world octree scan. Raw
// pointers kept in the old state map may refer to destroyed actors and must not
// be dereferenced directly.
void RestoreAllBatchedTurrets() {
    if (g_turret_batch_states.empty()) return;

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return;

    std::vector<FVector> origins;
    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        APlayerController* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        AShooterCharacter* character = pc->GetPlayerCharacter();
        if (character && character->RootComponentField()) {
            origins.push_back(character->RootComponentField()->RelativeLocationField());
        }
    }

    int restored = 0;
    std::unordered_set<APrimalStructureTurret*> visited;
    for (const FVector& origin : origins) {
        TArray<AActor*> actors;
        TSubclassOf<AActor> turret_class(APrimalStructureTurret::GetPrivateStaticClass());
        UVictoryCore::ServerOctreeOverlapActorsClass(
            &actors, world, origin,
            std::max(g_config.hard_cap_scan_radius, g_config.network_scan_radius),
            EServerOctreeGroup::STRUCTURES, turret_class, true);

        for (AActor* actor : actors) {
            if (!actor || !actor->IsA(APrimalStructureTurret::GetPrivateStaticClass())) continue;
            auto* turret = static_cast<APrimalStructureTurret*>(actor);
            if (!visited.insert(turret).second) continue;
            auto it = g_turret_batch_states.find(turret);
            if (it == g_turret_batch_states.end()) continue;
            turret->FireIntervalField() = it->second.fire_interval;
            turret->FireDamageAmountField() = it->second.fire_damage;
            turret->NumBulletsPerShotField() = it->second.ammo_per_shot;
            g_turret_batch_states.erase(it);
            ++restored;
        }
    }

    Log::GetLog()->info(
        "TurretControl v2.4: restored {} tracked turret(s); {} could not be safely re-discovered",
        restored, g_turret_batch_states.size());
}

void LogRpcDiagnostics() {
    if (!g_config.rpc_diagnostics_enabled || !g_rpc_diagnostics_hook_installed) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_rpc_log) return;
    const int seconds = std::max(1, g_config.rpc_log_interval_seconds);
    g_next_rpc_log = now + std::chrono::seconds(seconds);

    std::vector<TurretRpcStats*> active;
    for (auto& entry : g_turret_rpc_stats) {
        if (entry.second.client_rpc_window == 0 && entry.second.do_fire_window == 0 &&
            entry.second.projectile_window == 0 && entry.second.damage_window == 0) continue;
        active.push_back(&entry.second);
    }
    std::sort(active.begin(), active.end(), [](const TurretRpcStats* a, const TurretRpcStats* b) {
        return a->do_fire_window > b->do_fire_window;
    });

    Log::GetLog()->info(
        "TurretControl.RpcStats: window={}s activeClasses={} (zero means the hooked native path was not used)",
        seconds, active.size());
    for (TurretRpcStats* stats : active) {
        Log::GetLog()->info(
            "TurretControl.RpcStats: class='{}' DoFire={} ({:.2f}/s) Projectile={} Damage={} ClientRPC={} totals=[{},{},{},{}]",
            stats->class_name, stats->do_fire_window,
            static_cast<double>(stats->do_fire_window) / static_cast<double>(seconds),
            stats->projectile_window, stats->damage_window, stats->client_rpc_window,
            stats->do_fire_total, stats->projectile_total, stats->damage_total,
            stats->client_rpc_total);
        stats->client_rpc_window = 0;
        stats->do_fire_window = 0;
        stats->projectile_window = 0;
        stats->damage_window = 0;
    }
}

void HardCapTimer() {
    if (!g_runtime_ready ||
        (!g_config.hard_cap_enabled && !g_config.disable_client_projectile_effects &&
         !g_config.shot_batching_enabled)) return;

    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_hard_cap_check) return;
    g_next_hard_cap_check = now + std::chrono::seconds(std::max(1, g_config.hard_cap_interval_seconds));

    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) return;

    struct RefundTarget {
        AShooterPlayerController* pc = nullptr;
        UPrimalInventoryComponent* inventory = nullptr;
        FVector position{};
    };

    std::unordered_set<APrimalStructureTurret*> checked;
    std::unordered_map<int, std::vector<RefundTarget>> refund_targets;
    std::vector<FVector> scan_origins;
    int disabled_client_effects = 0;
    int batched_turrets = 0;

    // Build the owner/refund lookup before scanning any turret. Previously an
    // enemy controller could encounter the turret first, mark it checked and
    // make the sweep delete overflow even with an owner standing nearby.
    const auto& controllers = world->PlayerControllerListField();
    for (TWeakObjectPtr<APlayerController> weak_pc : controllers) {
        auto* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;

        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        AShooterCharacter* character = pc->GetPlayerCharacter();
        if (!character || !character->RootComponentField()) continue;

        const FVector player_pos = character->RootComponentField()->RelativeLocationField();
        scan_origins.push_back(player_pos);
        const int team = ArkApi::GetApiUtils().GetTribeID(pc);
        if (team > 0) {
            if (UPrimalInventoryComponent* inventory = character->MyInventoryComponentField()) {
                refund_targets[team].push_back(RefundTarget{pc, inventory, player_pos});
            }
        }
    }

    for (const FVector& pos : scan_origins) {

        TArray<AActor*> actors;
        TSubclassOf<AActor> turret_class(APrimalStructureTurret::GetPrivateStaticClass());
        UVictoryCore::ServerOctreeOverlapActorsClass(
            &actors, world, pos,
            std::max(g_config.hard_cap_scan_radius, g_config.network_scan_radius),
            EServerOctreeGroup::STRUCTURES, turret_class, true);

        for (AActor* actor : actors) {
            if (!actor || !actor->IsA(APrimalStructureTurret::GetPrivateStaticClass())) continue;
            auto* turret = static_cast<APrimalStructureTurret*>(actor);
            if (!IsValidTurret(turret)) continue;
            if (!checked.insert(turret).second) continue;

            if (g_config.rpc_diagnostics_enabled &&
                g_logged_turret_classes.insert(turret->ClassField()).second) {
                UClass* runtime_class = turret->ClassField();
                UClass* parent_class = runtime_class
                    ? static_cast<UClass*>(runtime_class->SuperStructField()) : nullptr;
                Log::GetLog()->info(
                    "TurretControl.TurretClass: class='{}' parent='{}' ammo='{}' instant={} clientProjectile={} interval={:.4f} damage={:.2f} bulletsPerShot={}",
                    GetClassFullName(runtime_class), GetClassFullName(parent_class),
                    GetClassFullName(turret->AmmoItemTemplateField().uClass),
                    turret->bUseInstantDamageShooting().Get(),
                    turret->bClientFireProjectile().Get(), turret->FireIntervalField(),
                    turret->FireDamageAmountField(), turret->NumBulletsPerShotField());
            }

            if (g_config.disable_client_projectile_effects) {
                auto client_projectile = turret->bClientFireProjectile();
                if (client_projectile.Get()) {
                    client_projectile.Set(false);
                    ++disabled_client_effects;
                }
            }

            if (ApplyShotBatching(turret)) ++batched_turrets;

            if (!g_config.hard_cap_enabled) continue;

            const TurretKind kind = DetectTurretKind(turret);
            if (kind == TurretKind::Unsupported) continue;

            UPrimalInventoryComponent* inventory = turret->MyInventoryComponentField();
            if (!inventory) continue;

            const int limit = LimitFor(kind);
            const int current = FamilyQuantity(inventory, kind);
            if (current <= limit) continue;

            const int turret_team = turret->TargetingTeamField();
            RefundTarget* refund_target = nullptr;
            float nearest_distance = g_config.hard_cap_scan_radius;
            const auto team_it = refund_targets.find(turret_team);
            if (team_it != refund_targets.end()) {
                const FVector turret_pos = turret->RootComponentField()->RelativeLocationField();
                for (auto& candidate : team_it->second) {
                    const float distance = FVector::Distance(turret_pos, candidate.position);
                    if (distance <= nearest_distance) {
                        nearest_distance = distance;
                        refund_target = &candidate;
                    }
                }
            }

            // Never remove overflow unless a same-tribe player is close enough
            // to receive it. This prevents enemy scans and offline enforcement
            // from deleting another tribe's ammunition.
            if (!refund_target) continue;

            const int overflow = current - limit;
            std::vector<RemovedStack> removed_stacks;
            const int removed = RemoveFamilyOverflow(inventory, kind, overflow, &removed_stacks);
            const int refunded = RefundRemovedStacks(
                refund_target->inventory, kind, removed_stacks, inventory);
            turret->UpdateNumBullets();

            if (refunded > 0) {
                Send(refund_target->pc,
                    ReplaceToken(g_config.hard_cap_refund, "{0}", std::to_string(refunded)));
            }

            Log::GetLog()->warn(
                "TurretControl v1.7 safe hard cap: turret='{}' kind={} current={} limit={} overflow={} removed={} refunded={}",
                GetClassFullName(turret), static_cast<int>(kind), current, limit, overflow, removed, refunded);
        }
    }

    if (disabled_client_effects > 0) {
        Log::GetLog()->info(
            "TurretControl v2.0 network guard: disabled per-shot client projectile effects on {} turret(s)",
            disabled_client_effects);
    }
    if (batched_turrets > 0) {
        Log::GetLog()->info(
            "TurretControl v2.4: grouped {} shots into one real firing cycle on {} turret(s)",
            std::clamp(g_config.shots_per_network_event, 2, 5), batched_turrets);
    }
}

// Do not install the global inventory hook or scan structures while ARK is
// restoring the save. Both operations are postponed until a valid game mode
// and game state have existed for StartupDelaySeconds.
void RuntimeTimer() {
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world || !world->AuthorityGameModeField() || !world->GameStateField()) {
        g_world_ready_seen = false;
        g_runtime_ready = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!g_world_ready_seen) {
        g_world_ready_seen = true;
        g_runtime_enable_at = now + std::chrono::seconds(std::max(0, g_config.startup_delay_seconds));
        return;
    }

    if (!g_runtime_ready && now >= g_runtime_enable_at) {
        g_runtime_ready = true;
        ApplyInventoryHookState();
        ApplyRpcDiagnosticsHookState();
        Log::GetLog()->info(
            "TurretControl v2.4 runtime enabled after world startup (InventoryCap={}, HardCap={}, DisableClientProjectileEffects={}, ShotBatching={}, BatchSize={}, RpcDiagnostics={})",
            g_config.inventory_cap_enabled, g_config.hard_cap_enabled,
            g_config.disable_client_projectile_effects,
            g_config.shot_batching_enabled, g_config.shots_per_network_event,
            g_config.rpc_diagnostics_enabled);
    }

    if (g_runtime_ready) {
        HardCapTimer();
        LogRpcDiagnostics();
    }
}


UClass* LoadTurretStructureClass(const std::string& path) {
    if (path.empty()) return nullptr;
    FString fpath(path.c_str());
    UClass* cls = UVictoryCore::BPLoadClass(&fpath);
    if (!cls) {
        Log::GetLog()->warn("TurretControl: custom turret class did not load: {}", path);
        return nullptr;
    }
    UObject* cdo = cls->GetDefaultObject(true);
    if (!cdo || !cdo->IsA(APrimalStructureTurret::GetPrivateStaticClass())) {
        Log::GetLog()->warn("TurretControl: ignored custom class because it is not an APrimalStructureTurret structure class: {}", path);
        return nullptr;
    }
    return cls;
}

void LoadCustomClasses() {
    g_custom_heavy.clear(); g_custom_tek.clear(); g_custom_auto.clear();
    for (const auto& p : g_config.custom_heavy_classes) if (UClass* c = LoadTurretStructureClass(p)) g_custom_heavy.push_back(c);
    for (const auto& p : g_config.custom_tek_classes) if (UClass* c = LoadTurretStructureClass(p)) g_custom_tek.push_back(c);
    for (const auto& p : g_config.custom_auto_classes) if (UClass* c = LoadTurretStructureClass(p)) g_custom_auto.push_back(c);
}

Config ParseConfig(const minijson::Value& root) {
    Config c;
    c.fill_command = minijson::str(root, "General", "FillCommand", c.fill_command);
    c.turrets_command = minijson::str(root, "General", "TurretsCommand", c.turrets_command);
    c.fill_radius = minijson::number(root, "General", "FillRadius", c.fill_radius);
    c.heavy_ammo_limit = minijson::integer(root, "General", "HeavyAmmoLimit", c.heavy_ammo_limit);
    c.tek_ammo_limit = minijson::integer(root, "General", "TekAmmoLimit", c.tek_ammo_limit);
    c.auto_ammo_limit = minijson::integer(root, "General", "AutoTurretAmmoLimit", c.auto_ammo_limit);
    c.require_same_tribe = minijson::boolean(root, "General", "RequireSameTribe", c.require_same_tribe);
    c.allow_during_pvp_cooldown = minijson::boolean(root, "General", "AllowDuringPvpCooldown", c.allow_during_pvp_cooldown);
    c.show_messages = minijson::boolean(root, "General", "ShowMessages", c.show_messages);
    c.inventory_cap_enabled = minijson::boolean(root, "General", "InventoryCapEnabled", c.inventory_cap_enabled);
    c.hard_cap_enabled = minijson::boolean(root, "General", "HardCapEnabled", c.hard_cap_enabled);
    c.hard_cap_interval_seconds = minijson::integer(root, "General", "HardCapIntervalSeconds", c.hard_cap_interval_seconds);
    c.hard_cap_scan_radius = minijson::number(root, "General", "HardCapScanRadius", c.hard_cap_scan_radius);
    c.startup_delay_seconds = minijson::integer(root, "General", "StartupDelaySeconds", c.startup_delay_seconds);
    c.disable_client_projectile_effects = minijson::boolean(
        root, "NetworkOptimization", "DisableClientProjectileEffects",
        c.disable_client_projectile_effects);
    c.network_scan_radius = minijson::number(
        root, "NetworkOptimization", "ScanRadius", c.network_scan_radius);
    c.shot_batching_enabled = minijson::boolean(
        root, "NetworkOptimization", "ShotBatchingEnabled", c.shot_batching_enabled);
    c.shots_per_network_event = minijson::integer(
        root, "NetworkOptimization", "ShotsPerNetworkEvent", c.shots_per_network_event);
    c.rpc_diagnostics_enabled = minijson::boolean(
        root, "NetworkOptimization", "RpcDiagnosticsEnabled", c.rpc_diagnostics_enabled);
    c.rpc_log_interval_seconds = minijson::integer(
        root, "NetworkOptimization", "RpcLogIntervalSeconds", c.rpc_log_interval_seconds);

    c.pvp_placement_cooldown_enabled = minijson::boolean(
        root, "PvpPlacementCooldown", "Enabled", c.pvp_placement_cooldown_enabled);
    c.pvp_placement_cooldown_seconds = minijson::number(
        root, "PvpPlacementCooldown", "Seconds", c.pvp_placement_cooldown_seconds);
    const auto placement_tokens = minijson::strings(root, "PvpPlacementCooldown", "ClassNameTokens");
    if (!placement_tokens.empty()) c.pvp_placement_class_tokens = placement_tokens;

    c.use_permissions = minijson::boolean(root, "Permissions", "UsePermissions", c.use_permissions);
    c.permission = minijson::str(root, "Permissions", "DefaultPermission", c.permission);

    c.fill_radius_hologram_enabled = minijson::boolean(root, "FillRadiusHologram", "Enabled", c.fill_radius_hologram_enabled);
    c.fill_radius_native_sphere = minijson::boolean(root, "FillRadiusHologram", "NativeDebugSphere", c.fill_radius_native_sphere);
    c.fill_radius_sphere_duration = minijson::number(root, "FillRadiusHologram", "DurationSeconds", c.fill_radius_sphere_duration);
    c.fill_radius_sphere_segments = minijson::integer(root, "FillRadiusHologram", "Segments", c.fill_radius_sphere_segments);
    c.fill_radius_hologram_blueprint = minijson::str(root, "FillRadiusHologram", "BlueprintPath", c.fill_radius_hologram_blueprint);
    c.fill_radius_hologram_spawn_distance = minijson::number(root, "FillRadiusHologram", "SpawnDistance", c.fill_radius_hologram_spawn_distance);
    c.fill_radius_hologram_spawn_y_offset = minijson::number(root, "FillRadiusHologram", "SpawnYOffset", c.fill_radius_hologram_spawn_y_offset);
    c.fill_radius_hologram_spawn_z_offset = minijson::number(root, "FillRadiusHologram", "SpawnZOffset", c.fill_radius_hologram_spawn_z_offset);

    c.fill_notification_enabled = minijson::boolean(root, "FillNotification", "Enabled", c.fill_notification_enabled);
    c.fill_notification_scale = minijson::number(root, "FillNotification", "Scale", c.fill_notification_scale);
    c.fill_notification_time = minijson::number(root, "FillNotification", "DisplayTime", c.fill_notification_time);

    c.vanilla_heavy = minijson::boolean(root, "Turrets", "VanillaHeavy", c.vanilla_heavy);
    c.vanilla_tek = minijson::boolean(root, "Turrets", "VanillaTek", c.vanilla_tek);
    c.vanilla_auto = minijson::boolean(root, "Turrets", "VanillaAuto", c.vanilla_auto);
    c.custom_heavy_classes = minijson::strings(root, "Turrets", "CustomHeavyClasses");
    c.custom_tek_classes = minijson::strings(root, "Turrets", "CustomTekClasses");
    c.custom_auto_classes = minijson::strings(root, "Turrets", "CustomAutoClasses");

    c.range_low = minijson::integer(root, "RangeValues", "Low", c.range_low);
    c.range_medium = minijson::integer(root, "RangeValues", "Medium", c.range_medium);
    c.range_high = minijson::integer(root, "RangeValues", "High", c.range_high);
    c.targeting_players_only = minijson::integer(root, "TargetingValues", "PlayersOnly", c.targeting_players_only);
    c.targeting_players_and_tames = minijson::integer(root, "TargetingValues", "PlayersAndTames", c.targeting_players_and_tames);

    c.sender = minijson::str(root, "Messages", "Sender", c.sender);
    c.no_permission = minijson::str(root, "Messages", "NoPermission", c.no_permission);
    c.no_turrets = minijson::str(root, "Messages", "NoTurrets", c.no_turrets);
    c.no_ammo = minijson::str(root, "Messages", "NoAmmo", c.no_ammo);
    c.already_full = minijson::str(root, "Messages", "AlreadyFull", c.already_full);
    c.fill_success = minijson::str(root, "Messages", "FillSuccess", c.fill_success);
    c.fill_failed = minijson::str(root, "Messages", "FillFailed", c.fill_failed);
    c.turret_success = minijson::str(root, "Messages", "TurretSuccess", c.turret_success);
    c.targeting_unconfigured = minijson::str(root, "Messages", "TargetingUnconfigured", c.targeting_unconfigured);
    c.pvp_blocked = minijson::str(root, "Messages", "PvpBlocked", c.pvp_blocked);
    c.pvp_placement_blocked = minijson::str(root, "Messages", "PvpPlacementBlocked", c.pvp_placement_blocked);
    c.reload_ok = minijson::str(root, "Messages", "ReloadOk", c.reload_ok);
    c.reload_failed = minijson::str(root, "Messages", "ReloadFailed", c.reload_failed);
    c.hard_cap_refund = minijson::str(root, "Messages", "HardCapRefund", c.hard_cap_refund);

    c.fill_radius = std::max(100.0f, c.fill_radius);
    c.heavy_ammo_limit = std::max(0, c.heavy_ammo_limit);
    c.tek_ammo_limit = std::max(0, c.tek_ammo_limit);
    c.auto_ammo_limit = std::max(0, c.auto_ammo_limit);
    c.hard_cap_interval_seconds = std::max(1, c.hard_cap_interval_seconds);
    c.hard_cap_scan_radius = std::max(1000.0f, c.hard_cap_scan_radius);
    c.startup_delay_seconds = std::max(0, c.startup_delay_seconds);
    c.network_scan_radius = std::max(1000.0f, c.network_scan_radius);
    c.shots_per_network_event = std::clamp(c.shots_per_network_event, 2, 5);
    c.rpc_log_interval_seconds = std::max(1, c.rpc_log_interval_seconds);
    c.pvp_placement_cooldown_seconds = std::max(0.0f, c.pvp_placement_cooldown_seconds);
    c.fill_notification_scale = std::max(0.1f, c.fill_notification_scale);
    c.fill_notification_time = std::max(0.1f, c.fill_notification_time);
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

#if !(defined(TURRETCONTROL_WITH_PERMISSIONS) && TURRETCONTROL_WITH_PERMISSIONS)
    if (g_config.use_permissions) {
        Log::GetLog()->warn("TurretControl: UsePermissions=true but this DLL was built without Permissions support; commands will fail closed.");
    }
#endif
}

void RegisterChatCommands();

void UnregisterChatCommands() {
    if (!g_registered_fill_command.empty()) {
        ArkApi::GetCommands().RemoveChatCommand(F(g_registered_fill_command));
        g_registered_fill_command.clear();
    }
    if (!g_registered_turrets_command.empty()) {
        ArkApi::GetCommands().RemoveChatCommand(F(g_registered_turrets_command));
        g_registered_turrets_command.clear();
    }
}

void RegisterChatCommands() {
    if (!g_registered_fill_command.empty() || !g_registered_turrets_command.empty()) {
        UnregisterChatCommands();
    }
    g_registered_fill_command = g_config.fill_command;
    g_registered_turrets_command = g_config.turrets_command;
    ArkApi::GetCommands().AddChatCommand(F(g_registered_fill_command), &FillCommand);
    ArkApi::GetCommands().AddChatCommand(F(g_registered_turrets_command), &TurretsCommand);
}

void ReloadCommand(APlayerController* player_controller, FString*, bool) {
    auto* pc = player_controller && player_controller->IsA(AShooterPlayerController::GetPrivateStaticClass())
        ? static_cast<AShooterPlayerController*>(player_controller) : nullptr;
    try {
        const std::string old_fill = g_registered_fill_command;
        const std::string old_turrets = g_registered_turrets_command;
        RestoreAllBatchedTurrets();
        g_turret_batch_states.clear();
        ReadConfig();
        LoadCustomClasses();
        if (g_runtime_ready) {
            ApplyInventoryHookState();
            ApplyRpcDiagnosticsHookState();
        }
        if (g_config.fill_command != old_fill || g_config.turrets_command != old_turrets) {
            UnregisterChatCommands();
            RegisterChatCommands();
        }
        if (pc) ArkApi::GetApiUtils().SendServerMessage(pc, FColorList::Green, g_config.reload_ok.c_str());
        Log::GetLog()->info("TurretControl config reloaded");
    } catch (const std::exception& e) {
        if (pc) ArkApi::GetApiUtils().SendServerMessage(pc, FColorList::Red, e.what());
        Log::GetLog()->error("TurretControl reload failed: {}", e.what());
    }
}

// Read-only server/RCON command. It scans around every online character and
// logs runtime class information and the exact live turret fields.
void DumpTurretsCommand(APlayerController*, FString*, bool) {
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!world) {
        Log::GetLog()->warn("TurretControl.DumpTurrets: world is not ready");
        return;
    }

    std::vector<FVector> origins;
    for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField()) {
        APlayerController* base_pc = weak_pc.Get();
        if (!base_pc || !base_pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) continue;
        auto* pc = static_cast<AShooterPlayerController*>(base_pc);
        AShooterCharacter* character = pc->GetPlayerCharacter();
        if (character && character->RootComponentField()) {
            origins.push_back(character->RootComponentField()->RelativeLocationField());
        }
    }

    if (origins.empty()) {
        Log::GetLog()->warn(
            "TurretControl.DumpTurrets: no online character available as a scan origin");
        return;
    }

    std::unordered_set<APrimalStructureTurret*> visited;
    int dumped = 0;
    for (const FVector& origin : origins) {
        TArray<AActor*> actors;
        TSubclassOf<AActor> turret_class(APrimalStructureTurret::GetPrivateStaticClass());
        UVictoryCore::ServerOctreeOverlapActorsClass(
            &actors, world, origin, g_config.network_scan_radius,
            EServerOctreeGroup::STRUCTURES, turret_class, true);

        for (AActor* actor : actors) {
            if (!actor || !actor->IsA(APrimalStructureTurret::GetPrivateStaticClass())) continue;
            auto* turret = static_cast<APrimalStructureTurret*>(actor);
            if (!visited.insert(turret).second || !IsValidTurret(turret)) continue;

            UClass* runtime_class = turret->ClassField();
            UClass* parent_class = runtime_class
                ? static_cast<UClass*>(runtime_class->SuperStructField()) : nullptr;
            const TurretKind kind = DetectTurretKind(turret);
            const auto state_it = g_turret_batch_states.find(turret);
            const bool tracked = state_it != g_turret_batch_states.end();

            Log::GetLog()->info(
                "TurretControl.DumpTurrets: class='{}' parent='{}' ammo='{}' kind={} "
                "instant={} clientProjectile={} interval={:.4f} damage={:.2f} "
                "bulletsPerShot={} ammoCount={} trackedOldBatch={}",
                GetClassFullName(runtime_class), GetClassFullName(parent_class),
                GetClassFullName(turret->AmmoItemTemplateField().uClass),
                static_cast<int>(kind), turret->bUseInstantDamageShooting().Get(),
                turret->bClientFireProjectile().Get(), turret->FireIntervalField(),
                turret->FireDamageAmountField(), turret->NumBulletsPerShotField(),
                turret->NumBulletsField(), tracked);
            ++dumped;
        }
    }

    Log::GetLog()->info(
        "TurretControl.DumpTurrets: dumped {} turret(s) within {:.0f} uu of {} online scan origin(s)",
        dumped, g_config.network_scan_radius, origins.size());
}

void Load() {
    Log::Get().Init(kPluginName);
    ReadConfig();
    LoadCustomClasses();
    RegisterChatCommands();
    g_next_hard_cap_check = std::chrono::steady_clock::now();
    g_world_ready_seen = false;
    g_runtime_ready = false;
    g_inventory_max_hook_installed = false;
    g_inventory_add_hook_installed = false;
    g_inventory_remote_hook_installed = false;
    g_placement_check_hook_installed = false;
    g_placed_structure_hook_installed = false;
    g_rpc_diagnostics_hook_installed = false;
    g_do_fire_diagnostics_hook_installed = false;
    g_projectile_diagnostics_hook_installed = false;
    g_damage_diagnostics_hook_installed = false;
    g_last_pvp_defense_placement.clear();
    g_turret_batch_states.clear();
    g_turret_rpc_stats.clear();
    g_logged_turret_classes.clear();
    g_next_rpc_log = std::chrono::steady_clock::now();
    InstallPlacementHooks();
    ArkApi::GetCommands().AddOnTimerCallback("TurretControl.Runtime", &RuntimeTimer);
    ArkApi::GetCommands().AddConsoleCommand("TurretControl.Reload", &ReloadCommand);
    ArkApi::GetCommands().AddConsoleCommand("TurretControl.DumpTurrets", &DumpTurretsCommand);
    Log::GetLog()->info("Loaded plugin - TurretControl v2.4 MeasuredShotGrouping");
}

void Unload() {
    RestoreAllBatchedTurrets();
    UnregisterChatCommands();
    UninstallPlacementHooks();
    UninstallRpcDiagnosticsHook();
    if (g_inventory_max_hook_installed) {
        ArkApi::GetHooks().DisableHook("UPrimalInventoryComponent.AllowAddInventoryItem_MaxQuantity",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity);
        g_inventory_max_hook_installed = false;
    }
    if (g_inventory_add_hook_installed) {
        ArkApi::GetHooks().DisableHook("UPrimalInventoryComponent.AllowAddInventoryItem",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem);
        g_inventory_add_hook_installed = false;
    }
    if (g_inventory_remote_hook_installed) {
        ArkApi::GetHooks().DisableHook("UPrimalInventoryComponent.RemoteInventoryAllowAddItems",
            &Hook_UPrimalInventoryComponent_RemoteInventoryAllowAddItems);
        g_inventory_remote_hook_installed = false;
    }
    ArkApi::GetCommands().RemoveOnTimerCallback("TurretControl.Runtime");
    ArkApi::GetCommands().RemoveConsoleCommand("TurretControl.Reload");
    ArkApi::GetCommands().RemoveConsoleCommand("TurretControl.DumpTurrets");
    g_pvp_checker = nullptr;
    g_last_pvp_defense_placement.clear();
    g_turret_batch_states.clear();
    g_turret_rpc_stats.clear();
    g_logged_turret_classes.clear();
    g_custom_heavy.clear(); g_custom_tek.clear(); g_custom_auto.clear();
}

} // namespace TurretControl

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try {
        TurretControl::Load();
    } catch (const std::exception& e) {
        Log::Get().Init("TurretControl");
        Log::GetLog()->error("TurretControl failed to initialize: {}", e.what());
    } catch (...) {
        Log::Get().Init("TurretControl");
        Log::GetLog()->error("TurretControl failed to initialize with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try {
        TurretControl::Unload();
    } catch (const std::exception& e) {
        Log::GetLog()->error("TurretControl unload exception: {}", e.what());
    } catch (...) {
        Log::GetLog()->error("TurretControl unload unknown exception");
    }
}

// Optional bridge for a separate PvPCooldowns adapter. No PvPCooldowns symbols are invented or linked here.
extern "C" __declspec(dllexport) void __fastcall TurretControl_SetPvpCooldownChecker(TurretControl::PvpCooldownChecker checker) {
    TurretControl::g_pvp_checker = checker;
}
