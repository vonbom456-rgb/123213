#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib,"ArkApi.lib")
namespace BlueprintSorter {
struct Config{bool enabled=true;std::string command="/bpsort";};Config g_config;
std::string PluginDir(){return ArkApi::Tools::GetCurrentDir()+"/ArkApi/Plugins/BlueprintSorter";}
std::string Lower(std::string v){std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return v;}
std::string FullName(UObject*o){if(!o)return{};FString s;o->GetFullName(&s,nullptr);return s.ToString();}
void Send(AShooterPlayerController*pc,const std::string&m){if(!pc)return;FString s(ArkApi::Tools::Utf8Decode(m).c_str());ArkApi::GetApiUtils().SendServerMessage(pc,FColorList::Green,*s);}
void ReadConfig(){std::ifstream f(PluginDir()+"/config.json",std::ios::binary);if(!f.is_open())throw std::runtime_error("cannot open config");std::ostringstream s;s<<f.rdbuf();auto r=minijson::parse(s.str());Config v;v.enabled=minijson::boolean(r,"Sorter","Enabled",v.enabled);v.command=minijson::str(r,"Sorter","Command",v.command);g_config=v;}
std::string Category(UPrimalItem* item){const std::string n=Lower(FullName(item?item->ClassField():nullptr));if(n.find("saddle")!=std::string::npos)return "Saddles";if(n.find("tek")!=std::string::npos)return "Tek";if(n.find("weapon")!=std::string::npos||n.find("rifle")!=std::string::npos||n.find("bow")!=std::string::npos)return "Weapons";if(n.find("armor")!=std::string::npos||n.find("shirt")!=std::string::npos||n.find("helmet")!=std::string::npos||n.find("boots")!=std::string::npos||n.find("gloves")!=std::string::npos||n.find("pants")!=std::string::npos)return "Armor";if(n.find("pick")!=std::string::npos||n.find("hatchet")!=std::string::npos||n.find("tool")!=std::string::npos)return "Tools";return "Other";}
UPrimalInventoryComponent* Target(AShooterPlayerController*pc){if(!pc)return nullptr;for(TWeakObjectPtr<UPrimalInventoryComponent> weak:pc->RemoteViewingInventoriesField()){auto*inv=weak.Get();if(!inv)continue;AActor*owner=inv->GetOwner();if(owner&&owner->IsA(APrimalStructure::GetPrivateStaticClass())){auto*s=static_cast<APrimalStructure*>(owner);if(s->TargetingTeamField()!=ArkApi::GetApiUtils().GetTribeID(pc))continue;}return inv;}AShooterCharacter*ch=pc->GetPlayerCharacter();return ch?ch->MyInventoryComponentField():nullptr;}
void Command(AShooterPlayerController*pc,FString*,EChatSendMode::Type){if(!g_config.enabled||!pc)return;auto*inv=Target(pc);if(!inv){Send(pc,"Open an inventory first.");return;}int sorted=0;std::unordered_set<std::string>folders;const TArray<UPrimalItem*>items=inv->InventoryItemsField();for(UPrimalItem*item:items){if(!item||!item->bIsBlueprint()()||item->bIsEngram()())continue;const int q=std::clamp(static_cast<int>(static_cast<unsigned char>(item->ItemQualityIndexField())),0,20);const std::string folder="BP Q"+std::to_string(q)+" "+Category(item);if(folders.insert(folder).second)inv->AddCustomFolder(FString(folder.c_str()),0);FString f(folder.c_str());inv->RemoteAddItemToCustomFolder(&f,0,item->ItemIDField());++sorted;}Send(pc,"Sorted "+std::to_string(sorted)+" blueprint(s) into quality/category folders. Nothing was deleted.");}
void Reload(APlayerController*,FString*,bool){try{ReadConfig();}catch(const std::exception&e){Log::GetLog()->error("BlueprintSorter reload: {}",e.what());}}
}
void Load(){Log::Get().Init("BlueprintSorter");BlueprintSorter::ReadConfig();ArkApi::GetCommands().AddChatCommand(FString(BlueprintSorter::g_config.command.c_str()),&BlueprintSorter::Command);ArkApi::GetCommands().AddConsoleCommand("BlueprintSorter.Reload",&BlueprintSorter::Reload);Log::GetLog()->info("Loaded plugin - BlueprintSorter v1.0");}
void Unload(){ArkApi::GetCommands().RemoveChatCommand(FString(BlueprintSorter::g_config.command.c_str()));ArkApi::GetCommands().RemoveConsoleCommand("BlueprintSorter.Reload");}
extern "C" __declspec(dllexport)void __fastcall Plugin_Init()noexcept{try{Load();}catch(...){}}
extern "C" __declspec(dllexport)void __fastcall Plugin_Unload()noexcept{try{Unload();}catch(...){}}
