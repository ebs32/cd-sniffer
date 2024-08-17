#pragma once

#include "driver/gpio.h"

// GPIO Mappings - By default, GPIO1 and GPIO3 are reserved for UART
#define XRST_PORT       GPIO_NUM_16 // D0
#define LED_PORT        GPIO_NUM_2  // D4
#define CLK_PORT        GPIO_NUM_14 // D5
#define XLT_PORT        GPIO_NUM_12 // D6
#define DATA_PORT       GPIO_NUM_13 // D7

// Macros for setting the level of the GPIO ports - I found it is way faster to
// use this approach rather than writing to the GPIO struct
#define SPI_REG_BASE    0x60000200
#define SET_LO(n)       WRITE_PERI_REG(SPI_REG_BASE + 0x108 /* L */, 1 << n)
#define SET_HI(n)       WRITE_PERI_REG(SPI_REG_BASE + 0x104 /* H */, 1 << n)

// Macro for converting a value given in uS to ticks for the FRC Timer
#define US_TO_TICKS(t)  ((80000000 >> frc1.ctrl.div) / 1000000) * t

// Threshold values for ADC
#define POWER_ON_STATUS_MIN 100
#define POWER_ON_STATUS_MAX 900

// Reads the voltage at ADC pin
extern uint16_t test_tout();
