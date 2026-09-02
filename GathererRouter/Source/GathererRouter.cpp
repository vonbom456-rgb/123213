#include "../../ResourceLogistics/Source/StorageSafety.h"
#include <API/UE/Math/ColorList.h>
#include "../../DamageAlerts/Source/MiniJson.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <Windows.h>

namespace GathererRouter {
namespace S = StorageSafety;
struct Config {
    bool enabled = true;
    float radius = 6000;
    int interval = 10, max_cycle = 10000;
    std::string state = "routes.tsv", command = "/router";
    std::vector<std::string> sources{"resourcegatherer", "gatherer_", "structure_gatherer"};
} g;
struct Route {
    unsigned int hub = 0;
    bool auto_sources = true, running = false;
    std::unordered_set<unsigned int> sources;
    std::unordered_map<std::string, unsigned int> targets;
};
std::unordered_map<int, Route> routes;
std::unordered_map<int, std::chrono::steady_clock::time_point> cooldowns;
int tick = 0;
bool faulted = false;
std::string registered_command;
std::string Dir() { return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/GathererRouter"; }
void Send(AShooterPlayerController* pc, const std::string& text) {
    if (pc) { FString s(ArkApi::Tools::Utf8Decode(text).c_str()); ArkApi::GetApiUtils().SendServerMessage(pc, FColorList::Yellow, *s); }
}
void ReadConfig() {
    std::ifstream f(Dir() + "/config.json"); if (!f) throw std::runtime_error("config missing");
    std::ostringstream ss; ss << f.rdbuf(); auto root = minijson::parse(ss.str()); Config c;
    c.enabled = minijson::boolean(root, "General", "Enabled", true);
    c.radius = std::clamp(minijson::number(root, "General", "Radius", c.radius), 500.0f, 10000.0f);
    c.interval = std::clamp(minijson::integer(root, "General", "IntervalSeconds", 10), 5, 60);
    c.max_cycle = std::clamp(minijson::integer(root, "General", "MaxItemsPerCycle", 10000), 1, 10000);
    c.state = minijson::str(root, "General", "StateFile", c.state);
    if (c.state.find_first_of("/\\:") != std::string::npos || c.state.find("..") != std::string::npos) throw std::runtime_error("StateFile must be a filename");
    c.command = minijson::str(root, "Commands", "Root", c.command);
    auto a = minijson::strings(root, "Detection", "SourceClassTokens"); if (!a.empty()) c.sources = a;
    for (auto& s : c.sources) s = S::Lower(s);
    if (!registered_command.empty()) c.command = registered_command;
    g = c;
}
void Save() {
    const auto path = Dir() + "/" + g.state;
    std::ofstream f(path + ".tmp", std::ios::trunc); if (!f) throw std::runtime_error("cannot write routes");
    for (const auto& p : routes) {
        f << p.first << '\t' << p.second.hub << '\t' << p.second.auto_sources << '\t';
        for (auto id : p.second.sources) f << id << ',';
        f << '\t'; for (const auto& t : p.second.targets) f << t.first << '=' << t.second << ';';
        f << '\n';
    }
    f.flush(); if (!f) throw std::runtime_error("routes write failed"); f.close();
    if (!MoveFileExA((path+".tmp").c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot commit routes; original file preserved");
}
void LoadState() {
    std::ifstream f(Dir() + "/" + g.state); routes.clear(); std::string line; int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        try {
            std::istringstream in(line); std::vector<std::string> x; std::string z;
            while (std::getline(in, z, '\t')) x.push_back(z);
            if (x.size() < 3) throw std::runtime_error("missing fields");
            int tribe = std::stoi(x[0]); if (tribe < 50000) continue;
            Route r; r.hub = static_cast<unsigned int>(std::stoul(x[1])); r.auto_sources = std::stoi(x[2]) != 0;
            if (x.size() > 3) { std::istringstream a(x[3]); while (std::getline(a, z, ',')) if (!z.empty()) r.sources.insert(static_cast<unsigned int>(std::stoul(z))); }
            if (x.size() > 4) { std::istringstream a(x[4]); while (std::getline(a, z, ';')) {
                auto n = z.find('='); if (n != std::string::npos && n>0) r.targets[S::Lower(z.substr(0,n))] = static_cast<unsigned int>(std::stoul(z.substr(n+1)));
            } }
            routes[tribe] = r; // Always paused on load. Never replay old routes automatically.
            if (routes.size() >= 256) break;
        } catch (const std::exception& e) { Log::GetLog()->warn("Ignored invalid routes line {}: {}", lineno, e.what()); }
    }
}
bool Source(APrimalStructureItemContainer* c) {
    if (!c || S::Dedicated(c)) return false;
    auto name = S::Name(c->ClassField());
    for (const auto& s : g.sources) if (!s.empty() && name.find(s) != std::string::npos) return true;
    return false;
}
APrimalStructureItemContainer* Select(AShooterPlayerController* pc, int tribe, const std::string& action) {
    // No GetAimedUseActor / overloaded GetAimedActor / client camera / stale held actor.
    auto* player = pc->GetPlayerCharacter(); APrimalStructureItemContainer* chosen = nullptr;
    for (auto* c : S::Nearby(player, tribe, 300)) {
        if (action == "hub" && S::Dedicated(c)) continue;
        if (action == "source" && !Source(c)) continue;
        if (chosen && S::Distance(player,c) - S::Distance(player,chosen) < 50) {
            Send(pc, "Two storages are equally close. Move closer to the intended one."); return nullptr;
        }
        if (!chosen) chosen = c; else break;
    }
    if (!chosen) Send(pc, "Stand within 3m of your tribe storage (hub: normal box; source: Gatherer).");
    return chosen;
}
APrimalStructureItemContainer* Target(UClass* cls, Route& r, const std::vector<APrimalStructureItemContainer*>& nearby) {
    const auto text = S::Name(cls); unsigned int id = 0; size_t length = 0;
    for (const auto& t : r.targets) if (text.find(t.first) != std::string::npos && t.first.size() > length) { id=t.second; length=t.first.size(); }
    if (id) {
        for (auto* c : nearby) if (c->StructureIDField() == id && S::Accepts(c->MyInventoryComponentField(), cls)) return c;
        return nullptr;
    }
    for (auto* c : nearby) if (S::Dedicated(c) && S::Accepts(c->MyInventoryComponentField(), cls)) return c;
    return nullptr;
}
int Process(int tribe, Route& r) {
    auto* hub = S::Resolve(r.hub, tribe);
    if (!hub || S::Dedicated(hub)) { r.running=false; return 0; }
    auto nearby = S::Nearby(hub, tribe, g.radius); int moved=0, ops=0;
    for (auto* c : nearby) {
        if (c==hub || S::Dedicated(c) || !(r.sources.count(c->StructureIDField()) || (r.auto_sources && Source(c)))) continue;
        for (auto* cls : S::Resources(c->MyInventoryComponentField())) {
            if (moved>=g.max_cycle/2 || ++ops>32) break;
            moved += S::Transfer(c->MyInventoryComponentField(), hub->MyInventoryComponentField(), cls, std::max(1,g.max_cycle/2)-moved);
        }
        if (ops>32 || moved>=g.max_cycle/2) break;
    }
    for (auto* cls : S::Resources(hub->MyInventoryComponentField())) {
        if (moved>=g.max_cycle || ++ops>64) break;
        auto* t = Target(cls,r,nearby);
        if (t && t!=hub && !r.sources.count(t->StructureIDField()) && !Source(t))
            moved += S::Transfer(hub->MyInventoryComponentField(),t->MyInventoryComponentField(),cls,g.max_cycle-moved);
    }
    return moved;
}
void Fail(const char* stage, const char* message) {
    faulted=true; for (auto& p : routes) p.second.running=false;
    Log::GetLog()->error("GathererRouter stopped at {}: {}. Inspect inventory counts before restarting.", stage, message);
}
void CommandImpl(AShooterPlayerController* pc,FString* raw) {
    if (!pc || !raw || !g.enabled || ArkApi::IApiUtils::IsPlayerDead(pc)) return;
    const int tribe=ArkApi::GetApiUtils().GetTribeID(pc); if (tribe<50000) { Send(pc,"You must be in a tribe."); return; }
    std::istringstream in(raw->ToString()); std::string root,action,arg; in>>root>>action>>arg;
    action=S::Lower(action); arg=S::Lower(arg);
    auto& r=routes[tribe];
    if (action=="status") { Send(pc,"Router v1.2: hub="+std::to_string(r.hub)+", running="+(r.running?"YES":"NO")+", fault="+(faulted?"YES":"NO")+", sources="+std::to_string(r.sources.size())+", targets="+std::to_string(r.targets.size())); return; }
    if (action=="stop") { r.running=false; Send(pc,"Router paused."); return; }
    if (action=="clear") { routes.erase(tribe); Save(); Send(pc,"Route cleared."); return; }
    if (faulted) { Send(pc,"Router stopped after an error. Check log; restart after inspection."); return; }
    const auto now=std::chrono::steady_clock::now();
    if (now<cooldowns[tribe]) { Send(pc,"Wait 2 seconds."); return; } cooldowns[tribe]=now+std::chrono::seconds(2);
    Log::GetLog()->info("Router command tribe={} action={} stage=begin",tribe,action);
    if (action=="hub" || action=="source" || action=="target") {
        if (action=="target" && (arg.empty() || arg.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos)) { Send(pc,"Usage: /router target metalingot (or elementshard, etc.)"); return; }
        auto* c=Select(pc,tribe,action); if (!c) return;
        const auto id=c->StructureIDField(); if (!id) { Send(pc,"Storage has no persistent ID yet."); return; }
        if (action=="hub") { r.hub=id; r.running=false; }
        else if (action=="source") r.sources.insert(id);
        else {
            if (id==r.hub || r.sources.count(id) || Source(c)) { Send(pc,"Target cannot be the hub or a source."); return; }
            if (S::Dedicated(c)) { S::DediState d;
                if (!S::ReadDedi(c->MyInventoryComponentField(),d) || S::Name(d.resource).find(arg)==std::string::npos) { Send(pc,"Dedicated resource unsupported/unassigned or does not match the key."); return; }
            }
            r.targets[arg]=id;
        }
        Save(); Send(pc,"Assigned "+action+" id="+std::to_string(id)+". Test /router run, then /router start.");
        Log::GetLog()->info("Router assigned tribe={} action={} id={} class={}",tribe,action,id,S::Name(c->ClassField()));
    } else if (action=="auto") { r.auto_sources=!r.auto_sources; Save(); Send(pc,r.auto_sources?"Automatic source discovery ON":"Automatic source discovery OFF"); }
    else if (action=="start" || action=="run") {
        auto* hub=S::Resolve(r.hub,tribe); if (!hub || S::Dedicated(hub)) { Send(pc,"Assign a normal storage with /router hub first."); return; }
        if (action=="run") { r.running=false; Send(pc,"Test cycle moved "+std::to_string(Process(tribe,r))+" items; router remains paused."); }
        else { r.running=true; Send(pc,"Router started. It pauses after a server restart."); }
    } else Send(pc,"/router hub | source | target <class-key> | auto | run | start | stop | status | clear");
}
void Command(AShooterPlayerController* pc,FString* raw,EChatSendMode::Type) {
    try { CommandImpl(pc,raw); }
    catch(const std::exception& e) { Fail("command",e.what()); Send(pc,"Router stopped after an error. Check log."); }
    catch(...) { Fail("command","unknown exception"); }
}
void Timer() {
    if (!g.enabled || faulted || ++tick<g.interval) return; tick=0;
    try { for (auto& p:routes) if (p.second.running) Process(p.first,p.second); }
    catch(const std::exception& e) { Fail("timer",e.what()); }
    catch(...) { Fail("timer","unknown exception"); }
}
void Reload(APlayerController*,FString*,bool) {
    try { ReadConfig(); for(auto& p:routes) p.second.running=false; Log::GetLog()->info("Router config reloaded; routes paused"); }
    catch(const std::exception& e) { Fail("reload",e.what()); }
}
}
extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    Log::Get().Init("GathererRouter");
    try {
        GathererRouter::ReadConfig(); GathererRouter::LoadState(); GathererRouter::registered_command=GathererRouter::g.command;
        ArkApi::GetCommands().AddChatCommand(FString(GathererRouter::registered_command.c_str()),&GathererRouter::Command);
        ArkApi::GetCommands().AddOnTimerCallback("GathererRouter.Tick",&GathererRouter::Timer);
        ArkApi::GetCommands().AddConsoleCommand("GathererRouter.Reload",&GathererRouter::Reload);
        Log::GetLog()->info("Loaded GathererRouter v1.2: nearest storage selection; all routes paused; no turret hooks");
    } catch(const std::exception& e) { Log::GetLog()->error("Initialization failed: {}",e.what()); }
}
extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try {
        ArkApi::GetCommands().RemoveChatCommand(FString(GathererRouter::registered_command.c_str()));
        ArkApi::GetCommands().RemoveOnTimerCallback("GathererRouter.Tick");
        ArkApi::GetCommands().RemoveConsoleCommand("GathererRouter.Reload");
    } catch(...) {}
}
