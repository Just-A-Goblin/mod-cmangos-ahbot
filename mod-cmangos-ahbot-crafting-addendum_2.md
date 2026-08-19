# Addendum 2: Crafted-Goods Economy for `mod-cmangos-ahbot`

**Applies to:** `mod-cmangos-ahbot-plan.md` and `mod-cmangos-ahbot-progression-addendum_1.md`
**Status:** adds a parallel generator (the "craft layer") alongside the loot simulator; amends the buyer (base Phase 6), commands (Phase 7), and measurement (Phase 8); adds Phases C0–C7.
**Companion modules:** `ZhengPeiRu21/mod-individual-progression` (via the existing effective-state machinery only — no new IP coupling).

---

## 0. Read this before writing any code

### What this layer is

The base module simulates **loot flow**: it executes the generative process (loot tables) and lets the distribution fall out. This addendum extends the same philosophy one level up the causal chain: it simulates **crafters**, not crafted items.

A configurable population of virtual crafters, each with a skill level drawn from a progression-shaped distribution, runs *crafting sessions* each sell pass:

- A crafter **below the skill cap** is *leveling*: it picks the recipe that maximizes expected skill-up per gold at its current skill — the same optimization every leveling guide encodes — crafts a batch, gains simulated skill, and dumps the products at or below material cost.
- A crafter **at the cap** is *producing*: it picks recipes weighted by a demand model, crafts in realistic batch sizes, and lists at material cost × margin.
- Either way the session **consumes reagents**, and that consumption feeds a demand ledger that drives the buyer.

Nothing about the output distribution is specified anywhere. The Bronze Bar glut, the flask meta, the mat demand — all emergent from "simulate a player optimizing," exactly as rarity and stack mix are emergent from executing loot tables.

### What it replaces

The existing `Items.Profession` source (base plan §2.3 + Phase 4.4 quality-decay roll) is **retired when `Craft.Enable = 1`**. Its flat scan over all `SPELL_EFFECT_CREATE_ITEM` spells produces uniform noise: no glut, no meta, no demand. Keep the code path behind the flag for fallback; do not run both simultaneously (double-listing).

### Why pricing must change for this path — and only this path

`CalculateBuyoutPrice()` is anchored to vendor `BuyPrice`/`SellPrice`. That is a defensible proxy for world drops and **must not be touched for them** (base plan §6: the formula's oddities are load-bearing). It is structurally wrong for crafted goods: a Flask of the Titans has a trivial vendor price; its real price was always Σ(material market value) + margin.

This breaks *both* directions, because of buyer coherence (base plan §0): the bot lists flasks at pennies **and** values player-listed flasks at pennies, so a player who levels Alchemy can never sell anything. Cost-anchored pricing for crafted goods, applied symmetrically to seller and buyer, is the difference between an economy and a garage sale.

### Non-goals

- **No market-price learning.** Still excluded. The undercut mechanism in §7 is floored at material cost, which is anchored to the intrinsic formula — prices cannot drift monotonically. Do not add learned state.
- **No per-item historical meta curation.** Era behavior comes from category weights + ilvl windowing + recipe-source gating. The override table covers operators who want item-level control.
- **No multi-character seller pool.** Singleton bot ownership stands (base plan non-goals). A cosmetic seller-alias pool is explicitly deferred; it complicates the mail script and GUID-membership checks for marginal payoff.

---

## 1. Assumptions to verify first (Phase C0)

Same discipline as base Phase 0. Record all results in `NOTES-verification.md` with file:line citations. Several of these silently produce wrong output rather than errors if wrong.

| #     | Assumption                                                                                                                                                                                 | How to verify                                                                                    | If wrong                                                                                          |
| ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- |
| C-A1  | `SpellInfo` exposes reagents as `Reagent[MAX_SPELL_REAGENTS]` / `ReagentCount[...]` (expect 8 slots)                                                                                       | `src/server/game/Spells/SpellInfo.h`                                                             | Adjust member access in the graph builder                                                          |
| C-A2  | AC's own skill-up model is reachable: `Player::UpdateCraftSkill` computes gain chance from `SkillLineAbilityEntry` trivial ranks (grey = `TrivialSkillLineRankHigh`, yellow = `...Low`, green = midpoint) via a `SkillGainChance`-style helper | `src/server/game/Entities/Player/Player.cpp`, `DBCStructure.h`                                  | Port the formula from wherever AC actually computes it; do **not** invent one                       |
| C-A3  | Recipe items (`ITEM_CLASS_RECIPE`) resolve to their crafting spell via a learn-spell indirection (item spell → `SPELL_EFFECT_LEARN_SPELL` → crafting spell)                                | Pick 5 known recipe items (one per acquisition class) and walk the chain in `item_template` + DBC | Adjust the resolver; the rarity classifier depends on it                                            |
| C-A4  | Trainer-taught recipes enumerable from the world DB (`npc_trainer` or AC's current trainer table) keyed by spell id                                                                        | `describe npc_trainer` on `acore_world`; grep `ObjectMgr` trainer loading                        | Enumerate via whatever table AC loads trainers from                                                 |
| C-A5  | Daily-cooldown recipes detectable via `SpellInfo::RecoveryTime` / `CategoryRecoveryTime` (ms), ≥ 86 400 000 for transmute/Titansteel/Moonshroud-class CDs                                  | Check 3 known CD recipes and 3 non-CD recipes                                                     | Find the correct field (some CDs live on the spell category)                                        |
| C-A6  | Herb and ore nodes already flow through the base module's gameobject vector (`gameobject_template` type 3), i.e. raw-mat supply is already live                                            | Look up 2 known herb node ids and 2 mining vein ids; confirm type and `Data1` loot id            | Add the missing node type to the gameobject query; the craft layer depends on mat supply existing   |
| C-A7  | Live bot listings for an item are iterable in-memory per house via `AuctionHouseObject` without SQL, cheaply enough to compute a per-item median each pass                                 | `AuctionHouseMgr.h/.cpp`                                                                          | Cache medians once per pass from a single iteration; never per-item queries                          |
| C-A8  | Skill line ids: Alchemy 171, Blacksmithing 164, Enchanting 333, Engineering 202, Leatherworking 165, Tailoring 197, Cooking 185, First Aid 129, Jewelcrafting 755, Inscription 773        | `SkillLine.dbc` / `SharedDefines.h`                                                              | Correct the id table                                                                                |
| C-A9  | Enchanting produces no listable product before WotLK vellums; enchant-scroll creation spells exist only at expansion 2                                                                     | Scan the graph output for Enchanting products at expansion 0/1                                    | If pre-WotLK products appear, they are misclassified — investigate before gating                     |
| C-A10 | `urand`-family RNG can be bypassed by a module-local seeded engine without affecting core determinism                                                                                       | grep `urand` implementation                                                                       | Use `std::mt19937` held by the craft layer regardless; never reseed the core's RNG                   |

---

## 2. The recipe graph (build once at startup)

New files: `CMangosAHBotCraft.h/.cpp` (population + sessions), `CMangosAHBotRecipes.h/.cpp` (graph + classification), `CMangosAHBotCost.h/.cpp` (cost engine). All owned by the existing singleton; built during the same startup phase as the id vectors, rebuilt on progression change (cheap: the graph is static, only the *availability mask* changes).

### 2.1 Node shape

For each spell passing the existing profession filter (`Attributes & 0x20 && Attributes & 0x10000`, `SPELL_EFFECT_CREATE_ITEM`):

```
struct CraftRecipe {
    uint32 spellId;
    uint32 skillLine;            // C-A8
    uint32 productItem;
    uint32 productCount;         // effect basepoints + 1; verify against a multi-output recipe
    std::vector<std::pair<uint32,uint32>> reagents;   // (itemId, count), from C-A1
    uint16 minSkill;             // SkillLineAbilityEntry::MinSkillLineRank
    uint16 greySkill;            // TrivialSkillLineRankHigh   (C-A2)
    uint16 yellowSkill;          // TrivialSkillLineRankLow
    RecipeRarity rarity;         // §2.2
    uint8  maxExpansion;         // §2.3 gate
    bool   dailyCooldown;        // C-A5
    ItemCategory category;       // §5.1
};
```

Skip recipes with no reagents (conjures) and recipes whose product fails the base module's existing filters (BoP, quest-bound, zeroed class in the value matrix, blacklisted override).

### 2.2 Acquisition rarity — margin correlates with it

| Rarity      | Detection                                                                                   | Default margin band |
| ----------- | -------------------------------------------------------------------------------------------- | ------------------- |
| `TRAINER`   | Spell id present in trainer table (C-A4)                                                      | 100–125 %           |
| `VENDOR`    | A recipe item teaching it (C-A3) appears in `npc_vendor`                                      | 110–140 %           |
| `DROP`      | A recipe item teaching it appears in any loot template                                        | 150–300 %           |
| `UNSOURCED` | None of the above (rep vendors missed by the join, quest rewards, etc.)                       | treat as `DROP`     |

Log counts per profession per rarity at startup. `TRAINER` should dominate; `UNSOURCED` in the low hundreds is expected, thousands means the resolver (C-A3/C-A4) is broken.

### 2.3 Gating (reuses addendum 1 machinery — no new subsystems)

Applied as a per-state **availability mask** over the graph, recomputed with the existing `RefreshInterval` poll:

1. **Skill line by expansion:** Jewelcrafting requires expansion ≥ 1, Inscription ≥ 2, using the existing `TbcAtState` / `WotlkAtState` thresholds. Enchanting *products* (scrolls) require expansion 2 (C-A9); Enchanting *consumption* (§6) is active at all states.
2. **Skill rank by expansion:** the addendum's existing `SkillCaps` (300/375/450) applied to `minSkill` — already specified in addendum §4 Layer 2b; the graph is where it lives now.
3. **Recipe source by drop map:** for `DROP` recipes, resolve the recipe item's loot source(s) to a map and gate by `MapEntry::expansionID` — the same Layer-1 trick. This is what makes crafted Naxx frost-resist gear appear at the right state instead of state 0. Recipes whose drop source is unresolvable: gate by product `ItemLevel` against the existing `ItemLevelCaps` row instead, and count them.
4. **Product ilvl/reqlevel:** the existing Layer 2a caps apply to products as a final mask. Note bags and consumables have junk ilvl — the mask must exempt `ItemCategory` ∈ {BAG, CONSUMABLE-classes} from the ilvl test (they are governed by skill and recipe-source gates instead).

The existing **Layer 3 safety net and its counter apply unchanged** to craft-layer output. A growing counter now also signals a graph-gating hole.

---

## 3. The cost engine

### 3.1 `MatValue(itemId)` — recursive, memoized, cycle-safe

```
MatValue(item):
    if memo.contains(item):        return memo[item]
    if inProgress.contains(item):  return MarketAnchor(item)      # cycle (transmutes!)
    inProgress.insert(item)
    anchor = MarketAnchor(item)
    best   = anchor
    for recipe in producers[item]:                                 # only AVAILABLE recipes
        craftCost = Σ MatValue(r.item) × r.count / recipe.productCount
        best = min(best, craftCost)          # intermediates: market vs make
    inProgress.erase(item)
    memo[item] = best
    return best
```

`MarketAnchor(item)`:
- median buyout-per-unit of **live bot-owned listings** of the item in the neutral house (C-A7), clamped to `[Craft.AnchorClampMin, Craft.AnchorClampMax]` × `CalculateBuyoutPrice(item)` (defaults 50 %–300 %);
- fallback when nothing is listed: `CalculateBuyoutPrice(item)` unchanged.

This gives internal consistency for free: the world-drop simulator already lists herbs, ore, cloth, and leather (C-A6), so crafted prices track the mat prices the same bot charges — and the clamp prevents a player-driven mat squeeze or dump from distorting the whole crafted price sheet.

**Memo lifetime:** one sell pass. Build lazily; a full pass touches a few hundred items — measure in C7, do not pre-optimize.

**Hard requirements:** cycle detection is not optional (Transmute Earth↔Water etc. is an infinite loop on a stock DB). Depth-limit at 8 as a belt-and-suspenders guard and count hits.

### 3.2 Listing price

```
CraftPrice(recipe) = Σ MatValue(reagent)×count / productCount
                     × Margin(recipe)            # §3.3
                     → ValueWithVariance()       # existing, unchanged
```

### 3.3 Margins

| Situation                         | Config                          | Default    |
| --------------------------------- | ------------------------------- | ---------- |
| Leveling-session output           | `Craft.Margin.Leveling`         | 70–95 %    |
| `TRAINER` recipe, producing       | `Craft.Margin.Trainer`          | 100–125 %  |
| `VENDOR` recipe, producing        | `Craft.Margin.Vendor`           | 110–140 %  |
| `DROP`/`UNSOURCED`, producing     | `Craft.Margin.Drop`             | 150–300 %  |
| Daily-cooldown product, extra ×   | `Craft.Margin.CooldownBonus`    | 150 %      |

Below-cost leveling dumps are **correct and important** — they are what makes the AH the cheap place to buy bars and bolts, exactly like live.

---

## 4. The crafter population and session loop

### 4.1 Population

`Craft.Population` virtual crafters distributed across production professions by `Craft.ProfessionWeights`. Each crafter is 12 bytes of state: profession, current skill, leveling/producing flag. Persisted **in memory only** — on restart the population is re-rolled from the skill distribution (§4.2), which is the correct behavior: the distribution, not the individuals, is the model.

### 4.2 Skill distribution per progression state

`Craft.SkillDist` maps effective state → (cap, concentration). Early states: mass spread across 1–cap (a fresh server: everyone skilling up → big intermediate glut). Late states: mass at cap. Implement as: skill = cap × Beta(α(state), β(state)) with the two shape params in a per-state config row, same string format as `ItemLevelCaps`. `LevelingShare` (the fraction of sessions drawn from below-cap crafters) is derived from the distribution, with a config override.

The cap itself follows the expansion unlocks: 300 below `TbcAtState`, 375 below `WotlkAtState`, 450 above — one source of truth with §2.3.

### 4.3 Session loop (runs in the sell half of the existing rotation)

```
for s in urand(Craft.Sessions.Min, Craft.Sessions.Max):        # per pass, after TickCompensation
    if urand(0,99) >= Craft.Chance: continue
    crafter = population.sample()
    if crafter.skill < cap(crafter.profession):
        recipe = ChooseLevelingRecipe(crafter)                  # §4.4
        batch  = urand(Craft.Batch.Leveling.Min, .Max)          # default 5–15
        simulate skill-ups over batch via C-A2 formula
        margin = Margin.Leveling
    else:
        recipe = ChooseProductionRecipe(crafter)                # §5
        batch  = CategoryBatch(recipe.category)                 # §7.2
        margin = Margin(recipe.rarity) [× CooldownBonus]
        if recipe.dailyCooldown: enforce §4.5 cap
    ledger.Credit(recipe.reagents × batch)                      # §6
    queue listings: product × batch × productCount, priced §3.2, stacked §7.2
```

Listings go through the **existing** auction-creation path (Phase 5 code): same transaction batching, same owner, same duration roll, same notification suppression. The craft layer is a second producer feeding the same lister.

### 4.4 `ChooseLevelingRecipe` — the emergent-glut engine

At skill S, among available recipes with `S < greySkill`:

```
score(r) = P(skillup | S, r)                 # AC's own formula, C-A2
           / max(ε, Σ MatValue(reagent)×count)
```

Sample from the top-k (k=3) by score with weights, not argmax — real players didn't all follow the same guide. This reproduces the historically observed gluts (Bronze Bars, Silk Bandages, Bolts of Runecloth, low LW/BS greens) **without any of those items being named anywhere**, because it is the same optimization players ran.

### 4.5 Daily-cooldown scarcity

Per-recipe daily output cap: `Craft.Population × professionShare(recipe.skillLine) × Craft.CooldownPerCrafter` (default 1.0). Track a rolling 24 h counter per CD recipe. This makes Titansteel, Moonshroud, and transmute products scarce and expensive with one mechanism.

---

## 5. The demand model (production recipe choice)

### 5.1 Category classifier — heuristic, at graph build time

| Category      | Detection heuristic                                                                       |
| ------------- | ------------------------------------------------------------------------------------------ |
| `FLASK`       | Consumable whose use-spell applies a beneficial aura ≥ 1 h and persists through death (flag) — fall back to ≥ 1 h duration alone |
| `ELIXIR_POT`  | Consumable, beneficial aura or heal/mana effect, < 1 h                                      |
| `FOOD`        | Consumable with food/drink category and a stat aura                                         |
| `BAG`         | `ITEM_CLASS_CONTAINER`, weight ∝ slot count                                                 |
| `GEM_CUT`     | `ITEM_CLASS_GEM`, product of a JC recipe                                                    |
| `SCROLL`      | Enchanting product (expansion 2 only, C-A9)                                                 |
| `AMMO`        | `ITEM_CLASS_PROJECTILE`                                                                     |
| `GEAR`        | Armor/weapon classes                                                                        |
| `INTERMEDIATE`| Trade-goods-class product that appears as a reagent in ≥ 1 other recipe                     |
| `MISC`        | Everything else                                                                             |

Log category counts. `MISC` should be a long thin tail; if it dominates, the classifier heuristics need work before proceeding.

### 5.2 Production choice

```
weight(r) = CategoryWeight(era, r.category)                     # §8 config strings
          × ilvlWindow(r)      # GEAR only: triangular weight over
                               # [ilvlCap − Craft.GearWindow, ilvlCap]; 0 outside
          × stateBoost(r)      # optional sparse per-state multipliers (§8) —
                               # the hook for resistance-consumable spikes pre-AQ/Naxx etc.
          × overrideWeight(r)  # per-item override table extension (§8.2)
```

Sample proportionally among the crafter's profession's available recipes. `INTERMEDIATE` production is mostly *implicit* (leveling crafters make bars/bolts en masse); give it a small explicit weight so at-cap smelters exist.

---

## 6. The buyer extension — the demand ledger

The single highest immersion-per-line component: **your gathered mats sell.**

1. Every session credits `ledger[itemId] += count` (rolling window, `Craft.Ledger.WindowHours`, default 24; decay by truncation).
2. In the buy half of the rotation, for each non-bot auction: if the item has ledger demand, value it at `MatValue(item) × count × Buy.Value / 100` instead of the vendor-anchored intrinsic; bid/buy exactly as the base Phase 6 buyer does, then debit the ledger by the quantity bought.
3. **Crafted-goods symmetry:** non-bot auctions of items that are *products* of available recipes are valued at `CraftCost(item) × Buy.Value / 100` — this is buyer coherence extended to the craft layer, and it is what lets a player alchemist actually sell flasks.
4. **Demand saturation (exploit control):** per item, track quantity bought in the window. Valuation multiplier = 1.0 up to `Craft.Buy.DailyCap` units, then decays linearly to `Craft.Buy.FloorMult` (default 0.3) at 3× the cap. Realistic (dump 100 flasks → price crashes) and caps the gold faucet with one mechanism. Sessions proceed whether or not mats were actually bought — the virtual crafter "farmed it" — so the sim never stalls on an empty AH.
5. The deferred-buyout vector pattern (base Phase 6.2) now serves two valuation paths feeding one loop. **Still not optional.**

Gold-flow note for the README: bot mat purchases inject gold into player pockets; crafted listings sink it. On a solo/household server the faucet is the point; `Craft.Buy.DailyCap` is the throttle. Document, don't "fix."

---

## 7. Market texture

### 7.1 Undercut ladders — bounded, not learned

When listing an item with live bot listings in the same house:

```
price = min(existingBuyoutPerUnit) × urand(95, 99) / 100
floor = producing: 100 % of unit mat cost | leveling: 60 %
price = max(price, floor)
```

The floor is anchored to `MatValue`, which is clamp-anchored to the intrinsic formula — the monotonic-decay pathology that justified excluding market-price learning cannot occur. State: none beyond the current pass.

### 7.2 Stacks and price points

Per category: `AMMO` full stacks ×N; `FLASK/ELIXIR_POT/FOOD` mixed 5s and 20s; `INTERMEDIATE` full stacks plus one ragged partial (`urand(1, max−1)`); `GEAR/BAG/GEM_CUT` singles. Split each session's output into 2–4 price points via variance + one undercut step, so browsing shows a ladder, not a wall.

---

## 8. Config keys

### 8.1 `conf/cmangos_ahbot.conf.dist` additions

```
CMangosAHBot.Craft.Enable            = 0
CMangosAHBot.Craft.Seed              = 0        # 0 = nondeterministic; nonzero = reproducible (module-local RNG)
CMangosAHBot.Craft.Population        = 120
CMangosAHBot.Craft.Sessions          = 4, 8     # per sell pass (after TickCompensation)
CMangosAHBot.Craft.Chance            = 100
CMangosAHBot.Craft.LevelingShare     = -1       # -1 = derive from skill distribution
CMangosAHBot.Craft.ProfessionWeights = "171:20,164:12,165:12,197:15,202:10,333:10,755:10,773:6,185:3,129:2"
CMangosAHBot.Craft.SkillDist         = "<state:alpha:beta,...>"   # Beta shape per state; see addendum §4.2
CMangosAHBot.Craft.Batch.Leveling    = 5, 15
CMangosAHBot.Craft.GearWindow        = 26       # ilvl window below cap for GEAR demand

CMangosAHBot.Craft.Margin.Leveling      = 70, 95
CMangosAHBot.Craft.Margin.Trainer       = 100, 125
CMangosAHBot.Craft.Margin.Vendor        = 110, 140
CMangosAHBot.Craft.Margin.Drop          = 150, 300
CMangosAHBot.Craft.Margin.CooldownBonus = 150
CMangosAHBot.Craft.CooldownPerCrafter   = 1.0

CMangosAHBot.Craft.AnchorClampMin    = 50       # % of intrinsic
CMangosAHBot.Craft.AnchorClampMax    = 300

CMangosAHBot.Craft.Buy.DailyCap      = 20       # units/item/window at full valuation
CMangosAHBot.Craft.Buy.FloorMult     = 30       # % valuation floor past 3× cap
CMangosAHBot.Craft.Ledger.WindowHours= 24

# category:weight strings; categories from §5.1
CMangosAHBot.Craft.Demand.Vanilla    = "FLASK:20,ELIXIR_POT:30,FOOD:25,BAG:10,AMMO:25,GEAR:15,INTERMEDIATE:8,MISC:1"
CMangosAHBot.Craft.Demand.Tbc        = "FLASK:35,ELIXIR_POT:25,FOOD:25,GEM_CUT:30,BAG:10,GEAR:20,INTERMEDIATE:8,MISC:1"
CMangosAHBot.Craft.Demand.Wotlk      = "FLASK:40,ELIXIR_POT:15,FOOD:30,GEM_CUT:35,SCROLL:20,BAG:12,GEAR:20,INTERMEDIATE:8,MISC:1"
CMangosAHBot.Craft.Demand.StateBoost = ""       # sparse "state:CATEGORY:mult,..." e.g. "4:ELIXIR_POT:180,7:GEAR:150"
```

### 8.2 Override table extension

`ALTER TABLE` (new SQL file, additive, in `data/sql/db-characters/`):

```
ALTER TABLE cmangos_ahbot_items
  ADD COLUMN craft_weight INT UNSIGNED NULL,   -- NULL = no override; 0 = never craft; else weight mult %
  ADD COLUMN craft_margin INT UNSIGNED NULL;   -- NULL = default; else margin %
```

Existing semantics (`value = 0` blacklists, `add_chance` injects) apply unchanged to craft-layer products.

### 8.3 Commands (extends `.cmahbot`)

- `craft status` — population summary, sessions run, ledger depth (top 10), per-category live listing counts, cost-memo stats, CD counters.
- `craft dump [file]` — CSV: `item, category, rarity, count, unit_price, unit_matcost, ratio` for all live craft-layer listings.
- `craft simulate <n>` — run n sessions with listing creation suppressed; print what would list. Fast tuning loop.
- `craft selftest` — run §10.3 invariants; print single parseable `CRAFT SELFTEST: PASS|FAIL <detail>` line.
- `craft testlist <itemId> <count> <stack> <price>` — **gated behind `Craft.TestCommands = 1`**: create an auction owned by a synthetic non-bot GUID, for buyer automation (§10.4).
- `craft ledger [itemId]` — inspect demand ledger.

---

## 9. Phased task list

Each phase ends in a compiling, runnable server. Do not proceed past a failed acceptance check.

### Phase C0 — Verification
C-A1…C-A10 into `NOTES-verification.md` with citations. **Acceptance:** all rows resolved; the 5-recipe walk (C-A3) documented per acquisition class.

### Phase C1 — Recipe graph + classification + gating
§2 in full, behind `Craft.Enable` with zero behavior change to the base module. **Acceptance:** startup logs per-profession × per-rarity counts; JC/Inscription masked at expansion 0; 5 known raid-drop recipes absent below their state and present above; `MISC` a minority; graph build < 5 s.

### Phase C2 — Cost engine
§3, plus `craft simulate` limited to cost queries. **Facade requirement (for §10.2):** `CMangosAHBotCost` consumes an abstract `IRecipeSource` + `IMarketAnchor` pair, not `SpellMgr`/`AuctionHouseObject` directly. **Acceptance:** hand-check ten chains (bar←ore, bolt←cloth, flask←herbs, a transmute, a multi-reagent epic) — every price explainable as mats × margin; zero recursion blowups over the full graph; cycle counter > 0 (transmutes found) but bounded.

### Phase C3 — Leveling simulator, sell only
§4 with production sessions disabled. **Acceptance:** at `Progression.Static = 0` after `rebuild` + 2 h soak, the AH develops the historically correct gluts — smelted bars, cloth bolts, bandage-tier goods, low BS/LW greens — priced below craft cost, **with none of those items named in any config**; craft-layer listings pass the Layer 3 net with counter ≈ 0.

### Phase C4 — Demand-weighted production
§5. **Acceptance:** composition CSV at states 0, 7, 8, 13, 18 shows category mix shifting per the era weights; GEAR listings cluster inside the ilvl window; CD-recipe products present but ≤ §4.5 cap; sentinel cross-era crafted items (Netherweave Bag pre-state-8, Frost Wyrm flask pre-state-13) absent.

### Phase C5 — Buyer integration
§6. **Acceptance:** `craft testlist` an era-appropriate herb stack at ≤ anchor price → bought within 3 buy passes and ledger debited; testlist 50 identical flasks at cost → first `DailyCap` units bought near full valuation, tail bids decay per saturation curve; 1 h soak with listing churn, no iterator invalidation; base-module world-drop buying behavior unchanged when `Craft.Enable = 0`.

### Phase C6 — Texture
§7. **Acceptance:** browsing any high-volume item shows a 2–4 rung price ladder; no bot listing below its floor; stack-size mix per category matches §7.2.

### Phase C7 — Measurement and tuning
Extend the Phase 8 harness: per-pass timing now includes session + cost-memo cost; composition CSV gains the §8.3 `craft dump` columns; run the §10.5 sweep. **Acceptance:** p99 total pass time still < 200 ms at defaults; price-to-cost ratio histogram per category within margin bands; equilibrium craft-layer stock within ±30 % of `inflow × mean lifetime` prediction.

---

## 10. Testing and automation

### 10.1 Determinism first
With `Craft.Seed ≠ 0`, all craft-layer randomness (population roll, session sampling, recipe choice, batches, variance, undercut rolls) comes from one module-local `std::mt19937`. Identical config + seed + DB ⇒ identical `craft dump`. This is the property every layer below depends on; implement it in C1, not later.

### 10.2 Offline unit layer (no server)
Because C2 mandates the `IRecipeSource`/`IMarketAnchor` facade, the cost engine, leveling chooser, saturation curve, and skill-up simulation compile into a standalone test binary (`tools/craft-tests/`, plain `assert` or doctest, built by an optional CMake target — AC modules have no test convention, so keep it self-contained). Fixture: ~30 hand-written recipes including a transmute cycle, a multi-output recipe, and a CD recipe. Assert: memoization correctness, cycle fallback, min(market, make) for intermediates, margin math, glut-chooser picks the known-cheapest recipe at each skill band, saturation decay endpoints.

### 10.3 In-server invariants (`craft selftest`)
Cheap checks runnable any time: every live craft-layer listing ≥ its floor; no listing violates the availability mask at current state; ledger non-negative; CD counters ≤ caps; Layer 3 craft counter below threshold; graph counts match startup log. Single parseable output line → this is the CI heartbeat.

### 10.4 Black-box behavioral layer
Headless worldserver + SOAP (or console) + `acore_characters` SQL. Driver script (Python) per run:
```
start server → wait ready → set Progression.Static=N → .cmahbot rebuild
→ soak T minutes → .cmahbot craft dump → .cmahbot craft selftest
→ assertions (pandas) on the CSV + auctionhouse table → next state
```
Assertions are on **distribution properties**, never exact contents: glut share at state 0, category mix vs weights (χ² tolerance), zero cross-era sentinels, ratio histograms, ladder presence. Buyer tests drive `craft testlist` and assert purchase latency + ledger movement via SQL.

### 10.5 The progression sweep (extends addendum 1 §8)
States 0, 4, 7, 8, 13, 16, 18 × the full 10.4 script, seeded. Archive CSVs per run; regressions diff against the archived baseline rather than absolute numbers where tuning is expected to move them.

### 10.6 What stays manual
"Feels like a populated server" and the historical fidelity of era weights are judgment and tuning respectively. Everything mechanical beneath them is covered above.

---

## 11. Risks

| Risk                                                                    | Mitigation                                                                                                    |
| ------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------- |
| Transmute cycles hang the cost engine                                     | Cycle set + depth 8 + counter; unit-tested offline (10.2) before any server run                                  |
| Craft-and-dump gold exploit via cost-based buyer                          | Saturation curve (§6.4) + `DailyCap`; testlist-based regression in C5                                              |
| Recipe-item resolver (C-A3) misclassifies rarity → wrong margins/gates    | 5-recipe verification walk in C0; per-rarity count logs; `UNSOURCED` treated conservatively as `DROP`             |
| Category classifier mislabels → wrong demand mix                          | Category count logs at startup; `MISC`-dominance is a hard stop in C1                                             |
| Cost-memo or median-anchor pass cost blows the 200 ms p99                 | Single-iteration median cache per pass (C-A7); measure in C7; lazily-built memo                                    |
| Enchant scrolls leak pre-WotLK                                            | C-A9 verification + expansion-2 product gate + sentinel in the sweep                                              |
| Double listing if legacy profession path left on                          | `Craft.Enable = 1` hard-disables `Items.Profession`; log a warning if both configured                             |
| Skill distribution / demand weights are approximations                    | All config-driven (§8); tune in C7; seeded sweeps make retuning diffable                                          |

---

## 12. Definition of done

- [ ] C-A1–C-A10 in `NOTES-verification.md` with citations
- [ ] Graph builds < 5 s; rarity/category/gating counts logged; masks verified at states 0/8/13
- [ ] Offline test binary green (cost engine, chooser, saturation, skill-up)
- [ ] State-0 soak produces the leveling glut with no glut item named in config
- [ ] Progression sweep (10.5) green: category shifts, zero cross-era sentinels, CD caps held
- [ ] Player-simulation buys work end to end via `craft testlist`; saturation curve verified
- [ ] Buyer coherence holds: seller list price and buyer valuation derive from the same `CraftPrice` for every crafted item
- [ ] p99 pass time < 200 ms at defaults with `Craft.Enable = 1`
- [ ] `craft selftest` wired into the soak driver; README documents the craft layer, its config, the gold-flow behavior, and the legacy-path fallback
