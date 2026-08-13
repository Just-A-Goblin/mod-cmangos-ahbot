#include "CMangosAHBot.h"
#include "CMangosAHBotConfig.h"
#include "AuctionHouseMgr.h"
#include "Player.h"
#include "ScriptMgr.h"

// Drives the bot tick, and suppresses proceeds mail / notifications for bot-owned
// auctions (plan §2.4 / §7 — proceeds never reaching the bot is the gold sink).
class CMangosAHBot_AuctionHouseScript : public AuctionHouseScript
{
public:
    CMangosAHBot_AuctionHouseScript() : AuctionHouseScript("CMangosAHBot_AuctionHouseScript",
        {
            AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_UPDATE,
            AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_SUCCESSFUL_MAIL,
            AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_EXPIRED_MAIL,
        }) {}

    void OnBeforeAuctionHouseMgrUpdate() override
    {
        if (gCMangosAHBotConfig.enable)
            sCMangosAHBot->Update();
    }

    void OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(
        AuctionHouseMgr* /*mgr*/, AuctionEntry* /*auction*/,
        Player* owner, uint32& /*owner_accId*/, uint32& /*profit*/,
        bool& sendNotification, bool& updateAchievementCriteria, bool& sendMail) override
    {
        if (owner && owner->GetGUID().GetCounter() == gCMangosAHBotConfig.guid)
        {
            sendNotification = false;
            updateAchievementCriteria = false;
            sendMail = false;
        }
    }

    void OnBeforeAuctionHouseMgrSendAuctionExpiredMail(
        AuctionHouseMgr* /*mgr*/, AuctionEntry* /*auction*/,
        Player* owner, uint32& /*owner_accId*/,
        bool& sendNotification, bool& sendMail) override
    {
        if (owner && owner->GetGUID().GetCounter() == gCMangosAHBotConfig.guid)
        {
            sendNotification = false;
            sendMail = false;
        }
    }
};

void AddCMangosAHBotAuctionHouseScripts()
{
    new CMangosAHBot_AuctionHouseScript();
}
