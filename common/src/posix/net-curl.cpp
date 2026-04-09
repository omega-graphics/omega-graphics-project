#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>

#include "omega-common/net.h"

namespace OmegaCommon {

    static struct CurlGlobalInit {
        CurlGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
        ~CurlGlobalInit() { curl_global_cleanup(); }
    } curlInit;

    // RAII wrapper for a PEM temp file with secure cleanup.
    class TempPemFile {
        String path_;
    public:
        TempPemFile() = default;

        explicit TempPemFile(const String &pemData) {
            char tmpl[] = "/tmp/omega-crypto-XXXXXX";
            int fd = mkstemp(tmpl);
            if (fd == -1) return;
            fchmod(fd, 0600);
            ::write(fd, pemData.data(), pemData.size());
            ::close(fd);
            path_ = tmpl;
        }

        ~TempPemFile() { cleanup(); }

        TempPemFile(TempPemFile &&o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
        TempPemFile &operator=(TempPemFile &&o) noexcept {
            if (this != &o) { cleanup(); path_ = std::move(o.path_); o.path_.clear(); }
            return *this;
        }
        TempPemFile(const TempPemFile &) = delete;
        TempPemFile &operator=(const TempPemFile &) = delete;

        const char *path() const { return path_.empty() ? nullptr : path_.c_str(); }

    private:
        void cleanup() {
            if (path_.empty()) return;
            // Overwrite with zeros before unlinking
            FILE *f = fopen(path_.c_str(), "r+b");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                if (sz > 0) {
                    fseek(f, 0, SEEK_SET);
                    Vector<char> zeros(static_cast<size_t>(sz), 0);
                    fwrite(zeros.data(), 1, static_cast<size_t>(sz), f);
                }
                fclose(f);
            }
            unlink(path_.c_str());
            path_.clear();
        }
    };

    class CURLHttpClientContext : public HttpClientContext {
        HttpTlsConfig tlsConfig_;
        TempPemFile caTempFile_;
        TempPemFile certTempFile_;
        TempPemFile keyTempFile_;

        struct TransferState {
            Vector<std::uint8_t> body;
            Vector<std::pair<String, String>> responseHeaders;
        };

        static size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
            size_t bytes = size * nmemb;
            auto *state = static_cast<TransferState *>(userdata);
            state->body.insert(state->body.end(), ptr, ptr + bytes);
            return bytes;
        }

        static size_t headerCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
            size_t bytes = size * nitems;
            auto *state = static_cast<TransferState *>(userdata);
            String line(buffer, bytes);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty() || line.rfind("HTTP/", 0) == 0)
                return bytes;
            auto colon = line.find(':');
            if (colon != String::npos) {
                String key = line.substr(0, colon);
                String value = line.substr(colon + 1);
                auto start = value.find_first_not_of(" \t");
                if (start != String::npos)
                    value = value.substr(start);
                else
                    value.clear();
                state->responseHeaders.push_back({std::move(key), std::move(value)});
            }
            return bytes;
        }

        void applyTlsConfig(CURL *curl) const {
            if (!tlsConfig_.verifyPeer) {
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            }

            if (caTempFile_.path())
                curl_easy_setopt(curl, CURLOPT_CAINFO, caTempFile_.path());

            if (certTempFile_.path())
                curl_easy_setopt(curl, CURLOPT_SSLCERT, certTempFile_.path());

            if (keyTempFile_.path())
                curl_easy_setopt(curl, CURLOPT_SSLKEY, keyTempFile_.path());
        }

    public:
        CURLHttpClientContext() = default;

        explicit CURLHttpClientContext(HttpTlsConfig config)
            : tlsConfig_(std::move(config))
        {
            if (!tlsConfig_.caBundlePem.empty())
                caTempFile_ = TempPemFile(tlsConfig_.caBundlePem);
            if (!tlsConfig_.clientCertPem.empty())
                certTempFile_ = TempPemFile(tlsConfig_.clientCertPem);
            if (!tlsConfig_.clientKeyPem.empty())
                keyTempFile_ = TempPemFile(tlsConfig_.clientKeyPem);
        }

        std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) override {
            std::promise<HttpResponse> promise;
            auto future = promise.get_future();

            CURL *curl = curl_easy_init();
            if (!curl) {
                promise.set_value(HttpResponse{});
                return future;
            }

            curl_easy_setopt(curl, CURLOPT_URL, descriptor.url.data());

            switch (descriptor.method) {
                case HttpMethod::Get:
                    break;
                case HttpMethod::Post:
                    curl_easy_setopt(curl, CURLOPT_POST, 1L);
                    break;
                case HttpMethod::Put:
                    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
                    break;
                case HttpMethod::Delete:
                    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
                    break;
                case HttpMethod::Patch:
                    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
                    break;
                case HttpMethod::Head:
                    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
                    break;
                case HttpMethod::Options:
                    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "OPTIONS");
                    break;
            }

            if (!descriptor.body.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, descriptor.body.data());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)descriptor.body.size());
            }

            struct curl_slist *headerList = nullptr;
            for (const auto &h : descriptor.headers) {
                String headerLine = h.first + ": " + h.second;
                headerList = curl_slist_append(headerList, headerLine.c_str());
            }
            if (headerList)
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

            TransferState state;
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);

            applyTlsConfig(curl);

            CURLcode res = curl_easy_perform(curl);

            HttpResponse response;
            if (res == CURLE_OK) {
                long code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
                response.statusCode = (int)code;
                response.body = std::move(state.body);
                response.headers = std::move(state.responseHeaders);
            }

            if (headerList)
                curl_slist_free_all(headerList);
            curl_easy_cleanup(curl);

            promise.set_value(std::move(response));
            return future;
        }

        ~CURLHttpClientContext() override = default;
    };

    std::shared_ptr<HttpClientContext> HttpClientContext::Create() {
        return std::make_shared<CURLHttpClientContext>();
    }

    std::shared_ptr<HttpClientContext> HttpClientContext::Create(HttpTlsConfig config) {
        return std::make_shared<CURLHttpClientContext>(std::move(config));
    }
}
