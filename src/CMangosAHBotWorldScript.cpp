#include "CMangosAHBot.h"
#include "CMangosAHBotConfig.h"
#include "ScriptMgr.h"

// Startup + config lifecycle.
class CMangosAHBot_WorldScript : public WorldScript
{
public:
    CMangosAHBot_WorldScript() : WorldScript("CMangosAHBot_WorldScript",
        {
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_BEFORE_CONFIG_LOAD,
        }) {}

    void OnStartup() override
    {
        gCMangosAHBotConfig.Load();
        sCMangosAHBot->Initialize();
    }

    void OnBeforeConfigLoad(bool reload) override
    {
        gCMangosAHBotConfig.Load();
        if (reload)
            sCMangosAHBot->ReloadData();
    }
};

void AddCMangosAHBotWorldScripts()
{
    new CMangosAHBot_WorldScript();
}
