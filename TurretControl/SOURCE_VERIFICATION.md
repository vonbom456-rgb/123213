# TurretControl — ArkApi 3.56 source verification notes

Verified target: `ArkServerApi/AseApi` commit `f13b85979254b6b19c8d9255a0fc11c128978b8b`.

Key source locations checked before implementing the plugin:

- `version/Core/Private/Ark/ArkBaseApi.cpp`
  - declares `constexpr float api_version = 3.56f;`
- `version/Core/Public/ICommands.h`
  - `AddChatCommand`, `RemoveChatCommand`, `AddConsoleCommand`, `RemoveConsoleCommand`
- `version/Core/Public/API/ARK/PrimalStructure.h`
  - `APrimalStructureTurret`
  - `AmmoItemTemplateField()`
  - `RangeSettingField()`
  - `AISettingField()`
  - `UpdateNumBullets()`
  - `UpdatedTargeting()`
  - inherited `MyInventoryComponentField()` / `SetContainerActive(bool)`
- `version/Core/Public/API/ARK/Actor.h`
  - `AShooterPlayerController::SpawnActor(FString* blueprintPath, float spawnDistance, float spawnYOffset, float ZOffset, bool bDoDeferBeginPlay) -> AActor*`
    (confirmed against the public ArkServerApi (ASE) doxygen mirror at arkserverapi.wiki, generated from the same `AseApi` source tree; used only to spawn an actor whose class path is supplied by the server owner via config -- no game-content blueprint path is hard-coded, since that lives in the game's .pak files and is outside this SDK)
- `version/Core/Public/API/ARK/Inventory.h`
  - `InventoryItemsField()`
  - `GetItemTemplateQuantity(...)`
  - `IncrementItemTemplateQuantity(...)`
  - `RemoveItem(...) -> bool`
  - `NotifyClientsItemStatus(...)`
- `version/Core/Public/API/ARK/Other.h`
  - `UVictoryCore::BPLoadClass(...)`
  - `UVictoryCore::ServerOctreeOverlapActorsClass(...)`
- `version/Core/Public/API/ARK/Enums.h`
  - `EServerOctreeGroup::STRUCTURES`
- `version/Core/Public/Ark/ArkApiUtils.h`
  - `GetWorld()`
  - `GetTribeID(AShooterPlayerController*)`
  - `IsPlayerDead(...)`
  - message helpers
- `version/Core/Public/API/UE/UE.h`
  - `TSubclassOf<T>` layout (`UClass* uClass`)
- `out_lib/ArkApi.lib`
  - matching import library exists in the verified AseApi tree

Open ASE plugin examples checked:

- `ArkServerApi/ASE-Plugins/ArkShop/ArkShop/Private/StoreSell.cpp`
  - real stack decrement/removal pattern
- `ArkServerApi/ASE-Plugins/ArkShop/ArkShop/Private/Store.cpp`
  - real `IncrementItemTemplateQuantity(...)` usage
- `ArkServerApi/ASE-Plugins/AllEngrams/AllEngrams/AllEngrams.cpp`
  - real config loading, chat/console command registration and reload pattern
- `ArkServerApi/ASE-Plugins/AllEngrams/AllEngrams/AllEngrams.vcxproj`
  - official-style x64 DLL project, v142, C++17, ArkApi public headers and `ArkApi.lib`
- `ArkServerApi/ASE-Plugins/Permissions/Permissions/Public/ArkPermissions.h`
  - `Permissions::IsPlayerHasPermission(...)`

S+/SS references were used only to verify item/ammo facts. The shipped config intentionally does not hard-code an unverified placed-structure path derived from an item path.

No PvPCooldowns API symbol is linked or invented. The plugin exposes its own optional callback bridge for a future adapter.
