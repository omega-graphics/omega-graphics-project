#include "omega-common/net.h"

namespace OmegaCommon {

    class NullHttpClientContext : public HttpClientContext {
    public:
        std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) override {
            (void)descriptor;
            std::promise<HttpResponse> promise;
            HttpResponse res;
            res.statusCode = 0;
            res.size = 0;
            res.data = nullptr;
            promise.set_value(res);
            return promise.get_future();
        }
    };

    std::shared_ptr<HttpClientContext> HttpClientContext::Create() {
        return std::make_shared<NullHttpClientContext>();
    }
}
