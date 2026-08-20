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

---

# Addendum 2 (Crafting) — Phase C0 verification (C-A1..C-A10)

Verification of `mod-cmangos-ahbot-crafting-addendum_2.md` §1, run **2026-08-19** against
the same reference tree and a **live `acore_world`** (`mysql -u acore`, server up, core
HEAD still `dfae3da` on `Playerbot`). Source citations are relative to
`/home/leo/wow/azerothcore`; every DB claim below shows the query and the row counts it
returned.

**Two rows diverge from the spec and change the design — flagged in the phase summary:**
**C-A3** (AC uses a *direct* recipe→spell encoding, not the assumed `SPELL_EFFECT_LEARN_SPELL`
effect-walk) and **C-A9** (Enchanting *does* have listable pre-WotLK products). The core wins;
the design is adapted minimally as noted.

| #     | Assumption (abridged)                                                        | Result | Where |
|-------|------------------------------------------------------------------------------|--------|-------|
| C-A1  | Reagents as `Reagent[8]`/`ReagentCount[8]`                                    | **CONFIRMED** | see below |
| C-A2  | AC skill-up formula reachable (grey=`TrivialHigh`, yellow=`TrivialLow`, green=mid) | **CONFIRMED** (cite corrected to `PlayerUpdates.cpp`) | see below |
| C-A3  | Recipe item → crafting spell resolution                                      | **CONFIRMED w/ DESIGN CHANGE** (direct trigger-6, not effect-walk) | see below |
| C-A4  | Trainer-taught recipes enumerable by spell id                                | **CONFIRMED** | see below |
| C-A5  | CD recipes detectable via `RecoveryTime`/`CategoryRecoveryTime` ≥ 86 400 000 | **CONFIRMED (fields)** / runtime threshold **DEFERRED to C1** | see below |
| C-A6  | Herb/ore nodes already flow through the type-3 gameobject vector             | **CONFIRMED** | see below |
| C-A7  | Live bot listings iterable in-memory per house (no SQL)                      | **CONFIRMED** | see below |
| C-A8  | Skill-line ids (171/164/333/202/165/197/185/129/755/773)                     | **CONFIRMED** | see below |
| C-A9  | Enchanting has no listable product before WotLK vellums                      | **CONTRADICTED — design correction** | see below |
| C-A10 | Module-local seeded RNG independent of core `urand`                          | **CONFIRMED** | see below |

---

### C-A1 — reagents = `Reagent[8]` / `ReagentCount[8]` — **CONFIRMED**
`src/server/game/Spells/SpellInfo.h:397-398`
`std::array<int32, MAX_SPELL_REAGENTS> Reagent;` / `std::array<uint32, MAX_SPELL_REAGENTS> ReagentCount;`.
`MAX_SPELL_REAGENTS` = **8** at `src/server/shared/DataStores/DBCStructure.h:1675`. Access on the
`SpellInfo` the profession scan already holds: `spell->Reagent[i]` / `spell->ReagentCount[i]`,
`i ∈ [0,8)`, `Reagent[i] > 0` marks a live slot (`int32`, so guard the sentinel).

### C-A2 — AC's own skill-up formula — **CONFIRMED** (spec cite corrected)
Spec cited `Player.cpp`; the function is in **`src/server/game/Entities/Player/PlayerUpdates.cpp`**.
- `Player::UpdateCraftSkill(uint32 spellid)` **:823** resolves the spell's `SkillLineAbilityMap`
  and calls, **:850-858**:
  `UpdateSkillPro(SkillLine, SkillGainChance(SkillValue, TrivialSkillLineRankHigh, (High+Low)/2, TrivialSkillLineRankLow), craft_skill_gain)`.
  This matches the spec exactly: **grey = `TrivialSkillLineRankHigh`, green = midpoint, yellow = `TrivialSkillLineRankLow`.**
- `SkillGainChance(SkillValue, Gray, Green, Yellow)` inline **:749-759**:
  `>= Gray → CONFIG_SKILL_CHANCE_GREY*10`, `>= Green → GREEN*10`, `>= Yellow → YELLOW*10`,
  else `ORANGE*10`. Return is **per-mille** (percent × 10). Stock config defaults:
  grey 0, green 25, yellow 75, orange 100 (i.e. below-yellow crafts skill 100 % of the time,
  grey never). `UpdateSkillPro` **:912** early-returns on `Chance <= 0` (grey case).
- Trivial ranks: `SkillLineAbilityEntry::TrivialSkillLineRankHigh` / `...Low` at
  `src/server/shared/DataStores/DBCStructure.h:1645-1646`.
- **Port note (constraint #7):** reproduce this branch verbatim in the offline simulator —
  read `CONFIG_SKILL_CHANCE_*` defaults into the fixture; do not invent a curve.

### C-A3 — recipe item → crafting spell — **CONFIRMED, with a design change**
`ITEM_CLASS_RECIPE = 9` at `src/server/game/Entities/Item/ItemTemplate.h:300` (3,066 rows:
`SELECT COUNT(*) FROM item_template WHERE class=9;` → **3066**).

The spec assumed a two-hop walk (item on-use spell → `SPELL_EFFECT_LEARN_SPELL` → craft spell).
**AC encodes it directly.** Item on-use spells live in `_Spell Spells[MAX_ITEM_PROTO_SPELLS=5]`
(`ItemTemplate.h:589,615,662`); the learn trigger is `ITEM_SPELLTRIGGER_LEARN_SPELL_ID = 6`
(`ItemTemplate.h:88`, comment: *"used in item_template.spell_2 with spell_id with SPELL_GENERIC_LEARN in spell_1"*).

5-recipe walk (`SELECT entry,name,subclass,spellid_1,spelltrigger_1,spellid_2,spelltrigger_2 FROM item_template WHERE class=9 AND entry IN (...)`):

| recipe item | name | acquisition class | `spellid_1`/`trig` | `spellid_2`/`trig` = **craft spell** |
|---|---|---|---|---|
| 6045  | Plans: Iron Counterweight        | trainer/drop (BS) | 483 / 0 | **7222** / 6 |
| 11167 | Formula: Enchant Boots – L.Spirit| vendor (Ench)     | 483 / 0 | **13687** / 6 |
| 20757 | Formula: Brilliant Mana Oil      | drop (Ench)       | 483 / 0 | **25130** / 6 |
| 44494 | Formula: Enchant Weapon – Lifeward| drop (Ench, WotLK)| 483 / 0 | **44576** / 6 |

Pattern holds across the table: `spellid_1 = 483` (generic "Learning"), `spelltrigger_1 = 0`;
the **craft spell is `spellid_2` where `spelltrigger_2 = 6`**. Coverage
(`SELECT SUM(spelltrigger_2=6), SUM(spellid_1=483), COUNT(*) FROM item_template WHERE class=9`):
**2034 / 2034 / 3066**. The 1032 non-matches are dominated by **subclass 0 = Book**
(943 rows, class-ability Tomes with all-zero spell cols — *not* profession recipes, correctly
excluded); profession subclasses 1–10 are ≈ all trigger-6 (e.g. JC subclass 10: 466/480,
LW subclass 1: 359/395).

**Design adaptation (recorded):** rarity resolver = for craft spell `S`, its recipe item is
`SELECT entry FROM item_template WHERE class=9 AND spelltrigger_2=6 AND spellid_2=S`. Read
`spellid_2` **directly**; do not chase `SPELL_EFFECT_LEARN_SPELL`. Trainer-taught recipes have
**no** recipe item (taught straight off the trainer) → resolve those via C-A4. The taught-spell →
`SPELL_EFFECT_CREATE_ITEM` → product hop is DBC-resident and is already exercised by the base
module's profession scan (3202 items live, per status); re-log per-profession graph counts in C1.

### C-A4 — trainer recipes enumerable by spell id — **CONFIRMED**
`npc_trainer` (`ID`,`SpellID` composite PK; `SHOW COLUMNS`) — **4934** rows; AC's current
`trainer`+`trainer_spell` system also populated (`trainer_spell` **6769** rows). Trainer-taught
craft spells are keyed by spell id in either. Sanity join
(`... FROM npc_trainer nt JOIN (SELECT DISTINCT spellid_2 FROM item_template WHERE class=9 AND spelltrigger_2=6) it ON nt.SpellID=it.spellid_2`):
**173** craft spells are both trainer-listed and recipe-item-taught. Rarity rule: craft spell
present in the trainer table → `TRAINER`; else recipe item in `npc_vendor` → `VENDOR`; else recipe
item in any loot template → `DROP`; else `UNSOURCED` (treat as `DROP`). Prefer `trainer_spell`
(current loader); keep `npc_trainer` as a fallback.

### C-A5 — daily-cooldown recipes — **fields CONFIRMED; threshold check DEFERRED to C1**
`SpellInfo` exposes `RecoveryTime` **:372** and `CategoryRecoveryTime` **:373** (ms), plus
`GetRecoveryTime()` **:544** — `src/server/game/Spells/SpellInfo.h`. Structurally sufficient.
The concrete `≥ 86 400 000 ms` (24 h) test is DBC-resident and cannot be read from `acore_world`
offline (the item-side `spellcooldown_*` columns are the *learn* action, not the craft). **Deferred
to a C1 startup log**, spot-checking known CD spells — Transmute: Arcanite **17187**, Titansteel
Bar **55208**, Moonshroud **56002** — vs non-CD Smelt Copper **2657**, Bronze Bar **2660**, Bolt of
Linen **2963**. **Design note:** transmutes use a *shared category* cooldown, so test
`max(RecoveryTime, CategoryRecoveryTime) >= 86400000`, not `RecoveryTime` alone.

### C-A6 — herb/ore nodes flow through the type-3 GO vector — **CONFIRMED**
`GAMEOBJECT_TYPE_CHEST = 3` (`src/server/game/Miscellaneous/SharedDefines.h:1567`). Nodes are
type-3 chests gated by a gather-skill lock, carrying a `Data1` loot id — so they already ride the
base module's `Data1`/type-3 gameobject vector (base plan §2.2). Verified (`SELECT entry,name,type,Data0,Data1 FROM gameobject_template WHERE entry IN (...)`):
Peacebloom 1618 → loot **1415**, Silverleaf 1617 → **1414**, Copper Vein 1731 → **1502**,
Tin Vein 1732 → **1503**; TBC Felweed 181270 → 18111, WotLK Cobalt Deposit 189978 → 24153. Across
the table `SELECT COUNT(*),SUM(Data1>0) FROM gameobject_template WHERE type=3` → **1349 / 1310**
carry loot. Raw-mat supply for the craft layer already exists; no new GO query needed.

### C-A7 — live bot listings iterable in-memory — **CONFIRMED**
`AuctionHouseObject` (`src/server/game/AuctionHouse/AuctionHouseMgr.h:126`) holds
`AuctionEntryMap _auctionsMap` (`std::map<uint32, AuctionEntry*>`, **:137,158**) exposed via
`GetAuctions()` / `GetAuctionsBegin()/End()` **:141-143** — pure in-memory, no SQL. The base module
already walks it (`BotAuctionCount`). `MarketAnchor` must build a **per-item median cache in one
pass** over the neutral house per sell pass; never per-item queries.

### C-A8 — skill-line ids — **CONFIRMED**
`src/server/shared/SharedDefines.h`: First Aid 129 (**:3125**), Blacksmithing 164 (**:3142**),
Leatherworking 165 (**:3143**), Alchemy 171 (**:3144**), Cooking 185 (**:3151**), Tailoring 197
(**:3155**), Engineering 202 (**:3156**), Enchanting 333 (**:3187**), Jewelcrafting 755 (**:3218**),
Inscription 773 (**:3235**). All ten match the spec's `SkillLine` table.

### C-A9 — "Enchanting has no pre-WotLK listable product" — **CONTRADICTED (design correction)**
False. Enchanting produces `SPELL_EFFECT_CREATE_ITEM` goods well before WotLK. Evidence
(`SELECT entry,name,class,subclass,ItemLevel,RequiredSkill FROM item_template WHERE name IN (...)`):
- **Wizard Oil** 20750 (ilvl 50, subclass 8), **Brilliant Mana Oil** 20748 (ilvl 55) — enchanting
  oils; the latter is taught by Formula spell 25130 walked in C-A3.
- **Runed Arcanite Rod** 16207 (ilvl 58) — enchanting-made rod (vanilla).
The WotLK *scroll* mechanism rides **vellums** — Armor Vellum 38682 / Weapon Vellum 39349
(`RequiredSkill = 333`, ilvl 1–65) — the enchant-on-vellum product is what is WotLK-only.
**Correction:** do **not** blanket-exclude Enchanting pre-WotLK. Gate **SCROLL/vellum-enchant**
products to expansion 2 (§5.1 `SCROLL` category); classify oils/rods/wands by their own product
`ItemLevel`/recipe-source like any other craft. C-A9's sweep sentinel becomes "no *scroll* product
below state 13," not "no enchanting product." Verify by scanning graph output in C1.

### C-A10 — module-local seeded RNG is independent of core `urand` — **CONFIRMED**
`urand`/`irand`/`frand`/`rand_norm` all draw from a single **file-local** `static RandomEngine engine;`
(`src/common/Utilities/Random.cpp:25`, used at `:44` etc.); `RandomEngine` is the core's SFMT wrapper
(`src/common/Utilities/Random.h:71`). A module-owned `std::mt19937` seeded from `Craft.Seed` shares no
state with it. **Constraint #4:** route *all* craft-layer randomness through that one engine when
seeded, and never call the core `urand` family from a seeded path (it would be unseeded and would also
perturb core determinism).

---

## C0 deltas / design decisions carried forward

- **C-A3:** direct `spelltrigger_2 == 6` → `spellid_2` resolver; exclude recipe subclass 0 (Books).
  No `SPELL_EFFECT_LEARN_SPELL` walk. Trainer recipes have no item — resolve rarity via C-A4.
- **C-A5:** CD test is `max(RecoveryTime, CategoryRecoveryTime) >= 86400000`; concrete spot-check
  logged at C1 startup (spells 17187/55208/56002 CD; 2657/2660/2963 non-CD).
- **C-A9:** Enchanting is gated at the **product** level, not the profession level; only
  scroll/vellum products are expansion-2.
- **C-A2:** citation is `PlayerUpdates.cpp`, not `Player.cpp`; formula otherwise as specified.

## Still deferred to runtime (checked via C1 startup log / soak)
- Per-profession × per-rarity graph counts; `MISC` category a minority (C1 acceptance).
- CD-recipe detection spot-check (C-A5 spell ids above).
- Enchanting product scan shows oils/rods at expansion 0/1 and scrolls only at expansion 2 (C-A9).
- `Craft.Seed` determinism: identical seed+config+DB ⇒ identical `craft dump` (C1 deliverable).

---

# Phase C1 — runtime verification (recipe graph, live server)

Built + installed the C1 binary, ran with `Craft.Enable=1 Craft.Seed=12345` against the
live `acore_world` (2026-08-19). Startup log evidence:

```
recipe graph built — 2885 recipes in 0.12s (skipped: noReagent=0 product=239) cooldownRecipes=1
per-profession x rarity (TRAINER/VENDOR/DROP/UNSOURCED):
  Alchemy 75/33/52/33(193)  Blacksmithing 197/78/137/47(459)  Leatherworking 175/168/130/25(498)
  Tailoring 173/93/107/29(402)  Engineering 131/39/60/22(252)  Enchanting 7/11/0/1(19)
  Jewelcrafting 95/206/95/26(422)  Inscription 215/3/0/209(427)  Cooking 29/105/23/15(172)
  First Aid 12/1/1/1(15)  Mining(smelt) 22/0/2/2(26)
categories: FLASK=16 ELIXIR_POT=163 FOOD=172 BAG=42 GEM_CUT=293 SCROLL=0 AMMO=11 GEAR=1397 INTERMEDIATE=152 MISC=639
CRAFT SELFTEST: PASS recipes=2885 available=1317 misc=639
gate sweep state=0  maxExp=0 skill300 ilvl66  => available=1317 | JC=0   Inscription=0   GEM_CUT=0   GEAR=664  INTERMEDIATE=93
gate sweep state=8  maxExp=1 skill375 ilvl115 => available=2045 | JC=393 Inscription=0   GEM_CUT=293 GEAR=981  INTERMEDIATE=136
gate sweep state=13 maxExp=2 skill450 ilvl200 => available=2798 | JC=421 Inscription=427 GEM_CUT=293 GEAR=1312 INTERMEDIATE=152
gate sweep state=18 maxExp=2 skill450 ilvl284 => available=2885 | JC=422 Inscription=427 GEM_CUT=293 GEAR=1397 INTERMEDIATE=152
```

**Acceptance met:** graph 0.12s (<5s); TRAINER dominates, UNSOURCED total ≈410 (low hundreds,
mostly Inscription discovery recipes); MISC 639/2885 = 22 % (minority); JC/Inscription = 0 at
state 0 and unlock at their era (JC→state 8, Inscription→state 13); available count monotone
1317→2045→2798→2885; selftest PASS. Base module unchanged (`ready=true`, craft not wired into
sell/buy).

## C1 findings that adjust the C0 assumptions (carry into C4)

- **C-A5 undercounts cooldown recipes (cooldownRecipes=1).** Transmutes / Titansteel / Moonshroud
  carry their 24 h CD on the *spell category* cooldown store, not on `SpellInfo::RecoveryTime` /
  `CategoryRecoveryTime` (both read 0 for them). Field-based detection catches only recipes with a
  direct recovery time. **C4 fix (§4.5):** also consult the category-cooldown data
  (`sSpellMgr` spell-category cooldowns / `SpellCategoryStore`) keyed by `SpellInfo::GetCategory()`.
- **SCROLL category is empty (SCROLL=0) — enchant scrolls are not create-item spells.** The WotLK
  vellum-scroll is produced by an `SPELL_EFFECT_ENCHANT_ITEM` spell applied to a vellum, so it never
  appears in the `SPELL_EFFECT_CREATE_ITEM` scan. Enchanting's in-graph products are its create-item
  goods (oils/rods, 19 recipes) — consistent with the C-A9 correction. **C4 decision needed:** either
  model enchant-on-vellum as a synthetic recipe (vellum + dust reagents → scroll product) so the
  `SCROLL:20` WotLK demand weight has supply, or drop the SCROLL category. Flagged, not yet resolved.
- **Inscription UNSOURCED=209** are discovery/research-taught recipes (no trainer/vendor/loot row);
  correctly treated as DROP for margins. Within the "low hundreds" tolerance (§2.2).

---

# Phase C2 — cost engine verification

**Offline (`tools/craft-tests/`, the "green before C2" gate):** `CRAFT-TESTS: PASS (22 passed,
0 failed)`. Asserts min(market,make) both directions, multi-output division, transmute-cycle
termination + fallback + `cycleHits>0` + zero depth hits, deep epic chain, margin + cooldown-bonus
math, memoization stability, `NewPass` reset. The engine links with **no core headers** (facade
constraint #3 satisfied).

**In-server hand-check (live graph, state 0, seed 12345; anchor medians from 2127 live bot listings):**
```
bar<-ore : Copper Bar x1 <= Copper Ore x1@19                        | cost/unit=19    (1*19 ✓)
bolt<-cloth: Bolt of Linen x1 <= Linen Cloth x2@56                  | cost/unit=112   (2*56 ✓)
flask<-herb: Flask of the Titans x2 <= Gromsblood x30@990 + Stonescale Oil x10@102
             + Black Lotus x1@4000 + Crystal Vial x1@500            | cost/unit=17610 ((29700+1020+4000+500)/2 ✓)
epic-gear : White Leather Jerkin x1 <= Light Leather x8@91 + Coarse Thread x2@10 + Bleach x1@25
                                                                    | cost/unit=773   (728+20+25 ✓)
full-graph cost pass — priced 1317 products, cycleHits=1 depthHits=0 memo=1745   (no blowups, cycle bounded)
```
Every price reduces to Σ(MatValue×count)/productCount, multi-output `/2` correct on real data, the
one graph cycle is detected and bounded, no recursion blowups over all 1317 available products, no
crashes. **C2 acceptance met.**

## C2 finding — gating leak (assign to C4)

The `[CD]` sample surfaced **Glacial Bag (WotLK, 22-slot) available at `state 0`.** Root cause: BAG
(and consumable) categories are exempt from the ilvl mask (§2.3 rule 4); when such a recipe's
crafting spell has `MinSkillLineRank = 0` (requirement enforced elsewhere) the skill-rank era-bake
(rule 2) also misses it, and it is neither JC nor Inscription (rule 1) — so it bakes to expansion 0.
The deferred **rule 3 (recipe-source drop map)** is exactly what would catch it. **C4 must fix** —
its `Netherweave Bag pre-state-8` sentinel enforces this. Candidate fix: also bake
`recipe.expansion = max(reagent eras)` (a bag using Moonshroud is WotLK) and/or resolve the recipe
item's drop-source map. Cost math is unaffected; this is a mask bug only.

Secondary note: smelting spells (Smelt Copper etc.) classify **UNSOURCED** (their craft spell id is
not in `trainer_spell`), so bars carry DROP production margins — but bars are overwhelmingly
leveling-dumped (LevelingMargin), so the production margin rarely applies. Revisit in C3/C4 if bar
pricing looks off.

---

# Phase C3 — leveling simulator (sell-only) + reagent-era gate (pulled from C4)

**Pure sim layer (`CMangosAHBotCraft.{h,cpp}`) offline-tested:** `CRAFT-TESTS: PASS (37/37)` — added
skill-up bands (C-A2), glut-chooser picks the cheapest-per-skill-up recipe (grey/too-high never
chosen), skill-up advancement bounded by cap, population roll (skills in `[1,cap]`, professions from
weights). All craft RNG flows through the seeded module engine.

**Live 500-session leveling snapshot (state 0, seed 12345):** emergent glut with **nothing named in
config** — Engineering ammo, alchemy/enchant oils, low BS/LW/Tailoring greens, First-Aid anti-venoms,
enchant shards — **100% below craft cost** (priced items), **Layer 3 would-drop = 0**, no crash. Per
the glut-presence probe, all seven named vanilla gluts (**Copper/Bronze/Tin Bar, Bolt of Linen/Woolen
Cloth, Linen/Heavy Wool Bandage**) are available at state 0 (dispersed below ammo's high-`productCount`
raw counts in the top-20 view). Legacy `Items.Profession` is retired when `Craft.Enable=1`;
`Craft.Enable=0` leaves the base seller/buyer byte-identical.

## Reagent-era gate (§2.3 rule 3) — pulled forward from C4 and fully closed

C3 output initially leaked **Netherweave Net (TBC) at state 0** (the C2-flagged class: ilvl/skill-mask
-exempt items using cross-era mats). Root cause found: **TBC/WotLK *instance* maps carry unreliable
`MapEntry::expansionID` (often 0)** in the 3.3.5 client DBC, so a `MIN`-over-sources classification let
one mis-flagged instance drag cloth to Vanilla. Ore was immune (open-world only), which is why it
already gated right.

Fix (in `BuildItemExpansion` + graph `Build`):
1. **Plurality by spawn count**, not MIN — Netherweave's 6305 open-world Outland spawns outvote the
   handful in mis-flagged instances. Vote accumulation is **separate** from the base source-vector
   `MinMerge`, so world-drop gating is unchanged (constraint #1).
2. **Fixpoint propagation through crafted reagents** — a mat's obtain-era = `min(looted era, cheapest
   craftable era)`; iterate so bags/flasks consuming *crafted* cross-era mats gate too.

Verified at state 0: all four raw sentinels classify correctly (Netherweave/Fel Iron = TBC,
Frostweave/Saronite = WotLK) with **0 available consumers**; crafted cross-era sentinels
**Netherweave Bag, Glacial Bag, Flask of the Frost Wyrm all absent (correct)**. This also resolves the
**C2 Glacial Bag leak** and pre-satisfies the **C4 §12 crafted-sentinel** requirement. Gate sweep stays
monotonic (state 0/8/13/18 → 1088/1892/2798/2885 available).

Remaining for later: ammo over-dominates raw counts (high `productCount`); smelted-bar volume is
understated in the *startup* snapshot because some mats have no live median at `OnStartup` (0 matcost
inflates 0-cost recipes) — a measurement/tuning item for **C7**, not a mechanism bug.

---

# Phase C4 — demand-weighted production

Adds §5 production sessions for at-cap crafters (leveling stays for below-cap). Offline tests now
**41/41** (added the production chooser: category weights steer selection, GEAR ilvl-window gates,
override-weight 0 forbids).

**Demand sweep (all-at-cap population, 1500 production sessions/state, seed 12345), by listing share:**
```
state=0  Vanilla: FLASK=11 ELIXIR=13 FOOD=7 BAG=11 AMMO=7 GEAR=8 INTERMEDIATE=36 MISC=3  | GEAR ilvl [41..66]  window[40..66]
state=7  Vanilla: FLASK=7  ELIXIR=12 FOOD=5 BAG=16 AMMO=6 GEAR=0 INTERMEDIATE=44 MISC=6  | GEAR ilvl [70..72]  window[66..92]
state=8  TBC:     FLASK=9  ELIXIR=14 FOOD=5 BAG=15 GEM_CUT=4 GEAR=10 INTERMEDIATE=35 MISC=4 | GEAR ilvl [93..115] window[89..115]
state=13 WotLK:   FLASK=10 ELIXIR=7  FOOD=3 BAG=16 GEM_CUT=7 GEAR=4 INTERMEDIATE=41 MISC=7 | GEAR ilvl [183..200] window[174..200] | CD=4/1recipe
state=18 WotLK:   FLASK=14 ELIXIR=8  FOOD=1 BAG=16 GEM_CUT=6 GEAR=0 INTERMEDIATE=43 MISC=7 | GEAR ilvl [264..264] window[258..284] | CD=4/1recipe
```

**Acceptance:**
- **Category mix shifts per era** ✅ — AMMO is Vanilla-only (weight 0 in TBC/WotLK); **GEM_CUT appears
  only from TBC** (0→4→7→6); FLASK/ELIXIR/FOOD/BAG/GEAR/INTERMEDIATE track their weights.
- **GEAR clusters inside the ilvl window** ✅ — at every state the GEAR ilvl range sits inside
  `[ilvlCap-GearWindow, ilvlCap]` (e.g. state 13: [183..200] ⊂ [174..200]).
- **CD products ≤ §4.5 cap** ✅ — bounded (4 crafts on the detected recipe); mechanism works.
- **Cross-era crafted sentinels absent** ✅ (regression from C3 holds).

**Design decisions / caveats:**
- **Category-weight normalization (refines §5.2):** each category's weight is split across its available
  recipes, so a category's *selection share* tracks the configured weight regardless of recipe count
  (else 40 elixir recipes bury 4 flask recipes → FLASK read 0%). The sweep reports by **listings** (not
  units) because units are dominated by high-`productCount` categories (ammo) and hide the weights.
- **CD detection remains ≈1 recipe (C1/C-A5 limitation, confirmed):** `spell_dbc` (SQL) is a 4518-row
  override subset with zero ≥24h cooldowns, so transmute/Titansteel dailies live only in the real
  `Spell.dbc` and are enforced via triggered-spell/category indirection not exposed on the create-item
  spell's `RecoveryTime`. The §4.5 **cap mechanism is implemented and demonstrably bounds output**; only
  the detected set is small. Refining detection (mapping triggered→trigger spells) is a future item.
- **SCROLL category dropped as inert (C-A9 follow-up):** enchant scrolls are `ENCHANT_ITEM`-on-vellum,
  not create-item spells, so the `SCROLL` demand weight has no supply. Logged at startup; not synthesized.
- **Override table §8.2:** additive `craft_weight`/`craft_margin` columns shipped in
  `data/sql/db-characters/cmangos_ahbot_items_craft.sql` (idempotent `ADD COLUMN IF NOT EXISTS`,
  MariaDB), loaded and applied in the production chooser/margin.

---

# Phase C5 — buyer integration + demand ledger (§6)

Extends the base Phase-6 buyer: sessions credit a rolling **demand ledger** (mats consumed by
crafters, §6.1); the buyer values crafted goods at their **make cost** (§6.3) and ledger-demanded
mats at **MatValue** (§6.2) — both through the SAME cost engine (buyer coherence, #2) — scaled by a
**saturation curve** (§6.4). All gated behind `Craft.Enable`, so with it off the buyer is byte-identical
to the base module (#1). The deferred-buyout vector (base §6.2) is preserved and now feeds both
valuation paths. Offline tests **47/47** (added saturation endpoints: 100% ≤cap, 65% @2cap, 30% @3cap).

**Runtime verification (live Update loop, `Craft.TestCommands=1`, `Chance.Buy=100`, seed 12345).** The
buyer only runs correctly during `Update()` — `OnStartup` fires *before* "World initialized" so buyouts
don't complete there (a test-harness artifact, not a buyer bug; the in-startup attempt was flaky). So
the self-test **creates** synthetic non-bot auctions (`craft testlist`, §8.3) and lets the live buy
passes consume them; verified by polling `auctionhouse`:

```
TEST-A  mat 15994, MatValue 7011, ask 3154/u (< buyer val 6309/u): bought within ~3 buy passes; ledger debited 1000->980.
TEST-B  item 4358 (craftCost 40), ask 25/u, listed 60, DailyCap 20:
        bought EXACTLY 38 then stopped (== predicted 2x-cap saturation cutoff); 22 left unbought.
        BuyPass: queuedBuyouts=39 executed=39 in one pass (deferred-buyout, no iterator invalidation).
```

**Acceptance:**
- Ledger-demanded mat bought within 3 passes + ledger debited ✅
- Saturation: first ~DailyCap near full valuation, tail decays and buying stops at the curve cutoff
  (38 = 2×cap for these params) ✅
- Deferred buyout, no iterator invalidation, no crash ✅
- Buyer coherence: crafted/mat valuations both via the cost engine (`craftCostOf`/`MatValue`) ✅
- `Craft.Enable=0` ⇒ base world-drop buying unchanged (valuation branch gated on `craftVal`) ✅

**Notes:** saturation must engage **within** a single buy pass (buyouts are deferred, so `_ledgerBought`
only updates after the loop) — a per-pass `localBought` counter feeds the in-loop valuation. Cleaned up
leftover test auctions via SQL after verification; live server restored (craft off).

---

# Phase C6 — market texture (§7)

Listing-time texture: stacks (§7.2) spread across a 2-4 rung undercut ladder (§7.1), floored. Pure,
offline-tested (`SplitStacks`, `TexturedListings`); offline **55/55** (stack rules per category,
2-4 rung ladder, floor clamp). `CraftSellPass` now: (1) computes the cheapest existing bot buyout/unit
per item in the house and undercuts it by ×urand(95,99)% (cross-pass §7.1); (2) floors at 100% of mat
cost (producing) / 60% (leveling dumps); (3) splits each listing via `TexturedListings`.

**Texture sample (state 0, seed 12345):**
```
INTERMEDIATE Rough Blasting Powder matcost 12: stacks[20 20 5 15] (full + ragged)  prices 2 rungs  >=floor
FLASK        Flask of the Titans  matcost 16860: stacks[20 20 5 5 5 5] (mixed 20s/5s) prices 2 rungs >=floor
GEAR         Rough Boomstick      matcost 360: 60 singles, prices 698/691/684/649 = 4-rung ladder  >=floor
BAG          Linen Bag            matcost 366: 60 singles, prices 702/694/659/639 = 4-rung ladder  >=floor
AMMO         Crafted Light Shot   matcost 1: full stack (§7.2)
```

**Acceptance:**
- 2-4 rung price ladder on high-volume items ✅ (GEAR/BAG 4 rungs, consumables/intermediates 2)
- No bot listing below its floor ✅ (`aboveFloor=yes` for every category)
- Stack-size mix per §7.2 ✅ (GEAR/BAG singles, AMMO full stacks, FLASK/ELIXIR/FOOD mixed 5s/20s,
  INTERMEDIATE full + ragged)
- Cross-era sentinels still gated; no crash ✅

Design note: the 2-4 price points ARE the ladder (§7.2 "via variance + one undercut step") — variance
jitters the top rung once and each lower rung is one undercut step below, floored; stacks spread across
rungs with no extra per-stack jitter (else a wall of near-dupes instead of a clean ladder).

---

# Phase C7 — measurement, tuning, and the progression sweep

**1. p99 pass time (< 200 ms budget):**
```
pass-time bench (anchors + cost + 4-8 sessions, 200 iters): p50=0.48ms p99=1.01ms max=1.73ms mean=0.52ms
```
**p99 = 1.01 ms — ~200× under budget.** ✅ (DB posting is one batched transaction per pass, not in this
compute figure; the world-thread stall is the compute, which is ~0.5 ms.)

**2. price-to-cost ratio histogram per category (from `craft dump`, state 13):**
```
ratio% [min/mean/max]: FLASK 62/157/328  ELIXIR_POT 63/197/328  FOOD 61/118/294  BAG 63/116/314
                       GEM_CUT 61/116/318  AMMO 58/76/101  GEAR 50/76/288  INTERMEDIATE 33/105/300  MISC 60/84/327
```
Means sit in sensible bands: leveling-heavy categories (AMMO/GEAR ~76%) below cost; production categories
(FLASK 157%, ELIXIR_POT 197%) in the DROP-margin range. Mins ≈ 60% (leveling floor); the 33% outlier is
tiny-matcost integer rounding floored at 1 copper, not a floor violation. ✅

**3. §10.4/§10.5 sweep driver (`tools/craft-soak/run_sweep.py`, states 0/8/13):** `SWEEP: PASS`. Per state
(12k–16k dump rows): all raw + crafted cross-era sentinels gated; **GEM_CUT present iff state ≥ TBC**;
SCROLL inert; state-0 leveling glut share 0.34; median ratio 83–89 % in band; ≥98 % of listings ≥ floor.
Baselines archived under `tools/craft-soak/baselines/`. The driver needs no SOAP — it drives via
config-edit + restart + the module's startup `Craft.DumpFile` CSV.

**4. Equilibrium stock (inflow × mean lifetime):** analytical prediction only — with defaults
(Sessions 4–8/pass, ~2–4 stacks each ⇒ ~15 listings per neutral sell pass at ~120 s cadence, mean
lifetime (2+24)/2 = 13 h) the per-house craft equilibrium ≈ (15/120 s) × 46 800 s ≈ **5.9 k listings**.
Empirical confirmation needs a multi-hour soak; **deferred**, consistent with the base plan's Phase-8
treatment of equilibrium (there is no stock cap — expiry is the sink).

**Tuning notes (carried, not blockers):** ammo over-dominates *unit* counts (high `productCount`) though
its *listing* share is weight-correct; CD detection stays ≈1 recipe (real `Spell.dbc` dailies aren't on
the create-item spell — documented in C4); both are config-tunable and do not affect correctness.
