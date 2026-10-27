#ifndef LP_CONFIG_H
#define LP_CONFIG_H

#include "subsonic.h"
#include <string>

// Reads [Server] from Navi2KPlayer.ini (creating a commented template if missing).
// A plain Password is converted to Token/Salt on first load and removed from the file.
bool ConfigLoad(const std::wstring& iniPath, SubsonicConfig& cfg, std::wstring& err);

#endif
