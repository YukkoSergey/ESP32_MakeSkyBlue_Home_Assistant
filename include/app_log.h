#ifndef APP_LOG_H
#define APP_LOG_H

#include <Arduino.h>
#include "compat.h"

// -----------------------------------------------------------------------------
// Log sink: everything goes to Serial and into a RAM ring buffer, so the web
// debug UI can show the log without a USB cable. Thread-safe — the Tuya task
// and the main loop both log. Each line in the ring buffer starts with the
// local time (or uptime before NTP), so events can be lined up afterwards.
//
// Positions are monotonic byte offsets (total bytes ever written). A reader
// keeps the last position it saw and asks for everything after it.
// -----------------------------------------------------------------------------
class AppLog : public Print {
public:
#if defined(ESP8266)
    static constexpr size_t kBufSize = 6 * 1024;    // ~45 KB RAM in total there
#else
    static constexpr size_t kBufSize = 16 * 1024;
#endif

    void begin();
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* data, size_t len) override;

    // Appends to `out` everything written after position `since` (or as much
    // of it as is still in the buffer). Returns the current end position.
    uint32_t readSince(uint32_t since, String& out);

private:
    void put(char c);

    char              _buf[kBufSize];
    uint32_t          _total = 0;
    bool              _lineStart = true;
    CompatMutex       _mutex;
};

extern AppLog Log;

#endif
