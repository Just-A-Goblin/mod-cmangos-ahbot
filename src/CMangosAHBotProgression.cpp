#include "CMangosAHBotProgression.h"
#include "CMangosAHBotConfig.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"
#include "Log.h"
#include <algorithm>

namespace
{
    // IP quest-encoding bounds, verified at IndividualProgression.cpp:132-133
    //   minQuest = 66000 + PROGRESSION_MOLTEN_CORE   (66001)
    //   maxQuest = 66000 + PROGRESSION_WOTLK_TIER_5  (66018)
    constexpr uint32_t IP_QUEST_MIN = 66001;
    constexpr uint32_t IP_QUEST_MAX = 66018;
    constexpr uint32_t IP_QUEST_BASE = 66000;
}

uint8_t CMangosAHBotProgression::QueryAccountProgression(uint32_t accountId)
{
    if (!accountId)
        return 0;

    // Ported verbatim from IndividualProgression::GetAccountProgression
    // (IndividualProgression.cpp:136-138). Single point of dependency on IP.
    QueryResult result = CharacterDatabase.Query(
        "SELECT cc.quest FROM character_queststatus_rewarded cc "
        "JOIN characters c ON cc.guid = c.guid "
        "WHERE c.account = {} AND cc.quest BETWEEN {} AND {}",
        accountId, IP_QUEST_MIN, IP_QUEST_MAX);

    uint8_t level = 0;
    if (result)
    {
        do
        {
            uint32_t questId = (*result)[0].Get<uint32_t>();
            uint8_t s = static_cast<uint8_t>(questId - IP_QUEST_BASE);
            if (s > level)
                level = s;
        } while (result->NextRow());
    }
    return level;
}

uint8_t CMangosAHBotProgression::QueryHighestOnlineProgression()
{
    const auto& cfg = gCMangosAHBotConfig;

    std::vector<uint32_t> excluded = cfg.ExcludedAccountList();
    excluded.push_back(cfg.account); // never sample the bot account

    QueryResult result = CharacterDatabase.Query(
        "SELECT DISTINCT account FROM characters WHERE online = 1");
    if (!result)
        return 0;

    uint8_t best = 0;
    do
    {
        uint32_t acc = (*result)[0].Get<uint32_t>();
        if (std::find(excluded.begin(), excluded.end(), acc) != excluded.end())
            continue;
        best = std::max<uint8_t>(best, QueryAccountProgression(acc));
    } while (result->NextRow());
    return best;
}

uint8_t CMangosAHBotProgression::IPProgressionLimit()
{
    // Read IP's own ceiling from the shared config. IP default is 0 (none).
    return static_cast<uint8_t>(sConfigMgr->GetOption<uint32_t>("IndividualProgression.ProgressionLimit", 0));
}

uint8_t CMangosAHBotProgression::GetEffectiveState()
{
    const auto& cfg = gCMangosAHBotConfig;

    uint8_t state;
    switch (cfg.progSource)
    {
        case CMAHB_PROG_STATIC:
            state = static_cast<uint8_t>(cfg.progStatic);
            break;
        case CMAHB_PROG_HIGHEST_ONLINE:
            state = QueryHighestOnlineProgression();
            break;
        case CMAHB_PROG_ACCOUNT:
        default:
            state = QueryAccountProgression(cfg.progAccountId);
            break;
    }

    // Clamp to IP's server-wide ceiling if set.
    if (uint8_t limit = IPProgressionLimit())
        state = std::min(state, limit);

    return state;
}

CmAHBCaps CMangosAHBotProgression::CapsForState(uint8_t state)
{
    const auto& cfg = gCMangosAHBotConfig;
    CmAHBCaps caps;
    caps.state = state;

    // Expansion gate (addendum §4 Layer 1). Thresholds are config-driven.
    if (state >= cfg.progWotlkAtState)
        caps.maxExpansion = CMAHB_EXP_WOTLK;
    else if (state >= cfg.progTbcAtState)
        caps.maxExpansion = CMAHB_EXP_TBC;
    else
        caps.maxExpansion = CMAHB_EXP_VANILLA;

    // Item level cap (addendum §4 Layer 2a), index = state clamped to 0..18.
    uint32_t idx = std::min<uint32_t>(state, 18);
    caps.itemLevelCap = cfg.progItemLevelCaps[idx];

    // Required-level ceiling derived from expansion.
    caps.reqLevelCap = (caps.maxExpansion == CMAHB_EXP_WOTLK) ? 80
                     : (caps.maxExpansion == CMAHB_EXP_TBC)   ? 70
                     : 60;

    // Profession skill cap for the expansion (addendum §4 Layer 2b).
    caps.skillCap = cfg.progSkillCaps[std::min<uint32_t>(caps.maxExpansion, 2)];

    return caps;
}
