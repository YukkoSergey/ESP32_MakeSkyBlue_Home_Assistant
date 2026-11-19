#include "app_log.h"
#include <time.h>

AppLog Log;

void AppLog::begin() {
    _mutex.init();
}

void AppLog::put(char c) {
    _buf[_total % kBufSize] = c;
    _total++;
}

size_t AppLog::write(const uint8_t* data, size_t len) {
    _mutex.lock();
    Serial.write(data, len);
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r') continue;   // keep the web view clean
        if (_lineStart && c != '\n') {
            char ts[24];
            time_t now = time(nullptr);
            if (now > 1700000000) {
                struct tm t;
                localtime_r(&now, &t);
                strftime(ts, sizeof(ts), "%d.%m %H:%M:%S ", &t);
            } else {
                snprintf(ts, sizeof(ts), "+%lus ", (unsigned long)(millis() / 1000));
            }
            for (const char* p = ts; *p; p++) put(*p);
            _lineStart = false;
        }
        put(c);
        if (c == '\n') _lineStart = true;
    }
    _mutex.unlock();
    return len;
}

uint32_t AppLog::readSince(uint32_t since, String& out) {
    _mutex.lock();
    uint32_t end = _total;
    uint32_t start = since;
    if (start > end) start = 0;                       // board rebooted
    if (end - start > kBufSize) start = end - kBufSize;
    out.reserve(out.length() + (end - start));
    for (uint32_t p = start; p < end; p++) out += _buf[p % kBufSize];
    _mutex.unlock();
    return end;
}
