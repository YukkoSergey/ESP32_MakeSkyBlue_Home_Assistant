#ifndef SM_CONFIG_H
#define SM_CONFIG_H

#include <Arduino.h>

struct SmSegment {
    uint8_t slaveId;
    uint8_t function;
    uint16_t startAddress;
    uint16_t length;
};

#endif
