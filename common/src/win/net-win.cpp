#include "omega-common/net.h"


#include <comdef.h>
#include <MsXml6.h>

#include <Windows.h>
#include <winhttp.h>
//#include <WinInet.h>


#include <icu.h>

#pragma comment(lib,"icuuc.lib")
#pragma comment(lib,"winhttp.lib")




namespace OmegaCommon {

    class WinHTTPHttpClientContext : public HttpClientContext {
        HINTERNET hinternet;
    public:
        struct PrivData {
            std::promise<HttpResponse> *prom;
            HINTERNET connection;
        };
        static void HttpStatusCallback(HINTERNET hInternet,DWORD_PTR dwContext,DWORD dwInternetStatus,LPVOID lpvStatusInformation,DWORD dwStatusInformationLength){
            auto data = (PrivData *)dwContext;
            DWORD bytesAvailable = 0;
            WinHttpQueryDataAvailable(hInternet,&bytesAvailable);
            HttpResponse res;
            res.statusCode = 200;
            res.size = (size_t)bytesAvailable;
            res.data = bytesAvailable ? std::malloc((size_t)bytesAvailable) : nullptr;
            if (res.data && bytesAvailable) {
                DWORD bytesRead = 0;
                WinHttpReadData(hInternet,res.data,bytesAvailable,&bytesRead);
                res.size = (size_t)bytesRead;
            }
            data->prom->set_value(res);
            WinHttpCloseHandle(hInternet);
            WinHttpCloseHandle(data->connection);
        }

        WinHTTPHttpClientContext(){
            hinternet = WinHttpOpen(L"OmegaCommon",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
        }
        std::future<HttpResponse> makeRequest(HttpRequestDescriptor descriptor) override{
            size_t urlLen = descriptor.url.size();
            auto * url_w = new UChar[urlLen];
            u_charsToUChars(descriptor.url.data(),url_w,(int32_t)urlLen);
            URL_COMPONENTS url_components;
            WinHttpCrackUrl(reinterpret_cast<LPCWSTR>(url_w), urlLen, ICU_DECODE, &url_components);

            HINTERNET connection = WinHttpConnect(hinternet,url_components.lpszHostName,INTERNET_DEFAULT_HTTPS_PORT,NULL);

             auto prom = new std::promise<HttpResponse>();
             auto data = new PrivData {prom,connection};
          
            HINTERNET request = WinHttpOpenRequest(connection,L"GET",url_components.lpszUrlPath,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,NULL);
            WinHttpSetOption(request,WINHTTP_OPTION_CONTEXT_VALUE,data,sizeof(*data));
            WinHttpSetStatusCallback(request,HttpStatusCallback,WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED,0);
            WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0);

           
            return prom->get_future();
        }
        ~WinHTTPHttpClientContext(){
            WinHttpCloseHandle(hinternet);
        }


    };



    std::shared_ptr<HttpClientContext> HttpClientContext::Create() {
        return std::make_shared<WinHTTPHttpClientContext>();
    }
}