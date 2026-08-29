#pragma once
// Bounded, line-framed Serial Control protocol. Internal legacy opcode names
// remain stable so existing hosts can reconnect across the user-facing rename.
// This header stays free
// of Arduino/FreeRTOS so malformed-host input is testable on the native host.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

constexpr size_t SERIAL_CONTROL_FRAME_MAX = 160;

// Enum value names below are the actual wire opcode strings (via
// serialControlOpcodeName()/serialControlOpcodeFromName()) and stay stable
// for host compatibility even where they still read "LOW_PROFILE" — see this
// file's own top comment. Only the surrounding C++ type/function names were
// renamed to Serial Control.
enum class SerialControlOpcode : uint8_t {
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
    BENCH_SWEEP_MARGIN,
    SD_RETRY,
    LOW_PROFILE_OFF,
    ACK,
    ERROR,
};

struct SerialControlFrame {
    uint16_t sequence = 0;
    SerialControlOpcode opcode = SerialControlOpcode::INVALID;
    char argument[96] = {};
};

inline uint16_t serialControlCrc16(const char *data, size_t length) {
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

inline const char *serialControlOpcodeName(SerialControlOpcode opcode) {
    switch (opcode) {
        case SerialControlOpcode::HELLO: return "HELLO";
        case SerialControlOpcode::STATUS: return "STATUS";
        case SerialControlOpcode::TRACE_SET: return "TRACE_SET";
        case SerialControlOpcode::PROFILE_SET: return "PROFILE_SET";
        case SerialControlOpcode::PROBE_START: return "PROBE_START";
        case SerialControlOpcode::PROBE_CANCEL: return "PROBE_CANCEL";
        case SerialControlOpcode::SWEEP_START: return "SWEEP_START";
        case SerialControlOpcode::SWEEP_CANCEL: return "SWEEP_CANCEL";
        case SerialControlOpcode::BENCH_FAULT: return "BENCH_FAULT";
        case SerialControlOpcode::BENCH_CAD: return "BENCH_CAD";
        case SerialControlOpcode::BENCH_SWEEP_MARGIN: return "BENCH_SWEEP_MARGIN";
        case SerialControlOpcode::SD_RETRY: return "SD_RETRY";
        case SerialControlOpcode::LOW_PROFILE_OFF: return "LOW_PROFILE_OFF";
        case SerialControlOpcode::ACK: return "ACK";
        case SerialControlOpcode::ERROR: return "ERROR";
        default: return "INVALID";
    }
}

inline SerialControlOpcode serialControlOpcodeFromName(const char *name) {
    if (name == nullptr) return SerialControlOpcode::INVALID;
    if (strcmp(name, "HELLO") == 0) return SerialControlOpcode::HELLO;
    if (strcmp(name, "STATUS") == 0) return SerialControlOpcode::STATUS;
    if (strcmp(name, "TRACE_SET") == 0) return SerialControlOpcode::TRACE_SET;
    if (strcmp(name, "PROFILE_SET") == 0) return SerialControlOpcode::PROFILE_SET;
    if (strcmp(name, "PROBE_START") == 0) return SerialControlOpcode::PROBE_START;
    if (strcmp(name, "PROBE_CANCEL") == 0) return SerialControlOpcode::PROBE_CANCEL;
    if (strcmp(name, "SWEEP_START") == 0) return SerialControlOpcode::SWEEP_START;
    if (strcmp(name, "SWEEP_CANCEL") == 0) return SerialControlOpcode::SWEEP_CANCEL;
    if (strcmp(name, "BENCH_FAULT") == 0) return SerialControlOpcode::BENCH_FAULT;
    if (strcmp(name, "BENCH_CAD") == 0) return SerialControlOpcode::BENCH_CAD;
    if (strcmp(name, "BENCH_SWEEP_MARGIN") == 0) return SerialControlOpcode::BENCH_SWEEP_MARGIN;
    if (strcmp(name, "SD_RETRY") == 0) return SerialControlOpcode::SD_RETRY;
    if (strcmp(name, "LOW_PROFILE_OFF") == 0) return SerialControlOpcode::LOW_PROFILE_OFF;
    if (strcmp(name, "ACK") == 0) return SerialControlOpcode::ACK;
    if (strcmp(name, "ERROR") == 0) return SerialControlOpcode::ERROR;
    return SerialControlOpcode::INVALID;
}

inline bool serialControlParseSequence(const char *text, uint16_t &value) {
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

inline bool serialControlParseHex16(const char *text, uint16_t &value) {
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
inline bool serialControlParseFrame(char *line, SerialControlFrame &out) {
    if (line == nullptr) return false;
    const size_t length = strlen(line);
    if (length == 0 || length >= SERIAL_CONTROL_FRAME_MAX) return false;
    if (length > 0 && line[length - 1] == '\r') line[length - 1] = '\0';

    char *crcSep = strrchr(line, ' ');
    if (crcSep == nullptr || crcSep == line || crcSep[1] == '\0') return false;
    uint16_t suppliedCrc = 0;
    if (!serialControlParseHex16(crcSep + 1, suppliedCrc)) return false;
    *crcSep = '\0';
    if (serialControlCrc16(line, strlen(line)) != suppliedCrc) return false;

    char *save = nullptr;
    char *marker = strtok_r(line, " ", &save);
    char *seq = strtok_r(nullptr, " ", &save);
    char *opcode = strtok_r(nullptr, " ", &save);
    char *argument = strtok_r(nullptr, " ", &save);
    if (marker == nullptr || seq == nullptr || opcode == nullptr || argument == nullptr ||
        strtok_r(nullptr, " ", &save) != nullptr || strcmp(marker, "@LTRX/1") != 0 ||
        !serialControlParseSequence(seq, out.sequence)) {
        return false;
    }
    out.opcode = serialControlOpcodeFromName(opcode);
    if (out.opcode == SerialControlOpcode::INVALID || strlen(argument) >= sizeof(out.argument)) {
        return false;
    }
    strcpy(out.argument, argument);
    return true;
}

inline size_t serialControlFormatFrame(char *out, size_t outSize, uint16_t sequence,
                                       SerialControlOpcode opcode, const char *argument) {
    if (out == nullptr || outSize == 0 || opcode == SerialControlOpcode::INVALID ||
        argument == nullptr || strchr(argument, ' ') != nullptr) {
        return 0;
    }
    char body[SERIAL_CONTROL_FRAME_MAX] = {};
    const int bodyLen = snprintf(body, sizeof(body), "@LTRX/1 %u %s %s",
                                 (unsigned)sequence, serialControlOpcodeName(opcode), argument);
    if (bodyLen < 0 || (size_t)bodyLen >= sizeof(body)) return 0;
    const int total = snprintf(out, outSize, "%s %04X", body,
                               (unsigned)serialControlCrc16(body, (size_t)bodyLen));
    return total >= 0 && (size_t)total < outSize ? (size_t)total : 0;
}
