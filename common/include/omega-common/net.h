#include "common.h"

#include <cstdint>
#include <future>

#ifndef OMEGA_COMMON_NET_H
#define OMEGA_COMMON_NET_H

namespace OmegaCommon {

    enum class HttpMethod {
        Get,
        Post,
        Put,
        Delete,
        Patch,
        Head,
        Options
    };

    struct HttpRequestDescriptor {
        StrRef url;
        HttpMethod method = HttpMethod::Get;
        String body;
        Vector<std::pair<String, String>> headers;
    };

    struct HttpResponse {
        int statusCode = 0;
        Vector<std::uint8_t> body;
        Vector<std::pair<String, String>> headers;

        String bodyAsString() const {
            if (body.empty()) return {};
            return String(reinterpret_cast<const char *>(body.data()), body.size());
        }

        bool ok() const {
            return statusCode >= 200 && statusCode < 300;
        }
    };

    class OMEGACOMMON_EXPORT HttpClientContext {
    public:
        static std::shared_ptr<HttpClientContext> Create();
        virtual std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) = 0;
        virtual ~HttpClientContext() = default;
    };

}

#endif
