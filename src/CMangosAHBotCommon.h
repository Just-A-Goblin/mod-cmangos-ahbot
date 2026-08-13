/*
 * mod-cmangos-ahbot — a loot-simulation auction house bot for AzerothCore.
 *
 * Port of the CMaNGOS AuctionHouseBot *mechanism* (flow simulation), NOT its
 * output. See mod-cmangos-ahbot-plan.md §0.
 *
 * Provenance: mechanism and pricing formulas derive from
 * cmangos/mangos-classic src/game/AuctionHouseBot/ (GPL-2.0). This module is
 * AGPL-3.0. Ported files are attributed in-header per the plan §5 licence note.
 */
#pragma once

#include <cstdint>
#include <map>
#include <vector>

// ---------------------------------------------------------------------------
// Auction house identity (verified against the running core reference module)
// House indices: 0 = Alliance, 1 = Horde, 2 = Neutral.
// ---------------------------------------------------------------------------
static constexpr uint32_t CMAHB_HOUSE_ALLIANCE = 0;
static constexpr uint32_t CMAHB_HOUSE_HORDE    = 1;
static constexpr uint32_t CMAHB_HOUSE_NEUTRAL  = 2;
static constexpr uint32_t CMAHB_HOUSE_COUNT    = 3;

// houseId as stored in the `auctionhouse` table
static constexpr uint32_t CMAHB_AH_IDS[CMAHB_HOUSE_COUNT]  = { 2,  6,  7   };
// faction template ids for sAuctionMgr->GetAuctionsMap / GetAuctionHouseEntryFromFactionTemplate
static constexpr uint32_t CMAHB_AH_FIDS[CMAHB_HOUSE_COUNT] = { 55, 29, 120 };

// ---------------------------------------------------------------------------
// Item taxonomy sizes (WotLK)
//   qualities 0..6  : Poor, Normal, Uncommon, Rare, Epic, Legendary, Artifact
//   classes   0..16 : Consumable .. Glyph
// ---------------------------------------------------------------------------
static constexpr uint32_t CMAHB_MAX_QUALITY = 7;
static constexpr uint32_t CMAHB_MAX_CLASS   = 17;

// ---------------------------------------------------------------------------
// Creature rank buckets (both cores agree on these rank values)
//   0 Normal, 1 Elite, 2 RareElite, 3 WorldBoss, 4 Rare
// Index order here mirrors the CMaNGOS config tuple order.
// ---------------------------------------------------------------------------
static constexpr uint32_t CMAHB_CREATURE_RANKS = 5;

// ---------------------------------------------------------------------------
// Loot source config tuple (CMaNGOS four-int semantics).
//   minSources..maxSources : how many distinct loot sources to sample this pass
//                            (NEGATIVE minSources is legal: "usually nothing")
//   minLootings..maxLootings : how many times to loot each sampled source
// See plan §4.1 / §5. Profession reuses this struct with a different meaning
// (see CMangosAHBotConfig::professionTuple).
// ---------------------------------------------------------------------------
struct CmAHBSourceConfig
{
    int32_t  minSources  = 0;
    int32_t  maxSources  = 0;
    uint32_t minLootings = 0;
    uint32_t maxLootings = 0;
};

// One accumulated line of the loot sim: itemId -> total count across the pass.
using CmAHBItemMap = std::map<uint32_t, uint32_t>;

// Expansion of a loot source: 0 Vanilla, 1 TBC, 2 WotLK (MapEntry::expansionID).
static constexpr uint8_t CMAHB_EXP_VANILLA = 0;
static constexpr uint8_t CMAHB_EXP_TBC     = 1;
static constexpr uint8_t CMAHB_EXP_WOTLK   = 2;
// Sentinel for a source whose map could not be resolved (no spawn row).
static constexpr uint8_t CMAHB_EXP_UNKNOWN = 0xFF;

// A loot id paired with the expansion it belongs to (addendum §4 Layer 1).
struct CmAHBClassifiedId
{
    uint32_t lootId;
    uint8_t  expansion;
};
