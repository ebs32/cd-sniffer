#include "common.h"
#include "wifi.h"

// ESP8266
#include "rom/ets_sys.h"
#include "esp8266/spi_struct.h"
#include "esp8266/gpio_struct.h"
#include "esp8266/timer_struct.h"
#include "driver/gpio.h"
#include "driver/hw_timer.h"

// ESP SDK
#include "esp_attr.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

#define LED_ON_US    40000 // The time the LED must be ON
#define LED_OFF_US  800000 // The time the LED must be OFF

uint16_t DRAM_ATTR tracking_balance = 0x80a;
uint16_t DRAM_ATTR tracking_gain    = 0x827;

static uint16_t     DRAM_ATTR timer_start_wifi;
static TaskHandle_t DRAM_ATTR user_task_handle;

static void IRAM_ATTR timer_handler() {
  frc1.ctrl.en = 0;

  if (timer_start_wifi == 1) {
    timer_start_wifi = 0;

    vTaskResume(user_task_handle);
  }
}

static void IRAM_ATTR gpio_handler() {
  _xt_isr_mask(1 << ETS_GPIO_INUM);

  uint32_t        status  = GPIO.status;
  static uint32_t command = 0;
  static uint32_t ticks   = 0;

  if (status & BIT(CLK_CPU)) {
    command |= (READ_PERI_REG(RTC_GPIO_IN_DATA) & 0x1) << ticks++;

    GPIO.status_w1tc = BIT(CLK_CPU);
  }

  if (status & BIT(XLT_CPU)) {
    if (command >= 0x800 && command <= 0x81f && tracking_balance != 0) {
      command = tracking_balance;

      SET_HI(SENS_PORT);
    } else if (command >= 0x820 && command <= 0x83f && tracking_gain != 0) {
      command = tracking_gain;

      SET_LO(SENS_PORT);
    }

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

    while (SPI1.cmd.usr == 1);

    // Trigger the latch signal - At this point the minimum time required for
    // enabling the latch signal has lapsed

    SET_LO(XLT_CONTROLLER);
    DELAY (40);

    SET_HI(XLT_CONTROLLER);

    command = 0;
    ticks   = 0;

    GPIO.status_w1tc = BIT(XLT_CPU);
  }

  if (status & BIT(SWITCH_PORT)) {
    if (((GPIO.in >> SWITCH_PORT) & 0x1) == 1) {
      timer_start_wifi = 1;

      frc1.load.data   = US_TO_TICKS(1000000);
      frc1.ctrl.en     = 1;
    } else {
      timer_start_wifi = 0;
    }

    GPIO.status_w1tc = BIT(SWITCH_PORT);
  }

  _xt_isr_unmask(1 << ETS_GPIO_INUM);
}

static void configure_timer() {
  /* The callback for the timer is required as the handler will enter into an
   * infinite loop if the interrupt is not cleared
   */
  _xt_isr_unmask(1 << ETS_FRC_TIMER1_INUM);
  _xt_isr_attach(ETS_FRC_TIMER1_INUM, timer_handler, 0);

  TM1_EDGE_INT_ENABLE();

  frc1.ctrl.div       = TIMER_CLKDIV_16;
  frc1.ctrl.intr_type = TIMER_EDGE_INT;
  frc1.ctrl.reload    = 0;
  frc1.ctrl.en        = 0;
}

static void configure_gpio() {
  gpio_set_direction(CLK_CPU       , GPIO_MODE_INPUT   );
  gpio_set_pull_mode(CLK_CPU       , GPIO_PULLUP_ONLY  );
  gpio_set_intr_type(CLK_CPU       , GPIO_INTR_POSEDGE );

  gpio_set_direction(XLT_CPU       , GPIO_MODE_INPUT   );
  gpio_set_pull_mode(XLT_CPU       , GPIO_PULLUP_ONLY  );
  gpio_set_intr_type(XLT_CPU       , GPIO_INTR_NEGEDGE );

  gpio_set_direction(DATA_CPU      , GPIO_MODE_INPUT   );
  gpio_set_pull_mode(DATA_CPU      , GPIO_PULLDOWN_ONLY);

  gpio_set_direction(XLT_CONTROLLER, GPIO_MODE_OUTPUT  );
  SET_HI(XLT_CONTROLLER);

  gpio_set_direction(SWITCH_PORT   , GPIO_MODE_INPUT   );
  gpio_set_intr_type(SWITCH_PORT   , GPIO_INTR_ANYEDGE );

  gpio_set_direction(LED_PORT      , GPIO_MODE_OUTPUT  );
  SET_HI(LED_PORT);

  gpio_set_direction(SENS_PORT     , GPIO_MODE_OUTPUT  );
  SET_LO(SENS_PORT);

  _xt_isr_attach(ETS_GPIO_INUM, gpio_handler, NULL);
  _xt_isr_unmask(1 << ETS_GPIO_INUM);
}

static void configure_spi() {
  // Initialize the SPI struct leaving the reserved bits unchanged
  SPI1.cmd.val      &= 0x0003ffff;
  SPI1.ctrl.val     &= 0xf86f8000;
  SPI1.ctrl1.val    &= 0x0000ffff;
  SPI1.ctrl2.val    &= 0x0000ffff;
  SPI1.clock.val     = 0;
  SPI1.user.val     &= 0x04fe0308;
  SPI1.user1.val     = 0;
  SPI1.user2.val    &= 0x0fff0000;
  SPI1.pin.val      &= 0xdff7fff8;
  SPI1.slave.val    &= 0x007ffc00;
  SPI1.slave1.val   &= 0x04000000;
  SPI1.slave2.val    = 0;
  SPI1.slave3.val    = 0;
  SPI1.ext2          = 0;
  SPI1.ext3.val     &= 0xfffffffc;
  SPI1.addr          = 0;
  SPI1.rd_status.val = 0;
  SPI1.wr_status     = 0;

  // Clear the data buffer
  for (int i = 0; i < sizeof(SPI1.data_buf) / sizeof(uint32_t); i++) {
    SPI1.data_buf[i] = 0;
  }

  // Set SPI bus interface configuration
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_HSPI_CLK  );
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_HSPID_MOSI);

  // Set CPOL and CPHA
  SPI1.pin.ck_idle_edge      = 1; // CPOL
  SPI1.user.ck_out_edge      = 1; // CPHA

  // Disable CS
  SPI1.pin.cs0_dis           = 1;
  SPI1.pin.cs1_dis           = 1;
  SPI1.pin.cs2_dis           = 1;

  // Set endianess
  SPI1.ctrl.wr_bit_order     = 1; // 1: LE 0: BE
  SPI1.user.wr_byte_order    = 0; // 1: BE 0: LE

  // Set clock frequency
  CLEAR_PERI_REG_MASK(PERIPHS_IO_MUX_CONF_U, SPI1_CLK_EQU_SYS_CLK);

  SPI1.clock.clk_equ_sysclk  = 0;
  SPI1.clock.clkdiv_pre      = 1;   // 80 / ( 1 + 1) = 40
  SPI1.clock.clkcnt_n        = 39;  // 40 / (39 + 1) = 1 MHz
  SPI1.clock.clkcnt_h        = 20;
  SPI1.clock.clkcnt_l        = 39;

  // Set MOSI signal delay configuration
  SPI1.user.ck_out_edge      = 1;
  SPI1.ctrl2.mosi_delay_num  = 1;
  SPI1.ctrl2.mosi_delay_mode = 1;
}

static void configure() {
  portENTER_CRITICAL();

  configure_timer();
  configure_spi  ();
  configure_gpio ();

  portEXIT_CRITICAL();
}

static void fetch_values() {
  nvs_handle handle;

  nvs_flash_init();

  if (nvs_open("tracking", NVS_READWRITE, &handle) == ESP_OK) {
    printf("NVS successfully opened\n");

    if (nvs_get_u16(handle, "balance", &tracking_balance) == ESP_ERR_NVS_NOT_FOUND) {
      printf("Setting default value for tracking balance...\n");

      nvs_set_u16(handle, "balance", tracking_balance);
    }

    if (nvs_get_u16(handle, "gain"   , &tracking_gain   ) == ESP_ERR_NVS_NOT_FOUND) {
      printf("Setting default value for tracking gain...\n");

      nvs_set_u16(handle, "gain", tracking_gain);
    }

    nvs_commit(handle);
    nvs_close (handle);
  } else {
    printf("Failed to open NVS\n");
  }

  nvs_flash_deinit();

  printf("tracking balance = 0x%03x\n", tracking_balance);
  printf("tracking gain    = 0x%03x\n", tracking_gain);
}

void run_patcher() {
  extern uint32_t *pxCurrentTCB;

  timer_start_wifi = 0;
  user_task_handle = (TaskHandle_t) pxCurrentTCB;

  fetch_values();
  configure   ();

  vTaskSuspend(NULL);

  if (wifi_start() != 0) {
    printf("Failed to start WiFi service");
  }

  while (true) {
    if (frc1.ctrl.en == 0) {
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
}
