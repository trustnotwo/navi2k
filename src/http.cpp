#include "http.h"
#include "util.h"

// WinINet error codes by number: several are missing from older/MinGW headers.
enum {
    WI_TIMEOUT              = 12002,
    WI_INVALID_URL          = 12005,
    WI_UNRECOGNIZED_SCHEME  = 12006,
    WI_NAME_NOT_RESOLVED    = 12007,
    WI_OPERATION_CANCELLED  = 12017,
    WI_CANNOT_CONNECT       = 12029,
    WI_CONNECTION_ABORTED   = 12030,
    WI_CONNECTION_RESET     = 12031,
    WI_SEC_CERT_DATE        = 12037,
    WI_SEC_CERT_CN          = 12038,
    WI_INVALID_CA           = 12045,
    WI_SEC_CERT_ERRORS      = 12055,
    WI_SEC_CERT_REV_FAILED  = 12057,
    WI_INVALID_SERVER_RESP  = 12152,
    WI_SECURITY_CHANNEL     = 12157,
    WI_SEC_INVALID_CERT     = 12169,
    WI_SEC_CERT_REVOKED     = 12170
};

static HINTERNET g_session = NULL;

bool HttpInit(const wchar_t* userAgent)
{
    g_session = InternetOpenW(userAgent, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!g_session)
        LogF(L"InternetOpen failed: %lu", GetLastError());
    return g_session != NULL;
}

void HttpShutdown()
{
    if (g_session) {
        InternetCloseHandle(g_session);
        g_session = NULL;
    }
}

static HttpResult Classify(DWORD err, bool https)
{
    switch (err) {
    case WI_INVALID_URL:
    case WI_UNRECOGNIZED_SCHEME:
        return HTTP_ERR_BAD_URL;
    case WI_NAME_NOT_RESOLVED:
        return HTTP_ERR_RESOLVE;
    case WI_CANNOT_CONNECT:
        return HTTP_ERR_CONNECT;
    case WI_TIMEOUT:
        return HTTP_ERR_TIMEOUT;
    case WI_OPERATION_CANCELLED:
    case ERROR_INVALID_HANDLE:
        return HTTP_ERR_CANCELLED;
    case WI_SECURITY_CHANNEL:
        return HTTP_ERR_TLS_HANDSHAKE;
    case WI_SEC_CERT_DATE:
    case WI_SEC_CERT_CN:
    case WI_INVALID_CA:
    case WI_SEC_CERT_ERRORS:
    case WI_SEC_CERT_REV_FAILED:
    case WI_SEC_INVALID_CERT:
    case WI_SEC_CERT_REVOKED:
        return HTTP_ERR_TLS_CERT;
    case WI_CONNECTION_ABORTED:
    case WI_CONNECTION_RESET:
    case WI_INVALID_SERVER_RESP:
        // A modern server typically just drops a TLS 1.0 ClientHello, which surfaces
        // as abort/reset rather than a proper SChannel error.
        return https ? HTTP_ERR_TLS_HANDSHAKE : HTTP_ERR_CONNECT;
    }
    return HTTP_ERR_OTHER;
}

HttpRequest::HttpRequest()
    : m_conn(NULL), m_req(NULL), m_aborted(0), m_status(0), m_lastError(0), m_https(false)
{
}

HttpRequest::~HttpRequest()
{
    CloseHandles();
}

void HttpRequest::CloseHandles()
{
    HINTERNET req  = (HINTERNET)InterlockedExchangePointer((PVOID volatile*)&m_req, NULL);
    HINTERNET conn = (HINTERNET)InterlockedExchangePointer((PVOID volatile*)&m_conn, NULL);
    if (req)
        InternetCloseHandle(req);
    if (conn)
        InternetCloseHandle(conn);
}

void HttpRequest::Abort()
{
    InterlockedExchange(&m_aborted, 1);
    // Closing a WinINet handle is the documented way to cancel a blocking call on it.
    CloseHandles();
}

HttpResult HttpRequest::Open(const std::wstring& url, const HttpOptions& opts)
{
    CloseHandles();
    m_status = 0;
    m_lastError = 0;
    if (!g_session)
        return HTTP_ERR_OTHER;

    URL_COMPONENTSW uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = 1;
    uc.dwHostNameLength = 1;
    uc.dwUrlPathLength = 1;
    uc.dwExtraInfoLength = 1;
    if (!InternetCrackUrlW(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        m_lastError = GetLastError();
        return HTTP_ERR_BAD_URL;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)
        return HTTP_ERR_BAD_URL;
    m_https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty())
        path = L"/";

    HINTERNET conn = InternetConnectW(g_session, host.c_str(), uc.nPort, NULL, NULL,
                                      INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) {
        m_lastError = GetLastError();
        return Classify(m_lastError, m_https);
    }
    m_conn = conn;

    DWORD t = opts.timeoutMs;
    InternetSetOptionW(conn, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    InternetSetOptionW(conn, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    InternetSetOptionW(conn, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof(t));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE |
                  INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_AUTO_REDIRECT;
    if (m_https) {
        flags |= INTERNET_FLAG_SECURE;
        if (opts.allowUntrustedCert)
            flags |= INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
    }

    // GET only: Windows 2000's WinINet answers form POSTs to Navidrome with error 12150.
    HINTERNET req = HttpOpenRequestW(conn, L"GET", path.c_str(), NULL, NULL, NULL, flags, 0);
    if (!req) {
        m_lastError = GetLastError();
        return Classify(m_lastError, m_https);
    }
    m_req = req;
    if (m_aborted)
        return HTTP_ERR_CANCELLED;

    if (m_https && opts.allowUntrustedCert) {
        DWORD sf = 0, len = sizeof(sf);
        InternetQueryOptionW(req, INTERNET_OPTION_SECURITY_FLAGS, &sf, &len);
        sf |= SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_REVOCATION;
        InternetSetOptionW(req, INTERNET_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
    }

    if (!HttpSendRequestW(req, NULL, 0, NULL, 0)) {
        m_lastError = GetLastError();
        if (m_aborted)
            return HTTP_ERR_CANCELLED;
        LogF(L"HttpSendRequest failed: %lu (https=%d)", m_lastError, (int)m_https);
        return Classify(m_lastError, m_https);
    }

    DWORD status = 0, len = sizeof(status);
    if (!HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL)) {
        m_lastError = GetLastError();
        return HTTP_ERR_OTHER;
    }
    m_status = status;
    return status == 200 ? HTTP_OK : HTTP_ERR_STATUS;
}

int HttpRequest::Read(void* buf, int len)
{
    HINTERNET req = m_req;
    if (!req)
        return -1;
    DWORD got = 0;
    if (!InternetReadFile(req, buf, (DWORD)len, &got)) {
        m_lastError = GetLastError();
        return -1;
    }
    return (int)got;
}

HttpResult HttpGetAll(const std::wstring& url, const HttpOptions& opts, std::string& body,
                      DWORD* status, DWORD* winErr)
{
    static const size_t kMaxBody = 32 * 1024 * 1024;
    body.clear();
    HttpRequest req;
    HttpResult r = req.Open(url, opts);
    if (r == HTTP_OK) {
        char chunk[8192];
        for (;;) {
            int n = req.Read(chunk, sizeof(chunk));
            if (n == 0)
                break;
            if (n < 0) {
                r = Classify(req.LastError(), HttpIsHttpsUrl(url));
                if (r == HTTP_ERR_OTHER)
                    r = HTTP_ERR_CONNECT;
                break;
            }
            body.append(chunk, n);
            if (body.size() > kMaxBody) {
                r = HTTP_ERR_OTHER;
                break;
            }
        }
    }
    if (status)
        *status = req.StatusCode();
    if (winErr)
        *winErr = req.LastError();
    return r;
}

bool HttpIsHttpsUrl(const std::wstring& url)
{
    return _wcsnicmp(url.c_str(), L"https:", 6) == 0;
}

bool HttpIsTlsFailure(HttpResult r)
{
    return r == HTTP_ERR_TLS_HANDSHAKE || r == HTTP_ERR_TLS_CERT;
}

// 1 = TLS 1.0 enabled for WinINet, 0 = disabled, -1 = unknown.
// IE6 on Windows 2000 ships with "Use TLS 1.0" unticked, leaving only SSL 3.0.
// We only read the setting; changing security settings is the user's decision.
static int WinInetTls10State()
{
    static const DWORD kTls10 = 0x80;
    const struct { HKEY root; const wchar_t* key; } places[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CurrentVersion\\Internet Settings" },
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings" },
    };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); ++i) {
        HKEY k;
        if (RegOpenKeyExW(places[i].root, places[i].key, 0, KEY_READ, &k) != ERROR_SUCCESS)
            continue;
        DWORD value = 0, type = 0, size = sizeof(value);
        LONG rc = RegQueryValueExW(k, L"SecureProtocols", NULL, &type, (BYTE*)&value, &size);
        RegCloseKey(k);
        if (rc == ERROR_SUCCESS && type == REG_DWORD)
            return (value & kTls10) ? 1 : 0;
    }
    return -1;
}

std::wstring HttpDescribe(HttpResult r, DWORD status, DWORD winErr, bool https)
{
    switch (r) {
    case HTTP_OK:
        return L"OK";
    case HTTP_ERR_BAD_URL:
        return L"The server address is not a valid http:// or https:// URL.";
    case HTTP_ERR_RESOLVE:
        return L"The server name could not be found. Try its IP address instead.";
    case HTTP_ERR_CONNECT:
        return Fmt(L"Could not connect to the server (WinINet error %lu). "
                   L"Check the address, port (Navidrome defaults to 4533) and firewall.", winErr);
    case HTTP_ERR_TIMEOUT:
        return L"The server did not respond in time.";
    case HTTP_ERR_TLS_HANDSHAKE: {
        std::wstring m = Fmt(
            L"The server refused the secure (HTTPS) connection (WinINet error %lu).\r\n\r\n"
            L"Windows 2000 and XP can only use SSL 3.0 / TLS 1.0 with RC4 or 3DES ciphers, "
            L"and almost every server today requires TLS 1.2.", winErr);
        if (WinInetTls10State() == 0)
            m += L" TLS 1.0 is also switched off on this PC "
                 L"(Internet Options > Advanced > Use TLS 1.0).";
        m += L"\r\n\r\nWays to connect:\r\n"
             L"  - Use Navidrome's plain HTTP address on your local network: set LanUrl and "
             L"AllowHttpFallback=1 in Navi2KPlayer.ini.\r\n"
             L"  - Run a reverse proxy on your network that still accepts TLS 1.0 with "
             L"TLS_RSA_WITH_3DES_EDE_CBC_SHA and forwards to Navidrome.";
        return m;
    }
    case HTTP_ERR_TLS_CERT:
        return Fmt(L"The server's certificate was not accepted (WinINet error %lu): it is "
                   L"self-signed, expired, for a different name, or issued by an authority this "
                   L"old Windows doesn't know (e.g. Let's Encrypt).\r\n\r\n"
                   L"If you trust this network, set AllowUntrustedCert=1 in Navi2KPlayer.ini.", winErr);
    case HTTP_ERR_STATUS:
        if (status == 401 || status == 403)
            return Fmt(L"The server refused access (HTTP %lu). Check User and Password.", status);
        if (status == 404)
            return L"HTTP 404: this address doesn't look like a Navidrome/Subsonic server.";
        if (status >= 300 && status < 400)
            return Fmt(L"The server redirected the request (HTTP %lu)%ls. Point Url directly at "
                       L"Navidrome, e.g. http://192.168.1.10:4533", status,
                       https ? L"" : L", probably from HTTP to HTTPS");
        return Fmt(L"The server returned HTTP %lu.", status);
    case HTTP_ERR_CANCELLED:
        return L"Cancelled.";
    case HTTP_ERR_OTHER:
        break;
    }
    return Fmt(L"Network error (WinINet error %lu).", winErr);
}
