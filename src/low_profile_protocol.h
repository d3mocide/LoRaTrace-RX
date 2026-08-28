#pragma once
// Bounded, line-framed Serial Control protocol. Internal legacy opcode names
// remain stable so existing hosts can reconnect across the user-facing rename.
// This header stays free
// of Arduino/FreeRTOS so malformed-host input is testable on the native host.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

constexpr size_t LOW_PROFILE_FRAME_MAX = 160;

enum class LowProfileOpcode : uint8_t {
    INVALID = 0,
    HELLO,
    STATUS,
    TRACE_SET,
    PROFILE_SET,
    PROBE_START,
    PROBE_CANCEL,
    SWEEP_START,
    SWEEP_CANCEL,
    BENCH_FAULT,
    BENCH_CAD,
    LOW_PROFILE_OFF,
    ACK,
    ERROR,
};

struct LowProfileFrame {
    uint16_t sequence = 0;
    LowProfileOpcode opcode = LowProfileOpcode::INVALID;
    char argument[96] = {};
};

inline uint16_t lowProfileCrc16(const char *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint8_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

inline const char *lowProfileOpcodeName(LowProfileOpcode opcode) {
    switch (opcode) {
        case LowProfileOpcode::HELLO: return "HELLO";
        case LowProfileOpcode::STATUS: return "STATUS";
        case LowProfileOpcode::TRACE_SET: return "TRACE_SET";
        case LowProfileOpcode::PROFILE_SET: return "PROFILE_SET";
        case LowProfileOpcode::PROBE_START: return "PROBE_START";
        case LowProfileOpcode::PROBE_CANCEL: return "PROBE_CANCEL";
        case LowProfileOpcode::SWEEP_START: return "SWEEP_START";
        case LowProfileOpcode::SWEEP_CANCEL: return "SWEEP_CANCEL";
        case LowProfileOpcode::BENCH_FAULT: return "BENCH_FAULT";
        case LowProfileOpcode::BENCH_CAD: return "BENCH_CAD";
        case LowProfileOpcode::LOW_PROFILE_OFF: return "LOW_PROFILE_OFF";
        case LowProfileOpcode::ACK: return "ACK";
        case LowProfileOpcode::ERROR: return "ERROR";
        default: return "INVALID";
    }
}

inline LowProfileOpcode lowProfileOpcodeFromName(const char *name) {
    if (name == nullptr) return LowProfileOpcode::INVALID;
    if (strcmp(name, "HELLO") == 0) return LowProfileOpcode::HELLO;
    if (strcmp(name, "STATUS") == 0) return LowProfileOpcode::STATUS;
    if (strcmp(name, "TRACE_SET") == 0) return LowProfileOpcode::TRACE_SET;
    if (strcmp(name, "PROFILE_SET") == 0) return LowProfileOpcode::PROFILE_SET;
    if (strcmp(name, "PROBE_START") == 0) return LowProfileOpcode::PROBE_START;
    if (strcmp(name, "PROBE_CANCEL") == 0) return LowProfileOpcode::PROBE_CANCEL;
    if (strcmp(name, "SWEEP_START") == 0) return LowProfileOpcode::SWEEP_START;
    if (strcmp(name, "SWEEP_CANCEL") == 0) return LowProfileOpcode::SWEEP_CANCEL;
    if (strcmp(name, "BENCH_FAULT") == 0) return LowProfileOpcode::BENCH_FAULT;
    if (strcmp(name, "BENCH_CAD") == 0) return LowProfileOpcode::BENCH_CAD;
    if (strcmp(name, "LOW_PROFILE_OFF") == 0) return LowProfileOpcode::LOW_PROFILE_OFF;
    if (strcmp(name, "ACK") == 0) return LowProfileOpcode::ACK;
    if (strcmp(name, "ERROR") == 0) return LowProfileOpcode::ERROR;
    return LowProfileOpcode::INVALID;
}

inline bool lowProfileParseSequence(const char *text, uint16_t &value) {
    if (text == nullptr || text[0] == '\0') return false;
    uint32_t parsed = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        parsed = parsed * 10U + (uint32_t)(*p - '0');
        if (parsed > 65535U) return false;
    }
    value = (uint16_t)parsed;
    return true;
}

inline bool lowProfileParseHex16(const char *text, uint16_t &value) {
    if (text == nullptr || strlen(text) != 4) return false;
    uint16_t parsed = 0;
    for (const char *p = text; *p; ++p) {
        uint8_t nibble;
        if (*p >= '0' && *p <= '9') nibble = (uint8_t)(*p - '0');
        else if (*p >= 'A' && *p <= 'F') nibble = (uint8_t)(*p - 'A' + 10);
        else if (*p >= 'a' && *p <= 'f') nibble = (uint8_t)(*p - 'a' + 10);
        else return false;
        parsed = (uint16_t)((parsed << 4) | nibble);
    }
    value = parsed;
    return true;
}

// `line` is modified in place. It must be a NUL-terminated, bounded buffer.
inline bool lowProfileParseFrame(char *line, LowProfileFrame &out) {
    if (line == nullptr) return false;
    const size_t length = strlen(line);
    if (length == 0 || length >= LOW_PROFILE_FRAME_MAX) return false;
    if (length > 0 && line[length - 1] == '\r') line[length - 1] = '\0';

    char *crcSep = strrchr(line, ' ');
    if (crcSep == nullptr || crcSep == line || crcSep[1] == '\0') return false;
    uint16_t suppliedCrc = 0;
    if (!lowProfileParseHex16(crcSep + 1, suppliedCrc)) return false;
    *crcSep = '\0';
    if (lowProfileCrc16(line, strlen(line)) != suppliedCrc) return false;

    char *save = nullptr;
    char *marker = strtok_r(line, " ", &save);
    char *seq = strtok_r(nullptr, " ", &save);
    char *opcode = strtok_r(nullptr, " ", &save);
    char *argument = strtok_r(nullptr, " ", &save);
    if (marker == nullptr || seq == nullptr || opcode == nullptr || argument == nullptr ||
        strtok_r(nullptr, " ", &save) != nullptr || strcmp(marker, "@LTRX/1") != 0 ||
        !lowProfileParseSequence(seq, out.sequence)) {
        return false;
    }
    out.opcode = lowProfileOpcodeFromName(opcode);
    if (out.opcode == LowProfileOpcode::INVALID || strlen(argument) >= sizeof(out.argument)) {
        return false;
    }
    strcpy(out.argument, argument);
    return true;
}

inline size_t lowProfileFormatFrame(char *out, size_t outSize, uint16_t sequence,
                                    LowProfileOpcode opcode, const char *argument) {
    if (out == nullptr || outSize == 0 || opcode == LowProfileOpcode::INVALID ||
        argument == nullptr || strchr(argument, ' ') != nullptr) {
        return 0;
    }
    char body[LOW_PROFILE_FRAME_MAX] = {};
    const int bodyLen = snprintf(body, sizeof(body), "@LTRX/1 %u %s %s",
                                 (unsigned)sequence, lowProfileOpcodeName(opcode), argument);
    if (bodyLen < 0 || (size_t)bodyLen >= sizeof(body)) return 0;
    const int total = snprintf(out, outSize, "%s %04X", body,
                               (unsigned)lowProfileCrc16(body, (size_t)bodyLen));
    return total >= 0 && (size_t)total < outSize ? (size_t)total : 0;
}
