/*
 * VISTA-Panel-Tool RP2040 ECP bridge
 *
 * Emulates one Vista alpha keypad on the ECP bus and exposes it to the Pi
 * as a simple USB-serial line protocol -- see ../SERIAL_PROTOCOL.md for the
 * contract this sketch implements, and HARDWARE_ARCHITECTURE.md ("Bus
 * coprocessor: RP2040-Zero") for the pin/divider/transistor rationale.
 *
 * Board: Waveshare RP2040-Zero, via earlephilhower's arduino-pico core.
 * Library: Adafruit NeoPixel (for the onboard status LED on GP16).
 *
 * vista.h/vista.cpp/ECPSoftwareSerial.h/ECPSoftwareSerial.cpp in this
 * directory are vendored from Dilbert66/esphome-components
 * (components/vista_alarm_panel/), patched for RP2040/arduino-pico -- see
 * the "RP2040/Arduino-Pico port" comments in those files for exactly what
 * changed and why (Xtensa-only interrupt intrinsics, no arg-passing
 * attachInterrupt on this core).
 *
 * Current scope / known limitations (breadboard bring-up, panel not yet
 * connected):
 *  - Keypad address is fixed at compile time (KEYPAD_ADDR below). The
 *    scan-and-hot-swap address flow described in CONCEPT.md's "Onboarding
 *    / keypad-address flow" is a future protocol extension -- there is no
 *    address-selection command in SERIAL_PROTOCOL.md yet.
 *  - Only one emulated keypad address means only one partition is
 *    meaningful right now; KEY commands for any partition other than 1
 *    are rejected with ERR.
 *  - Address-conflict detection ("refuse to start if our address is
 *    already active") is not implemented -- it needs the same bus-sniffing
 *    this firmware doesn't do yet. Only generic keybus-down faults are
 *    reported.
 *  - ACK,<partition>,<char> fires once the ECP library's internal transmit
 *    queue has drained (i.e. the panel actually polled this keypad address
 *    and the pulse train went out), not merely once the key was queued.
 *    Without a panel on the bus, no polling ever happens, so KEY commands
 *    will time out with ERR -- that's expected until the panel is wired
 *    up, not a firmware bug.
 */

// vista.h itself defines ARDUINO_MQTT (every .cpp in this sketch is a
// separate translation unit, so a #define here wouldn't reach
// vista.cpp/ECPSoftwareSerial.cpp) -- kept here too as documentation of
// intent, harmless as an identical redefinition.
#define ARDUINO_MQTT
#include "vista.h"

#if defined(ARDUINO_ARCH_RP2040)
#include <Adafruit_NeoPixel.h>
#endif

// ---- Pin configuration -------------------------------------------------
// See HARDWARE_ARCHITECTURE.md "Bus coprocessor: RP2040-Zero" pin table.
static const int PIN_YELLOW_RX = 26;  // Yellow (panel "data out") -> 39k/10k divider -> GP26
static const int PIN_GREEN_TX = 27;   // GP27 -> 1k base resistor -> 2N2222 -> Green ("data in from keypad")
static const int PIN_GREEN_MON = 28;  // Green bus-monitor tap -> 33k/10k divider -> GP28
static const int PIN_STATUS_LED = 16; // Onboard WS2812, hardwired -- nothing to wire

// Fixed keypad address for the Vista-20P/15P/10P family (a fixed address
// on those panels, per CONCEPT.md's onboarding-flow notes). Runtime
// address selection is not implemented yet -- see file header.
static const uint8_t KEYPAD_ADDR = 16;

// This firmware emulates a single keypad address, so only partition 1 is
// meaningful. The field is still carried in the protocol so a future
// multi-keypad build doesn't need a wire-format change.
static const int PARTITION = 1;

static const unsigned long KEY_PACE_MS = 500;         // SERIAL_PROTOCOL.md: ~0.5s inter-key pacing, enforced here
static const unsigned long KEY_TX_TIMEOUT_MS = 4000;  // give up waiting for the panel to poll our address
static const unsigned long BUS_FAULT_REPORT_MS = 5000;  // rate-limit repeated "keybus down" ERR lines

Vista vista;

#if defined(ARDUINO_ARCH_RP2040)
// Bench report: red/green consistently came out swapped from what
// setStatusColor()'s logic calls for (green when disconnected, red when
// connected) -- classic symptom of the actual LED's wire order not
// matching the color-order flag declared here. NEO_RGB instead of
// NEO_GRB fixes it for this board.
static Adafruit_NeoPixel statusPixel(1, PIN_STATUS_LED, NEO_RGB + NEO_KHZ800);
#endif

static bool keyPending = false;
static char pendingChar = 0;
static unsigned long pendingSinceMs = 0;
static unsigned long lastKeySentMs = 0;

static bool lastKeybusConnected = false;
static unsigned long lastBusFaultReportMs = 0;

// vista.keybusConnected (upstream) is never actually set true anywhere in
// the library -- only ever assigned false, in Vista::stop(). Confirmed by
// grepping both the original esphome-components source and our vendored
// copy. Track real bus activity ourselves instead: any decoded frame at
// all (not just valid 0xF7 display frames) proves the bus is alive, since
// routine polling traffic (0xF0) is constant on a live ECP bus.
static unsigned long lastBusActivityMs = 0;
static bool everSawBusActivity = false;
static const unsigned long BUS_ACTIVITY_TIMEOUT_MS = 3000;  // no frame in 3s -> call it down

static void emitDisp(const statusFlagType &sf);
static void handleSerialLine(const String &line);

// Mirrors the allow-list inside Vista::write(char, uint8_t) (vista.cpp).
// That function silently no-ops any character outside this set rather
// than erroring, which would otherwise show up here as a false ACK (the
// internal transmit queue never had anything in it to drain, so
// sendPending() would report "done" immediately). Reject those up front
// instead so the Pi gets an honest ERR.
static bool isValidEcpKey(char key) {
  return (key >= '0' && key <= '9') || key == '#' || key == '*' || key == '|' ||
         (key >= 'A' && key <= 'D') || key == 'F' || key == 'M' || key == 'P' ||
         key == 'G' || key == 't';
}

static void setStatusColor(uint8_t r, uint8_t g, uint8_t b) {
#if defined(ARDUINO_ARCH_RP2040)
  statusPixel.setPixelColor(0, statusPixel.Color(r, g, b));
  statusPixel.show();
#else
  (void)r; (void)g; (void)b;
#endif
}

static void sendLine(const String &s) {
  Serial.print(s);
  Serial.print('\n');
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) {
    // give the USB-CDC enumeration a couple seconds; don't hang forever if
    // nothing is listening yet.
  }
  Serial.println("BOOT: serial up");

#if defined(ARDUINO_ARCH_RP2040)
  statusPixel.begin();
  setStatusColor(0, 0, 32);  // dim blue: bringing up the bus
  Serial.println("BOOT: status LED up");
#endif

  Serial.println("BOOT: calling vista.begin()");
  vista.begin(PIN_YELLOW_RX, PIN_GREEN_TX, (char)KEYPAD_ADDR, PIN_GREEN_MON);
  Serial.println("BOOT: vista.begin() returned, entering loop()");
}

static uint32_t framesDecodedCount = 0;  // any cmdAvail() drain, any frame type

// Bench diagnostic: per-opcode breakdown, so we can see whether F7 display
// frames are even showing up at the rate the rest of the bus traffic
// suggests they should, and if so, what fraction of them pass checksum.
// Opcode meanings per vista.cpp's decodePacket(): F0=poll loop,
// F7=status/display, F9=LRR, F6=key ack, F2=AUI, F8=unknown/generic,
// FA=expander, FB=RF supervision.
static uint32_t cntF0 = 0, cntF7Seen = 0, cntF7Valid = 0, cntF6 = 0, cntF9 = 0,
                 cntFA = 0, cntF2 = 0, cntF8 = 0, cntFB = 0, cntOther = 0;

// Bench diagnostic: cumulative count of SoftwareSerial::overflow() going
// true. Polled every loop() iteration since the underlying flag is
// sticky-and-clear-on-read (Vista::rxOverflow() -> SoftwareSerial::
// overflow()) -- catches whether the ISR-filled ring buffer is outrunning
// how often the main-loop-driven readChars() drains it, which would show
// up as exactly the kind of mid-frame truncation seen on long F7 reads.
static uint32_t rxOverflowCount = 0;

void loop() {
  static unsigned long lastHeartbeatMs = 0;
  if (millis() - lastHeartbeatMs > 2000) {
#if defined(ARDUINO_ARCH_RP2040)
    Serial.println("ALIVE rxEdges=" + String(rxEdgeCountRP2040) +
                    " txEdges=" + String(txEdgeCountRP2040) +
                    " framesDecoded=" + String(framesDecodedCount) +
                    " rxOverflow=" + String(rxOverflowCount));
#else
    Serial.println("ALIVE framesDecoded=" + String(framesDecodedCount) +
                    " rxOverflow=" + String(rxOverflowCount));
#endif
    Serial.println("STATS F0=" + String(cntF0) + " F7seen=" + String(cntF7Seen) +
                    " F7valid=" + String(cntF7Valid) + " F6=" + String(cntF6) +
                    " F9=" + String(cntF9) + " FA=" + String(cntFA) +
                    " F2=" + String(cntF2) + " F8=" + String(cntF8) +
                    " FB=" + String(cntFB) + " other=" + String(cntOther));
#if defined(ARDUINO_ARCH_RP2040)
    // readChars()'s own poll loop, isolated to long (44-byte, F7-only)
    // reads. longReadAttempts should roughly track F7seen. avgPolls very
    // low relative to the ~20ms/4us ~= 5000 polls a full timeout should
    // produce means something is starving the poll loop of CPU time
    // (an interference problem); avgPolls near that ceiling with bytes
    // still short means the loop is running at full speed but genuinely
    // isn't getting new data (points back at the ISR/edge-loss theory).
    uint32_t lra = longReadAttempts;
    Serial.println("LONGREAD attempts=" + String(lra) +
                    " polls=" + String(longReadPolls) +
                    " bytes=" + String(longReadBytes) +
                    " timeouts=" + String(longReadTimeouts) +
                    " avgPolls=" + String(lra ? longReadPolls / lra : 0) +
                    " avgUs=" + String(lra ? longReadElapsedUs / lra : 0));
#endif
    lastHeartbeatMs = millis();
  }

  if (vista.rxOverflow()) rxOverflowCount++;

  vista.handle();

  static String rxLine;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      rxLine.trim();
      if (rxLine.length())
        handleSerialLine(rxLine);
      rxLine = "";
    } else if (c != '\r') {
      rxLine += c;
    }
  }

  while (vista.cmdAvail()) {
    cmdQueueItem *cmd = vista.getNextCmd();
    if (cmd == NULL)
      break;
    lastBusActivityMs = millis();
    everSawBusActivity = true;
    framesDecodedCount++;
    // getNextCmd() surfaces every decoded ECP frame type (routine bus
    // polls, key-acks, expander/LRR/RF/AUI traffic, ...), not just alpha
    // display updates -- they all funnel through the same
    // pushCmdQueueItem() call in vista.cpp. Only 0xF7 frames populate
    // statusFlags.prompt1/prompt2 (see Vista::onDisplay(), only called
    // from the cbuf[0]==0xF7 branch of decodePacket()); cbuf[12]==0x77 is
    // that branch's own "checksum failed" marker. Mirrors the same gate
    // esphome-vistaECP's own wrapper (vistaalarm.cpp) uses before trusting
    // a decoded frame's prompt fields. Anything else here would emit a
    // blank/stale DISP on every bus poll cycle.
    uint8_t opcode = (uint8_t)cmd->cbuf[0];
    bool f7Valid = false;
    switch (opcode) {
      case 0xF0: cntF0++; break;
      case 0xF7:
        cntF7Seen++;
        f7Valid = (uint8_t)cmd->cbuf[12] != 0x77;
        if (f7Valid) cntF7Valid++;
        // Ground-truth dump: content is coming back suspiciously blank
        // (all-zero flags, empty prompt) even on checksum-valid frames.
        // A real frame's checksum byte is chosen by the sender to make
        // the total sum work regardless of content, so "valid" alone
        // doesn't prove the bytes were sampled correctly -- print the raw
        // frame so it can be compared against the documented F7 layout
        // (see the byte-position comments atop Vista::onDisplay() and the
        // example frames commented in vista.h) rather than guessing
        // further from the parsed/summarized fields alone.
        {
          String hex = "RAWF7 valid=" + String(f7Valid ? 1 : 0) + " size=" + String(cmd->size) + " bytes=";
          for (size_t i = 0; i < cmd->size && i < CMDBUFSIZE; i++) {
            uint8_t b = (uint8_t)cmd->cbuf[i];
            if (b < 0x10) hex += "0";
            hex += String(b, HEX);
            hex += " ";
          }
          sendLine(hex);
        }
        break;
      case 0xF6: cntF6++; break;
      case 0xF9: cntF9++; break;
      case 0xFA: cntFA++; break;
      case 0xF2: cntF2++; break;
      case 0xF8: cntF8++; break;
      case 0xFB: cntFB++; break;
      default: cntOther++; break;
    }
    if (opcode == 0xF7 && f7Valid) {
      emitDisp(cmd->statusFlags);
    }
  }

  if (keyPending) {
    if (!vista.sendPending()) {
      sendLine("ACK," + String(PARTITION) + "," + String(pendingChar));
      keyPending = false;
    } else if (millis() - pendingSinceMs > KEY_TX_TIMEOUT_MS) {
      sendLine("ERR,key transmit timeout for '" + String(pendingChar) +
                "' -- panel never polled keypad address " + String(KEYPAD_ADDR));
      keyPending = false;
    }
  }

  bool connected = everSawBusActivity && (millis() - lastBusActivityMs < BUS_ACTIVITY_TIMEOUT_MS);
  if (connected != lastKeybusConnected) {
    setStatusColor(connected ? 0 : 32, connected ? 32 : 0, 0);
    lastKeybusConnected = connected;
  }
  if (!connected && millis() - lastBusFaultReportMs > BUS_FAULT_REPORT_MS) {
    sendLine("ERR,keybus not detected on GP" + String(PIN_YELLOW_RX));
    lastBusFaultReportMs = millis();
  }
}

// Builds the 32-char two-line alpha_text from the decoded display prompt
// (prompt1/prompt2 are 16 real chars + null terminator each) and a
// firmware-defined flags byte. flags_hex isn't specified by any upstream
// standard here (unlike TPI, which SERIAL_PROTOCOL.md deliberately mirrors
// the *shape* of but not the bit layout) -- vista_tool/transports/base.py
// only currently depends on bit 0 meaning "armed", so that bit is load
// bearing; the rest are this firmware's own convention, documented in
// SERIAL_PROTOCOL.md.
//
// The panel re-broadcasts the same F7 status frame on essentially every
// poll cycle even when nothing changed, so this also suppresses repeats --
// SERIAL_PROTOCOL.md says DISP is "pushed whenever the ... display state
// changes", not on every poll.
static uint8_t lastFlags = 0xFF;  // sentinel: doesn't match any real byte we'd send on the very first real update
static char lastAlpha[33] = {0};
static bool haveLastDisp = false;

static void emitDisp(const statusFlagType &sf) {
  char alpha[33];
  memcpy(alpha, sf.prompt1, 16);
  memcpy(alpha + 16, sf.prompt2, 16);
  alpha[32] = '\0';

  uint8_t flags = 0;
  if (sf.armed) flags |= 0x01;
  if (sf.armedAway) flags |= 0x02;
  if (sf.armedStay) flags |= 0x04;
  if (sf.ready) flags |= 0x08;
  if (sf.chime) flags |= 0x10;
  if (sf.zoneBypass) flags |= 0x20;
  if (sf.alarm) flags |= 0x40;
  if (sf.acPower) flags |= 0x80;

  if (haveLastDisp && flags == lastFlags && memcmp(alpha, lastAlpha, sizeof(alpha)) == 0)
    return;

  lastFlags = flags;
  memcpy(lastAlpha, alpha, sizeof(alpha));
  haveLastDisp = true;

  char flagsHex[3];
  snprintf(flagsHex, sizeof(flagsHex), "%02X", flags);

  sendLine("DISP," + String(PARTITION) + "," + String(flagsHex) + "," + String(alpha));
}

static void handleSerialLine(const String &line) {
  if (line == "PING") {
    sendLine("PONG");
    return;
  }

  if (line.startsWith("KEY,")) {
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);
    if (secondComma < 0 || secondComma + 1 >= (int)line.length()) {
      sendLine("ERR,malformed KEY command: " + line);
      return;
    }

    String partStr = line.substring(firstComma + 1, secondComma);
    if (partStr.toInt() != PARTITION) {
      sendLine("ERR,unsupported partition in KEY command: " + line);
      return;
    }

    char key = line.charAt(secondComma + 1);

    if (!isValidEcpKey(key)) {
      sendLine("ERR,unsupported key '" + String(key) + "' in KEY command: " + line);
      return;
    }

    if (keyPending) {
      sendLine("ERR,key '" + String(key) + "' dropped -- previous key still pending");
      return;
    }

    unsigned long now = millis();
    if (now - lastKeySentMs < KEY_PACE_MS) {
      delay(KEY_PACE_MS - (now - lastKeySentMs));
    }

    vista.write(key);
    keyPending = true;
    pendingChar = key;
    pendingSinceMs = millis();
    lastKeySentMs = pendingSinceMs;
    return;
  }

  sendLine("ERR,unrecognized command: " + line);
}
