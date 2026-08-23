#pragma once
// LoRaTrace RX — the detection record that crosses cores, plus the pure
// helpers that fill and format it.
//
// This struct is the sole payload of the Core 1 -> Core 0 FreeRTOS queue
// (DESIGN.md §2). It is deliberately small and deliberately GPS-free:
// DESIGN.md §1 budgets ~40B/entry and names SD (not RAM) as the datastore,
// and §2 makes the *logger* responsible for stamping the GPS fix at dequeue
// time. Keeping GPS out of here also keeps the radio task away from the GPS
// mutex, which is the whole point of the split — the radio task must never
// block on anything another task owns.
//
// Everything in this header is pure logic so it can be exercised by
// `pio test -e native`.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "channel_plans.h"

// Radio-side record of one received packet. Field order groups the 4-byte
// members first so the struct packs tightly without padding holes.
struct Detection {
    uint32_t rx_millis;  // device uptime at RX; written to the log as
                         // rx_uptime_ms. Lets post-processing spot a stale
                         // GPS stamp caused by queue backlog, joins a row to
                         // session.csv, and is the ONLY time reference a
                         // detection heard before the first fix has
    uint32_t node_id;    // protocol sender id (Meshtastic `from`); 0 = unknown
    uint32_t packet_id;  // protocol packet id; 0 = unknown. With node_id this
                         // is the dedupe key that separates an original
                         // transmission from its mesh rebroadcasts
    float freq_mhz;
    float rssi_dbm;
    float snr_db;

    uint16_t raw_len;
    uint16_t bw_khz_x10; // bandwidth * 10, so 125.0kHz fits an integer

    uint8_t sf;
    uint8_t cr_denom;
    uint8_t sync_word;
    uint8_t profile;      // MissionProfile
    uint8_t channel_hash; // Meshtastic GGA-style channel hint; 0 = unknown
    uint8_t hop_limit;    // remaining hops; with hop_start gives hops taken
    uint8_t hop_start;
    uint8_t relay_node;   // last byte of the NodeNum that relayed this copy
};

// DESIGN.md §1 budgets ~40B per queue entry. Assert it rather than trust it:
// this struct is easy to grow thoughtlessly, and on a no-PSRAM part the
// queue depth times this number is real memory.
static_assert(sizeof(Detection) <= 40, "Detection exceeds the ~40B queue budget (DESIGN.md §1)");

// --- Meshtastic packet header ------------------------------------------
//
// Layout VERIFIED against upstream source (meshtastic/firmware
// `src/mesh/RadioInterface.h`, `PacketHeader`):
//
//   uint32 to; uint32 from; uint32 id;   // little-endian on the wire
//   uint8 flags; uint8 channel; uint8 next_hop; uint8 relay_node;
//
// and `flags` carries hop_limit in the bottom 3 bits and hop_start in bits
// 5-7 (RadioLibInterface). Confirmed empirically on 2026-08-23 against live
// traffic: broadcast frames decoded with to == 0xFFFFFFFF, a consistent
// channel hash, hop_start 7, and original/rebroadcast pairs differing only
// in hop_limit and relay_node. See PROGRESS.md for that capture.
//
// This is header metadata only — the payload after these 16 bytes is
// encrypted and stays that way. Reading routing metadata off the air is the
// entire point of a wardriving receiver; decryption is not attempted.
constexpr size_t MESHTASTIC_HEADER_LEN = 16;
constexpr uint32_t MESHTASTIC_BROADCAST_ADDR = 0xFFFFFFFFu;

inline uint32_t readLE32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Fills the protocol-specific fields of `det` from a raw Meshtastic frame.
// Returns false (leaving those fields zeroed) when the buffer is too short
// to hold a header — a runt frame is not a reason to publish garbage node
// ids into the log.
inline bool detectionApplyMeshtasticHeader(Detection &det, const uint8_t *buf, size_t len) {
    det.node_id = 0;
    det.packet_id = 0;
    det.channel_hash = 0;
    det.hop_limit = 0;
    det.hop_start = 0;
    det.relay_node = 0;
    if (buf == nullptr || len < MESHTASTIC_HEADER_LEN) return false;

    det.node_id = readLE32(buf + 4);   // `from`
    det.packet_id = readLE32(buf + 8); // `id`
    const uint8_t flags = buf[12];
    det.hop_limit = (uint8_t)(flags & 0x07);
    det.hop_start = (uint8_t)((flags >> 5) & 0x07);
    det.channel_hash = buf[13];
    det.relay_node = buf[15];
    return true;
}

// True when the frame's destination is the Meshtastic broadcast address.
inline bool meshtasticIsBroadcast(const uint8_t *buf, size_t len) {
    if (buf == nullptr || len < MESHTASTIC_HEADER_LEN) return false;
    return readLE32(buf) == MESHTASTIC_BROADCAST_ADDR;
}

// --- Log formatting -----------------------------------------------------

inline const char *missionProfileName(uint8_t profile) {
    switch ((MissionProfile)profile) {
        case MissionProfile::MESHTASTIC: return "meshtastic";
        case MissionProfile::MESHCORE: return "meshcore";
        case MissionProfile::RETICULUM: return "reticulum";
        case MissionProfile::GENERAL_EXPLORATION: return "general";
        default: return "unknown";
    }
}

// DESIGN.md §8 column order. Kept as one string so the header row and the
// row writer can never drift apart.
constexpr const char *LOG_CSV_HEADER =
    "timestamp_utc,lat,lon,fix_quality,profile,freq_mhz,sf,bw_khz,"
    "rssi_dbm,snr_db,classification,decoded,channel_or_node_id,raw_len,"
    "rx_uptime_ms,run";

// Phase 2 placeholder for DESIGN.md §6 fingerprinting (phase 4): with
// HOME_LISTEN locked to one profile's channel, "what we were listening for"
// is the only honest classification available. Real post-hoc classification
// — which needs the sweep data phases 4/5 produce — replaces this via
// fingerprint.h. Named as a placeholder so a later reader doesn't mistake
// it for a finished classifier.
inline const char *detectionClassification(const Detection &det) {
    return missionProfileName(det.profile);
}

// Renders one CSV row per DESIGN.md §8 into `out`. `fix_*` come from the
// logger's GPS stamp; pass has_fix=false when no usable fix was available,
// which writes empty lat/lon rather than 0,0 (Null Island is a real place
// and would silently corrupt a track).
//
// Returns the number of characters written (excluding NUL), or 0 on
// truncation/failure — callers should treat 0 as "don't write this line".
inline size_t detectionFormatCsv(const Detection &det, char *out, size_t outSize,
                                 const char *timestamp_utc, bool has_fix, double lat, double lon,
                                 uint8_t fix_quality, uint16_t run) {
    if (out == nullptr || outSize == 0) return 0;

    char idbuf[24];
    if (det.node_id != 0) {
        // '!'-prefixed lowercase hex is Meshtastic's own node-id convention,
        // so these are greppable against a node list without conversion.
        snprintf(idbuf, sizeof(idbuf), "!%08lx", (unsigned long)det.node_id);
    } else {
        idbuf[0] = '\0';
    }

    char latbuf[16], lonbuf[16];
    if (has_fix) {
        // 6 decimal places is ~0.1m at the equator — far finer than GPS
        // accuracy, and enough that rounding never limits the track.
        snprintf(latbuf, sizeof(latbuf), "%.6f", lat);
        snprintf(lonbuf, sizeof(lonbuf), "%.6f", lon);
    } else {
        latbuf[0] = '\0';
        lonbuf[0] = '\0';
    }

    int n = snprintf(out, outSize,
                     "%s,%s,%s,%u,%s,%.3f,%u,%.1f,%.1f,%.2f,%s,%s,%s,%u,%lu,%u",
                     timestamp_utc ? timestamp_utc : "",
                     latbuf, lonbuf,
                     (unsigned)fix_quality,
                     missionProfileName(det.profile),
                     (double)det.freq_mhz,
                     (unsigned)det.sf,
                     (double)det.bw_khz_x10 / 10.0,
                     (double)det.rssi_dbm,
                     (double)det.snr_db,
                     detectionClassification(det),
                     "", // `decoded`: nothing is decrypted in Phase 2 (and
                         // Meshtastic/MeshCore payloads are encrypted) —
                         // column reserved by DESIGN.md §8, left empty
                     idbuf,
                     (unsigned)det.raw_len,
                     // Device uptime at RX. Appended last so existing
                     // parsers keep working. Without it a detection heard
                     // before the first GPS fix has an empty timestamp AND
                     // empty coordinates — nothing at all to place it in
                     // time, not even an ordering against the health log.
                     // This is also what rx_millis was captured for in the
                     // first place; it was crossing the queue and then
                     // being dropped at format time.
                     (unsigned long)det.rx_millis,
                     // Run index. Redundant with the directory the file
                     // sits in, right up until someone concatenates several
                     // runs for analysis — at which point every row's
                     // rx_uptime_ms has restarted at zero and, without this,
                     // the merged data is silently ambiguous about which
                     // drive a packet came from.
                     (unsigned)run);

    if (n < 0 || (size_t)n >= outSize) return 0; // truncated — drop the row
    return (size_t)n;
}

// Formats the GPS fix as ISO-8601 UTC. Writes an empty string when the fix
// has no date, which is the common case indoors and during the first
// seconds of a cold start.
inline void detectionFormatTimestamp(char *out, size_t outSize, bool has_time, uint16_t year,
                                     uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                     uint8_t second) {
    if (out == nullptr || outSize == 0) return;
    if (!has_time) {
        out[0] = '\0';
        return;
    }
    snprintf(out, outSize, "%04u-%02u-%02uT%02u:%02u:%02uZ", (unsigned)year, (unsigned)month,
             (unsigned)day, (unsigned)hour, (unsigned)minute, (unsigned)second);
}
