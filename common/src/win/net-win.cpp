#include "omega-common/net.h"

#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace OmegaCommon {

    class WinHTTPHttpClientContext : public HttpClientContext {
        HINTERNET hSession;
        HttpTlsConfig tlsConfig_;
    public:
        WinHTTPHttpClientContext() {
            hSession = WinHttpOpen(L"OmegaCommon",
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS,
                                   0);
        }

        explicit WinHTTPHttpClientContext(HttpTlsConfig config)
            : WinHTTPHttpClientContext()
        {
            tlsConfig_ = std::move(config);
        }

        std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) override {
            std::promise<HttpResponse> promise;
            auto future = promise.get_future();
            HttpResponse response;

            if (!hSession) {
                promise.set_value(std::move(response));
                return future;
            }

            int wideLen = MultiByteToWideChar(CP_UTF8, 0, descriptor.url.data(),
                                               (int)descriptor.url.size(), nullptr, 0);
            std::wstring urlWide(wideLen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, descriptor.url.data(),
                                 (int)descriptor.url.size(), &urlWide[0], wideLen);

            URL_COMPONENTS urlComp = {};
            urlComp.dwStructSize = sizeof(urlComp);
            urlComp.dwSchemeLength = (DWORD)-1;
            urlComp.dwHostNameLength = (DWORD)-1;
            urlComp.dwUrlPathLength = (DWORD)-1;
            urlComp.dwExtraInfoLength = (DWORD)-1;

            if (!WinHttpCrackUrl(urlWide.c_str(), (DWORD)urlWide.size(), 0, &urlComp)) {
                promise.set_value(std::move(response));
                return future;
            }

            std::wstring hostName(urlComp.lpszHostName, urlComp.dwHostNameLength);
            std::wstring urlPath(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
            if (urlComp.dwExtraInfoLength > 0)
                urlPath.append(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);

            HINTERNET hConnect = WinHttpConnect(hSession, hostName.c_str(), urlComp.nPort, 0);
            if (!hConnect) {
                promise.set_value(std::move(response));
                return future;
            }

            const wchar_t *method = L"GET";
            switch (descriptor.method) {
                case HttpMethod::Get:     method = L"GET"; break;
                case HttpMethod::Post:    method = L"POST"; break;
                case HttpMethod::Put:     method = L"PUT"; break;
                case HttpMethod::Delete:  method = L"DELETE"; break;
                case HttpMethod::Patch:   method = L"PATCH"; break;
                case HttpMethod::Head:    method = L"HEAD"; break;
                case HttpMethod::Options: method = L"OPTIONS"; break;
            }

            DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, urlPath.c_str(),
                                                     nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
            if (!hRequest) {
                WinHttpCloseHandle(hConnect);
                promise.set_value(std::move(response));
                return future;
            }

            for (const auto &h : descriptor.headers) {
                String line = h.first + ": " + h.second;
                int hdrWideLen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(),
                                                      (int)line.size(), nullptr, 0);
                std::wstring hdrWide(hdrWideLen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                     &hdrWide[0], hdrWideLen);
                WinHttpAddRequestHeaders(hRequest, hdrWide.c_str(), (DWORD)hdrWide.size(),
                                          WINHTTP_ADDREQ_FLAG_ADD);
            }

            if (!tlsConfig_.verifyPeer) {
                DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                 SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                 SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                 SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                                 &secFlags, sizeof(secFlags));
            }

            LPVOID bodyPtr = WINHTTP_NO_REQUEST_DATA;
            DWORD bodyLen = 0;
            if (!descriptor.body.empty()) {
                bodyPtr = (LPVOID)descriptor.body.data();
                bodyLen = (DWORD)descriptor.body.size();
            }

            BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                            bodyPtr, bodyLen, bodyLen, 0);
            if (sent)
                sent = WinHttpReceiveResponse(hRequest, nullptr);

            if (sent) {
                DWORD statusCode = 0;
                DWORD statusSize = sizeof(statusCode);
                WinHttpQueryHeaders(hRequest,
                                     WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                     WINHTTP_NO_HEADER_INDEX);
                response.statusCode = (int)statusCode;

                DWORD headerBufSize = 0;
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                     WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &headerBufSize,
                                     WINHTTP_NO_HEADER_INDEX);
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && headerBufSize > 0) {
                    std::wstring headerBuf(headerBufSize / sizeof(wchar_t), L'\0');
                    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                             WINHTTP_HEADER_NAME_BY_INDEX, &headerBuf[0],
                                             &headerBufSize, WINHTTP_NO_HEADER_INDEX)) {
                        size_t pos = 0;
                        while (pos < headerBuf.size()) {
                            size_t eol = headerBuf.find(L"\r\n", pos);
                            if (eol == std::wstring::npos) eol = headerBuf.size();
                            std::wstring hdrLine(headerBuf, pos, eol - pos);
                            pos = eol + 2;
                            if (hdrLine.empty() || hdrLine.find(L"HTTP/") == 0)
                                continue;
                            auto colon = hdrLine.find(L':');
                            if (colon != std::wstring::npos) {
                                int keyLen = WideCharToMultiByte(CP_UTF8, 0, hdrLine.c_str(),
                                                                  (int)colon, nullptr, 0,
                                                                  nullptr, nullptr);
                                String key(keyLen, '\0');
                                WideCharToMultiByte(CP_UTF8, 0, hdrLine.c_str(), (int)colon,
                                                     &key[0], keyLen, nullptr, nullptr);

                                auto valStart = hdrLine.find_first_not_of(L" \t", colon + 1);
                                String value;
                                if (valStart != std::wstring::npos) {
                                    int valLen = WideCharToMultiByte(
                                        CP_UTF8, 0, hdrLine.c_str() + valStart,
                                        (int)(hdrLine.size() - valStart),
                                        nullptr, 0, nullptr, nullptr);
                                    value.resize(valLen);
                                    WideCharToMultiByte(
                                        CP_UTF8, 0, hdrLine.c_str() + valStart,
                                        (int)(hdrLine.size() - valStart),
                                        &value[0], valLen, nullptr, nullptr);
                                }
                                response.headers.push_back({std::move(key), std::move(value)});
                            }
                        }
                    }
                }

                DWORD bytesAvailable = 0;
                while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                    size_t prevSize = response.body.size();
                    response.body.resize(prevSize + bytesAvailable);
                    DWORD bytesRead = 0;
                    WinHttpReadData(hRequest, response.body.data() + prevSize,
                                     bytesAvailable, &bytesRead);
                    response.body.resize(prevSize + bytesRead);
                    bytesAvailable = 0;
                }
            }

            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);

            promise.set_value(std::move(response));
            return future;
        }

        ~WinHTTPHttpClientContext() override {
            if (hSession)
                WinHttpCloseHandle(hSession);
        }
    };

    std::shared_ptr<HttpClientContext> HttpClientContext::Create() {
        return std::make_shared<WinHTTPHttpClientContext>();
    }

    std::shared_ptr<HttpClientContext> HttpClientContext::Create(HttpTlsConfig config) {
        return std::make_shared<WinHTTPHttpClientContext>(std::move(config));
    }
}
