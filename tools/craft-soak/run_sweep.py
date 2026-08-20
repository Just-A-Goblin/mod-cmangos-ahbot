#!/usr/bin/env python3
"""
Craft-layer progression sweep driver (crafting addendum §10.4 / §10.5).

For each progression state it: rewrites the module conf (static state N, craft on, a
seeded RNG, and a Craft.DumpFile path), restarts the worldserver, waits for the module
to write its simulated craft-dump CSV at startup, then runs pandas assertions on the
distribution *properties* (never exact contents) and the cross-era sentinels. Baselines
are archived for regression diffing.

This deliberately drives the server via config + restart + the module's startup dump
(NOT SOAP/console), so it needs no remote command interface. Assertions are tolerant of
tuning drift; they check shape, gating, and margin sanity.

Usage:
    python3 run_sweep.py                 # default demo sweep: states 0 8 13
    python3 run_sweep.py 0 4 7 8 13 16 18 # the full §10.5 sweep

Requires: pandas.  Paths assume the deployment in cmangos-ahbot-deployment memory.
"""
import os, sys, time, shutil, subprocess, tempfile
import pandas as pd

WOW        = "/home/leo/wow"
CONF       = f"{WOW}/server/etc/modules/cmangos_ahbot.conf"
LOG        = f"{WOW}/worldserver.log"
BASELINES  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "baselines")
SEED       = 12345

# Cross-era sentinels (addendum §8 / §12). Raw mats + crafted goods that MUST NOT appear
# below their unlock state.
RAW_SENTINELS   = {21877: "Netherweave Cloth", 33470: "Frostweave Cloth",
                   23424: "Fel Iron Ore", 36912: "Saronite Ore"}
CRAFT_SENTINELS = {21841: ("Netherweave Bag", 8), 43575: ("Glacial Bag", 13),
                   46376: ("Flask of the Frost Wyrm", 13)}
TBC_AT, WOTLK_AT = 8, 13


def sh(cmd):
    subprocess.run(cmd, shell=True, check=False)


def set_conf(state, dump_path):
    """Rewrite the craft/progression keys for this sweep step (idempotent-ish)."""
    with open(CONF) as f:
        lines = f.readlines()
    keys = {
        "CMangosAHBot.Progression.Source": "0",
        "CMangosAHBot.Progression.Static": str(state),
        "CMangosAHBot.Craft.Enable": "1",
        "CMangosAHBot.Craft.Seed": str(SEED),
        "CMangosAHBot.Craft.DumpFile": f'"{dump_path}"',
    }
    seen = set()
    out = []
    for ln in lines:
        k = ln.split("=")[0].strip()
        if k in keys:
            out.append(f"{k} = {keys[k]}\n"); seen.add(k)
        else:
            out.append(ln)
    for k, v in keys.items():
        if k not in seen:
            out.append(f"{k} = {v}\n")
    with open(CONF, "w") as f:
        f.writelines(out)


def restart_and_wait(dump_path, timeout=240):
    if os.path.exists(dump_path):
        os.remove(dump_path)
    sh(f"{WOW}/stop.sh")
    sh(f"{WOW}/start.sh")
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(dump_path) and os.path.getsize(dump_path) > 0:
            time.sleep(2)  # let the write finish
            return True
        time.sleep(3)
    return False


def assert_state(state, df):
    """Distribution-property + sentinel assertions for one state. Returns (passed, msgs)."""
    msgs, ok = [], True

    def check(cond, label):
        nonlocal ok
        msgs.append(("PASS" if cond else "FAIL") + f": {label}")
        ok = ok and cond

    check(len(df) > 0, f"state {state}: dump non-empty ({len(df)} rows)")
    if df.empty:
        return ok, msgs

    items = set(df["item"])
    cats = df["category"].value_counts(normalize=True)

    # 1) Cross-era gating: no sentinel product appears below its unlock state.
    for iid, name in RAW_SENTINELS.items():
        check(iid not in items, f"raw sentinel absent: {name} ({iid})")
    for iid, (name, unlock) in CRAFT_SENTINELS.items():
        if state < unlock:
            check(iid not in items, f"crafted sentinel absent pre-state-{unlock}: {name}")

    # 2) Era category shift: GEM_CUT (JC) only from TBC; SCROLL never (inert).
    check(("GEM_CUT" in cats.index) == (state >= TBC_AT),
          f"GEM_CUT present iff state>=TBC ({state>=TBC_AT})")
    check("SCROLL" not in cats.index, "SCROLL has no supply (inert)")

    # 3) Fresh-server glut: at state 0 the leveling intermediates/ammo dominate.
    if state == 0:
        glut = cats.get("INTERMEDIATE", 0) + cats.get("AMMO", 0)
        check(glut >= 0.20, f"state 0 leveling glut share {glut:.2f} >= 0.20")

    # 4) Margin sanity: price/cost ratios sit in a plausible band per category
    #    (leveling floor 60% .. drop max 300% x variance ~330%). Check the median.
    r = df[df["unit_matcost"] > 0]
    if len(r):
        med = r["ratio_pct"].median()
        check(40 <= med <= 350, f"median price/cost ratio {med:.0f}% within [40,350]")
        check((r["ratio_pct"] >= 30).mean() > 0.98, "≥98% of listings priced >= floor-ish (30%)")

    return ok, msgs


def main():
    states = [int(x) for x in sys.argv[1:]] or [0, 8, 13]
    os.makedirs(BASELINES, exist_ok=True)
    shutil.copy(CONF, CONF + ".sweep-backup")
    all_ok = True
    try:
        for st in states:
            dump = os.path.join(tempfile.gettempdir(), f"craft_dump_state{st}.csv")
            print(f"\n=== state {st}: reconfigure + restart ===")
            set_conf(st, dump)
            if not restart_and_wait(dump):
                print(f"FAIL: state {st}: dump file never appeared"); all_ok = False; continue
            df = pd.read_csv(dump)
            ok, msgs = assert_state(st, df)
            for m in msgs:
                print("  " + m)
            shutil.copy(dump, os.path.join(BASELINES, f"craft_dump_state{st}.csv"))
            all_ok = all_ok and ok
    finally:
        shutil.move(CONF + ".sweep-backup", CONF)
        print("\nconf restored. (Restart once more to apply the restored conf if needed.)")
    print("\nSWEEP:", "PASS" if all_ok else "FAIL")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
