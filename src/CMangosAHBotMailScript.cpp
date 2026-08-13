#include "CMangosAHBotConfig.h"
#include "Mail.h"
#include "ScriptMgr.h"

// Belt-and-suspenders gold sink (plan §2.3 / §7): destroy any mail addressed to
// the bot character, and drop the attached items (won auctions the bot bought).
class CMangosAHBot_MailScript : public MailScript
{
public:
    CMangosAHBot_MailScript() : MailScript("CMangosAHBot_MailScript",
        {
            MAILHOOK_ON_BEFORE_MAIL_DRAFT_SEND_MAIL_TO,
        }) {}

    void OnBeforeMailDraftSendMailTo(
        MailDraft* /*mailDraft*/,
        MailReceiver const& receiver,
        MailSender const& sender,
        MailCheckMask& /*checked*/,
        uint32& /*deliver_delay*/,
        uint32& /*custom_expiration*/,
        bool& deleteMailItemsFromDB,
        bool& sendMail) override
    {
        if (!gCMangosAHBotConfig.guid)
            return;

        if (receiver.GetPlayerGUIDLow() == gCMangosAHBotConfig.guid)
        {
            if (sender.GetMailMessageType() == MAIL_AUCTION)
                deleteMailItemsFromDB = true;
            sendMail = false;
        }
    }
};

void AddCMangosAHBotMailScripts()
{
    new CMangosAHBot_MailScript();
}
