#include "actions.h"
#include "common.h"
#include "controller.h"
#include "wifi.h"

// ESP8266
#include "rom/ets_sys.h"
#include "esp8266/spi_struct.h"
#include "esp8266/timer_struct.h"
#include "driver/gpio.h"
#include "driver/hw_timer.h"

// ESP SDK
#include "esp_attr.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

#define BUFFER_READ_MS 250 // Time period between input buffer reads

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

  // Force the controller to be in RESET state; otherwise, the controller may
  // start to move the optical pickup and the spindle motor looking for a disc
  gpio_set_direction(XRST_PORT, GPIO_MODE_OUTPUT);
  SET_LO(XRST_PORT);

  gpio_set_direction(FOK_PORT , GPIO_MODE_INPUT);
  gpio_set_pull_mode(FOK_PORT , GPIO_FLOATING);

  gpio_set_direction(SENS_PORT, GPIO_MODE_INPUT);
  gpio_set_pull_mode(SENS_PORT, GPIO_FLOATING);
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
  PIN_PULLUP_EN(PERIPHS_IO_MUX_MTMS_U);
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_HSPI_CLK);

  PIN_PULLUP_EN(PERIPHS_IO_MUX_MTCK_U);
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
  SPI1.clock.clkcnt_n        = 63;  // 40 / (63 + 1) = 625 KHz
  SPI1.clock.clkcnt_h        = 32;
  SPI1.clock.clkcnt_l        = 63;

  // Set MOSI signal delay configuration
  SPI1.user.ck_out_edge      = 1;
  SPI1.ctrl2.mosi_delay_num  = 1;
  SPI1.ctrl2.mosi_delay_mode = 1;
}

static void configure() {
  portENTER_CRITICAL();

  configure_timer();
  configure_gpio ();
  configure_spi  ();

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

static void handle_ctl_update(const TEvent* status) {
  // If the status has changed then print the friendly description of the new
  // status and show the menu iif the controller is ready

  printf("\033[1mStatus\033[22m: %s\n", status->status_text);

  if (!status->is_busy && status->is_powered) {
    show_menu();

    // Clear input buffer after an action has been completed
    while ((fgetc(stdin)) != EOF);
  }
}

void run_sender() {
  configure();

  // Initialize the controller
  ctl_start();

#if 1
  if (wifi_start() != 0) {
    printf("Failed to initialize the WiFi service - Only UART will be available...\n");
  }
#endif

  // Add the listener after the WiFi, if enabled, has been started as it prints
  // some information to the console
  ctl_add_listener(handle_ctl_update);

  while (true) {
    int option = fgetc(stdin);

    if (option != EOF) {
      process_option(option);
    }

    vTaskDelay(BUFFER_READ_MS / portTICK_RATE_MS);
  }
}
