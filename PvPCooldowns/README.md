# PvPCooldowns

Puts a player on a PvP cooldown when they're killed by another tribe — a
player, or that tribe's turret/dino — and lets other plugins (TurretControl,
or your own) check that cooldown before running their own commands.

## How a kill is detected

Hooks `APrimalCharacter.TakeDamage`, captures the victim controller before
the lethal call, then checks whether the target changed from alive to dead.
This ordering matters because ArkApi intentionally does not return a
controller from `FindControllerFromCharacter` after the character is dead.

Attribution accepts enemy players, tamed dinos, structures and their
projectiles. Wild dinos are explicitly rejected, and `MinTribeTeamId`
(default `50000`) filters environmental/non-player teams.

`config.json`:

```json
{
  "General": {
    "CooldownSeconds": 45,
    "ShowMessages": true,
    "SelfCheckCommand": "/pvpcd",
    "ReminderIntervalSeconds": 15,
    "MinTribeTeamId": 50000
  },
  "HudBuff": {
    "Enabled": true,
    "BlueprintPath": "/Game/Mods/PvPCooldowns/Buff_PvPCooldown.Buff_PvPCooldown_C"
  }
}
```

`/pvpcd` lets a player check their own remaining cooldown (set
`SelfCheckCommand` to `""` to disable it). While a cooldown is active, a
reminder with the remaining time is also re-sent to chat every
`ReminderIntervalSeconds` (default 15; set to `0` to disable).

## Crossed-swords HUD icon and native countdown

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

A server DLL cannot transmit a new texture to unmodded clients. If the class
is not installed, the plugin logs one warning and safely keeps the chat timer
and `/pvpcd` fallback instead of calling an invalid asset.

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
  `APrimalCharacter::IsDead()`, `APrimalBuff::StaticAddBuff(...)`,
  `APrimalBuff::DeactivateAfterTimeField()` and the native HUD timer flags.
- `AActor::TargetingTeamField()`, `ArkApi::IApiUtils::GetSteamIdFromController`,
  `ArkApi::IApiUtils::FindControllerFromCharacter`, `ArkApi::GetApiUtils().GetTribeID`
  — same source tree as TurretControl (see TurretControl's
  `SOURCE_VERIFICATION.md`).

As with TurretControl, please sanity-test on a non-production server before
relying on this.
