# TurretControl 1.5 — ASE ArkApi 3.56

`TurretControl` is a server-side plugin for **ARK: Survival Evolved (ASE)**. It is not an ASA/CurseForge mod.

## Safe startup defaults

`HardCapEnabled` and `InventoryCapEnabled` are disabled by default. The global
inventory hook is not installed at all while InventoryCap is disabled. When a
cap is explicitly enabled, runtime work waits for a valid GameMode/GameState
and then an additional `StartupDelaySeconds` (default 60), so ARK can restore
the save before the plugin scans or hooks turret inventories.

Enable `HardCapEnabled` first after confirming a clean startup.
`InventoryCapEnabled` should be treated as experimental because other server
plugins can hook the same global inventory function. The periodic hard cap is
the safer enforcement mode.

Target environment:

- ARK: Survival Evolved dedicated server 358.x
- Ark Server API / ArkApi **3.56**
- Windows x64 `ShooterGameServer.exe`
- Works the same when that Windows server is hosted under Proton/Wine on Linux: the plugin is still a Windows x64 `.dll`
- ArkShop is **not** used by this plugin
- Permissions is optional and has a separate build configuration

## Implemented commands

### `/fill`

Finds nearby `APrimalStructureTurret` structures in the server structure octree, filters them by radius and tribe/team, then fills supported turret inventories using ammo from the invoking player's inventory only. Advanced Rifle Bullets are matched by the specific `advancedriflebullet` class-name token; generic `riflebullet` matching is intentionally rejected so Simple Rifle Ammo cannot be consumed.

Default caps applied by this plugin:

- Heavy Auto Turret: 10000 Advanced Rifle Bullets per turret
- Tek Turret: 5000 Element Shards per turret
- Auto Turret: 10000 Advanced Rifle Bullets per turret (all three are configurable in `config.json`)

Default search radius (`FillRadius`): 3000 units, on purpose kept modest (a "your own base" range rather than a map-spanning one) -- raise it in `config.json` if your base layout needs more reach. `HardCapScanRadius` (the separate radius used by the periodic backup sweep, see below) defaults to 5000 for the same reason.

The fill transaction is deliberately conservative:

1. Calculate each turret's live deficit.
2. Count the player's exact ammo class.
3. Plan an approximately even distribution when ammo is insufficient.
4. Remove ammo from the player first.
5. Measure the real inventory quantity difference after removal.
6. Add no more than the successfully removed amount to the turret.
7. Measure the real amount added.
8. Refund any failed/partial add to the player.
9. If an unexpected inventory behavior crosses the configured cap, take back only the overflow attributable to this command and give it straight back to the player it came from (it is never simply deleted).

This design is intended to prevent a failed removal from becoming duplicated ammo.

### Radius hologram on `/fill`

`FillRadiusHologram` shows a green native ARK debug sphere at the player when
`/fill` runs. Its radius is exactly `General.FillRadius`; it has no collision,
does not spawn a gameplay structure, needs no client mod and disappears by
itself. Because it is sent through the requesting player controller, normally
only that player sees it.

```json
"FillRadiusHologram": {
  "Enabled": true,
  "NativeDebugSphere": true,
  "DurationSeconds": 6,
  "Segments": 64,
  "BlueprintPath": "",
  "SpawnDistance": 0,
  "SpawnYOffset": 0,
  "SpawnZOffset": 0
}
```

The native sphere is enabled by default. `DurationSeconds` controls how long it
remains and `Segments` controls smoothness. Its upper half looks like a green
dome above normal terrain.

`BlueprintPath` is an optional, separate fallback. If supplied, the plugin also
spawns that visual actor through `AShooterPlayerController::SpawnActor`:

1. Find or build an actor Blueprint that renders as a ring/circle (many raid-protection or base-radius mods already ship one you can point at; or make a small one yourself in the Dev Kit).
2. Give **that Blueprint** its own lifespan (e.g. `Set Life Span` in its own Event Graph) so it disappears after a few seconds on its own -- this plugin does not track or destroy what it spawns, it only spawns it once per `/fill`.
3. Set `BlueprintPath` to that actor's full class path. Set
   `NativeDebugSphere` to `false` if you want only the custom visual.
4. `SpawnDistance` / `SpawnYOffset` / `SpawnZOffset` are passed straight through to `SpawnActor` and control where relative to the player it appears; `0, 0, 0` spawns it at the player's own position.

The optional Blueprint is not automatically scaled. The native sphere always
tracks `FillRadius` exactly.

## Inventory cap enforcement

`InventoryCapEnabled` (default `true`) hooks the game's own `AllowAddInventoryItem_MaxQuantity` check on supported turret inventories. This means a turret **cannot physically hold more than its configured limit** through *any* manual path — drag-and-drop, "transfer all", crafting straight into the turret, etc. The game clamps the amount actually added and leaves the remainder in the source inventory; nothing is removed or lost at this stage.

`HardCapEnabled` (default `true`) is a periodic backup sweep (every `HardCapIntervalSeconds`, default 2s) that re-checks turrets near online players and trims anything still above the limit. This only matters for cases that bypass the live hook (e.g. console/admin item commands, imported save data, or a limit that was lowered via `TurretControl.Reload` after turrets were already filled past the new cap). When the sweep has to trim overflow it now tries to hand that ammo back:

- it builds the same-tribe recipient list before scanning, so an enemy player encountered first cannot suppress a rightful refund;
- it refunds into the inventory of a nearby online player, but **only** if that player belongs to the same tribe as the turret;
- if no eligible tribe member is nearby at the moment the sweep runs, the overflow can't be handed to anyone and is removed (there is no way to identify who originally placed it after the fact).

In short: normal manual placement is blocked outright once a turret is at its cap, and the rare backup-sweep case now returns the ammo whenever a rightful owner is present to receive it.

### `/turrets on`
Turns on nearby tribe turrets.

### `/turrets off`
Turns off nearby tribe turrets.

### `/turrets low`
Sets `RangeSettingField()` to the configured Low value (default `0`) and calls `UpdatedTargeting()`.

### `/turrets medium`
Default value `1`.

### `/turrets high`
Default value `2`.

### `/turrets players`
### `/turrets tames`

The code path exists, but **these two commands are intentionally disabled by default** because ArkApi 3.56 exposes `AISettingField()` without documenting the numeric mapping for the individual targeting choices. No undocumented numeric value is hard-coded.

If you have verified values for your exact ASE build/mod stack, set:

The shipped config keeps both values disabled:

```json
"TargetingValues": {
  "PlayersOnly": -1,
  "PlayersAndTames": -1
}
```

Replace `-1` only after you have verified the numeric mapping for your exact ASE build/mod stack.

## S+ / Super Structures support

The plugin does not identify a turret by a `PrimalItemStructure_...` item path. It works with the **placed structure actor** and its real `AmmoItemTemplateField()`.

For S+/SS derivatives that inherit `APrimalStructureTurret`, automatic detection normally works by:

- Tek: Element Shard ammo template or Tek turret class naming
- Heavy: turret class naming contains Heavy + Turret
- Auto: turret class + Advanced Rifle Bullet ammo template

If your S+/SS fork uses a class name that automatic detection does not recognize, put the **placed Structure UClass** path in the relevant `Custom*Classes` array in `config.json`.

Do not put paths such as:

```text
.../PrimalItemStructure_AutoTurret_Heavy.PrimalItemStructure_AutoTurret_Heavy_C
```

That is an item class, not the placed structure actor. `TurretControl` validates configured custom classes at load time and rejects/logs classes that are not derived from `APrimalStructureTurret`.

## Nearby-structure search

`/fill` does not run an unrestricted all-actor scan. It uses ArkApi/ASE's server spatial octree via `UVictoryCore::ServerOctreeOverlapActorsClass(...)` with:

- octree group: `EServerOctreeGroup::STRUCTURES`
- class: `APrimalStructureTurret`
- configured radius
- an additional distance re-check before mutation

## PvPCooldowns integration

The core plugin has **no hard dependency** on PvPCooldowns and does not invent an API for it.

Default behavior if no adapter is attached: `/fill` works normally, even if `AllowDuringPvpCooldown` is `false`, because the plugin has no authoritative way to know cooldown state.

An optional bridge is exported:

```cpp
using PvpCooldownChecker = bool(__fastcall*)(AShooterPlayerController*);
extern "C" __declspec(dllexport)
void __fastcall TurretControl_SetPvpCooldownChecker(PvpCooldownChecker checker);
```

A small adapter can provide a checker later once the exact PvPCooldowns API used on your server is known. If a checker is installed and returns `true`, `/fill` is blocked when `AllowDuringPvpCooldown=false`.

## Permissions

Default `Release` build has no Permissions dependency and ships with:

```json
"UsePermissions": false
```

For Permissions support, build `ReleasePermissions|x64` and set `PERMISSIONS_ROOT` to a source/dev tree that contains:

```text
Permissions/Permissions/Public/ArkPermissions.h
Permissions/out_lib/Permissions.lib
```

Then set:

```json
"Permissions": {
  "UsePermissions": true,
  "DefaultPermission": "TurretControl.Default"
}
```

The Permissions build copies `PluginInfo.permissions.json` as the final `PluginInfo.json`, declaring the `Permissions` dependency.

## Required ArkApi SDK/headers

Use the official `ArkServerApi/AseApi` tree whose core reports API version `3.56`.

This project was written against the verified tree at commit:

```text
f13b85979254b6b19c8d9255a0fc11c128978b8b
```

Required paths from that tree:

```text
AseApi/version/Core/Public/
AseApi/include/
AseApi/out_lib/ArkApi.lib
```

The code uses the same general Visual Studio layout as official ASE plugin projects: x64 DLL, C++17, Windows SDK, ArkApi public headers and `ArkApi.lib`.

## Build — Visual Studio

Recommended baseline for maximum compatibility with existing ASE plugin projects:

- Visual Studio 2019, or Visual Studio 2022 with **MSVC v142 build tools** installed
- Desktop development with C++
- Windows 10 SDK
- x64

Clone/get the matching AseApi source/dev package and point `ARKAPI_ROOT` at it, for example:

```bat
set ARKAPI_ROOT=C:\SDK\AseApi
```

Open:

```text
TurretControl.sln
```

Select:

```text
Release | x64
```

Build the solution. The post-build step creates:

```text
dist\TurretControl\TurretControl.dll
dist\TurretControl\config.json
dist\TurretControl\PluginInfo.json
```

For the Permissions-linked build, also set:

```bat
set PERMISSIONS_ROOT=C:\SDK\ASE-Plugins\Permissions
```

and select:

```text
ReleasePermissions | x64
```

## Build — CMake

From a Visual Studio Developer Command Prompt:

```bat
cmake -S . -B build -A x64 -T v142 -DARKAPI_ROOT=C:/SDK/AseApi
cmake --build build --config Release
```

Permissions build:

```bat
cmake -S . -B build-permissions -A x64 -T v142 ^
  -DARKAPI_ROOT=C:/SDK/AseApi ^
  -DTURRETCONTROL_WITH_PERMISSIONS=ON ^
  -DPERMISSIONS_ROOT=C:/SDK/ASE-Plugins/Permissions
cmake --build build-permissions --config Release
```

## Dependencies

Required:

- ArkApi / AseApi 3.56 public headers
- `ArkApi.lib` matching that SDK
- MSVC x64 toolchain
- Windows 10 SDK

Optional:

- `ArkPermissions.h`
- `Permissions.lib`
- `Permissions.dll` on the server

Not required:

- ArkShop
- ArkShopUI
- CurseForge/ASA mods
- nlohmann/json (the project includes a small local JSON parser)

## Installation

Final server folder:

```text
ShooterGame/Binaries/Win64/ArkApi/Plugins/TurretControl/
├── TurretControl.dll
├── config.json
└── PluginInfo.json
```

Copy the three files there and restart the server.

## Reload

Registered ArkApi console command:

```text
cheat TurretControl.Reload
```

The handler re-reads `config.json`, reloads custom turret classes, and re-registers the chat commands if their configured names changed.

## Verification checklist

1. Start/restart the server.
2. Confirm the ArkApi log shows the plugin loading. `TurretControl` itself writes:
   ```text
   Loaded plugin - TurretControl
   ```
3. Join as a normal player.
4. Put Advanced Rifle Bullets and Element Shards in the **player inventory**.
5. Place Heavy and Tek turrets belonging to the player's tribe.
6. Put a partial amount into at least one Heavy (for example 8500) and one Tek (for example 4300).
7. Run:
   ```text
   /fill
   ```
8. Verify Heavy does not exceed 10000.
9. Verify Tek does not exceed 5000.
10. Verify the player's ammo decreases by exactly the amount added.
11. Verify a turret belonging to another tribe is unchanged.
12. Test low-ammo behavior by carrying less ammo than total free capacity; confirm it distributes what is available without creating extra ammo.
13. Try to manually drag more Advanced Rifle Bullets into an already-full Heavy turret; confirm the extra amount is rejected and stays in your own inventory rather than disappearing.
14. Test `/turrets on`, `/turrets off`, `/turrets low`, `/turrets medium`, `/turrets high`.
15. If using S+/SS, repeat with its Heavy/Tek structures. If one is not detected for `/fill`, add its actual placed structure class to the relevant `Custom*Classes` array and reload.

## Important build-status note

The source tree is complete, but the repository ZIP supplied from ChatGPT is **not a claim that the Win64 DLL was linked in ChatGPT's Linux execution environment**. A real ASE plugin must be linked against the Windows/MSVC ArkApi ABI. Do not rename any placeholder file to `.dll`; build `Release|x64` with MSVC and the matching ArkApi 3.56 SDK.
