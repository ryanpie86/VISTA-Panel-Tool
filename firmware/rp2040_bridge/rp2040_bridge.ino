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

static const unsigned long KEY_PACE_MS = 500;         // SERIAL_PROTOCOL.md: ~0.5s pacing between KEY batches, enforced here
static const unsigned long KEY_TX_TIMEOUT_MS_PER_KEY = 4000;  // give up waiting for the panel to poll our address, per queued key
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
static String pendingKeys;
static unsigned long pendingSinceMs = 0;
static unsigned long lastKeySentMs = 0;
// Snapshots of vista.cpp's keySend* bench counters taken when a batch is
// queued, so the ACK/DEBUG lines can report deltas scoped to just this
// batch -- see those counters' declaration in vista.cpp for why: Vista::
// sendPending() can't tell a genuine panel-acknowledged send apart from
// writeChars() quietly giving up after 5 failed attempts, so the plain
// ACK line alone can't answer whether a send actually worked.
static uint32_t pendingFramesBefore = 0;
static uint32_t pendingResentBefore = 0;
static uint32_t pendingGaveUpBefore = 0;
static uint32_t pendingAckedBefore = 0;

static bool lastKeybusConnected = false;
static unsigned long lastBusFaultReportMs = 0;

// vista.keybusConnected (upstream) is never actually set true anywhere in
// the library -- only ever assigned false, in Vista::stop(). Confirmed by
// grepping both the original esphome-components source and our vendored
// copy. Track real bus activity ourselves instead.
//
// This originally fired on *any* decoded frame, including the catch-all
// "other" (unrecognized opcode) bucket -- fine with the interrupt-driven
// software bit sampler, which rarely survives long enough on pure noise
// (e.g. GP26 floating with the panel powered off) to assemble a complete
// frame. It was narrowed to *valid F7 only* during the PIO-RX experiment,
// since PIO was bench-confirmed to happily frame ambient noise into a
// steady stream of garbage "other" frames, keeping this permanently
// "connected" with no panel attached at all -- a valid F7 needs a specific
// 45-byte structure to pass its checksum, which noise essentially never
// produces by chance. PIO RX is now disabled again (VISTA_RP2040_USE_PIO_RX
// 0 in vista.h) in favor of the interrupt-driven decoder, so that noise-
// framing risk is gone and the narrowing left this permanently reporting
// "keybus not detected" on live bus traffic instead (bench-confirmed: F0/F6/
// F8/F9/other frames decoding continuously while no F7 has yet passed
// checksum, due to the still-open F7 truncation issue) -- reverted back to
// any decoded frame, matching the decoder actually in use.
static unsigned long lastBusActivityMs = 0;
static bool everSawBusActivity = false;
static const unsigned long BUS_ACTIVITY_TIMEOUT_MS = 3000;  // no decoded frame in 3s -> call it down

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
        // cbuf[12] is only meaningful once the frame actually read that
        // far -- a short/timed-out read leaves it as whatever garbage was
        // in that queue slot, which can spuriously look "valid".
        f7Valid = cmd->size > 12 && (uint8_t)cmd->cbuf[12] != 0x77;
        if (f7Valid) cntF7Valid++;
        break;
      case 0xF6: cntF6++; break;
      case 0xF9: cntF9++; break;
      case 0xFA: cntFA++; break;
      case 0xF2: cntF2++; break;
      case 0xF8: cntF8++; break;
      case 0xFB: cntFB++; break;
      default: cntOther++; break;
    }
    // Ground-truth dump for every decoded frame, not just F7 -- the
    // per-opcode counters in STATS only show volume, not content, and
    // that's been the bottleneck in every round of this investigation so
    // far (e.g. content coming back suspiciously blank on checksum-"valid"
    // frames -- a real frame's checksum byte is chosen by the sender to
    // make the total sum work regardless of content, so "valid" alone
    // doesn't prove the bytes were sampled correctly). Dumping everything
    // means the next live-traffic capture doesn't need yet another
    // instrumentation round just to see what F0/F6/F8/"other" frames
    // actually contain.
    {
      char opcodeHex[3];
      snprintf(opcodeHex, sizeof(opcodeHex), "%02X", opcode);
      String hex = "RAW op=" + String(opcodeHex);
      if (opcode == 0xF7) hex += " valid=" + String(f7Valid ? 1 : 0);
      hex += " size=" + String(cmd->size) + " bytes=";
      for (size_t i = 0; i < cmd->size && i < CMDBUFSIZE; i++) {
        uint8_t b = (uint8_t)cmd->cbuf[i];
        if (b < 0x10) hex += "0";
        hex += String(b, HEX);
        hex += " ";
      }
      sendLine(hex);
    }
    // Any decoded frame proves the bus is live (see the comment on
    // lastBusActivityMs above) -- emitDisp() still only fires for a valid F7.
    lastBusActivityMs = millis();
    everSawBusActivity = true;
    if (opcode == 0xF7 && f7Valid) {
      emitDisp(cmd->statusFlags);
    }
  }

  if (keyPending) {
    if (!vista.sendPending()) {
      // Bench diagnostic: batching (see handleSerialLine()'s KEY parsing)
      // fixed the software-side round-trip cost per key, but real
      // transmission still only happens on an actual bus poll of our
      // keypad address -- if that poll cycle itself is slow, a multi-key
      // batch could still take long enough in real bus time to outlast
      // the panel's own inter-digit code-entry timeout, independent of
      // anything on our end. This reports exactly how long the whole
      // batch took to drain on the bus, to tell that apart from some
      // other cause (wrong address, wrong sequence, etc.) if programming
      // mode still isn't entered despite a clean ACK.
      unsigned long elapsedMs = millis() - pendingSinceMs;
      // Vista::sendPending() (what the ACK above is based on) can't tell
      // a genuine panel-acknowledged send apart from writeChars() quietly
      // giving up after 5 failed attempts -- both end with the outbound
      // queue empty. framesBuilt/resent/gaveUp/acked (see their
      // declaration in vista.cpp) answer that directly: gaveUp>0 here
      // means this "ACK" is a false positive -- the panel never actually
      // confirmed receiving the data, no matter how clean the ACK looks.
      uint32_t framesBuilt = keySendFramesBuilt - pendingFramesBefore;
      uint32_t resent = keySendResent - pendingResentBefore;
      uint32_t gaveUp = keySendGaveUp - pendingGaveUpBefore;
      uint32_t acked = keySendAcked - pendingAckedBefore;
      sendLine("ACK," + String(PARTITION) + "," + pendingKeys);
      sendLine("DEBUG,key batch '" + pendingKeys + "' (" + String(pendingKeys.length()) +
                " keys) drained in " + String(elapsedMs) + "ms framesBuilt=" + String(framesBuilt) +
                " charsInFrame=" + String(keySendCharsInLastFrame) + " resent=" + String(resent) +
                " gaveUp=" + String(gaveUp) + " acked=" + String(acked));
      keyPending = false;
    } else if (millis() - pendingSinceMs > KEY_TX_TIMEOUT_MS_PER_KEY * (unsigned long)pendingKeys.length()) {
      sendLine("ERR,key transmit timeout for '" + pendingKeys +
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

    String keys = line.substring(secondComma + 1);

    for (size_t i = 0; i < keys.length(); i++) {
      if (!isValidEcpKey(keys.charAt(i))) {
        sendLine("ERR,unsupported key '" + String(keys.charAt(i)) + "' in KEY command: " + line);
        return;
      }
    }

    // Vista::write() appends to the library's own outbound ring buffer
    // (CMDBUFSIZE entries) with no overflow check of its own -- queuing
    // more than that before any of it drains would silently wrap and
    // corrupt already-queued-but-unsent entries. Not reachable before this
    // batching existed (each KEY command only ever carried one character),
    // so guard it explicitly now that a single command can carry many.
    if ((int)keys.length() >= CMDBUFSIZE) {
      sendLine("ERR,key batch too long (" + String(keys.length()) + " chars, max " +
                String(CMDBUFSIZE - 1) + "): " + line);
      return;
    }

    if (keyPending) {
      sendLine("ERR,keys '" + keys + "' dropped -- previous batch still pending");
      return;
    }

    unsigned long now = millis();
    if (now - lastKeySentMs < KEY_PACE_MS) {
      delay(KEY_PACE_MS - (now - lastKeySentMs));
    }

    // Queue every key immediately, back-to-back: Vista::write() just
    // appends to the library's own outbound ring buffer, and the panel's
    // real poll cycle drains it at native bus speed from there -- same as
    // how a human pressing keys on a physical keypad only needs each
    // press registered quickly, not a full bus-poll round trip before the
    // next press. Waiting for an ACK after every single character (a full
    // USB round trip *and* a wait for the next real bus poll, per key)
    // was bench-confirmed too slow: a 7-digit installer code took long
    // enough key by key that the panel's own inter-digit code-entry
    // timeout reset before the sequence finished, even though every
    // individual key transmitted and acked fine on its own.
    pendingFramesBefore = keySendFramesBuilt;
    pendingResentBefore = keySendResent;
    pendingGaveUpBefore = keySendGaveUp;
    pendingAckedBefore = keySendAcked;
    for (size_t i = 0; i < keys.length(); i++) {
      vista.write(keys.charAt(i));
    }
    keyPending = true;
    pendingKeys = keys;
    pendingSinceMs = millis();
    lastKeySentMs = pendingSinceMs;
    return;
  }

  sendLine("ERR,unrecognized command: " + line);
}
