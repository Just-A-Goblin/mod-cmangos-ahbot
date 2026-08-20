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
#include "CMangosAHBotCraft.h"
#include <cstdio>
#include <cstdint>
#include <random>
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

    // -----------------------------------------------------------------------
    // Craft-sim layer (§4): skill-up chance, glut chooser, skill-ups, population
    // -----------------------------------------------------------------------
    std::printf("== craft-sim tests ==\n");
    CraftSkillChances chances; // AC defaults 0/25/75/100

    // 14. Skill-up chance bands (C-A2). grey=100, yellow=25 -> green=62.
    checkEq(CraftSim::SkillGainChancePerMille(10, 100, 25, chances), 1000, "S<yellow -> orange 100%");
    checkEq(CraftSim::SkillGainChancePerMille(30, 100, 25, chances), 750,  "yellow<=S<green -> 75%");
    checkEq(CraftSim::SkillGainChancePerMille(70, 100, 25, chances), 250,  "green<=S<grey -> 25%");
    checkEq(CraftSim::SkillGainChancePerMille(100,100, 25, chances), 0,    "S>=grey -> 0%");

    // 15. Glut chooser: at S=50 the cheapest trainable recipe wins the most weight,
    //     and grey/too-high recipes are never chosen. Values come out of the fixture
    //     anchors (COPPER_ORE=50, SILVER_ORE=120) via the cost engine.
    auto mkRecipe = [](uint32_t product, std::vector<std::pair<uint32_t,uint32_t>> reag,
                       uint16_t minS, uint16_t yellow, uint16_t grey) {
        CraftRecipe r; r.productItem=product; r.productCount=1; r.reagents=std::move(reag);
        r.minSkill=minS; r.yellowSkill=yellow; r.greySkill=grey; r.available=true; return r;
    };
    CraftRecipe cheapBar   = mkRecipe(5001, {{COPPER_ORE,1}}, 1, 25, 100);   // matcost 50
    CraftRecipe dearTrinket= mkRecipe(5002, {{SILVER_ORE,2}}, 1, 25, 100);   // matcost 240
    CraftRecipe alreadyGrey= mkRecipe(5003, {{COPPER_ORE,1}}, 1, 10, 40);    // grey at S=50
    CraftRecipe tooHigh    = mkRecipe(5004, {{COPPER_ORE,1}}, 80, 100, 200); // minSkill 80
    std::vector<const CraftRecipe*> cands = {&cheapBar,&dearTrinket,&alreadyGrey,&tooHigh};

    cost.NewPass();
    check(CraftSim::LevelingScore(cheapBar, 50, cost, chances) >
          CraftSim::LevelingScore(dearTrinket, 50, cost, chances),
          "cheaper recipe scores higher (skill-up per gold)");
    checkEq((uint64_t)(CraftSim::LevelingScore(alreadyGrey,50,cost,chances)*1000), 0, "grey recipe scores 0");
    checkEq((uint64_t)(CraftSim::LevelingScore(tooHigh,50,cost,chances)*1000), 0, "too-high recipe scores 0");

    std::mt19937 rng(999);
    int cntCheap=0, cntDear=0, cntOther=0;
    for (int i=0;i<2000;++i) {
        const CraftRecipe* pick = CraftSim::ChooseLevelingRecipe(cands, 50, cost, chances, rng);
        if (pick==&cheapBar) ++cntCheap; else if (pick==&dearTrinket) ++cntDear; else ++cntOther;
    }
    check(cntCheap > cntDear, "cheapest recipe chosen most often (emergent glut)");
    checkEq((uint64_t)cntOther, 0, "grey/too-high recipes never chosen");

    // 16. Skill-ups advance skill, bounded by cap; none once grey.
    Crafter cr{171, 10, 300};
    uint32_t ups = CraftSim::SimulateSkillUps(cr, cheapBar, 200, chances, rng);
    check(ups > 0 && cr.skill == 10 + ups, "skill-ups advance skill by the count returned");
    Crafter atGrey{171, 150, 300};
    checkEq(CraftSim::SimulateSkillUps(atGrey, cheapBar, 100, chances, rng), 0, "no skill-ups past grey");

    // 17. Population roll: right size, skills in [1,cap], professions from weights,
    //     and BOTH levelers and at-cap producers exist (atCapFraction=0.4).
    auto pop = CraftSim::RollPopulation(500, {{171,3},{164,1}}, 300, 2.0, 3.0, 0.4, rng);
    checkEq((uint64_t)pop.size(), 500, "population size honored");
    bool skillsOk=true, profOk=true; uint32_t below=0, atcap=0;
    for (auto& c : pop) {
        if (c.skill < 1 || c.skill > 300) skillsOk=false;
        if (c.skillLine!=171 && c.skillLine!=164) profOk=false;
        if (c.leveling()) ++below; else ++atcap;
    }
    check(skillsOk, "all skills within [1,cap]");
    check(profOk, "all professions from the weight table");
    check(below > 0, "population has below-cap levelers (the glut)");
    check(atcap > 0, "population has at-cap producers (bags/flasks/gems)");
    // ~40% should be at cap; allow slack for the RNG.
    check(atcap > 500*0.25 && atcap < 500*0.55, "at-cap producer fraction ~= 0.4");

    // 18. Production chooser (§5.2): category weights steer selection; GEAR ilvl
    //     window gates; override weight 0 forbids.
    CraftRecipe pFlask = mkRecipe(6001, {{PEACEBLOOM,2}}, 300, 300, 300);
    pFlask.category = CAT_FLASK;      pFlask.productIlvl = 75;
    CraftRecipe pGearIn = mkRecipe(6002, {{COPPER_ORE,3}}, 300, 300, 300);
    pGearIn.category = CAT_GEAR;      pGearIn.productIlvl = 66;   // inside [40,66]
    CraftRecipe pGearOut = mkRecipe(6003, {{COPPER_ORE,3}}, 300, 300, 300);
    pGearOut.category = CAT_GEAR;     pGearOut.productIlvl = 200; // outside window
    std::vector<const CraftRecipe*> prod = {&pFlask,&pGearIn,&pGearOut};
    uint32_t wFlaskOnly[CAT_COUNT] = {0}; wFlaskOnly[CAT_FLASK] = 100;
    uint32_t wGearOnly[CAT_COUNT]  = {0}; wGearOnly[CAT_GEAR] = 100;
    auto boost = [](uint8_t){ return 100u; };
    auto ovr   = [](uint32_t){ return 100u; };

    int fFlask=0; for (int i=0;i<500;++i) if (CraftSim::ChooseProductionRecipe(prod,wFlaskOnly,66,26,boost,ovr,rng)==&pFlask) ++fFlask;
    checkEq((uint64_t)fFlask, 500, "FLASK-only weights -> always flask");
    int inWin=0,outWin=0; for (int i=0;i<500;++i){ auto* p=CraftSim::ChooseProductionRecipe(prod,wGearOnly,66,26,boost,ovr,rng); if(p==&pGearIn)++inWin; else if(p==&pGearOut)++outWin; }
    check(inWin > 0, "GEAR in ilvl window is chosen");
    checkEq((uint64_t)outWin, 0, "GEAR outside ilvl window never chosen");
    auto ovrZero = [](uint32_t){ return 0u; };
    check(CraftSim::ChooseProductionRecipe(prod,wFlaskOnly,66,26,boost,ovrZero,rng)==nullptr, "override weight 0 forbids");

    // 19. Saturation curve endpoints (§6.4): 100% up to cap, floor at 3*cap, linear between.
    checkEq(CraftSim::SaturationMultPct(0, 20, 30), 100, "saturation: below cap = 100%");
    checkEq(CraftSim::SaturationMultPct(20, 20, 30), 100, "saturation: at cap = 100%");
    checkEq(CraftSim::SaturationMultPct(60, 20, 30), 30, "saturation: at 3x cap = floor 30%");
    checkEq(CraftSim::SaturationMultPct(80, 20, 30), 30, "saturation: past 3x cap = floor");
    checkEq(CraftSim::SaturationMultPct(40, 20, 30), 65, "saturation: midpoint (2x cap) = 65%");
    checkEq(CraftSim::SaturationMultPct(10, 0, 30), 100, "saturation: cap 0 => neutral 100%");

    // 20. Stack split by category (§7.2).
    { auto s = CraftSim::SplitStacks(5, CAT_GEAR, 20, rng);
      bool allOne = s.size()==5; for (auto x : s) if (x!=1) allOne=false;
      check(allOne, "GEAR -> 5 single stacks"); }
    { auto s = CraftSim::SplitStacks(500, CAT_AMMO, 200, rng);
      uint32_t sum=0; bool full=true; for (size_t i=0;i<s.size();++i){ sum+=s[i]; if(i+1<s.size() && s[i]!=200) full=false; }
      check(sum==500 && full, "AMMO -> full 200-stacks summing to 500"); }
    { auto s = CraftSim::SplitStacks(30, CAT_FLASK, 20, rng);
      uint32_t sum=0; bool sizesOk=true; for (auto x : s){ sum+=x; if(x!=5 && x!=20 && x>20) sizesOk=false; }
      check(sum==30 && sizesOk, "FLASK -> mixed 5s/20s summing to 30"); }
    { auto s = CraftSim::SplitStacks(250, CAT_INTERMEDIATE, 200, rng);
      uint32_t sum=0; for (auto x : s) sum+=x;
      check(sum==250 && s.size()>=2, "INTERMEDIATE -> full stack(s) + ragged, summing to 250"); }

    // 21. Textured listings (§7): 2-4 rungs, per-stack variance, never below floor.
    { auto t = CraftSim::TexturedListings(5, CAT_GEAR, 20, 1000, 500, 10, rng);
      check(t.size()==5, "GEAR texture -> 5 single listings");
      std::vector<uint64_t> distinct; bool floorOk=true;
      for (auto& [st,pr] : t){ if(pr<500) floorOk=false; bool seen=false; for(auto d:distinct) if(d==pr) seen=true; if(!seen) distinct.push_back(pr); }
      check(floorOk, "no textured price below floor");
      check(distinct.size()>=2 && distinct.size()<=5, "GEAR shows a 2-4 rung ladder"); }
    { auto t = CraftSim::TexturedListings(10, CAT_AMMO, 200, 40, 100, 10, rng); // basePrice < floor
      bool floorOk=true; for (auto& [st,pr] : t) if (pr<100) floorOk=false;
      check(floorOk, "basePrice below floor -> all listings clamped to floor"); }

    std::printf("CRAFT-TESTS: %s  (%d passed, %d failed)\n",
                g_fail==0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
