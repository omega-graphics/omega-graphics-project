#include <curl/curl.h>

#include "omega-common/net.h"

namespace OmegaCommon {


    class CURLHttpClientContext : public HttpClientContext {

        CURL *curl;
    public:
        static size_t responseCallback(char *ptr, size_t size, size_t nmemb, void *userdata){
            size_t dataSize = size * nmemb;
            auto prom = (std::promise<HttpResponse> *)userdata;
            HttpResponse resp {dataSize,std::malloc(dataSize)};
            memmove(resp.data,ptr,dataSize);
            prom->set_value(resp);
            return dataSize;
        };
        CURLHttpClientContext(){
            curl = curl_easy_init();
        }
        std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) override {
       
            curl_easy_setopt(curl,CURLOPT_URL,descriptor.url.data());
            // curl_easy_setopt(curl,CURLOPT_CURLU,url);
            curl_easy_setopt(curl,CURLOPT_HEADERDATA,descriptor.header.data());

            auto promise_ptr = new std::promise<HttpResponse>();

            curl_easy_setopt(curl,CURLOPT_WRITEDATA,(void *)promise_ptr);

            curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,responseCallback);

            auto code = curl_easy_perform(curl);

            curl_easy_reset(curl);

            return promise_ptr->get_future();
        }
        ~CURLHttpClientContext() override {
            curl_global_cleanup();
        }
    };

    std::shared_ptr<HttpClientContext> HttpClientContext::Create(){
        return (std::shared_ptr<HttpClientContext>)new CURLHttpClientContext();
    }
}

