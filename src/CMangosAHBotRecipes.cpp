/*
 * CMangosAHBotRecipes — recipe graph build + classification + gating (addendum §2, §5.1).
 *
 * Provenance: the profession spell scan mirrors the base module's BuildProfessionPool
 * (plan §2.3). Recipe->craft-spell resolution and enchant-product gating follow the
 * C-A3 / C-A9 findings in NOTES-verification.md (direct spelltrigger_2==6 encoding;
 * enchanting oils/rods are pre-WotLK, only vellum SCROLLs are expansion 2).
 */
#include "CMangosAHBotRecipes.h"
#include "CMangosAHBotConfig.h"
#include "Common.h"           // DAY, IN_MILLISECONDS
#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>

namespace
{
    constexpr uint32_t NOCAP = std::numeric_limits<uint32_t>::max();
    constexpr uint64_t CD_THRESHOLD_MS = uint64_t(DAY) * IN_MILLISECONDS; // 86,400,000

    // Skill lines whose create-item spells the graph accepts (C-A8) plus Mining(186)
    // for smelting — bars are load-bearing intermediates (the Bronze Bar glut, C3).
    bool IsAcceptedSkill(uint32_t s)
    {
        switch (s)
        {
            case SKILL_ALCHEMY: case SKILL_BLACKSMITHING: case SKILL_LEATHERWORKING:
            case SKILL_TAILORING: case SKILL_ENGINEERING: case SKILL_ENCHANTING:
            case SKILL_JEWELCRAFTING: case SKILL_INSCRIPTION: case SKILL_COOKING:
            case SKILL_FIRST_AID: case SKILL_MINING:
                return true;
            default:
                return false;
        }
    }

    // Categories exempt from the product item-level mask (§2.3 rule 4): governed by
    // skill/recipe-source instead, because their ilvl is junk (bags, consumables).
    bool ExemptFromIlvlMask(ItemCategory c)
    {
        return c == CAT_BAG || c == CAT_FLASK || c == CAT_ELIXIR_POT ||
               c == CAT_FOOD || c == CAT_SCROLL;
    }

    // Base-module listability filter (mirrors PassesFilters minus dynamic overrides).
    bool ProductListable(ItemTemplate const* p, const CMangosAHBotConfig& cfg)
    {
        uint32_t q = p->Quality, cls = p->Class;
        if (q >= CMAHB_MAX_QUALITY || cls >= CMAHB_MAX_CLASS)
            return false;
        if (cfg.valueMatrix[q][cls] == 0)
            return false;
        if (p->Bonding == BIND_WHEN_PICKED_UP || p->Bonding == BIND_QUEST_ITEM)
            return false;
        if (cls == ITEM_CLASS_QUEST)
            return false;
        if (p->Flags & ITEM_FLAG_HAS_LOOT)
            return false;
        return true;
    }
}

const char* RarityName(RecipeRarity r)
{
    switch (r)
    {
        case RARITY_TRAINER:   return "TRAINER";
        case RARITY_VENDOR:    return "VENDOR";
        case RARITY_DROP:      return "DROP";
        case RARITY_UNSOURCED: return "UNSOURCED";
        default:               return "?";
    }
}

const char* CategoryName(ItemCategory c)
{
    switch (c)
    {
        case CAT_FLASK:        return "FLASK";
        case CAT_ELIXIR_POT:   return "ELIXIR_POT";
        case CAT_FOOD:         return "FOOD";
        case CAT_BAG:          return "BAG";
        case CAT_GEM_CUT:      return "GEM_CUT";
        case CAT_SCROLL:       return "SCROLL";
        case CAT_AMMO:         return "AMMO";
        case CAT_GEAR:         return "GEAR";
        case CAT_INTERMEDIATE: return "INTERMEDIATE";
        case CAT_MISC:         return "MISC";
        default:               return "?";
    }
}

RecipeRarity CMangosAHBotRecipeGraph::ClassifyRarity(
    uint32_t spellId, uint32_t recipeItemId,
    const std::unordered_set<uint32_t>& trainerSpells,
    const std::unordered_set<uint32_t>& vendorItems,
    const std::unordered_set<uint32_t>& lootItems) const
{
    // §2.2: trainer beats vendor beats drop; unsourced is treated as drop for margins.
    if (trainerSpells.count(spellId))
        return RARITY_TRAINER;
    if (recipeItemId == 0)
        return RARITY_UNSOURCED;              // taught with no recipe item and not on a trainer
    if (vendorItems.count(recipeItemId))
        return RARITY_VENDOR;
    if (lootItems.count(recipeItemId))
        return RARITY_DROP;
    return RARITY_UNSOURCED;
}

void CMangosAHBotRecipeGraph::Build(const CMangosAHBotConfig& cfg,
                                    const std::unordered_set<uint32_t>& vendorItems)
{
    auto t0 = std::chrono::steady_clock::now();

    _recipes.clear();
    _producers.clear();
    _profRarity.clear();
    _categoryCounts.fill(0);
    _cooldownCount = _skippedNoReagent = _skippedProduct = 0;

    // ---- trainer-taught craft spells (C-A4): current table + legacy fallback ----
    std::unordered_set<uint32_t> trainerSpells;
    if (QueryResult r = WorldDatabase.Query("SELECT DISTINCT SpellID FROM trainer_spell WHERE SpellID > 0"))
        do { trainerSpells.insert(r->Fetch()[0].Get<uint32_t>()); } while (r->NextRow());
    if (QueryResult r = WorldDatabase.Query("SELECT DISTINCT SpellID FROM npc_trainer WHERE SpellID > 0"))
        do { trainerSpells.insert(r->Fetch()[0].Get<uint32_t>()); } while (r->NextRow());

    // ---- recipe item -> craft spell (C-A3 direct encoding: class=9, spelltrigger_2=6) ----
    std::unordered_map<uint32_t, uint32_t> spellToRecipeItem; // craftSpell -> recipe itemId
    if (QueryResult r = WorldDatabase.Query(
            "SELECT spellid_2, entry FROM item_template "
            "WHERE class = 9 AND spelltrigger_2 = 6 AND spellid_2 > 0"))
    {
        do
        {
            Field* f = r->Fetch();
            spellToRecipeItem.emplace(f[0].Get<uint32_t>(), f[1].Get<uint32_t>());
        } while (r->NextRow());
    }

    // ---- items that appear in ANY loot template => DROP rarity membership (C-A6 tables) ----
    std::unordered_set<uint32_t> lootItems;
    static const char* kLootTables[] = {
        "creature_loot_template", "gameobject_loot_template", "item_loot_template",
        "disenchant_loot_template", "fishing_loot_template", "skinning_loot_template",
        "pickpocketing_loot_template", "reference_loot_template", "mail_loot_template",
        "milling_loot_template", "prospecting_loot_template", "spell_loot_template"
    };
    for (const char* tbl : kLootTables)
    {
        if (QueryResult r = WorldDatabase.Query("SELECT DISTINCT item FROM {} WHERE item > 0", tbl))
            do { lootItems.insert(r->Fetch()[0].Get<uint32_t>()); } while (r->NextRow());
    }

    // ---- scan create-item profession spells (plan §2.3 filter, C-A1 reagents) ----
    std::unordered_set<uint32_t> reagentUniverse; // itemIds consumed by >=1 recipe (§5.1 INTERMEDIATE)
    uint32_t storeSize = sSpellMgr->GetSpellInfoStoreSize();
    for (uint32_t id = 0; id < storeSize; ++id)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
        if (!spell)
            continue;
        if (!(spell->Attributes & 0x20) || !(spell->Attributes & 0x10000))
            continue;

        // Resolve skill line + trivial ranks; require an accepted profession skill line.
        uint32_t skillLine = 0, minRank = 0, grey = 0, yellow = 0;
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(id);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            if (IsAcceptedSkill(it->second->SkillLine))
            {
                skillLine = it->second->SkillLine;
                minRank   = it->second->MinSkillLineRank;
                grey      = it->second->TrivialSkillLineRankHigh;
                yellow    = it->second->TrivialSkillLineRankLow;
                break;
            }
        }
        if (!skillLine)
            continue;

        for (uint8_t e = 0; e < MAX_SPELL_EFFECTS; ++e)
        {
            if (spell->Effects[e].Effect != SPELL_EFFECT_CREATE_ITEM || !spell->Effects[e].ItemType)
                continue;

            uint32_t productItem = spell->Effects[e].ItemType;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(productItem);
            if (!proto)
                continue;

            CraftRecipe rec;
            rec.spellId      = id;
            rec.skillLine    = skillLine;
            rec.productItem  = productItem;
            rec.productCount = std::max(1, spell->Effects[e].BasePoints + 1);
            rec.minSkill     = static_cast<uint16_t>(minRank);
            rec.greySkill    = static_cast<uint16_t>(grey);
            rec.yellowSkill  = static_cast<uint16_t>(yellow);
            rec.productIlvl     = proto->ItemLevel;
            rec.productReqLevel = proto->RequiredLevel;

            // Reagents (C-A1): int32 Reagent[8] / uint32 ReagentCount[8].
            for (uint8_t ri = 0; ri < MAX_SPELL_REAGENTS; ++ri)
            {
                int32 reagentId = spell->Reagent[ri];
                if (reagentId > 0 && spell->ReagentCount[ri] > 0)
                {
                    rec.reagents.emplace_back(uint32_t(reagentId), spell->ReagentCount[ri]);
                    reagentUniverse.insert(uint32_t(reagentId));
                }
            }
            if (rec.reagents.empty()) { ++_skippedNoReagent; break; } // conjure, not a mat sink
            if (!ProductListable(proto, cfg)) { ++_skippedProduct; break; }

            uint32_t recipeItemId = 0;
            if (auto it = spellToRecipeItem.find(id); it != spellToRecipeItem.end())
                recipeItemId = it->second;
            rec.rarity = ClassifyRarity(id, recipeItemId, trainerSpells, vendorItems, lootItems);

            // Cooldown (C-A5): shared category CD for transmutes => use the max.
            rec.dailyCooldown =
                std::max<uint64_t>(spell->RecoveryTime, spell->CategoryRecoveryTime) >= CD_THRESHOLD_MS;

            _producers[productItem].push_back(uint32_t(_recipes.size()));
            _recipes.push_back(std::move(rec));
            break; // one product per create-item spell
        }
    }

    // ---- second pass: category (§5.1) + bake expansion (§2.3 rules 1+2) ----
    const uint32_t* skillCaps = cfg.progSkillCaps; // {vanilla, tbc, wotlk} rank ceilings
    for (CraftRecipe& rec : _recipes)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rec.productItem);
        ItemCategory cat = CAT_MISC;
        if (proto->Class == ITEM_CLASS_CONTAINER)
            cat = CAT_BAG;
        else if (proto->Class == ITEM_CLASS_GEM && rec.skillLine == SKILL_JEWELCRAFTING)
            cat = CAT_GEM_CUT;
        else if (proto->Class == ITEM_CLASS_PROJECTILE)
            cat = CAT_AMMO;
        else if (proto->Class == ITEM_CLASS_CONSUMABLE)
        {
            switch (proto->SubClass)
            {
                case ITEM_SUBCLASS_FLASK:                        cat = CAT_FLASK; break;
                case ITEM_SUBCLASS_POTION:
                case ITEM_SUBCLASS_ELIXIR:                       cat = CAT_ELIXIR_POT; break;
                case ITEM_SUBCLASS_FOOD:                         cat = CAT_FOOD; break;
                default:                                         cat = CAT_MISC; break;
            }
        }
        else if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
            cat = CAT_GEAR;
        else if (proto->Class == ITEM_CLASS_TRADE_GOODS && reagentUniverse.count(rec.productItem))
            cat = CAT_INTERMEDIATE;

        // SCROLL (§5.1, C-A9): an Enchanting recipe that consumes a vellum reagent.
        if (rec.skillLine == SKILL_ENCHANTING)
        {
            for (auto& [rid, rc] : rec.reagents)
            {
                ItemTemplate const* rp = sObjectMgr->GetItemTemplate(rid);
                if (rp && (rp->IsWeaponVellum() || rp->IsArmorVellum())) { cat = CAT_SCROLL; break; }
            }
        }
        rec.category = cat;

        // Bake expansion: max(skill-line era, skill-rank era, scroll=WotLK).
        uint8_t exp = CMAHB_EXP_VANILLA;
        if (rec.skillLine == SKILL_JEWELCRAFTING) exp = std::max(exp, CMAHB_EXP_TBC);
        if (rec.skillLine == SKILL_INSCRIPTION)   exp = std::max(exp, CMAHB_EXP_WOTLK);
        if (rec.minSkill > skillCaps[1])          exp = std::max(exp, CMAHB_EXP_WOTLK);
        else if (rec.minSkill > skillCaps[0])     exp = std::max(exp, CMAHB_EXP_TBC);
        if (cat == CAT_SCROLL)                    exp = std::max(exp, CMAHB_EXP_WOTLK);
        rec.expansion = exp;

        _categoryCounts[cat]++;
        if (rec.dailyCooldown) ++_cooldownCount;
        _profRarity[rec.skillLine][rec.rarity]++;
    }

    _buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    _built = true;

    LOG_INFO("module", "CMangosAHBot[craft]: recipe graph built — {} recipes in {:.2f}s "
             "(skipped: noReagent={} product={}) cooldownRecipes={}",
             _recipes.size(), _buildSeconds, _skippedNoReagent, _skippedProduct, _cooldownCount);
    SendGraphReport();
}

void CMangosAHBotRecipeGraph::SendGraphReport() const
{
    std::istringstream ss(StartupReport());
    std::string line;
    while (std::getline(ss, line))
        LOG_INFO("module", "{}", line);
}

void CMangosAHBotRecipeGraph::RecomputeMask(const CmAHBCaps& caps, uint32_t /*tbcAtState*/,
                                            uint32_t /*wotlkAtState*/, const uint32_t /*skillCaps*/[3])
{
    for (CraftRecipe& rec : _recipes)
    {
        bool ok = rec.expansion <= caps.maxExpansion;                       // rules 1 + 3(baked) + scroll
        if (ok && caps.skillCap != NOCAP)  ok = rec.minSkill <= caps.skillCap;      // rule 2
        if (ok && !ExemptFromIlvlMask(rec.category))                        // rule 4 (exempt bags/consumables)
        {
            if (caps.itemLevelCap != NOCAP) ok = rec.productIlvl <= caps.itemLevelCap;
            if (ok && caps.reqLevelCap != NOCAP) ok = rec.productReqLevel <= caps.reqLevelCap;
        }
        rec.available = ok;
    }
}

const std::vector<uint32_t>* CMangosAHBotRecipeGraph::Producers(uint32_t itemId) const
{
    auto it = _producers.find(itemId);
    return it == _producers.end() ? nullptr : &it->second;
}

uint32_t CMangosAHBotRecipeGraph::AvailableCount() const
{
    uint32_t n = 0;
    for (auto& r : _recipes) if (r.available) ++n;
    return n;
}

uint32_t CMangosAHBotRecipeGraph::CategoryCount(ItemCategory c, bool availableOnly) const
{
    uint32_t n = 0;
    for (auto& r : _recipes) if (r.category == c && (!availableOnly || r.available)) ++n;
    return n;
}

uint32_t CMangosAHBotRecipeGraph::SkillLineAvailableCount(uint32_t skillLine) const
{
    uint32_t n = 0;
    for (auto& r : _recipes) if (r.skillLine == skillLine && r.available) ++n;
    return n;
}

std::string CMangosAHBotRecipeGraph::StartupReport() const
{
    static const std::pair<uint32_t, const char*> kProf[] = {
        { SKILL_ALCHEMY, "Alchemy" }, { SKILL_BLACKSMITHING, "Blacksmithing" },
        { SKILL_LEATHERWORKING, "Leatherworking" }, { SKILL_TAILORING, "Tailoring" },
        { SKILL_ENGINEERING, "Engineering" }, { SKILL_ENCHANTING, "Enchanting" },
        { SKILL_JEWELCRAFTING, "Jewelcrafting" }, { SKILL_INSCRIPTION, "Inscription" },
        { SKILL_COOKING, "Cooking" }, { SKILL_FIRST_AID, "First Aid" },
        { SKILL_MINING, "Mining(smelt)" }
    };

    std::ostringstream ss;
    ss << "CMangosAHBot[craft]: per-profession x rarity (TRAINER/VENDOR/DROP/UNSOURCED):";
    for (auto& [sl, name] : kProf)
    {
        auto it = _profRarity.find(sl);
        if (it == _profRarity.end())
            continue;
        const auto& a = it->second;
        ss << "\n  " << name << ": "
           << a[RARITY_TRAINER] << "/" << a[RARITY_VENDOR] << "/"
           << a[RARITY_DROP] << "/" << a[RARITY_UNSOURCED]
           << " (total " << (a[0] + a[1] + a[2] + a[3]) << ")";
    }
    ss << "\nCMangosAHBot[craft]: categories:";
    for (uint8_t c = 0; c < CAT_COUNT; ++c)
        ss << " " << CategoryName(ItemCategory(c)) << "=" << _categoryCounts[c];
    return ss.str();
}

std::string CMangosAHBotRecipeGraph::StatusReport(const CmAHBCaps& caps) const
{
    std::ostringstream ss;
    ss << "Craft graph: recipes=" << _recipes.size()
       << " available=" << AvailableCount()
       << " (state=" << int(caps.state) << " maxExp=" << int(caps.maxExpansion)
       << " skillCap=" << (caps.skillCap == NOCAP ? 0 : caps.skillCap)
       << " ilvlCap=" << (caps.itemLevelCap == NOCAP ? 0 : caps.itemLevelCap) << ")"
       << " cooldownRecipes=" << _cooldownCount
       << " buildTime=" << _buildSeconds << "s"
       << "\n  avail by cat:";
    for (uint8_t c = 0; c < CAT_COUNT; ++c)
        ss << " " << CategoryName(ItemCategory(c)) << "=" << CategoryCount(ItemCategory(c), true);
    ss << "\n  avail JC=" << SkillLineAvailableCount(SKILL_JEWELCRAFTING)
       << " Inscription=" << SkillLineAvailableCount(SKILL_INSCRIPTION);
    return ss.str();
}

bool CMangosAHBotRecipeGraph::SelfTest(const CmAHBCaps& caps, uint32_t /*tbcAtState*/,
                                       uint32_t /*wotlkAtState*/, const uint32_t /*skillCaps*/[3],
                                       std::string& detail) const
{
    if (!_built || _recipes.empty())
    {
        detail = "graph empty/not built";
        return false;
    }
    // Every available recipe must satisfy the current gate (mask consistency, §10.3).
    for (auto& r : _recipes)
    {
        if (!r.available)
            continue;
        if (r.expansion > caps.maxExpansion)
        {
            detail = "available recipe spell " + std::to_string(r.spellId) +
                     " exceeds expansion gate";
            return false;
        }
        if (caps.skillCap != NOCAP && r.minSkill > caps.skillCap)
        {
            detail = "available recipe spell " + std::to_string(r.spellId) +
                     " exceeds skill cap";
            return false;
        }
        if (!ExemptFromIlvlMask(r.category) && caps.itemLevelCap != NOCAP &&
            r.productIlvl > caps.itemLevelCap)
        {
            detail = "available recipe spell " + std::to_string(r.spellId) +
                     " product ilvl over cap";
            return false;
        }
    }
    // MISC must be a minority of the graph (§5.1: classifier sanity).
    uint32_t misc = _categoryCounts[CAT_MISC];
    if (misc * 2 > _recipes.size())
    {
        detail = "MISC dominates (" + std::to_string(misc) + "/" +
                 std::to_string(_recipes.size()) + ")";
        return false;
    }
    detail = "recipes=" + std::to_string(_recipes.size()) +
             " available=" + std::to_string(AvailableCount()) +
             " misc=" + std::to_string(misc);
    return true;
}
