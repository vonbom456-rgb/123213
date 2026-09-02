#pragma once
#include <API/ARK/Ark.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "CheckedTransfer.h"

// Shared by the two resource plugins only. No hooks or inventory-capacity writes.
namespace StorageSafety {
inline std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
inline std::string Name(UObject* o) {
    if (!o) return {};
    FString s; o->GetFullName(&s, nullptr); return Lower(s.ToString());
}
inline bool Dedicated(AActor* a) { return a && Name(a->ClassField()).find("dedicated") != std::string::npos; }
inline bool Excluded(AActor* a) {
    // Check ancestry as well: modded turrets may have arbitrary leaf class names.
    for (auto* c = a ? a->ClassField() : nullptr; c; c = static_cast<UClass*>(c->SuperStructField())) {
        const auto n = Name(c);
        if (n.find("turret") != std::string::npos || n.find("itemcache") != std::string::npos) return true;
    }
    return false;
}
inline APrimalStructureItemContainer* Container(AActor* a, int tribe) {
    if (!a || tribe < 50000 || a->IsPendingKillPending() ||
        !a->IsA(APrimalStructureItemContainer::GetPrivateStaticClass()) || Excluded(a)) return nullptr;
    auto* c = static_cast<APrimalStructureItemContainer*>(a);
    if (c->TargetingTeamField() != tribe || !c->RootComponentField() || !c->MyInventoryComponentField()) return nullptr;
    return c;
}
inline float Distance(AActor* a, AActor* b) {
    if (!a || !b || !a->RootComponentField() || !b->RootComponentField()) return std::numeric_limits<float>::max();
    const auto d = a->RootComponentField()->RelativeLocationField() - b->RootComponentField()->RelativeLocationField();
    return d.Size();
}
inline std::vector<APrimalStructureItemContainer*> Nearby(AActor* center, int tribe, float radius) {
    std::vector<APrimalStructureItemContainer*> out;
    auto* world = ArkApi::GetApiUtils().GetWorld();
    if (!world || !center || !center->RootComponentField() || tribe < 50000) return out;
    TArray<AActor*> actors;
    UVictoryCore::ServerOctreeOverlapActorsClass(&actors, world,
        center->RootComponentField()->RelativeLocationField(), radius, EServerOctreeGroup::STRUCTURES,
        TSubclassOf<AActor>(APrimalStructureItemContainer::GetPrivateStaticClass()), true);
    for (auto* a : actors) if (auto* c = Container(a, tribe)) {
        if (Distance(center, c) <= radius) out.push_back(c);
        if (out.size() >= 256) break; // Bound work per request / tribe cycle.
    }
    std::sort(out.begin(), out.end(), [center](auto* a, auto* b) { return Distance(center, a) < Distance(center, b); });
    return out;
}
inline APrimalStructureItemContainer* Resolve(unsigned int id, int tribe) {
    auto* world = ArkApi::GetApiUtils().GetWorld();
    return id && world ? Container(APrimalStructure::GetFromID(world, id), tribe) : nullptr;
}
inline bool Resource(UPrimalItem* i) {
    return i && !i->bIsBlueprint()() && !i->bIsEngram()() && i->bAllowRemovalFromInventory()() &&
        i->MyItemTypeField().GetValue() == EPrimalItemType::Resource && i->GetItemQuantity() > 0;
}
template<typename T> inline bool Property(UObject* o, const char* name, const char* type, T& value) {
    if (!o) return false;
    auto* p = o->FindProperty(FName(name, EFindName::FNAME_Find));
    if (!p || Name(p->ClassField()).find(type) == std::string::npos ||
        p->ElementSizeField() != sizeof(T) || p->Offset_InternalField() <= 0 || p->Offset_InternalField() > 1048576) return false;
    value = *reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(o) + p->Offset_InternalField());
    return true;
}
struct DediState { UClass* resource = nullptr; int count = 0; };
inline bool ReadDedi(UPrimalInventoryComponent* inv, DediState& state) {
    auto* owner = inv ? inv->GetOwner() : nullptr;
    if (!Dedicated(owner)) return false;
    return Property(owner, "SelectedResourceClass", "classproperty", state.resource) && state.resource &&
        Property(owner, "ResourceCount", "intproperty", state.count) && state.count >= 0;
}
inline int Quantity(UPrimalInventoryComponent* inv, UClass* cls) {
    if (!inv || !cls) return 0;
    if (Dedicated(inv->GetOwner())) {
        DediState state;
        return ReadDedi(inv, state) && state.resource == cls ? state.count : 0;
    }
    // Read-only count: do not use bForceCheckForDupes (can mutate the inventory).
    long long total = 0;
    for (auto* item : inv->InventoryItemsField()) if (Resource(item) && item->ClassField() == cls) total += item->GetItemQuantity();
    return static_cast<int>(std::min<long long>(total, INT_MAX));
}
inline std::vector<UClass*> Resources(UPrimalInventoryComponent* inv) {
    std::vector<UClass*> out;
    if (!inv) return out;
    if (Dedicated(inv->GetOwner())) {
        DediState s; if (ReadDedi(inv, s) && s.count > 0) out.push_back(s.resource);
        return out;
    }
    for (auto* i : inv->InventoryItemsField()) if (Resource(i) && std::find(out.begin(), out.end(), i->ClassField()) == out.end()) {
        out.push_back(i->ClassField());
        if (out.size() >= 128) break;
    }
    return out;
}
inline bool Accepts(UPrimalInventoryComponent* inv, UClass* cls) {
    if (!inv || !cls) return false;
    if (!Dedicated(inv->GetOwner())) return true;
    DediState s;
    return ReadDedi(inv, s) && s.resource == cls;
}
inline void Change(UPrimalInventoryComponent* inv, UClass* cls, int delta) {
    if (Dedicated(inv->GetOwner())) {
        if (!Accepts(inv, cls)) throw std::runtime_error("Unsupported/unassigned Dedicated Storage; no generic inventory writes allowed");
        inv->BPIncrementItemTemplateQuantity(TSubclassOf<UPrimalItem>(cls), delta, true, false, true, false, false, false, nullptr, true);
        inv->GetOwner()->ForceNetUpdate(false, true, false);
    } else if (delta < 0) {
        // Let the engine remove items; never iterate raw item pointers after removal callbacks.
        inv->IncrementItemTemplateQuantity(TSubclassOf<UPrimalItem>(cls), delta, true, false, nullptr, nullptr,
            true, false, false, false, true, false, true);
    } else if (delta > 0) {
        TSubclassOf<UPrimalItem> skin; skin.uClass = nullptr;
        UPrimalItem::AddNewItem(TSubclassOf<UPrimalItem>(cls), inv, false, false, 0, true, delta, false, 0, false, skin, 0, false, false);
    }
}
inline int Transfer(UPrimalInventoryComponent* from, UPrimalInventoryComponent* to, UClass* cls, int wanted) {
    if (!from || !to || from == to || wanted <= 0 || !Accepts(from, cls) || !Accepts(to, cls)) return 0;
    auto* prototype = cls->ClassDefaultObjectField();
    if (!prototype || !prototype->IsA(UPrimalItem::GetPrivateStaticClass())) return 0;
    if (!Dedicated(to->GetOwner())) {
        int allowed=std::min(wanted,1000);
        if (!to->AllowAddInventoryItem(static_cast<UPrimalItem*>(prototype),&allowed,false) || allowed<=0) return 0;
        wanted=std::min(wanted,allowed);
    }
    try {
        return CheckedTransfer(wanted,[&]{return Quantity(from,cls);},[&]{return Quantity(to,cls);},
            [&](int d){Change(from,cls,d);},[&](int d){Change(to,cls,d);});
    } catch(...) {
        Log::GetLog()->error("Storage transfer fault: source={} destination={} resource={} source_count={} destination_count={}",
            Name(from->GetOwner()),Name(to->GetOwner()),Name(cls),Quantity(from,cls),Quantity(to,cls));
        throw;
    }
}
}
