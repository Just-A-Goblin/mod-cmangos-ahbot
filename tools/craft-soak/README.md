# craft-soak — progression sweep driver

Black-box distribution verification for the craft layer (crafting addendum §10.4 / §10.5).

`run_sweep.py` drives the **live worldserver** through a set of progression states and
asserts **distribution properties** (never exact contents) plus the cross-era sentinels.

## How it works (no SOAP needed)

For each state it:

1. Rewrites the module conf (`Progression.Static=N`, `Craft.Enable=1`, `Craft.Seed=12345`,
   `Craft.DumpFile=<tmp>/craft_dump_stateN.csv`).
2. Restarts the server (`stop.sh` / `start.sh`).
3. Waits for the module to write its **simulated craft-dump CSV** at startup
   (`item,name,category,rarity,stack,unit_price,unit_matcost,ratio_pct,leveling`).
4. Loads the CSV with pandas and asserts:
   - **cross-era sentinels absent** below their unlock state (raw: Netherweave/Frostweave
     Cloth, Fel Iron/Saronite Ore; crafted: Netherweave/Glacial Bag, Frost Wyrm flask),
   - **era category shift** (GEM_CUT appears iff state ≥ TBC; SCROLL never — inert),
   - **fresh-server leveling glut** at state 0 (INTERMEDIATE+AMMO share),
   - **margin sanity** (median price/cost ratio in band; ~all listings ≥ floor).
5. Archives the CSV under `baselines/` for regression diffing.

The conf is restored at the end.

## Run

```sh
pip install pandas
python3 run_sweep.py                    # demo sweep: states 0 8 13
python3 run_sweep.py 0 4 7 8 13 16 18   # full §10.5 sweep
```

Each state is a full server restart (~1–2 min), so the full 7-state sweep is ~10–15 min.

## Notes

- The dump is a **non-mutating production+leveling simulation** at the configured state
  (it does not fill the live AH), so the sweep is safe to run without disturbing auctions.
- For a **live-AH** dump instead, use the in-game/console command `.cmahbot craft dump <file>`
  (dumps bot-owned auctions that are craft products).
- Assertions are tolerant of tuning drift; they check shape, gating, and margin sanity —
  the exact gluts/ratios are expected to move as the config is tuned (C7).
