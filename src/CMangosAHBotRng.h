/*
 * Module-local RNG for the craft layer (crafting addendum §10.1, constraint #4).
 *
 * Determinism contract: with CMangosAHBot.Craft.Seed != 0, EVERY craft-layer
 * random draw (population roll, session sampling, recipe choice, batches,
 * variance, undercut rolls) must come from this one engine. It is entirely
 * separate from the core's global `urand`/`irand` (Random.cpp static engine,
 * verified C-A10) — so seeding here neither reads nor perturbs core determinism,
 * and the world-drop simulator (which uses the core RNG) is unaffected.
 *
 * With Seed == 0 the engine is seeded nondeterministically (std::random_device),
 * so live servers still get variety; only a nonzero seed pins the sequence.
 */
#pragma once

#include <cstdint>
#include <random>

class CMangosAHBotRng
{
public:
    static CMangosAHBotRng* instance()
    {
        static CMangosAHBotRng inst;
        return &inst;
    }

    // Seed == 0 => nondeterministic; nonzero => reproducible.
    void Seed(uint32_t seed)
    {
        _seed = seed;
        if (seed == 0)
        {
            std::random_device rd;
            _engine.seed(rd());
        }
        else
        {
            _engine.seed(seed);
        }
    }

    uint32_t Seeded() const { return _seed; }

    // Inclusive integer range [min, max]; tolerant of max < min (returns min).
    uint32_t Urand(uint32_t min, uint32_t max)
    {
        if (max <= min)
            return min;
        std::uniform_int_distribution<uint32_t> d(min, max);
        return d(_engine);
    }

    int32_t Irand(int32_t min, int32_t max)
    {
        if (max <= min)
            return min;
        std::uniform_int_distribution<int32_t> d(min, max);
        return d(_engine);
    }

    // [0,1) uniform.
    double Rand01()
    {
        std::uniform_real_distribution<double> d(0.0, 1.0);
        return d(_engine);
    }

    // true with probability pct/100.
    bool RollPct(uint32_t pct) { return Urand(0, 99) < pct; }

    std::mt19937& Engine() { return _engine; }

private:
    CMangosAHBotRng() { Seed(0); }
    std::mt19937 _engine;
    uint32_t     _seed = 0;
};

#define sCMangosAHBotRng CMangosAHBotRng::instance()
