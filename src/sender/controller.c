#include "controller.h"
#include "common.h"

// ESP8266
#include "rom/ets_sys.h"
#include "esp8266/spi_struct.h"
#include "esp8266/timer_struct.h"
#include "driver/gpio.h"

// ESP SDK
#include "esp_attr.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

// Reads the voltage at ADC pin
extern uint16_t test_tout();

// Threshold values for ADC

#define POWER_ON_STATUS_MIN 200
#define POWER_ON_STATUS_MAX 800
#define PICKUP_LIMIT_SW_MIN 350
#define PICKUP_LIMIT_SW_MAX 500

#define LED_ON_US            40000 // The time the LED must be ON
#define LED_OFF_US          800000 // The time the LED must be OFF

// A few macros/helpers for dealing with the status

#define STATUS_MASK         0x0000FFFF
#define BUSY_BIT            (1 << 31)
#define IS_BUSY(s)          ((s & BUSY_BIT) != 0)
#define IS_POWERED(s)       ((s & STATUS_MASK) != S_WAIT_FOR_POWER)
#define STATUS_TEXT(s)      controller_status_text[s & STATUS_MASK]

// A macro for creating a task for running an action
#define RUN_TASK(t)         xTaskCreate(run_action, "ctlAction", 1024, t, 1, NULL)

// Maximum number of registered listeners allowed
#define MAX_LISTENERS       2

// The timeout for operations
#define OPERATION_TIMEOUT_S 5

// Delay between MICOM commands
#define MICOM_CMD_DELAY_MS  50

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

enum kControllerStatus {
  S_WAIT_FOR_POWER = 0,
  S_IDLE,
  S_ERROR_TIMED_OUT,
  S_UNEXPECTED_ERROR,
  S_NO_DISC,
  S_PLAYING,
  S_PAUSED,
  S_RESET_IN_PROGRESS,
  S_PICKUP_TO_INITIAL_POSITION,
  S_PICKUP_MOVE_BACKWARDS,
  S_TESTING_TRACKING_COIL,
  S_TESTING_SLED_MOTOR,
  S_TESTING_FOCUS_COIL,
  S_TESTING_SPINDLE_MOTOR,
  S_LOOKING_FOR_DISC,
  S_RUNNING_MICOM_COMMANDS,
};

typedef int32_t ctl_status;

static ctl_status     controller_status        = S_WAIT_FOR_POWER;
static const char*    controller_status_text[] = {
  "Waiting for Controller PCB to be powered up...",
  "Idle",
  "Idle - The last action timed out",
  "Idle - The last action found an unexpected error",
  "Idle - No disc detected",
  "Idle - Playing disc...",
  "Idle - Paused...",
  "Resetting...",
  "Moving the optical pickup to the initial position...",
  "Moving the optical pickup backwards...",
  "Testing tracking coil...",
  "Testing sled motor...",
  "Testing focus coil...",
  "Testing spindle motor...",
  "Looking for disc...",
  "Running MICOM commands...",
};
static size_t         n_listeners              = 0;
static ctl_listener_t listeners[MAX_LISTENERS];

static size_t         micom_task_n             = 0;
static uint16_t*      micom_task_commands      = NULL;

static void IRAM_ATTR notify_update(ctl_status status) {
  TEvent event;

  event.is_busy     = IS_BUSY    (status);
  event.is_powered  = IS_POWERED (status);
  event.status_text = STATUS_TEXT(status);

  for (size_t i = 0; i < n_listeners; i++) {
    listeners[i](&event);
  }
}

static void IRAM_ATTR set_status(ctl_status status) {
  ctl_status current_status;

  portENTER_CRITICAL();

  current_status    = controller_status;
  controller_status = status;

  portEXIT_CRITICAL();

  if (status != current_status) {
    notify_update(status);
  }
}

static bool IRAM_ATTR try_lock() {
  bool is_ready;

  portENTER_CRITICAL();

  is_ready           = IS_POWERED(controller_status) && !IS_BUSY(controller_status);
  controller_status |= BUSY_BIT;

  portEXIT_CRITICAL();

  return is_ready;
}

static void IRAM_ATTR run_action(void* action_fn) {
  if (try_lock()) {
    ((void(*)()) action_fn)();
  }

  vTaskDelete(NULL);
}

// <-- Controller Actions

static void IRAM_ATTR send(uint16_t command) {
  // Enable the command phase
  SPI1.user.usr_command         = 1;

  // Set the command and the length
  SPI1.user2.usr_command_value  = command;
  SPI1.user2.usr_command_bitlen = command <= 0xFF
                                ? 7
                                : command <= 0xFFF
                                ? 11
                                : 15;

  // Start the operation
  SPI1.cmd.usr                  = 1;

  // Wait for the operation to complete
  while (SPI1.cmd.usr == 1);

  // Trigger the latch signal - At this point the minimum time required for
  // enabling the latch signal has lapsed

  portENTER_CRITICAL();

  SET_LO(XLT_PORT);
  DELAY (40);

  SET_HI(XLT_PORT);

  portEXIT_CRITICAL();
}

static void IRAM_ATTR reset() {
  set_status(S_RESET_IN_PROGRESS | BUSY_BIT);

  // Reset both ICs
  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  // Disable the reset line and send few commands that apparently are required
  // or the SERVO IC behaves erratically
  SET_HI(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  send(0x00); // Stop focus servo
  send(0x10); // Reset tracking control
  send(0x20); // Stop both tracking and sled servos
  send(0x40); // Cancel any auto-sequence command
  send(0xe0); // Stop CLV

  // Keep both ICs in reset state
  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  set_status(S_IDLE);
}

static void IRAM_ATTR move_pickup_to_initial_position() {
  uint8_t t = 0;

  set_status(S_PICKUP_TO_INITIAL_POSITION | BUSY_BIT);

  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  SET_HI(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  send(0x23); // Reverse kick

  frc1.load.data = US_TO_TICKS(1000000);
  frc1.ctrl.en   = 1;

  while (t < OPERATION_TIMEOUT_S) {
    vTaskSuspendAll();
    uint16_t v = test_tout();
    xTaskResumeAll ();

    if (v >= PICKUP_LIMIT_SW_MIN && v <= PICKUP_LIMIT_SW_MAX) {
      break;
    }

    if (frc1.ctrl.en == 0 && ++t < OPERATION_TIMEOUT_S) {
      frc1.load.data = US_TO_TICKS(1000000);
      frc1.ctrl.en   = 1;
    }
  }

  send(0x00);
  send(0x10);
  send(0x20);
  send(0x40);
  send(0xe0);

  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  set_status(t == OPERATION_TIMEOUT_S ? S_ERROR_TIMED_OUT : S_IDLE);
}

static void IRAM_ATTR move_pickup_to_initial_position_then_move_it_back() {
  uint8_t t = 0;

  set_status(S_PICKUP_TO_INITIAL_POSITION | BUSY_BIT);

  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  SET_HI(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  send(0x23); // Reverse kick

  frc1.load.data = US_TO_TICKS(1000000);
  frc1.ctrl.en   = 1;

  while (t < OPERATION_TIMEOUT_S) {
    vTaskSuspendAll();
    uint16_t v = test_tout();
    xTaskResumeAll ();

    if (v >= PICKUP_LIMIT_SW_MIN && v <= PICKUP_LIMIT_SW_MAX) {
      break;
    }

    if (frc1.ctrl.en == 0 && ++t < OPERATION_TIMEOUT_S) {
      frc1.load.data = US_TO_TICKS(1000000);
      frc1.ctrl.en   = 1;
    }
  }

  if (t < OPERATION_TIMEOUT_S) {
    send(0x20); // Stop sled servo

    set_status(S_PICKUP_MOVE_BACKWARDS | BUSY_BIT);

    send(0x22); // Forward kick

    frc1.load.data = US_TO_TICKS(1000000);
    frc1.ctrl.en   = 1;

    while (frc1.ctrl.en == 1);
  }

  send(0x00);
  send(0x10);
  send(0x20);
  send(0x40);
  send(0xe0);

  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  set_status(t == OPERATION_TIMEOUT_S ? S_ERROR_TIMED_OUT : S_IDLE);
}

static void IRAM_ATTR run_test_coils_and_motors() {
  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  SET_HI(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  // Move the lens along the X axis (tracking)
  set_status(S_TESTING_TRACKING_COIL | BUSY_BIT);

  for (size_t i = 0; i < 4; i++) {
    send(0x20);
    vTaskDelay(100 / portTICK_RATE_MS);

    send(0x2C);
    vTaskDelay(100 / portTICK_RATE_MS);

    send(0x20);
    vTaskDelay(100 / portTICK_RATE_MS);

    send(0x28);
    vTaskDelay(100 / portTICK_RATE_MS);
  }

  send(0x20);

  // Move the optical pickup along the X axis (sled motor)
  set_status(S_TESTING_SLED_MOTOR | BUSY_BIT);

  for (size_t i = 0; i < 4; i++) {
    send(0x23);
    vTaskDelay(100 / portTICK_RATE_MS);

    send(0x20);
    vTaskDelay(100 / portTICK_RATE_MS);

    send(0x22);
    vTaskDelay(100 / portTICK_RATE_MS);

    send(0x20);
    vTaskDelay(100 / portTICK_RATE_MS);
  }

  send(0x20);

  // Move the lens up and down (focus)
  set_status(S_TESTING_FOCUS_COIL | BUSY_BIT);

  for (size_t i = 0; i < 4; i++) {
    send(0x47);
    vTaskDelay(500 / portTICK_RATE_MS);

    send(0x40);
    vTaskDelay(500 / portTICK_RATE_MS);
  }

  // Move the spindle motor in both directions
  set_status(S_TESTING_SPINDLE_MOTOR | BUSY_BIT);

  send(0xe8);
  vTaskDelay(1000 / portTICK_RATE_MS);

  send(0xe0);
  vTaskDelay(1000 / portTICK_RATE_MS);

  send(0xea);
  vTaskDelay(1000 / portTICK_RATE_MS);

  send(0xe0);
  vTaskDelay(1000 / portTICK_RATE_MS);

  send(0x00);
  send(0x10);
  send(0x20);
  send(0x40);
  send(0xe0);

  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  set_status(S_IDLE);
}

static void IRAM_ATTR play() {
  uint32_t status = controller_status & STATUS_MASK;

  if (status != S_PLAYING && status != S_PAUSED) {
    bool got_focus = false;
    bool got_error = false;

    SET_LO(XRST_PORT);
    vTaskDelay(10 / portTICK_RATE_MS);

    SET_HI(XRST_PORT);
    vTaskDelay(10 / portTICK_RATE_MS);

    set_status(S_LOOKING_FOR_DISC | BUSY_BIT);

    // Adjust focus error bias
    send(0x878);
    send(0x87f);
    send(0x841);

    vTaskDelay(100 / portTICK_RATE_MS);

    if (gpio_get_level(SENS_PORT) == 0) {
      got_error = true;

      goto reset;
    }

    // Adjust focus servo offset cancel
    send(0x08 );

    send(0x867);
    vTaskDelay(200 / portTICK_RATE_MS);

    send(0x86f);
    send(0x842);

    vTaskDelay(100 / portTICK_RATE_MS);

    if (gpio_get_level(SENS_PORT) == 0) {
      got_error = true;

      goto reset;
    }

    // Laser on
    send(0x854);

    // Look for focus
    for (size_t i = 0; i < 3 && !got_focus; i++) {
      send(0x47);
      vTaskDelay(500 / portTICK_RATE_MS);

      got_focus = gpio_get_level(FOK_PORT) == 1;
    }

    if (!got_focus) {
      goto reset;
    }

    send(0x99 ); // Set CNTL-Z register
    send(0xae ); // Set CNTL-S register
    send(0xe6 ); // Set CNTL-C register
    send(0x20 ); // Disable tracking and sled servos
    send(0x08 ); // Enable focus
    send(0x844); // Set tracking balance
    send(0x80b);
    send(0x848); // Set tracking gain
    send(0x827);
    send(0x840);
    send(0x25 ); // Enable tracking and sled servos
    send(0x18 ); // Enable anti-shock and release the brake

    goto done;

reset:
    send(0x00);
    send(0x10);
    send(0x20);
    send(0x40);
    send(0xe0);

    SET_LO(XRST_PORT);
    vTaskDelay(10 / portTICK_RATE_MS);

done:
    if (got_error) {
      set_status(S_UNEXPECTED_ERROR);
    } else {
      set_status(got_focus ? S_PLAYING : S_NO_DISC);
    }
  } else {
    if (status == S_PLAYING) {
      send(0x20);

      set_status(S_PAUSED);
    } else {
      send(0x25);

      set_status(S_PLAYING);
    }
  }
}

static void IRAM_ATTR stop() {
  uint32_t status = controller_status & STATUS_MASK;

  if (status == S_PLAYING || status == S_PAUSED) {
    send(0x85c); // Laser Off

    move_pickup_to_initial_position();
  }
}

static void IRAM_ATTR tune_tracking() {
  bool got_focus = false;
  bool got_error = false;

  move_pickup_to_initial_position();

  SET_HI(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  set_status(S_LOOKING_FOR_DISC | BUSY_BIT);

  // Adjust focus error bias
  send(0x878);
  send(0x87f);
  send(0x841);

  vTaskDelay(100 / portTICK_RATE_MS);

  if (gpio_get_level(SENS_PORT) == 0) {
    got_error = true;

    goto reset;
  }

  // Adjust focus servo offset cancel
  send(0x08 );

  send(0x867);
  vTaskDelay(200 / portTICK_RATE_MS);

  send(0x86f);
  send(0x842);

  vTaskDelay(100 / portTICK_RATE_MS);

  if (gpio_get_level(SENS_PORT) == 0) {
    got_error = true;

    goto reset;
  }

  // Laser on
  send(0x854);

  // Look for focus
  for (size_t i = 0; i < 3 && !got_focus; i++) {
    send(0x47);
    vTaskDelay(500 / portTICK_RATE_MS);

    got_focus = gpio_get_level(FOK_PORT) == 1;
  }

  if (!got_focus) {
    goto reset;
  }

  // Set the environment
  send(0x08 );
  send(0xe8 );
  send(0x20 );
  send(0x830);

  // At this point, the user can run MICOM commands to find the right tracking
  // balance and gain values

  goto done;

reset:
  send(0x00);
  send(0x10);
  send(0x20);
  send(0x40);
  send(0xe0);

  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

done:
  if (got_error) {
    set_status(S_UNEXPECTED_ERROR);
  } else {
    set_status(got_focus ? S_IDLE : S_NO_DISC);
  }
}

static void IRAM_ATTR run_micom_commands() {
  set_status(S_RUNNING_MICOM_COMMANDS | BUSY_BIT);

  SET_LO(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  SET_HI(XRST_PORT);
  vTaskDelay(10 / portTICK_RATE_MS);

  for (size_t i = 0; i < micom_task_n; i++) {
    send(micom_task_commands[i]);

    vTaskDelay(MICOM_CMD_DELAY_MS / portTICK_RATE_MS);
  }

  set_status(S_IDLE);

  // This task requires custom actions before disposing it
  free(micom_task_commands);

  vTaskDelete(NULL);
}

// Controller Actions -->

static void IRAM_ATTR check_pwr_task() {
  uint16_t v;

  while (true) {
    // If the controller is not busy running an operation then check whether it
    // is still powered up or not and update the status accordingly
    if (!IS_BUSY(controller_status)) {
      vTaskSuspendAll();
      v = test_tout();
      xTaskResumeAll ();

      if (v >= POWER_ON_STATUS_MIN && v <= POWER_ON_STATUS_MAX) {
        if (!IS_POWERED(controller_status)) {
          // Make sure LED is off
          SET_HI(LED_PORT);

          // Reset the controller - This will switch to IDLE status
          reset();
        }
      } else if (IS_POWERED(controller_status)) {
        printf("ADC = %d\n", v);

        set_status(S_WAIT_FOR_POWER);
      }

      if (!IS_POWERED(controller_status) && frc1.ctrl.en == 0) {
        // It is safe to use the FRC timer at this point as if we are here it
        // means the controller board has no power

        if (gpio_get_level(LED_PORT) == 1) {
          SET_LO(LED_PORT);

          frc1.load.data = US_TO_TICKS(LED_ON_US);
        } else {
          SET_HI(LED_PORT);

          frc1.load.data = US_TO_TICKS(LED_OFF_US);
        }

        frc1.ctrl.en = 1;
      }
    }

    if (IS_POWERED(controller_status)) {
      vTaskDelay(500 / portTICK_RATE_MS);
    } else {
      portYIELD();
    }
  }
}

void ctl_start() {
  xTaskCreate(check_pwr_task, "ctlTask", 1024, NULL, 1, NULL);
}

int32_t ctl_add_listener(const ctl_listener_t listener_fn) {
  TEvent event;

  portENTER_CRITICAL();

  if (n_listeners == MAX_LISTENERS) {
    return -1;
  }

  listeners[n_listeners++] = listener_fn;

  event.is_busy     = IS_BUSY    (controller_status);
  event.is_powered  = IS_POWERED (controller_status);
  event.status_text = STATUS_TEXT(controller_status);

  portEXIT_CRITICAL();

  listener_fn(&event);

  return 0;
}

void ctl_reset() {
  RUN_TASK(reset);
}

void ctl_move_pickup_to_initial_position() {
  RUN_TASK(move_pickup_to_initial_position);
}

void ctl_move_pickup_to_initial_position_then_move_it_back() {
  RUN_TASK(move_pickup_to_initial_position_then_move_it_back);
}

void ctl_run_test_coils_and_motors() {
  RUN_TASK(run_test_coils_and_motors);
}

void ctl_play() {
  RUN_TASK(play);
}

void ctl_stop() {
  RUN_TASK(stop);
}

void ctl_tune_tracking() {
  RUN_TASK(tune_tracking);
}

void ctl_run_micom_commands(size_t n, uint16_t* commands) {
  if (try_lock()) {
    micom_task_n        = n;
    micom_task_commands = commands;

    xTaskCreate(run_micom_commands, "ctlAction", 1024, NULL, 1, NULL);
  }
}
