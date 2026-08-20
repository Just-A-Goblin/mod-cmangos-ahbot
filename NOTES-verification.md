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
