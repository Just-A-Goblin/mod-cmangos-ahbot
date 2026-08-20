/*
 * CMangosAHBotCost — recursive memoized cycle-safe cost engine (addendum §3.1/§3.2).
 * Pure logic over the IRecipeSource / IMarketAnchor facade; no core dependencies,
 * so it links into tools/craft-tests/.
 */
#include "CMangosAHBotCost.h"
#include <algorithm>

void CMangosAHBotCost::NewPass()
{
    _memo.clear();
    _inProgress.clear();
    _cycleHits = 0;
    _depthHits = 0;
}

uint64_t CMangosAHBotCost::MatValue(uint32_t itemId)
{
    return MatValueRec(itemId, 0);
}

uint64_t CMangosAHBotCost::MatValueRec(uint32_t itemId, int depth)
{
    if (auto it = _memo.find(itemId); it != _memo.end())
        return it->second;

    uint64_t anchor = _anchor.Anchor(itemId);

    // Cycle (transmute Earth<->Water etc.): value the reagent at market, do not
    // recurse back into it. Not memoized — the value is context-dependent here.
    if (_inProgress.count(itemId))
    {
        ++_cycleHits;
        return anchor;
    }
    // Depth guard (belt-and-suspenders, §3.1). Also not memoized (incomplete).
    if (depth >= kMaxDepth)
    {
        ++_depthHits;
        return anchor;
    }

    _inProgress.insert(itemId);

    uint64_t best = anchor; // "buy from market" is always an option
    for (const CostRecipe* r : _src.Producers(itemId))
    {
        if (!r || !r->available || r->productCount == 0)
            continue;
        // Intermediates: market vs make — take the cheaper (§3.1 min()).
        uint64_t sum = 0;
        for (auto& [rid, rc] : r->reagents)
            sum += MatValueRec(rid, depth + 1) * rc;
        uint64_t craftCost = sum / r->productCount;
        best = std::min(best, craftCost);
    }

    _inProgress.erase(itemId);
    _memo[itemId] = best;
    return best;
}

uint64_t CMangosAHBotCost::CraftCost(const CostRecipe& r)
{
    if (r.productCount == 0)
        return 0;
    uint64_t sum = 0;
    for (auto& [rid, rc] : r.reagents)
        sum += MatValue(rid) * rc;
    return sum / r.productCount;
}

uint64_t CMangosAHBotCost::CraftPrice(const CostRecipe& r, const IMarginModel& margins)
{
    uint64_t cost = CraftCost(r);
    uint32_t marginPct = margins.ProductionMarginPct(r);
    return cost * marginPct / 100;
}
