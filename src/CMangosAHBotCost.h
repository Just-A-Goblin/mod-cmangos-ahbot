/*
 * CMangosAHBotCost — the craft-layer cost engine (crafting addendum §3).
 *
 * Recursive, memoized, cycle-safe material valuation + listing price. The single
 * valuation path for crafted goods: the seller lists at CraftPrice() and the buyer
 * values crafted goods at MatValue()/CraftCost() through THIS class — buyer
 * coherence (constraint #2), no second formula.
 *
 * Facade (constraint #3): this engine talks only to the abstract IRecipeSource /
 * IMarketAnchor below, never to sSpellMgr or AuctionHouseObject. That is what lets
 * the whole thing compile into tools/craft-tests/ and be unit-tested (cycles,
 * min(market,make), margins) before it ever touches a server.
 */
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A recipe as the cost engine needs to see it (a projection of CraftRecipe).
struct CostRecipe
{
    uint32_t productItem  = 0;
    uint32_t productCount = 1;
    std::vector<std::pair<uint32_t, uint32_t>> reagents; // (itemId, count)
    uint8_t  rarity       = 0; // RecipeRarity, kept opaque here (margin lookup is external)
    bool     available    = true;
    bool     dailyCooldown = false;
};

// Abstract graph view. The live impl wraps CMangosAHBotRecipeGraph; the test impl
// is a hand-built vector.
class IRecipeSource
{
public:
    virtual ~IRecipeSource() = default;

    // Recipes that produce `itemId` AND are currently available. Empty => leaf mat.
    virtual const std::vector<const CostRecipe*>& Producers(uint32_t itemId) const = 0;
};

// Abstract market anchor. The live impl returns the clamped median of live bot
// listings (falling back to the intrinsic formula); the test impl returns a fixed
// per-item table.
class IMarketAnchor
{
public:
    virtual ~IMarketAnchor() = default;

    // Anchor price per unit for `itemId` (already clamped to
    // [AnchorClampMin, AnchorClampMax] % of intrinsic by the impl). Must be > 0 for
    // any item the engine can be asked about (leaf mats included).
    virtual uint64_t Anchor(uint32_t itemId) const = 0;
};

// Margin band + variance provider (kept abstract so tests are deterministic and the
// live impl can pull config + the module RNG). Returns a per-mille-free integer %:
// e.g. 125 means 1.25x. Variance is applied by the caller's ValueWithVariance.
class IMarginModel
{
public:
    virtual ~IMarginModel() = default;
    // Production margin for a recipe (rarity + cooldown bonus already folded in).
    virtual uint32_t ProductionMarginPct(const CostRecipe& r) const = 0;
    // Leveling-dump margin (below 100%).
    virtual uint32_t LevelingMarginPct() const = 0;
};

class CMangosAHBotCost
{
public:
    CMangosAHBotCost(const IRecipeSource& src, const IMarketAnchor& anchor)
        : _src(src), _anchor(anchor) {}

    // Reset the per-pass memo (addendum §3.1: memo lifetime is one sell pass).
    void NewPass();

    // Recursive material value per unit of `itemId`: min(market anchor, cheapest
    // make-cost via available producers). Cycle-safe (transmutes) + depth-limited.
    uint64_t MatValue(uint32_t itemId);

    // Listing price per unit for a recipe: (Σ MatValue(reagent)*count / productCount) * margin/100.
    // Margin comes from the IMarginModel (production) — the caller applies variance.
    uint64_t CraftPrice(const CostRecipe& r, const IMarginModel& margins);

    // Bare craft cost per unit (no margin) — used by the buyer for crafted-goods coherence.
    uint64_t CraftCost(const CostRecipe& r);

    // Diagnostics (addendum §3.1 hard requirement: prove cycles are bounded).
    uint64_t CycleHits() const { return _cycleHits; }
    uint64_t DepthHits() const { return _depthHits; }
    size_t   MemoSize()  const { return _memo.size(); }

    static constexpr int kMaxDepth = 8; // belt-and-suspenders guard (§3.1)

private:
    uint64_t MatValueRec(uint32_t itemId, int depth);

    const IRecipeSource& _src;
    const IMarketAnchor& _anchor;

    std::unordered_map<uint32_t, uint64_t> _memo;
    std::unordered_set<uint32_t>           _inProgress; // cycle detection set
    uint64_t _cycleHits = 0;
    uint64_t _depthHits = 0;
};
