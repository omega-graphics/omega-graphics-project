#include "omega-common/net.h"

namespace OmegaCommon {

    class NullHttpClientContext : public HttpClientContext {
    public:
        std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) override {
            (void)descriptor;
            std::promise<HttpResponse> promise;
            promise.set_value(HttpResponse{0,nullptr});
            return promise.get_future();
        }
    };

    std::shared_ptr<HttpClientContext> HttpClientContext::Create() {
        return std::make_shared<NullHttpClientContext>();
    }
}
