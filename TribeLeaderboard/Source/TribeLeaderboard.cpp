#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../DamageAlerts/Source/MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace TribeLeaderboard {

struct Range { int min = 1; int max = 1; };
struct Config {
    bool enabled = true; int min_team = 50000; int top_count = 10; int max_per_minute = 30;
    std::string state_file = "state.tsv", top_command = "/top", rank_command = "/rank";
    Range thatch{1,1}, wood{1,2}, stone{2,4}, metal{3,6}, tek{5,10}, turret{6,12}, other{1,3};
};
struct Entry { long long points = 0; std::string name; };
Config g_config;
std::unordered_map<int, Entry> g_scores;
std::unordered_map<unsigned long long, std::vector<std::chrono::steady_clock::time_point>> g_awards;
std::mt19937 g_rng{std::random_device{}()};

std::string Dir() { return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/TribeLeaderboard"; }
std::string Lower(std::string s) { std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));}); return s; }
std::string Full(UObject* o) { if(!o)return{}; FString s; o->GetFullName(&s,nullptr); return Lower(s.ToString()); }
void Send(AShooterPlayerController* pc,const std::string& s,const FLinearColor& c=FColorList::Green){if(pc){FString f(ArkApi::Tools::Utf8Decode(s).c_str());ArkApi::GetApiUtils().SendServerMessage(pc,c,*f);}}

Range ReadRange(const minijson::Value& root,const char* prefix,Range d){
    d.min=minijson::integer(root,"Points",std::string(prefix)+"Min",d.min);
    d.max=minijson::integer(root,"Points",std::string(prefix)+"Max",d.max);
    if(d.min<0)d.min=0; if(d.max<d.min)d.max=d.min; return d;
}
void ReadConfig(){
    std::ifstream f(Dir()+"/config.json",std::ios::binary); if(!f)throw std::runtime_error("config missing");
    std::ostringstream ss;ss<<f.rdbuf();auto r=minijson::parse(ss.str());Config c;
    c.enabled=minijson::boolean(r,"General","Enabled",c.enabled);c.min_team=minijson::integer(r,"General","MinTribeTeamId",c.min_team);
    c.top_count=std::clamp(minijson::integer(r,"General","TopCount",c.top_count),1,25);
    c.max_per_minute=std::clamp(minijson::integer(r,"General","MaxAwardsFromSameVictimPerMinute",c.max_per_minute),1,1000);
    c.state_file=minijson::str(r,"General","StateFile",c.state_file);c.top_command=minijson::str(r,"Commands","Top",c.top_command);c.rank_command=minijson::str(r,"Commands","Rank",c.rank_command);
    c.thatch=ReadRange(r,"Thatch",c.thatch);c.wood=ReadRange(r,"Wood",c.wood);c.stone=ReadRange(r,"Stone",c.stone);c.metal=ReadRange(r,"Metal",c.metal);c.tek=ReadRange(r,"Tek",c.tek);c.turret=ReadRange(r,"Turret",c.turret);c.other=ReadRange(r,"Default",c.other);g_config=c;
}
std::string CleanName(std::string s){std::replace(s.begin(),s.end(),'\t',' ');std::replace(s.begin(),s.end(),'\n',' ');return s;}
std::string TeamName(int team){
    UWorld* world=ArkApi::GetApiUtils().GetWorld();
    if(world) for(TWeakObjectPtr<APlayerController> weak:world->PlayerControllerListField()){
        auto* base=weak.Get();if(!base||!base->IsA(AShooterPlayerController::GetPrivateStaticClass()))continue;auto* pc=static_cast<AShooterPlayerController*>(base);
        if(ArkApi::GetApiUtils().GetTribeID(pc)==team){auto* ch=pc->GetPlayerCharacter();if(ch&&!ch->TribeNameField().IsEmpty())return CleanName(ch->TribeNameField().ToString());}
    }
    auto it=g_scores.find(team);return it!=g_scores.end()&&!it->second.name.empty()?it->second.name:"Tribe "+std::to_string(team);
}
void Save(){std::ofstream f(Dir()+"/"+g_config.state_file,std::ios::trunc);for(auto& p:g_scores)f<<p.first<<'\t'<<p.second.points<<'\t'<<CleanName(p.second.name)<<'\n';}
void LoadState(){g_scores.clear();std::ifstream f(Dir()+"/"+g_config.state_file);std::string line;while(std::getline(f,line)){std::istringstream ss(line);int team;long long points;std::string name;if(ss>>team>>points){std::getline(ss,name);if(!name.empty()&&name[0]=='\t')name.erase(0,1);g_scores[team]={points,name};}}}
struct Attacker{int team=-1;AShooterPlayerController* pc=nullptr;};
Attacker Resolve(AController* c,AActor* a){Attacker x;if(c&&c->IsA(AShooterPlayerController::GetPrivateStaticClass())){x.pc=static_cast<AShooterPlayerController*>(c);x.team=ArkApi::GetApiUtils().GetTribeID(x.pc);}else if(c&&c->PawnField())x.team=c->PawnField()->TargetingTeamField();else if(a)x.team=a->TargetingTeamField();if(x.team<g_config.min_team)x.team=-1;return x;}
Range ClassRange(APrimalStructure* s){const auto n=Full(s);if(n.find("turret")!=std::string::npos)return g_config.turret;if(n.find("tek")!=std::string::npos)return g_config.tek;if(n.find("metal")!=std::string::npos)return g_config.metal;if(n.find("stone")!=std::string::npos)return g_config.stone;if(n.find("wood")!=std::string::npos)return g_config.wood;if(n.find("thatch")!=std::string::npos)return g_config.thatch;return g_config.other;}
bool Allowed(int attacker,int victim){
    const unsigned long long key=(static_cast<unsigned long long>(static_cast<unsigned int>(attacker))<<32)|static_cast<unsigned int>(victim);auto& v=g_awards[key];const auto now=std::chrono::steady_clock::now();
    v.erase(std::remove_if(v.begin(),v.end(),[&](auto t){return now-t>std::chrono::minutes(1);}),v.end());if(static_cast<int>(v.size())>=g_config.max_per_minute)return false;v.push_back(now);return true;
}
DECLARE_HOOK(APrimalStructure_TakeDamage,float,APrimalStructure*,float,FDamageEvent*,AController*,AActor*);
float Hook(APrimalStructure* s,float damage,FDamageEvent* e,AController* c,AActor* a){const int victim=s?s->TargetingTeamField():-1;const bool dead=!s||s->IsDead();const Attacker at=Resolve(c,a);const float out=APrimalStructure_TakeDamage_original(s,damage,e,c,a);if(g_config.enabled&&s&&!dead&&s->IsDead()&&victim>=g_config.min_team&&at.team>=g_config.min_team&&at.team!=victim&&Allowed(at.team,victim)){auto range=ClassRange(s);std::uniform_int_distribution<int>d(range.min,range.max);int pts=d(g_rng);auto& en=g_scores[at.team];en.points+=pts;en.name=TeamName(at.team);Save();if(at.pc)Send(at.pc,"Tribe rating: +"+std::to_string(pts)+" (total "+std::to_string(en.points)+")");}return out;}
void Top(AShooterPlayerController* pc,FString*,EChatSendMode::Type){std::vector<std::pair<int,Entry>> v(g_scores.begin(),g_scores.end());std::sort(v.begin(),v.end(),[](auto&a,auto&b){return a.second.points>b.second.points;});Send(pc,"=== TOP TRIBES ===",FColorList::Yellow);for(int i=0;i<std::min<int>(g_config.top_count,v.size());++i)Send(pc,std::to_string(i+1)+". "+(v[i].second.name.empty()?"Tribe "+std::to_string(v[i].first):v[i].second.name)+" - "+std::to_string(v[i].second.points));}
void Rank(AShooterPlayerController* pc,FString*,EChatSendMode::Type){int team=ArkApi::GetApiUtils().GetTribeID(pc);auto it=g_scores.find(team);Send(pc,"Your tribe rating: "+std::to_string(it==g_scores.end()?0:it->second.points));}
void Reload(APlayerController*,FString*,bool){try{ReadConfig();Log::GetLog()->info("TribeLeaderboard reloaded");}catch(const std::exception&e){Log::GetLog()->error("reload: {}",e.what());}}
void Add(APlayerController*,FString* raw,bool){std::istringstream ss(raw?raw->ToString():"");std::string cmd;int team=0,pts=0;ss>>cmd>>team>>pts;if(team>=g_config.min_team){auto&e=g_scores[team];e.points=std::max<long long>(0,e.points+pts);e.name=TeamName(team);Save();}}
void Reset(APlayerController*,FString*,bool){g_scores.clear();Save();}
}
void Load(){Log::Get().Init("TribeLeaderboard");TribeLeaderboard::ReadConfig();TribeLeaderboard::LoadState();ArkApi::GetHooks().SetHook("APrimalStructure.TakeDamage",reinterpret_cast<LPVOID>(&TribeLeaderboard::Hook),&TribeLeaderboard::APrimalStructure_TakeDamage_original);ArkApi::GetCommands().AddChatCommand(FString(TribeLeaderboard::g_config.top_command.c_str()),&TribeLeaderboard::Top);ArkApi::GetCommands().AddChatCommand(FString(TribeLeaderboard::g_config.rank_command.c_str()),&TribeLeaderboard::Rank);ArkApi::GetCommands().AddConsoleCommand("TribeLeaderboard.Reload",&TribeLeaderboard::Reload);ArkApi::GetCommands().AddConsoleCommand("TribeLeaderboard.Add",&TribeLeaderboard::Add);ArkApi::GetCommands().AddConsoleCommand("TribeLeaderboard.Reset",&TribeLeaderboard::Reset);Log::GetLog()->info("Loaded TribeLeaderboard v1.0");}
void Unload(){TribeLeaderboard::Save();ArkApi::GetHooks().DisableHook("APrimalStructure.TakeDamage",reinterpret_cast<LPVOID>(&TribeLeaderboard::Hook));ArkApi::GetCommands().RemoveChatCommand(FString(TribeLeaderboard::g_config.top_command.c_str()));ArkApi::GetCommands().RemoveChatCommand(FString(TribeLeaderboard::g_config.rank_command.c_str()));ArkApi::GetCommands().RemoveConsoleCommand("TribeLeaderboard.Reload");ArkApi::GetCommands().RemoveConsoleCommand("TribeLeaderboard.Add");ArkApi::GetCommands().RemoveConsoleCommand("TribeLeaderboard.Reset");}
extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept{try{Load();}catch(...){}}
extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept{try{Unload();}catch(...){}}
