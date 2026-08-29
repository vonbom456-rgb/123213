# InventoryCapacity

Standalone ArkApi 3.56 plugin for ARK: Survival Evolved.

- Preserves ASE's normal 300-slot ceiling instead of granting extra inventory slots.
- Clears ARK's stale full-inventory flag only while the player really has a free slot.
- Fixes false `Inventory limit reached` results from vanilla or S+ Dedicated Storage without allowing a genuinely full inventory to overflow.
- Does not modify storage capacity, stack sizes, weight, turrets, or item quantities.

Install the complete `InventoryCapacity` folder under `ArkApi/Plugins` and restart the server.
