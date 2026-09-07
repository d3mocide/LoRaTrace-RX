#pragma once
// LoRaTrace RX — Phase 12 Focus Survey bounded RSSI statistics and durable
// row contract. No coverage label is produced until its thresholds are earned
// by the controlled matrix in phase12-survey-truth-design.md.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "detection.h"   // missionProfileName()
#include "focus_coverage.h"
#include "focus_plan.h"

constexpr int16_t FOCUS_RSSI_NO_SAMPLE_DBM_X10 = 32767;
constexpr int16_t FOCUS_RSSI_HISTOGRAM_MIN_DBM = -140;
constexpr int16_t FOCUS_RSSI_HISTOGRAM_MAX_DBM = 0;
constexpr uint16_t FOCUS_RSSI_HISTOGRAM_BUCKET_COUNT =
    (uint16_t)(FOCUS_RSSI_HISTOGRAM_MAX_DBM - FOCUS_RSSI_HISTOGRAM_MIN_DBM + 1);

constexpr uint8_t FOCUS_RSSI_FLAG_SAMPLE_OVERFLOW = 0x01;
constexpr uint8_t FOCUS_RSSI_FLAG_BUCKET_OVERFLOW = 0x02;
constexpr uint8_t FOCUS_RSSI_FLAG_UNDERFLOW = 0x04;
constexpr uint8_t FOCUS_RSSI_FLAG_OVERFLOW = 0x08;

// A fixed 1dB histogram is compact enough for one result while retaining no
// raw samples. Values beyond the documented receiver range are clamped to an
// endpoint and flagged, so summaries never pretend that the range is wider.
struct FocusRssiHistogram {
    uint8_t bucket_counts[FOCUS_RSSI_HISTOGRAM_BUCKET_COUNT] = {};
    uint16_t sample_count = 0;
    int16_t peak_dbm_x10 = FOCUS_RSSI_NO_SAMPLE_DBM_X10;
    uint8_t flags = 0;
};

static_assert(sizeof(FocusRssiHistogram) <= 160,
              "Focus histogram exceeds its one-result static SRAM budget");

inline int16_t focusRssiDbmX10ToNearestDbm(int16_t value_dbm_x10) {
    // C++ integer division truncates toward zero, so handle negative values
    // explicitly to retain half-away-from-zero 1dB binning.
    return value_dbm_x10 >= 0 ? (int16_t)((value_dbm_x10 + 5) / 10)
                              : (int16_t)((value_dbm_x10 - 5) / 10);
}

inline void focusHistogramAddSample(FocusRssiHistogram &histogram,
                                    int16_t rssi_dbm_x10) {
    if (histogram.sample_count == UINT16_MAX) {
        histogram.flags |= FOCUS_RSSI_FLAG_SAMPLE_OVERFLOW;
        return;
    }

    if (histogram.sample_count == 0 || rssi_dbm_x10 > histogram.peak_dbm_x10) {
        histogram.peak_dbm_x10 = rssi_dbm_x10;
    }
    histogram.sample_count++;

    int16_t dbm = focusRssiDbmX10ToNearestDbm(rssi_dbm_x10);
    if (dbm < FOCUS_RSSI_HISTOGRAM_MIN_DBM) {
        dbm = FOCUS_RSSI_HISTOGRAM_MIN_DBM;
        histogram.flags |= FOCUS_RSSI_FLAG_UNDERFLOW;
    } else if (dbm > FOCUS_RSSI_HISTOGRAM_MAX_DBM) {
        dbm = FOCUS_RSSI_HISTOGRAM_MAX_DBM;
        histogram.flags |= FOCUS_RSSI_FLAG_OVERFLOW;
    }

    const uint16_t index = (uint16_t)(dbm - FOCUS_RSSI_HISTOGRAM_MIN_DBM);
    if (histogram.bucket_counts[index] == UINT8_MAX) {
        histogram.flags |= FOCUS_RSSI_FLAG_BUCKET_OVERFLOW;
    } else {
        histogram.bucket_counts[index]++;
    }
}

constexpr bool focusHistogramHasExactQuantiles(const FocusRssiHistogram &histogram) {
    return histogram.sample_count > 0 &&
           (histogram.flags & (FOCUS_RSSI_FLAG_SAMPLE_OVERFLOW |
                               FOCUS_RSSI_FLAG_BUCKET_OVERFLOW)) == 0;
}

// Returns a 1dB-quantized tenths-of-dBm value, or NO_SAMPLE when the retained
// buckets cannot faithfully represent the count (or no sample was accepted).
inline int16_t focusHistogramPercentileDbmX10(const FocusRssiHistogram &histogram,
                                              uint8_t percentile) {
    if (!focusHistogramHasExactQuantiles(histogram) || percentile == 0 || percentile > 100) {
        return FOCUS_RSSI_NO_SAMPLE_DBM_X10;
    }
    const uint32_t rank = ((uint32_t)histogram.sample_count * percentile + 99u) / 100u;
    uint32_t cumulative = 0;
    for (uint16_t i = 0; i < FOCUS_RSSI_HISTOGRAM_BUCKET_COUNT; ++i) {
        cumulative += histogram.bucket_counts[i];
        if (cumulative >= rank) {
            return (int16_t)((FOCUS_RSSI_HISTOGRAM_MIN_DBM + (int16_t)i) * 10);
        }
    }
    return FOCUS_RSSI_NO_SAMPLE_DBM_X10;
}

inline int16_t focusHistogramMedianDbmX10(const FocusRssiHistogram &histogram) {
    return focusHistogramPercentileDbmX10(histogram, 50);
}

inline int16_t focusHistogramP90DbmX10(const FocusRssiHistogram &histogram) {
    return focusHistogramPercentileDbmX10(histogram, 90);
}

// How many accepted samples read at or above `threshold_dbm_x10`.
//
// A count is a different instrument from a summary statistic. `peak` is one
// sample and therefore noise-prone; a count integrates over the pass, which
// is what the 2026-09-05 field-level measurement suggested was missing when
// every summary statistic failed to separate a known source from ambient
// (docs/hardware-results/2026-09-04-phase12-focus-matrix.md). Pure, so the
// rule can be evaluated on the host before any of it is trusted on-device.
inline uint16_t focusHistogramCountAtLeast(const FocusRssiHistogram &histogram,
                                           int16_t threshold_dbm_x10) {
    const int16_t threshold_dbm = focusRssiDbmX10ToNearestDbm(threshold_dbm_x10);
    if (threshold_dbm > FOCUS_RSSI_HISTOGRAM_MAX_DBM) return 0;
    const int16_t lowest = threshold_dbm < FOCUS_RSSI_HISTOGRAM_MIN_DBM
                               ? FOCUS_RSSI_HISTOGRAM_MIN_DBM
                               : threshold_dbm;
    uint16_t total = 0;
    for (uint16_t i = (uint16_t)(lowest - FOCUS_RSSI_HISTOGRAM_MIN_DBM);
         i < FOCUS_RSSI_HISTOGRAM_BUCKET_COUNT; ++i) {
        total = (uint16_t)(total + histogram.bucket_counts[i]);
    }
    return total;
}

// The margin the `qualifying_count` column is measured at. 6 dB is the C6 of
// the phase-12 campaigns (docs/research/phase12-survey-truth-design.md): the
// one rule that separated source-on from source-off where every summary
// statistic failed. Recorded, never turned into an activity claim on device —
// it degrades exactly where the band is busiest.
constexpr int16_t FOCUS_QUALIFYING_MARGIN_DBM_X10 = 60;

// The same count expressed relative to the pass's own median, so the rule
// adapts to the ambient floor where it ran instead of assuming one. An
// absolute threshold was measured and rejected at field levels; this is the
// candidate that replaced it, and it is still a candidate.
inline uint16_t focusHistogramCountAboveMedian(const FocusRssiHistogram &histogram,
                                               int16_t margin_dbm_x10) {
    const int16_t median = focusHistogramMedianDbmX10(histogram);
    if (median == FOCUS_RSSI_NO_SAMPLE_DBM_X10) return 0;
    return focusHistogramCountAtLeast(histogram, (int16_t)(median + margin_dbm_x10));
}

enum class FocusRequestStatus : uint8_t {
    COMPLETE = 0,
    CANCELLED,
    TIMEOUT,
    FAILED,
};

inline const char *focusRequestStatusName(FocusRequestStatus status) {
    switch (status) {
        case FocusRequestStatus::COMPLETE: return "complete";
        case FocusRequestStatus::CANCELLED: return "cancelled";
        case FocusRequestStatus::TIMEOUT: return "timeout";
        case FocusRequestStatus::FAILED: return "failed";
        default: return "unknown";
    }
}

// Queue/log record. GPS/run fields stay formatter parameters so radio
// ownership remains bounded.
struct FocusObservation {
    uint32_t rx_millis = 0;
    uint32_t observation_ms = 0;
    // Sampling time that did not earn coverage credit. A cancelled or failed
    // pass used to report observation_ms 0 whatever it had actually sampled.
    uint32_t partial_observation_ms = 0;
    float freq_mhz = 0.0f;
    uint16_t focus_id = 0;
    uint16_t selection_bin_index = 0;
    uint16_t requested_dwell_ms = 0;
    uint16_t requested_samples = 0;
    uint16_t sample_count = 0;
    // Samples above this pass's own median by FOCUS_QUALIFYING_MARGIN_DBM_X10.
    // Zero until 2026-09-07 despite the column being documented as measured
    // (audit A19). Recorded
    // raw and deliberately unlabelled: a rule over this count was measured and
    // works, but its accuracy depends on link quality the device cannot know
    // and degrades where the band is busiest, so Focus reports coverage and
    // never activity (2026-09-06 decision, docs/research/
    // phase12-survey-truth-design.md §8). The number is here so a host with
    // ground truth can conclude what the device may not.
    uint16_t qualifying_count = 0;
    // Sample slots abandoned because the bus made them arrive more than a
    // whole slot late. They are skipped rather than taken back-to-back to
    // catch up, so `sample_count` describes samples that really were spaced
    // the way the row claims (audit A22).
    uint16_t skipped_samples = 0;
    int16_t rssi_median_dbm_x10 = FOCUS_RSSI_NO_SAMPLE_DBM_X10;
    int16_t rssi_p90_dbm_x10 = FOCUS_RSSI_NO_SAMPLE_DBM_X10;
    int16_t rssi_peak_dbm_x10 = FOCUS_RSSI_NO_SAMPLE_DBM_X10;
    int16_t radio_status = 0;
    uint8_t profile = 0; // MissionProfile
    uint8_t requested_passes = 0;
    uint8_t valid_passes = 0;
    // Coverage across repeated requests at this bin, not this request alone —
    // one row cannot know about earlier ones, so radio_task carries the
    // accumulator (focus_coverage.h) and stamps the resulting label here.
    FocusCoverageLabel coverage = FocusCoverageLabel::INSUFFICIENT;
    FocusSelectionSource selection_source = FocusSelectionSource::PRESET;
    FocusRequestStatus request_status = FocusRequestStatus::FAILED;
    bool home_restore = false;
    bool wifi_on = false;
};

static_assert(sizeof(FocusObservation) <= 48,
              "FocusObservation exceeds the Phase 12 one-result queue budget");
static_assert(sizeof(FocusRssiHistogram) + sizeof(FocusObservation) <= 224,
              "Focus working state must leave headroom below the 256B target");

// The logger's row buffer, defined here so the writer and its budget test
// cannot drift apart. An over-long row is dropped, not truncated
// (focusObservationFormatCsv() returns 0), so the margin is deliberate:
// measured worst case is 189 bytes with every field at its widest, including
// widths the bounded request cannot actually produce.
constexpr size_t FOCUS_CSV_ROW_MAX = 256;

constexpr const char *FOCUS_CSV_HEADER =
    "timestamp_utc,lat,lon,fix_quality,run,rx_uptime_ms,profile,focus_id,"
    "selection_source,selection_bin_index,freq_mhz,requested_passes,valid_passes,"
    "requested_dwell_ms,observation_ms,requested_samples,sample_count,"
    "rssi_median_dbm,rssi_p90_dbm,rssi_peak_dbm,qualifying_count,coverage,"
    "request_status,home_restore,wifi_on,radio_status,partial_observation_ms,"
    "skipped_samples";

inline void focusFormatRssiOrBlank(int16_t rssi_dbm_x10, char *out, size_t out_size) {
    if (rssi_dbm_x10 == FOCUS_RSSI_NO_SAMPLE_DBM_X10) {
        if (out_size > 0) out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "%.1f", (double)rssi_dbm_x10 / 10.0);
}

inline size_t focusObservationFormatCsv(const FocusObservation &observation,
                                        char *out, size_t out_size,
                                        const char *timestamp_utc, bool has_fix,
                                        double lat, double lon, uint8_t fix_quality,
                                        uint16_t run) {
    if (out == nullptr || out_size == 0) return 0;

    char latbuf[16], lonbuf[16], medianbuf[12], p90buf[12], peakbuf[12];
    if (has_fix) {
        snprintf(latbuf, sizeof(latbuf), "%.6f", lat);
        snprintf(lonbuf, sizeof(lonbuf), "%.6f", lon);
    } else {
        latbuf[0] = '\0';
        lonbuf[0] = '\0';
    }
    focusFormatRssiOrBlank(observation.rssi_median_dbm_x10, medianbuf, sizeof(medianbuf));
    focusFormatRssiOrBlank(observation.rssi_p90_dbm_x10, p90buf, sizeof(p90buf));
    focusFormatRssiOrBlank(observation.rssi_peak_dbm_x10, peakbuf, sizeof(peakbuf));

    // coverage is populated as of 2026-09-07 — the thresholds §3.1 required be
    // selected by measurement now are (focus_coverage.h). It was an empty field
    // with a doubled comma here until then, which was the honest state while
    // they were unmeasured.
    const int n = snprintf(
        out, out_size,
        "%s,%s,%s,%u,%u,%lu,%s,%u,%s,%u,%.3f,%u,%u,%u,%lu,%u,%u,%s,%s,%s,%u,%s,%s,%u,%u,%d,%lu,%u",
        timestamp_utc ? timestamp_utc : "", latbuf, lonbuf,
        (unsigned)fix_quality, (unsigned)run, (unsigned long)observation.rx_millis,
        missionProfileName(observation.profile), (unsigned)observation.focus_id,
        focusSelectionSourceName(observation.selection_source),
        (unsigned)observation.selection_bin_index, (double)observation.freq_mhz,
        (unsigned)observation.requested_passes, (unsigned)observation.valid_passes,
        (unsigned)observation.requested_dwell_ms, (unsigned long)observation.observation_ms,
        (unsigned)observation.requested_samples, (unsigned)observation.sample_count,
        medianbuf, p90buf, peakbuf, (unsigned)observation.qualifying_count,
        focusCoverageLabelName(observation.coverage),
        focusRequestStatusName(observation.request_status),
        (unsigned)observation.home_restore, (unsigned)observation.wifi_on,
        (int)observation.radio_status, (unsigned long)observation.partial_observation_ms,
        (unsigned)observation.skipped_samples);

    if (n < 0 || (size_t)n >= out_size) return 0;
    return (size_t)n;
}
