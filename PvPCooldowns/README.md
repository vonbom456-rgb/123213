# PvPCooldowns 1.2

Version 1.2 starts the RAID/PVP timer on real enemy damage to a player,
tamed dino or structure and tags every online member of both tribes. It uses
ARK's native on-screen notification with a vanilla icon, so no client mod is
required. `/pvpcdtest` starts a safe self-test; `/pvpcd` shows the remaining
time. `/pvpcdicon default|players|dinos|structures` previews every safe icon
exposed directly by the player controller. A custom PNG buff icon still
requires a client mod.

The plugin hooks both `APrimalCharacter.TakeDamage` and
`APrimalStructure.TakeDamage`. When applied damage crosses tribes, all online
members of the attacking and defending tribes are tagged. Repeated hits
extend the expiry without repeating the start message for every bullet.

Attribution accepts enemy players, tamed dinos, structures and their
projectiles. Wild dinos are explicitly rejected, and `MinTribeTeamId`
(default `50000`) filters environmental/non-player teams.

`config.json`:

```json
{
  "General": {
    "CooldownSeconds": 45,
    "MinDamageToTrigger": 1.0,
    "ShowMessages": true,
    "SelfCheckCommand": "/pvpcd",
    "TestCommand": "/pvpcdtest",
    "IconTestCommand": "/pvpcdicon",
    "ReminderIntervalSeconds": 5,
    "MinTribeTeamId": 50000
  },
  "HudNotification": {
    "Enabled": true,
    "DisplayScale": 1.1,
    "DisplayTime": 4.0,
    "Icon": "players"
  },
  "HudBuff": {
    "Enabled": false,
    "BlueprintPath": ""
  }
}
```

`/pvpcd` lets a player check their own remaining cooldown (set
`SelfCheckCommand` to `""` to disable it). While a cooldown is active, a
reminder with the remaining time is also re-sent to chat and the native
on-screen notification every `ReminderIntervalSeconds` (default 5; set to
`0` to disable).

## Optional custom crossed-swords buff icon

ArkApi 3.56 exposes the verified `APrimalBuff::StaticAddBuff` signature. While
the cooldown is active, the plugin attaches the configured buff to the player
after respawn, forces its HUD icon/timer visible and sets its lifetime to the
remaining cooldown.

The crossed-swords texture is included at
`Assets/PvPCooldown_CrossedSwords.png`. Import it into the ASE Dev Kit and make
a minimal `PrimalBuff` child named `Buff_PvPCooldown` with no gameplay stat
changes. Set the texture as its HUD icon, cook/install the tiny mod on both
server and clients, and keep the class path from the shipped config (or change
`HudBuff.BlueprintPath` to your cooked path).

A server DLL cannot transmit a new texture to unmodded clients. The default
configuration therefore leaves `HudBuff.Enabled` false and uses the native
notification with a vanilla icon. Enable the buff only after installing the
cooked companion mod on server and clients.

## Blocking other plugins' commands

This plugin does not — and safely cannot — reach into every other plugin's
chat commands and block them centrally; ArkApi doesn't expose a verified
hook for that. Instead it exports a query function, and plugins opt in the
same way TurretControl already does:

```cpp
extern "C" __declspec(dllexport) bool __fastcall PvpCooldowns_IsOnCooldown(AShooterPlayerController* pc);
```

**TurretControl integration is automatic.** On load (and then on a periodic
retry, so load order doesn't matter), PvPCooldowns looks for
`TurretControl.dll` and — if found — registers itself via TurretControl's
own `TurretControl_SetPvpCooldownChecker` export. Once linked, `/fill` is
blocked for a player while they're on cooldown (unless TurretControl's
`AllowDuringPvpCooldown` is set to `true`). No manual config needed on
either side; just have both plugins installed.

For any other plugin you write yourself, copy `Source/PvPCooldownsApi.h`
into it and look up `PvpCooldowns_IsOnCooldown` via `GetModuleHandleA` +
`GetProcAddress` (see the header for the exact snippet) before running
whatever command you want gated.

## Build

Same as TurretControl:

```
cmake -B build -S . -DARKAPI_ROOT=C:/path/to/AseApi
cmake --build build --config Release
```

Output goes to `dist/PvPCooldowns/`. Copy that folder into
`ShooterGame/Binaries/Win64/ArkApi/Plugins/`.

## Verified against

- `APrimalCharacter.TakeDamage(float, FDamageEvent*, AController*, AActor*) -> float`,
  `APrimalStructure.TakeDamage(...)`, `APrimalBuff::StaticAddBuff(...)`,
  `APrimalBuff::DeactivateAfterTimeField()` and the native HUD timer flags.
- `AActor::TargetingTeamField()`, `ArkApi::IApiUtils::GetSteamIdFromController`,
  `ArkApi::GetApiUtils().GetTribeID`
  — same source tree as TurretControl (see TurretControl's
  `SOURCE_VERIFICATION.md`).

As with TurretControl, please sanity-test on a non-production server before
relying on this.
