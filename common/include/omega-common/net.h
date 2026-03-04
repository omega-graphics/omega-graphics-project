#include "common.h"

#include <future>

#ifndef OMEGA_COMMON_NET_H
#define OMEGA_COMMON_NET_H

namespace OmegaCommon {



    struct HttpRequestDescriptor {
        StrRef url;
        StrRef header;
    };

    /**
     * HTTP response body and metadata.
     * Ownership: data is allocated with malloc() and must be freed by the caller (e.g. free(resp.data)).
     * If the request failed or no body was returned, data may be nullptr and size 0; still call free(data) if non-null.
     */
    struct HttpResponse {
        /** HTTP status code (e.g. 200, 404). 0 or negative indicates failure or unknown. */
        int statusCode = 0;
        size_t size = 0;
        void *data = nullptr;
    };

    class OMEGACOMMON_EXPORT HttpClientContext {
    public:
        static std::shared_ptr<HttpClientContext> Create();
        virtual std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) = 0;
        virtual ~HttpClientContext() = default;
    };

}

#endif