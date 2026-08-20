/*
 * CMangosAHBotCraft — crafter population + session logic (crafting addendum §4).
 *
 * The pure, offline-testable core of the craft simulator: the skill-up chance
 * (ported verbatim from AC's SkillGainChance, C-A2 / constraint #7), the leveling
 * recipe chooser (§4.4, the emergent-glut engine), and the population roll (§4.1/4.2).
 * These take a std::mt19937 and the cost engine by reference, so tools/craft-tests/
 * can exercise them without a server (§10.2). Server-side posting lives in
 * CMangosAHBot (it needs the auction path); this file stays core-free.
 */
#pragma once

#include "CMangosAHBotRecipes.h"
#include "CMangosAHBotCost.h"
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

// AC's per-difficulty skill-up chances (percent), from CONFIG_SKILL_CHANCE_* (C-A2).
struct CraftSkillChances
{
    uint32_t grey = 0, green = 25, yellow = 75, orange = 100;
};

// One virtual crafter — 12 bytes of state (§4.1). Persisted in memory only.
struct Crafter
{
    uint32_t skillLine = 0;
    uint16_t skill     = 0;
    uint16_t cap       = 300;
    bool     leveling() const { return skill < cap; }
};

// A would-be auction line produced by a session (posted by the server, or
// aggregated by `craft simulate`).
struct CraftListing
{
    uint32_t itemId    = 0;
    uint32_t count     = 0;
    uint64_t unitPrice = 0;
    uint64_t unitMatCost = 0;
    uint32_t skillLine = 0;
    uint8_t  category  = CAT_MISC;
    bool     leveling  = false; // leveling dump (floor 60% of matcost) vs producing (100%)
};

namespace CraftSim
{
    // C-A2 verbatim: grey=TrivialHigh, yellow=TrivialLow, green=midpoint. Returns
    // per-mille (percent x10), matching Player::UpdateSkillPro's Chance units.
    int SkillGainChancePerMille(uint32_t skill, uint16_t greyLvl, uint16_t yellowLvl,
                                const CraftSkillChances& c);

    // Per-unit material cost of a recipe via the cost engine (Σ MatValue*count/productCount).
    uint64_t MatCost(const CraftRecipe& r, CMangosAHBotCost& cost);

    // Leveling score (§4.4): P(skillup at S) / matcost. 0 when S is outside the
    // trainable window [minSkill, greySkill).
    double LevelingScore(const CraftRecipe& r, uint32_t skill, CMangosAHBotCost& cost,
                         const CraftSkillChances& c);

    // Choose a leveling recipe: sample from the top-k by score, weighted (not argmax —
    // real players didn't all follow one guide). nullptr if nothing trainable at S.
    const CraftRecipe* ChooseLevelingRecipe(const std::vector<const CraftRecipe*>& profRecipes,
                                            uint32_t skill, CMangosAHBotCost& cost,
                                            const CraftSkillChances& c, std::mt19937& rng,
                                            int topK = 3);

    // Simulate `crafts` craft attempts at the recipe, advancing skill via C-A2 rolls.
    // Returns the number of skill-ups (skill is clamped to cap).
    uint32_t SimulateSkillUps(Crafter& cr, const CraftRecipe& r, uint32_t crafts,
                              const CraftSkillChances& c, std::mt19937& rng);

    // The profession skill cap for a progression state (§4.2, one source of truth
    // with §2.3): 300 below tbcAt, 375 below wotlkAt, else 450.
    uint16_t CapForState(uint8_t state, uint32_t tbcAt, uint32_t wotlkAt);

    // Roll the population (§4.1/§4.2): distribute `size` crafters across professions
    // by weight, skill = cap * Beta(alpha,beta). `weights` is (skillLine, weight).
    std::vector<Crafter> RollPopulation(uint32_t size,
                                        const std::vector<std::pair<uint32_t, uint32_t>>& weights,
                                        uint16_t cap, double alpha, double beta,
                                        std::mt19937& rng);

    // Production recipe choice (§5.2): weight = CategoryWeight(era) * ilvlWindow(GEAR)
    // * stateBoost * overrideWeight, sampled proportionally among the crafter's
    // profession's available recipes. catWeight is indexed by ItemCategory (size
    // CAT_COUNT). stateBoostPct/overrideWeightPct return percentages (100 = neutral;
    // overrideWeightPct==0 => never craft). GEAR outside [ilvlCap-gearWindow, ilvlCap]
    // gets weight 0. Returns nullptr if nothing is craftable.
    const CraftRecipe* ChooseProductionRecipe(const std::vector<const CraftRecipe*>& profRecipes,
                                              const uint32_t* catWeight, uint32_t ilvlCap,
                                              uint32_t gearWindow,
                                              const std::function<uint32_t(uint8_t)>& stateBoostPct,
                                              const std::function<uint32_t(uint32_t)>& overrideWeightPct,
                                              std::mt19937& rng);

    // Realistic production batch (# of crafts) by category (§7.2, basic form).
    uint32_t CategoryBatch(uint8_t category, std::mt19937& rng);

    // Demand-saturation multiplier as a percentage (§6.4, exploit control): 100 up to
    // `cap` units bought in the window, then linear decay to `floorMultPct` at 3*cap
    // (dump 100 flasks -> price crashes). Caps the gold faucet with one mechanism.
    uint32_t SaturationMultPct(uint32_t bought, uint32_t cap, uint32_t floorMultPct);

    // Split a listing's total count into category-appropriate stack sizes (§7.2):
    // GEAR/BAG/GEM_CUT singles; AMMO full stacks; FLASK/ELIXIR_POT/FOOD mixed 5s/20s;
    // INTERMEDIATE full stacks + one ragged partial; else full stacks.
    std::vector<uint32_t> SplitStacks(uint32_t count, uint8_t category, uint32_t maxStack,
                                      std::mt19937& rng);

    // Texture a listing (§7): stacks (§7.2) spread across 2-4 undercut price rungs
    // (§7.1), each with per-stack variance, never below `floor`. `basePrice` is the
    // top rung (the CraftPrice, or the in-house undercut). Returns (stackSize, unitPrice).
    std::vector<std::pair<uint32_t, uint64_t>> TexturedListings(
        uint32_t count, uint8_t category, uint32_t maxStack, uint64_t basePrice,
        uint64_t floor, uint32_t variancePct, std::mt19937& rng);
}
