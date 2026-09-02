# ResourceLogistics 1.1 / GathererRouter 1.2

These are candidate fixes; compiling and accounting tests do not replace an ASE/S+ runtime test.
The exact previous crash stack is not available. The unsafe aiming fallback chain is removed,
null/deleting containers are rejected, item pointers are no longer retained across removal callbacks,
and Dedicated Storage is never treated as an ordinary inventory.

## Install / rollback

Back up the world and both current plugin directories. Stop the server. Replace only
ResourceLogistics and GathererRouter DLLs and PluginInfo.json; supplied config files reduce batch limits.
Existing configs remain readable and their values are clamped. Preserve routes.tsv.
Do not install other DLLs from a full build archive. TurretControl and its configuration are unchanged.
To roll back, stop the server and restore the two plugin backups (or move them outside Plugins).

## Router workflow changed

All routes start paused on every process start and config reload. Assignment alone does not move resources.
Stand within 300 game units of the intended tribe storage and use /router hub.
Selection is by proximity, not camera aiming. Similar-distance candidates are rejected;
move closer to the desired one. The response and log show its structure ID.
The hub must be a normal inventory (ordinary S+ boxes work), not Dedicated Storage.
Sources must match SourceClassTokens. /router source registers the closest matching gatherer.
Manual targets can be ordinary boxes or supported Dedicated Storage:
/router target metalingot or /router target elementshard, etc. Keys match item class names.
Automatic Dedicated routing uses SelectedResourceClass and ResourceCount, including assigned empty storage.
Unassigned/unsupported Dedicated Storage is skipped, never written through generic AddNewItem.
All resources of the game's Resource item type are eligible; no hardcoded metal-only list.

Run /router status, then test /router run with a few inexpensive resources.
Verify total resource counts across source, hub and target. Only then use /router start.
/router stop pauses the tribe route. /router clear removes only its saved routing settings.
After each server restart, /router start is required once per tribe, not once per withdrawal/player.
At most 1000 units move per individual transfer, 10000 per cycle and 256 nearby containers are considered.

## Other commands / limitations

/pull metalingot 1 and /distribute retain tribe and RAID/PvP checks.
Test with a single inexpensive resource first. These commands do not repair the S+ radial Withdraw action.
If a callback reports inconsistent quantities, further transfers in that plugin stop until restart.
Do not repeatedly retry: collect the plugin log and inspect both inventory counts.
There are no global hooks, turret writes, forced slot increases or stat changes.
Exception handling covers C++ exceptions, not arbitrary access violations inside the game/mod.
Use a test server/world backup before production; no promise of crash-proof or atomic cross-inventory storage.
