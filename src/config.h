#ifndef LP_CONFIG_H
#define LP_CONFIG_H

#include "subsonic.h"
#include <string>

// Settings live in Navi2KPlayer.ini next to the exe; File > Settings edits them.

// Reads [Server] as-is (creating a commented template if the file is missing), with no
// checks, for filling the Settings dialog.
void ConfigRead(const std::wstring& iniPath, SubsonicConfig& cfg);

// Reads [Server] and checks it's complete enough to connect. A plain Password typed into the
// ini is converted to Token/Salt and removed from the file.
bool ConfigLoad(const std::wstring& iniPath, SubsonicConfig& cfg, std::wstring& err);

// Writes [Server]. A non-empty newPassword replaces the saved token/salt (it isn't stored).
bool ConfigSaveServer(const std::wstring& iniPath, const SubsonicConfig& cfg, const std::wstring& newPassword,
                      std::wstring& err);

struct PlaybackSettings {
    std::wstring device;    // waveOut device name; empty = system default
    int bufferCount;        // 8 KB buffers queued ahead
    bool scrobble;          // report plays to the server
};

void PlaybackLoad(const std::wstring& iniPath, PlaybackSettings& s);
void PlaybackSave(const std::wstring& iniPath, const PlaybackSettings& s);

#endif
