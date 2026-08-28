# CryoRecovery

ArkApi 3.56 plugin for ARK: Survival Evolved.

- Dinos recover 2% of maximum HP per minute while stored in a cryopod.
- If the releasing player has a PvPCooldowns RAID/PvP tag, recovery is slightly slower at 1.5% per minute instead of being disabled.
- Daeodon healing is multiplied by 3 outside combat and by 2.25 during RAID/PvP.
- Cryopod recovery and Daeodon healing have independent switches and rates in `config.json`.
- Storage timestamps survive server/plugin restarts in `state.csv`.
- Recovery is applied after ARK finishes post-spawn stat initialization, so it is not lost when a dino leaves its cryopod.
- `/cryoheal` shows the active rates and hook status.

Install the complete `CryoRecovery` folder in `ArkApi/Plugins` and restart the server.
