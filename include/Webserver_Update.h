#pragma once

#include <ESPAsyncWebServer.h>

class WebserverUpdateClass
{
    public:
        void init(AsyncWebServer& server);

    private:
        void onUploadRequest(AsyncWebServerRequest* request);
        void onUpload(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final);
        void updateCrcFromChunk(AsyncWebServerRequest* request, uint8_t* data, size_t data_offset, size_t data_len);
};

/* vim:set ts=4 et: */
