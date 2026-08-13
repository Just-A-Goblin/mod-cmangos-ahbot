# Plan: `mod-cmangos-ahbot` — a new AzerothCore module

**Deliverable:** a standalone AzerothCore module reimplementing CMaNGOS's loot-simulation auction house bot.
**Core:** AzerothCore WotLK (`mod-playerbots/azerothcore-wotlk`, branch `Playerbot`)
**Reference implementation:** `cmangos/mangos-classic`, `src/game/AuctionHouseBot/`
**Supersedes:** any earlier plan that forked `azerothcore/mod-ah-bot`.

---

## 0. Read this before writing any code

### What this module is

A port of CMaNGOS's AHBot **mechanism**, not its output. The two existing bots differ fundamentally:

- **`mod-ah-bot`** enumerates `item_template`, filters it, buckets it into 14 quality bins, and fills toward a quota. Every surviving item is equally likely. Drop rates play no role. It is a *stock controller*.
- **CMaNGOS AHBot** executes the server's real loot tables — the same `LootTemplate::Process()` path that runs when a mob dies — and lists whatever falls out. Rarity, stack sizes, and item mix are inherited from the world DB for free. It is a *flow simulator*.

No amount of quota, price, or `ItemsPerCycle` tuning reproduces a drop-rate-weighted distribution, because the quota system has no access to drop rates. Attempts to tune `mod-ah-bot` into behaving like CMaNGOS have already failed for this reason. The generative mechanism has to be built, not configured.

### Why a new module rather than a fork

Roughly 150 lines of `mod-ah-bot` are reusable here (auction creation, transient `Player` construction, mail suppression, the update hook) out of about 5,000. Everything else — `InitializeBins()`, the bins, quota math, `SetPercentages`/`CalculatePercents`, the 60-column `mod_auctionhousebot` table, `Inc`/`DecItemCounts`, market-price learning — is machinery for a model this module does not use. A fork carries all of it, initializes it every startup, and rebases forever against an upstream whose architecture it has bypassed.

The decisive reason, though, is **buyer coherence.** CMaNGOS's seller and buyer are two halves of one idea: both call `CalculateBuyoutPrice()`, so the bot lists at its intrinsic valuation and bids up to that same valuation × `Buy.Value`. `mod-ah-bot`'s buyer computes from an unrelated config path. Grafting a CMaNGOS seller onto it yields a bot that prices its own goods one way and values everyone else's another — and on a solo server, where the bot is most of the market, that inconsistency *is* the economy. Porting `Buy()` costs about 60 lines here and falls out of code the seller already needs.

`mod-ah-bot` can remain installed alongside this module with its seller and buyer disabled, giving a zero-cost fallback. Use a **separate bot account** for each so the two mail scripts never see each other's GUIDs.

### Scope

**In scope:** loot-simulation seller; CMaNGOS-equivalent intrinsic pricing; CMaNGOS-equivalent buyer; probabilistic flow volume control with expiry as the sink; per-item override table; GM commands; cadence matching.

**Out of scope:** market-price learning (CMaNGOS has no analogue and it is near-inert on a solo server — see §7); quota/setpoint filling of any kind; anything that touches `mod-ah-bot`'s files.

### Non-goals

- **Ownerless auctions.** CMaNGOS lists with `owner = 0` and identifies its own listings via `if (!entry->owner)`. AzerothCore's `AuctionHouseMgr`, mail, notification, and expiry paths assume a real owner GUID; an empty `ObjectGuid` risks null derefs across core code this module does not control. Keep bot-character ownership and identify own auctions by GUID membership. The mail script destroying bot-addressed mail approximates CMaNGOS's gold sink closely enough. Document this as an accepted economic delta, not a defect.
- Multi-bot support. CMaNGOS is a singleton; mirror that. One bot character.

---

## 1. Assumptions to verify first (Phase 0)

Do not skip these. Several are load-bearing and at least three differ from CMaNGOS in ways that silently produce wrong output rather than errors.

| # | Assumption | How to verify | If wrong |
|---|---|---|---|
| A1 | `Loot::AddItem` caps at `MAX_NR_LOOT_ITEMS` (**18** in `LootMgr.h`) | grep the constant | See §2.1 — the single biggest porting trap |
| A2 | `LootTemplate::Process(Loot&, LootStore const&, uint16 lootMode, Player const* player, uint8 groupId, bool isTopLevel)` tolerates `player == nullptr` | read `LootStoreItem::Roll` and `Loot::AddItem`; both take `Player const*` | Pass the transient bot `Player` the module already constructs |
| A3 | AC creature loot is keyed by `creature_template.lootid`, **not** by `entry` | inspect `creature_template.sql` | See §2.2 |
| A4 | AC has **no** `spell_template` table | run the CMaNGOS profession query against `acore_world` | See §2.3 |
| A5 | Module loader function name is `Add` + directory name with `-`→`_` + `Scripts()` | `modules/CMakeLists.txt` L153-157 | Rename per whatever the regex actually produces |
| A6 | Module SQL auto-applies from `data/sql/db-world/` and `data/sql/db-characters/`, tracked by filename in the `updates` table | compare against `mod-ah-bot/data/sql/db-world/mod_auctionhousebot.sql` | Ship SQL as a manual import step in the README |
| A7 | `sScriptMgr->OnItemRoll(...)` / `OnAfterRefCount(...)` fire during `Process()` and tolerate our synthetic player | grep `LootMgr.cpp` | Guard; log if another installed module asserts |
| A8 | AC gameobject chest loot id is `gameobject_template.Data1` where `type = 3` | inspect `gameobject_template.sql` | Adjust the query in §2.2 |

**Deliverable:** `NOTES-verification.md` in the repo recording each result with file:line citations. Everything downstream depends on it.

---

## 2. Known porting traps

### 2.1 The 18-item loot cap — critical

CMaNGOS accumulates into a single `Loot` object:

```cpp
std::unique_ptr<Loot> loot = std::make_unique<Loot>(LOOT_DEBUG);
for (uint32 repeat = urand(cfg[2], cfg[3]); repeat > 0; --repeat)
    lootTable->Process(*loot, ...);          // accumulate into ONE Loot
```

A direct translation **silently truncates at 18 items**, because AC's `Loot::AddItem` enforces `lootItems.size() < MAX_NR_LOOT_ITEMS`. At CMaNGOS-like tuning (`30,40` lootings per source) that discards the overwhelming majority of generated items, biased toward whatever rolls last — producing output that looks structurally plausible and is compositionally wrong. This is the most likely cause of previous unsatisfactory results.

**Fix:** construct a **fresh `Loot` per iteration**, `Process()` once, harvest `loot.items` into the accumulator map, destroy, repeat. Higher allocation churn, obviously correct. Do not reimplement `LootTemplate`'s group/reference traversal to avoid the churn unless Phase 8 measurement proves it necessary.

### 2.2 Loot-template ID queries must be rewritten, not copied

CMaNGOS uses the creature *entry* as the loot id; AzerothCore uses a separate `lootid` column. Copying the SQL verbatim returns plausible-looking rows that resolve to the wrong tables or to nothing.

| Source | CMaNGOS | AzerothCore replacement |
|---|---|---|
| Creature (rank 0–4) | `SELECT entry FROM creature_template WHERE rank = N AND entry IN (SELECT entry FROM creature_loot_template)` | ``SELECT DISTINCT lootid FROM creature_template WHERE `rank` = N AND lootid > 0`` |
| Skinning | `SELECT DISTINCT entry FROM skinning_loot_template` | `SELECT DISTINCT skinloot FROM creature_template WHERE skinloot > 0` — cross-check row count against the direct table query |
| Disenchant | `SELECT DISTINCT entry FROM disenchant_loot_template` | unchanged |
| Fishing | `SELECT DISTINCT entry FROM fishing_loot_template` | unchanged |
| Gameobject | nested via `data1` + `spawntimesecsmax` | `SELECT DISTINCT Data1 FROM gameobject_template WHERE type = 3 AND Data1 > 0` (optionally restrict to ids present in `gameobject`) |
| Vendor items | `SELECT item FROM npc_vendor` | unchanged |

Rank values match on both cores: `0` normal, `1` elite, `2` rare elite, `3` world boss, `4` rare.

Log every list's row count at startup. A silently empty list means that loot source contributes nothing and the AH quietly skews.

### 2.3 Profession items have no SQL equivalent

CMaNGOS enumerates crafted items via `spell_template`, a MaNGOS world-DB table **AzerothCore does not have** — spells come from `Spell.dbc` via `sSpellMgr`. Replace with an in-memory scan preserving the same bitmask semantics (`32` and `65536` become `0x20` and `0x10000`):

```cpp
for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
    if (!spell)
        continue;
    if (!(spell->Attributes & 0x20) || !(spell->Attributes & 0x10000))
        continue;
    for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
        if (spell->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM && spell->Effects[e].ItemType)
            m_professionItems.push_back(spell->Effects[e].ItemType);
}
```

Confirm `SpellEffectInfo` member names against `src/server/game/Spells/SpellInfo.h` before compiling. Log the count: low thousands is expected on a stock WotLK DB. Single digits means those attribute bits mean something different on WotLK and the enumeration needs re-deriving from `skill_line_ability`.

### 2.4 Cadence mismatch — must be compensated

CMaNGOS ticks every **20 s** (`WUPDATE_AHBOT`, `World.cpp` L1366) and rotates `m_houseAction` through 0–5, so each of the three houses gets one sell pass and one buy pass **every 120 s**.

AzerothCore's `OnBeforeAuctionHouseMgrUpdate` fires from `AuctionHouseMgr::Update()`, whose `_updateIntervalTimer` is **60 s** (`AuctionHouseMgr.cpp` L38). A naive one-rotation-step-per-hook gives each house a sell pass every 6 minutes — one third of CMaNGOS's rate, so the AH fills three times too slowly and settles at a third the equilibrium size.

**Fix:** run **three internal rotation steps per hook** (60 s ÷ 20 s), preserving both the per-house 120 s period and the meaning of every `Loot.*` tuple. Make the divisor a config key (`CMangosAHBot.TickCompensation`, default 3) so it survives any future change to the core's auction interval.

### 2.5 Transactions and performance

Vanilla-installer-equivalent tuning (`90,100,30,40` for normal creatures alone) implies roughly 3,000–4,000 `Process()` calls per sell pass. `Sell()` runs on the world thread. Measure in Phase 8; do not pre-optimize. Batch auction creation into one `CharacterDatabase` transaction per pass (or chunks of ~50) rather than one per auction. Mitigations if needed, in order: lower the multipliers; amortise generation across ticks via a pending queue; cap wall-clock per pass and resume.

---

## 3. Module layout

Directory name determines the loader function (A5), so it must be exact.

```
modules/mod-cmangos-ahbot/
├── include.sh                                  # empty marker file
├── README.md
├── NOTES-verification.md                       # Phase 0 output
├── conf/
│   └── cmangos_ahbot.conf.dist
├── data/sql/db-characters/
│   └── cmangos_ahbot_items.sql
└── src/
    ├── cmangos_ahbot_loader.cpp                # Addmod_cmangos_ahbotScripts()
    ├── CMangosAHBot.h / .cpp                   # Update/Sell/Buy/Rebuild/pricing
    ├── CMangosAHBotConfig.h / .cpp             # conf + loot id vectors + value matrix + overrides
    ├── CMangosAHBotWorldScript.cpp             # OnStartup / OnAfterConfigLoad
    ├── CMangosAHBotAuctionHouseScript.cpp      # OnBeforeAuctionHouseMgrUpdate + notification suppression
    ├── CMangosAHBotMailScript.cpp              # destroy mail addressed to the bot
    └── CMangosAHBotCommands.cpp                # .cmahbot ...
```

No root `CMakeLists.txt` — AzerothCore globs module sources automatically.

**Loader:**

```cpp
void Addmod_cmangos_ahbotScripts()
{
    new CMangosAHBot_WorldScript();
    new CMangosAHBot_AuctionHouseScript();
    new CMangosAHBot_MailScript();
    new CMangosAHBot_CommandScript();
}
```

**Class shape:** one singleton `CMangosAHBot` mirroring the CMaNGOS class — `m_houseAction`, the ten loot-config tuples, the ten id vectors, the value matrix, `m_itemData`. Three `AHConfig`-style per-house structs are *not* wanted; CMaNGOS keeps one config and rotates the house index, and the whole point is to mirror it.

---

## 4. Phased task list

Each phase ends in a compiling, runnable server. Do not proceed past a failed acceptance check.

### Phase 0 — Verification
1. Verify A1–A8; write `NOTES-verification.md` with file:line citations.
2. Stand up a scratch worldserver you can restart quickly; confirm you can dump AH contents to CSV (`item_id, quality, class, subclass, item_level, stack, bid, buyout, owner`).

**Acceptance:** notes file complete; CSV dump works.

### Phase 1 — Module scaffold
1. Create the tree in §3 with empty script classes that log on construction.
2. `conf/cmangos_ahbot.conf.dist` with `CMangosAHBot.Enable = 0` only; load via `sConfigMgr->GetOption<>`.
3. Verify the generated `ModulesLoader.cpp` calls your loader (check the build dir, don't assume).

**Acceptance:** clean build; startup log shows all four scripts constructed and the conf value read.

### Phase 2 — Bot identity and lifecycle
1. Config: `CMangosAHBot.Account`, `CMangosAHBot.GUID`. Resolve the character at `OnStartup` (`SELECT guid FROM characters WHERE account = ?`); error clearly and stay disabled if unset or unresolvable.
2. Per-tick transient `WorldSession` + `Player` + `ObjectAccessor::AddObject` / `RemoveObject`, modelled on `mod-ah-bot`'s `Update()` — this is one of the few pieces genuinely worth copying.
3. Mail script: destroy mail addressed to the bot GUID (`deleteMailItemsFromDB = true`, `sendMail = false`).
4. Auction house script: suppress `sendNotification` and `updateAchievementCriteria` for bot-owned auctions.
5. Wire `OnBeforeAuctionHouseMgrUpdate` → `sCMangosAHBot->Update()`, gated on `Enable`.

**Acceptance:** with `Enable = 1` the tick fires every 60 s, the transient player is created and torn down cleanly, no leaks or warnings over a 30-minute run.

### Phase 3 — Loot template enumeration
1. `FillIdVectorFromQuery()` (port of `FillUintVectorFromQuery`).
2. Populate all nine id vectors using the **AzerothCore** queries from §2.2.
3. Profession scan per §2.3.
4. Vendor item set from `npc_vendor`.
5. Log every vector size at startup; record in `NOTES-verification.md`.

**Acceptance:** all nine non-empty on a stock `acore_world`. Rank-3 and rank-4 lists will be small — expected; empty is not.

### Phase 4 — Loot simulation
1. `ParseLootConfig()` — four-int tuples, **negative `minSources` permitted** (CMaNGOS uses negatives to mean "usually nothing this pass").
2. `AddLootToItemMap()` per §2.1: fresh `Loot` per iteration, harvest `loot.items`, accumulate `itemMap[itemId] += count`.
3. Level constraints: skip when `RequiredLevel > maxRequiredLevel || ItemLevel > maxItemLevel`.
4. Profession items with CMaNGOS's quality-decay roll: skip when `urand(0, convertEnumToFlag(quality) - 1) > 0` (white 100%, green 50%, blue 25%, purple 12.5%).

**Acceptance:** with a debug dump and no auction creation, one simulated pass yields a quality histogram dominated by white/green with a long thin tail into blue/purple. Purple rivalling green means the decay roll or the rank lists are wrong.

### Phase 5 — Pricing and auction creation
1. Port `CalculateBuyoutPrice()` **literally**:
   - base = `BuyPrice`; if `BuyPrice == 0` or `BuyPrice / SellPrice > 5`, use `SellPrice * (quality <= NORMAL ? 4 : 5)`
   - × per-quality-per-class percentage from the value matrix (or `100` if vendor-sold and `Value.Vendor` is on)
   - ÷ 100
2. Port `ValueWithVariance()`.
3. Filters: skip `BIND_WHEN_PICKED_UP` and `BIND_QUEST_ITEM`; skip items flagged as containing loot (verify AC's flag name); skip classes zeroed in the matrix; skip blacklisted overrides.
4. Split accumulated counts into `GetMaxStackSize()` stacks; bid = `buyout * urand(Bid.Min, Bid.Max) / 100`; duration = `urand(Time.Min, Time.Max)` hours.
5. Create auctions — `GenerateAuctionID`, `item->SaveToDB`, `sAuctionMgr->AddAItem`, `auctionHouse->AddAuction`, `auctionEntry->SaveToDB` — owner = bot GUID. Batch per §2.5.
6. Gate the pass on `urand(0, 99) < Chance.Sell`.
7. Implement the rotation and tick compensation from §2.4.

**Acceptance:** AH fills over ~30 minutes. Hand-check 10 items (grey trade good, white consumable, green BoE, blue BoE, a stackable herb, an epic) — prices within the same order of magnitude as CMaNGOS for the equivalent item. Per-house sell period measured at ~120 s.

### Phase 6 — Buyer
1. Port CMaNGOS's buy branch: iterate the house's auctions, skip bot-owned ones, value each at `CalculateBuyoutPrice() × count × Buy.Value / 100`, and either `UpdateBid()` or buy out when the ask is below that valuation.
2. Preserve the deferred-buyout pattern — CMaNGOS collects buyout targets into a vector and executes them *after* iterating, because buying mutates the auction map and invalidates the iterator. **This is a real crash if skipped.**
3. Gate on `urand(0, 99) < Chance.Buy` in the buy half of the rotation.

**Acceptance:** bot bids on and buys player-listed items priced below its valuation; ignores its own; no iterator invalidation over a 1-hour soak with active listing churn.

### Phase 7 — Overrides, commands, rebuild
1. `data/sql/db-characters/cmangos_ahbot_items.sql`:
   `item INT UNSIGNED PRIMARY KEY, value INT UNSIGNED, add_chance INT UNSIGNED, min_amount INT UNSIGNED, max_amount INT UNSIGNED`
   (CMaNGOS keeps `ahbot_items` in the characters DB; keep that.)
2. Load at init; apply during accumulation — `AddChance > 0` replaces the natural loot roll for that item, `Value == 0` blacklists it.
3. Commands under `.cmahbot`: `status`, `reload`, `rebuild [all]`, `item <id> <value> <chance> <min> <max>`, `item reset <id>`.
4. `rebuild`: expire unsold bot auctions, then run `((Time.Max - Time.Min) / 2 + Time.Min) * 90` simulated passes to prefill — with the buy half suppressed, exactly as CMaNGOS does.
5. Safety ceiling `CMangosAHBot.HardCap` (default 0 = unlimited): skip the pass if bot-owned auctions in that house exceed it. A runaway guard, **not** a setpoint — do not let this become quota filling.

**Acceptance:** blacklisting removes an item from subsequent passes; `rebuild` fills the AH in one command without stalling the world thread more than a few seconds (chunk it if it does).

### Phase 8 — Measurement and tuning
1. Per-pass timer; log p50/p99 over an hour.
2. Composition CSV; diff against a CMaNGOS AH dump if available, else sanity-check the quality histogram and top-30 items by count.
3. Tune default tuples toward the Vanilla installer's target (~15k items at equilibrium). Remember: **equilibrium = inflow rate × listing lifetime**, there is no cap doing this work.

**Acceptance:** p99 pass time under 200 ms at default config; histogram plausibly drop-rate-weighted.

### Phase 9 — Integration
1. `README.md`: install, the required bot account/character, config reference, accepted deltas from CMaNGOS (bot ownership, deposits, no market-price learning).
2. Add to `wow-manage.sh`: `MODULE_REGISTRY` entry, `MODULE_UPDATE_FILES` entry for `cmangos_ahbot_items.sql` against `acore_characters`, a `configure_cmangos_ahbot()` modelled on `configure_ahbot()`, and menu wiring.
3. Note in the README that `mod-ah-bot` may coexist with `EnableSeller = 0` / `EnableBuyer = 0` and **must** use a different bot account.

**Acceptance:** clean install from `wow-manage.sh` on a fresh server reaches a populated AH with no manual file editing beyond account/GUID entry.

---

## 5. Config keys

`conf/cmangos_ahbot.conf.dist`. Use a `CMangosAHBot.` prefix rather than CMaNGOS's `AuctionHouseBot.` so both modules can be installed simultaneously without key collisions. The suffixes are identical, so translating the Vanilla installer's `sed` block is a one-token substitution.

```
CMangosAHBot.Enable  = 0
CMangosAHBot.Account = 0
CMangosAHBot.GUID    = 0
CMangosAHBot.TickCompensation = 3     # internal 20s steps per 60s core hook

CMangosAHBot.Chance.Sell = 10
CMangosAHBot.Chance.Buy  = 10

CMangosAHBot.Loot.Creature.Normal    =  30, 35,  8, 12
CMangosAHBot.Loot.Creature.Rare      =   0, 10,  1,  1
CMangosAHBot.Loot.Creature.Elite     =  30, 34,  1,  2
CMangosAHBot.Loot.Creature.RareElite = -10,  2,  1,  1
CMangosAHBot.Loot.Creature.WorldBoss = -20,  1,  1,  1
CMangosAHBot.Loot.Disenchant         =  10, 12,  1,  1
CMangosAHBot.Loot.Fishing            =   3,  5, 30, 40
CMangosAHBot.Loot.Gameobject         =  13, 16,  7, 11
CMangosAHBot.Loot.Skinning           =   3,  5, 50, 50
CMangosAHBot.Items.Profession        =  80, 90,  0, 50

CMangosAHBot.Value.Poor      = ...    # per-class percentage lists, CMaNGOS format
CMangosAHBot.Value.Normal    = ...
CMangosAHBot.Value.Uncommon  = ...
CMangosAHBot.Value.Rare      = ...
CMangosAHBot.Value.Epic      = ...
CMangosAHBot.Value.Legendary = ...
CMangosAHBot.Value.Artifact  = ...
CMangosAHBot.Value.Vendor    = 1
CMangosAHBot.Value.Variance  = 10

CMangosAHBot.Bid.Min = 75
CMangosAHBot.Bid.Max = 90
CMangosAHBot.Buy.Value = 90
CMangosAHBot.Time.Min = 2
CMangosAHBot.Time.Max = 24

CMangosAHBot.HardCap = 0
```

Copy default values and the explanatory comment blocks from CMaNGOS `ahbot.conf.dist.in` — the four-number tuple semantics are genuinely hard to guess and the upstream comments explain them well. **Licence note:** CMaNGOS is GPL-2.0, AzerothCore modules are AGPL-3.0. Attribute ported files in-header and state the provenance in the README.

---

## 6. Guardrails

- Directory name, loader function name, and `include.sh` must match exactly or the module builds and does nothing. Verify against the generated `ModulesLoader.cpp`, not by assumption.
- If `Enable = 1` and any loot-template vector is empty, log an **error** naming the vector and disable the module. Silent partial operation is the failure mode that has already cost time.
- Do not "improve" the CMaNGOS pricing formula. Its oddities are load-bearing — the `BuyPrice / SellPrice > 5` guard exists specifically because arrows and shells are mispriced in `item_template`. Port literally; tune via config afterward.
- Never touch `mod-ah-bot`'s files, tables, or config.
- Every filter that drops an item should be counted and logged in aggregate at debug level. When the composition looks wrong, the drop counters are how it gets diagnosed.
- The buyer's deferred-buyout vector is not optional (Phase 6.2).

---

## 7. Accepted deltas from CMaNGOS

Document these in the README so they aren't rediscovered as bugs:

| Delta | Reason | Effect |
|---|---|---|
| Auctions owned by a real character, not `owner = 0` | AC core assumes a valid owner GUID | Bot character must exist and not be played; sale proceeds are mailed then destroyed by the mail script rather than never existing |
| Deposits nominal | The transient `Player` is never charged | No gold constraint on listing volume — same net effect as CMaNGOS, different mechanism |
| 60 s core hook vs 20 s tick | AC's auction update interval | Compensated by `TickCompensation`; verify the per-house 120 s period empirically in Phase 5 |
| No market-price learning | CMaNGOS has no analogue | On a solo server this feature is near-inert anyway: with no real buyers the dominant signal is expiry, which drives learned prices monotonically down |

---

## 8. Definition of done

- [ ] Builds clean against the pinned `mod-playerbots/azerothcore-wotlk` `Playerbot` branch
- [ ] Module loads, all four scripts register, config reads
- [ ] All three auction houses populate; per-house sell period measured at ~120 s
- [ ] Quality histogram is drop-rate-weighted, not flat-per-bin
- [ ] Buyer bids and buys out using the same valuation the seller lists at; 1-hour soak with no iterator invalidation
- [ ] `.cmahbot rebuild` prefills without a visible world-thread stall
- [ ] p99 pass time under 200 ms at default config
- [ ] `NOTES-verification.md` documents A1–A8 with citations
- [ ] `README.md` documents install, config, coexistence with `mod-ah-bot`, and §7
- [ ] `wow-manage.sh` can install and configure it end to end

---

## Appendix: reference file map

**CMaNGOS (read-only reference)**
- `src/game/AuctionHouseBot/AuctionHouseBot.cpp` — `Initialize()` L43-157, `Update()` L159-286 (sell branch L166-255, buy branch L256-285), `Rebuild()` L295-320, `AddLootToItemMap()` L582-611, `CalculateBuyoutPrice()` L613-625
- `src/game/AuctionHouseBot/AuctionHouseBot.h` — member layout to mirror
- `src/game/AuctionHouseBot/ahbot.conf.dist.in` — config semantics and comments
- `src/game/World/World.cpp` L1366 (20 s tick), L1580-1587 (dispatch)

**AzerothCore core (read-only)**
- `src/server/game/Loot/LootMgr.h` — `MAX_NR_LOOT_ITEMS` L51, `LootStore` L206, `Process()` L256, `Loot` L311, extern stores L413-425
- `src/server/game/Loot/LootMgr.cpp` — `LootStoreItem::Roll()` L311, `Loot::AddItem()` L481, `LootTemplate::Process()` L1667
- `src/server/game/AuctionHouse/AuctionHouseMgr.cpp` — L38 (60 s interval), L441 `Update()` and the `OnBeforeAuctionHouseMgrUpdate` hook
- `modules/CMakeLists.txt` L153-157 — loader function naming
- `src/server/game/Spells/SpellInfo.h` — `SpellEffectInfo`, `Attributes`

**`mod-ah-bot` (pattern reference only — do not modify)**
- `src/AuctionHouseBot.cpp` `Update()` L1012-1120 — transient `WorldSession`/`Player` construction
- `src/AuctionHouseBot.cpp` `Sell()` L890-925 — auction creation sequence
- `src/AuctionHouseBotMailScript.cpp` — mail suppression, whole file
- `src/AuctionHouseBotAuctionHouseScript.cpp` — hook registration and notification suppression
