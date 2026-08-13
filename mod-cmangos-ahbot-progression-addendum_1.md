# Addendum: Progression Gating for `mod-cmangos-ahbot`

**Applies to:** `mod-cmangos-ahbot-plan.md`
**Companion module:** `ZhengPeiRu21/mod-individual-progression` (IP)
**Status:** amends Phases 3, 4, 5, 7 and §5 of the base plan; adds Phase 3.5.

---

## 1. The problem

AzerothCore's `acore_world` contains every item, creature, and loot table from all three expansions. Without gating, a character at `PROGRESSION_START` sees Netherweave Cloth, Frostweave Bandages, Saronite Ore, and Outland greens in the auction house on day one. That is a worse immersion break than any pricing or volume error, and it defeats the point of running IP.

**The item-level cap alone does not fix this.** Netherweave Cloth is item level 55; Frostweave Cloth is 70. A vanilla-appropriate ilvl ceiling passes both. Trade goods — the bulk of what a loot-simulation bot lists — are almost entirely invisible to level-based filtering.

Gating therefore has to happen at the **loot source**, which is exactly where a flow model wants it. This is a place where the CMaNGOS architecture is a better host for the feature than `mod-ah-bot` would have been: the module already enumerates loot sources into id vectors at startup, so gating is a filter on those vectors, not a new subsystem.

---

## 2. Reading progression without depending on IP

IP persists progression as **hidden rewarded quests**, one per state, with quest id `66000 + state`. `IndividualProgression::GetAccountProgression()` reads them with a plain query:

```sql
SELECT cc.quest FROM character_queststatus_rewarded cc
JOIN characters c ON cc.guid = c.guid
WHERE c.account = ? AND cc.quest BETWEEN 66001 AND 66018;
```

The highest matching quest id minus 66000 is the account's progression state.

**Use this query, not IP's headers.** Doing so gives the module:

- no compile-time dependency on IP, so no module build-order coupling and no breakage when IP refactors
- correct behaviour when IP is absent (query returns nothing → state 0 → configure `Progression.Enable = 0` or a static cap)
- no need for a `Player*`, which the bot does not have for arbitrary accounts

Record in `NOTES-verification.md` that this encoding is confirmed at `IndividualProgression.cpp` L128-149, and note that it is the one piece of IP internals this module depends on. If IP ever changes the encoding, this query is the single point of failure — comment it accordingly.

### `ProgressionState` values (from `IndividualProgression.h` L227-247)

```
0  START            7  NAXX40           14  WOTLK_TIER_1
1  MOLTEN_CORE      8  PRE_TBC          15  WOTLK_TIER_2
2  ONYXIA           9  TBC_TIER_1       16  WOTLK_TIER_3
3  BLACKWING_LAIR  10  TBC_TIER_2       17  WOTLK_TIER_4
4  PRE_AQ          12  TBC_TIER_4       18  WOTLK_TIER_5
5  AQ_WAR          13  TBC_TIER_5
6  AQ
```

Note `11` (TBC_TIER_3 / Zul'Aman) is commented out upstream. Treat the enum as sparse; do not assume contiguity.

---

## 3. Whose progression?

The auction house is shared; IP progression is per character. This needs an explicit policy, exposed as config:

| `Progression.Source` | Behaviour | When to use |
|---|---|---|
| `0` static | Use `Progression.Static` verbatim | Deterministic; good for testing and for operators who want a fixed era |
| `1` account (**default**) | Max progression across all characters on `Progression.AccountId` | Solo or single-household server — the intended case |
| `2` highest online | Max across online characters, excluding bot accounts | Multiplayer |

**`Progression.AccountId` is the human player's account, not `CMangosAHBot.Account`.** The bot's own character has no progression and would pin the AH at state 0 forever. Make the config comment explicit about this — it is an easy and silent misconfiguration.

**Playerbots interaction (important for this stack).** With 1,600–2,000 random bots and IP's `SyncBotsProgressionToLeader()`, mode `2` will sample bot characters unless they are excluded. IP exposes `isBotAccount()`, but since this module deliberately avoids linking IP, exclude by account id range or by a configured `Progression.ExcludeAccounts` list instead. Default to mode `1` and sidestep the problem.

Also honour IP's own server-wide ceiling, `IndividualProgression.ProgressionLimit` (0 = none): read it from the shared config and clamp the effective state to it. Otherwise the AH can run ahead of content the server has capped.

---

## 4. Gating model

Three layers, applied in order. Layer 1 does the heavy lifting.

### Layer 1 — Expansion gate on loot sources (primary)

Every loot source resolves to a map, and `MapEntry::expansionID` (`DBCStructure.h` L1344 — `0` Vanilla, `1` TBC, `2` WotLK) gives its expansion directly. Filter each id vector at build time and rebuild on progression change.

State → maximum allowed expansion, derived from IP's own tier comments (`PRE_TBC` unlocks Karazhan/Gruul/Magtheridon; `TBC_TIER_5` unlocks WotLK Naxx/EoE/OS):

```
state <  8   →  expansion 0        (Vanilla only)
state >= 8   →  expansion 1        (+ TBC)
state >= 13  →  expansion 2        (+ WotLK)
```

Make these two thresholds config keys rather than constants — IP's tier semantics are the kind of thing that shifts between releases.

Per-source resolution:

| Source | Resolve expansion via |
|---|---|
| Creature (rank 0–4) | `creature_template.lootid` → spawn rows in `creature` → `map` → `sMapStore` |
| Skinning | `creature_template.skinloot` → same join as above |
| Gameobject | `gameobject_template.Data1` (type 3) → spawn rows in `gameobject` → `map` → `sMapStore` |
| Fishing | `fishing_loot_template.entry` **is an area id** → `sAreaTableStore.LookupEntry(entry)->mapid` (`DBCStructure.h` L521) → `sMapStore` |
| Disenchant | `disenchant_loot_template.entry` is a `DisenchantID` — no map. See below. |
| Profession items | No map. See Layer 2b. |

**Creatures and gameobjects with no spawn row** (summons, script-spawned, event-only) cannot be resolved. Exclude them and log the count. Including them by default is the wrong call: it lets instance-summoned Outland adds leak vanilla-side.

**Disenchant** is gated by item level instead, which happens to be exactly right — enchanting materials tier with the ilvl of the items they come from:

```sql
SELECT DISTINCT DisenchantID FROM item_template
WHERE DisenchantID > 0 AND ItemLevel <= <cap>;
```

This naturally excludes Void Crystals and Abyss Crystals at vanilla progression without any expansion mapping.

### Layer 2 — Level and skill caps (tier refinement within an expansion)

**2a. Item level / required level.** The base plan already ports CMaNGOS's `m_maxItemLevel` / `m_maxRequiredLevel` constraint (base plan Phase 4.3). Drive those from progression instead of a static config. Starting values — **verify and tune, these are approximations, not gospel**:

```
state:  0   1   2   3   4   5   6   7    8    9   10   12   13   14   15   16   17   18
ilvl:  66  71  71  76  76  81  81  92  115  133  141  154  200  226  245  264  277  284
```

Vanilla anchors: T1 ≈ 66, T2 ≈ 76, T2.5 ≈ 81, T3 ≈ 92. WotLK anchors: T7 ≈ 200-213, T8 ≈ 219-226, T9 ≈ 232-245, T10 ≈ 251-264, RS ≈ 271-277. Expose the whole row as a config string so it can be retuned without a rebuild.

**2b. Profession items by skill rank.** Crafted items have no map and no useful ilvl signal (Netherweave Bag is ilvl 1). Gate them by the crafting spell's required skill, which is precise and available: for each candidate spell, `sSpellMgr->GetSkillLineAbilityMapBounds(spellId)` (`SpellMgr.h` L722) yields `SkillLineAbilityEntry::MinSkillLineRank` (`DBCStructure.h` L1606). Cap it:

```
expansion 0  →  MinSkillLineRank <= 300
expansion 1  →  MinSkillLineRank <= 375
expansion 2  →  MinSkillLineRank <= 450
```

This is a clean expansion gate for the entire profession path and should be applied in the Phase 3 enumeration (base plan §2.3), not at listing time.

### Layer 3 — Post-generation safety net

After the accumulator is built and before pricing, drop any item whose `ItemLevel` or `RequiredLevel` exceeds the current caps. Layers 1 and 2 should already have prevented this; anything caught here indicates a hole in the source gating. **Count these separately and log the count at debug level** — a non-zero, non-decreasing counter is the signal that a source mapping is wrong.

---

## 5. Refresh and lifecycle

- Cache the effective progression state and the filtered id vectors together. Recompute both only when the state changes.
- Poll for changes every `Progression.RefreshInterval` seconds (default 300) in the sell pass, mirroring the lazy-refresh pattern the CMaNGOS fork already uses for its dynamic level cap.
- **Do not purge existing listings when progression advances.** Progression is monotonic, so the cap only rises; existing listings stay valid and expire naturally.
- When an operator *lowers* a static cap, existing over-cap listings persist until expiry. Document this and point at `.cmahbot rebuild` as the way to force a reset.
- Log every progression transition at info level with the old and new state and the resulting vector sizes. This is the single most useful diagnostic for "why did the AH change."

---

## 6. New config keys

Append to `conf/cmangos_ahbot.conf.dist`:

```
CMangosAHBot.Progression.Enable          = 1
CMangosAHBot.Progression.Source          = 1     # 0 static, 1 account, 2 highest online
CMangosAHBot.Progression.AccountId       = 0     # PLAYER's account for Source=1 — NOT the bot account
CMangosAHBot.Progression.Static          = 0     # used when Source=0
CMangosAHBot.Progression.ExcludeAccounts = ""    # comma-separated, for Source=2 with playerbots
CMangosAHBot.Progression.RefreshInterval = 300

# Expansion unlock thresholds (IP ProgressionState values)
CMangosAHBot.Progression.TbcAtState      = 8
CMangosAHBot.Progression.WotlkAtState    = 13

# Item level cap per state, index = ProgressionState 0..18
CMangosAHBot.Progression.ItemLevelCaps   = "66,71,71,76,76,81,81,92,115,133,141,141,154,200,226,245,264,277,284"

# Profession skill rank caps per expansion
CMangosAHBot.Progression.SkillCaps       = "300,375,450"

# Exclude unresolvable (unspawned) creatures and gameobjects
CMangosAHBot.Progression.ExcludeUnspawned = 1
```

Note the `ItemLevelCaps` list has 19 entries covering states 0–18 inclusive; index 11 is a placeholder for the commented-out TBC_TIER_3 and should duplicate index 10.

---

## 7. Amendments to the base plan

### New — Phase 3.5: Progression resolution and source classification
*(insert between base Phases 3 and 4)*

1. Implement `GetEffectiveProgression()` per §3, including the `IndividualProgression.ProgressionLimit` clamp.
2. Build the source→expansion maps per §4 Layer 1. Do this **once at startup** into a `lootId → expansion` lookup; do not re-query per pass.
3. Implement vector filtering keyed on effective progression, plus the disenchant ilvl query and the profession skill-rank cap.
4. Log, per source, `total ids / allowed at current state / excluded unresolvable`.

**Acceptance:** at a forced `Progression.Static = 0`, every creature/gameobject/fishing/skinning id resolves to `expansionID == 0`; spot-check five known Outland creature loot ids and five Northrend ones are absent. At `Static = 13`, all three expansions present.

### Amend Phase 3
Add the `MinSkillLineRank` cap to the profession enumeration described in base plan §2.3.

### Amend Phase 4
`AddLootToItemMap()` iterates the **filtered** vectors. Level constraints (step 4.3) now come from the progression-derived caps rather than static config.

### Amend Phase 5
Add the Layer 3 safety-net filter and its counter before pricing.

### Amend Phase 7
Add commands: `.cmahbot progression` (report effective state, source, resulting caps, per-source vector sizes) and `.cmahbot progression refresh` (force recompute).

### Amend Phase 8
Add a progression sweep to measurement: run the composition CSV at states 0, 7, 8, 13, and 18, and confirm the item mix shifts as expected and that no cross-expansion trade goods appear below their unlock state.

---

## 8. Additional acceptance criteria

- [ ] At `PROGRESSION_START`, zero TBC or WotLK trade goods appear in the AH over a 2-hour soak — specifically check Netherweave Cloth (21877), Frostweave Cloth (33470), Fel Iron Ore (23424), Saronite Ore (36912), Void Crystal (22450)
- [ ] Advancing the player account past `PRE_TBC` causes Outland sources to appear within one `RefreshInterval`
- [ ] Layer 3 safety-net counter stays at or near zero; any sustained growth is investigated as a source-mapping bug
- [ ] Module functions correctly with IP **not** installed (progression query returns empty → state 0; `Progression.Enable = 0` disables gating entirely)
- [ ] `.cmangos ahbot progression` output matches what IP's own `.ip` / progression display reports for the same account

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| IP changes the `66000 + state` quest encoding | Single documented query; comment it and cite `IndividualProgression.cpp` L128-149; add a startup sanity log of the raw result |
| Spawn-map join is expensive on a large `creature` table | Build the lookup once at startup, not per pass; measure in Phase 8 |
| Creatures spawned only by scripts get excluded, thinning vanilla loot | Log the excluded count; if it is large, add an override list rather than defaulting them to vanilla |
| Mode `2` samples playerbot characters | Default to mode `1`; document `ExcludeAccounts` |
| Item level caps are approximations | Config-driven; tune in Phase 8 against actual tier data |
| `mod-ah-bot` also installed and ungated | Its seller must be off (`AuctionHouseBot.EnableSeller = 0`), or it will list ungated items alongside — worth an explicit README warning |
