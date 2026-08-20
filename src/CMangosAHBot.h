#pragma once

#include "CMangosAHBotCommon.h"
#include "CMangosAHBotProgression.h"
#include "CMangosAHBotRecipes.h"
#include "CMangosAHBotCost.h"
#include "CMangosAHBotCraft.h"
#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Player;
struct ItemTemplate;
class LootStore;

// Per-item override row (data/sql/db-characters/cmangos_ahbot_items.sql).
struct CmAHBOverride
{
    uint32_t value     = 0; // 0 => blacklist; >0 => fixed per-unit price base
    uint32_t addChance = 0; // >0 => inject this item at this % chance, bypassing loot roll
    uint32_t minAmount = 0;
    uint32_t maxAmount = 0;
    int32_t  craftWeight = -1; // §8.2: -1 no override (neutral 100); 0 never craft; else weight mult %
    int32_t  craftMargin = -1; // §8.2: -1 no override; else margin %
};

// Singleton mirroring the CMaNGOS AuctionHouseBot class (plan §3).
class CMangosAHBot
{
public:
    static CMangosAHBot* instance();

    // Lifecycle
    void Initialize();                 // build vectors + resolve caps (after config load)
    bool Ready() const { return _ready; }
    void Update();                     // 60s core hook; runs tickCompensation rotation steps
    void ReloadData();                 // .cmahbot reload — reload overrides + rebuild vectors

    // Progression
    void RefreshProgression(bool force);
    CmAHBCaps const& Caps() const { return _caps; }

    // GM ops
    void Rebuild(bool all, uint32_t forHouse = CMAHB_HOUSE_COUNT);
    void SetOverride(uint32_t item, uint32_t value, uint32_t chance, uint32_t minA, uint32_t maxA);
    void ResetOverride(uint32_t item);
    std::string StatusReport() const;
    std::string ProgressionReport() const;

    // Craft layer (crafting addendum). Graph is built only when Craft.Enable=1;
    // with it off the module is behaviorally identical to the base seller/buyer.
    std::string CraftStatusReport() const;
    std::string CraftSelfTest() const;   // single "CRAFT SELFTEST: PASS|FAIL ..." line
    std::string CraftSimulateCost(uint32_t n) const;     // C2: cost queries only, no listing
    std::string CraftSimulateSessions(uint32_t n) const; // C3: run sessions, listing suppressed
    // C7 §8.3: write a craft-dump CSV (item,name,category,rarity,stack,unit_price,
    // unit_matcost,ratio,leveling). liveAH=true dumps bot-owned AH listings that are
    // craft products; false dumps a non-mutating production+leveling sim.
    std::string CraftDump(const std::string& file, bool liveAH);

private:
    CMangosAHBot() = default;

    // Enumeration + classification (Phase 3 / addendum Phase 3.5)
    void BuildClassifiedSources();
    void BuildItemExpansion(); // itemId -> earliest expansion (from loot sources), for reagent-era gating
    void BuildProfessionPool();
    void BuildDisenchantPool();
    void BuildVendorSet();
    void LoadOverrides();
    void RebuildFilteredVectors();

    uint8_t ExpansionOfMap(uint32_t mapId) const;
    uint8_t ExpansionOfArea(uint32_t areaId) const;

    // One-shot craft-layer startup diagnostic (logs selftest + a per-era gating
    // sweep). Cheap; only runs when the craft graph is built. Leaves the live mask
    // as it found it.
    void CraftStartupDiagnostics();

    // Project the available graph recipes into the cost-engine facade view (§10.2)
    // and index producers. Rebuilt when the availability mask changes.
    void BuildCostProjection();
    // Build the per-pass market-anchor median map from live bot listings (neutral
    // house, C-A7): itemId -> median buyout-per-unit.
    void BuildAnchorMedians(std::unordered_map<uint32_t, uint64_t>& out) const;
    // C2 cost hand-check over the real graph (logged at startup + `craft simulate`).
    std::string CraftCostChains(uint32_t sampleN) const;

    // Craft session layer (C3, §4). Population is rolled at init and its caps track
    // progression; candidates are the available recipes per skill line.
    void BuildPopulation();
    void UpdatePopulationCaps();     // on progression change: raise caps, keep skills
    void BuildCraftCandidates();     // skillLine -> available recipes (rebuilt with the mask)
    void ParseDemand();              // demand weights + state boosts + profession shares (C4 §5)
    // Run n sessions over `pop`, appending would-be listings to `out`. Mutates `pop`
    // (skill-ups). Below-cap crafters level (§4.4); at-cap crafters produce (§5.2) when
    // `production` is true. countCd tracks per-recipe CD output within the run (§4.5).
    // If ledgerCredit != nullptr, each session credits its reagents (the demand ledger,
    // §6.1) — passed only on the LIVE path so the sim/sweep don't mutate live demand.
    void RunSessions(std::vector<Crafter>& pop, uint32_t n, CMangosAHBotCost& cost,
                     std::vector<CraftListing>& out, bool production,
                     std::unordered_map<uint32_t, uint32_t>& cdCount,
                     std::unordered_map<uint32_t, uint32_t>* ledgerCredit = nullptr);
    void CraftSellPass(Player* bot, uint32_t houseIdx); // real sessions -> posted listings
    void CraftDemandSweep();  // C4: production category-mix across probe states (logged)
    void CraftTextureSample(); // C6: stack/price-point texture per category (logged)
    void CraftTimingBench();   // C7: p50/p99 of the craft compute per pass (logged)

    // Simulation (Phase 4)
    void AddLootToItemMap(Player* bot, CmAHBItemMap& out);
    void SimSource(const std::vector<uint32_t>& ids, LootStore const& store,
                   const CmAHBSourceConfig& cfg, Player* bot, CmAHBItemMap& out);
    void SimProfession(CmAHBItemMap& out);
    void ApplyOverridesToMap(CmAHBItemMap& out);

    // Pricing + filters (Phase 5)
    bool PassesFilters(ItemTemplate const* proto) const;
    uint64_t CalculateBuyoutPrice(ItemTemplate const* proto) const; // per unit
    uint64_t ValueWithVariance(uint64_t value) const;

    // Passes
    void SellPass(Player* bot, uint32_t houseIdx, bool prefill);
    void BuyPass(Player* bot, uint32_t houseIdx);

    // Helpers
    // Runs fn with a transient bot Player scoped tightly to this call. The player
    // is registered in ObjectAccessor only for fn's duration — never long-lived,
    // because a mapless transient player observed by other subsystems (playerbots)
    // asserts on GetMap(). Keep every pass's window as short as a normal tick.
    void WithTransientBot(const std::function<void(Player*)>& fn);
    bool ResolveBotCharacter();
    uint32_t BotAuctionCount(uint32_t houseIdx) const;
    void ExpireBotAuctions(uint32_t forHouse);

    // ---- classified (full) source lists ----
    std::vector<CmAHBClassifiedId> _creatureClassified[CMAHB_CREATURE_RANKS];
    std::vector<CmAHBClassifiedId> _gameobjectClassified;
    std::vector<CmAHBClassifiedId> _fishingClassified;
    std::vector<CmAHBClassifiedId> _skinningClassified;
    std::vector<std::pair<uint32_t, uint32_t>> _professionClassified; // (itemId, minSkillRank)
    std::vector<std::pair<uint32_t, uint32_t>> _disenchantClassified; // (disenchantId, minSourceIlvl)

    // ---- filtered (current-progression) working lists ----
    std::vector<uint32_t> _creature[CMAHB_CREATURE_RANKS];
    std::vector<uint32_t> _gameobject;
    std::vector<uint32_t> _fishing;
    std::vector<uint32_t> _skinning;
    std::vector<uint32_t> _disenchant;
    std::vector<uint32_t> _profession;

    std::unordered_set<uint32_t> _vendorItems;
    std::unordered_map<uint32_t, CmAHBOverride> _overrides;
    std::unordered_map<uint32_t, uint8_t> _itemExpansion; // mat/item -> plurality expansion

    // Spawn-count votes per loot id per expansion [vanilla,tbc,wotlk], accumulated in
    // BuildClassifiedSources and consumed by BuildItemExpansion (plurality classify).
    // Separate from the MinMerge'd source vectors so base gating is unchanged (#1).
    std::unordered_map<uint32_t, std::array<uint32_t, 3>> _creatureLootVotes;
    std::unordered_map<uint32_t, std::array<uint32_t, 3>> _goLootVotes;
    std::unordered_map<uint32_t, std::array<uint32_t, 3>> _skinLootVotes;

    // diagnostics
    uint32_t _excludedUnspawnedCreature = 0;
    uint32_t _excludedUnspawnedGO = 0;
    uint64_t _layer3Dropped = 0; // addendum §4 Layer 3 counter

    CmAHBCaps _caps;
    uint32_t  _lastProgRefresh = 0; // unix seconds
    uint32_t  _houseAction = 0;     // rotation 0..5
    bool      _ready = false;
    bool      _sourcesBuilt = false;

    // Craft layer state (built lazily when Craft.Enable=1).
    CMangosAHBotRecipeGraph _craftGraph;
    bool _craftBuilt = false;

    // Cost-engine facade projection of the available graph (§10.2). Pointers in
    // _costProducers reference _costRecipes, so it is filled fully then indexed and
    // never reallocated while indexed.
    std::vector<CostRecipe> _costRecipes;
    std::unordered_map<uint32_t, std::vector<const CostRecipe*>> _costProducers;

    // Craft session state (C3). Candidate pointers reference _craftGraph.Recipes(),
    // which is stable after Build(); the mask flips availability in place.
    std::vector<Crafter> _population;
    std::unordered_map<uint32_t, std::vector<const CraftRecipe*>> _craftCandidates;
    CraftSkillChances _craftChances;

    // Demand model (C4 §5). _demandWeights[era][category]; _stateBoost keyed by
    // state*CAT_COUNT+category -> mult%; _professionShare[skillLine] -> share of the
    // population (for §4.5 CD caps). CD output tracked per spell in a rolling window.
    uint32_t _demandWeights[3][CAT_COUNT] = {};
    std::unordered_map<uint32_t, uint32_t> _stateBoost;
    std::unordered_map<uint32_t, double>   _professionShare;
    std::unordered_map<uint32_t, uint32_t> _cdCountLive; // spellId -> CD crafts in current window
    uint32_t _cdWindowStart = 0;                          // unix; reset every 24h

    // Demand ledger + saturation (C5 §6). Both roll on a Craft.Ledger.WindowHours window.
    std::unordered_map<uint32_t, uint32_t> _ledgerCredit; // itemId -> mat demand this window
    std::unordered_map<uint32_t, uint32_t> _ledgerBought; // itemId -> units bought this window (§6.4)
    uint32_t _ledgerWindowStart = 0;
    std::vector<uint32_t> _testAuctionIds; // ids created by craft testlist (for cleanup)

public:
    // §8.3: create `count` synthetic non-bot auctions (owner = a resolved real char) of
    // `stack` units at `price`/unit, for the buyer test harness (gated by
    // Craft.TestCommands). Returns a status string.
    std::string CraftTestList(uint32_t itemId, uint32_t count, uint32_t stack, uint32_t price);
    void CraftBuyerSelfTest();  // Craft.TestCommands-gated in-server buyer check (logged)
};

#define sCMangosAHBot CMangosAHBot::instance()
