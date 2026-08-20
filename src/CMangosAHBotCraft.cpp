/*
 * CMangosAHBotCraft — pure craft-sim core (addendum §4). No core headers, so it
 * links into tools/craft-tests/ alongside the cost engine.
 */
#include "CMangosAHBotCraft.h"
#include <algorithm>
#include <cmath>

namespace CraftSim
{
    int SkillGainChancePerMille(uint32_t skill, uint16_t greyLvl, uint16_t yellowLvl,
                                const CraftSkillChances& c)
    {
        // green = midpoint(grey, yellow), per Player::UpdateCraftSkill (C-A2).
        uint32_t green = (uint32_t(greyLvl) + uint32_t(yellowLvl)) / 2;
        if (skill >= greyLvl)   return int(c.grey   * 10);
        if (skill >= green)     return int(c.green  * 10);
        if (skill >= yellowLvl) return int(c.yellow * 10);
        return int(c.orange * 10);
    }

    uint64_t MatCost(const CraftRecipe& r, CMangosAHBotCost& cost)
    {
        if (r.productCount == 0)
            return 0;
        uint64_t sum = 0;
        for (auto& [rid, rc] : r.reagents)
            sum += cost.MatValue(rid) * rc;
        return sum / r.productCount;
    }

    double LevelingScore(const CraftRecipe& r, uint32_t skill, CMangosAHBotCost& cost,
                         const CraftSkillChances& c)
    {
        // Trainable window: can craft it (skill >= minSkill) and still skills up
        // (skill < greySkill).
        if (skill < r.minSkill || skill >= r.greySkill)
            return 0.0;
        int chance = SkillGainChancePerMille(skill, r.greySkill, r.yellowSkill, c);
        if (chance <= 0)
            return 0.0;
        uint64_t mc = MatCost(r, cost);
        double denom = double(mc > 0 ? mc : 1);
        return double(chance) / denom; // skill-up per gold
    }

    const CraftRecipe* ChooseLevelingRecipe(const std::vector<const CraftRecipe*>& profRecipes,
                                            uint32_t skill, CMangosAHBotCost& cost,
                                            const CraftSkillChances& c, std::mt19937& rng,
                                            int topK)
    {
        // Score all trainable recipes, keep the top-k, weighted-sample among them.
        std::vector<std::pair<double, const CraftRecipe*>> scored;
        scored.reserve(profRecipes.size());
        for (const CraftRecipe* r : profRecipes)
        {
            if (!r || !r->available)
                continue;
            double s = LevelingScore(*r, skill, cost, c);
            if (s > 0.0)
                scored.emplace_back(s, r);
        }
        if (scored.empty())
            return nullptr;

        std::partial_sort(scored.begin(),
                          scored.begin() + std::min<size_t>(topK, scored.size()),
                          scored.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        size_t k = std::min<size_t>(topK, scored.size());

        double total = 0.0;
        for (size_t i = 0; i < k; ++i)
            total += scored[i].first;
        std::uniform_real_distribution<double> pick(0.0, total);
        double roll = pick(rng);
        for (size_t i = 0; i < k; ++i)
        {
            roll -= scored[i].first;
            if (roll <= 0.0)
                return scored[i].second;
        }
        return scored[0].second;
    }

    uint32_t SimulateSkillUps(Crafter& cr, const CraftRecipe& r, uint32_t crafts,
                              const CraftSkillChances& c, std::mt19937& rng)
    {
        uint32_t ups = 0;
        std::uniform_int_distribution<int> d(0, 999); // per-mille roll
        for (uint32_t i = 0; i < crafts && cr.skill < cr.cap; ++i)
        {
            int chance = SkillGainChancePerMille(cr.skill, r.greySkill, r.yellowSkill, c);
            if (chance > 0 && d(rng) < chance)
            {
                ++cr.skill;
                ++ups;
            }
        }
        return ups;
    }

    uint16_t CapForState(uint8_t state, uint32_t tbcAt, uint32_t wotlkAt)
    {
        if (state < tbcAt)   return 300;
        if (state < wotlkAt) return 375;
        return 450;
    }

    const CraftRecipe* ChooseProductionRecipe(const std::vector<const CraftRecipe*>& profRecipes,
                                              const uint32_t* catWeight, uint32_t ilvlCap,
                                              uint32_t gearWindow,
                                              const std::function<uint32_t(uint8_t)>& stateBoostPct,
                                              const std::function<uint32_t(uint32_t)>& overrideWeightPct,
                                              std::mt19937& rng)
    {
        // Distribute each category's weight ACROSS its available recipes, so a
        // category's selection share tracks the configured weight regardless of how
        // many recipes it happens to have (else 40 elixirs would bury 4 flasks). This
        // refines §5.2's literal per-recipe formula toward its "demand weight" intent.
        uint32_t catCount[CAT_COUNT] = {0};
        for (const CraftRecipe* r : profRecipes)
            if (r && r->available)
                ++catCount[r->category];

        std::vector<std::pair<double, const CraftRecipe*>> scored;
        double total = 0.0;
        for (const CraftRecipe* r : profRecipes)
        {
            if (!r || !r->available)
                continue;
            double w = catWeight[r->category];
            if (w <= 0.0)
                continue;
            w /= double(catCount[r->category] ? catCount[r->category] : 1); // per-recipe share
            // GEAR: triangular demand window peaking at the ilvl cap (§5.2).
            if (r->category == CAT_GEAR)
            {
                uint32_t lo = ilvlCap > gearWindow ? ilvlCap - gearWindow : 0;
                if (gearWindow == 0 || r->productIlvl < lo || r->productIlvl > ilvlCap)
                    continue;
                w *= double(r->productIlvl - lo) / double(gearWindow); // 0 at lo, 1 at cap
            }
            w *= stateBoostPct(r->category) / 100.0;
            uint32_t ow = overrideWeightPct(r->productItem);
            if (ow == 0)
                continue;               // override: never craft
            w *= ow / 100.0;
            if (w <= 0.0)
                continue;
            total += w;
            scored.emplace_back(total, r); // cumulative for sampling
        }
        if (scored.empty() || total <= 0.0)
            return nullptr;
        std::uniform_real_distribution<double> pick(0.0, total);
        double roll = pick(rng);
        for (auto& [cum, r] : scored)
            if (roll <= cum)
                return r;
        return scored.back().second;
    }

    uint32_t SaturationMultPct(uint32_t bought, uint32_t cap, uint32_t floorMultPct)
    {
        if (cap == 0) return 100;
        if (bought <= cap) return 100;
        if (bought >= 3 * cap) return floorMultPct;
        // Linear from 100% at `cap` to floorMultPct at 3*cap (span = 2*cap).
        uint32_t drop = (100 - floorMultPct) * (bought - cap) / (2 * cap);
        return 100 - drop;
    }

    uint32_t CategoryBatch(uint8_t category, std::mt19937& rng)
    {
        auto u = [&](uint32_t a, uint32_t b) {
            return std::uniform_int_distribution<uint32_t>(a, b)(rng);
        };
        switch (category)
        {
            case CAT_GEAR: case CAT_BAG: case CAT_GEM_CUT: return u(1, 3);
            case CAT_FLASK: case CAT_ELIXIR_POT: case CAT_FOOD: return u(5, 20);
            case CAT_AMMO:          return u(2, 5);
            case CAT_INTERMEDIATE:  return u(5, 15);
            default:                return u(1, 5);
        }
    }

    std::vector<Crafter> RollPopulation(uint32_t size,
                                        const std::vector<std::pair<uint32_t, uint32_t>>& weights,
                                        uint16_t cap, double alpha, double beta,
                                        std::mt19937& rng)
    {
        std::vector<Crafter> pop;
        if (weights.empty() || size == 0)
            return pop;
        pop.reserve(size);

        uint32_t wsum = 0;
        for (auto& [sl, w] : weights)
            wsum += w;
        if (wsum == 0)
            return pop;

        std::uniform_int_distribution<uint32_t> wpick(0, wsum - 1);
        // Beta(a,b) = X/(X+Y), X~Gamma(a,1), Y~Gamma(b,1).
        std::gamma_distribution<double> ga(alpha, 1.0), gb(beta, 1.0);

        for (uint32_t i = 0; i < size; ++i)
        {
            uint32_t pickv = wpick(rng), acc = 0, sl = weights.front().first;
            for (auto& [wsl, w] : weights)
            {
                acc += w;
                if (pickv < acc) { sl = wsl; break; }
            }
            double x = ga(rng), y = gb(rng);
            double frac = (x + y > 0.0) ? x / (x + y) : 0.5;
            uint16_t skill = static_cast<uint16_t>(std::lround(frac * cap));
            if (skill < 1)   skill = 1;
            if (skill > cap) skill = cap;
            pop.push_back(Crafter{ sl, skill, cap });
        }
        return pop;
    }
}
