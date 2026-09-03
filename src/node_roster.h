#pragma once
// LoRaTrace RX — Phase 10 Field Analyzer, Passive Nodes view storage.
//
// docs/research/LoRaTrace-Phases-7-10-Design.md §8.2: "cleartext node ID,
// last seen, packet count, best/latest RSSI and SNR, hop metadata if
// already available. Fixed roster only. Human-readable names, chat, and
// position are excluded because those generally require encrypted payload
// access." §8.6 requires deterministic eviction — every tie in this file
// resolves by lowest slot index, never by iteration/hardware timing.

#include <stdint.h>

// §8.3.
constexpr uint8_t NODE_ROSTER_MAX_ENTRIES = 24;
// Detection.node_id's own "unknown" sentinel (detection.h) — reused here so
// an unidentified sender can never occupy a roster slot.
constexpr uint32_t NODE_ROSTER_EMPTY_ID = 0;

struct NodeRosterEntry {
    uint32_t node_id = NODE_ROSTER_EMPTY_ID;
    uint32_t last_seen_millis = 0;
    uint32_t packet_count = 0;
    float best_rssi_dbm = 0.0f;
    float latest_rssi_dbm = 0.0f;
    float latest_snr_db = 0.0f;
    uint8_t profile = 0; // MissionProfile
    uint8_t hop_limit = 0;
    uint8_t hop_start = 0;
    bool has_hop_metadata = false;
};

struct NodeRoster {
    NodeRosterEntry entries[NODE_ROSTER_MAX_ENTRIES];
};

// Existing entry for node_id, else an empty slot, else the
// least-recently-seen slot (LRU eviction, §8.3). Ties go to the lowest
// index so the result never depends on scan order.
inline uint8_t nodeRosterFindSlot(const NodeRoster &roster, uint32_t node_id) {
    for (uint8_t i = 0; i < NODE_ROSTER_MAX_ENTRIES; i++) {
        if (roster.entries[i].node_id == node_id) return i;
    }
    for (uint8_t i = 0; i < NODE_ROSTER_MAX_ENTRIES; i++) {
        if (roster.entries[i].node_id == NODE_ROSTER_EMPTY_ID) return i;
    }
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < NODE_ROSTER_MAX_ENTRIES; i++) {
        if (roster.entries[i].last_seen_millis < roster.entries[oldest].last_seen_millis) oldest = i;
    }
    return oldest;
}

// Refuses node_id == 0 (detection.h's own "unknown" sentinel) rather than
// creating a roster entry nothing could ever look back up. hop_limit/
// hop_start only overwrite when has_hop_metadata is true, since not every
// caller (e.g. a MeshCore Detection, whose header isn't parsed per
// CLAUDE.md's house rule) has them.
inline bool nodeRosterUpdate(NodeRoster &roster, uint32_t node_id, uint32_t now_millis,
                              float rssi_dbm, float snr_db, uint8_t profile,
                              bool has_hop_metadata, uint8_t hop_limit, uint8_t hop_start) {
    if (node_id == NODE_ROSTER_EMPTY_ID) return false;
    const uint8_t slot = nodeRosterFindSlot(roster, node_id);
    NodeRosterEntry &entry = roster.entries[slot];
    const bool isNew = entry.node_id != node_id;

    entry.node_id = node_id;
    entry.last_seen_millis = now_millis;
    entry.packet_count = isNew ? 1 : entry.packet_count + 1;
    entry.latest_rssi_dbm = rssi_dbm;
    entry.latest_snr_db = snr_db;
    entry.best_rssi_dbm = (isNew || rssi_dbm > entry.best_rssi_dbm) ? rssi_dbm : entry.best_rssi_dbm;
    entry.profile = profile;
    if (isNew) {
        entry.has_hop_metadata = false;
        entry.hop_limit = 0;
        entry.hop_start = 0;
    }
    if (has_hop_metadata) {
        entry.hop_limit = hop_limit;
        entry.hop_start = hop_start;
        entry.has_hop_metadata = true;
    }
    return true;
}
