# Lingxiao Relaax220-VS Pool Pump — RS485 Controller (ESP8266)

A WiFi web dashboard, Home Assistant / MQTT bridge, and RS485 sniffer for a
**Lingxiao Relaax220-VS** variable-speed pool pump, running on an ESP8266.
The pump speaks a **Pentair / IntelliFlo–compatible** protocol over RS485, so
this controller builds and decodes Pentair-style automation-bus frames.

> This is a hobbyist reverse-engineering project shared **for educational and
> informational purposes only**. It is **not** affiliated with, authorized, or
> endorsed by Pentair, Lingxiao, or any other manufacturer. Use entirely at
> your own risk. See the full [Disclaimer](#disclaimer) and [Safety](#safety)
> sections below before building or using any of this.

### Project status (current)

The original pump this was developed and tested on **failed recently — my own
fault, I accidentally let it run dry**. I have since installed a **new pump**,
and the sketch is **not currently working on the new unit**. I have not yet had
a chance to investigate why (it may be a different firmware revision, changed
status/mode values, a wiring/connector difference, or a settings issue on the
new pump). This repo reflects what worked on the original pump; treat the new-
pump behavior as **untested / a known open issue** until updated. This is also a
good reminder from the error-code section: **do not let the pump run dry.**

---

## ⚠️ Important prerequisite — the pump must be Stopped and in Manual mode

**Nothing this controller sends will take effect unless the pump is first
stopped and set to Manual mode on the pump's own control panel.**

- Put the pump in **Manual** mode and **stop** it from the keypad first.
- Only then will the RS485 Start / Stop / Set-Speed / Remote-Enable commands be
  honored by the drive.
- If the pump is running one of its own onboard schedules / auto programs, it
  ignores external commands until you return it to Manual and stop it.

This matches how a Pentair automation controller takes over a pump: it must
enable remote control before the pump obeys the bus. Plan your testing with the
pump stopped and in Manual mode.

### Keep-alive: the pump times out (~2 min) without polling

Once the pump is under external/remote control, it expects to keep hearing from
the controller. **If it does not receive a status request for roughly 2 minutes,
it times out and reverts to Manual (local) mode**, dropping remote control.

This is exactly what the **Auto Poll** feature is for: it periodically sends a
status request (`CMD 0x07`) to keep the pump in remote mode and to refresh the
live readings. Set the **Auto Poll interval** comfortably under the ~2-minute
timeout (a few seconds to tens of seconds is typical; the default is a few
seconds). If you turn Auto Poll off and send nothing for ~2 minutes, expect the
pump to fall back to Manual mode and ignore commands until you re-enable
remote / resume polling.

---

## Features

- **Live web dashboard** (dark theme): running state, actual RPM, reported
  power (watts), pump Mode and State, MQTT status, auto-poll counter.
- **RPM slider** — sets pump speed (sends the speed command on release).
- **Quick controls** — Start, Stop, Remote Enable, Status request.
- **Auto status polling** with adjustable interval.
- **Home Assistant MQTT discovery** — pump RPM, power, running binary sensor,
  Start/Stop/Status/Remote buttons, and an RPM setpoint number entity appear
  automatically.
- **Advanced panel** — send any built-in verified command, send a custom raw
  hex packet, and view / export a decoded RX/TX log.
- **OTA updates** over WiFi.
- **First-boot friendly** — credentials live in `secrets.h`.

---

## Hardware

**Pump this project is built for and tested on:**
**Lingxiao Relaax220-VS** — variable-speed, self-priming in-ground pool pump
(115 / 208–230 V, ENERGY STAR certified). It exposes a **Pentair / IntelliFlo–
compatible RS485** automation bus at **9600 baud, 8N1**. It is *compatible with*
the Pentair protocol but runs Lingxiao's own firmware, so some status/mode enum
values differ from genuine Pentair pumps (documented below).

| Part | Used here | Notes |
|------|-----------|-------|
| Pump | **Lingxiao Relaax220-VS** | Pentair-compatible RS485, 9600 8N1 |
| Microcontroller | **ESP8266 — NodeMCU 1.0 (ESP-12E module)** | Any ESP8266 board works (Wemos D1 mini, etc.) |
| RS485 adapter | **Auto-direction TTL↔RS485 module** (no DE/RE pin, e.g. MAX3485-based) | Auto flow-control; if your module has DE/RE, tie them enabled or drive from a GPIO |

> If you are using different parts, adjust the pin names and, for a
> DE/RE-style module, add direction control. This build assumes an
> **auto-direction** module, which is why the sketch uses plain SoftwareSerial
> with no direction GPIO.

### Wiring

RS485 adapter ↔ ESP8266 (SoftwareSerial, 9600 8N1):

| RS485 module pin | ESP8266 (NodeMCU) |
|------------------|-------------------|
| RO (receiver out) | **D6** |
| DI (driver in)    | **D7** |
| VCC | **3V3** |
| GND | **GND** |

The Pentair automation bus is **9600 baud, 8N1**.

### Pump communication port (Lingxiao RS485 signal cable)

The pump's watertight **Communication Port** is an **M16 4-pin waterproof
connector**. Confirmed pinout:

| M16 pin | Signal | Cable wire | Connect to RS485 module |
|---------|--------|------------|-------------------------|
| **1** | RS485 A | Yellow | A |
| **2** | RS485 B | Red    | B |
| **3** | GND | Black  | GND |
| **4** | **+5V (from pump)** | — | (optional: power the ESP) |

The port is on the side of the drive housing, labeled **"Communication Port"**
(M16 male, 4 pins around a center key).

![Pump Communication Port location](docs/pump-communication-port.png)

The mating cable connector and its pins (Yellow / Red / Black populated):

![Connector face](docs/connector-face.jpg)
![Connector pins](docs/connector-pins.jpg)

**Self-powered, no extra wiring, and can be potted:** because pin 4 supplies
5V from the pump, the whole controller (Wemos D1 + RS485 module) runs entirely
off this single M16 cable — **no separate power connection or USB is needed** in
normal operation. Once wired and tested, the electronics can be **sealed in
epoxy / potting compound** for a compact, weatherproof module that just plugs
into the pump's communication port.

Notes on colors and power:
- The manual's diagram labels the B line "green"; on the physical cable that
  conductor is **red** (colors vary by batch — verify with a multimeter).
- **Pin 4 supplies 5V from the pump — this is where power is taken for the
  Wemos D1.** Feed pin 4 into the Wemos D1 **`5V`** pin (the USB/VBUS rail, NOT
  `3V3`); the board's onboard regulator produces 3.3V for the ESP. Then the
  single M16 cable carries both RS485 data and power. Confirm ~5V under load
  and adequate current with a meter before relying on it; keep grounds common.
- The pump-side connector is **male**; the mating cable end is **female**, so
  the two faces are mirror images. Always reference the pin the wire physically
  lands on (confirm with a continuity check).

> **Measure before you connect.** Before wiring anything to the ESP8266, use a
> multimeter to **verify the voltage on pin 4 (expect ~5V) and confirm the
> polarity/pinout** of every pin against the table above. Connector faces are
> mirrored and cable colors vary between batches, so do not assume — measure.
> Applying the wrong voltage to the wrong pin, or reversed polarity, can
> destroy the ESP8266, the RS485 module, or the pump's control board.

Notes:
- The pump is compatible with Pentair PL4/PLS4, Jandy, and Hayward automation
  over this port. For Hayward, the manual says to change the baud rate on the
  pump (press **Tab + 2**) so the pump speaks Hayward's language.
- On successful link the pump display shows **ECON** and the communication
  indicator lights — at that point the pump has handed control to the external
  system.
- If you get no communication, **swap Yellow/Green (A/B)** — A/B polarity is the
  usual culprit.
- Tighten the watertight nut on the port to keep moisture out.

### Connection diagram

```
        ESP8266 (NodeMCU 1.0 / ESP-12E)            RS485 auto-direction module           Lingxiao Relaax220-VS
        +-----------------------------+            +------------------------+             +---------------------+
        |                             |            |                        |             |   RS485 terminals    |
        |                         3V3 +----------->+ VCC                    |             |                     |
        |                         GND +----------->+ GND               A  <-+------------>+ A (Data +)          |
        |                             |            |                   B  <-+------------>+ B (Data -)          |
        |     D7 (DI / TX out)  ------+----------->+ DI (driver in)         |             | M16 4-pin port      |
        |     D6 (RO / RX in)   <-----+------------+ RO (receiver out)  A  <+--YELLOW---->+ Pin 1: RS485 A      |
        |                             |            |                    B  <+--RED------->+ Pin 2: RS485 B      |
        |     GND               ------+------------+ GND               GND <+--BLACK----->+ Pin 3: GND          |
        |     5V/VIN (optional) <-----+----------------------------------------5V---------+ Pin 4: +5V (pump)   |
        +-----------------------------+            +------------------------+             |  (9600 baud, 8N1)   |
                                                                                          +---------------------+

  Pump connector: M16 4-pin waterproof.
  Pinout:  Pin 1 = RS485 A (Yellow),  Pin 2 = RS485 B (Red),  Pin 3 = GND (Black),  Pin 4 = +5V from pump.
  Notes:
   - Auto-direction module: no DE/RE wiring needed.
   - Common ground across ESP8266, RS485 module, and pump (Pin 3 / Black).
   - Optional: power the ESP8266 from Pin 4 (+5V) instead of USB (verify voltage/current first).
   - Keep the A/B pair short/twisted. If no comms, swap A/B (Yellow/Red).
   - On a good link the pump shows ECON and the comm indicator lights.
```

---

## Software setup

1. Install the **ESP8266 Arduino core** (Boards Manager URL:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`).
2. Install libraries: **PubSubClient**, **EspSoftwareSerial** (ESP8266WiFi,
   ESP8266WebServer, ArduinoOTA come with the core).
3. Copy `LingxiaoPumpRS485/secrets.h.example` to
   `LingxiaoPumpRS485/secrets.h` and fill in your WiFi + MQTT details.
   `secrets.h` is gitignored so your credentials stay private.
4. Open `LingxiaoPumpRS485/LingxiaoPumpRS485.ino`, select your board, and
   upload.
5. Find the device IP (serial monitor at 115200, or your router). Open it in a
   browser.

---

## The Pentair / IntelliFlo RS485 protocol

The pump uses the Pentair automation-bus frame format. This has been
reverse-engineered by the community over many years; this project relies on
that public work (see **Sources**). Frame layout:

```
FF 00 FF A5 | PROTO | DEST | SRC | CMD | LEN | DATA[LEN] | CHK_HI CHK_LO
```

- **Header:** `FF 00 FF A5` (the leading `FF`s are a preamble; `A5` starts the
  real frame).
- **Protocol:** `0x00` for IntelliFlo pump messages.
- **Addresses:** pump = `0x60` (96), controller = `0x10` (16).
- **Checksum:** 16-bit sum of all bytes starting at `A5` through the last data
  byte, high byte first.

### Pump status response — Command `0x07`, 15-byte payload

Requesting status (`CMD 0x07`, no data) makes the pump reply with a 15-byte
status payload. Field mapping (community "longstanding interpretation",
confirmed/corrected against live systems — see Sources):

| Payload byte | Field | Notes |
|--------------|-------|-------|
| [0] | Pump Running | `0x0A` = started, `0x04` = stopped |
| [1] | Pump Mode | `0x00` Filter, `0x01` Manual, `0x02` Backwash, `0x06` Feature 1, `0x09`–`0x0C` Ext Program 1–4 |
| [2] | Pump State | `0x02` Running, `0x01` Priming, `0x04` System Priming, `0x00` Fault |
| [3]–[4] | Power (watts) | `watts = [3]*256 + [4]` |
| [5]–[6] | Speed (RPM) | `rpm = [5]*256 + [6]` |
| [7] | Flow (GPM) | VF / VS-F pumps only; 0 on plain VS |
| [8] | Filter percent used | 100 during a Filter Error |
| [9] | Unknown | 0 in captures |
| [10] | Pump Error | `0x00` OK, `0x02` Filter Error; other codes not fully mapped |
| [11]–[12] | Time remaining | hh : mm |
| [13] | Clock hours | 0–23 |
| [14] | Clock minutes | 0–59 |

### Note: the Lingxiao pump differs from standard Pentair values

This is a Pentair-**compatible** pump, not a real Pentair, so some enum values
differ from the tables above. Values observed on this Relaax220-VS:

| Byte | Observed on Relaax220-VS |
|------|--------------------------|
| Mode [1] | `0x00` = Local, `0x11` = Remote |
| State [2] | `0x01` = Normal Operation, `0x03` = Priming, `0x07` = Alarm, `0xFF` = Stopped |

The firmware decodes both the observed Lingxiao values **and** the standard
Pentair values (labeled), and shows raw hex for anything unknown. A built-in
passive discovery view flags never-seen packet types and logs which status byte
changed — useful for mapping the remaining fields (e.g. the error byte) by
provoking a condition and watching what changes.

### Verified controller → pump commands

| Purpose | CMD | Data |
|---------|-----|------|
| Status request | `0x07` | (none) |
| Start / Run | `0x06` | `0x0A` |
| Stop | `0x06` | `0x04` |
| Remote enable | `0x04` | `0xFF` |
| Set speed | `0x01` | `02 C4 <rpm_hi> <rpm_lo>` |

There is **no documented "clear fault" command** in the reverse-engineered
protocol — a faulted pump is cleared by removing the cause or power-cycling
(see Error codes).

---

## Lingxiao Relaax220-VS error / fault codes

From the Lingxiao variable-speed pump instruction manual (Troubleshooting
section). **E002 auto-recovers; all other codes stop the controller and require
a power cycle to restart.**

| Code | Fault | Common cause |
|------|-------|--------------|
| E001 | IPM (power module) failure | Power electronics / interference / ground |
| E002 | Output current over limit | Sudden load change (auto-recovers) |
| E006 | Input voltage too high | Abnormal supply / load disconnect |
| E009 | Input voltage too low | Low supply voltage |
| E011 | Motor overload | Low voltage / stall / load change |
| E013 | Output phase loss | U/V/W phase loss or imbalance |
| E014 | Controller overheat | High ambient / control board fault |
| E018 | Faulty current-sampling circuit | Current-detect element fault |
| E021 | Display board EEPROM failure | Bad display↔main-board link |
| E040 | Static blockage | Motor mechanical lock-up |
| E048 | PFC over-current | Low voltage / PFC circuit fault |
| E095 | Communication fault | Bad display↔main-board connection |
| E030 | Dry Run Alarm | Running dry / mis-report on stop |
| LOF  | Dry Run Alarm (low flow) | Flow below 18 GPM / 70 LPM |

> How the pump detects dry-run / prime: variable-speed drives infer it from the
> motor's electrical load (water present ⇒ expected power draw at a given RPM;
> spinning air ⇒ abnormally low power). The priming routine runs at a high
> fixed RPM for a set time on start; if the load never indicates water, it
> raises a dry-run/priming alarm.

---

## MQTT topics

- **State (published):** `pool/pump/state` — JSON with `running`, `rpm`,
  `setRpm`, `reported_power`, `mode`, `state`, `last`, `auto_poll`.
- **Commands (subscribed):** `pool/pump/cmd/start`, `/stop`, `/status`,
  `/remote`, `/rpm`.
- **Discovery:** published under `homeassistant/...` for auto-config.

---

## Safety

- **Pump must be Stopped and in Manual mode** for commands to take effect (see
  top of this document).
- This project drives real pool equipment. Test with the pump stopped, and be
  cautious sending experimental / custom packets.
- No dedicated "clear fault" command exists; clear faults by fixing the cause
  or power-cycling the pump.
- **Measure with a multimeter to verify voltage and polarity** on the M16
  connector before wiring anything — confirm pin 4 is ~5V and that each pin
  matches the pinout. Wrong voltage or reversed polarity can destroy hardware.
- Mind electrical safety and GFCI requirements per the pump manual.
- **Educational use only** — see the [Disclaimer](#disclaimer). You assume all
  risk and responsibility.

---

## Sources

Reverse-engineering and protocol interpretation build on prior community work.
Content in this document was paraphrased/summarized from these sources for
licensing compliance:

- **Pentair Automation Bus (RS-485) Protocol** — community protocol document.
  <https://docs.google.com/document/d/1M0KMfXfvbszKeqzu6MUF_7yM6KDHk8cZ5nrH1_OUcAc/mobilebasic>
- **nodejs-poolController** — tagyoureit et al. (status byte mapping beyond
  byte [2]). <https://github.com/tagyoureit/nodejs-poolController>
- **Arduino.Pentair** — Zuntara (RS-485 pin location, early decoding).
  <https://github.com/Zuntara/Arduino.Pentair>
- **Brute Forcing Pentair RS-485** — wolfteck.
  <http://wolfteck.com/2019/02/04/brute_forcing_pentair_rs-485/>
- **openHAB Pentair binding** — bus basics (9600 8N1).
  <https://www.openhab.org/addons/bindings/pentair/>
- **Lingxiao variable-speed pump instruction manual** — Troubleshooting /
  error-code table (E001–E095, E030, LOF) and priming behavior.

Trademarks (Pentair, IntelliFlo, Lingxiao, Home Assistant, Tasmota) belong to
their respective owners.

---

## Disclaimer

**This project is provided for educational and informational purposes only.**

- It documents personal, hobbyist reverse-engineering and experimentation with
  an RS485 interface on a pool pump. It is **not** official documentation and is
  **not** affiliated with, authorized, manufactured, or endorsed by Pentair,
  Lingxiao, Jandy, Hayward, or any other company. All trademarks belong to their
  respective owners.
- The information (protocol decoding, pinouts, wire colors, commands, and error
  codes) is **provided "as is," without any warranty** of any kind, express or
  implied, including accuracy, completeness, merchantability, or fitness for a
  particular purpose. Details vary between pump models, firmware, and production
  batches and **may be wrong for your unit** — always verify independently
  (e.g. with a multimeter and continuity checks).
- Working with mains-powered pool equipment, RS485 wiring, and custom firmware
  carries real risks including **electric shock, equipment damage, voiding your
  warranty, fire, water damage, and personal injury**. Pool pumps are connected
  to electrical and water systems governed by codes and regulations in your
  area; consult a **licensed electrician / qualified professional** and follow
  the manufacturer's manual and all applicable codes.
- **You assume all responsibility and risk.** By using any part of this project
  you agree that the author(s) and contributors are **not liable** for any
  damage, loss, injury, or other consequence arising from its use, misuse, or
  inability to use it. If you are not comfortable assuming that responsibility,
  do not use this project.

## License

MIT — see [LICENSE](LICENSE). Note the MIT license's "AS IS", no-warranty, and
no-liability terms apply to everything in this repository, including the
documentation.
