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
