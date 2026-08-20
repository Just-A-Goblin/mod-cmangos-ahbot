/*
 * CMangosAHBotRecipes — the craft-layer recipe graph (crafting addendum §2).
 *
 * Built once at startup from the profession spell scan (same filter as the base
 * module's BuildProfessionPool, C-A3/C-A9 verified), classified by acquisition
 * rarity (§2.2) and demand category (§5.1), gated per progression (§2.3) via a
 * per-state availability mask recomputed on the existing RefreshInterval poll.
 *
 * This class is the concrete data source the C2 cost engine will consume behind
 * the IRecipeSource facade; keep the node struct a POD so it can back the offline
 * test fixture without dragging in sSpellMgr.
 */
#pragma once

#include "CMangosAHBotCommon.h"
#include "CMangosAHBotProgression.h"
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class CMangosAHBotConfig;

// Acquisition rarity — margin correlates with it (§2.2).
enum RecipeRarity : uint8_t
{
    RARITY_TRAINER = 0,
    RARITY_VENDOR,
    RARITY_DROP,
    RARITY_UNSOURCED, // treated as DROP for margins/gating
    RARITY_COUNT
};

// Demand category (§5.1). Classified at graph-build time.
enum ItemCategory : uint8_t
{
    CAT_FLASK = 0,
    CAT_ELIXIR_POT,
    CAT_FOOD,
    CAT_BAG,
    CAT_GEM_CUT,
    CAT_SCROLL,
    CAT_AMMO,
    CAT_GEAR,
    CAT_INTERMEDIATE,
    CAT_MISC,
    CAT_COUNT
};

const char* RarityName(RecipeRarity r);
const char* CategoryName(ItemCategory c);

// One recipe node (§2.1). POD-ish: no owning pointers, safe to copy into a fixture.
struct CraftRecipe
{
    uint32_t spellId      = 0;
    uint32_t skillLine    = 0;   // C-A8 skill-line id
    uint32_t productItem  = 0;
    uint32_t productCount = 1;   // Effects[e].BasePoints + 1 (C-A1 area; multi-output verified)
    std::vector<std::pair<uint32_t, uint32_t>> reagents; // (itemId, count) from Reagent[]/ReagentCount[]

    uint16_t minSkill    = 0;    // SkillLineAbilityEntry::MinSkillLineRank
    uint16_t greySkill   = 0;    // TrivialSkillLineRankHigh (C-A2)
    uint16_t yellowSkill = 0;    // TrivialSkillLineRankLow

    RecipeRarity rarity     = RARITY_UNSOURCED;
    ItemCategory category   = CAT_MISC;
    uint8_t      expansion  = CMAHB_EXP_VANILLA; // era this recipe belongs to (baked; §2.3 rules 1+2)
    uint32_t     productIlvl = 0;
    uint32_t     productReqLevel = 0;
    bool         dailyCooldown = false;          // max(RecoveryTime, CategoryRecoveryTime) >= 24h (C-A5)

    bool         available = false;              // current availability mask (§2.3)
};

class CMangosAHBotRecipeGraph
{
public:
    // Build the full graph. Uses sSpellMgr + world DB directly (this IS the
    // concrete recipe source; the facade wraps it in C2). vendorItems is the
    // module's existing npc_vendor set, reused for VENDOR rarity. itemExpansion maps
    // a mat/item to the earliest expansion it is obtainable in (from loot sources) —
    // used to bake reagent-era gating (§2.3 rule 3 proxy: a recipe can't be earlier
    // than its rarest reagent, so a bag using Netherweave is TBC even at ilvl 1).
    void Build(const CMangosAHBotConfig& cfg, const std::unordered_set<uint32_t>& vendorItems,
               const std::unordered_map<uint32_t, uint8_t>& itemExpansion);

    // Recompute per-recipe availability for the given caps (§2.3). Cheap: only the
    // mask changes on progression transition, not the graph.
    void RecomputeMask(const CmAHBCaps& caps, uint32_t tbcAtState, uint32_t wotlkAtState,
                       const uint32_t skillCaps[3]);

    bool   Built()        const { return _built; }
    size_t Size()         const { return _recipes.size(); }
    double BuildSeconds() const { return _buildSeconds; }

    const std::vector<CraftRecipe>& Recipes() const { return _recipes; }

    // itemId -> indices into _recipes that produce it (only used by the C2 cost engine).
    const std::vector<uint32_t>* Producers(uint32_t itemId) const;

    // Aggregate counts for logging / selftest.
    uint32_t AvailableCount() const;
    uint32_t CategoryCount(ItemCategory c, bool availableOnly) const;
    uint32_t SkillLineAvailableCount(uint32_t skillLine) const;

    // Multi-line startup block: per-profession x per-rarity, category histogram,
    // cooldown count, build timing (§2.2 / §5.1 / constraint #8).
    std::string StartupReport() const;
    // `craft status`-style current-state summary.
    std::string StatusReport(const CmAHBCaps& caps) const;

    // C1 selftest invariants (subset of §10.3 checkable without listings):
    // mask monotonic w.r.t. caps, no available recipe exceeds skill/expansion gate,
    // graph nonempty, category MISC a minority. Fills `detail` on failure.
    bool SelfTest(const CmAHBCaps& caps, uint32_t tbcAtState, uint32_t wotlkAtState,
                  const uint32_t skillCaps[3], std::string& detail) const;

private:
    void SendGraphReport() const; // logs StartupReport() line by line

    RecipeRarity ClassifyRarity(uint32_t spellId, uint32_t recipeItemId,
                                const std::unordered_set<uint32_t>& trainerSpells,
                                const std::unordered_set<uint32_t>& vendorItems,
                                const std::unordered_set<uint32_t>& lootItems) const;

    std::vector<CraftRecipe> _recipes;
    std::unordered_map<uint32_t, std::vector<uint32_t>> _producers;

    // per-profession(skillLine) x per-rarity build-time counts (full graph, pre-mask)
    std::unordered_map<uint32_t, std::array<uint32_t, RARITY_COUNT>> _profRarity;
    std::array<uint32_t, CAT_COUNT> _categoryCounts{};
    uint32_t _cooldownCount = 0;
    uint32_t _skippedNoReagent = 0;
    uint32_t _skippedProduct   = 0;

    double _buildSeconds = 0.0;
    bool   _built = false;
};
