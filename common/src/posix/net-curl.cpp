#include <curl/curl.h>

#include "omega-common/net.h"

namespace OmegaCommon {

    static struct CurlGlobalInit {
        CurlGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
        ~CurlGlobalInit() { curl_global_cleanup(); }
    } curlInit;

    class CURLHttpClientContext : public HttpClientContext {

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

    public:
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
}
