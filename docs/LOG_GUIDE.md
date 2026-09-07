# LoRaTrace operator log guide

Each power-on creates one wardrive under `/loratrace/runNNNN/`. Treat that
directory as the unit of collection: copy or archive the whole folder, not
individual CSV rows. The run number is chosen before GPS time is available,
so it is an index rather than a date.

## Before you share a run

`detections.csv`, `nodes.csv`, and the optional scan files can contain precise
GPS coordinates, radio identifiers, names, public keys, and raw frames. Keep
the original private unless every affected operator has agreed to publication.
For a public summary, remove `lat`, `lon`, `timestamp_utc`, `raw_packet_hex`,
and identity fields first; do not rely on rounding alone to anonymize a route.

Always use each file's header rather than a fixed column position. Schemas are
append-only where practical, and older firmware runs may have fewer fields.

## Run contents

| File | Written when | What it answers |
| --- | --- | --- |
| `detections.csv` | Every received packet | What was heard, where/when, and RF quality. |
| `nodes.csv` | A supported identity advertisement/NodeInfo is decoded | Which identifiable nodes were observed. |
| `session.csv` | Boot and once per minute | Did the receiver, GPS, queue, and SD logger stay healthy? |
| `probe.csv` | A Probe is run | Which fixed-candidate channels produced CAD activity. A CAD hit is not necessarily a packet. |
| `energy.csv` | An Energy Sweep is run | Sparse high-energy bins and follow-up CAD results. Not a full spectrum recording. |
| `focus.csv` | A Focus survey is run | How one frequency bin behaved over a longer dwell than a Sweep gives it. Reports what was observed; it does not conclude that anything transmitted. |

`probe.csv`, `energy.csv` and `focus.csv` may exist with only a header when the
feature was not used. Their absence is not an error on older runs.

## Fields shared by observation files

`timestamp_utc` is the GPS UTC time at logger dequeue. It is blank before the
receiver has both a usable position and date/time. `rx_uptime_ms` is the
device uptime at radio reception and is the reliable ordering key during that
startup window.

`lat` and `lon` are blank rather than `0,0` when there was no fresh position.
`fix_quality` is the GPS receiver's GGA fix-quality value: `0` means no fix;
`1` is a normal GPS fix; `2` is DGPS. `run` repeats the directory number so
multiple runs can be combined safely.

### RSSI comparability across firmware versions

`energy.csv`'s `rssi_avg_dbm` and `rssi_peak_dbm` read about **2.4 dB lower
from `v1.0.2` onward** than they did before it. That release replaced Sweep's
per-bin full `radio.begin()` with a lighter retune, which measurably shifts
the reported noise floor: measured at -120.30 dBm before and -122.70 dBm
after, reproduced identically across five independent bench runs
(`docs/research/2026-09-04-project-audit.md`, M6).

This does **not** affect which bins are flagged as peaks. Pass A's test is
`peak >= floor + margin`, and peak and floor are measured the same way in the
same sweep, so a uniform shift cancels — the 35.0 dB margin still means what
it was calibrated to mean.

It does matter if you compare absolute RSSI values across that boundary:
runs logged before and after `v1.0.2` are on slightly different scales, so
subtract the offset or compare within one firmware version. `detections.csv`
is unaffected — packet RSSI is read from the radio's own packet status, not
from Sweep's per-bin sampling.

## `detections.csv`: packet observations

The current header is:

```text
timestamp_utc,lat,lon,fix_quality,run,rx_uptime_ms,profile,classification,channel_or_node_id,packet_id,hop_limit,hop_start,relay_node,freq_mhz,sf,bw_khz,rssi_dbm,snr_db,raw_len,raw_packet_hex,decoded
```

The most useful common fields are `profile`, `freq_mhz`, `sf`, `bw_khz`,
`rssi_dbm`, `snr_db`, and `raw_len`. `classification` describes the listening
mode, not a cryptographically proven protocol identity. `raw_packet_hex` is
the exact received frame and `decoded` is intentionally blank unless a future
payload decoder has a verified schema.

### Meshtastic routing fields

For Meshtastic, `channel_or_node_id` is the sender's `!`-prefixed node ID and
`packet_id` is the packet ID in hexadecimal. The same `packet_id` may appear
more than once because a mesh relay retransmits it. Read the fields together:

- Same sender + same `packet_id` + changed `hop_limit` or `relay_node` is
  usually a direct/relay observation pair, not an accidental duplicate.
- `hop_start - hop_limit` is the hops consumed when both values are present.
- `relay_node` is the final one-byte relay hint carried by the Meshtastic
  header, not a complete verified route.

### MeshCore and other profiles

`channel_or_node_id`, `packet_id`, `hop_limit`, `hop_start`, and `relay_node`
are Meshtastic header fields. MeshCore does not use that header, so those
columns are blank or zero for MeshCore rows. Use the RF fields and
`raw_packet_hex` for those observations; identity advertisements appear in
`nodes.csv` separately. Reticulum and General Exploration are likewise raw
packet observations unless their framing has been explicitly identified.

## `nodes.csv`: identity observations

The current header is:

```text
timestamp_utc,lat,lon,fix_quality,run,rx_uptime_ms,profile,node_id,node_type,long_name,short_name,public_key_hex,raw_len
```

This is an observation table, not a de-duplicated inventory. A node can appear
many times as it advertises or is heard through a relay. For an inventory,
group by `profile` and `public_key_hex`, retaining the newest non-empty name
and the latest location only when you are authorized to retain it.

| Profile | `node_id` | Names and public key |
| --- | --- | --- |
| Meshtastic | Full `!`-prefixed sender node ID | `long_name`, `short_name`, and 32-byte public key from a decoded public-default-channel NodeInfo. |
| MeshCore | `#` plus the one-byte public-key hash | Advert name in `long_name`; `node_type` is `chat`, `repeater`, `room_server`, or `sensor`; full 32-byte Ed25519 public key is present. |

Identity capture starts enabled and can be paused at `Diagnostics ->
Identities`. Meshtastic private/custom-channel NodeInfo remains raw-only; the
firmware only decrypts the published default public channel. MeshCore
advertisements are signed, but LoRaTrace records the signed observation and
does not claim on-device Ed25519 signature verification.

Three columns added 2026-09-07 (audit A13) separate what the radio was
configured for from what was actually established about the bytes:

- `classification` is unchanged and is **the listening configuration** — the
  mission profile in use. It is not an identification. Reticulum and General
  Exploration fall back to the Meshtastic channel tuple, so traffic heard
  under them could read `reticulum` with no Reticulum parser involved.
- `protocol_candidate` is evidence-based: `unknown` until a parser actually
  got somewhere, then `meshtastic_header` or `meshcore_advert`, or
  `unknown_lora_candidate` for an off-grid Pass-B hit.
- `parse_status` is `none`, `header`, or `identity` — how far parsing got.
- `auth_status` is always `unauthenticated`, and that is the point. MeshCore
  adverts carry an Ed25519 signature this firmware does not verify, and
  decrypting with a published default PSK proves possession of a public key,
  not a sender's identity. Treat every node id, name and key in these files
  as an **observed claim**.

`manifest.txt` is downloadable alongside the CSVs over the AP.

## `session.csv`: health evidence

Use this file before drawing conclusions from a drive. A `reason=boot` row
marks the run start; later rows are `periodic`.

The first health checks are:

- `sd=ok`, `queue_drop=0`, and `row_drop=0`: packets reached durable logging
  without the known queue/logger loss modes.
- `bus_miss=0`: the radio did not time out waiting for the shared SPI bus.
- `identities_decoded` and `identity_drops`: identity pipeline activity and
  loss since boot. A nonzero decode count with zero drops is the desired
  result.
- `heap_free`, `heap_min`, `heap_largest`, and block counts: look for a
  sustained trend, not one isolated sample.
- `gps_max_loop_gap_ms`, `nmea_bad_crc`, `sats`, and `fix_type`: distinguish
  poor sky view from a healthy-but-still-acquiring receiver.

`max_flush_ms` is the worst detection-batch SD bus hold. `max_session_ms` is
the separate worst health-row write; do not use it to tune the batch size.

Five columns added 2026-09-07 answer questions the older ones could not:

- `home` is `armed` only when the radio was confirmed listening on the home
  channel at the moment the row was written; it is `down` while a bounded
  action owns the radio, while Trace is paused, and after a restore or re-arm
  that failed. **A quiet run with `home=down` is not evidence that the band
  was quiet.**
- `read_err` and `rearm_err` split reception faults that `crc_err` used to
  absorb: `crc_err` is now only a CRC rejection (the radio heard a corrupted
  packet), `read_err` is our own side failing to read one, and `rearm_err`
  counts re-arms the radio refused — each one is airtime the receiver spent
  deaf.
- `sd_short_writes` counts appends where the card accepted fewer bytes than
  asked. Any nonzero value means rows were lost, usually a full or failing
  card, and `sd` will have gone `down` at that point.
- `sd_csv_repairs` counts CSVs this run had to close a partial last row on, or
  set aside because their header was not ours. Nonzero means the card carried
  damaged or foreign files before this run started; check for `.bad` files in
  the run directory.

`sd_outages` and `sd_last_outage_ms` (added 2026-09-07, audit A26) count
times the card went away this run and how long the most recent gap lasted. A
health row cannot be written while the card is missing, so an outage used to be
nothing but a hole between two `sd=ok` rows. A **`reason=sd_recovered`** row is
now written the moment the card comes back, carrying the outage duration.

`session_id` names the boot that wrote the row. A card reseated mid-drive
rejoins the run directory it left on purpose, so the run number alone cannot
separate two boots' rows; `manifest.txt` carries a matching block per boot.

None of these count RF the receiver never had a chance to hear. Unknown
reception loss stays unknown.

## `manifest.txt`: what the device was while it heard all this

Added 2026-09-07 (audit A18). One `[session]` block per boot that opened this
run directory, appended, never rewritten. Read it before interpreting anything
else in the folder: the settings files at the SD root can be edited after a run
ends, so they are not evidence of what the run used.

Each block records the firmware version and git revision (including a `-dirty`
marker when the build had uncommitted changes), the board and radio, the
resolved modem parameters actually in use, the region, capture window and sweep
margin, and the mission profile. Two lines state the semantics that host
analysts most often get wrong:

- every `*_millis` column is device uptime, not wall clock; and
- a position is where the **receiver** was, never where a transmitter was.

`rejoined_existing_run=1` means this boot appended to a directory that already
existed — a reseated card, or run 9999 with no free number left. When you see
it, the folder holds more than one measurement session and `session_id` is what
separates them.

`ui_redraw_max_us` and `ui_redraw_mean_us` are the worst and mean cost of one
full screen redraw since boot. The UI task runs on Core 0 and never touches the
radio, so a slow frame cannot explain a missed packet on its own; what these
answer is whether redraw cost moves at all while a bounded action owns the
radio. Compare rows from a quiet window against rows written during a Sweep.

## `focus.csv`: one bin, observed for longer

A Sweep gives each bin a few tens of milliseconds. Focus gives one bin up to
two seconds, sampling it every 20 ms, and writes a single row per request. Run
it from **Activity** (press down to the Focus view, then Enter); it surveys the
last Sweep's strongest peak, or the bin containing the home channel if no sweep
has completed.

Read the row in this order:

- `request_status` and `home_restore` first. Only a `complete` row with
  `home_restore=1` is evidence of anything. A `timeout` row means the request
  hit its wall-clock deadline under bus contention and stopped sampling; its
  partial statistics are still real, but `observation_ms` will be short of
  `requested_dwell_ms`.
- `sample_count` against `requested_samples`. Samples are derived from the
  dwell at a fixed 20 ms spacing, not requested independently, so a short
  `observation_ms` means fewer samples and a correspondingly thinner picture.
- `rssi_median_dbm`, `rssi_p90_dbm`, `rssi_peak_dbm`. These are exact
  percentiles from a 1 dB histogram, or **blank** when the histogram cannot
  give an exact answer. A blank is not a zero and not a missing reading; it
  means the summary was refused rather than estimated.
- `qualifying_count` is the number of samples that sat above the pass's own
  median by the qualifying margin. It is the count that survived controlled
  measurement where every RSSI summary statistic failed.
- `skipped_samples` (2026-09-07, audit A22) counts sample slots abandoned
  because the SPI bus made them arrive more than a full slot late. They are
  skipped rather than taken back-to-back to catch up, so `sample_count` and the
  requested spacing describe samples that really were spaced that way. A
  non-zero value means the pass was measured under contention — not that
  anything was wrong with the signal.
- `partial_observation_ms` is sampling time from a pass that did not qualify
  for coverage credit. A cancelled or failed pass used to report
  `observation_ms` 0 whatever it had actually sampled.

Two things this file deliberately does not tell you:

- **`coverage` says how much looking a bin has had — nothing else.** It reads
  `insufficient`, `sampled`, or `repeated`, derived only from valid-pass count
  and accumulated observation time. It is not signal strength, not a likelihood
  that the bin is empty, and not confidence in anything. With the shipped 2s
  pass, one Enter earns `sampled` and three earn `repeated`.
  - Coverage accumulates **per bin, for consecutive requests at that bin**, and
    resets when you survey a different one. Accumulated time means time on
    *that* frequency, so it cannot be carried across a retune.
  - A cancelled, failed or timed-out request contributes nothing, and neither
    does a pass shorter than 500ms.
  - The raw `valid_passes` and `observation_ms` stay in the row beside the
    label. If the two ever disagree with your reading of it, trust the counts.
- **Elevated RSSI is not a transmission, and coverage does not imply
  activity.** A high `qualifying_count` says
  samples were elevated over that pass's own floor. Whether that is a
  transmission depends on link quality the device cannot know, and controlled
  measurement found the indication least reliable exactly where a band is
  busiest. Use `detections.csv` for what was actually received.

`wifi_on` records whether the AP was up during the request; keep it in mind
when comparing rows, since it changes the resource picture around the radio.

## Practical workflow

1. Copy the complete `runNNNN` folder before editing or importing it.
2. Open `session.csv` first and record the run number, GPS availability, and
   any nonzero drop counters.
3. Filter `detections.csv` by `profile`, then inspect frequency/modem/RSSI/SNR
   before interpreting protocol-specific fields.
4. Join `nodes.csv` to packet observations by run and nearby
   `rx_uptime_ms`; do not assume every packet from a node carries an identity.
5. Treat `focus.csv` as a follow-up to `energy.csv`, not a replacement: join
   them on `selection_bin_index` to see what a longer look at a swept peak
   found.
6. Preserve the original header with exports. If combining runs, union fields
   by column name rather than concatenating by position.

`raw_packet_hex` is useful for offline protocol research, but it is not a
claim that a payload has been decrypted or authenticated. Keep raw data and
GPS traces out of public spreadsheets by default.
