#pragma once

// --- RP2040/Arduino-Pico port (VISTA-Panel-Tool) -----------------------
// Upstream (Dilbert66/esphome-components, components/vista_alarm_panel)
// only targets ESP8266/ESP32. arduino-pico's core auto-defines
// ARDUINO_ARCH_RP2040; turn that into the USE_RP2040 gate the upstream
// headers already have partial hooks for, so we don't need a manual
// build flag.
#if defined(ARDUINO_ARCH_RP2040) && !defined(USE_RP2040)
#define USE_RP2040
#endif

// PIO-based RX (ecp_uart_rx.pio / Vista::pioRxInit()/pioRxPump()) was
// added to remove CPU/interrupt-load jitter from byte sampling, after
// bench testing showed the plain interrupt-driven decoder (the same
// approach upstream uses on ESP8266/ESP32) losing sync under real bus
// load. It's since needed a long chain of fixes to keep its own state
// machine correctly synchronized with rxHandleISR()'s pre-existing
// bus-protocol state machine (_rxState) -- gating, FIFO races, ACK-slot
// interactions -- and F7 frames specifically still aren't decoding
// reliably even with those fixed. Set to 0 to fall back to the plain
// interrupt-driven decoder for RX byte assembly (still on RP2040,
// unrelated to ESP8266/ESP32's own separate code path): every PIO call
// site already guards on s_ecpSm==-1 and falls back to
// vistaSerial->rxRead() on its own when pioRxInit() is never called, so
// this is a clean, reversible toggle -- no code path needs deleting to
// flip it back once/if PIO's remaining issues are understood.
#define VISTA_RP2040_USE_PIO_RX 0

// Upstream builds either as an ESPHome component (needs
// esphome/core/defines.h) or standalone via its own ARDUINO_MQTT escape
// hatch, which skips that include. We want standalone, always -- but a
// #define in the .ino (rp2040_bridge.ino) only applies to that one
// translation unit, not to vista.cpp/ECPSoftwareSerial.cpp, which the
// Arduino build compiles separately. Define it here instead, in a header
// every translation unit in this sketch includes first.
#if !defined(ARDUINO_MQTT)
#define ARDUINO_MQTT
#endif
// -------------------------------------------------------------------------

#if not defined(USE_ESP_IDF)
#include "Arduino.h"
#else
#define ESP32
#endif

#include <queue>
#include "ECPSoftwareSerial.h"

#if defined(USE_RP2040)
// Bench diagnostic counters -- see the matching comment above
// rxISRTrampolineRP2040() in vista.cpp.
extern volatile uint32_t rxEdgeCountRP2040;
extern volatile uint32_t txEdgeCountRP2040;
// Raw Green-wire edge-timing trace -- see the matching comment above its
// declaration in vista.cpp. Shared size #define so the .ino sketch can
// iterate the same ring buffer without hardcoding its length twice.
#define GREEN_EDGE_TRACE_SIZE 64
extern volatile uint32_t greenEdgeTimestamps[GREEN_EDGE_TRACE_SIZE];
extern volatile bool greenEdgeLevels[GREEN_EDGE_TRACE_SIZE];
extern volatile uint32_t greenEdgeTraceHead;
extern volatile uint32_t greenEdgeTraceCount;
extern volatile uint32_t keySendFramesBuilt;
extern volatile uint32_t keySendCharsInLastFrame;
extern volatile uint32_t keySendResent;
extern volatile uint32_t keySendGaveUp;
extern volatile uint32_t keySendAcked;
extern volatile uint32_t keySendAddrAnnounced;
extern volatile uint32_t keySendPendingAckTimeout;
extern volatile uint32_t keySendAddrDropped;
#endif


// #define DEBUG

#define MONITORTX

#define OUTBUFSIZE 30
#define CMDBUFSIZE 50
#ifdef ESP32
#define CMDQUEUESIZE 5
#else
#define CMDQUEUESIZE 2
#endif
#define FAULTQUEUESIZE 5
#define LRRADDR 3

// Used to read bits on F7 message
#define BIT_MASK_BYTE1_BEEP 0x07
#define BIT_MASK_BYTE1_NIGHT 0x10
//F7 00 00 51 10 21 00 - 50 28 - 02 00 00 4C //fault lowbat
//F7 00 00 07 10 16 00 - 12 28 - 02 00 00 20 //check co
//F7 00 00 07 10 03 00 - 00 28 - 02 00 00 46 //fault open
//F7 00 00 40 00 08 00 - 5C 28 - 02 00 00 33 //system ready low bat
//F7 00 00 20 00 08 00 - 4C 28 - 02 00 00 53 /system not ready low bat
//F7 00 00 03 10 08 00 - CC 28 - 02 00 00 31 // system arming stay
//F7 00 00 03 10 08 00 - CC 28 - 02 00 00 31 // system armed stay
//F7 00 00 40 00 BF 00 - 12 28 - 02 00 00 43 //system check 103 ready
//F7 00 00 20 00 BF 04 - 02 28 - 02 00 00 43 //system check 103 not ready
//F7 00 00 20 00 04 01 - 50 38 - 02 00 00 42 // zone bypass
//F7 00 00 03 10 17 00 - 80 2B - 02 00 00 41 // alarm zone 17 , in alarm
//F7 00 00 02 00 08 00 - 8C 28 - 02 00 00 44 //entry when armed
//F7 00 00 03 10 17 00 - 00 2A - 02 00 00 41 // alarm zone 17 , cleared, disarmed
//F7 00 00 03 10 EA 00 - 00 2A - 02 00 00 45 // exit alarm
//F7 00 00 03 10 EA 00 - 00 2A - 02 00 00 45 //alarm cancelled
//F7 00 00 07 10 12 00 - 80 08 - 02 00 00 41  //armed stay countdown
#define BIT_MASK_BYTE2_ARMED_HOME 0x80
#define BIT_MASK_BYTE2_LOW_BAT 0x40
#define BIT_MASK_BYTE2_ZONE_FIRE 0x20
#define BIT_MASK_BYTE2_READY 0x10
#define BIT_MASK_BYTE2_UNKNOWN 0x08
#define BIT_MASK_BYTE2_SYSTEM_FLAG 0x04
#define BIT_MASK_BYTE2_CHECK_FLAG 0x02
#define BIT_MASK_BYTE2_FIRE 0x01

#define BIT_MASK_BYTE3_INSTANT 0x80
#define BIT_MASK_BYTE3_PROGRAM 0x40
#define BIT_MASK_BYTE3_CHIME_MODE 0x20
#define BIT_MASK_BYTE3_BYPASS 0x10
#define BIT_MASK_BYTE3_AC_POWER 0x08
#define BIT_MASK_BYTE3_ARMED_AWAY 0x04
#define BIT_MASK_BYTE3_ZONE_ALARM 0x02
#define BIT_MASK_BYTE3_IN_ALARM 0x01

#define F7_MESSAGE_LENGTH 45
#define N98_MESSAGE_LENGTH 6

#define MAX_MODULES 9

// enum ecpState { sPulse, sNormal, sAckf7,sSendkpaddr,sPolling };
#define sPulse 1
#define sNormal 2
#define sAckf7 3
#define sSendkpaddr 4
#define sPolling 5
#define sCmdHigh 6

struct statusFlagType
{
    char beeps : 3;
    uint8_t armedStay : 1;
    uint8_t armedAway : 1;
    uint8_t night : 1;
    uint8_t instant : 1;
    uint8_t chime : 1;
    uint8_t acPower : 1;
    uint8_t acLoss : 1;
    uint8_t ready : 1;
    uint8_t entryDelay : 1;
    uint8_t programMode : 1;
    uint8_t zoneBypass : 1;
    uint8_t zoneAlarm : 1;
    uint8_t alarm : 1;
    uint8_t check : 1;
    uint8_t systemFlag : 1;
    uint8_t lowBattery : 1;
    uint8_t systemTrouble : 1;
    uint8_t fire : 1;
    uint8_t fireZone : 1;
    uint8_t backlight : 1;
    uint8_t armed : 1;
    uint8_t away : 1;
    uint8_t bypass : 1;
    uint8_t inAlarm : 1;
    uint8_t noAlarm : 1;
    uint8_t exitDelay : 1;
    uint8_t cancel : 1;
    uint8_t fault : 1;
    uint8_t panicAlarm : 1;
    char keypad[4];
    int zone;
    char prompt1[18];
    char prompt2[18];
    char promptPos;
    uint8_t attempts = 10;
    struct
    {
        int code;
        uint8_t qual;
        int data;
        uint8_t partition;
    } lrr;
};

struct expanderType
{
    char expansionAddr;
    char expFault;
    char expFaultBits;
    char relayState;
    uint8_t idx;
};
const expanderType expanderType_INIT = {.expansionAddr = 0xFF,.expFault = 0, .expFaultBits = 0, .relayState = 0, .idx = 0};

struct keyType
{
    char key;
    uint8_t kpaddr;
    bool direct;
    uint8_t count;
    uint8_t seq;
};
const keyType keyType_INIT = {.key = 0, .kpaddr = 0, .direct = false, .count = 0, .seq = 0};

struct cmdQueueItem
{
    char cbuf[CMDBUFSIZE];
    char extbuf[CMDBUFSIZE];
    bool newCmd;
    bool newExtCmd;
    size_t size;
    size_t rawsize;
    struct statusFlagType statusFlags;
};
struct rfSerialQueueItem
{
  uint8_t fault;
  uint32_t serial; 
  uint8_t idx; 
};
//const cmdQueueItem cmdQueueItem_INIT = {.newCmd = false, .newExtCmd = false,.size=0,.rawsize=0};

class Vista
{

public:
    Vista();
    ~Vista();
    void begin(int receivePin, int transmitPin, char keypadAddr, int monitorTxPin, bool invertRx = true, bool invertTx = true, bool invertMon = true, uint8_t inputRx = INPUT, uint8_t inputMon = INPUT);
    void stop();
    bool handle();
    void printStatus();
    void printTrouble();
    void decodeBeeps();
    void decodeKeypads();
    void printPacket(char *, int);
    void write(const char *);
    void write(const char);
    void write(const char *, uint8_t addr);
    void write(const char, uint8_t addr);
    void writeDirect(const char *keys, uint8_t addr, size_t len);
    void writeDirect(const char key, uint8_t addr, uint8_t seq = 0);
    statusFlagType statusFlags;
    void setKpAddr(char keypadAddr)
    {
        if (keypadAddr > 0)
            _kpAddr = keypadAddr;
    }
    void addModule(uint8_t addr);
    void gpioISRHandler();
    void rxHandleISR();

    void txHandleISR();
    bool areEqual(char *, char *, uint8_t);
    bool keybusConnected, connected;
    // Bench diagnostic (VISTA-Panel-Tool): SoftwareSerial::overflow() is
    // private to vistaSerial, which is itself a private member here --
    // expose it so the sketch can check whether the main-loop-driven
    // readChars() is draining the ISR-filled ring buffer fast enough
    // during a long (44-byte) F7 read. Clears the underlying sticky flag
    // on read, same as SoftwareSerial::overflow() itself.
    bool rxOverflow();
    int toDec(int);
    void resetStatus();
    void initSerialHandlers(int, int, int);


    bool lrrSupervisor;
    void setExpFault(int, bool);
    void setRFFault(uint8_t fault,uint32_t serial);
    bool charAvail();
    bool cmdAvail();
    cmdQueueItem * getNextCmd();
    bool sendPending();
    void set_rf_emulation(bool emulate);
    void set_rf_addr(uint8_t addr);
    bool get_rf_emulation();

    // Bench diagnostic (VISTA-Panel-Tool): the real outgoing-keypress
    // announce (addrToBitmask1/2/3 triplet on the Green TX pin) only ever
    // fires from inside rxHandleISR(), gated on detecting a real >9ms low
    // pulse on Yellow -- a pattern normally produced by the panel's own bus
    // timing. With no panel connected, whatever edges show up on that
    // floating line aren't reliably producing that pattern, so there's no
    // guarantee the announce code ever runs at all. This calls the
    // identical write sequence directly, triggered by the .ino sketch on a
    // plain timer instead of the Yellow-wire state machine, so Green TX
    // gets exercised on a predictable schedule regardless of whether a
    // panel is present at all.
    //
    // This diagnostic is what found the actual root cause of "nothing
    // happens" on the bench: with the panel disconnected and Green TX
    // isolated from the base-drive transistor, a scope caught nothing at
    // all on GP27 (the pin originally assigned to this signal) across many
    // forced-announce attempts, but caught a clean, correctly bit-shaped
    // burst on GP1 with the identical firmware -- pointing to a dead GP27
    // GPIO driver, not a protocol or timing bug. Green TX now lives on GP1
    // (see HARDWARE_ARCHITECTURE.md's pin table).
    void debugForceKeyAnnounce();

    // std::queue<struct cmdQueueItem> cmdQueue;

private:
#ifndef ESP32
    static uint32_t m_savedPS;
#else
    static portMUX_TYPE m_interruptsMux;
#endif
    static void disableInterrupts();
    static void restoreInterrupts();
#if defined(USE_RP2040)
    // See ecp_uart_rx.pio: moves byte assembly for the primary RX pin
    // (Yellow/_rxPin) off the CPU-interrupt-driven software bit sampler
    // and onto a PIO state machine, which samples GPIO with hardware
    // timing immune to CPU load -- bench-confirmed necessary since the
    // software path reliably loses sync once real bus traffic (e.g. a
    // physical keypad in use) overlaps a long read. Drains the PIO RX
    // FIFO into vistaSerial's existing byte buffer via pushByte(),
    // gated by the same _rxState/_highTime check rxHandleISR() already
    // used before calling vistaSerial->rxRead() -- PIO free-runs
    // regardless of bus state, so bytes framed during a poll/preamble/ACK
    // window (not real data) are drained and discarded rather than fed
    // to the decoder. The Green monitor pin and TX are untouched -- both
    // still use the original software path, since neither has shown this
    // problem.
    void pioRxInit();
    void pioRxPump();
#endif
    SoftwareSerial *vistaSerial, *vistaSerialMonitor;
    bool _newExtCmd, _newCmd;
    bool _filterOwnTx;
    expanderType _zoneExpanders[MAX_MODULES];
    uint8_t _moduleIdx;
    char *_cbuf, *_extbuf, *_extcmd;
    char _lcbuf[14];
    uint8_t _lcbuflen;
    uint8_t _retriesf9;
    char _expectCmd;
    keyType *_outbuf;
    char *_tmpOutBuf;
    cmdQueueItem *_cmdQueue;
    volatile uint8_t _outbufIdx, _inbufIdx;
    uint8_t _outcmdIdx, _incmdIdx;
    int _rxPin, _txPin;
    volatile char _kpAddr;
    char _monitorPin;
    volatile char _rxState;
    volatile unsigned long _lowTime, _highTime;
    volatile bool _pendingAck;
    uint8_t *_faultQueue;
    rfSerialQueueItem *_rfSerialQueue;
    char _expectByte;
    volatile uint8_t _retries;
    volatile uint8_t _retryAddr;
    //volatile bool sending;
    bool _emulate_rf_receiver=false;
    uint8_t _rf_addr=0;
    volatile uint8_t _markPulse;
    volatile int _extIdx;
    uint8_t _writeSeq;
    char _expFault;
    char _expFaultBits;
    bool _invertRead;
    uint8_t _outFaultIdx, _inFaultIdx,_outRfSerialIdx,_inRfSerialIdx;
    volatile bool _is2400;

    void setNextFault(uint8_t);
    void setNextRfSerial(uint8_t fault,uint32_t serial);
    expanderType peekNextFault();
    expanderType getNextFault();
    rfSerialQueueItem peekNextRfSerial();
    rfSerialQueueItem getNextRfSerial();


    void onAUI(char *, int *);
    void onDisplay(char *, int *);
    void writeChars();
    void readChars(int, char *, int *);
    bool validChksum(char *, int, int);
    void readChar(char *, int *);
    void onLrr(char *, int *);
    void onExp(char *);
    void onRF(char *);
    keyType getChar();
    uint8_t peekNextKpAddr();
    size_t decodePacket();
    uint8_t getExtBytes();
    void pushExtBuffer();
    void sendBuffer(char *lcbuf,uint8_t lcbuflen);
    void ckSumSendBuffer(char *lcbuf,uint8_t lcbuflen);

    void pushCmdQueueItem(size_t cmdsize=0,size_t rawsize=0);


    char IRAM_ATTR addrToBitmask1(char addr)
    {
        if (addr > 7)
            return 0xFF;
        else
            return 0xFF ^ (0x01 << (addr));
    }
    char IRAM_ATTR addrToBitmask2(char addr)
    {
        if (addr < 8)
            return 0;
        else if (addr > 16)
            return 0xFF;
        else
            return 0xFF ^ (0x01 << (addr - 8));
    }
    char IRAM_ATTR addrToBitmask3(char addr)
    {
        if (addr < 16)
            return 0;
        else
            return 0xFF ^ (0x01 << (addr - 16));
    }

    void hw_wdt_disable();
    void hw_wdt_enable();



#if defined (USE_ESP_IDF)

unsigned long IRAM_ATTR micros() {
  return (unsigned long)(esp_timer_get_time());
}

unsigned long millis() {
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
 }

#if not defined(NOP)
#define NOP __asm__ __volatile__ ("nop\n\t")
#endif
void IRAM_ATTR delayMicroseconds(uint32_t us) {
  uint64_t m = (uint64_t)esp_timer_get_time();
  if (us) {
    uint64_t e = (m + us);
    if (m > e) {  //overflow
      while ((uint64_t)esp_timer_get_time() > e) {
        NOP;
      }
    }
    while ((uint64_t)esp_timer_get_time() < e) {
      NOP;
    }
  }
}
 #endif
};
