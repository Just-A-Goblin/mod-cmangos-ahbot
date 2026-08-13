# NOTES-verification.md — Phase 0

Verification of the load-bearing assumptions in `mod-cmangos-ahbot-plan.md` (base
plan §1, A1–A8) and `mod-cmangos-ahbot-progression-addendum_1.md` (§2–§4 anchors),
against the pinned core.

**Reference tree:** `/home/leo/WOW-BACKUP-8-11-26/wow/azerothcore`
**Branch:** `Playerbot` (`mod-playerbots/azerothcore-wotlk`, remote `liyunfan1223/azerothcore-wotlk`), HEAD `dfae3da`
**Date:** 2026-08-13

All paths below are relative to the reference tree. Line numbers are from HEAD `dfae3da`
and may drift; the citation is the symbol, the line is a convenience.

---

## Base plan — A1..A8

| # | Assumption | Result | Citation |
|---|---|---|---|
| A1 | `Loot::AddItem` caps at `MAX_NR_LOOT_ITEMS` = **18** | **CONFIRMED** | `src/server/game/Loot/LootMgr.h:51` — `#define MAX_NR_LOOT_ITEMS 18`. This is the §2.1 truncation trap: accumulate one `Loot` per iteration, harvest, destroy. |
| A2 | `LootTemplate::Process(...)` takes `Player const*` and tolerates a synthetic player | **CONFIRMED** | `LootMgr.h:256` — `void Process(Loot& loot, LootStore const& store, uint16 lootMode, Player const* player, uint8 groupId = 0, bool isTopLevel = true) const;`. `Roll(bool, Player const*, Loot&, LootStore const&)` `LootMgr.h:146`; `AddItem(LootStoreItem const&)` `LootMgr.h:380`. **Resolution:** the module already builds a transient bot `Player` (Phase 2); pass it, sidestepping the `nullptr` question entirely. |
| A3 | AC creature loot keyed by `creature_template.lootid`, not `entry` | **CONFIRMED** | `src/server/game/Entities/Creature/CreatureData.h:220` `uint32 lootid;`, `:221` `pickpocketLootId;`, `:222` `SkinLootId;`. SQL column `lootid int unsigned NOT NULL DEFAULT '0'`. |
| A4 | AC has **no** `spell_template` table; profession items via `sSpellMgr` | **CONFIRMED (architecture)** | No `spell_template` reference in `src/server/game/Spells/SpellMgr.cpp`; spells load from `Spell.dbc`. `SPELL_EFFECT_CREATE_ITEM = 24` at `src/server/shared/SharedDefines.h:790`. **Runtime count check** (low-thousands expected) deferred to the Phase 3 startup log per §2.3. |
| A5 | Loader = `Add` + dir-with-`-`→`_` + `Scripts()` | **CONFIRMED** → `Addmod_cmangos_ahbotScripts()` | Existing modules: `Addmod_ah_botScripts()`, `Addmod_individual_progressionScripts()`. Regex `string(REGEX REPLACE - "_" ...)` in `modules/CMakeLists.txt`. Re-verify against generated `build/modules/gen_scriptloader/static/ModulesLoader.cpp` after first build. |
| A6 | Module SQL auto-applies from `data/sql/db-world/` and `data/sql/db-characters/`, tracked by filename | **CONFIRMED** | `mod-ah-bot/data/sql/db-world/` holds `mod_auctionhousebot.sql` etc. Override table ships under `data/sql/db-characters/` (characters DB, per base plan Phase 7). |
| A7 | `OnItemRoll` / `OnAfterRefCount` fire during `Process()` and tolerate our player | **CONFIRMED** | `LootMgr.cpp:315`, `:1276` `sScriptMgr->OnItemRoll(player, ...)`; `LootMgr.cpp:1420`, `:1707` `sScriptMgr->OnAfterRefCount(player, ...)`. Auction hook `OnBeforeAuctionHouseMgrUpdate()` at `AuctionHouseMgr.cpp:444`. Guard against other modules asserting on the synthetic player; log if hit. |
| A8 | GO chest loot id is `gameobject_template.Data1` where `type = 3` | **CONFIRMED** | `src/server/game/Entities/GameObject/GameObjectData.h` — `//3 GAMEOBJECT_TYPE_CHEST { uint32 lockId; //0 ... uint32 lootId; //1 ... }`. `Data1` == `lootId`. |

---

## Addendum anchors (§2–§4)

| Anchor | Result | Citation |
|---|---|---|
| IP progression = hidden rewarded quest `66000 + state` | **CONFIRMED** | `modules/mod-individual-progression/src/IndividualProgression.cpp:128` `GetAccountProgression(uint32)`; query at `:136-138`: `SELECT cc.quest FROM character_queststatus_rewarded cc JOIN characters c ON cc.guid = c.guid WHERE c.account = {} AND cc.quest BETWEEN {} AND {}`. Bounds `minQuest = 66000 + PROGRESSION_MOLTEN_CORE` (**66001**), `maxQuest = 66000 + PROGRESSION_WOTLK_TIER_5` (**66018**). Highest match − 66000 = state. **This is the single point of dependency on IP internals** — port the query verbatim, comment it, cite this line. |
| `ProgressionState` enum values | **CONFIRMED** | `IndividualProgression.h:224` `PROGRESSION_START = 0`, `:232` `PROGRESSION_PRE_TBC = 8`, `:237` `PROGRESSION_TBC_TIER_5 = 13`, `:238` `PROGRESSION_WOTLK_TIER_1 = 14`. Enum is sparse (11 = TBC_TIER_3 commented out upstream); do not assume contiguity. |
| IP server-wide ceiling `IndividualProgression.ProgressionLimit` (0 = none) | **CONFIRMED** | `IndividualProgression.cpp:888` `progressionLimit = sConfigMgr->GetOption<uint8>("IndividualProgression.ProgressionLimit", 0);`. Clamp effective state to it when non-zero (addendum §3). |
| `MapEntry::expansionID` (0 Vanilla / 1 TBC / 2 WotLK) | **CONFIRMED** | `src/server/shared/DataStores/DBCStructure.h:1360` `struct MapEntry` — `uint32 expansionID; // 63 (0: Vanilla, 1:TBC, 2:WotLK)`; helper `uint32 Expansion() const`. *(Note: addendum cited a slightly different path/line; actual file is under `shared/DataStores`.)* |
| `AreaTableEntry::mapid` (fishing area → map) | **CONFIRMED** | `DBCStructure.h:518` `struct AreaTableEntry` — `uint32 ID; //0`, `uint32 mapid; //1`, `uint32 zone; //2`. `fishing_loot_template.entry` is an area id → `sAreaTableStore.LookupEntry(entry)->mapid` → `sMapStore`. |
| `SkillLineAbilityEntry::MinSkillLineRank` | **CONFIRMED** | `DBCStructure.h:1633` `struct SkillLineAbilityEntry` — `uint32 SkillLine; //1`, `uint32 Spell; //2`, `uint32 MinSkillLineRank; //7`. |
| `sSpellMgr->GetSkillLineAbilityMapBounds(spellId)` | **CONFIRMED** | `src/server/game/Spells/SpellMgr.h:721` `SkillLineAbilityMapBounds GetSkillLineAbilityMapBounds(uint32 spell_id) const;`. |
| §2.3 spell scan member names | **CONFIRMED** | `src/server/game/Spells/SpellInfo.h:348` `uint32 Attributes;`; `:417` `std::array<SpellEffectInfo, MAX_SPELL_EFFECTS> Effects;`; `SpellEffectInfo` `:274` `uint32 Effect;`, `:291` `uint32 ItemType;`. Access `spell->Effects[e].Effect` / `.ItemType`. `sSpellMgr->GetSpellInfo(id)` `SpellMgr.h:741`, `GetSpellInfoStoreSize()` `:766`. |
| Item bind / has-loot flags (Phase 5 filters) | **CONFIRMED** | `src/server/game/Entities/Item/ItemTemplate.h:96` `BIND_WHEN_PICKED_UP = 1`, `:99` `BIND_QUEST_ITEM = 4`; `:149` `ITEM_FLAG_HAS_LOOT = 0x00000004`. |

---

## Deltas from the plan documents

- **DBCStructure.h path.** The addendum cites `DBCStructure.h` line numbers as if in `src/server/game/DataStores/`; the file lives at **`src/server/shared/DataStores/DBCStructure.h`** and line numbers differ. Structures and field order are otherwise as described.
- **IP query bounds are enum-derived, not literals.** The addendum shows `BETWEEN 66001 AND 66018`; the source derives these from `PROGRESSION_MOLTEN_CORE`/`PROGRESSION_WOTLK_TIER_5`. Values match. The port hardcodes 66001/66018 with a comment citing `IndividualProgression.cpp:132-133`.

## Deferred to runtime (need a running server / DB)

These cannot be confirmed by source reading and are checked via startup logging or a
scratch worldserver (base plan Phase 0.2, Phase 8):

- A4 profession-scan count is in the low thousands on a stock WotLK DB.
- All nine loot-id vectors non-empty on stock `acore_world` (Phase 3 acceptance).
- CSV dump of AH contents (`item_id, quality, class, subclass, item_level, stack, bid, buyout, owner`).
- Addendum acceptance: at `Progression.Static = 0` every resolvable source maps to `expansionID == 0`; spot-check Outland/Northrend loot ids absent.
