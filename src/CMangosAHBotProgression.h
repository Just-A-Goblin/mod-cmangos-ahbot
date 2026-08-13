#pragma once

#include "CMangosAHBotCommon.h"

// Effective gating caps derived from a progression state (addendum §4).
struct CmAHBCaps
{
    uint8_t  state        = 0;
    uint8_t  maxExpansion = CMAHB_EXP_VANILLA;
    uint32_t itemLevelCap = 66;
    uint32_t reqLevelCap  = 60;  // derived from maxExpansion (60/70/80)
    uint32_t skillCap     = 300; // profession MinSkillLineRank cap for maxExpansion
};

// Progression resolution. Deliberately does NOT link mod-individual-progression;
// it reads IP's persisted state via the one documented query (addendum §2).
namespace CMangosAHBotProgression
{
    // The single point of dependency on IP internals: hidden rewarded quest
    // 66000+state. Verified at IndividualProgression.cpp:128-151 / :132-133.
    // If IP ever changes this encoding, THIS is what breaks.
    uint8_t QueryAccountProgression(uint32_t accountId);

    // Max progression across accounts with a character currently online,
    // excluding the bot account and Progression.ExcludeAccounts.
    uint8_t QueryHighestOnlineProgression();

    // IP's server-wide ceiling (IndividualProgression.ProgressionLimit, 0 = none),
    // read from the shared worldserver config. Clamp the effective state to it.
    uint8_t IPProgressionLimit();

    // Apply Source policy + IP ceiling clamp. Returns the effective state.
    uint8_t GetEffectiveState();

    // Map an effective state to the full set of caps.
    CmAHBCaps CapsForState(uint8_t state);
}
