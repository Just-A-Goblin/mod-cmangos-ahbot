#pragma once

#include "CMangosAHBotCommon.h"
#include "CMangosAHBotProgression.h"
#include "CMangosAHBotRecipes.h"
#include "CMangosAHBotCost.h"
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
    std::string CraftSimulateCost(uint32_t n) const; // C2: cost queries only, no listing

private:
    CMangosAHBot() = default;

    // Enumeration + classification (Phase 3 / addendum Phase 3.5)
    void BuildClassifiedSources();
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
};

#define sCMangosAHBot CMangosAHBot::instance()
