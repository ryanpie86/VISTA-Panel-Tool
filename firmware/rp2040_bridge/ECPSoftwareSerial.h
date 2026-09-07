/*
SoftwareSerial.h

SoftwareSerial.cpp - Implementation of the Arduino software serial for ESP8266/ESP32.
Copyright (c) 2015-2016 Peter Lerup. All rights reserved.
Copyright (c) 2018-2019 Dirk O. Kaar. All rights reserved.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

Modified for 4800 8E2
*/

#pragma once

// RP2040/Arduino-Pico port (VISTA-Panel-Tool): see vista.h for the
// matching comments. Repeated here (USE_RP2040 and ARDUINO_MQTT both) so
// this header is self-contained if ever included on its own -- and
// because ECPSoftwareSerial.cpp is a separate translation unit from
// vista.cpp, so it doesn't see vista.h's #defines either.
#if defined(ARDUINO_ARCH_RP2040) && !defined(USE_RP2040)
#define USE_RP2040
#endif
#if !defined(ARDUINO_MQTT)
#define ARDUINO_MQTT
#endif

#if not defined(ALWAYS_INLINE_ATTR)
#define ALWAYS_INLINE_ATTR __attribute__((always_inline))
#endif

#include <inttypes.h>

#if defined(USE_ESP_IDF) 
#define ESP32
#include <cstring>
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include <esp_attr.h>
#include <stdio.h>
#include <cstdlib>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
typedef char byte;
#define INPUT           (0x0)
#define OUTPUT          (0x1)
#define INPUT_PULLUP    (0x2)
#define INPUT_PULLDOWN  (0x3)
#define HIGH 1
#define LOW 0
#else
#include <Stream.h>
#include "Arduino.h"
#endif

#include <functional>
#include <atomic>

#if defined(USE_RP2040) && not defined(IRAM_ATTR)
// RP2040/Arduino-Pico port (VISTA-Panel-Tool): upstream's own IRAM_ATTR
// hook (above) just no-ops this on RP2040, which means these hot,
// edge-frequency ISR functions run from flash (XIP) here instead of RAM
// like they do on ESP8266/ESP32 -- losing the timing guarantee IRAM_ATTR
// exists for in the first place (immunity to flash-cache-miss stalls).
// Give RP2040 a real equivalent: GCC's section attribute, prefix-usable
// exactly like the existing "void IRAM_ATTR foo()" call sites already
// are. Anything placed in a ".time_critical*" section is pulled into RAM
// by the Pico SDK's standard linker script -- this is the same mechanism
// the SDK's own __not_in_flash_func() macro uses, just without its
// per-function unique section-name wrapping (not required for the
// linker's placement rule to apply, just for organization).
#define IRAM_ATTR __attribute__((section(".time_critical")))
#endif

#if defined(USE_RP2040) && not defined(ESP)
#define ESP rp2040
#endif

// If only one tx or rx wanted then use this as parameter for the unused pin
constexpr int SW_SERIAL_UNUSED_PIN = -1;

enum SoftwareSerialConfig
{
    SWSERIAL_5N1 = 0,
    SWSERIAL_6N1,
    SWSERIAL_7N1,
    SWSERIAL_8E2 // ecp is 4800 8E2
};

// This class is compatible with the corresponding AVR one,
// the constructor however has an optional rx buffer size.
// Baudrates up to 115200 can be used.

class SoftwareSerial 
{
public:
    SoftwareSerial(int receivePin, int transmitPin, bool invertRx = false, bool invertTx = false, int bufSize = 64, int isrBufSize = 0, int inputRx = INPUT);
    virtual ~SoftwareSerial();

    void begin(int32_t baud = 2400)
    {
        begin(baud, SWSERIAL_8E2);
    }
    void begin(int32_t baud, SoftwareSerialConfig config);
    void setConfig(int32_t baud, SoftwareSerialConfig config);
    void setBaud(int32_t baud);
    // Transmit control pin
    void setTransmitEnablePin(int transmitEnablePin);
    bool overflow();
    bool processSingle = false;

    // RP2040/Arduino-Pico port (VISTA-Panel-Tool): hands a byte assembled
    // elsewhere (a PIO state machine, for the primary RX pin -- see
    // ecp_uart_rx.pio and Vista::pioRxPump()) directly into this class's
    // existing byte-level ring buffer, the same one rxBits() otherwise
    // fills. Reuses the identical overflow-checked insertion rxBits()
    // itself does, so available()/read()/overflow() all keep working
    // completely unchanged for callers -- they can't tell the byte came
    // from PIO instead of this class's own interrupt-driven bit sampler.
    bool pushByte(uint8_t b);

    // RP2040/Arduino-Pico port (VISTA-Panel-Tool): rxHandleISR() calls this
    // instead of rxRead() for the rising edge that ends a bus ACK-slot's
    // sustained low pulse (>9ms) -- see the call site's comment for why.
    // Without it, that edge still reaches rxBits() as an ordinary
    // transition; measured against the timestamp of whatever real edge
    // preceded the ACK slot, a multi-ms gap looks like a very long run of
    // same-level "masked" bits (see rxBits()'s bits/hiddenBits logic), so
    // it force-completes whatever byte was mid-flight with its remaining
    // bits padded from the last known level, then keeps consuming the
    // leftover elapsed time as one or more further synthetic all-zero
    // bytes -- bench-confirmed as the source of F7 reads stalling on a
    // run of zero payload bytes at a fixed offset (wherever the ACK slot
    // falls in the frame). Resetting the bit-tracking state here instead
    // makes the next real edge -- the actual resumption of frame data --
    // get treated as a fresh start bit measured from now, exactly like
    // enableRx(true) primes a fresh read, rather than against a
    // multi-millisecond-stale timestamp.
    void resyncRx();

    int available();
    int peek();
    int read(bool processRxbits);
    int read();
    // size_t write(uint8_t byte, bool parity);
    size_t write(uint8_t b, bool parity, int32_t baud);
    size_t write(uint8_t byte,bool interrupt=true);

    // size_t write(const uint8_t * buffer, size_t size, bool parity);
    // size_t write(const uint8_t *buffer, size_t size) override;
    // operator bool() const
    // {
    //     return m_rxValid || m_txValid;
    // }

    // Disable or enable interrupts on the rx pin
    void enableRx(bool on);
    // One wire control
    void enableTx(bool on);
    uint8_t checkParity(uint8_t b);

    void rxRead();
    void rxSave(bool);

    int bitsAvailable();

    void end();

    bool m_parity = true;
    
    bool isValidGPIOpin(int pin);
    bool debug;

private:


    unsigned long m_bitTime;
    /* check m_rxValid that calling is safe */
    void rxBits();

    static void disableInterrupts();
    static void restoreInterrupts();

    // Member variables
    bool m_oneWire;
    int m_rxPin = SW_SERIAL_UNUSED_PIN;
    int m_txPin = SW_SERIAL_UNUSED_PIN;
    bool m_rxValid = false;
    bool m_rxEnabled = false;
    bool m_txValid = false;
    bool m_invert;
    bool m_invert_tx;
    bool m_invert_rx;
    uint8_t m_input_type;
    bool m_overflow = false;
    int8_t m_dataBits;
    int32_t m_bitCycles;
    int16_t m_4800_bitCycles;
    int m_inPos, m_outPos;
    int m_bufSize = 0;
    uint8_t *m_buffer = 0;
    // the ISR stores the relative bit times in the buffer. The inversion corrected level is used as sign bit (2's complement):
    // 1 = positive including 0, 0 = negative.
    std::atomic<int> m_isrInPos,
        m_isrOutPos;
    int m_isrBufSize = 0;
    std::atomic<uint32_t> *m_isrBuffer;
    std::atomic<bool> m_isrOverflow;
    std::atomic<uint32_t> m_isrLastCycle;
    int m_rxCurBit; // 0 - 7: data bits. -1: start bit. 8: stop bit.
    uint8_t m_rxCurByte = 0;
#ifndef ESP32
    static uint32_t m_savedPS;
#else
    static portMUX_TYPE m_interruptsMux;
#endif

    static inline unsigned long  IRAM_ATTR microsToTicks(unsigned long micros) ALWAYS_INLINE_ATTR
    {
        // return (ESP.getCpuFreqMHz() * micros) << 1;
       // return ((esp_clk_cpu_freq()/1000000) * micros) << 1;
    //    #if defined(USE_ARDUINO) && defined(CONFIG_IDF_TARGET_ESP32)
    //    return (ESP.getCpuFreqMHz() * micros) ;
    //    #else
       return micros << 1;
    //    #endif
    }
   static inline unsigned long ticksToMicros(unsigned long ticks) ALWAYS_INLINE_ATTR
    {
        return ticks >> 1;
    }

    static inline unsigned long IRAM_ATTR ticks()  ALWAYS_INLINE_ATTR
    {
// #if defined(USE_ARDUINO) && defined(CONFIG_IDF_TARGET_ESP32)
// return ESP.getCycleCount() ;
// #else

#if defined(ESP32) or defined(USE_ESP_IDF)
    //return esp_cpu_get_cycle_count() << 1;
       return (unsigned long) esp_timer_get_time()  << 1;

#else
        return micros() << 1;
       
#endif

//#endif
    }

void IRAM_ATTR digitalWritePin(int pin, int val) {
    #if defined(USE_ESP_IDF) or defined(ESP32)
        gpio_set_level((gpio_num_t)pin, val);
    #else
        digitalWrite(pin,val);
    #endif

}



#if defined(USE_ESP_IDF)


#if not defined(NOP)
#define NOP __asm__ __volatile__ ("nop\n\t")
#endif
 void delayMicroseconds(uint32_t us)
{
    uint32_t m = esp_timer_get_time();
    if(us){
        uint32_t e = (m + us);
        if(m > e){ //overflow
            while(esp_timer_get_time() > e){
                NOP;
            }
        }
        while(esp_timer_get_time() < e){
            NOP;
        }
    }
}

#endif
};

