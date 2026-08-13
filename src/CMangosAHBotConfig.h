#pragma once

#include "CMangosAHBotCommon.h"
#include <string>

// ---------------------------------------------------------------------------
// Progression source policy (addendum §3).
// ---------------------------------------------------------------------------
enum CmAHBProgressionSource : uint32_t
{
    CMAHB_PROG_STATIC        = 0, // use Progression.Static verbatim
    CMAHB_PROG_ACCOUNT       = 1, // max progression across chars on Progression.AccountId (default)
    CMAHB_PROG_HIGHEST_ONLINE = 2 // max across online chars, excluding bot/excluded accounts
};

// Parsed configuration. Loaded at OnStartup and OnAfterConfigLoad.
class CMangosAHBotConfig
{
public:
    void Load();

    // Identity
    uint32_t enable  = 0;
    uint32_t account = 0;
    uint32_t guid    = 0;

    // Cadence — internal 20s steps per 60s core hook (plan §2.4)
    uint32_t tickCompensation = 3;

    // Flow gates (percent, 0..100)
    uint32_t chanceSell = 10;
    uint32_t chanceBuy  = 10;

    // Ten loot config tuples (CMaNGOS order)
    CmAHBSourceConfig creature[CMAHB_CREATURE_RANKS]; // Normal,Rare,Elite,RareElite,WorldBoss
    CmAHBSourceConfig disenchant;
    CmAHBSourceConfig fishing;
    CmAHBSourceConfig gameobject;
    CmAHBSourceConfig skinning;
    // Profession tuple: (countMin, countMax, stackPctMin, stackPctMax)
    int32_t  professionTuple[4] = { 80, 90, 0, 50 };

    // Value matrix [quality][class]; 0 in a cell => that (quality,class) is skipped.
    uint32_t valueMatrix[CMAHB_MAX_QUALITY][CMAHB_MAX_CLASS] = {};
    bool     valueVendor   = true;  // vendor-sold items priced at 100% of computed base
    uint32_t valueVariance = 10;    // +/- percent applied to every price

    // Bid / duration / buyer valuation
    uint32_t bidMin  = 75;  // percent of buyout
    uint32_t bidMax  = 90;
    uint32_t buyValue = 90; // buyer values an ask up to buyout * count * buyValue/100
    uint32_t timeMin = 2;   // hours
    uint32_t timeMax = 24;

    // Runaway guard only — NOT a setpoint (plan §7). 0 = unlimited.
    uint32_t hardCap = 0;

    // -------- Progression (addendum §6) --------
    uint32_t progEnable   = 1;
    uint32_t progSource   = CMAHB_PROG_ACCOUNT;
    uint32_t progAccountId = 0;         // PLAYER account, NOT the bot account
    uint32_t progStatic   = 0;
    std::string progExcludeAccounts;    // comma-separated (Source=2 with playerbots)
    uint32_t progRefreshInterval = 300; // seconds
    uint32_t progTbcAtState   = 8;      // expansion unlock thresholds (IP ProgressionState)
    uint32_t progWotlkAtState = 13;
    // Per-state item-level caps, index = ProgressionState 0..18 (19 entries).
    uint32_t progItemLevelCaps[19] = {66,71,71,76,76,81,81,92,115,133,141,141,154,200,226,245,264,277,284};
    uint32_t progSkillCaps[3] = { 300, 375, 450 }; // per expansion 0/1/2
    bool     progExcludeUnspawned = true;

    // Helpers
    std::vector<uint32_t> ExcludedAccountList() const;

private:
    static CmAHBSourceConfig ParseTuple(const std::string& key, const char* def);
    static void ParseValueRow(const std::string& key, uint32_t out[CMAHB_MAX_CLASS], const char* def);
    static void ParseUIntList(const std::string& raw, uint32_t* out, size_t n);
};

extern CMangosAHBotConfig gCMangosAHBotConfig;
