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

`probe.csv` and `energy.csv` may exist with only a header when the feature was
not used. Their absence is not an error on older runs.

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

## Practical workflow

1. Copy the complete `runNNNN` folder before editing or importing it.
2. Open `session.csv` first and record the run number, GPS availability, and
   any nonzero drop counters.
3. Filter `detections.csv` by `profile`, then inspect frequency/modem/RSSI/SNR
   before interpreting protocol-specific fields.
4. Join `nodes.csv` to packet observations by run and nearby
   `rx_uptime_ms`; do not assume every packet from a node carries an identity.
5. Preserve the original header with exports. If combining runs, union fields
   by column name rather than concatenating by position.

`raw_packet_hex` is useful for offline protocol research, but it is not a
claim that a payload has been decrypted or authenticated. Keep raw data and
GPS traces out of public spreadsheets by default.
