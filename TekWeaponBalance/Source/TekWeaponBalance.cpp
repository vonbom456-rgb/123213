#include <API/ARK/Ark.h>
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../DamageAlerts/Source/MiniJson.h"
#pragma comment(lib,"ArkApi.lib")
namespace TekWeaponBalance {
struct WeaponCfg{bool enabled=true;float damage=1.0f;float reduction=0.0f;std::vector<std::string> tokens;};
struct Config{WeaponCfg bow{true,1.25f,0.15f,{"tekbow","weapontekbow"}},launcher{true,1.35f,0.25f,{"tekgrenadelauncher","weaptekgrenadelauncher"}};float max_damage=100000.0f;}g;
std::string Dir(){return ArkApi::Tools::GetCurrentDir()+"/ArkApi/Plugins/TekWeaponBalance";}
std::string Lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return s;}
std::string Full(UObject*o){if(!o)return{};FString s;o->GetFullName(&s,nullptr);return Lower(s.ToString());}
std::vector<std::string> ReadTokens(const minijson::Value&r,const char*section,std::vector<std::string>d){auto values=minijson::strings(r,section,"ClassTokens");if(values.empty())return d;d.clear();for(auto&v:values)d.push_back(Lower(v));return d;}
WeaponCfg ReadWeapon(const minijson::Value&r,const char*s,WeaponCfg d){d.enabled=minijson::boolean(r,s,"Enabled",d.enabled);d.damage=std::clamp(minijson::number(r,s,"DamageMultiplier",d.damage),0.1f,10.0f);d.reduction=std::clamp(minijson::number(r,s,"ServerCooldownReductionSeconds",d.reduction),0.0f,2.0f);d.tokens=ReadTokens(r,s,d.tokens);return d;}
void ReadConfig(){std::ifstream f(Dir()+"/config.json",std::ios::binary);if(!f)throw std::runtime_error("config missing");std::ostringstream ss;ss<<f.rdbuf();auto r=minijson::parse(ss.str());Config c;c.bow=ReadWeapon(r,"TekBow",c.bow);c.launcher=ReadWeapon(r,"TekGrenadeLauncher",c.launcher);c.max_damage=std::clamp(minijson::number(r,"Safety","MaximumFinalDamage",c.max_damage),100.0f,10000000.0f);g=c;}
bool MatchText(const std::string&text,const WeaponCfg&c){if(!c.enabled)return false;for(auto&t:c.tokens)if(!t.empty()&&text.find(t)!=std::string::npos)return true;return false;}
const WeaponCfg* Kind(UObject*o){std::string text;AActor*a=o&&o->IsA(AActor::GetPrivateStaticClass())?static_cast<AActor*>(o):nullptr;for(int i=0;a&&i<5;++i){text+=' '+Full(a);a=a->OwnerField();}if(MatchText(text,g.bow))return&g.bow;if(MatchText(text,g.launcher))return&g.launcher;return nullptr;}
const WeaponCfg* DamageKind(AController*c,AActor*a){if(auto*k=Kind(a))return k;if(c&&c->PawnField()&&c->PawnField()->IsA(AShooterCharacter::GetPrivateStaticClass())){auto*w=static_cast<AShooterCharacter*>(c->PawnField())->CurrentWeaponField();if(auto*k=Kind(w))return k;}return nullptr;}
DECLARE_HOOK(APrimalTargetableActor_AdjustDamage,void,APrimalTargetableActor*,float*,FDamageEvent*,AController*,AActor*);
DECLARE_HOOK(AShooterWeapon_HandleFiring,void,AShooterWeapon*,bool);
void DamageHook(APrimalTargetableActor*t,float*d,FDamageEvent*e,AController*c,AActor*a){APrimalTargetableActor_AdjustDamage_original(t,d,e,c,a);if(!d||*d<=0)return;if(auto*k=DamageKind(c,a))*d=std::min(g.max_damage,*d*k->damage);}
void FireHook(AShooterWeapon*w,bool client){if(w)if(auto*k=Kind(w);k&&k->reduction>0.0f)w->LastFireTimeField()-=static_cast<long double>(k->reduction);AShooterWeapon_HandleFiring_original(w,client);}
void Reload(APlayerController*,FString*,bool){try{ReadConfig();Log::GetLog()->info("TekWeaponBalance reloaded");}catch(const std::exception&e){Log::GetLog()->error("reload: {}",e.what());}}
}
void Load(){Log::Get().Init("TekWeaponBalance");TekWeaponBalance::ReadConfig();ArkApi::GetHooks().SetHook("APrimalTargetableActor.AdjustDamage",reinterpret_cast<LPVOID>(&TekWeaponBalance::DamageHook),&TekWeaponBalance::APrimalTargetableActor_AdjustDamage_original);ArkApi::GetHooks().SetHook("AShooterWeapon.HandleFiring",reinterpret_cast<LPVOID>(&TekWeaponBalance::FireHook),&TekWeaponBalance::AShooterWeapon_HandleFiring_original);ArkApi::GetCommands().AddConsoleCommand("TekWeaponBalance.Reload",&TekWeaponBalance::Reload);Log::GetLog()->info("Loaded TekWeaponBalance v1.0");}
void Unload(){ArkApi::GetCommands().RemoveConsoleCommand("TekWeaponBalance.Reload");ArkApi::GetHooks().DisableHook("AShooterWeapon.HandleFiring",reinterpret_cast<LPVOID>(&TekWeaponBalance::FireHook));ArkApi::GetHooks().DisableHook("APrimalTargetableActor.AdjustDamage",reinterpret_cast<LPVOID>(&TekWeaponBalance::DamageHook));}
extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept{try{Load();}catch(...){}}
extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept{try{Unload();}catch(...){}}
