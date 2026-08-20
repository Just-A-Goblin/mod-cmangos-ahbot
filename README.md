# mod-cmangos-ahbot

A **loot-simulation** auction house bot for AzerothCore (WotLK, `mod-playerbots`
`Playerbot` branch). It ports the CMaNGOS AuctionHouseBot *mechanism*: instead of
filling quality bins toward a quota, it executes the server's real loot tables —
the same `LootTemplate::Process()` path a dying mob runs — and lists whatever
falls out. Rarity, stack sizes, and item mix are inherited from the world DB for
free. Volume is controlled probabilistically, with auction **expiry as the sink**
(equilibrium = inflow rate × listing lifetime); there is no stock cap doing that
work.

It also **gates listings by progression** (via `mod-individual-progression`) so a
character at `PROGRESSION_START` never sees Outland/Northrend trade goods in the
AH. See `mod-cmangos-ahbot-progression-addendum_1.md`.

> This is a fresh module, not a fork of `mod-ah-bot`. It supersedes any earlier
> `CmAHBot*` attempt. Provenance: mechanism/pricing derive from
> `cmangos/mangos-classic` (GPL-2.0); this module is AGPL-3.0.

## Install

1. Drop this directory into `azerothcore/modules/mod-cmangos-ahbot` (the directory
   name is load-bearing — it generates the loader `Addmod_cmangos_ahbotScripts`).
2. Re-run CMake and rebuild the server.
3. Apply the override-table SQL to the **characters** DB (auto-applied from
   `data/sql/db-characters/`, or import `cmangos_ahbot_items.sql` manually).
4. Copy `conf/cmangos_ahbot.conf.dist` → `cmangos_ahbot.conf` and set at minimum:
   - `CMangosAHBot.Enable = 1`
   - `CMangosAHBot.Account` / `CMangosAHBot.GUID` — the bot's **own** account and
     character (must exist; never logged in by a human).
   - `CMangosAHBot.Progression.AccountId` — the **human player's** account (for
     `Source = 1`). This is *not* the bot account; the bot has no progression and
     would pin the AH at state 0 forever.

## Commands (`.cmahbot`, GM)

- `status` — ready flag, effective state, ilvl cap, Layer-3 drop counter, per-house auction counts.
- `reload` — reload config + override table and recompute progression.
- `rebuild [ally|horde|neutral]` — expire the bot's auctions and prefill (buy half suppressed).
- `progression` — effective state, source, caps, and per-source vector sizes.
- `progression refresh` — force an immediate progression recompute.
- `item <id> <value> <chance> <min> <max>` — set a per-item override.
- `item reset <id>` — clear an override.

Override semantics: `value = 0` blacklists an item; `add_chance > 0` injects it
every pass at that % chance (count `min..max`), bypassing the loot roll.

## Progression gating (summary)

Three layers, applied in order (addendum §4):

1. **Expansion gate at the loot source** (primary). Each creature/gameobject/
   skinning/fishing loot id is classified to an expansion via its spawn map
   (`MapEntry::expansionID`); vectors are filtered to `state`'s max expansion and
   rebuilt on progression change. Disenchant is gated by source item level.
2. **Level/skill caps** within an expansion: item-level cap per state; profession
   items gated by crafting-spell `MinSkillLineRank`.
3. **Post-generation safety net**: anything still over the caps is dropped and
   counted (`layer3Dropped`). A non-zero, growing counter signals a source-mapping
   bug — check `.cmahbot status`.

Effective state is `Static`, or max across an account's characters, or max across
online non-bot characters (`Progression.Source` 0/1/2), then clamped to IP's own
`IndividualProgression.ProgressionLimit`. With IP not installed the query returns
nothing → state 0; set `Progression.Enable = 0` to disable gating entirely.

The single dependency on IP internals is the quest-encoding query
(`66000 + state`), ported verbatim and cited in `NOTES-verification.md`. If IP
changes that encoding, that query is the one thing to update.

## Accepted deltas from CMaNGOS (not bugs — plan §7)

| Delta | Reason |
|---|---|
| Auctions owned by a real bot character, not `owner = 0` | AC core assumes a valid owner GUID. |
| Sale proceeds mailed then destroyed (mail script) / suppressed at source | Approximates CMaNGOS's `owner = 0` gold sink. |
| Deposits nominal | The transient `Player` is never charged. |
| 60s core hook vs CMaNGOS 20s tick | Compensated by `TickCompensation` (verify ~120s per-house period empirically). |
| No market-price learning | Near-inert on a solo server; expiry is the dominant price signal. |

## Craft economy (addendum 2)

A second, parallel generator that **simulates crafters** (not crafted items), enabled with
`CMangosAHBot.Craft.Enable = 1`. With it off the module is behaviorally identical to the
base loot-simulation seller/buyer.

- **Recipe graph** built from the server's `SPELL_EFFECT_CREATE_ITEM` profession spells,
  classified by acquisition rarity and demand category, and gated by the same progression
  machinery (skill line/rank era, product ilvl, and a **reagent-era** rule so cross-era
  goods — a Netherweave bag, a Frost Wyrm flask — never appear before their unlock).
- **Cost engine**: recursive, memoized, cycle-safe `MatValue` = min(market anchor, cheapest
  make). One valuation path feeds both seller and buyer (buyer coherence).
- **Sessions**: a virtual crafter population runs each sell pass. Below-cap crafters *level*
  (dump the cheapest-skill-up-per-gold goods below cost — the emergent bar/bolt/bandage
  glut); at-cap crafters *produce* demand-weighted goods (per-era category weights, a GEAR
  ilvl window, daily-cooldown scarcity). Nothing is hardcoded — the gluts and era meta are
  emergent.
- **Buyer + demand ledger**: sessions credit the mats they consume; the buyer buys those
  mats from players (the gold **faucet**) and values crafted goods at make-cost, throttled
  by a **saturation curve** (`Craft.Buy.DailyCap` → `FloorMult`) so dumping crashes the price.
- **Market texture**: listings are split into category-appropriate stacks across a 2–4 rung
  undercut ladder, never below a floor (100 % of mat cost producing / 60 % leveling).

**Gold flow:** bot mat purchases inject gold into player pockets; crafted listings sink it.
On a solo/household server the faucet is the point — `Craft.Buy.DailyCap` is the throttle.

**Legacy fallback:** the flat `Items.Profession` loot source is **retired** while
`Craft.Enable = 1` (it produces uniform noise — no glut, no meta). Set `Craft.Enable = 0`
to fall back to it. Do not run both as listing producers.

**Config:** all keys under `CMangosAHBot.Craft.*` (see `conf/cmangos_ahbot.conf.dist`).
Notable: `Craft.Seed` (nonzero = reproducible, via a module-local RNG); `Craft.Population`,
`Craft.ProfessionWeights`, `Craft.SkillDist`; per-era `Craft.Demand.*`; margins; the buyer
`Craft.Buy.*` + `Craft.Ledger.WindowHours`; `Craft.TestCommands` (buyer test harness) and
`Craft.DumpFile` (sweep CSV). SQL adds `craft_weight`/`craft_margin` to the override table
(`data/sql/db-characters/cmangos_ahbot_items_craft.sql`, additive/idempotent).

**Commands:** `.cmahbot craft status | selftest | simulate [n] | cost [n] | dump [file] |
testlist <id> <count> <stack> <price>`.

**Known deltas / limits** (see `NOTES-verification.md`): the `SCROLL` category is inert
(enchant scrolls are `ENCHANT_ITEM`-on-vellum, not create-item spells); daily-cooldown
detection catches only recipes whose CD is on the create-item spell (WotLK profession
dailies enforce theirs via triggered-spell/category indirection); ammo over-dominates unit
counts (its *listing* share is weight-correct). All are config-tunable and correctness-neutral.

### Tooling

- `tools/craft-tests/` — offline unit tests for the cost engine, glut chooser, skill-up,
  saturation, and texture (no server). `55/55`.
- `tools/craft-soak/run_sweep.py` — black-box progression sweep (config + restart + startup
  dump CSV + pandas assertions on distribution properties and cross-era sentinels).

## Coexistence with `mod-ah-bot`

`mod-ah-bot` may remain installed as a fallback, but its **seller and buyer must
be off** (`AuctionHouseBot.EnableSeller = 0`, `AuctionHouseBot.EnableBuyer = 0`)
or it will list ungated items alongside this module. Use a **different bot
account** for each so their mail scripts never see each other's GUIDs.

## Build / runtime status

The source implements Phases 0–7 of the base plan plus the progression addendum.
Runtime-dependent acceptance (Phase 8/9: soak tests, composition CSV, p99 pass
timing, `wow-manage.sh` wiring) must be run against a built server — see
`NOTES-verification.md` for what was verified statically vs. deferred to runtime.

### Notes on layout vs. the plan

- Progression resolution lives in `CMangosAHBotProgression.{h,cpp}`; the loot id
  vectors live on the `CMangosAHBot` singleton rather than in the config object.
  Functionally equivalent to the plan's §3 file split.
- The gold sink uses both the auction-mail-suppression hooks *and* a `MailScript`
  (belt and suspenders).
- Value-matrix defaults are approximations; copy CMaNGOS's exact rows for a
  faithful economy.
