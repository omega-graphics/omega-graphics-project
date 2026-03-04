#include <curl/curl.h>

#include "omega-common/net.h"

namespace OmegaCommon {


    class CURLHttpClientContext : public HttpClientContext {

        CURL *curl;
    public:
        struct ResponseUserdata {
            std::promise<HttpResponse> *prom;
            CURL *curl;
        };
        static size_t responseCallback(char *ptr, size_t size, size_t nmemb, void *userdata){
            size_t dataSize = size * nmemb;
            auto *ud = (ResponseUserdata *)userdata;
            HttpResponse resp;
            resp.size = dataSize;
            resp.data = std::malloc(dataSize);
            if (resp.data && dataSize)
                memmove(resp.data, ptr, dataSize);
            long code = 0;
            if (ud->curl)
                curl_easy_getinfo(ud->curl, CURLINFO_RESPONSE_CODE, &code);
            resp.statusCode = (int)code;
            ud->prom->set_value(resp);
            return dataSize;
        };
        CURLHttpClientContext(){
            curl = curl_easy_init();
        }
        std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) override {
       
            curl_easy_setopt(curl,CURLOPT_URL,descriptor.url.data());
            curl_easy_setopt(curl,CURLOPT_HEADERDATA,descriptor.header.data());

            auto promise_ptr = new std::promise<HttpResponse>();
            ResponseUserdata ud{promise_ptr, curl};
            curl_easy_setopt(curl,CURLOPT_WRITEDATA,(void *)&ud);
            curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,responseCallback);

            curl_easy_perform(curl);
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

