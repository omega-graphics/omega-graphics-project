#include "common.h"

#include <future>

#ifndef OMEGA_COMMON_NET_H
#define OMEGA_COMMON_NET_H

namespace OmegaCommon {



    struct HttpRequestDescriptor {
        StrRef url;
        StrRef header;
    };

    struct HttpResponse {
        size_t size;
        void *data;
    };

    class OMEGACOMMON_EXPORT HttpClientContext {
    public:
        static std::shared_ptr<HttpClientContext> Create();
        virtual std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) = 0;
        virtual ~HttpClientContext() = default;
    };

}

#endif