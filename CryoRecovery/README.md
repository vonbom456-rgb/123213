# CryoRecovery

Version 1.3 also shows stored healing time through `/cryoheal` and limits actual cryopod releases during RAID/PvP to 10 total / 5 large within 30 seconds.

Default recovery profile is controlled: cryopods restore 3% of maximum values per minute normally and 2.5% during RAID/PvP. Health, stamina, oxygen, food and water are restored; torpor and combat stats are never changed. Daeodon healing is x5 normally and x4 during RAID/PvP.

ArkApi 3.56 plugin for ARK: Survival Evolved.

- Dinos recover 2% of maximum HP per minute while stored in a cryopod.
- If the releasing player has a PvPCooldowns RAID/PvP tag, recovery is slightly slower at 1.5% per minute instead of being disabled.
- Daeodon healing is multiplied by 3 outside combat and by 2.25 during RAID/PvP.
- Cryopod recovery and Daeodon healing have independent switches and rates in `config.json`.
- Storage timestamps survive server/plugin restarts in `state.csv`.
- Recovery is applied after ARK finishes post-spawn stat initialization, so it is not lost when a dino leaves its cryopod.
- `/cryoheal` shows the active rates and hook status.

Install the complete `CryoRecovery` folder in `ArkApi/Plugins` and restart the server.
