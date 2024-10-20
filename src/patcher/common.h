#pragma once

#include "driver/gpio.h"

// GPIO Mappings
//
// - GPIO1 and GPIO3 are externally connected to the UART interface of CH340C
// - GPIO16 is internally connected to RTC

#define DATA_CPU        GPIO_NUM_16 // D0 (RTC)
#define CLK_CPU         GPIO_NUM_5  // D1
#define XLT_CPU         GPIO_NUM_4  // D2
#define SENS_PORT       GPIO_NUM_0  // D3
#define LED_PORT        GPIO_NUM_2  // D4
#define XLT_CONTROLLER  GPIO_NUM_12 // D6
#define SWITCH_PORT     GPIO_NUM_15 // D8

// Macros for setting the level of the GPIO ports - I found it is way faster to
// use those rather than writing to the GPIO struct

#define SPI_REG_BASE    0x60000200

// Sets GPIO output LO (GND)
#define SET_LO(n)       if (n == GPIO_NUM_16) { \
                          CLEAR_PERI_REG_MASK(RTC_GPIO_OUT, 0x1); \
                        } else { \
                          WRITE_PERI_REG(SPI_REG_BASE + 0x108 /* L */, 1 << n); \
                        }

// Sets GPIO output HI (VDD)
#define SET_HI(n)       if (n == GPIO_NUM_16) { \
                          SET_PERI_REG_MASK(RTC_GPIO_OUT, 0x1); \
                        } else { \
                          WRITE_PERI_REG(SPI_REG_BASE + 0x104 /* H */, 1 << n); \
                        }

// Macro for converting a value given in uS to ticks for the FRC Timer
// Clock divider must be set to TIMER_CLKDIV_16
#define US_TO_TICKS(t)  ((80000000 >> frc1.ctrl.div) / 1000000) * t

// Delays for X * 25 ns (X < 2048)
#define DELAY(X) \
  __asm__ __volatile__ ( \
    "movi a2, "#X"\n" \
  "loop%=:\n" \
    "addi a2, a2, -1\n" \
    "bnez a2, loop%=\n" \
    : \
    : \
    : "a2" \
  )
