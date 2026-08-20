# craft-tests — offline cost-engine unit tests

Self-contained unit tests for the craft-layer **cost engine** (`CMangosAHBotCost`),
per crafting addendum §10.2. No server, no AzerothCore headers — the engine talks
only to the `IRecipeSource` / `IMarketAnchor` / `IMarginModel` facade, so it links
against a hand-built ~30-recipe fixture that includes a **transmute cycle**, a
**multi-output** recipe, and a **cooldown** recipe.

## Run

Directly with a compiler (fastest):

```sh
g++ -std=c++17 -I../../src test_craft.cpp ../../src/CMangosAHBotCost.cpp -o craft-tests
./craft-tests            # exit 0 = all pass; prints "CRAFT-TESTS: PASS (N passed, 0 failed)"
```

Or via CMake:

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## What it asserts

- leaf material = market anchor
- `min(market, make)` for intermediates (both directions)
- multi-output recipes divide cost by `productCount`
- **transmute cycle terminates**, falls back to the market anchor, cycle counter > 0,
  no depth-limit hits on a shallow graph
- deep multi-reagent chain (epic) costs out correctly
- margin math (`cost × margin%`) incl. the cooldown bonus
- memoization stability and `NewPass()` reset

This is the "green before C2 is done" gate: cycle safety and margin correctness are
proven here before the engine runs a single live cost query.
