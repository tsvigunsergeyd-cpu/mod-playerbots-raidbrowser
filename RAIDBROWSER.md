# mod-playerbots: raid browser bots (work in progress)

A branch of [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots): when a player signs up for a raid through the raid browser (LFG), it fills the group with bots, prepares them for the content and teleports the group to the raid entrance.

**Status: experimental, unfinished. Looking for people who want to continue it.** It was developed and tested on a single private AzerothCore server. The end of this file lists what was tested in game and what was not.

## Base

- Branch `raidbrowser-bots`, built on top of mod-playerbots `master` at commit `979e69d0`.
- About 40 commits, 15 files, roughly +1400 / -60 lines. Most of the code is in `src/Script/Playerbots.cpp` and `src/Bot/Factory/PlayerbotFactory.cpp`.

## What changed: before and after

**1. Raid signup**
- Before: bots did not react to a raid signup; the group had to be assembled by hand.
- After: on a raid signup (currently Icecrown Citadel only) the group is assembled with bots automatically. The feature is off by default. If signup is not possible (player dead, in combat, inside an instance, already in a raid browser group, group already has bots or is full), the player gets a short message with the reason.

**2. Raid roster**
- Before: no roster selection.
- After: the roster comes from a template in the config (how many tanks, healers and damage dealers, of which class). 25-man uses 24 slots, 10-man uses 9. Live players in the group are subtracted by role.

**3. Bot preparation**
- Before: no preparation specific to the content.
- After: each bot gets a spec by role, gear (BiS list with a fallback), glyphs, enchants, gems, ammo, a pet and consumables.

**4. Joining the group and teleport**
- Before: nothing like this.
- After: bots are invited to the group; bots that do not join within 60 seconds are dropped. The group is teleported to the raid entrance.

**5. Bots leaving**
- Before: bots stayed in the world.
- After: when the player leaves, disbands the group, logs out, or a bot is kicked, the bots log out.

**6. Protection and zones**
- Before: no restrictions specific to raid bots.
- After:
  - Admin `.playerbots` commands are closed to non-GM players.
  - Raid bots obey only their owner.
  - If a raid bot leaves the raid zone (all of Icecrown Citadel, or a 150-yard circle around the gathering point at the entrance), it stops, switches to "stay" and is teleported back to the gathering point. Checked once a second, and only for a bot that has been inside the zone at least once.
  - Some command words (for example `cast`, `castnc`, `debug`) are denied for raid bots, including for the owner. GMs are not affected.

**7. Addon and `.dc` control**
- Before: the addon and the dungeon-clear commands were open to any player in the group.
- After: when the tank in the group is a raid bot, only the owner and GMs can control it. Status and boss list stay open to everyone. The addon itself was not changed; only the server side was.

**8. Dungeon queue (mod-dungeon-clear)**
- Before: bot queue fill worked only while the dungeon-clear master switch was on.
- After: it works independently, with its own switch. Dungeon bots are prepared the same way as raid bots and also get consumables.

## Configuration

Added to `conf/playerbots.conf.dist`:

```
Playerbots.RaidBrowserAutoFill = 0      # 1 = fill raid groups (default: disabled)
Playerbots.FillRaid.Templates = 25,10   # which templates exist

# Slot: Playerbots.FillRaid.<name>.Slot.<n> = <classId>:<role>
# classId: 1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 6 dk,
#          7 shaman, 8 mage, 9 warlock, 11 druid, 0 = any dps filler
# role: tank / heal / dps; n is contiguous from 0
Playerbots.FillRaid.25.Slot.0 = 2:tank
...
```

The defaults include a full 25-man template (24 slots) and a 10-man template (9 slots).

## Companion: mod-dungeon-clear

A separate repository holds 3 commits on top of `jrad7/mod-dungeon-clear` (`7f63c22`). The last one (`d5130ff`) calls `PlayerbotFactory::PrepareBotForContent`, which exists only in this branch. **Both branches must be used together.**

## Tested in game

- 25-man assembly (24/24 bots) and 10-man assembly (9/9), repeated runs.
- After preparation: spec matches, 0 free talent points, glyphs 6/6, 0 empty sockets, 9-11 enchants, BiS applied, ammo present, about 150-190 ms per bot.
- Signup refused from a raid browser group and for a leader with an `addclass` bot.
- `cast` blocked for a non-GM owner, zone reset radius 150.

## Not tested

- Return to the gathering point: the zone radius 150 was tested; other cases (for example a dead bot at the entrance) were not.
- Other denied words, the GM command chain, arrival at the ICC entrance.
- Addon and `.dc` permissions (needs a second account), assembly by a leader with live friends and no bots, 10-man with BiS, several rare cleanup paths.
- Glyph match with the template, bot strategies (`co ?`), consumables in bags, pets, druid bear list.
- mod-dungeon-clear after `d5130ff`, a build on a clean machine.

## Known issues

- The template gives every dps mage spec number 2, which is `frost pve` in `playerbots.conf.dist`, so all raid browser mages end up frost. The effect on damage was not measured.
- Bot behavior inside the raid (rotations, positioning) comes from stock mod-playerbots and was not changed here.
- Temporary diagnostic logging and a few WIP commits remain in the history.

## License

mod-playerbots is GPL v2, mod-dungeon-clear is AGPL v3 (per the file headers). Changes follow the license of the repository they modify.