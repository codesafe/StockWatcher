
#include "Network.h"

CurlSession::~CurlSession()
{
    if (curl)
    {
        curl_easy_cleanup(curl);
        curl = nullptr;
    }
    initialized = false;
}

bool CurlSession::Initialize() 
{
    if (!initialized) 
    {
        curl = curl_easy_init();
        initialized = (curl != nullptr);
    }
    return initialized;
}

bool CurlSession::PerformGet(const std::string& url, std::string& response) 
{
    if (!initialized || !curl) return false;

    curl_easy_reset(curl);
    std::string buffer;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) 
    {
        response = buffer;
        return true;
    }
    return false;
}


