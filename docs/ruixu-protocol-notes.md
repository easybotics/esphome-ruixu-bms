# Ruixu Lithi2-16 BMS over RS485: error replies, bus timing and extra commands

Research notes compiled in November 2026 from public protocol documents and captures on a 3-pack bank.

**What this fork does with it (2.1.0):**
- Requests go out one at a time (`response_timeout` 1000 ms, `request_gap` 100 ms on `seplos_modbus`).
- Error replies (non-zero RTN) are logged with their meaning against the request that was outstanding (section 1).
- 0x44, 0x51 and the EF binary protocol are documented below but not decoded by the component yet.

**Field measurement behind the queue (2026-11-17, 3 packs, 141 s of logs):**
- 81 requests gave 75 good replies.
- All 6 failures were pairs of requests sent 23 ms apart (two packs polled in the same loop pass); the master answered `21 00 46 03 0000` (RTN 0x03) and neither request got data.
- Every other spacing, including 30-250 ms, succeeded. Replies arrived 182-207 ms after a request, the same for the master and the relayed slaves.

**Scope.** These are the author's Ruixu 314 Ah 16S packs: 3 in parallel, polled on the master pack's RS485 port at 9600 8N1. The ASCII protocol uses VER 0x21 and CID1 0x46.

**Labels.**
- **CONFIRMED** means a primary document or a capture shows it directly.
- **INFERRED** means our reasoning, with no Ruixu document behind it.

**No public Ruixu protocol document exists.**
- Ruixu's download page has no files: https://www.ruixubattery.com/download
- Searches for "RS_GA02", "CN10.02", "SZTB" and the binary EF..16 framing found nothing.
- Everything below is therefore fitted from the Pylontech, Seplos, EG4 and PACE specs, which share the same frame format, plus the author's captures.

---

## 1. RTN codes, and what the master's `21 00 46 03 0000` reply means

In a reply, the CID2 byte holds RTN, the return code. Pylontech's frame table calls it "Response information: return code". Source: Pylon RS485 V3.3 (2018-08-21), §2.2.2 and §2.5.2, available at https://www.photovoltaikforum.com/core/file-download/72784/ and in https://github.com/nkinnan/esphome-pace-bms/tree/main/protocol_documentation/paceic/0x20/PYLON. A copy is also in the repo: `docs/PYLON RS485-protocol-V3.3-20180821.pdf`.

| RTN | Meaning | Where it is defined (all CONFIRMED in the cited document) |
|---|---|---|
| 00 | Normal | Every spec |
| 01 | VER error | Pylon V2.8/V3.3, Seplos, EG4, Gobel. Pylon 0x4F ignores VER, so 01 can only come from commands that check it. |
| 02 | CHKSUM error (whole-frame checksum) | Every spec |
| **03** | **LCHKSUM error**: the 4-bit checksum nibble of the request's LENGTH field did not match its 12-bit LENID | Pylon V2.8/V3.3, EG4 LifePower4, Seplos V2.0, PACE 0x25, Gobel, Vertiv 电总 |
| 04 | CID2 invalid (unknown command) | Every spec |
| 05 | Command format error | Every spec |
| 06 | Invalid data ("INFO data invalid") | Every spec |
| 07 | No data (history records) | Seplos only (https://forum.iobroker.net/assets/uploads/files/1631599969177-seplos-48v-100a-bms-rs485-protocol.pdf) |
| 09 | Operation or write error | PACE RS232 0x25 only |
| 90 | ADR error (marked user-defined) | Pylon V2.8/V3.3, Gobel (https://forum.iobroker.net/assets/uploads/files/1675922117046-gobelpower.pdf) |
| 91 | Communication error. Pylon V2.8 footnote: the link between the Master Pack and a Slave Pack failed. V3.3 says "internal communication error". | Pylon V2.8/V3.3, Gobel |
| E1 / E2 / E3 / E4 | CID1 invalid / command execution failed / equipment failure / invalid permission | Seplos 48V 100A PDF (above) |

A drop-in name table, covering the union of all the above, is in nkinnan's `FormatReturnCode()`: https://github.com/nkinnan/esphome-pace-bms/blob/main/components/pace_bms/pace_bms_protocol_base.cpp. Codes it does not list come out as "Undocumented Response Error Code".

**Form of the error reply.**
- `21 00 46 03 0000` is a well-formed frame: VER 21, ADR 00, CID1 46, RTN 03, LENGTH 0000. **CONFIRMED** by framing arithmetic: for LENID 0 the LCHKSUM is (~0+1)&0xF = 0.
- Pylon V3.3 says "LENID = 0 means INFO is empty". **CONFIRMED.**
- No spec says outright that an error reply never carries data. So decode RTN before reading LENGTH or INFO, and accept any LENGTH.

**Our requests are correctly encoded.**
- `~21004642E00200FD36`: LENID 002 gives LCHKSUM (~2+1)&0xF = E, so LENGTH = E002. The CHKSUM is 0x10000 − 0x2CA = FD36. Both **CONFIRMED** by our arithmetic using the Pylon V3.3 §2.3.2 and §2.3.3 rules.
- `~2100464D0000FD9B` has LENGTH 0000, which is also valid.
- So RTN 03 does **not** mean the host builds LENGTH wrongly.

**Likely cause here (INFERRED).** The master is receiving corrupted, truncated or merged request bytes. The evidence:
- The error rate is about 1 in 27 polls.
- Requests sometimes go out about 30 ms apart.
- At 9600 8N1 one character takes 1.04 ms. A 0x42 request is 20 chars (≈21 ms), so a second request starting 30 ms after the first begins about 9 ms after the first ends. That is inside the BMS's turnaround time and before or during its reply.
- A 0x42 reply is 146 chars, about 152 ms. A 0x44 reply is about 96 ms and a 0x51 reply about 85 ms.
- Requests for ADR 01 and 02 must be relayed by the master to the slaves, which makes the master slower to answer.
- On a half-duplex bus, a request sent during a reply garbles both.
- Why the master reports LCHKSUM rather than CHKSUM is unknown. It probably checks LENGTH first, and a misaligned or merged frame fails that check first.
- **No public source reports RTN 03 during multi-pack polling.** The explanation is our inference.

**Required decoder behaviour (recommendation).**
- After the frame checksum passes, read `data[3]`. If it is not 0x00:
  - log `"Error reply RTN 0x%02X (%s) from ADR 0x%02X to request CID2 0x%02X for ADR 0x%02X"`;
  - complete the outstanding request as failed;
  - optionally retry once, after the inter-request gap.
- Do **not** publish sensors from an error reply.
- **Attribute the error to the outstanding request, not to the reply's ADR byte.** It is unknown whether the master echoes the requested ADR or puts its own (00) in an error reply.
- In the current syssi-derived code, the error frame:
  - falls into "Unhandled data received", because it is under 44 bytes and has no cell count at `data[8]`;
  - still calls `reset_online_status_tracker_()`, because routing is by `data[1]`;
  - so it counts as pack 0 "online".
  
  CONFIRMED from the code: https://github.com/syssi/esphome-seplos-bms/blob/d467742222199a12dea9a75796040443288dd40d/components/seplos_bms/seplos_bms.cpp#L24-L40
- Reference implementations all treat a non-zero RTN as a failed poll and do nothing beyond logging, rejecting or retrying:
  - nkinnan rejects the frame with no retry;
  - dbus-serialbattery logs a warning and rejects it;
  - seplos-mqtt-remote-rs485 retries up to 5×;
  - bmspace raises an exception.

---

## 2. Timing and serialization (fixing collisions on the 3-pack bus)

**What the specs say.**
- **CONFIRMED.** Seplos V2.0 §1.3: "If SU does not receive the response information of SM in 500ms or receives the wrong response information, the communication process is considered to have failed." PDF in https://github.com/syssi/esphome-seplos-bms/tree/d467742222199a12dea9a75796040443288dd40d/docs and in the repo `docs/`.
- PACE RS232 has the same 500 ms rule.
- Pylon V2.8, Pylon V3.3 and EG4 specify **no** response time, inter-frame gap or byte timeout. No document anywhere gives a minimum gap between frames.

**What working implementations do. All CONFIRMED in their code or README.**
- **nkinnan/esphome-pace-bms** (https://github.com/nkinnan/esphome-pace-bms/blob/49985c491a0fc200d39d9867053a9a30a4e33d7f/components/pace_bms/pace_bms_component_master.cpp#L518-L544):
  - one hub-owned read queue and write queue, with one request in flight (`request_outstanding_`);
  - the next request goes out only after the reply or timeout, **and** once `request_throttle` has passed since the last transmit;
  - defaults are 50 ms throttle and 200 ms timeout; the timeout measures silence since data last arrived, not total time;
  - bytes that arrive with no request outstanding are discarded;
  - if the queue is not empty at `update()`, that refresh is skipped.
  - Its README (https://github.com/nkinnan/esphome-pace-bms/blob/49985c491a0fc200d39d9867053a9a30a4e33d7f/README.md#L245-L258) recommends **200 ms throttle and 2000 ms timeout** for the slow 0x20 (EG4 LIFEPOWER4) BMS, which "locks up" when polled too fast.
  - For multi-pack setups through a master it suggests a timeout of about 2 s per pack.
  - It notes relay-mode firmware bugs where payload sizes are "off by a couple of bytes".
- **RUiXU's own tool** polls one pack at a time, with "Interval 2 S" in its screenshot. Manual: https://invertersrus.com/wp-content/uploads/2024/03/RUiXU-Tool-Software-Operation-Instructions.pdf
- **Home Assistant users with 10 Ruixu packs** saw CRC failures and became more stable after spreading polls out: https://community.home-assistant.io/t/rs485-component-for-pulling-battery-telemetry/790199
- **seplos-mqtt-remote-rs485**: a hard 1 s sleep between 0x42 and 0x44 and between packs, commented "Mandatory delay ... or there will be corrupt data". https://github.com/privatecoder/seplos-mqtt-remote-rs485/blob/3a105dfd299c86ec4ff8ddbfec1ec0bf8d91b87e/src/fetch_bms_data.py
- **bmspace**: a fixed 250 ms wait after each write. https://github.com/Tertiush/bmspace/blob/c756863884bad153b35330c97f107deea3b8de18/bms.py
- **dbus-serialbattery**: polls all addresses sequentially in one loop with a 1 s read timeout. Its EG4 driver uses a 2 s interval because 1 s caused errors. https://github.com/mr-manuel/venus-os_dbus-serialbattery/blob/99833373ffcf442e61800805393c1118db093bdf/dbus-serialbattery/bms/seplos.py
- **syssi's own Seplos V3 Modbus multi-pack example** serializes through the ESPHome modbus hub, with `send_wait_time: 200ms` and `command_throttle: 200ms`.

**Why our requests collide. The code facts are CONFIRMED; the 30 ms link is INFERRED.**
- syssi/esphome-seplos-bms does no serialization. Each pack is its own PollingComponent, and its `update()` calls `send(0x42, pack)` directly. There is no wait, lock or queue. https://github.com/syssi/esphome-seplos-bms/blob/d467742222199a12dea9a75796040443288dd40d/components/seplos_bms/seplos_bms.cpp#L222-L225
- `rx_timeout` (150 ms) is only an inter-byte gap for discarding partial frames.
- ESPHome's scheduler gives each interval a random first-run offset in [0, min(interval/2, 5000 ms)). https://github.com/esphome/esphome/blob/dev/esphome/core/scheduler.cpp
- So 3 packs with the same `update_interval` keep fixed random phase gaps between them, which can be a few ms. That fits the observed ~30 ms pairs.

**Recommended design (synthesis of the above).**
1. The hub (`seplos_modbus` equivalent) owns a FIFO of (ADR, CID2, INFO, callback) requests. A pack's `update()` enqueues and does not send. Skip or dedupe the enqueue if that pack still has the same request queued.
2. Allow one request in flight.
3. The request is done when a complete `~…\r` frame arrives, or when the response timeout expires.
   - Timeout: at least 500 ms per the Seplos spec. 1000–2000 ms is safer for ADR 01 and 02, which the master relays.
   - Measure it from the end of transmit, and restart it on each received byte.
4. Wait a minimum of **200 ms** after the reply or timeout before the next transmit. That is nkinnan's empirical figure; the RUiXU tool uses up to 2 s between packs.
5. The budget is comfortable. One cycle of 3 × (0x42 + 0x44) is about 3 × (21 + 152 + 21 + 96 ms + 2 × 200 ms gap + BMS turnaround), roughly 2–2.5 s. That fits a 5 s interval. If it does not fit, stretch the interval or skip that cycle, as nkinnan does.
6. On a non-zero RTN or a timeout:
   - retry once after the gap;
   - count the failure per pack;
   - keep `MAX_NO_RESPONSE_COUNT`-style offline tracking keyed on the **requested** ADR.
7. Discard or log bytes that arrive with no request outstanding. They indicate another bus master or a late reply.

---

## 3. Captures decoded field by field

All INFO below is hex-encoded except 0x51, which is noted there. Byte counts are checked against LENID.

### 0x44 alarm/status: request `~21004644E00200FD34`, reply LENGTH 204A

LENID 0x04A = 74 chars = 37 bytes, which matches the field count below. **CONFIRMED** by arithmetic.

| Bytes | Value | Field | Status |
|---|---|---|---|
| 1 | 00 | DATAFLAG/INFOFLAG. There is no command/group byte, the same as in 0x42. | Layout INFERRED, by analogy with Pylon and Seplos |
| 1 | 10 | Cell count M = 16 | CONFIRMED (matches 0x42) |
| 16 | 00 ×16 | Per-cell alarm state. Pylon and Seplos code these 00 normal, 01 below lower limit, 02 above upper limit, F0 other. | Meaning CONFIRMED in Pylon V3.3 §3.5 and Seplos Table 12; that Ruixu uses it is INFERRED |
| 1 | 07 | Temperature count N = 7 | CONFIRMED (matches 0x42) |
| 7 | 00 ×7 | Per-temperature alarm state, same coding | INFERRED |
| 1 | 00 | Charge/discharge current state | INFERRED (Seplos-style single byte; Pylon instead has charge current, module voltage, discharge current) |
| 1 | 00 | Pack voltage state | INFERRED |
| 1 | **08** | **Custom-item count P = 8**, not "discharge current status": 08 is not a valid state value (00/01/02/F0), and exactly 8 bytes follow | INFERRED, strong fit |
| 1 | 00 | Status1 (Pylon bits): 7 module UV, 6 charge OT, 5 discharge OT, 4 DOC, 3 reserved, 2 COC, 1 cell UV, 0 module OV | Bit definitions CONFIRMED in Pylon V3.3; mapping INFERRED |
| 1 | **06** | Status2: bit3 using module power, **bit2 DFET on, bit1 CFET on**, bit0 pre-FET. 0x06 = both FETs on, which matches the author's guess. Seplos's on/off byte would read 0x03, so the Pylon reading fits better. | Bits CONFIRMED in Pylon; mapping INFERRED |
| 1 | 00 | Status3: bit7 effective charge current (>0.1 A), bit6 effective discharge current, bit5 heater, bit3 fully charged (SOC 100 %), bit0 buzzer. 00 fits current 0 and SOC 59 % in the 0x42 capture. | INFERRED |
| 1 | 00 | Status4: cell 1–8 voltage error bits (>4.2 V or <1 V) | INFERRED |
| 1 | 00 | Status5: cell 9–16 voltage error bits | INFERRED |
| 3 | 00 00 00 | Unknown or reserved. They may carry the RUiXU tool's other flags: CHG LIMIT, AC-in, HEATER, Chg-Match, Fireproof, FAN, and Fault/Alarm/Protect. | Unknown |

To test the layout:
- Capture 0x44 while charging or discharging. Status3 bit7 or bit6 should set.
- Capture it while a FET is off. Status2 should change.
- Check that P stays 08 in every capture.

### 0x51 manufacturer info: request `~21004651E00200FD36`, reply LENGTH C040

- **CONFIRMED from the raw capture in the example YAML line 850.** The INFO is **raw ASCII, not hex-encoded**: `~21004600C040RS_GA02V01_51200␠␠␠␠RX01BMS45xxxxxxxxxxxSZTB␠×20F0E7`.
- LENID 0x040 = 64 characters. The CHKSUM F0E7 is computed over those raw characters; recomputed here, it matches.
- A decoder must **not** hex-decode this INFO. Slice the characters instead:

| Chars | Value | Field (RUiXU tool label) | Status |
|---|---|---|---|
| 0–19 | `RS_GA02V01_51200` + 4 spaces | Hardware: "Sensor Name" in the tool's Firmware Burn tab | Slot widths CONFIRMED by capture; label INFERRED from the manual screenshot, which shows `RS_GA02V01_51100` on a 100 Ah pack |
| 20–23 | `RX01` | Software version: "Soft Release" / "SW". The manual's sample pack shows `LD07`. | Field CONFIRMED (tool shows RX01); label INFERRED |
| 24–63 | `BMS45xxxxxxxxxxxSZTB` + 20 spaces | BMS serial: "Factory Name". The same string is readable over EF register 0x01D4. | CONFIRMED (matches the EF read) |

- These character slots, 20/4/40, are exactly the hex-character widths of Pylon's 10/2/20-byte 0x51 layout (Pylon V3.3 §3.2, LENID 40H). In other words, Ruixu fills Pylon's slots with plain ASCII. **INFERRED.**
- `_51100` vs `_51200` may encode 51.2 V with 100 A vs 200 A. The Lithi2-16 is rated 200 A. **INFERRED.**

### 0x4D date/time: request `~2100464D0000FD9B` (no INFO), reply `~21004600200E07E8090D070100FAB3`

- LENID 0x00E = 14 chars = 7 bytes. **CONFIRMED.**
- 0x4D is named "acquisition/obtain time" and 0x4E "synchronization time" in Seplos (iobroker PDF above) and EMU1101. **CONFIRMED.** No Pylon revision defines either command.
- Seplos gives names only. The 7-byte format comes from Pylon V3.3 §2.3.5 DATA TIME: year (2 bytes), then month, day, hour, minute and second (1 byte each).

| Bytes | Value | Field | Status |
|---|---|---|---|
| 2 | 07E8 | Year 2024 | Format CONFIRMED (Pylon §2.3.5); that it applies here is INFERRED |
| 1 | 09 | Month | INFERRED |
| 1 | 0D | Day 13 | INFERRED |
| 1 | 07 | Hour | INFERRED |
| 1 | 01 | Minute | INFERRED |
| 1 | 00 | Second | INFERRED |

- Result: 2024-09-13 07:01:00. The RTC is probably unset or drifting.
- The RUiXU tool has "Read RTC" and "RTC= System time" buttons. **CONFIRMED** in its manual §2.3.3.
- That those buttons map to 0x4D and 0x4E is **INFERRED**. Do not send 0x4E until a 7-byte INFO is captured from the tool.

### 0x42, for reference (already decoded; two notes)

1. LENGTH 8080 is correct: LENID 0x080 = 64 bytes, which matches the field count.
2. The custom count `04` is followed by full capacity, cycles, and then **4 bytes**: `3B` SOC, `64` SOH, `0000` unknown or reserved.
   - Pylon's rule for user-defined = 4 sets the 2-byte capacities to FFFF and appends two 3-byte capacities (Pylon V3.3 §3.3).
   - Ruixu does not do that. **Do not** reuse python-pylontech's "if items > 2, parse 3-byte capacities" rule.
   - On Ruixu the count may mean "4 extra bytes". **INFERRED.**

---

## 4. Ruixu, the BMS hardware, and the EF binary protocol

**Product.** Lithi2-16, from Zhuhai Ruixu Electronic Technology. **CONFIRMED** in the data sheet: https://thecabindepot.ca/cdn/shop/files/Lithi2-16_Data_Sheet_v1.pdf
- 51.2 V, 314 Ah, 16 kWh, 16S1P LFP;
- RS485/CAN;
- 200 A charge and discharge;
- on-board LCD touch screen;
- self-heating;
- a "BMS inspection port";
- Link IN/OUT ports.

**Official tool.** RuiXUTools.exe V1.02. **CONFIRMED** in the manual (URL above).
- It connects at 9600 baud, set by the master's DIP switch, on the master's RS485 port, or over CAN.
- It sweeps packs 0–49.
- Tabs: real-time, History (event log and RTC), Parameter Settings 1–3 (need a dynamic password from techsupport@ruixubattery.com), User Parameters, and Firmware Burn.
- Status flags: CHG-MOS-ON, CHARGING, CHG LIMIT, AC-in, DSG-MOS-ON, DISCHARGING, HEATER, FULLY, Chg-Match, Fireproof, Pre-MOS, FAN.
- Temperatures: Tcell1–8, MOS_T, ENV_T.
- It gives no byte layouts.

**MCU.** The tool's BOOT INFO field "PCB" reads `R7FA2L1AB2DFP`, a Renesas RA2L1 (Cortex-M23, 48 MHz, 100-pin LQFP). **CONFIRMED** on Renesas's RA2L1 page; the 256 KB flash figure comes only from the RS listing. This does not identify the BMS maker. Boot Ver V001 and PROT Ver V001 also appear. Searches for an OEM through "SZTB", "RS_GA02", "LD07" and "CN10.02" found nothing.

**Open-source code.** The only Ruixu code found is easybotics/esphome-ruixu-bms, which is the author's origin repo and a fork of syssi/esphome-seplos-bms. **CONFIRMED.** Before 2.1.0 it had nothing on RTN, 0x44 or EF. The other forks named in the research were not verified. The Boqiang "LD43" vs Ruixu "LD07" naming match is a coincidence with no supporting source.

**EF binary protocol, on the same bus.** No public document was found. Everything here is **INFERRED** from the 7 captured frames.
- **Frame:** `EF | ADR | CMD | LEN | LEN data bytes | CHK_hi CHK_lo | 16`.
- **Checksum:** 0x10000 − (8-bit sum of ADR+CMD+LEN+data). EF and the checksum itself are excluded. This is the same two's-complement idea as the ASCII CHKSUM. It checks out on every captured frame, for example 00 83 04 00 65 00 01 → FF13, and reply 00 83 04 00 02 7A A8 → FE55.
- **0x83, read 16-bit registers.**
  - Request data: register (2 bytes) and count (2 bytes).
  - Reply data: byte count (00 02) then the value.
  - Known: 0x0065 = 0x7AA8 = 31400 = 314.00 Ah full capacity, which matches 0x42. 0x00B5 = 0x00FF, meaning unknown.
- **0xBD, read an ASCII block.**
  - Request: address and byte length. Reply: 2-byte byte count, then the data.
  - Known blocks:
    - 0x0112, length 0x20: battery code `<battery code>` (shown in the capture comment as the battery code);
    - 0x00FA, length 0x14: BMS SN1 `<BMS SN1>`;
    - 0x01D4, length 0x14: `BMS45xxxxxxxxxxxSZTB`, the same serial as in 0x51.
- **0xFB, version.** Reply data is `00` + `CN10.02` + 3 NULs.
- **Error reply:** `EF 00 83 01 02 FF7A 16`, that is LEN 1, code 02.
  - The request that triggered it, `EF 00 83 04 00 B5 00 01 FF 13 16`, had a **wrong checksum**: it should have been FEC3, and FF13 is the 0x0065 request's checksum pasted in.
  - So binary error 02 most likely means **checksum error**, mirroring ASCII RTN 02. This is a better fit than the Modbus "illegal data address" reading. It rests on one sample.
- The 0x16 end byte resembles DL/T 645-style meter framing, but nothing ties the two together.
- The data sheet's display and inspection port suggest where this protocol comes from, but that is unconfirmed.

---

## 5. Open questions

1. **Which request draws RTN 03, and what ADR does the error reply carry?** Log, for each error, the outstanding request (ADR, CID2), the time since the previous transmit, and whether the other request was still awaiting a reply. Check whether the errors stop after serialization (§2). If they do not stop, look for another bus master or a wiring or termination problem.
2. **Is anything else transmitting on this RS485 segment?** That could be the touch display, a Link port, or the inverter. Bytes arriving with no request outstanding would show it.
3. **The 0x42/0x44 byte after LENGTH.** Is it a data flag or the command value? Poll ADR 01 with INFO 01. If the byte becomes 01, it is the command value; if it stays 00, it is a flag.
4. **Must INFO equal ADR when addressing a slave?** Pylon says "Command should be matched with ADR". Test what a mismatch returns: 06? 90?
5. **The 0x44 layout.** Confirm that P stays 08. Map Status2/3 and the 3 trailing bytes by toggling states (charge, discharge, FET off, heater). Do this ideally next to the RUiXU tool's flag panel.
6. **The 4 bytes after cycles in 0x42.** Is `0000` reserved, or another value (for example remaining or design capacity)?
7. **Untested read-only commands worth probing,** one at a time and serialized:
   - 0x4F protocol version (VER 21 would mean v2.1; the reply's INFO is undocumented);
   - 0x47 system parameters;
   - 0x92 charge/discharge management;
   - 0x93 module SN;
   - 0x96 software version;
   - Pylon V3.5's master-only system commands 0x60–0x63 on ADR 00, which might return aggregated data for all 3 packs (https://tt-sun.com/wp-content/uploads/2025/03/PYLON-RS485-protocol-pylon-low-voltage-%E6%96%B0%E5%A2%9E%E5%8D%8F%E8%AE%AE%E8%AE%BE%E8%AE%A1-dy12%E5%8F%B79600.pdf).
   
   **Never send write or control commands** (0x4E time set, 0x94 set charge management, 0x95 turn-off, 0x64 system shutdown) without the author's explicit OK.
8. **The EF register map beyond the 5 known addresses**, and whether EF error code 02 always means a checksum error.
9. **The DIP-switch table and baud options for the 314 Ah pack.** The manuals at santansolar and manuals.plus returned 403.
10. **No free copy of YD/T 1363.3 was found.** No spec defines a byte timeout or an inter-frame gap. The 200 ms gap and 1–2 s timeout are empirical (nkinnan, the RUiXU tool, the HA thread).
