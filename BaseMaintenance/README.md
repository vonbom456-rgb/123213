# BaseMaintenance

ArkApi 3.56 plugin for ARK: Survival Evolved.

- `/repair all` repairs damaged nearby tribe structures.
- Uses the player's inventory and the structure item's normal repair requirements.
- The operation is all-or-nothing: when resources are insufficient nothing is consumed, and the missing list is shown.
- Respects ARK's post-damage structure repair cooldown.
- Radius is configured in foundation units.
- Can enable the native server `bPreventSpawnAnimations` flag to remove the respawn implant-inspection animation.

Server console command: `BaseMaintenance.Reload`
