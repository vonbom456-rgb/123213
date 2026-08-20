# Build status

## Completed

- Full `TurretControl.cpp` implementation: complete.
- `config.json`, `PluginInfo.json`, optional Permissions plugin info: complete.
- Visual Studio `.sln` / `.vcxproj`: complete and XML-validated.
- `CMakeLists.txt`: complete.
- Windows batch build scripts: complete.
- README/install/test instructions: complete.
- Local JSON parser smoke test: passed (`/fill 1000 5000`).
- Static safety checks: passed for radius-limited octree search, same-tribe filter, Heavy/Tek caps, checked `RemoveItem` result, before/after quantity accounting, partial-add refunds, bullet refresh, plugin exports, and absence of ArkShop/PvPCooldowns hard dependencies.
- ArkApi calls cross-checked against the official AseApi 3.56 headers and official/open ASE plugin examples.

## Win64 DLL status

A real `TurretControl.dll` was **not linked in this execution environment**.

Reason: the available environment is Linux and does not contain Microsoft `cl.exe`, MSBuild, the Windows SDK, or the MSVC standard-library/import-library environment required to ABI-correctly build an ASE ArkApi Windows plugin. `clang-cl`/`lld-link` are present, but without the Microsoft SDK/STL they are not sufficient. MinGW is not installed, and using a MinGW C++ ABI would not be an acceptable substitute for ArkApi's MSVC C++ ABI.

Do not rename a placeholder to `.dll`. Build `Release|x64` with the included project on Windows using MSVC v142 and the matching AseApi 3.56 SDK. The post-build step produces exactly:

```text
dist/TurretControl/
├── TurretControl.dll
├── config.json
└── PluginInfo.json
```
