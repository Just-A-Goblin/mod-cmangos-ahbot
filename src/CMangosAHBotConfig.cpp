#include "CMangosAHBotConfig.h"
#include "Config.h"
#include "Log.h"
#include <sstream>

CMangosAHBotConfig gCMangosAHBotConfig;

CmAHBSourceConfig CMangosAHBotConfig::ParseTuple(const std::string& key, const char* def)
{
    std::string raw = sConfigMgr->GetOption<std::string>(key, def);
    std::istringstream ss(raw);
    std::string tok;
    int32_t v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4 && std::getline(ss, tok, ','); ++i)
    {
        try { v[i] = std::stoi(tok); } catch (...) { v[i] = 0; }
    }
    CmAHBSourceConfig c;
    c.minSources  = v[0];
    c.maxSources  = v[1];
    c.minLootings = static_cast<uint32_t>(std::max(0, v[2]));
    c.maxLootings = static_cast<uint32_t>(std::max(0, v[3]));
    return c;
}

void CMangosAHBotConfig::ParseValueRow(const std::string& key, uint32_t out[CMAHB_MAX_CLASS], const char* def)
{
    std::string raw = sConfigMgr->GetOption<std::string>(key, def);
    ParseUIntList(raw, out, CMAHB_MAX_CLASS);
}

void CMangosAHBotConfig::ParseUIntList(const std::string& raw, uint32_t* out, size_t n)
{
    std::istringstream ss(raw);
    std::string tok;
    for (size_t i = 0; i < n && std::getline(ss, tok, ','); ++i)
    {
        try { out[i] = static_cast<uint32_t>(std::stoul(tok)); } catch (...) { out[i] = 0; }
    }
}

void CMangosAHBotConfig::ParseUIntPair(const std::string& key, const char* def, uint32_t& a, uint32_t& b)
{
    std::string raw = sConfigMgr->GetOption<std::string>(key, def);
    std::istringstream ss(raw);
    std::string tok;
    uint32_t v[2] = { a, b };
    for (int i = 0; i < 2 && std::getline(ss, tok, ','); ++i)
    {
        try { v[i] = static_cast<uint32_t>(std::stoul(tok)); } catch (...) {}
    }
    a = v[0];
    b = std::max(v[0], v[1]);
}

std::vector<uint32_t> CMangosAHBotConfig::ExcludedAccountList() const
{
    std::vector<uint32_t> ids;
    std::istringstream ss(progExcludeAccounts);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        // trim
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a == std::string::npos)
            continue;
        try { ids.push_back(static_cast<uint32_t>(std::stoul(tok.substr(a, b - a + 1)))); }
        catch (...) {}
    }
    return ids;
}

void CMangosAHBotConfig::Load()
{
    enable  = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Enable",  0);
    account = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Account", 0);
    guid    = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.GUID",    0);

    tickCompensation = std::max(1u, sConfigMgr->GetOption<uint32_t>("CMangosAHBot.TickCompensation", 3));

    chanceSell = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Chance.Sell", 10);
    chanceBuy  = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Chance.Buy",  10);

    creature[0] = ParseTuple("CMangosAHBot.Loot.Creature.Normal",    "30,35,8,12");
    creature[1] = ParseTuple("CMangosAHBot.Loot.Creature.Rare",      "0,10,1,1");
    creature[2] = ParseTuple("CMangosAHBot.Loot.Creature.Elite",     "30,34,1,2");
    creature[3] = ParseTuple("CMangosAHBot.Loot.Creature.RareElite", "-10,2,1,1");
    creature[4] = ParseTuple("CMangosAHBot.Loot.Creature.WorldBoss", "-20,1,1,1");
    disenchant  = ParseTuple("CMangosAHBot.Loot.Disenchant",         "10,12,1,1");
    fishing     = ParseTuple("CMangosAHBot.Loot.Fishing",            "3,5,30,40");
    gameobject  = ParseTuple("CMangosAHBot.Loot.Gameobject",         "13,16,7,11");
    skinning    = ParseTuple("CMangosAHBot.Loot.Skinning",           "3,5,50,50");

    {
        std::string raw = sConfigMgr->GetOption<std::string>("CMangosAHBot.Items.Profession", "80,90,0,50");
        std::istringstream ss(raw);
        std::string tok;
        int32_t v[4] = { 80, 90, 0, 50 };
        for (int i = 0; i < 4 && std::getline(ss, tok, ','); ++i)
        {
            try { v[i] = std::stoi(tok); } catch (...) {}
        }
        for (int i = 0; i < 4; ++i)
            professionTuple[i] = v[i];
    }

    // Value matrix — 17 per-class percentages per quality row.
    // Defaults are reasonable approximations; copy the exact numbers from
    // CMaNGOS ahbot.conf.dist.in for a faithful match (plan §5).
    ParseValueRow("CMangosAHBot.Value.Poor",      valueMatrix[0], "0,0,0,0,0,0,0,100,0,0,0,0,0,0,0,100,0");
    ParseValueRow("CMangosAHBot.Value.Normal",    valueMatrix[1], "150,150,150,150,150,150,150,150,0,150,0,150,0,0,0,150,150");
    ParseValueRow("CMangosAHBot.Value.Uncommon",  valueMatrix[2], "200,200,200,200,200,200,200,200,0,200,0,200,0,0,0,200,200");
    ParseValueRow("CMangosAHBot.Value.Rare",      valueMatrix[3], "300,300,300,300,300,300,300,300,0,300,0,300,0,0,0,300,300");
    ParseValueRow("CMangosAHBot.Value.Epic",      valueMatrix[4], "400,400,400,400,400,400,400,400,0,400,0,400,0,0,0,400,400");
    ParseValueRow("CMangosAHBot.Value.Legendary", valueMatrix[5], "500,500,500,500,500,500,500,500,0,500,0,500,0,0,0,500,500");
    ParseValueRow("CMangosAHBot.Value.Artifact",  valueMatrix[6], "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");

    valueVendor   = sConfigMgr->GetOption<bool>    ("CMangosAHBot.Value.Vendor",   true);
    valueVariance = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Value.Variance", 10);

    bidMin   = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Bid.Min",   75);
    bidMax   = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Bid.Max",   90);
    buyValue = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Buy.Value", 90);
    timeMin  = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Time.Min",  2);
    timeMax  = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Time.Max",  24);

    hardCap  = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.HardCap", 0);

    // -------- Progression --------
    progEnable    = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Progression.Enable", 1);
    progSource    = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Progression.Source", CMAHB_PROG_ACCOUNT);
    progAccountId = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Progression.AccountId", 0);
    progStatic    = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Progression.Static", 0);
    progExcludeAccounts = sConfigMgr->GetOption<std::string>("CMangosAHBot.Progression.ExcludeAccounts", "");
    progRefreshInterval = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Progression.RefreshInterval", 300);
    progTbcAtState   = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Progression.TbcAtState", 8);
    progWotlkAtState = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Progression.WotlkAtState", 13);
    progExcludeUnspawned = sConfigMgr->GetOption<bool>("CMangosAHBot.Progression.ExcludeUnspawned", true);

    {
        std::string raw = sConfigMgr->GetOption<std::string>("CMangosAHBot.Progression.ItemLevelCaps",
            "66,71,71,76,76,81,81,92,115,133,141,141,154,200,226,245,264,277,284");
        ParseUIntList(raw, progItemLevelCaps, 19);
    }
    {
        std::string raw = sConfigMgr->GetOption<std::string>("CMangosAHBot.Progression.SkillCaps", "300,375,450");
        ParseUIntList(raw, progSkillCaps, 3);
    }

    // -------- Craft layer (crafting addendum §8.1) --------
    craftEnable     = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.Enable", 0);
    craftSeed       = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.Seed", 0);
    craftPopulation = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.Population", 120);
    ParseUIntPair("CMangosAHBot.Craft.Sessions", "4,8", craftSessionsMin, craftSessionsMax);
    craftChance     = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.Chance", 100);
    craftLevelingShare = sConfigMgr->GetOption<int32_t>("CMangosAHBot.Craft.LevelingShare", -1);
    // Default adds Mining(186) to the addendum §8.1 list so smelters exist and bars
    // are a glut (the C3 acceptance names "smelted bars"; the spec's list omitted it).
    craftProfessionWeights = sConfigMgr->GetOption<std::string>("CMangosAHBot.Craft.ProfessionWeights",
        "171:20,164:12,165:12,197:15,202:10,333:10,755:10,773:6,185:3,129:2,186:8");
    craftSkillDist  = sConfigMgr->GetOption<std::string>("CMangosAHBot.Craft.SkillDist", "");
    ParseUIntPair("CMangosAHBot.Craft.Batch.Leveling", "5,15", craftBatchLevelingMin, craftBatchLevelingMax);
    craftGearWindow = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.GearWindow", 26);

    ParseUIntPair("CMangosAHBot.Craft.Margin.Leveling", "70,95",   craftMarginLevelingMin, craftMarginLevelingMax);
    ParseUIntPair("CMangosAHBot.Craft.Margin.Trainer",  "100,125", craftMarginTrainerMin,  craftMarginTrainerMax);
    ParseUIntPair("CMangosAHBot.Craft.Margin.Vendor",   "110,140", craftMarginVendorMin,   craftMarginVendorMax);
    ParseUIntPair("CMangosAHBot.Craft.Margin.Drop",     "150,300", craftMarginDropMin,     craftMarginDropMax);
    craftMarginCooldownBonus = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.Margin.CooldownBonus", 150);
    craftCooldownPerCrafter  = sConfigMgr->GetOption<float>("CMangosAHBot.Craft.CooldownPerCrafter", 1.0f);

    craftAnchorClampMin = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.AnchorClampMin", 50);
    craftAnchorClampMax = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.AnchorClampMax", 300);

    craftBuyDailyCap    = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.Buy.DailyCap", 20);
    craftBuyFloorMult   = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.Buy.FloorMult", 30);
    craftLedgerWindowHours = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.Ledger.WindowHours", 24);

    craftDemandVanilla = sConfigMgr->GetOption<std::string>("CMangosAHBot.Craft.Demand.Vanilla",
        "FLASK:20,ELIXIR_POT:30,FOOD:25,BAG:10,AMMO:25,GEAR:15,INTERMEDIATE:8,MISC:1");
    craftDemandTbc     = sConfigMgr->GetOption<std::string>("CMangosAHBot.Craft.Demand.Tbc",
        "FLASK:35,ELIXIR_POT:25,FOOD:25,GEM_CUT:30,BAG:10,GEAR:20,INTERMEDIATE:8,MISC:1");
    craftDemandWotlk   = sConfigMgr->GetOption<std::string>("CMangosAHBot.Craft.Demand.Wotlk",
        "FLASK:40,ELIXIR_POT:15,FOOD:30,GEM_CUT:35,SCROLL:20,BAG:12,GEAR:20,INTERMEDIATE:8,MISC:1");
    craftDemandStateBoost = sConfigMgr->GetOption<std::string>("CMangosAHBot.Craft.Demand.StateBoost", "");

    craftTestCommands = sConfigMgr->GetOption<uint32_t>("CMangosAHBot.Craft.TestCommands", 0);
    craftDumpFile     = sConfigMgr->GetOption<std::string>("CMangosAHBot.Craft.DumpFile", "");

    LOG_INFO("module", "CMangosAHBot: config loaded (enable={} account={} guid={} progression={}/src{} tickComp={} craft={}/seed{})",
        enable, account, guid, progEnable, progSource, tickCompensation, craftEnable, craftSeed);
}
