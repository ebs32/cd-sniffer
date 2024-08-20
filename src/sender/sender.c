#include "actions.h"
#include "common.h"
#include "controller.h"
#include "wifi.h"

// ESP8266
#include "rom/ets_sys.h"
#include "esp8266/timer_struct.h"
#include "driver/gpio.h"
#include "driver/hw_timer.h"

// ESP SDK
#include "esp_attr.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

#define LED_ON_US        40000 // The time the LED must be ON
#define LED_OFF_US      800000 // The time the LED must be OFF
#define BUFFER_READ_MS     250 // Time period between input buffer reads

static void IRAM_ATTR frc_timer_isr_cb() {
  frc1.ctrl.en = 0;
}

static void configure_timer() {
  /* The callback for the timer is required as the handler will enter into an
   * infinite loop if the interrupt is not cleared
   */
  _xt_isr_unmask(1 << ETS_FRC_TIMER1_INUM);
  _xt_isr_attach(ETS_FRC_TIMER1_INUM, frc_timer_isr_cb, 0);

  TM1_EDGE_INT_ENABLE();

  frc1.ctrl.div       = TIMER_CLKDIV_16;
  frc1.ctrl.intr_type = TIMER_EDGE_INT;
  frc1.ctrl.reload    = 0;
  frc1.ctrl.en        = 0;
}

static void configure_gpio() {
  PIN_PULLUP_EN  (PERIPHS_IO_MUX_MTDI_U);
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, /* XLT_PORT */ FUNC_GPIO12);

  gpio_set_direction(LED_PORT , GPIO_MODE_OUTPUT);
  SET_HI(LED_PORT);

  gpio_set_direction(XLT_PORT , GPIO_MODE_OUTPUT);
  SET_HI(XLT_PORT);

  gpio_set_direction(XRST_PORT, GPIO_MODE_OUTPUT);
  SET_LO(XRST_PORT);
}

static void configure() {
  portENTER_CRITICAL();

  configure_timer();
  configure_gpio ();

  portEXIT_CRITICAL();
}

static void show_menu() {
  printf("\n");

  for (size_t i = 0; i < sizeof(actions) / sizeof(TAction); i++) {
    printf("%c. %s\n", actions[i].id, actions[i].description);
  }

  printf("\n");
}

static void process_option(char option) {
  for (size_t i = 0; i < sizeof(actions) / sizeof(TAction); i++) {
    if (option == actions[i].id) {
      actions[i].fn();

      break;
    }
  }
}

void run_sender() {
  int32_t status = -1;

  configure();

  // Install the new vector
  __asm__ __volatile__(
    "movi   a0,  VectorBase\n"
    "wsr    a0,  vecbase\n"
    :
    :
    : "memory"
  );

#if 1
  if (start_wifi() != 0) {
    printf("Failed to initialize the WiFi service\n");
  }
#endif

  while (true) {
    // If the status has changed then print the friendly description of the new
    // status and show the menu iif the controller is ready
    if (status != ctl_get_status()) {
      status = ctl_get_status();

      printf("\033[1mStatus\033[22m: %s\n", ctl_get_status_text());

      if (!ctl_is_busy()) {
        show_menu();

        // Clear input buffer after an action has been completed
        while ((fgetc(stdin)) != EOF);
      }
    }

    // If the controller is not busy then read the input buffer and execute the
    // requested action if the buffer is not empty
    if (!ctl_is_busy()) {
      int option = fgetc(stdin);

      if (option != EOF) {
        process_option(option);
      }

      vTaskDelay(BUFFER_READ_MS / portTICK_RATE_MS);
    }

    if (status == STATUS_WAIT_FOR_POWER && frc1.ctrl.en == 0) {
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

    portYIELD();
  }
}
