# GitHub Actions build

1. Create a GitHub repository.
2. Upload the entire project contents to the repository root.
3. Open **Actions** -> **Build TurretControl** -> **Run workflow**.
4. When the run finishes, download **TurretControl-Release-x64** from Artifacts.
5. Extract it. It should contain `TurretControl.dll`, `config.json`, `PluginInfo.json`.

The workflow pins ArkServerApi/AseApi commit `f13b85979254b6b19c8d9255a0fc11c128978b8b` and verifies that it reports ArkApi 3.56 before building.
