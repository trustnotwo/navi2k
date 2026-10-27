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

bool ConfigLoad(const std::wstring& ini, SubsonicConfig& cfg, std::wstring& err)
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

    if (cfg.url.empty() || cfg.user.empty()) {
        err = L"Set Url, User and Password in\r\n" + ini;
        return false;
    }

    std::wstring password = IniGet(ini, L"Password");
    if (!password.empty()) {
        std::string token, salt;
        if (!SubsonicMakeToken(WideToUtf8(password), token, salt)) {
            err = Fmt(L"Could not hash the password with CryptoAPI (error %lu).", GetLastError());
            return false;
        }
        WritePrivateProfileStringW(L"Server", L"Token", Utf8ToWide(token).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Server", L"Salt", Utf8ToWide(salt).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Server", L"Password", NULL, ini.c_str());   // removes the key
    }
    cfg.token = WideToUtf8(IniGet(ini, L"Token"));
    cfg.salt = WideToUtf8(IniGet(ini, L"Salt"));
    if (cfg.token.empty() || cfg.salt.empty()) {
        err = L"Enter your Password in\r\n" + ini;
        return false;
    }
    return true;
}
