/*
    Handle updates of firmware, littlefs or any other fileupload (POST requests)
    at http://<espiIp>/update

    Configuration changes get updated via the websochet and a command
*/
#include "Webserver_Update.h"

#include "AmisReader.h"
#include "Application.h"
#include "config.h"
#include "Log.h"
#define LOGMODULE LOGMODULE_UPDATE
#include "Reboot.h"
#include "SystemMonitor.h"
#include "unused.h"

#include <LittleFS.h>


extern void historyInit(void);


typedef enum {
    firmware = U_FLASH,
    littlefs = U_FS,
    anyOther,
    monate,
    none
} uploadFileType_t;

typedef struct {
    uint32_t length;
    uint32_t crc32;
} firmwareData_t;

typedef struct {
    uploadFileType_t _uploadfiletype;
    firmwareData_t _firmwareinfo_in_file;     // Read from the firmware chunks
    firmwareData_t _firmwareinfo_computed;    // Computed while processing upload
    int _uploadHttpCode;
    const char *_uploadError_pstr;
    size_t _expectedSize;
    String _uploadFilename;
    char _md5sum[33];
    MD5Builder _md5;
} updateRequestData_t;


void WebserverUpdateClass::init(AsyncWebServer& server)
{
    using std::placeholders::_1;
    using std::placeholders::_2;
    using std::placeholders::_3;
    using std::placeholders::_4;
    using std::placeholders::_5;
    using std::placeholders::_6;

    server.on("/update", HTTP_POST,
                std::bind(&WebserverUpdateClass::onUploadRequest, this, _1),
                std::bind(&WebserverUpdateClass::onUpload, this, _1, _2, _3, _4, _5, _6));
}


void WebserverUpdateClass::updateCrcFromChunk(AsyncWebServerRequest* request, uint8_t* data, size_t data_offset, size_t data_len)
{
    // at offset 4122 = (4096 + 16) in a firmware.bin there two uint32_t containing length
    // and the crc32sum (generated while the two values in the file were set to 0)
    // see .platformio/packages/framework-arduinoespressif8266/tools/elf2bin.py

    // So now: while uploading firmware.bin extract the expected data and recompute crc32
    // Verify extracted length and crc32 before flashing

    updateRequestData_t *requestData = (updateRequestData_t *) request->_tempObject;

    constexpr size_t len_crc_start = 4096 + 16;
    constexpr size_t len_crc_end = 4096 + 16 + 8;
    const size_t chunk_start = data_offset;
    const size_t chunk_end = data_offset + data_len;
    const uint8_t *chunk_end_p = data + chunk_end;

    //  address       0          4122       4129             xxx
    //  chunk A       |-------|
    //  chunk B*              |-------|
    //  chunk C*                     |----|
    //  chunk D*                         |-------|
    //  chunk E                                     |-------|
    //  chunk F*           |----------------------------|


    if (chunk_end < len_crc_start || chunk_start >= len_crc_end) {
        // chunk A                ||  chunk E
        requestData->_firmwareinfo_computed.crc32 = crc32(data, data_len, requestData->_firmwareinfo_computed.crc32);
        requestData->_firmwareinfo_computed.length += data_len;
        return;
    }

    // Copy parts from chunk (length, crc) to _firmwareinfo_in_file
    // Overwrite those values with 0 within the chunk(data) so we can recompute crc32
    uint8_t *datax;
    uint8_t *lencrc;
    datax = data;
    lencrc = (uint8_t *) &requestData->_firmwareinfo_in_file;
    const uint8_t *lencrc_end_p = lencrc + sizeof(requestData->_firmwareinfo_in_file);
    if (chunk_start < len_crc_start) {
        datax -= chunk_start;       // datax now points to virtual "data[0]"
        datax += len_crc_start;     // datax now points to virtual "data[4122]"
    } else {
        lencrc -= len_crc_start;    // lencrc now points to virtual "lencrc[0]"
        lencrc += chunk_start;      // lencrc now points to virtual "lencrc[chunk_start]"
    }
    for (; lencrc < lencrc_end_p && datax < chunk_end_p; ) {
        *lencrc = *datax;
        *datax = 0;
        lencrc++;  datax++;
    }

    // update crc32 (we now have \0 at length and crc values in the chunk)
    requestData->_firmwareinfo_computed.crc32 = crc32(data, data_len, requestData->_firmwareinfo_computed.crc32);
    requestData->_firmwareinfo_computed.length += data_len;

    // We dont' have to restore the original data as Update.write() was already called!
#if 0
    // Restore overwritten values within chunk
    datax = data;
    lencrc = (uint8_t *) &requestData->_firmwareinfo_in_file;
    if (chunk_start < len_crc_start) {
        datax -= chunk_start;       // datax now points to virtual "data[0]"
        datax += len_crc_start;     // datax now points to virtual "data[4122]"
    } else {
        lencrc -= len_crc_start;    // lencrc now points to virtual "lencrc[0]"
        lencrc += chunk_start;      // lencrc now points to virtual "lencrc[chunk_start]"
    }
    for (; lencrc < lencrc_end_p && datax < chunk_end_p; ) {
        *datax++ = *lencrc++;
    }
#endif
}


void WebserverUpdateClass::onUpload(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final)
{
    updateRequestData_t *requestData;
    if (index == 0) {
        request->_tempObject = new updateRequestData_t();
        if (request->_tempObject == nullptr) {
            return;
        }
        requestData = static_cast<updateRequestData_t*>(request->_tempObject);
        requestData->_md5sum[0] = 0;
        requestData->_md5.begin();
    } else if (request->_tempObject == nullptr) {
        return;
    } else {
        requestData = static_cast<updateRequestData_t*>(request->_tempObject);
    }

    //Upload handler chunks in data
    if (index == 0) {  // Start der Übertragung: index==0
        size_t content_len = 0;

        requestData->_uploadHttpCode = 200;
        requestData->_uploadError_pstr = PSTR("OK");
        requestData->_uploadFilename = filename;
        requestData->_expectedSize = 0xffffffff;

        LOGF_IP("Update started: %s", filename.c_str());
        if (filename.isEmpty()) {
            requestData->_uploadHttpCode = 500;
            requestData->_uploadError_pstr = PSTR("Invalid filename");
            LOGF_EP("%S", requestData->_uploadError_pstr);
            return;
        }

        /*
        // Disabled in the webinterface as file.size() in the webbrowser only
        // changes on new file selection which means
        // if the underlying file changes, file.size() keeps unchanged till new "selection"
        // We would have to redesign the Web-UI to get this working
        */
        if (!request->hasParam("SIZE", true)) {
            LOG_DP("No SIZE");
        } else {
            requestData->_expectedSize = std::stoul(request->getParam("SIZE", true)->value().c_str());
        }

        if (!request->hasParam("MD5", true)) {
            LOG_DP("No MD5");
        } else {
            strlcpy(requestData->_md5sum, request->getParam("MD5", true)->value().c_str(), sizeof(requestData->_md5sum));
        }

        if (filename.startsWith(F("firmware")) && filename.endsWith(F(".bin"))) {
            if (!Reboot.startUpdateFirmware()) {
                requestData->_uploadHttpCode = 500;
                requestData->_uploadError_pstr = PSTR("Upload already in progress");
                LOGF_EP("%S", requestData->_uploadError_pstr);
                return;
            }
            requestData->_uploadfiletype = firmware;
            requestData->_firmwareinfo_computed.length = 0;
            requestData->_firmwareinfo_computed.crc32 = 0xffffffff;
            requestData->_firmwareinfo_in_file.length = 0xffffffff;
            requestData->_firmwareinfo_in_file.crc32 = 0xffffffff;
            if (requestData->_expectedSize != 0xffffffff) {
                content_len = requestData->_expectedSize;
            } else {
                content_len = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
            }
        } else if (filename == F("littlefs.bin")) {
            if (!Reboot.startUpdateLittleFS()) {
                return;
            }
            requestData->_uploadfiletype = littlefs;
            content_len = ((size_t) &_FS_end - (size_t) &_FS_start);        // eigentlich Größe d. Flash-Partition
        } else {
            requestData->_uploadfiletype = anyOther;// anderes File
        }

        //eprintf("command: %d  content_len: %x  request: %x \n",cmd,content_len,request->contentLength());
        // SPIFFS-Partition: eprintf("_FS_start %x;  _FS_end %x; Size: %x\n",(size_t)&_FS_start,(size_t)&_FS_end,(size_t)&_FS_end-(size_t)&_FS_start);
        if (requestData->_uploadfiletype == firmware || requestData->_uploadfiletype == littlefs) {
            /*
            if (!request->hasParam("MD5", true)) {
                LOG_EP("No MD5");
                requestData->_uploadHttpCode = 500;
                requestData->_uploadError_pstr = PSTR("MD5 parameter missing.");
                return;
            }
            */

            Update.runAsync(true);
            if (!Update.begin(content_len, requestData->_uploadfiletype)) {
                LOGF_EP("Update-begin() failed: '%S'", Update.getErrorString().c_str());
                requestData->_uploadHttpCode = 500;
                requestData->_uploadError_pstr = PSTR("Update-begin() failed. See Log.");
                return;
            }

            if (requestData->_md5sum[0]) {
                if (!Update.setMD5(requestData->_md5sum)) {
                    LOG_EP("MD5 invalid");
                    requestData->_uploadHttpCode = 500;
                    requestData->_uploadError_pstr = PSTR("MD5 parameter invalid.");
                    return;
                }
            }
        } else {
            if (!requestData->_uploadFilename.startsWith("/")) {
                requestData->_uploadFilename = "/" + requestData->_uploadFilename;
            }
            if (requestData->_uploadFilename.equals(F("/monate"))) {
                requestData->_uploadfiletype = monate;
                AmisReader.disable();
            }
            request->_tempFile = LittleFS.open(requestData->_uploadFilename, "w");// Open the file for writing in LittleFS (create if it doesn't exist)
            if (!request->_tempFile) {
                if (requestData->_uploadfiletype == monate) {
                    historyInit();
                    AmisReader.enable();
                }
                LOGF_EP("Error creating file: %s", requestData->_uploadFilename.c_str());
                requestData->_uploadHttpCode = 500;
                requestData->_uploadError_pstr = PSTR("Could not create file");
                return;
            }
        }
    }       // index==0

    if (requestData->_uploadHttpCode != 200) {
        // exit if an error has occured
        return;
    }


    // Here comes the data

    if (requestData->_uploadfiletype == firmware || requestData->_uploadfiletype == littlefs) { // Update Flash
        if (!Update.hasError() && len) {
            if (Update.write(data, len) != len) {
                LOGF_EP("Error writing to flash: %s", requestData->_uploadFilename.c_str());
                requestData->_uploadHttpCode = 500;
                requestData->_uploadError_pstr = PSTR("Writing flash.");
                return;
            }
        }
        if (requestData->_uploadfiletype == firmware && ApplicationRuntime.updateFirmwareCheckCRC32()) {
            updateCrcFromChunk(request, data, index, len);
        }
    } else if (requestData->_uploadfiletype == anyOther || requestData->_uploadfiletype == monate) { // write "any other file" content
        // If we have a md5sum .. update it also for "normal" files
        if (requestData->_md5sum[0]) {
            for(size_t l = 0; l < len;) {
                uint16_t ll;
                if (len - l >= 0xffff) {
                    ll = 0xffff;
                } else {
                    ll = len - l;
                }
                requestData->_md5.add(data+l, ll);
                l += ll;
            }
        }

        if (request->_tempFile) {
            if (request->_tempFile.write(data, len) != len) {
                request->_tempFile.close();
                if (requestData->_uploadfiletype == monate) {
                    historyInit();
                    AmisReader.enable();
                }
                LOGF_EP("Error writing file: %s", requestData->_uploadFilename.c_str());
                requestData->_uploadHttpCode = 500;
                requestData->_uploadError_pstr = PSTR("Writing file.");
                return;
            }
        }
    }

    if (!final) {
        // get next chunk
        SYSTEMMONITOR_STAT();
        return;
    }

    // We get the last chunk of data ==> End of transmission
    if (request->_tempFile) {
        request->_tempFile.close();
        request->_tempFile = File();
    }
    if (requestData->_uploadfiletype == monate) {
        historyInit();
        AmisReader.enable();
    }

    // Validate filesize
    if (requestData->_expectedSize != 0xffffffff && requestData->_expectedSize != (index+len)) {
        LOGF_EP("Upload size mismatch. Expected:%u Received:%u",
                    requestData->_expectedSize, index+len);
        requestData->_uploadHttpCode = 500;
        requestData->_uploadError_pstr = PSTR("Upload size mismatch.");
        return;
    }

    // Validate md5 for "normal" files if we had one
    // For firmware and littlefs this is done within Update.end()
    if (requestData->_uploadfiletype == anyOther || requestData->_uploadfiletype == monate) { // write "any other file" content
        if (requestData->_md5sum[0]) {
            requestData->_md5.calculate();
            if (strcasecmp(requestData->_md5sum, requestData->_md5.toString().c_str())) {
                LOGF_EP("md5sum mismatch. Expected:%s Received:%s",
                            requestData->_md5sum, requestData->_md5.toString().c_str());
                requestData->_uploadHttpCode = 500;
                requestData->_uploadError_pstr = PSTR("md5sum mismatch.");
                return;
            }
        }
    }

    if (requestData->_uploadfiletype == firmware && ApplicationRuntime.updateFirmwareCheckCRC32()) {
        if (requestData->_firmwareinfo_computed.crc32 != requestData->_firmwareinfo_in_file.crc32 ||
            requestData->_firmwareinfo_computed.length != requestData->_firmwareinfo_in_file.length ||
            ( requestData->_expectedSize != 0xffffffff && requestData->_expectedSize != requestData->_firmwareinfo_in_file.length) ) {
            // be sure enforcing an error on UpdateEnd()
            Update.setMD5("--------------------------------");
            LOGF_EP("Invalid data: len,crc: %u, 0x%08x  computed: len,crc: %u, 0x%08x",
                        requestData->_firmwareinfo_in_file.length, requestData->_firmwareinfo_in_file.crc32,
                        requestData->_firmwareinfo_computed.length, requestData->_firmwareinfo_computed.crc32
                    );
        }
    }

    if (requestData->_uploadfiletype == firmware || requestData->_uploadfiletype == littlefs) {
        // Flash or LittleFS Update
        bool exactFileSize = (requestData->_expectedSize == 0xffffffff) ?false :true;
        if (Update.end(!exactFileSize)) {
            if (requestData->_uploadfiletype == firmware) {
                LOGF_IP("Update succes: %s (crc32=0x%08x)", filename.c_str(), requestData->_firmwareinfo_computed.crc32);
            } else {
                LOGF_IP("Update succes: %s", filename.c_str());
            }
        } else {
            LOGF_EP("Update failed (%s): '%S'", filename.c_str(), Update.getErrorString().c_str());
            requestData->_uploadHttpCode = 599; // 599 ... we're doing a reboot even on failure
            requestData->_uploadError_pstr = PSTR("Update-end() failed. See Log.");
        }

        // Reboot anyway (even on errors)
        if (requestData->_uploadfiletype == firmware) {
            Reboot.endUpdateFirmware();
        } else { // _uploadfiletype == littlefs
            Reboot.endUpdateLittleFS();
        }
    } else {
        LOGF_IP("Update succes: %s", filename.c_str());
    }
    SYSTEMMONITOR_STAT();
}

void WebserverUpdateClass::onUploadRequest(AsyncWebServerRequest* request)
{
    // This request handler is triggered after the upload has finished...
    // Here we have to send back the "result"

    if (request->_tempFile) {
        request->_tempFile.close();
        request->_tempFile = File();
    }
    if (request->_tempObject == nullptr) {
        return request->send_P(500, asyncsrv::T_text_plain, PSTR("Out of memory"));
    }

    updateRequestData_t *requestData = (updateRequestData_t *) request->_tempObject;
    request->send_P(requestData->_uploadHttpCode, asyncsrv::T_text_plain, requestData->_uploadError_pstr);

    delete static_cast<updateRequestData_t*>(request->_tempObject);
    request->_tempObject = nullptr;

    LOG_DP("WebserverUpdateClass::onUploadRequest()");
}

/* vim:set ts=4 et: */
