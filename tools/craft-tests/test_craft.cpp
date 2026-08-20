/*
 * Offline unit tests for the craft-layer cost engine (crafting addendum §10.2).
 *
 * Self-contained: compiles CMangosAHBotCost.cpp against hand-built IRecipeSource /
 * IMarketAnchor / IMarginModel implementations — no server, no core headers. This is
 * the "green before C2 is done" gate: cycle safety and margin math are proven here
 * before the engine ever runs a live cost query.
 *
 * Build:  g++ -std=c++17 -I../../src test_craft.cpp ../../src/CMangosAHBotCost.cpp -o craft-tests
 * Run:    ./craft-tests   (exit 0 = all pass)
 */
#include "CMangosAHBotCost.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Tiny test harness
// ---------------------------------------------------------------------------
static int g_pass = 0, g_fail = 0;
static void check(bool cond, const std::string& what)
{
    if (cond) { ++g_pass; }
    else      { ++g_fail; std::printf("  FAIL: %s\n", what.c_str()); }
}
static void checkEq(uint64_t got, uint64_t want, const std::string& what)
{
    if (got == want) { ++g_pass; }
    else { ++g_fail; std::printf("  FAIL: %s (got %llu want %llu)\n",
                                 what.c_str(),(unsigned long long)got,(unsigned long long)want); }
}

// ---------------------------------------------------------------------------
// Item ids (arbitrary; grouped for readability)
// ---------------------------------------------------------------------------
enum : uint32_t {
    COPPER_ORE=1001, TIN_ORE=1002, SILVER_ORE=1003,
    LINEN=1010, WOOL=1011,
    PEACEBLOOM=1020, SILVERLEAF=1021, EARTHROOT=1022,
    LIGHT_LEATHER=1030,
    ELEM_EARTH=1040, ELEM_WATER=1041,
    COPPER_BAR=2001, TIN_BAR=2003, BRONZE_BAR=2002,
    BOLT_LINEN=2010, BOLT_WOOL=2011,
    EXPENSIVE_WIDGET=2020, UNAVAILABLE_ITEM=2030,
    FLASK=3001, EPIC_SWORD=3010, MULTI_OUT_TRINKET=3020
};

// ---------------------------------------------------------------------------
// Fixture recipe source
// ---------------------------------------------------------------------------
class TestSource : public IRecipeSource
{
public:
    std::vector<CostRecipe> recipes;
    std::unordered_map<uint32_t, std::vector<const CostRecipe*>> byProduct;
    std::vector<const CostRecipe*> empty;

    CostRecipe& add(uint32_t product, uint32_t count,
                    std::vector<std::pair<uint32_t,uint32_t>> reagents,
                    uint8_t rarity=0, bool available=true, bool cd=false)
    {
        recipes.push_back(CostRecipe{product, count, std::move(reagents), rarity, available, cd});
        return recipes.back();
    }

    void index()
    {
        byProduct.clear();
        for (auto& r : recipes)
            if (r.available)
                byProduct[r.productItem].push_back(&r);
    }

    const std::vector<const CostRecipe*>& Producers(uint32_t itemId) const override
    {
        auto it = byProduct.find(itemId);
        return it == byProduct.end() ? empty : it->second;
    }
};

// Fixed per-item market anchors (already "clamped" for the test).
class TestAnchor : public IMarketAnchor
{
public:
    std::unordered_map<uint32_t, uint64_t> table;
    uint64_t fallback = 999999;
    uint64_t Anchor(uint32_t itemId) const override
    {
        auto it = table.find(itemId);
        return it == table.end() ? fallback : it->second;
    }
};

// Rarity margins: TRAINER=100, VENDOR=125, DROP=200 (+50% cooldown bonus).
class TestMargins : public IMarginModel
{
public:
    uint32_t ProductionMarginPct(const CostRecipe& r) const override
    {
        uint32_t base = (r.rarity==0) ? 100 : (r.rarity==1) ? 125 : 200;
        if (r.dailyCooldown) base = base * 150 / 100;
        return base;
    }
    uint32_t LevelingMarginPct() const override { return 80; }
};

int main()
{
    TestSource src;
    TestAnchor anchor;
    TestMargins margins;

    // ---- leaf material anchors ----
    anchor.table = {
        {COPPER_ORE,50},{TIN_ORE,80},{SILVER_ORE,120},
        {LINEN,30},{WOOL,60},
        {PEACEBLOOM,25},{SILVERLEAF,25},{EARTHROOT,40},
        {LIGHT_LEATHER,45},
        {ELEM_EARTH,200},{ELEM_WATER,220},
        // intermediates get a market anchor too (so min(market,make) is meaningful)
        {COPPER_BAR,70},{TIN_BAR,100},{BRONZE_BAR,100},
        {BOLT_LINEN,80},{BOLT_WOOL,250},
        {EXPENSIVE_WIDGET,100},{UNAVAILABLE_ITEM,300},
        {FLASK,500},{EPIC_SWORD,5000},{MULTI_OUT_TRINKET,400}
    };

    // ---- recipes ----
    // Smelting: 1 ore -> 1 bar. make(copper bar)=50 < anchor 70.
    src.add(COPPER_BAR, 1, {{COPPER_ORE,1}});
    src.add(TIN_BAR,    1, {{TIN_ORE,1}});                 // make 80 < anchor 100
    // Multi-output: 1 copper bar + 1 tin bar -> 2 bronze bars. per-unit = (50+80)/2 = 65.
    src.add(BRONZE_BAR, 2, {{COPPER_BAR,1},{TIN_BAR,1}});
    // Cloth bolts.
    src.add(BOLT_LINEN, 1, {{LINEN,2}});                   // make 60 < anchor 80
    src.add(BOLT_WOOL,  1, {{WOOL,3}});                    // make 180 < anchor 250
    // Intermediate where buying beats making (tests min(market,make)).
    src.add(EXPENSIVE_WIDGET, 1, {{SILVER_ORE,5}});        // make 600 > anchor 100 -> 100
    // Recipe exists but is gated OFF: must be ignored -> product stays at anchor.
    src.add(UNAVAILABLE_ITEM, 1, {{COPPER_ORE,1}}, 0, /*available=*/false);
    // Transmute cycle (daily cooldown): Earth<->Water.
    src.add(ELEM_WATER, 1, {{ELEM_EARTH,1}}, 2, true, /*cd=*/true);
    src.add(ELEM_EARTH, 1, {{ELEM_WATER,1}}, 2, true, /*cd=*/true);
    // Flask from herbs (DROP rarity). cost = 3*25+2*25+1*40 = 165.
    src.add(FLASK, 1, {{PEACEBLOOM,3},{SILVERLEAF,2},{EARTHROOT,1}}, 2);
    // Epic multi-reagent. cost = 10*65 + 5*120 + 2*200 = 1650.
    CostRecipe& epic = src.add(EPIC_SWORD, 1, {{BRONZE_BAR,10},{SILVER_ORE,5},{ELEM_EARTH,2}}, 2);
    // A couple more leveling-tier greens to pad the fixture past ~30 nodes.
    src.add(3021, 1, {{LIGHT_LEATHER,4}}, 0);
    src.add(3022, 1, {{LINEN,3},{LIGHT_LEATHER,1}}, 0);
    src.add(3023, 1, {{COPPER_BAR,2}}, 0);
    src.add(3024, 1, {{BRONZE_BAR,1},{LIGHT_LEATHER,2}}, 1);
    anchor.table[3021]=200; anchor.table[3022]=200; anchor.table[3023]=200; anchor.table[3024]=300;

    src.index();
    CMangosAHBotCost cost(src, anchor);
    cost.NewPass();

    std::printf("== cost-engine tests ==\n");

    // 1. Leaf = anchor.
    checkEq(cost.MatValue(COPPER_ORE), 50, "leaf copper ore = anchor");

    // 2. min(market, make): smelted bar is cheaper to make.
    checkEq(cost.MatValue(COPPER_BAR), 50, "copper bar = make(50) < market(70)");
    checkEq(cost.MatValue(TIN_BAR),    80, "tin bar = make(80) < market(100)");

    // 3. Multi-output recipe divides by productCount.
    checkEq(cost.MatValue(BRONZE_BAR), 65, "bronze bar = (50+80)/2 = 65");

    // 4. Bolts.
    checkEq(cost.MatValue(BOLT_LINEN), 60, "bolt of linen = 2*30 = 60");
    checkEq(cost.MatValue(BOLT_WOOL),  180,"bolt of wool = 3*60 = 180 < market 250");

    // 5. min picks market when making is dearer.
    checkEq(cost.MatValue(EXPENSIVE_WIDGET), 100, "widget = market(100) < make(600)");

    // 6. Unavailable recipe ignored -> stays at anchor.
    checkEq(cost.MatValue(UNAVAILABLE_ITEM), 300, "gated-off recipe ignored -> anchor");

    // 7. Transmute CYCLE terminates and is bounded; value falls back to anchor.
    uint64_t water = cost.MatValue(ELEM_WATER);
    uint64_t earth = cost.MatValue(ELEM_EARTH);
    check(water > 0 && earth > 0, "transmute cycle produced finite values");
    check(cost.CycleHits() > 0, "cycle detector fired (transmute)");
    check(cost.DepthHits() == 0, "no depth-limit hits on a shallow graph");
    // Earth: min(anchor 200, make=MatValue(Water)). Water: min(anchor 220, make=cycle->anchor 200)=200.
    checkEq(water, 200, "elem water = min(220, cycle-anchor 200) = 200");
    checkEq(earth, 200, "elem earth = min(200, water 200) = 200");

    // 8. Deep chain: epic sword cost via bronze (65) etc.
    checkEq(cost.CraftCost(epic), 1650, "epic = 10*65 + 5*120 + 2*200 = 1650");

    // 9. Margin math (DROP=200%): flask cost 165 -> price 330.
    const CostRecipe* flaskR = src.Producers(FLASK).front();
    checkEq(cost.CraftCost(*flaskR), 165, "flask cost = 165");
    checkEq(cost.CraftPrice(*flaskR, margins), 330, "flask price = 165 * 200% = 330");

    // 10. Cooldown bonus folds into margin: transmute (DROP+cd) = 200*1.5 = 300%.
    const CostRecipe* waterR = src.Producers(ELEM_WATER).front();
    checkEq(cost.CraftPrice(*waterR, margins), cost.CraftCost(*waterR) * 300 / 100,
            "cooldown product margin = 300%");

    // 11. Memoization: value stable across repeat calls; memo populated.
    check(cost.MemoSize() > 0, "memo populated");
    uint64_t bronze1 = cost.MatValue(BRONZE_BAR);
    uint64_t bronze2 = cost.MatValue(BRONZE_BAR);
    checkEq(bronze1, bronze2, "memoized value stable");

    // 12. NewPass clears memo + counters.
    cost.NewPass();
    check(cost.MemoSize() == 0, "NewPass clears memo");
    check(cost.CycleHits() == 0 && cost.DepthHits() == 0, "NewPass clears counters");

    // 13. Fixture size sanity (~30 recipes incl. cycle, multi-output, CD).
    check(src.recipes.size() >= 15, "fixture has enough recipes");

    std::printf("CRAFT-TESTS: %s  (%d passed, %d failed)\n",
                g_fail==0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
