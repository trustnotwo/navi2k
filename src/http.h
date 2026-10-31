#ifndef LP_HTTP_H
#define LP_HTTP_H

#include <windows.h>
#include <wininet.h>
#include <string>

enum HttpResult {
    HTTP_OK = 0,
    HTTP_ERR_BAD_URL,
    HTTP_ERR_RESOLVE,
    HTTP_ERR_CONNECT,
    HTTP_ERR_TIMEOUT,
    HTTP_ERR_TLS_HANDSHAKE,   // server rejected every protocol/cipher SChannel offered
    HTTP_ERR_TLS_CERT,        // handshake worked, certificate not trusted/expired/wrong name
    HTTP_ERR_STATUS,          // got a response, but not 200
    HTTP_ERR_CANCELLED,
    HTTP_ERR_OTHER
};

struct HttpOptions {
    bool  allowUntrustedCert;  // opt-in only: old root stores lack Let's Encrypt etc.
    DWORD timeoutMs;
    HttpOptions() : allowUntrustedCert(false), timeoutMs(15000) {}
};

bool HttpInit(const wchar_t* userAgent);
void HttpShutdown();

// One synchronous GET via WinINet. Read() may block; Abort() from another thread unblocks it.
// A local file path (C:\... or \\server\...) in place of the URL reads that file instead, with
// the same range and length behaviour, so downloaded tracks play through the same code.
class HttpRequest {
public:
    HttpRequest();
    ~HttpRequest();

    // rangeStart > 0 asks for the body from that byte on ("Range: bytes=N-", 206 expected).
    HttpResult Open(const std::wstring& url, const HttpOptions& opts, DWORD rangeStart = 0);
    int  Read(void* buf, int len);     // >0 bytes read, 0 = end of body, -1 = error or aborted
    void Abort();

    DWORD StatusCode() const { return m_status; }
    // Length of the body from Content-Length, or 0 if the server didn't say.
    DWORD ContentLength() const { return m_contentLength; }
    DWORD LastError() const  { return m_lastError; }

private:
    HttpRequest(const HttpRequest&);
    HttpRequest& operator=(const HttpRequest&);
    void CloseHandles();

    HttpResult OpenFile(const std::wstring& path, DWORD rangeStart);

    HINTERNET volatile m_conn;
    HINTERNET volatile m_req;
    HANDLE volatile m_file;     // a local file instead of a request (INVALID_HANDLE_VALUE: none)
    volatile LONG m_aborted;
    DWORD m_status;
    DWORD m_lastError;
    DWORD m_contentLength;
    bool  m_https;
};

HttpResult HttpGetAll(const std::wstring& url, const HttpOptions& opts, std::string& body,
                      DWORD* status, DWORD* winErr);

bool HttpIsHttpsUrl(const std::wstring& url);
bool HttpIsLocalPath(const std::wstring& url);   // "C:\..." or "\\server\..." rather than a URL
bool HttpIsTlsFailure(HttpResult r);

// User-facing explanation, including what to do about TLS on Win2000/XP.
std::wstring HttpDescribe(HttpResult r, DWORD status, DWORD winErr, bool https);

#endif
