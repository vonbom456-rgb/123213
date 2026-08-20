#include <algorithm>
#include <cctype>
#include <chrono>
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
    bool inventory_cap_enabled = false;
    bool hard_cap_enabled = false;
    int hard_cap_interval_seconds = 2;
    float hard_cap_scan_radius = 5000.0f;
    int startup_delay_seconds = 60;

    bool use_permissions = false;
    std::string permission = "TurretControl.Default";

    // Purely cosmetic: spawns an admin-supplied actor (particle/decal/mesh
    // blueprint) at the player when /fill runs, so they can see roughly how
    // far the search radius reaches. Off by default because this plugin has
    // no way to verify a specific game-content blueprint path exists on your
    // server -- that part is game content, not API, so it can't be shipped
    // pre-filled. Point it at any ring/circle-style actor you already have
    // (from a mod, or one you build yourself) and give that actor its own
    // lifespan (e.g. Set Life Span in its Blueprint) so it disappears on its
    // own; this plugin does not track or destroy what it spawns.
    bool fill_radius_hologram_enabled = false;
    std::string fill_radius_hologram_blueprint;
    float fill_radius_hologram_spawn_distance = 0.0f;
    float fill_radius_hologram_spawn_y_offset = 0.0f;
    float fill_radius_hologram_spawn_z_offset = 0.0f;

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
    std::string pvp_blocked = "You cannot use /fill during PvP cooldown.";
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

using PvpCooldownChecker = bool(__fastcall*)(AShooterPlayerController*);
PvpCooldownChecker g_pvp_checker = nullptr;


DECLARE_HOOK(UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity,
    bool,
    UPrimalInventoryComponent*,
    UPrimalItem*,
    const int*,
    int*);

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
                         const std::vector<RemovedStack>& removed_stacks) {
    if (!to_inventory) return 0;
    int refunded_total = 0;
    for (const auto& stack : removed_stacks) {
        if (!stack.item_class || stack.quantity <= 0) continue;
        const int added = AddExact(to_inventory, stack.item_class, kind, stack.quantity);
        refunded_total += added;
        if (added < stack.quantity) {
            Log::GetLog()->warn(
                "TurretControl overflow refund partial: class='{}' expected={} refunded={}",
                GetClassFullName(stack.item_class), stack.quantity, added);
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
                    "TurretControl v1.3: refund failed. class='{}' expected={} refunded={}",
                    GetClassFullName(actual_class), refund, refunded);
            }
        }

        const int turret_after = FamilyQuantity(to, kind);
        Log::GetLog()->info(
            "TurretControl v1.3 fill: turret='{}' template='{}' player_class='{}' player_before={} turret_before={} requested={} removed={} added={} turret_after={}",
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

// Purely cosmetic. Uses the verified ArkApi 3.56 AShooterPlayerController::SpawnActor
// helper to drop an admin-chosen actor at the player so they can see roughly
// where /fill searches. No-op until FillRadiusHologram.BlueprintPath is set
// in config.json -- see the Config struct comment for why this ships empty.
void SpawnFillRadiusHologram(AShooterPlayerController* pc) {
    if (!g_config.fill_radius_hologram_enabled) return;
    if (!pc || g_config.fill_radius_hologram_blueprint.empty()) return;

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
                "TurretControl v1.3 /fill safety cap: turret='{}' kind={} before={} after={} limit={} overflow_removed={} refunded_to_player={}",
                GetClassFullName(ref.turret), static_cast<int>(ref.kind), live_before, after, limit, removed, refunded);
        }
    }

    if (filled_turrets <= 0) {
        Log::GetLog()->warn(
            "TurretControl v1.3: /fill found {} valid turrets but transferred nothing. ARB={} Shards={}",
            turrets.size(), arb_available, shards_available);
        Send(pc, g_config.fill_failed);
        return;
    }

    std::string message = g_config.fill_success;
    message = ReplaceToken(message, "{0}", std::to_string(filled_turrets));
    message = ReplaceToken(message, "{1}", std::to_string(arb_used));
    message = ReplaceToken(message, "{2}", std::to_string(shards_used));
    Send(pc, message);
}

void TurretsCommandImpl(AShooterPlayerController* pc, FString* message, EChatSendMode::Type) {
    if (!pc || !message || ArkApi::IApiUtils::IsPlayerDead(pc)) return;
    if (!HasPermission(pc)) { Send(pc, g_config.no_permission); return; }

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

bool Hook_UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity(
    UPrimalInventoryComponent* inventory,
    UPrimalItem* item,
    const int* requested_quantity_in,
    int* requested_quantity_out)
{
    const bool original_allowed =
        UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity_original(
            inventory, item, requested_quantity_in, requested_quantity_out);

    if (!original_allowed || !g_config.inventory_cap_enabled || !inventory || !item)
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
        "TurretControl v1.3 inventory cap: turret='{}' item='{}' current={} limit={} requested={} original_allowed={} final_allowed={}",
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

bool g_inventory_hook_installed = false;

void ApplyInventoryHookState() {
    if (g_config.inventory_cap_enabled && !g_inventory_hook_installed) {
        g_inventory_hook_installed = ArkApi::GetHooks().SetHook(
            "UPrimalInventoryComponent.AllowAddInventoryItem_MaxQuantity",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity,
            &UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity_original);
        if (!g_inventory_hook_installed) {
            Log::GetLog()->error("TurretControl: inventory-cap hook installation failed; continuing without InventoryCap");
        }
    } else if (!g_config.inventory_cap_enabled && g_inventory_hook_installed) {
        ArkApi::GetHooks().DisableHook(
            "UPrimalInventoryComponent.AllowAddInventoryItem_MaxQuantity",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity);
        g_inventory_hook_installed = false;
    }
}


std::chrono::steady_clock::time_point g_next_hard_cap_check = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point g_runtime_enable_at{};
bool g_world_ready_seen = false;
bool g_runtime_ready = false;

void HardCapTimer() {
    if (!g_runtime_ready || !g_config.hard_cap_enabled) return;

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
            &actors, world, pos, g_config.hard_cap_scan_radius,
            EServerOctreeGroup::STRUCTURES, turret_class, true);

        for (AActor* actor : actors) {
            if (!actor || !actor->IsA(APrimalStructureTurret::GetPrivateStaticClass())) continue;
            auto* turret = static_cast<APrimalStructureTurret*>(actor);
            if (!IsValidTurret(turret)) continue;
            if (!checked.insert(turret).second) continue;

            const TurretKind kind = DetectTurretKind(turret);
            if (kind == TurretKind::Unsupported) continue;

            UPrimalInventoryComponent* inventory = turret->MyInventoryComponentField();
            if (!inventory) continue;

            const int limit = LimitFor(kind);
            const int current = FamilyQuantity(inventory, kind);
            if (current <= limit) continue;

            const int overflow = current - limit;
            std::vector<RemovedStack> removed_stacks;
            const int removed = RemoveFamilyOverflow(inventory, kind, overflow, &removed_stacks);
            turret->UpdateNumBullets();

            int refunded = 0;
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
            if (refund_target) {
                refunded = RefundRemovedStacks(refund_target->inventory, kind, removed_stacks);
                if (refunded > 0) {
                    Send(refund_target->pc,
                        ReplaceToken(g_config.hard_cap_refund, "{0}", std::to_string(refunded)));
                }
            }

            Log::GetLog()->warn(
                "TurretControl v1.3 hard cap: turret='{}' kind={} current={} limit={} overflow={} removed={} refunded={}",
                GetClassFullName(turret), static_cast<int>(kind), current, limit, overflow, removed, refunded);
        }
    }
}

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
        Log::GetLog()->info(
            "TurretControl v1.4 runtime enabled after world startup (InventoryCap={}, HardCap={})",
            g_config.inventory_cap_enabled, g_config.hard_cap_enabled);
    }

    if (g_runtime_ready) HardCapTimer();
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

    c.use_permissions = minijson::boolean(root, "Permissions", "UsePermissions", c.use_permissions);
    c.permission = minijson::str(root, "Permissions", "DefaultPermission", c.permission);

    c.fill_radius_hologram_enabled = minijson::boolean(root, "FillRadiusHologram", "Enabled", c.fill_radius_hologram_enabled);
    c.fill_radius_hologram_blueprint = minijson::str(root, "FillRadiusHologram", "BlueprintPath", c.fill_radius_hologram_blueprint);
    c.fill_radius_hologram_spawn_distance = minijson::number(root, "FillRadiusHologram", "SpawnDistance", c.fill_radius_hologram_spawn_distance);
    c.fill_radius_hologram_spawn_y_offset = minijson::number(root, "FillRadiusHologram", "SpawnYOffset", c.fill_radius_hologram_spawn_y_offset);
    c.fill_radius_hologram_spawn_z_offset = minijson::number(root, "FillRadiusHologram", "SpawnZOffset", c.fill_radius_hologram_spawn_z_offset);

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
        ReadConfig();
        LoadCustomClasses();
        if (g_runtime_ready) ApplyInventoryHookState();
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

void Load() {
    Log::Get().Init(kPluginName);
    ReadConfig();
    LoadCustomClasses();
    RegisterChatCommands();
    g_next_hard_cap_check = std::chrono::steady_clock::now();
    g_world_ready_seen = false;
    g_runtime_ready = false;
    g_inventory_hook_installed = false;
    ArkApi::GetCommands().AddOnTimerCallback("TurretControl.Runtime", &RuntimeTimer);
    ArkApi::GetCommands().AddConsoleCommand("TurretControl.Reload", &ReloadCommand);
    Log::GetLog()->info("Loaded plugin - TurretControl v1.4 SafeStartup");
}

void Unload() {
    UnregisterChatCommands();
    if (g_inventory_hook_installed) {
        ArkApi::GetHooks().DisableHook("UPrimalInventoryComponent.AllowAddInventoryItem_MaxQuantity",
            &Hook_UPrimalInventoryComponent_AllowAddInventoryItem_MaxQuantity);
        g_inventory_hook_installed = false;
    }
    ArkApi::GetCommands().RemoveOnTimerCallback("TurretControl.Runtime");
    ArkApi::GetCommands().RemoveConsoleCommand("TurretControl.Reload");
    g_pvp_checker = nullptr;
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
