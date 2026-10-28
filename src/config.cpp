#include "config.h"
#include "util.h"

static std::wstring IniGet(const std::wstring& ini, const wchar_t* key)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"Server", key, L"", buf, 1024, ini.c_str());
    return buf;
}

static int IniGetInt(const std::wstring& ini, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(L"Server", key, def, ini.c_str());
}

static void WriteIniTemplate(const std::wstring& ini)
{
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    // UTF-16LE with BOM so the *PrivateProfile*W APIs keep non-ASCII names intact.
    static const wchar_t kTemplate[] =
        L"\xFEFF"
        L"; Navi2K Player configuration\r\n"
        L"[Server]\r\n"
        L"; Navidrome address. Plain HTTP on your LAN is the reliable option on Windows 2000/XP:\r\n"
        L";   Url=http://192.168.1.10:4533\r\n"
        L"; https:// only works if the server still accepts TLS 1.0.\r\n"
        L"Url=\r\n"
        L"; Optional plain-HTTP LAN address used if the https:// Url fails (LAN addresses only).\r\n"
        L"LanUrl=\r\n"
        L"AllowHttpFallback=0\r\n"
        L"; 1 = accept certificates this Windows doesn't trust. Only on networks you trust.\r\n"
        L"AllowUntrustedCert=0\r\n"
        L"User=\r\n"
        L"; Replaced by Token/Salt on first connect; the password itself is not kept.\r\n"
        L"Password=\r\n"
        L"; Navidrome transcodes everything to MP3 at this bitrate (kbps).\r\n"
        L"MaxBitRate=192\r\n";
    HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written;
    WriteFile(f, kTemplate, sizeof(kTemplate) - sizeof(wchar_t), &written, NULL);
    CloseHandle(f);
}

// Pre-rename builds were "Legacy Player": adopt their settings (and token) once.
static void MigrateOldIni(const std::wstring& ini)
{
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    size_t slash = ini.find_last_of(L'\\');
    std::wstring old = (slash == std::wstring::npos ? std::wstring() : ini.substr(0, slash + 1)) +
                       L"LegacyPlayer.ini";
    if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && MoveFileW(old.c_str(), ini.c_str()))
        LogF(L"Migrated settings from LegacyPlayer.ini");
}

// Replaces the saved login with a token/salt for 'password'; the password is never stored.
static bool StorePassword(const std::wstring& ini, const std::wstring& password, std::wstring& err)
{
    std::string token, salt;
    if (!SubsonicMakeToken(WideToUtf8(password), token, salt)) {
        err = Fmt(L"Could not hash the password with CryptoAPI (error %lu).", GetLastError());
        return false;
    }
    WritePrivateProfileStringW(L"Server", L"Token", Utf8ToWide(token).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Server", L"Salt", Utf8ToWide(salt).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Server", L"Password", NULL, ini.c_str());   // removes the key
    return true;
}

void ConfigRead(const std::wstring& ini, SubsonicConfig& cfg)
{
    MigrateOldIni(ini);
    WriteIniTemplate(ini);
    cfg.url = IniGet(ini, L"Url");
    cfg.lanUrl = IniGet(ini, L"LanUrl");
    cfg.allowHttpFallback = IniGetInt(ini, L"AllowHttpFallback", 0) != 0;
    cfg.allowUntrustedCert = IniGetInt(ini, L"AllowUntrustedCert", 0) != 0;
    cfg.user = WideToUtf8(IniGet(ini, L"User"));
    cfg.maxBitRate = IniGetInt(ini, L"MaxBitRate", 192);
    if (cfg.maxBitRate < 32 || cfg.maxBitRate > 320)
        cfg.maxBitRate = 192;
    cfg.token = WideToUtf8(IniGet(ini, L"Token"));
    cfg.salt = WideToUtf8(IniGet(ini, L"Salt"));
}

bool ConfigLoad(const std::wstring& ini, SubsonicConfig& cfg, std::wstring& err)
{
    ConfigRead(ini, cfg);
    if (cfg.url.empty() || cfg.user.empty()) {
        err = L"Enter the server address and your user name in File > Settings.";
        return false;
    }
    // A password typed into the ini by hand still works: it's swapped for a token here.
    std::wstring password = IniGet(ini, L"Password");
    if (!password.empty()) {
        if (!StorePassword(ini, password, err))
            return false;
        cfg.token = WideToUtf8(IniGet(ini, L"Token"));
        cfg.salt = WideToUtf8(IniGet(ini, L"Salt"));
    }
    if (cfg.token.empty() || cfg.salt.empty()) {
        err = L"Enter your password in File > Settings.";
        return false;
    }
    return true;
}

bool ConfigSaveServer(const std::wstring& ini, const SubsonicConfig& cfg, const std::wstring& newPassword,
                      std::wstring& err)
{
    const wchar_t* f = ini.c_str();
    WritePrivateProfileStringW(L"Server", L"Url", cfg.url.c_str(), f);
    WritePrivateProfileStringW(L"Server", L"LanUrl", cfg.lanUrl.c_str(), f);
    WritePrivateProfileStringW(L"Server", L"AllowHttpFallback", cfg.allowHttpFallback ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"Server", L"AllowUntrustedCert", cfg.allowUntrustedCert ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"Server", L"User", Utf8ToWide(cfg.user).c_str(), f);
    WritePrivateProfileStringW(L"Server", L"MaxBitRate", Fmt(L"%d", cfg.maxBitRate).c_str(), f);
    if (!newPassword.empty())
        return StorePassword(ini, newPassword, err);
    return true;
}

void PlaybackLoad(const std::wstring& ini, PlaybackSettings& s)
{
    wchar_t buf[256];
    GetPrivateProfileStringW(L"Player", L"OutputDevice", L"", buf, 256, ini.c_str());
    s.device = buf;
    s.bufferCount = (int)GetPrivateProfileIntW(L"Player", L"Buffers", 12, ini.c_str());
    s.scrobble = GetPrivateProfileIntW(L"Player", L"Scrobble", 1, ini.c_str()) != 0;
}

void PlaybackSave(const std::wstring& ini, const PlaybackSettings& s)
{
    WritePrivateProfileStringW(L"Player", L"OutputDevice", s.device.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Player", L"Buffers", Fmt(L"%d", s.bufferCount).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Player", L"Scrobble", s.scrobble ? L"1" : L"0", ini.c_str());
}
