#pragma once

// Drop this header into another plugin if it wants to check PvPCooldowns
// status. No compile-time link to PvPCooldowns.dll is required -- look the
// function up at runtime once PvPCooldowns has (hopefully) loaded:
//
//   #include "PvPCooldownsApi.h"
//   #include <Windows.h>
//
//   PvpCooldownsIsOnCooldownFn is_on_cooldown = nullptr;
//   if (HMODULE mod = GetModuleHandleA("PvPCooldowns.dll")) {
//       is_on_cooldown = reinterpret_cast<PvpCooldownsIsOnCooldownFn>(
//           GetProcAddress(mod, "PvpCooldowns_IsOnCooldown"));
//   }
//   if (is_on_cooldown && is_on_cooldown(pc)) {
//       // player is on PvP cooldown, block your command here
//   }
//
// GetModuleHandleA can return null if PvPCooldowns hasn't loaded yet (plugin
// load order isn't guaranteed) -- re-check on each use rather than caching a
// null result, the same way PvPCooldowns itself retries linking outward.

using PvpCooldownsIsOnCooldownFn = bool(__fastcall*)(class AShooterPlayerController*);
