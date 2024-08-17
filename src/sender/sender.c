#include "common.h"
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

#define LED_ON_US   40000
#define LED_OFF_US  800000

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

  gpio_set_direction(XLT_PORT , GPIO_MODE_OUTPUT);
  gpio_set_direction(XRST_PORT, GPIO_MODE_OUTPUT);

  gpio_set_direction(LED_PORT , GPIO_MODE_OUTPUT);
  gpio_pulldown_en  (LED_PORT);
  SET_HI            (LED_PORT);
}

static void configure() {
  portENTER_CRITICAL();

  configure_timer();
  configure_gpio ();

  portEXIT_CRITICAL();
}

void run_sender() {
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

  printf("Waiting for Controller PCB to be powered up...\n");

  while (true) {
    uint16_t v;

    // Read the voltage at ADC pin - It seems interrupts must be disabled...
    {
      portENTER_CRITICAL();

      v = test_tout();

      portEXIT_CRITICAL();
    }

    if (
      v >= POWER_ON_STATUS_MIN &&
      v <= POWER_ON_STATUS_MAX
    ) { // We got power - Exit...
      SET_HI(GPIO_NUM_2);

      break;
    }

    if (frc1.ctrl.en == 0) {
      if (gpio_get_level(GPIO_NUM_2) == 1) {
        SET_LO(GPIO_NUM_2);

        frc1.load.data = US_TO_TICKS(LED_ON_US);
      } else {
        SET_HI(GPIO_NUM_2);

        frc1.load.data = US_TO_TICKS(LED_OFF_US);
      }

      frc1.ctrl.en = 1;
    }
  }

  printf("Got powered up\n");
}
