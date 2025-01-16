#pragma once

#include <string>
#include <curl/curl.h>


class CurlSession
{
private:
    CURL* curl;
    bool initialized;
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp)
    {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }


public:
    CurlSession() : curl(nullptr), initialized(false) {}
    ~CurlSession();
    bool Initialize();
    bool PerformGet(const std::string& url, std::string& response);
};
