#pragma once

#include "CMangosAHBotCommon.h"
#include "CMangosAHBotProgression.h"
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
};

#define sCMangosAHBot CMangosAHBot::instance()
