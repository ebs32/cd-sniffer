#include "controller.h"
#include "common.h"

// ESP8266
#include "rom/ets_sys.h"
#include "esp8266/timer_struct.h"

// ESP SDK
#include "esp_attr.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

#define STATUS_MASK         0xFFFF
#define BUSY_BIT            1 << 31
#define RUN_TASK(t)         xTaskCreate(t, "ctlAction", 256, NULL, 1, NULL)

// Threshold values for ADC

#define POWER_ON_STATUS_MIN 200
#define POWER_ON_STATUS_MAX 900

// Reads the voltage at ADC pin
extern uint16_t test_tout();

static int32_t     controller_status        = STATUS_WAIT_FOR_POWER | BUSY_BIT;
static const char* controller_status_text[] = {
  "Waiting for Controller PCB to be powered up...",
  "Idle",
  "Resetting...",
  "Moving the optical pickup to the initial position"
};

static void set_status(enum kControllerStatus status) {
  portENTER_CRITICAL();

  controller_status = status;

  portEXIT_CRITICAL();
}

static void IRAM_ATTR check_power() {
  uint16_t v;

  // Read the voltage at ADC pin - It seems interrupts must be disabled...
  {
    portENTER_CRITICAL();

    v = test_tout();

    portEXIT_CRITICAL();
  }

  if (v >= POWER_ON_STATUS_MIN && v <= POWER_ON_STATUS_MAX) {
    if ((controller_status & STATUS_MASK) == STATUS_WAIT_FOR_POWER) {
      set_status(STATUS_IDLE);
    }
  } else {
    set_status(STATUS_WAIT_FOR_POWER | BUSY_BIT);
  }
}

// <-- Controller Actions

static void IRAM_ATTR reset() {
  check_power();

  if (controller_status != STATUS_WAIT_FOR_POWER && !ctl_is_busy()) {
    set_status(STATUS_RESET_IN_PROGRESS | BUSY_BIT);

    frc1.ctrl.en   = 0;
    frc1.load.data = US_TO_TICKS(1000000);
    frc1.ctrl.en   = 1;

    SET_LO(XRST_PORT);

    while (frc1.ctrl.en == 1) {
      soc_wait_int();
    }

    SET_HI(XRST_PORT);

    set_status(STATUS_IDLE);
  }

  vTaskDelete(NULL);
}

static void IRAM_ATTR move_pickup_to_initial_position() {
  check_power();

  if (controller_status != STATUS_WAIT_FOR_POWER && !ctl_is_busy()) {
    set_status(STATUS_PICKUP_TO_INITIAL_POSITION | BUSY_BIT);

    // TODO

    vTaskDelay(250 / portTICK_RATE_MS);
    set_status(STATUS_IDLE);
  }

  vTaskDelete(NULL);
}

// Controller Actions -->

uint16_t ctl_get_status() {
  check_power();

  return controller_status & STATUS_MASK;
}

const char* ctl_get_status_text() {
  return controller_status_text[controller_status & STATUS_MASK];
}

bool ctl_is_busy() {
  return (controller_status & BUSY_BIT) != 0;
}

void ctl_reset() {
  RUN_TASK(reset);
}

void ctl_move_pickup_to_initial_position() {
  RUN_TASK(move_pickup_to_initial_position);
}
