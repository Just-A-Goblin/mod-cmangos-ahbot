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
}
