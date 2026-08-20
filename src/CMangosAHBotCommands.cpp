#include "CMangosAHBot.h"
#include "CMangosAHBotConfig.h"
#include "Chat.h"
#include "ScriptMgr.h"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#if AC_COMPILER == AC_COMPILER_GNU
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

using namespace Acore::ChatCommands;

class CMangosAHBot_CommandScript : public CommandScript
{
public:
    CMangosAHBot_CommandScript() : CommandScript("CMangosAHBot_CommandScript") {}

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> table =
        {
            { "cmahbot", HandleCMahbotCommand, SEC_GAMEMASTER, Console::Yes }
        };
        return table;
    }

    static void SendMultiline(ChatHandler* handler, const std::string& text)
    {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line))
            handler->SendSysMessage(line.c_str());
    }

    static bool HandleCMahbotCommand(ChatHandler* handler, char const* args)
    {
        char* sub = strtok(const_cast<char*>(args), " ");
        if (!sub)
        {
            handler->SendSysMessage("cmahbot: status | reload | rebuild [ally|horde|neutral] | "
                                    "progression [refresh] | craft <status|selftest|simulate|cost|dump [file]|testlist ...> | "
                                    "item <id> <value> <chance> <min> <max> | item reset <id>");
            return true;
        }
        size_t l = strlen(sub);

        if (strncmp(sub, "status", l) == 0)
        {
            handler->SendSysMessage(sCMangosAHBot->StatusReport().c_str());
            return true;
        }

        if (strncmp(sub, "reload", l) == 0)
        {
            gCMangosAHBotConfig.Load();
            sCMangosAHBot->ReloadData();
            handler->SendSysMessage("CMangosAHBot: config + overrides reloaded.");
            return true;
        }

        if (strncmp(sub, "progression", l) == 0)
        {
            char* p2 = strtok(nullptr, " ");
            if (p2 && strncmp(p2, "refresh", strlen(p2)) == 0)
            {
                sCMangosAHBot->RefreshProgression(true);
                handler->SendSysMessage("CMangosAHBot: progression recomputed.");
                return true;
            }
            SendMultiline(handler, sCMangosAHBot->ProgressionReport());
            return true;
        }

        if (strncmp(sub, "craft", l) == 0)
        {
            char* p2 = strtok(nullptr, " ");
            if (p2 && strncmp(p2, "selftest", strlen(p2)) == 0)
            {
                handler->SendSysMessage(sCMangosAHBot->CraftSelfTest().c_str());
                return true;
            }
            if (p2 && strncmp(p2, "simulate", strlen(p2)) == 0)
            {
                char* nArg = strtok(nullptr, " ");
                uint32_t n = nArg ? static_cast<uint32_t>(strtoul(nArg, nullptr, 0)) : 0;
                SendMultiline(handler, sCMangosAHBot->CraftSimulateSessions(n));
                return true;
            }
            if (p2 && strncmp(p2, "cost", strlen(p2)) == 0)
            {
                char* nArg = strtok(nullptr, " ");
                uint32_t n = nArg ? static_cast<uint32_t>(strtoul(nArg, nullptr, 0)) : 0;
                SendMultiline(handler, sCMangosAHBot->CraftSimulateCost(n));
                return true;
            }
            if (p2 && strncmp(p2, "dump", strlen(p2)) == 0)
            {
                char* fArg = strtok(nullptr, " ");
                std::string file = fArg ? fArg : "";
                handler->SendSysMessage(sCMangosAHBot->CraftDump(file, /*liveAH=*/true).c_str());
                return true;
            }
            if (p2 && strncmp(p2, "testlist", strlen(p2)) == 0)
            {
                char* a[4]; for (int i = 0; i < 4; ++i) a[i] = strtok(nullptr, " ");
                if (!a[0] || !a[1] || !a[2] || !a[3])
                {
                    handler->SendSysMessage("Syntax: .cmahbot craft testlist <itemId> <count> <stack> <price>");
                    return true;
                }
                handler->SendSysMessage(sCMangosAHBot->CraftTestList(
                    strtoul(a[0], nullptr, 0), strtoul(a[1], nullptr, 0),
                    strtoul(a[2], nullptr, 0), strtoul(a[3], nullptr, 0)).c_str());
                return true;
            }
            // default: craft status
            SendMultiline(handler, sCMangosAHBot->CraftStatusReport());
            return true;
        }

        if (strncmp(sub, "rebuild", l) == 0)
        {
            char* p2 = strtok(nullptr, " ");
            uint32_t house = CMAHB_HOUSE_COUNT;
            if (p2)
            {
                if (!strcmp(p2, "ally"))         house = CMAHB_HOUSE_ALLIANCE;
                else if (!strcmp(p2, "horde"))   house = CMAHB_HOUSE_HORDE;
                else if (!strcmp(p2, "neutral")) house = CMAHB_HOUSE_NEUTRAL;
            }
            handler->SendSysMessage("CMangosAHBot: rebuilding (the world may pause briefly)...");
            sCMangosAHBot->Rebuild(true, house);
            handler->SendSysMessage(sCMangosAHBot->StatusReport().c_str());
            return true;
        }

        if (strncmp(sub, "item", l) == 0)
        {
            char* a1 = strtok(nullptr, " ");
            if (a1 && strncmp(a1, "reset", strlen(a1)) == 0)
            {
                char* ids = strtok(nullptr, " ");
                if (!ids)
                {
                    handler->SendSysMessage("Syntax: .cmahbot item reset <id>");
                    return true;
                }
                sCMangosAHBot->ResetOverride(static_cast<uint32_t>(strtoul(ids, nullptr, 0)));
                handler->SendSysMessage("CMangosAHBot: override reset.");
                return true;
            }

            char* v  = strtok(nullptr, " ");
            char* c  = strtok(nullptr, " ");
            char* mn = strtok(nullptr, " ");
            char* mx = strtok(nullptr, " ");
            if (!a1 || !v || !c || !mn || !mx)
            {
                handler->SendSysMessage("Syntax: .cmahbot item <id> <value> <chance> <min> <max>");
                return true;
            }
            sCMangosAHBot->SetOverride(
                static_cast<uint32_t>(strtoul(a1, nullptr, 0)),
                static_cast<uint32_t>(strtoul(v,  nullptr, 0)),
                static_cast<uint32_t>(strtoul(c,  nullptr, 0)),
                static_cast<uint32_t>(strtoul(mn, nullptr, 0)),
                static_cast<uint32_t>(strtoul(mx, nullptr, 0)));
            handler->SendSysMessage("CMangosAHBot: override set.");
            return true;
        }

        handler->SendSysMessage("cmahbot: unknown subcommand. Try: status | reload | rebuild | progression | item");
        return true;
    }
};

void AddCMangosAHBotCommandScripts()
{
    new CMangosAHBot_CommandScript();
}
