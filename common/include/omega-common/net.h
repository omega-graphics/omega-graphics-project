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

    /// TLS configuration for HTTP clients. PEM strings allow any key type.
    struct HttpTlsConfig {
        String caBundlePem;     /// Custom CA bundle in PEM format. Empty = system default.
        String clientCertPem;   /// Client certificate PEM for mutual TLS. Empty = none.
        String clientKeyPem;    /// Client private key PEM for mutual TLS. Empty = none.
        bool verifyPeer = true; /// Verify the server's certificate chain and hostname.
    };

    class OMEGACOMMON_EXPORT HttpClientContext {
    public:
        static std::shared_ptr<HttpClientContext> Create();
        static std::shared_ptr<HttpClientContext> Create(HttpTlsConfig config);
        virtual std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) = 0;
        virtual ~HttpClientContext() = default;
    };

}

#endif
