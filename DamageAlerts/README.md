# DamageAlerts 1.5 — RedEnemyNumbers

Native ARK floating damage numbers for the player dealing damage. When a
different player-owned tribe attacks, the victim tribe also receives native
enemy-coloured floating numbers at the damaged target and a red text warning.
PvPCooldowns separately handles the persistent raid timer.

Version 1.3 forces ARK's own server-side floating-damage setting. Use
`/datest` in chat to send a `12345` test number and show live hook counters.

Hooks `APrimalCharacter.TakeDamage` and `APrimalStructure.TakeDamage` (the
standard ASE damage hook points, same as DamageAlerts' sibling plugins) and
attributes tribe via `AActor::TargetingTeamField`. Hits are summed per
player and flushed at most once every `FlushIntervalSeconds` (default 1),
so rapid hits (turret fire, DoT) become one line, not spam.

`config.json`:

```json
{
  "General": {
    "MinDamageToReport": 1.0,
    "FlushIntervalSeconds": 1,
    "NotifyAttacker": true,
    "NotifyVictimTribeOnEnemyHit": true,
    "MinTribeTeamId": 50000
  },
  "FloatingDamage": {
    "Enabled": true,
    "ForceNativeServerSetting": true,
    "ShowEnemyDamageToVictimTribe": true,
    "AlsoSendAttackerChat": false,
    "VerticalOffset": 100.0
  },
  "Diagnostics": {
    "TestCommand": "/datest"
  },
  "Messages": {
    "AttackerHit": "+{0} урона по {1}",
    "VictimHit": "-{0} урона по {1} (враг)"
  }
}
```

- `NotifyAttacker` — master switch for the attacker's damage feedback.
- `FloatingDamage.Enabled` — calls ASE's native
  `ClientAddFloatingDamageText` RPC at the target location. ARK renders and
  colours the number on the client, so there is no custom HUD asset or mod.
- `FloatingDamage.AlsoSendAttackerChat` — also retain the old aggregated green
  chat line (disabled by default to avoid duplicate feedback).
- `FloatingDamage.ShowEnemyDamageToVictimTribe` — sends native floating damage
  to online victim-tribe clients using the enemy attacker's team, so ARK applies
  its normal enemy colour (red).
- `FloatingDamage.VerticalOffset` — moves the number upward from the target's
  root position, in Unreal units.
- `NotifyVictimTribeOnEnemyHit` sends a red warning only when the damage came
  from a different player-owned tribe; own/friendly/wild damage is ignored.
- `MinTribeTeamId` — lower boundary for player-owned teams; wild dinos are
  additionally rejected through `BPIsTamed()`.
- `{0}` = summed damage (rounded), `{1}` = target category
  (структура/живность/игрок).

Floating numbers are emitted per hit, like the screenshot. Chat alerts remain
aggregated by `FlushIntervalSeconds`. If the server already has
`ShowFloatingDamageText=true`, disable one of the two mechanisms to avoid
seeing duplicate numbers.

## Build

Same as the other two:

```
cmake -B build -S . -DARKAPI_ROOT=C:/path/to/AseApi
cmake --build build --config Release
```

Output goes to `dist/DamageAlerts/`.

## Note

This is a fresh, less battle-tested plugin (unlike TurretControl/PvPCooldowns
it hasn't been iterated on) — test on a non-production server first, and
tune `MinDamageToReport` / `FlushIntervalSeconds` if it's too chatty for
your rates.
