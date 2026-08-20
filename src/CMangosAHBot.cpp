/*
 * CMangosAHBot — loot-simulation seller + buyer + progression gating.
 *
 * Mechanism and pricing formulas ported from cmangos/mangos-classic
 * src/game/AuctionHouseBot/ (GPL-2.0). Progression gating per
 * mod-cmangos-ahbot-progression-addendum_1.md. AzerothCore integration
 * (transient Player, auction creation, hooks) verified in NOTES-verification.md.
 */
#include "CMangosAHBot.h"
#include "CMangosAHBotConfig.h"
#include "CMangosAHBotRng.h"
#include "AuctionHouseMgr.h"
#include "AuctionHouseSearcher.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "LootMgr.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "DBCStores.h"
#include "Player.h"
#include "WorldSession.h"
#include "ObjectAccessor.h"
#include "MapMgr.h"
#include "World.h"
#include "Random.h"
#include "Log.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>
#include <sstream>

namespace
{
    constexpr uint32_t NOCAP = std::numeric_limits<uint32_t>::max();

    // creature_template.rank -> config tuple index
    //   0 Normal->0, 4 Rare->1, 1 Elite->2, 2 RareElite->3, 3 WorldBoss->4
    inline int RankToBucket(uint8_t rank)
    {
        switch (rank)
        {
            case 0: return 0;
            case 4: return 1;
            case 1: return 2;
            case 2: return 3;
            case 3: return 4;
            default: return 0;
        }
    }

    inline void MinMerge(std::unordered_map<uint32_t, uint8_t>& m, uint32_t key, uint8_t exp)
    {
        auto it = m.find(key);
        if (it == m.end() || exp < it->second)
            m[key] = exp;
    }
}

CMangosAHBot* CMangosAHBot::instance()
{
    static CMangosAHBot inst;
    return &inst;
}

// ===========================================================================
// Lifecycle
// ===========================================================================

bool CMangosAHBot::ResolveBotCharacter()
{
    const auto& cfg = gCMangosAHBotConfig;
    if (cfg.account == 0 || cfg.guid == 0)
    {
        LOG_ERROR("module", "CMangosAHBot: Account/GUID unset — module stays disabled.");
        return false;
    }
    QueryResult r = CharacterDatabase.Query(
        "SELECT guid FROM characters WHERE account = {} AND guid = {}", cfg.account, cfg.guid);
    if (!r)
    {
        LOG_ERROR("module", "CMangosAHBot: bot character guid {} not found on account {} — module stays disabled.",
                  cfg.guid, cfg.account);
        return false;
    }
    return true;
}

void CMangosAHBot::Initialize()
{
    const auto& cfg = gCMangosAHBotConfig;

    if (!ResolveBotCharacter())
    {
        _ready = false;
        return;
    }

    if (!_sourcesBuilt)
    {
        BuildVendorSet();
        BuildClassifiedSources();
        BuildItemExpansion();
        BuildProfessionPool();
        BuildDisenchantPool();
        LoadOverrides();
        _sourcesBuilt = true;
    }

    // Craft layer (crafting addendum): build the recipe graph once, before the first
    // progression refresh so its availability mask is computed with the initial caps.
    // Gated on Craft.Enable — with it off, none of this runs and the seller/buyer path
    // is byte-for-byte the base module (constraint #1).
    if (cfg.craftEnable && !_craftBuilt)
    {
        sCMangosAHBotRng->Seed(cfg.craftSeed);
        _craftGraph.Build(cfg, _vendorItems, _itemExpansion);
        _craftBuilt = true;
        // The legacy Items.Profession source stays active until craft sessions produce
        // listings (C3); only then is it retired (double-listing guard). Note it here.
        LOG_INFO("module", "CMangosAHBot[craft]: graph ready (seed={}). Legacy Items.Profession "
                 "path remains active until craft production lands (C3).", sCMangosAHBotRng->Seeded());
    }

    RefreshProgression(true); // sets caps, builds filtered vectors, logs sizes, masks craft graph

    // Roll the crafter population once caps/candidates exist (C3, §4.1).
    if (_craftBuilt)
        BuildPopulation();

    // Guardrail (plan §6): a critical empty vector means a source contributes nothing.
    bool anyEmpty = _creature[0].empty() || _fishing.empty() || _gameobject.empty() ||
                    _skinning.empty() || _disenchant.empty() || _profession.empty();
    if (cfg.enable && anyEmpty)
    {
        LOG_ERROR("module", "CMangosAHBot: a critical loot vector is empty at current progression "
            "(creatureN={} fishing={} go={} skin={} disen={} prof={}). Disabling — check source mapping.",
            _creature[0].size(), _fishing.size(), _gameobject.size(),
            _skinning.size(), _disenchant.size(), _profession.size());
        _ready = false;
        return;
    }

    _ready = (cfg.enable != 0);
    LOG_INFO("module", "CMangosAHBot: initialized (ready={} state={} maxExp={} ilvlCap={}).",
             _ready, _caps.state, _caps.maxExpansion,
             _caps.itemLevelCap == NOCAP ? 0 : _caps.itemLevelCap);

    if (_craftBuilt)
        CraftStartupDiagnostics();
}

void CMangosAHBot::CraftStartupDiagnostics()
{
    const auto& cfg = gCMangosAHBotConfig;

    // Selftest at the live state.
    LOG_INFO("module", "CMangosAHBot[craft]: {}", CraftSelfTest());

    // Per-era gating sweep: re-mask the graph for a handful of probe states and log
    // how availability (and the era-marker categories/skills) unlocks. Restored to
    // the live caps afterwards so runtime behavior is unchanged.
    const uint8_t probes[] = { 0, static_cast<uint8_t>(cfg.progTbcAtState),
                               static_cast<uint8_t>(cfg.progWotlkAtState), 18 };
    for (uint8_t s : probes)
    {
        CmAHBCaps c = CMangosAHBotProgression::CapsForState(s);
        _craftGraph.RecomputeMask(c, cfg.progTbcAtState, cfg.progWotlkAtState, cfg.progSkillCaps);
        LOG_INFO("module", "CMangosAHBot[craft]: gate sweep state={} maxExp={} skillCap={} ilvlCap={} "
                 "=> available={}/{} | JC={} Inscription={} GEM_CUT={} SCROLL={} GEAR={} INTERMEDIATE={}",
                 int(s), int(c.maxExpansion), c.skillCap == NOCAP ? 0 : c.skillCap,
                 c.itemLevelCap == NOCAP ? 0 : c.itemLevelCap,
                 _craftGraph.AvailableCount(), _craftGraph.Size(),
                 _craftGraph.SkillLineAvailableCount(SKILL_JEWELCRAFTING),
                 _craftGraph.SkillLineAvailableCount(SKILL_INSCRIPTION),
                 _craftGraph.CategoryCount(CAT_GEM_CUT, true),
                 _craftGraph.CategoryCount(CAT_SCROLL, true),
                 _craftGraph.CategoryCount(CAT_GEAR, true),
                 _craftGraph.CategoryCount(CAT_INTERMEDIATE, true));
    }
    // Restore the live mask.
    _craftGraph.RecomputeMask(_caps, cfg.progTbcAtState, cfg.progWotlkAtState, cfg.progSkillCaps);

    // Reagent-era leak scan (sentinel mats from addendum §8 / constraint #6 sentinel list).
    {
        static const std::pair<uint32_t, const char*> kSentinels[] = {
            {21877,"Netherweave Cloth"},{33470,"Frostweave Cloth"},
            {23424,"Fel Iron Ore"},{36912,"Saronite Ore"}};
        for (auto& [mat, name] : kSentinels)
        {
            auto it = _itemExpansion.find(mat);
            int matExp = it == _itemExpansion.end() ? -1 : int(it->second);
            uint32_t consumers = 0, availConsumers = 0;
            std::string example;
            for (const CraftRecipe& r : _craftGraph.Recipes())
                for (auto& [rid, rc] : r.reagents)
                    if (rid == mat)
                    {
                        ++consumers;
                        if (r.available) { ++availConsumers;
                            if (example.empty())
                            { ItemTemplate const* p = sObjectMgr->GetItemTemplate(r.productItem);
                              example = p ? p->Name1 : std::to_string(r.productItem); } }
                    }
            LOG_INFO("module", "CMangosAHBot[craft]: sentinel {} (id {}) itemExpansion={} — {} recipes consume it, "
                     "{} AVAILABLE at state {} (leak e.g. '{}')", name, mat, matExp, consumers,
                     availConsumers, int(_caps.state), example);
        }
    }

    // Vanilla leveling-glut presence probe: confirm the historically-correct gluts
    // (smelted bars, cloth bolts, bandages) are in the state-0 available pool.
    {
        static const std::pair<uint32_t, const char*> kGlut[] = {
            {2840,"Copper Bar"},{2841,"Bronze Bar"},{3576,"Tin Bar"},
            {2996,"Bolt of Linen Cloth"},{2997,"Bolt of Woolen Cloth"},
            {1251,"Linen Bandage"},{2581,"Heavy Wool Bandage"}};
        std::string present, absent;
        for (auto& [item, name] : kGlut)
        {
            bool avail = false;
            if (const std::vector<uint32_t>* prod = _craftGraph.Producers(item))
                for (uint32_t idx : *prod)
                    if (_craftGraph.Recipes()[idx].available) { avail = true; break; }
            (avail ? present : absent) += std::string(name) + "; ";
        }
        LOG_INFO("module", "CMangosAHBot[craft]: glut presence @state{} — AVAILABLE: {} | ABSENT: {}",
                 int(_caps.state), present.empty() ? "(none)" : present, absent.empty() ? "(none)" : absent);

        // Crafted cross-era sentinels (addendum C4 §12): these MUST be absent below
        // their state — bags/flasks that consume crafted cross-era mats.
        static const std::pair<uint32_t, const char*> kXera[] = {
            {21841,"Netherweave Bag (TBC)"},{43575,"Glacial Bag (WotLK)"},
            {46376,"Flask of the Frost Wyrm (WotLK)"}};
        std::string leaked;
        for (auto& [item, name] : kXera)
        {
            bool avail = false;
            if (const std::vector<uint32_t>* prod = _craftGraph.Producers(item))
                for (uint32_t idx : *prod)
                    if (_craftGraph.Recipes()[idx].available) { avail = true; break; }
            if (avail) leaked += std::string(name) + "; ";
        }
        LOG_INFO("module", "CMangosAHBot[craft]: crafted cross-era sentinels @state{} — LEAKED: {}",
                 int(_caps.state), leaked.empty() ? "(none, correct)" : leaked);
    }

    // C2 hand-check: cost the real graph (5 representative chains + cycle stats).
    {
        std::istringstream ss(CraftCostChains(0));
        std::string line;
        while (std::getline(ss, line))
            LOG_INFO("module", "{}", line);
    }

    // C3 hand-check: a 500-session leveling snapshot (the emergent glut).
    {
        std::istringstream ss(CraftSimulateSessions(500));
        std::string line;
        while (std::getline(ss, line))
            LOG_INFO("module", "{}", line);
    }
}

// ===========================================================================
// Cost engine facade (crafting addendum §3 / §10.2)
// ===========================================================================

namespace
{
    // Concrete IRecipeSource over the singleton's projected producer index.
    struct ProjSource : IRecipeSource
    {
        const std::unordered_map<uint32_t, std::vector<const CostRecipe*>>& producers;
        std::vector<const CostRecipe*> empty;
        explicit ProjSource(const std::unordered_map<uint32_t, std::vector<const CostRecipe*>>& p)
            : producers(p) {}
        const std::vector<const CostRecipe*>& Producers(uint32_t itemId) const override
        {
            auto it = producers.find(itemId);
            return it == producers.end() ? empty : it->second;
        }
    };

    // Median of live bot listings, clamped to [min,max]% of the intrinsic formula;
    // falls back to intrinsic when nothing is listed (§3.1). intrinsic() is the
    // module's CalculateBuyoutPrice, passed in as a function to keep it private.
    struct LiveAnchor : IMarketAnchor
    {
        const std::unordered_map<uint32_t, uint64_t>& medians;
        std::function<uint64_t(uint32_t)> intrinsic;
        uint32_t clampMinPct, clampMaxPct;
        LiveAnchor(const std::unordered_map<uint32_t, uint64_t>& m,
                   std::function<uint64_t(uint32_t)> f, uint32_t lo, uint32_t hi)
            : medians(m), intrinsic(std::move(f)), clampMinPct(lo), clampMaxPct(hi) {}
        uint64_t Anchor(uint32_t itemId) const override
        {
            uint64_t intr = intrinsic(itemId);
            auto it = medians.find(itemId);
            if (it == medians.end())
                return intr > 0 ? intr : 1; // nothing listed -> intrinsic (floor 1)
            uint64_t med = it->second;
            if (intr == 0)
                return med > 0 ? med : 1;
            uint64_t lo = intr * clampMinPct / 100;
            uint64_t hi = intr * clampMaxPct / 100;
            return std::min(std::max(med, lo), hi);
        }
    };

    // Margin band per rarity (production), cooldown bonus folded in.
    std::pair<uint32_t, uint32_t> MarginBand(uint8_t rarity, bool cd)
    {
        const auto& c = gCMangosAHBotConfig;
        uint32_t lo, hi;
        switch (rarity)
        {
            case 0:  lo = c.craftMarginTrainerMin; hi = c.craftMarginTrainerMax; break; // TRAINER
            case 1:  lo = c.craftMarginVendorMin;  hi = c.craftMarginVendorMax;  break; // VENDOR
            default: lo = c.craftMarginDropMin;    hi = c.craftMarginDropMax;    break; // DROP/UNSOURCED
        }
        if (cd) { lo = lo * c.craftMarginCooldownBonus / 100; hi = hi * c.craftMarginCooldownBonus / 100; }
        return { lo, hi };
    }

    struct LiveMargins : IMarginModel
    {
        uint32_t ProductionMarginPct(const CostRecipe& r) const override
        {
            auto [lo, hi] = MarginBand(r.rarity, r.dailyCooldown);
            return sCMangosAHBotRng->Urand(lo, hi);
        }
        uint32_t LevelingMarginPct() const override
        {
            return sCMangosAHBotRng->Urand(gCMangosAHBotConfig.craftMarginLevelingMin,
                                           gCMangosAHBotConfig.craftMarginLevelingMax);
        }
    };
}

void CMangosAHBot::BuildCostProjection()
{
    _costRecipes.clear();
    _costProducers.clear();
    if (!_craftBuilt)
        return;

    // Fill fully first (so pointers stay valid), then index available producers.
    const auto& recipes = _craftGraph.Recipes();
    _costRecipes.reserve(recipes.size());
    for (const CraftRecipe& g : recipes)
    {
        CostRecipe c;
        c.productItem   = g.productItem;
        c.productCount  = g.productCount;
        c.reagents      = g.reagents;
        c.rarity        = static_cast<uint8_t>(g.rarity);
        c.available     = g.available;
        c.dailyCooldown = g.dailyCooldown;
        _costRecipes.push_back(std::move(c));
    }
    for (const CostRecipe& c : _costRecipes)
        if (c.available)
            _costProducers[c.productItem].push_back(&c);
}

void CMangosAHBot::BuildAnchorMedians(std::unordered_map<uint32_t, uint64_t>& out) const
{
    out.clear();
    AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(CMAHB_AH_FIDS[CMAHB_HOUSE_NEUTRAL]);
    if (!ah)
        return;
    std::unordered_map<uint32_t, std::vector<uint64_t>> perUnit;
    for (auto it = ah->GetAuctionsBegin(); it != ah->GetAuctionsEnd(); ++it)
    {
        AuctionEntry* a = it->second;
        if (!a || a->owner.GetCounter() != gCMangosAHBotConfig.guid || a->buyout == 0 || a->itemCount == 0)
            continue;
        perUnit[a->item_template].push_back(uint64_t(a->buyout) / a->itemCount);
    }
    for (auto& [item, v] : perUnit)
    {
        std::sort(v.begin(), v.end());
        out[item] = v[v.size() / 2]; // median (upper-middle for even counts)
    }
}

std::string CMangosAHBot::CraftCostChains(uint32_t sampleN) const
{
    std::ostringstream ss;
    if (!_craftBuilt || _costRecipes.empty())
        return "Craft cost engine: no projection (craft disabled).";

    std::unordered_map<uint32_t, uint64_t> medians;
    BuildAnchorMedians(medians);

    ProjSource src(_costProducers);
    LiveAnchor anchor(medians, [this](uint32_t id) -> uint64_t {
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(id);
        return p ? CalculateBuyoutPrice(p) : 0;
    }, gCMangosAHBotConfig.craftAnchorClampMin, gCMangosAHBotConfig.craftAnchorClampMax);
    LiveMargins margins;
    CMangosAHBotCost cost(src, anchor);

    auto nameOf = [](uint32_t id) -> std::string {
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(id);
        return p ? p->Name1 : std::string("item#") + std::to_string(id);
    };
    auto describe = [&](const char* label, const CraftRecipe* r)
    {
        if (!r) { ss << "\n  " << label << ": (none available at state " << int(_caps.state) << ")"; return; }
        cost.NewPass();
        uint64_t c = 0;
        std::ostringstream mats;
        for (auto& [rid, rc] : r->reagents)
        {
            uint64_t mv = cost.MatValue(rid);
            c += mv * rc;
            mats << nameOf(rid) << " x" << rc << "@" << mv << " ";
        }
        c = r->productCount ? c / r->productCount : c;
        auto [mlo, mhi] = MarginBand(static_cast<uint8_t>(r->rarity), r->dailyCooldown);
        ss << "\n  " << label << ": " << nameOf(r->productItem) << " x" << r->productCount
           << " <= " << mats.str() << "| cost/unit=" << c
           << " rarity=" << RarityName(r->rarity)
           << " margin=" << mlo << "-" << mhi << "% price=" << (c * mlo / 100) << "-" << (c * mhi / 100)
           << (r->dailyCooldown ? " [CD]" : "");
    };

    // Pick 5 representative real chains from available recipes.
    const CraftRecipe *bar=nullptr,*bolt=nullptr,*flask=nullptr,*transmute=nullptr,*epic=nullptr;
    for (const CraftRecipe& r : _craftGraph.Recipes())
    {
        if (!r.available) continue;
        if (!bar && r.skillLine == SKILL_MINING && !r.reagents.empty()) bar = &r;
        if (!bolt && r.category == CAT_INTERMEDIATE && r.skillLine == SKILL_TAILORING) bolt = &r;
        if (!flask && r.category == CAT_FLASK) flask = &r;
        if (!transmute && r.dailyCooldown) transmute = &r;
        if (!epic && r.category == CAT_GEAR && r.reagents.size() >= 3) epic = &r;
    }
    ss << "CMangosAHBot[craft]: cost hand-check (state=" << int(_caps.state)
       << ", anchor medians for " << medians.size() << " items):";
    describe("bar<-ore ", bar);
    describe("bolt<-cloth", bolt);
    describe("flask<-herb", flask);
    describe("transmute ", transmute);
    describe("epic-gear ", epic);

    // Full-graph pass to prove no recursion blowups + count cycles (§3.1 requirement).
    cost.NewPass();
    uint32_t n = 0, limit = sampleN ? sampleN : uint32_t(_costRecipes.size());
    for (const CostRecipe& r : _costRecipes)
    {
        if (!r.available) continue;
        if (n++ >= limit) break;
        cost.MatValue(r.productItem);
    }
    ss << "\nCMangosAHBot[craft]: full-graph cost pass — priced " << n
       << " products, cycleHits=" << cost.CycleHits()
       << " depthHits=" << cost.DepthHits()
       << " memo=" << cost.MemoSize();
    return ss.str();
}

std::string CMangosAHBot::CraftSimulateCost(uint32_t n) const
{
    if (!gCMangosAHBotConfig.craftEnable || !_craftBuilt)
        return "Craft layer disabled (Craft.Enable=0).";
    return CraftCostChains(n);
}

// ===========================================================================
// Craft session layer (crafting addendum §4) — C3 (leveling only)
// ===========================================================================

namespace
{
    std::vector<std::pair<uint32_t, uint32_t>> ParseWeights(const std::string& raw)
    {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        std::istringstream ss(raw);
        std::string tok;
        while (std::getline(ss, tok, ','))
        {
            auto colon = tok.find(':');
            if (colon == std::string::npos) continue;
            try {
                uint32_t sl = std::stoul(tok.substr(0, colon));
                uint32_t w  = std::stoul(tok.substr(colon + 1));
                if (sl && w) out.emplace_back(sl, w);
            } catch (...) {}
        }
        return out;
    }

    // Default Beta shape per era when Craft.SkillDist has no row for the state:
    // early = mass spread low (everyone leveling), late = mass near cap.
    std::pair<double, double> DefaultBeta(uint8_t state, uint32_t tbcAt, uint32_t wotlkAt)
    {
        if (state < tbcAt)   return { 2.0, 3.0 }; // mean 0.40 of cap
        if (state < wotlkAt) return { 3.0, 2.0 }; // mean 0.60
        return { 5.0, 1.5 };                      // mean 0.77
    }

    // SkillDist row "state:alpha:beta,..." lookup; falls back to DefaultBeta.
    std::pair<double, double> BetaForState(const std::string& raw, uint8_t state,
                                           uint32_t tbcAt, uint32_t wotlkAt)
    {
        std::istringstream ss(raw);
        std::string tok;
        while (std::getline(ss, tok, ','))
        {
            uint32_t s; double a, b;
            if (std::sscanf(tok.c_str(), "%u:%lf:%lf", &s, &a, &b) == 3 && s == state && a > 0 && b > 0)
                return { a, b };
        }
        return DefaultBeta(state, tbcAt, wotlkAt);
    }
}

void CMangosAHBot::BuildCraftCandidates()
{
    _craftCandidates.clear();
    if (!_craftBuilt)
        return;
    for (const CraftRecipe& r : _craftGraph.Recipes())
        if (r.available)
            _craftCandidates[r.skillLine].push_back(&r);
}

void CMangosAHBot::BuildPopulation()
{
    const auto& cfg = gCMangosAHBotConfig;
    _craftChances.grey   = sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREY);
    _craftChances.green  = sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREEN);
    _craftChances.yellow = sWorld->getIntConfig(CONFIG_SKILL_CHANCE_YELLOW);
    _craftChances.orange = sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE);

    uint16_t cap = CraftSim::CapForState(_caps.state, cfg.progTbcAtState, cfg.progWotlkAtState);
    auto weights = ParseWeights(cfg.craftProfessionWeights);
    auto [alpha, beta] = BetaForState(cfg.craftSkillDist, _caps.state,
                                      cfg.progTbcAtState, cfg.progWotlkAtState);
    _population = CraftSim::RollPopulation(cfg.craftPopulation, weights, cap, alpha, beta,
                                           sCMangosAHBotRng->Engine());

    uint32_t below = 0;
    for (auto& c : _population) if (c.leveling()) ++below;
    LOG_INFO("module", "CMangosAHBot[craft]: population rolled — {} crafters (cap={} below-cap={} "
             "beta={:.1f}/{:.1f} chances g/gr/y/o={}/{}/{}/{}).",
             _population.size(), cap, below, alpha, beta,
             _craftChances.grey, _craftChances.green, _craftChances.yellow, _craftChances.orange);
}

void CMangosAHBot::UpdatePopulationCaps()
{
    uint16_t cap = CraftSim::CapForState(_caps.state, gCMangosAHBotConfig.progTbcAtState,
                                         gCMangosAHBotConfig.progWotlkAtState);
    for (auto& c : _population)
        c.cap = cap; // skills persist; crafters resume leveling toward the new cap
}

void CMangosAHBot::RunLevelingSessions(std::vector<Crafter>& pop, uint32_t n,
                                       CMangosAHBotCost& cost, std::vector<CraftListing>& out)
{
    const auto& cfg = gCMangosAHBotConfig;
    if (pop.empty())
        return;

    for (uint32_t s = 0; s < n; ++s)
    {
        if (!sCMangosAHBotRng->RollPct(cfg.craftChance))
            continue;
        Crafter& cr = pop[sCMangosAHBotRng->Urand(0, uint32_t(pop.size() - 1))];
        if (!cr.leveling())
            continue; // at cap => production, disabled in C3

        auto it = _craftCandidates.find(cr.skillLine);
        if (it == _craftCandidates.end())
            continue;
        const CraftRecipe* r = CraftSim::ChooseLevelingRecipe(it->second, cr.skill, cost,
                                                              _craftChances, sCMangosAHBotRng->Engine());
        if (!r)
            continue;

        uint32_t batch = sCMangosAHBotRng->Urand(cfg.craftBatchLevelingMin, cfg.craftBatchLevelingMax);
        CraftSim::SimulateSkillUps(cr, *r, batch, _craftChances, sCMangosAHBotRng->Engine());

        uint64_t matcost = CraftSim::MatCost(*r, cost);
        uint32_t margin  = sCMangosAHBotRng->Urand(cfg.craftMarginLevelingMin, cfg.craftMarginLevelingMax);
        uint64_t unit    = matcost * margin / 100; // leveling dump: below cost by construction

        CraftListing L;
        L.itemId      = r->productItem;
        L.count       = batch * r->productCount;
        L.unitPrice   = unit;
        L.unitMatCost = matcost;
        L.skillLine   = r->skillLine;
        L.category    = static_cast<uint8_t>(r->category);
        out.push_back(L);
        // ledger.Credit(reagents) is the buyer's demand ledger — C5.
    }
}

void CMangosAHBot::CraftSellPass(Player* bot, uint32_t houseIdx)
{
    const auto& cfg = gCMangosAHBotConfig;
    if (!_craftBuilt || _population.empty())
        return;

    uint32_t fid = CMAHB_AH_FIDS[houseIdx];
    AuctionHouseEntry const* ahEntry = sAuctionMgr->GetAuctionHouseEntryFromFactionTemplate(fid);
    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(fid);
    if (!ahEntry || !auctionHouse)
        return;

    // Cost context (facade): median anchors from live bot listings + intrinsic fallback.
    std::unordered_map<uint32_t, uint64_t> medians;
    BuildAnchorMedians(medians);
    ProjSource src(_costProducers);
    LiveAnchor anchor(medians, [this](uint32_t id) -> uint64_t {
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(id);
        return p ? CalculateBuyoutPrice(p) : 0;
    }, cfg.craftAnchorClampMin, cfg.craftAnchorClampMax);
    CMangosAHBotCost cost(src, anchor);
    cost.NewPass();

    std::vector<CraftListing> listings;
    uint32_t nSessions = sCMangosAHBotRng->Urand(cfg.craftSessionsMin, cfg.craftSessionsMax);
    RunLevelingSessions(_population, nSessions, cost, listings);
    if (listings.empty())
        return;

    auto trans = CharacterDatabase.BeginTransaction();
    uint32_t posted = 0;
    for (const CraftListing& L : listings)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(L.itemId);
        if (!proto || !PassesFilters(proto))
            continue;
        // Layer 3 net (addendum §4) — craft output must respect the caps too.
        if ((_caps.itemLevelCap != NOCAP && proto->ItemLevel > _caps.itemLevelCap) ||
            (_caps.reqLevelCap  != NOCAP && proto->RequiredLevel > _caps.reqLevelCap))
        {
            ++_layer3Dropped;
            continue;
        }
        uint64_t unit = ValueWithVariance(L.unitPrice);
        if (unit == 0)
            continue;

        uint32_t maxStack = std::max(1u, static_cast<uint32_t>(proto->GetMaxStackSize()));
        uint32_t remaining = L.count;
        while (remaining > 0)
        {
            uint32_t stack = std::min(remaining, maxStack);
            remaining -= stack;
            uint64_t buyout64 = unit * stack;
            uint32_t buyout = buyout64 > NOCAP ? NOCAP : static_cast<uint32_t>(buyout64);
            if (buyout == 0)
                continue;
            uint32_t startbid = static_cast<uint32_t>(static_cast<uint64_t>(buyout) *
                sCMangosAHBotRng->Urand(cfg.bidMin, std::max(cfg.bidMin, cfg.bidMax)) / 100);

            Item* item = Item::CreateItem(L.itemId, 1, bot);
            if (!item)
                continue;
            item->AddToUpdateQueueOf(bot);
            if (uint32_t rp = Item::GenerateItemRandomPropertyId(L.itemId))
                item->SetItemRandomProperties(rp);
            item->SetCount(stack);

            uint32_t durationSecs = sCMangosAHBotRng->Urand(std::max(1u, cfg.timeMin),
                std::max(std::max(1u, cfg.timeMin), cfg.timeMax)) * 3600u;
            uint32_t deposit = sAuctionMgr->GetAuctionDeposit(ahEntry, durationSecs, item, stack);

            AuctionEntry* ae      = new AuctionEntry();
            ae->Id                = sObjectMgr->GenerateAuctionID();
            ae->houseId           = AuctionHouseId(CMAHB_AH_IDS[houseIdx]);
            ae->item_guid         = item->GetGUID();
            ae->item_template     = item->GetEntry();
            ae->itemCount         = item->GetCount();
            ae->owner             = bot->GetGUID();
            ae->startbid          = startbid;
            ae->buyout            = buyout;
            ae->bid               = 0;
            ae->deposit           = deposit;
            ae->expire_time       = time(nullptr) + static_cast<time_t>(durationSecs);
            ae->auctionHouseEntry = ahEntry;

            item->SaveToDB(trans);
            item->RemoveFromUpdateQueueOf(bot);
            sAuctionMgr->AddAItem(item);
            auctionHouse->AddAuction(ae);
            ae->SaveToDB(trans);
            ++posted;
        }
    }
    if (posted > 0)
        CharacterDatabase.CommitTransaction(trans);

    LOG_DEBUG("module", "CMangosAHBot[craft]: sell house={} craftPosted={} layer3Dropped={}",
              CMAHB_AH_IDS[houseIdx], posted, _layer3Dropped);
}

std::string CMangosAHBot::CraftSimulateSessions(uint32_t n) const
{
    const auto& cfg = gCMangosAHBotConfig;
    if (!cfg.craftEnable || !_craftBuilt)
        return "Craft layer disabled (Craft.Enable=0).";
    if (n == 0) n = 200;

    std::unordered_map<uint32_t, uint64_t> medians;
    BuildAnchorMedians(medians);
    ProjSource src(_costProducers);
    LiveAnchor anchor(medians, [this](uint32_t id) -> uint64_t {
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(id);
        return p ? CalculateBuyoutPrice(p) : 0;
    }, cfg.craftAnchorClampMin, cfg.craftAnchorClampMax);
    CMangosAHBotCost cost(src, anchor);
    cost.NewPass();

    // Simulate on a COPY so the snapshot is non-destructive (real leveling advances
    // the live population in CraftSellPass).
    std::vector<Crafter> pop = _population;
    std::vector<CraftListing> listings;
    const_cast<CMangosAHBot*>(this)->RunLevelingSessions(pop, n, cost, listings);

    // Aggregate by item.
    struct Agg { uint64_t count=0; uint64_t price=0; uint64_t matcost=0; uint8_t cat=CAT_MISC; };
    std::unordered_map<uint32_t, Agg> byItem;
    uint64_t belowCost = 0, layer3Would = 0;
    for (const CraftListing& L : listings)
    {
        auto& a = byItem[L.itemId];
        a.count += L.count; a.price = L.unitPrice; a.matcost = L.unitMatCost; a.cat = L.category;
        if (L.unitPrice < L.unitMatCost) ++belowCost;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(L.itemId);
        if (proto && ((_caps.itemLevelCap != NOCAP && proto->ItemLevel > _caps.itemLevelCap) ||
                      (_caps.reqLevelCap  != NOCAP && proto->RequiredLevel > _caps.reqLevelCap)))
            ++layer3Would;
    }

    std::vector<std::pair<uint32_t, Agg>> sorted(byItem.begin(), byItem.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });

    // Per-category volume (so bars/bolts/bandages show even when ammo tops the list).
    uint64_t catVol[CAT_COUNT] = {0};
    uint64_t pricedBelow = 0, priced = 0;
    for (auto& [item, a] : byItem)
    {
        catVol[a.cat] += a.count;
        if (a.matcost > 0) { ++priced; if (a.price < a.matcost) ++pricedBelow; }
    }

    std::ostringstream ss;
    ss << "Craft leveling sim: " << n << " sessions, seed=" << sCMangosAHBotRng->Seeded()
       << " state=" << int(_caps.state) << " pop=" << _population.size()
       << " -> " << listings.size() << " listing-lines / " << byItem.size() << " items"
       << " | belowCostShare(priced)=" << (priced ? pricedBelow * 100 / priced : 0) << "%"
       << " layer3Would=" << layer3Would;
    ss << "\n by category (units):";
    for (uint8_t c = 0; c < CAT_COUNT; ++c)
        ss << " " << CategoryName(ItemCategory(c)) << "=" << catVol[c];
    // Per-profession top product (proves the named gluts: bars/bolts/bandages/greens).
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint64_t>> perProf; // skillLine -> item -> count
    for (const CraftListing& L : listings)
        perProf[L.skillLine][L.itemId] += L.count;
    static const std::pair<uint32_t, const char*> kProf[] = {
        {171,"Alch"},{164,"BS"},{165,"LW"},{197,"Tailor"},{202,"Eng"},{333,"Ench"},
        {755,"JC"},{773,"Insc"},{185,"Cook"},{129,"FirstAid"},{186,"Mining"}};
    ss << "\n by profession top product:";
    for (auto& [sl, name] : kProf)
    {
        auto it = perProf.find(sl);
        if (it == perProf.end()) continue;
        uint32_t top = 0; uint64_t topN = 0;
        for (auto& [item, cnt] : it->second) if (cnt > topN) { topN = cnt; top = item; }
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(top);
        ss << "\n  " << name << ": " << (p ? p->Name1 : "?") << " x" << topN;
    }

    ss << "\n top items by volume (count | unit | matcost | ratio% | cat):";
    uint32_t shown = 0;
    for (auto& [item, a] : sorted)
    {
        if (shown++ >= 20) break;
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(item);
        std::string name = p ? p->Name1 : ("item#" + std::to_string(item));
        uint64_t ratio = a.matcost ? a.price * 100 / a.matcost : 0;
        ss << "\n  " << name << " x" << a.count << " | " << a.price << " | " << a.matcost
           << " | " << ratio << "% | " << CategoryName(ItemCategory(a.cat));
    }
    return ss.str();
}

void CMangosAHBot::ReloadData()
{
    LoadOverrides();
    RefreshProgression(true);
}

// ===========================================================================
// Enumeration + classification (Phase 3 / addendum Phase 3.5)
// ===========================================================================

uint8_t CMangosAHBot::ExpansionOfMap(uint32_t mapId) const
{
    if (MapEntry const* me = sMapStore.LookupEntry(mapId))
        return static_cast<uint8_t>(me->expansionID);
    return CMAHB_EXP_UNKNOWN;
}

uint8_t CMangosAHBot::ExpansionOfArea(uint32_t areaId) const
{
    if (AreaTableEntry const* a = sAreaTableStore.LookupEntry(areaId))
        return ExpansionOfMap(a->mapid);
    return CMAHB_EXP_UNKNOWN;
}

void CMangosAHBot::BuildVendorSet()
{
    _vendorItems.clear();
    if (QueryResult r = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE item > 0"))
        do { _vendorItems.insert(r->Fetch()[0].Get<uint32_t>()); } while (r->NextRow());
    LOG_INFO("module", "CMangosAHBot: vendor item set = {}", _vendorItems.size());
}

void CMangosAHBot::BuildClassifiedSources()
{
    for (auto& v : _creatureClassified) v.clear();
    _gameobjectClassified.clear();
    _fishingClassified.clear();
    _skinningClassified.clear();
    _excludedUnspawnedCreature = 0;
    _excludedUnspawnedGO = 0;
    _creatureLootVotes.clear();
    _goLootVotes.clear();
    _skinLootVotes.clear();

    // ---- creatures + skinning: entry -> (lootid, skinloot, rank) ----
    struct CT { uint32_t lootid = 0; uint32_t skinloot = 0; uint8_t rank = 0; };
    std::unordered_map<uint32_t, CT> ct;
    if (QueryResult r = WorldDatabase.Query(
            "SELECT entry, lootid, skinloot, `rank` FROM creature_template WHERE lootid > 0 OR skinloot > 0"))
    {
        do
        {
            Field* f = r->Fetch();
            CT c;
            c.lootid   = f[1].Get<uint32_t>();
            c.skinloot = f[2].Get<uint32_t>();
            c.rank     = static_cast<uint8_t>(f[3].Get<uint32_t>());
            ct[f[0].Get<uint32_t>()] = c;
        } while (r->NextRow());
    }

    std::unordered_map<uint32_t, uint8_t> lootExp, skinExp, lootRank;
    if (QueryResult r = WorldDatabase.Query("SELECT id1, id2, id3, map FROM creature"))
    {
        do
        {
            Field* f = r->Fetch();
            uint32_t ids[3] = { f[0].Get<uint32_t>(), f[1].Get<uint32_t>(), f[2].Get<uint32_t>() };
            uint8_t exp = ExpansionOfMap(f[3].Get<uint32_t>());
            if (exp == CMAHB_EXP_UNKNOWN)
                continue;
            for (uint32_t id : ids)
            {
                if (!id) continue;
                auto it = ct.find(id);
                if (it == ct.end()) continue;
                if (it->second.lootid)
                {
                    MinMerge(lootExp, it->second.lootid, exp);
                    lootRank[it->second.lootid] = it->second.rank;
                    if (exp < 3) ++_creatureLootVotes[it->second.lootid][exp]; // plurality votes
                }
                if (it->second.skinloot)
                {
                    MinMerge(skinExp, it->second.skinloot, exp);
                    if (exp < 3) ++_skinLootVotes[it->second.skinloot][exp];
                }
            }
        } while (r->NextRow());
    }

    // Bucket resolved creature lootids by rank; count unresolved.
    std::unordered_map<uint32_t, bool> lootSeen;
    for (auto& [entry, c] : ct)
        if (c.lootid)
            lootSeen[c.lootid];   // touch
    for (auto& [lootId, exp] : lootExp)
    {
        int bucket = RankToBucket(lootRank.count(lootId) ? lootRank[lootId] : 0);
        _creatureClassified[bucket].push_back({ lootId, exp });
    }
    for (auto& [skinId, exp] : skinExp)
        _skinningClassified.push_back({ skinId, exp });

    for (auto& [lootId, _] : lootSeen)
        if (!lootExp.count(lootId)) ++_excludedUnspawnedCreature;

    // ---- gameobjects: type 3 chest, Data1 = lootId ----
    std::unordered_map<uint32_t, uint32_t> goLoot; // entry -> lootId
    if (QueryResult r = WorldDatabase.Query(
            "SELECT entry, Data1 FROM gameobject_template WHERE type = 3 AND Data1 > 0"))
        do { Field* f = r->Fetch(); goLoot[f[0].Get<uint32_t>()] = f[1].Get<uint32_t>(); } while (r->NextRow());

    std::unordered_map<uint32_t, uint8_t> goExp;
    std::unordered_map<uint32_t, bool> goLootSeen;
    for (auto& [e, l] : goLoot) goLootSeen[l];
    if (QueryResult r = WorldDatabase.Query("SELECT id, map FROM gameobject"))
    {
        do
        {
            Field* f = r->Fetch();
            uint32_t id = f[0].Get<uint32_t>();
            uint8_t exp = ExpansionOfMap(f[1].Get<uint32_t>());
            if (exp == CMAHB_EXP_UNKNOWN) continue;
            auto it = goLoot.find(id);
            if (it != goLoot.end())
            {
                MinMerge(goExp, it->second, exp);
                if (exp < 3) ++_goLootVotes[it->second][exp];
            }
        } while (r->NextRow());
    }
    for (auto& [lootId, exp] : goExp)
        _gameobjectClassified.push_back({ lootId, exp });
    for (auto& [lootId, _] : goLootSeen)
        if (!goExp.count(lootId)) ++_excludedUnspawnedGO;

    // ---- fishing: entry IS an area id ----
    if (QueryResult r = WorldDatabase.Query("SELECT DISTINCT entry FROM fishing_loot_template"))
    {
        do
        {
            uint32_t areaId = r->Fetch()[0].Get<uint32_t>();
            uint8_t exp = ExpansionOfArea(areaId);
            if (exp != CMAHB_EXP_UNKNOWN)
                _fishingClassified.push_back({ areaId, exp });
        } while (r->NextRow());
    }

    LOG_INFO("module", "CMangosAHBot: classified sources — creature[N/R/E/RE/WB]={}/{}/{}/{}/{} go={} fishing={} skin={} (unresolved creature={} go={})",
        _creatureClassified[0].size(), _creatureClassified[1].size(), _creatureClassified[2].size(),
        _creatureClassified[3].size(), _creatureClassified[4].size(),
        _gameobjectClassified.size(), _fishingClassified.size(), _skinningClassified.size(),
        _excludedUnspawnedCreature, _excludedUnspawnedGO);
}

void CMangosAHBot::BuildItemExpansion()
{
    // Expansion each item most typically comes from, for reagent-era gating of the
    // craft graph (a recipe can't be earlier than its rarest mat) — closes the
    // ilvl/skill-exempt leak (Netherweave net/bag at Vanilla).
    //
    // PLURALITY by spawn count, NOT min: TBC/WotLK instance maps carry unreliable
    // MapEntry::expansionID (often 0), so a MIN over sources drags cloth (which also
    // drops in mis-flagged instances) to Vanilla. Netherweave's 6000+ open-world
    // Outland spawns outvote the handful in instances. Ore is unaffected (open-world
    // only) which is why it already classified right. This is deliberately separate
    // from the base source-vector MinMerge, so world-drop gating is unchanged (#1).
    _itemExpansion.clear();

    std::unordered_map<uint32_t, std::array<uint32_t, 3>> itemVotes;
    auto addVotes = [&](const char* tbl,
                        const std::unordered_map<uint32_t, std::array<uint32_t, 3>>& lootVotes)
    {
        if (QueryResult r = WorldDatabase.Query("SELECT item, entry FROM {} WHERE item > 0", tbl))
        {
            do
            {
                Field* f = r->Fetch();
                uint32_t item = f[0].Get<uint32_t>();
                auto it = lootVotes.find(f[1].Get<uint32_t>());
                if (it != lootVotes.end())
                    for (int e = 0; e < 3; ++e) itemVotes[item][e] += it->second[e];
            } while (r->NextRow());
        }
    };
    addVotes("creature_loot_template",   _creatureLootVotes);
    addVotes("gameobject_loot_template", _goLootVotes);
    addVotes("skinning_loot_template",   _skinLootVotes);

    // Fishing: one vote per area at its (open-world) expansion — no spawn weight.
    for (auto& c : _fishingClassified)
    {
        if (c.expansion < 3)
        {
            if (QueryResult r = WorldDatabase.Query(
                    "SELECT item FROM fishing_loot_template WHERE entry = {} AND item > 0", c.lootId))
                do { ++itemVotes[r->Fetch()[0].Get<uint32_t>()][c.expansion]; } while (r->NextRow());
        }
    }

    // Plurality: pick the expansion with the most spawn-weight; tie-break to the
    // higher era (safer to over-gate a genuinely cross-era mat than to leak it).
    uint32_t tbc = 0, wotlk = 0;
    for (auto& [item, v] : itemVotes)
    {
        uint8_t best = 0; uint32_t bestN = v[0];
        for (uint8_t e = 1; e < 3; ++e)
            if (v[e] >= bestN) { bestN = v[e]; best = e; } // >= => tie to higher era
        if (bestN == 0) continue;
        _itemExpansion[item] = best;
        if (best == CMAHB_EXP_TBC) ++tbc; else if (best == CMAHB_EXP_WOTLK) ++wotlk;
    }
    LOG_INFO("module", "CMangosAHBot: item->expansion map = {} items ({} TBC, {} WotLK), plurality-classified "
             "for reagent-era gating", _itemExpansion.size(), tbc, wotlk);
}

void CMangosAHBot::BuildProfessionPool()
{
    // §2.3 in-memory scan of create-item profession spells; skill-rank per addendum §4 Layer 2b.
    std::unordered_map<uint32_t, uint32_t> byItem; // itemId -> min skill rank
    uint32_t storeSize = sSpellMgr->GetSpellInfoStoreSize();
    for (uint32_t id = 0; id < storeSize; ++id)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
        if (!spell)
            continue;
        if (!(spell->Attributes & 0x20) || !(spell->Attributes & 0x10000))
            continue;

        // Lowest required skill rank across this spell's skill-line abilities.
        uint32_t minRank = 0;
        bool haveRank = false;
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(id);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            uint32_t r = it->second->MinSkillLineRank;
            if (!haveRank || r < minRank) { minRank = r; haveRank = true; }
        }

        for (uint8_t e = 0; e < MAX_SPELL_EFFECTS; ++e)
        {
            if (spell->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM && spell->Effects[e].ItemType)
            {
                uint32_t itemId = spell->Effects[e].ItemType;
                auto it = byItem.find(itemId);
                if (it == byItem.end() || minRank < it->second)
                    byItem[itemId] = minRank;
            }
        }
    }

    _professionClassified.clear();
    _professionClassified.reserve(byItem.size());
    for (auto& [itemId, rank] : byItem)
        _professionClassified.push_back({ itemId, rank });

    LOG_INFO("module", "CMangosAHBot: profession item pool = {} (low thousands expected on stock WotLK)",
             _professionClassified.size());
}

void CMangosAHBot::BuildDisenchantPool()
{
    // Disenchant gated by item level of source items (addendum §4 Layer 1 special case).
    _disenchantClassified.clear();
    if (QueryResult r = WorldDatabase.Query(
            "SELECT DisenchantID, MIN(ItemLevel) FROM item_template WHERE DisenchantID > 0 GROUP BY DisenchantID"))
    {
        do
        {
            Field* f = r->Fetch();
            _disenchantClassified.push_back({ f[0].Get<uint32_t>(), f[1].Get<uint32_t>() });
        } while (r->NextRow());
    }
    LOG_INFO("module", "CMangosAHBot: disenchant loot ids = {}", _disenchantClassified.size());
}

void CMangosAHBot::LoadOverrides()
{
    _overrides.clear();
    if (QueryResult r = CharacterDatabase.Query(
            "SELECT item, value, add_chance, min_amount, max_amount FROM cmangos_ahbot_items"))
    {
        do
        {
            Field* f = r->Fetch();
            CmAHBOverride o;
            o.value     = f[1].Get<uint32_t>();
            o.addChance = f[2].Get<uint32_t>();
            o.minAmount = f[3].Get<uint32_t>();
            o.maxAmount = f[4].Get<uint32_t>();
            _overrides[f[0].Get<uint32_t>()] = o;
        } while (r->NextRow());
    }
    LOG_INFO("module", "CMangosAHBot: overrides loaded = {}", _overrides.size());
}

void CMangosAHBot::RebuildFilteredVectors()
{
    const uint8_t maxExp = _caps.maxExpansion;

    for (int rank = 0; rank < (int)CMAHB_CREATURE_RANKS; ++rank)
    {
        _creature[rank].clear();
        for (auto& c : _creatureClassified[rank])
            if (c.expansion <= maxExp)
                _creature[rank].push_back(c.lootId);
    }
    auto filt = [maxExp](const std::vector<CmAHBClassifiedId>& in, std::vector<uint32_t>& out)
    {
        out.clear();
        for (auto& c : in)
            if (c.expansion <= maxExp)
                out.push_back(c.lootId);
    };
    filt(_gameobjectClassified, _gameobject);
    filt(_fishingClassified,    _fishing);
    filt(_skinningClassified,   _skinning);

    _disenchant.clear();
    for (auto& [id, ilvl] : _disenchantClassified)
        if (_caps.itemLevelCap == NOCAP || ilvl <= _caps.itemLevelCap)
            _disenchant.push_back(id);

    _profession.clear();
    for (auto& [itemId, rank] : _professionClassified)
        if (_caps.skillCap == NOCAP || rank <= _caps.skillCap)
            _profession.push_back(itemId);
}

// ===========================================================================
// Progression (addendum §3 / §5)
// ===========================================================================

void CMangosAHBot::RefreshProgression(bool force)
{
    const auto& cfg = gCMangosAHBotConfig;
    uint32_t now = static_cast<uint32_t>(time(nullptr));

    if (!force && _lastProgRefresh && (now - _lastProgRefresh) < cfg.progRefreshInterval)
        return;
    bool firstRun = (_lastProgRefresh == 0);
    _lastProgRefresh = now;

    CmAHBCaps newCaps;
    if (!cfg.progEnable)
    {
        newCaps.state = 255;
        newCaps.maxExpansion = CMAHB_EXP_WOTLK;
        newCaps.itemLevelCap = NOCAP;
        newCaps.reqLevelCap  = NOCAP;
        newCaps.skillCap     = NOCAP;
    }
    else
    {
        uint8_t state = CMangosAHBotProgression::GetEffectiveState();
        newCaps = CMangosAHBotProgression::CapsForState(state);
    }

    if (firstRun || newCaps.state != _caps.state)
    {
        uint8_t oldState = _caps.state;
        _caps = newCaps;
        RebuildFilteredVectors();
        if (_craftBuilt)
        {
            _craftGraph.RecomputeMask(_caps, cfg.progTbcAtState, cfg.progWotlkAtState, cfg.progSkillCaps);
            BuildCostProjection();
            BuildCraftCandidates();
            UpdatePopulationCaps(); // no-op until the population exists (built in Initialize)
            LOG_INFO("module", "CMangosAHBot[craft]: mask recomputed for state {} — available {}/{} recipes "
                     "(JC={} Inscription={}).", _caps.state, _craftGraph.AvailableCount(), _craftGraph.Size(),
                     _craftGraph.SkillLineAvailableCount(SKILL_JEWELCRAFTING),
                     _craftGraph.SkillLineAvailableCount(SKILL_INSCRIPTION));
        }
        LOG_INFO("module", "CMangosAHBot: progression {} -> {} (maxExp={} ilvlCap={} skillCap={}). "
            "Vectors: creN={} creR={} creE={} creRE={} creWB={} go={} fish={} skin={} disen={} prof={}",
            firstRun ? 0 : oldState, _caps.state, _caps.maxExpansion,
            _caps.itemLevelCap == NOCAP ? 0 : _caps.itemLevelCap,
            _caps.skillCap == NOCAP ? 0 : _caps.skillCap,
            _creature[0].size(), _creature[1].size(), _creature[2].size(),
            _creature[3].size(), _creature[4].size(),
            _gameobject.size(), _fishing.size(), _skinning.size(), _disenchant.size(), _profession.size());
    }
}

// ===========================================================================
// Simulation (Phase 4)
// ===========================================================================

void CMangosAHBot::SimSource(const std::vector<uint32_t>& ids, LootStore const& store,
                             const CmAHBSourceConfig& cfg, Player* bot, CmAHBItemMap& out)
{
    if (ids.empty())
        return;

    int32_t nsrc = (cfg.maxSources <= cfg.minSources)
                   ? std::max(0, cfg.minSources)
                   : irand(cfg.minSources, cfg.maxSources);
    if (nsrc <= 0)
        return;

    for (int32_t i = 0; i < nsrc; ++i)
    {
        uint32_t lootId = ids[urand(0, static_cast<uint32_t>(ids.size() - 1))];
        uint32_t nloot  = urand(cfg.minLootings, std::max(cfg.minLootings, cfg.maxLootings));
        for (uint32_t j = 0; j < nloot; ++j)
        {
            // §2.1 CRITICAL: a fresh Loot per iteration. AC's Loot::AddItem caps at
            // MAX_NR_LOOT_ITEMS (18); accumulating into one Loot silently truncates.
            Loot loot;
            if (loot.FillLoot(lootId, store, bot, true /*personal*/, true /*noEmptyError*/))
                for (LootItem const& li : loot.items)
                    out[li.itemid] += li.count;
            // loot destroyed here (clear() in dtor)
        }
    }
}

void CMangosAHBot::SimProfession(CmAHBItemMap& out)
{
    if (_profession.empty())
        return;

    const auto& cfg = gCMangosAHBotConfig;
    int32_t bmin = std::max(0, cfg.professionTuple[0]);
    int32_t bmax = std::max(bmin, cfg.professionTuple[1]);
    uint32_t budget = urand(static_cast<uint32_t>(bmin), static_cast<uint32_t>(bmax));
    uint32_t pctMin = static_cast<uint32_t>(std::max(0, cfg.professionTuple[2]));
    uint32_t pctMax = std::max(pctMin, static_cast<uint32_t>(std::max(0, cfg.professionTuple[3])));

    for (uint32_t i = 0; i < budget; ++i)
    {
        uint32_t itemId = _profession[urand(0, static_cast<uint32_t>(_profession.size() - 1))];
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        // CMaNGOS quality-decay roll (Phase 4.4): white 100%, green 50%, blue 25%, purple 12.5%.
        uint32_t q = proto->Quality;
        uint32_t flag = (q == 0) ? 1u : (1u << (q - 1));
        if (urand(0, flag - 1) > 0)
            continue;

        uint32_t maxStack = std::max(1u, static_cast<uint32_t>(proto->GetMaxStackSize()));
        uint32_t cnt = (pctMax > 0) ? std::max(1u, maxStack * urand(pctMin, pctMax) / 100u) : 1u;
        out[itemId] += cnt;
    }
}

void CMangosAHBot::ApplyOverridesToMap(CmAHBItemMap& out)
{
    for (auto& [itemId, ov] : _overrides)
    {
        if (ov.addChance == 0)
            continue;
        if (urand(0, 99) < ov.addChance)
            out[itemId] += urand(ov.minAmount, std::max(ov.minAmount, ov.maxAmount));
    }
}

void CMangosAHBot::AddLootToItemMap(Player* bot, CmAHBItemMap& out)
{
    const auto& cfg = gCMangosAHBotConfig;
    for (int rank = 0; rank < (int)CMAHB_CREATURE_RANKS; ++rank)
        SimSource(_creature[rank], LootTemplates_Creature, cfg.creature[rank], bot, out);
    SimSource(_fishing,    LootTemplates_Fishing,    cfg.fishing,    bot, out);
    SimSource(_gameobject, LootTemplates_Gameobject, cfg.gameobject, bot, out);
    SimSource(_skinning,   LootTemplates_Skinning,   cfg.skinning,   bot, out);
    SimSource(_disenchant, LootTemplates_Disenchant, cfg.disenchant, bot, out);
    // Retire the flat Items.Profession source when the craft layer is producing
    // (double-listing guard, addendum risk table). Craft listings come from
    // CraftSellPass instead.
    if (!cfg.craftEnable)
        SimProfession(out);
    ApplyOverridesToMap(out);
}

// ===========================================================================
// Pricing + filters (Phase 5)
// ===========================================================================

bool CMangosAHBot::PassesFilters(ItemTemplate const* proto) const
{
    const auto& cfg = gCMangosAHBotConfig;
    uint32_t q = proto->Quality, cls = proto->Class;
    if (q >= CMAHB_MAX_QUALITY || cls >= CMAHB_MAX_CLASS)
        return false;
    if (cfg.valueMatrix[q][cls] == 0)
        return false;
    if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
        return false;
    if (cls == ITEM_CLASS_QUEST)
        return false;
    if (proto->Flags & ITEM_FLAG_HAS_LOOT)   // right-clickable loot container
        return false;

    auto ov = _overrides.find(proto->ItemId);
    if (ov != _overrides.end() && ov->second.value == 0 && ov->second.addChance == 0)
        return false; // explicit blacklist
    return true;
}

uint64_t CMangosAHBot::CalculateBuyoutPrice(ItemTemplate const* proto) const
{
    // Ported literally from CMaNGOS CalculateBuyoutPrice — do not "fix" the oddities (plan §6).
    const auto& cfg = gCMangosAHBotConfig;
    uint32_t q = proto->Quality, cls = proto->Class;
    if (q >= CMAHB_MAX_QUALITY || cls >= CMAHB_MAX_CLASS)
        return 0;

    uint64_t base = proto->BuyPrice;
    if (proto->BuyPrice == 0 || (proto->SellPrice > 0 && proto->BuyPrice / proto->SellPrice > 5))
        base = static_cast<uint64_t>(proto->SellPrice) * (q <= ITEM_QUALITY_NORMAL ? 4 : 5);
    if (base == 0)
        return 0;

    uint32_t pct = (cfg.valueVendor && _vendorItems.count(proto->ItemId))
                   ? 100 : cfg.valueMatrix[q][cls];
    if (pct == 0)
        return 0;
    return base * pct / 100;
}

uint64_t CMangosAHBot::ValueWithVariance(uint64_t value) const
{
    uint32_t v = std::min(99u, gCMangosAHBotConfig.valueVariance);
    return value * urand(100 - v, 100 + v) / 100;
}

// ===========================================================================
// Sell pass (Phase 5 / addendum Layer 3)
// ===========================================================================

void CMangosAHBot::SellPass(Player* bot, uint32_t houseIdx, bool prefill)
{
    const auto& cfg = gCMangosAHBotConfig;

    if (!prefill && urand(0, 99) >= cfg.chanceSell)
        return;
    if (!prefill && cfg.hardCap > 0 && BotAuctionCount(houseIdx) >= cfg.hardCap)
        return;

    uint32_t fid = CMAHB_AH_FIDS[houseIdx];
    AuctionHouseEntry const* ahEntry = sAuctionMgr->GetAuctionHouseEntryFromFactionTemplate(fid);
    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(fid);
    if (!ahEntry || !auctionHouse)
        return;

    CmAHBItemMap itemMap;
    AddLootToItemMap(bot, itemMap);

    auto trans = CharacterDatabase.BeginTransaction();
    uint32_t posted = 0;

    for (auto& [itemId, totalCount] : itemMap)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto || !PassesFilters(proto))
            continue;

        // Layer 3 safety net (addendum §4): anything over the progression caps here
        // leaked through source gating. Drop + count; sustained growth == mapping bug.
        if ((_caps.itemLevelCap != NOCAP && proto->ItemLevel > _caps.itemLevelCap) ||
            (_caps.reqLevelCap  != NOCAP && proto->RequiredLevel > _caps.reqLevelCap))
        {
            ++_layer3Dropped;
            continue;
        }

        // Per-unit price: override value wins, else CMaNGOS formula.
        uint64_t unit;
        auto ov = _overrides.find(itemId);
        if (ov != _overrides.end() && ov->second.value > 0)
            unit = ValueWithVariance(ov->second.value);
        else
        {
            unit = CalculateBuyoutPrice(proto);
            if (unit == 0)
                continue;
            unit = ValueWithVariance(unit);
        }
        if (unit == 0)
            continue;

        uint32_t maxStack = std::max(1u, static_cast<uint32_t>(proto->GetMaxStackSize()));
        uint32_t remaining = totalCount;
        while (remaining > 0)
        {
            uint32_t stack = std::min(remaining, maxStack);
            remaining -= stack;

            uint64_t buyout64 = unit * stack;
            uint32_t buyout = buyout64 > NOCAP ? NOCAP : static_cast<uint32_t>(buyout64);
            if (buyout == 0)
                continue;
            uint32_t startbid = static_cast<uint32_t>(
                static_cast<uint64_t>(buyout) * urand(cfg.bidMin, std::max(cfg.bidMin, cfg.bidMax)) / 100);

            Item* item = Item::CreateItem(itemId, 1, bot);
            if (!item)
                continue;
            item->AddToUpdateQueueOf(bot);
            if (uint32_t rp = Item::GenerateItemRandomPropertyId(itemId))
                item->SetItemRandomProperties(rp);
            item->SetCount(stack);

            uint32_t durationSecs = urand(std::max(1u, cfg.timeMin), std::max(std::max(1u, cfg.timeMin), cfg.timeMax)) * 3600u;
            uint32_t deposit = sAuctionMgr->GetAuctionDeposit(ahEntry, durationSecs, item, stack);

            AuctionEntry* ae      = new AuctionEntry();
            ae->Id                = sObjectMgr->GenerateAuctionID();
            ae->houseId           = AuctionHouseId(CMAHB_AH_IDS[houseIdx]);
            ae->item_guid         = item->GetGUID();
            ae->item_template     = item->GetEntry();
            ae->itemCount         = item->GetCount();
            ae->owner             = bot->GetGUID();
            ae->startbid          = startbid;
            ae->buyout            = buyout;
            ae->bid               = 0;
            ae->deposit           = deposit;
            ae->expire_time       = time(nullptr) + static_cast<time_t>(durationSecs);
            ae->auctionHouseEntry = ahEntry;

            item->SaveToDB(trans);
            item->RemoveFromUpdateQueueOf(bot);
            sAuctionMgr->AddAItem(item);
            auctionHouse->AddAuction(ae);
            ae->SaveToDB(trans);
            ++posted;
        }
    }

    if (posted > 0)
        CharacterDatabase.CommitTransaction(trans);

    LOG_DEBUG("module", "CMangosAHBot: sell house={} posted={} layer3Dropped={}",
              CMAHB_AH_IDS[houseIdx], posted, _layer3Dropped);
}

// ===========================================================================
// Buyer (Phase 6)
// ===========================================================================

void CMangosAHBot::BuyPass(Player* bot, uint32_t houseIdx)
{
    const auto& cfg = gCMangosAHBotConfig;
    if (urand(0, 99) >= cfg.chanceBuy)
        return;

    uint32_t fid = CMAHB_AH_FIDS[houseIdx];
    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(fid);
    if (!auctionHouse)
        return;

    // §6.2: buyout mutates the auction map — collect targets, execute AFTER iterating.
    std::vector<uint32_t> buyoutIds;

    for (auto it = auctionHouse->GetAuctionsBegin(); it != auctionHouse->GetAuctionsEnd(); ++it)
    {
        AuctionEntry* auction = it->second;
        if (!auction || auction->owner == bot->GetGUID())
            continue; // never bid on our own listings

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto)
            continue;

        uint64_t perUnit = CalculateBuyoutPrice(proto);
        if (perUnit == 0)
            continue;

        // Valuation mirrors the seller (buyer coherence, plan §0).
        uint64_t valuation = perUnit * auction->itemCount * cfg.buyValue / 100;
        if (valuation == 0)
            continue;

        // Buy out when the ask sits below our valuation.
        if (auction->buyout > 0 && auction->buyout <= valuation)
        {
            buyoutIds.push_back(auction->Id);
            continue;
        }

        // Otherwise bid up to valuation (bidding does not invalidate the map).
        uint32_t curPrice = auction->bid ? auction->bid : auction->startbid;
        uint32_t nextBid = std::max<uint32_t>(curPrice + 1, auction->startbid);
        if (nextBid <= valuation && (auction->buyout == 0 || nextBid < auction->buyout) &&
            auction->bidder != bot->GetGUID())
        {
            auto trans = CharacterDatabase.BeginTransaction();
            if (auction->bidder)
                sAuctionMgr->SendAuctionOutbiddedMail(auction, nextBid, bot, trans);
            auction->bidder = bot->GetGUID();
            auction->bid = nextBid;
            sAuctionMgr->GetAuctionHouseSearcher()->UpdateBid(auction);
            CharacterDatabase.Execute(
                "UPDATE auctionhouse SET buyguid = '{}', lastbid = '{}' WHERE id = '{}'",
                auction->bidder.GetCounter(), auction->bid, auction->Id);
            CharacterDatabase.CommitTransaction(trans);
        }
    }

    // Deferred buyouts.
    for (uint32_t id : buyoutIds)
    {
        AuctionEntry* auction = auctionHouse->GetAuction(id);
        if (!auction)
            continue;
        auto trans = CharacterDatabase.BeginTransaction();
        if (auction->bidder && auction->bidder != bot->GetGUID())
            sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, bot, trans);
        auction->bidder = bot->GetGUID();
        auction->bid = auction->buyout;
        sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
        sAuctionMgr->SendAuctionWonMail(auction, trans);
        auction->DeleteFromDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        sAuctionMgr->RemoveAItem(auction->item_guid);
        auctionHouse->RemoveAuction(auction);
    }
}

// ===========================================================================
// Update — rotation + tick compensation (plan §2.4)
// ===========================================================================

void CMangosAHBot::WithTransientBot(const std::function<void(Player*)>& fn)
{
    const auto& cfg = gCMangosAHBotConfig;
    std::string accountName = "CMangosAHBot" + std::to_string(cfg.account);
    WorldSession session(cfg.account, std::move(accountName), 0, nullptr,
                         SEC_PLAYER, sWorld->getIntConfig(CONFIG_EXPANSION),
                         0, LOCALE_enUS, 0, false, false, 0);
    Player bot(&session);
    bot.Initialize(cfg.guid);

    // Give the bot a valid base map + position. LootTemplate::Process evaluates
    // per-row CONDITIONS against the looter — CONDITION_TEAM -> GetTeamId, and
    // CONDITION_AREAID/ZONEID -> GetZoneId -> GetMap(). On a mapless transient
    // player GetMap() asserts (Object.h:625): that is the crash. SetMap is a safe
    // pointer-set here (the bot is never AddToWorld'd), so condition eval reads a
    // real map/zone instead of dereferencing null. Detached again before the
    // Player is destroyed so teardown matches the mapless construction path.
    // (Zone-conditional drops evaluate as Elwynn/map 0 — a minor, documented skew.)
    bot.Relocate(-8949.95f, -132.493f, 83.5312f, 0.0f); // Northshire, map 0
    bot.SetMap(sMapMgr->CreateBaseMap(0));

    // NOT registered in ObjectAccessor: the listing/buying path uses this pointer
    // directly; an unregistered owner makes Item::GetOwner() return null so the
    // item map-update path is skipped, and the gold sink runs off the guid-based
    // MailScript. Registration would also expose it to world-player iteration.
    fn(&bot);

    bot.ResetMap();
}

void CMangosAHBot::Update()
{
    if (!_ready)
        return;

    const auto& cfg = gCMangosAHBotConfig;
    RefreshProgression(false);

    // One tightly-scoped transient player PER pass (not one held across all steps):
    // a mapless bot left registered while other systems run trips GetMap().
    for (uint32_t step = 0; step < cfg.tickCompensation; ++step)
    {
        _houseAction = (_houseAction + 1) % 6;
        uint32_t house = _houseAction % CMAHB_HOUSE_COUNT;
        if (_houseAction < CMAHB_HOUSE_COUNT)
            WithTransientBot([&](Player* b) {
                SellPass(b, house, false);
                if (cfg.craftEnable) CraftSellPass(b, house);
            });
        else
            WithTransientBot([&](Player* b) { BuyPass(b, house); });
    }
}

// ===========================================================================
// Rebuild + overrides + reporting (Phase 7)
// ===========================================================================

uint32_t CMangosAHBot::BotAuctionCount(uint32_t houseIdx) const
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM auctionhouse WHERE itemowner = {} AND houseid = {}",
        gCMangosAHBotConfig.guid, CMAHB_AH_IDS[houseIdx]);
    return r ? (*r)[0].Get<uint32_t>() : 0;
}

void CMangosAHBot::ExpireBotAuctions(uint32_t forHouse)
{
    for (uint32_t h = 0; h < CMAHB_HOUSE_COUNT; ++h)
    {
        if (forHouse != CMAHB_HOUSE_COUNT && forHouse != h)
            continue;
        AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(CMAHB_AH_FIDS[h]);
        if (!ah)
            continue;

        std::vector<uint32_t> ids;
        for (auto it = ah->GetAuctionsBegin(); it != ah->GetAuctionsEnd(); ++it)
            if (it->second && it->second->owner.GetCounter() == gCMangosAHBotConfig.guid)
                ids.push_back(it->first);

        auto trans = CharacterDatabase.BeginTransaction();
        for (uint32_t id : ids)
        {
            AuctionEntry* a = ah->GetAuction(id);
            if (!a) continue;
            a->DeleteFromDB(trans);
            sAuctionMgr->RemoveAItem(a->item_guid, true, &trans);
            ah->RemoveAuction(a);
        }
        CharacterDatabase.CommitTransaction(trans);
    }
}

void CMangosAHBot::Rebuild(bool /*all*/, uint32_t forHouse)
{
    if (!_ready)
        return;
    const auto& cfg = gCMangosAHBotConfig;

    ExpireBotAuctions(forHouse);

    // Unlike CMaNGOS (whose per-pass output is tiny), each SellPass here posts the
    // whole filtered itemMap — hundreds to low-thousands of auctions. A handful of
    // passes fills a realistic AH, so we cap hard: a large count would both freeze
    // the world thread and (previously) crash via a long-lived transient player.
    (void)cfg;
    const uint32_t passes = 8;

    for (uint32_t p = 0; p < passes; ++p)
        for (uint32_t h = 0; h < CMAHB_HOUSE_COUNT; ++h)
            if (forHouse == CMAHB_HOUSE_COUNT || forHouse == h)
                WithTransientBot([&](Player* b) {
                    SellPass(b, h, true); // prefill world drops
                    if (gCMangosAHBotConfig.craftEnable) CraftSellPass(b, h); // prefill craft
                });

    LOG_INFO("module", "CMangosAHBot: rebuild complete ({} passes/house).", passes);
}

void CMangosAHBot::SetOverride(uint32_t item, uint32_t value, uint32_t chance, uint32_t minA, uint32_t maxA)
{
    CmAHBOverride o{ value, chance, minA, maxA };
    _overrides[item] = o;
    CharacterDatabase.Execute(
        "REPLACE INTO cmangos_ahbot_items (item, value, add_chance, min_amount, max_amount) "
        "VALUES ({}, {}, {}, {}, {})", item, value, chance, minA, maxA);
}

void CMangosAHBot::ResetOverride(uint32_t item)
{
    _overrides.erase(item);
    CharacterDatabase.Execute("DELETE FROM cmangos_ahbot_items WHERE item = {}", item);
}

std::string CMangosAHBot::StatusReport() const
{
    std::ostringstream ss;
    ss << "CMangosAHBot: ready=" << (_ready ? "yes" : "no")
       << " state=" << (int)_caps.state
       << " maxExp=" << (int)_caps.maxExpansion
       << " ilvlCap=" << (_caps.itemLevelCap == NOCAP ? 0 : _caps.itemLevelCap)
       << " layer3Dropped=" << _layer3Dropped
       << " | auctions A/H/N=" << BotAuctionCount(0) << "/" << BotAuctionCount(1) << "/" << BotAuctionCount(2);
    return ss.str();
}

std::string CMangosAHBot::CraftStatusReport() const
{
    if (!gCMangosAHBotConfig.craftEnable || !_craftBuilt)
        return "Craft layer disabled (Craft.Enable=0).";
    return _craftGraph.StatusReport(_caps);
}

std::string CMangosAHBot::CraftSelfTest() const
{
    if (!gCMangosAHBotConfig.craftEnable || !_craftBuilt)
        return "CRAFT SELFTEST: FAIL craft layer disabled (Craft.Enable=0)";
    const auto& cfg = gCMangosAHBotConfig;
    std::string detail;
    bool ok = _craftGraph.SelfTest(_caps, cfg.progTbcAtState, cfg.progWotlkAtState,
                                   cfg.progSkillCaps, detail);
    return std::string("CRAFT SELFTEST: ") + (ok ? "PASS " : "FAIL ") + detail;
}

std::string CMangosAHBot::ProgressionReport() const
{
    const auto& cfg = gCMangosAHBotConfig;
    std::ostringstream ss;
    ss << "Progression: enable=" << cfg.progEnable
       << " source=" << cfg.progSource
       << " effectiveState=" << (int)_caps.state
       << " maxExpansion=" << (int)_caps.maxExpansion
       << " ilvlCap=" << (_caps.itemLevelCap == NOCAP ? 0 : _caps.itemLevelCap)
       << " skillCap=" << (_caps.skillCap == NOCAP ? 0 : _caps.skillCap)
       << " reqLevelCap=" << (_caps.reqLevelCap == NOCAP ? 0 : _caps.reqLevelCap)
       << "\nVectors: creN=" << _creature[0].size() << " creR=" << _creature[1].size()
       << " creE=" << _creature[2].size() << " creRE=" << _creature[3].size()
       << " creWB=" << _creature[4].size()
       << " go=" << _gameobject.size() << " fish=" << _fishing.size()
       << " skin=" << _skinning.size() << " disen=" << _disenchant.size()
       << " prof=" << _profession.size()
       << "\nUnresolved: creature=" << _excludedUnspawnedCreature << " go=" << _excludedUnspawnedGO;
    return ss.str();
}
