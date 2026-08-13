/*
 * Module loader. The function name is fixed by AzerothCore's generated
 * ModulesLoader.cpp: "Add" + <dir-name with '-'->'_'> + "Scripts".
 * Directory must be exactly `mod-cmangos-ahbot` (verified: existing modules
 * produce Addmod_ah_botScripts / Addmod_individual_progressionScripts).
 */

// Forward declarations of the per-file script registrars.
void AddCMangosAHBotWorldScripts();
void AddCMangosAHBotAuctionHouseScripts();
void AddCMangosAHBotMailScripts();
void AddCMangosAHBotCommandScripts();

void Addmod_cmangos_ahbotScripts()
{
    AddCMangosAHBotWorldScripts();
    AddCMangosAHBotAuctionHouseScripts();
    AddCMangosAHBotMailScripts();
    AddCMangosAHBotCommandScripts();
}
