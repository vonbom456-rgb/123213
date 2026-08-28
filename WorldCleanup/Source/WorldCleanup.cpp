#include <API/ARK/Ark.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib, "ArkApi.lib")
namespace WorldCleanup {
struct Config { bool enabled=true; int interval=60; int dropped_grace=30; int cache_grace=60; int corpse_grace=120; bool remove_corpses=true; int max_per_scan=250; };
Config g_config;
std::chrono::steady_clock::time_point g_next{};
std::unordered_map<AActor*, std::chrono::steady_clock::time_point> g_first_empty;
std::string PluginDir(){ return ArkApi::Tools::GetCurrentDir()+"/ArkApi/Plugins/WorldCleanup"; }
std::string Lower(std::string v){ std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));}); return v; }
std::string FullName(UObject* o){ if(!o)return{}; FString s; o->GetFullName(&s,nullptr); return s.ToString(); }
void ReadConfig(){
 std::ifstream f(PluginDir()+"/config.json",std::ios::binary); if(!f.is_open())throw std::runtime_error("cannot open config"); std::ostringstream s;s<<f.rdbuf();auto r=minijson::parse(s.str()); Config v;
 v.enabled=minijson::boolean(r,"Cleanup","Enabled",v.enabled);v.interval=minijson::integer(r,"Cleanup","ScanIntervalSeconds",v.interval);v.dropped_grace=minijson::integer(r,"Cleanup","EmptyDroppedItemGraceSeconds",v.dropped_grace);v.cache_grace=minijson::integer(r,"Cleanup","EmptyDeathCacheGraceSeconds",v.cache_grace);v.corpse_grace=minijson::integer(r,"Cleanup","EmptyCorpseGraceSeconds",v.corpse_grace);v.remove_corpses=minijson::boolean(r,"Cleanup","RemoveEmptyCorpses",v.remove_corpses);v.max_per_scan=minijson::integer(r,"Cleanup","MaxDeletesPerScan",v.max_per_scan);
 v.interval=std::clamp(v.interval,10,600);v.dropped_grace=std::max(10,v.dropped_grace);v.cache_grace=std::max(30,v.cache_grace);v.corpse_grace=std::max(60,v.corpse_grace);v.max_per_scan=std::clamp(v.max_per_scan,1,2000);g_config=v;
}
bool EmptyInventory(UPrimalInventoryComponent* inv){ if(!inv)return true; for(UPrimalItem* item:inv->InventoryItemsField()){if(item&&!item->bIsEngram()()&&item->GetItemQuantity()>0)return false;}return true; }
bool Ready(AActor* actor,int grace,const std::chrono::steady_clock::time_point now){auto it=g_first_empty.find(actor);if(it==g_first_empty.end()){g_first_empty.emplace(actor,now);return false;}return now-it->second>=std::chrono::seconds(grace);}
void Scan(){
 const auto now=std::chrono::steady_clock::now();if(now<g_next)return;g_next=now+std::chrono::seconds(g_config.interval);if(!g_config.enabled)return;UWorld* world=ArkApi::GetApiUtils().GetWorld();if(!world)return;
 int removed=0;std::unordered_set<AActor*> seen;
 TArray<AActor*> drops;UGameplayStatics::GetAllActorsOfClass(world,TSubclassOf<AActor>(ADroppedItem::StaticClass()),&drops);
 for(AActor* actor:drops){if(removed>=g_config.max_per_scan)break;if(!actor||!actor->IsA(ADroppedItem::StaticClass()))continue;seen.insert(actor);auto* drop=static_cast<ADroppedItem*>(actor);UPrimalItem* item=drop->MyItemField();if(item&&item->GetItemQuantity()>0){g_first_empty.erase(actor);continue;}if(Ready(actor,g_config.dropped_grace,now)&&actor->Destroy(true,true)){g_first_empty.erase(actor);++removed;}}
 TArray<AActor*> containers;UGameplayStatics::GetAllActorsOfClass(world,TSubclassOf<AActor>(APrimalStructureItemContainer::GetPrivateStaticClass()),&containers);
 for(AActor* actor:containers){if(removed>=g_config.max_per_scan)break;if(!actor||!actor->IsA(APrimalStructureItemContainer::GetPrivateStaticClass()))continue;auto* c=static_cast<APrimalStructureItemContainer*>(actor);const std::string name=Lower(FullName(c));const bool death=c->bUseDeathCacheCharacterID()()||name.find("deathcache")!=std::string::npos||name.find("deathitemcache")!=std::string::npos;if(!death)continue;seen.insert(actor);if(!EmptyInventory(c->MyInventoryComponentField())){g_first_empty.erase(actor);continue;}if(Ready(actor,g_config.cache_grace,now)&&actor->Destroy(true,true)){g_first_empty.erase(actor);++removed;}}
 if(g_config.remove_corpses&&removed<g_config.max_per_scan){TArray<AActor*> chars;UGameplayStatics::GetAllActorsOfClass(world,TSubclassOf<AActor>(APrimalCharacter::GetPrivateStaticClass()),&chars);for(AActor* actor:chars){if(removed>=g_config.max_per_scan)break;if(!actor||!actor->IsA(APrimalCharacter::GetPrivateStaticClass()))continue;auto* ch=static_cast<APrimalCharacter*>(actor);if(!ch->IsDead())continue;seen.insert(actor);if(!EmptyInventory(ch->MyInventoryComponentField())){g_first_empty.erase(actor);continue;}if(Ready(actor,g_config.corpse_grace,now)&&actor->Destroy(true,true)){g_first_empty.erase(actor);++removed;}}}
 for(auto it=g_first_empty.begin();it!=g_first_empty.end();){if(seen.find(it->first)==seen.end())it=g_first_empty.erase(it);else ++it;}if(removed>0)Log::GetLog()->info("WorldCleanup: safely removed {} empty object(s)",removed);
}
void Reload(APlayerController*,FString*,bool){try{ReadConfig();}catch(const std::exception&e){Log::GetLog()->error("WorldCleanup reload: {}",e.what());}}
}
void Load(){Log::Get().Init("WorldCleanup");WorldCleanup::ReadConfig();ArkApi::GetCommands().AddOnTimerCallback("WorldCleanup.Scan",&WorldCleanup::Scan);ArkApi::GetCommands().AddConsoleCommand("WorldCleanup.Reload",&WorldCleanup::Reload);Log::GetLog()->info("Loaded plugin - WorldCleanup v1.0");}
void Unload(){ArkApi::GetCommands().RemoveOnTimerCallback("WorldCleanup.Scan");ArkApi::GetCommands().RemoveConsoleCommand("WorldCleanup.Reload");WorldCleanup::g_first_empty.clear();}
extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept{try{Load();}catch(...){}}
extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept{try{Unload();}catch(...){}}
