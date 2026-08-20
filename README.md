# ARK plugins — corrected source release

Three ArkApi 3.56 plugins, one repo, one build:

- **TurretControl/** — turret ammo fill/caps, see `TurretControl/README.md`
- **PvPCooldowns/** — PvP kill cooldown, see `PvPCooldowns/README.md`
- **DamageAlerts/** — native floating damage numbers and tribe alerts, see `DamageAlerts/README.md`

Fixes in this release:

- PvPCooldowns captures the victim controller before death, rejects wild-dino
  damage, survives TurretControl hot reloads and supports an optional native
  crossed-swords buff icon/countdown after respawn.
- TurretControl no longer consumes Simple Rifle Ammo and resolves same-tribe
  overflow refund recipients before scanning turrets.
- DamageAlerts rejects wild dinos/non-player team ids and can send ASE's native
  floating damage RPC directly to the attacker without a client mod.

The custom PvP icon source is
`PvPCooldowns/Assets/PvPCooldown_CrossedSwords.png`. It must be imported into
a tiny ASE Dev Kit `PrimalBuff` mod; a server-only DLL cannot send a new
texture to unmodded clients. Without the mod, chat countdowns remain active.

## Build via GitHub Actions (no local Visual Studio needed)

Самый простой вариант для Windows: распаковать архив и запустить
`START_UPLOAD_ALLPLUGINS.bat`. Вставить ссылку на **новый пустой** GitHub
репозиторий; скрипт загрузит все три проекта и откроет страницу Actions.

Push this repo to GitHub, then check the **Actions** tab — `.github/workflows/build.yml`
runs on push to `main`/`master` (or manually via **Run workflow**), builds all
three `.sln` in one job, and uploads a single artifact **AllPlugins-Release-x64**
containing:

```
TurretControl/TurretControl.dll + config.json + PluginInfo.json
PvPCooldowns/PvPCooldowns.dll   + config.json + PluginInfo.json
DamageAlerts/DamageAlerts.dll   + config.json + PluginInfo.json
```

Download that artifact after the run finishes, then copy each subfolder into
your server's `ShooterGame/Binaries/Win64/ArkApi/Plugins/`.

## Build locally instead

Each plugin still has its own `CMakeLists.txt` if you'd rather build with
CMake + a local ArkApi SDK checkout — see the individual READMEs.
