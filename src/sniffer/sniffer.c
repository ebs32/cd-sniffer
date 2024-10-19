// ESP8266
#include "esp8266/gpio_struct.h"
#include "driver/gpio.h"

// ESP SDK
#include "esp_attr.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

// C
#include <stdint.h>

// GPIO Mappings - By default, GPIO01 and GPIO03 are reserved for UART
#define CLK_LINE    GPIO_NUM_13 // D7
#define DATA_LINE   GPIO_NUM_12 // D6
#define XLT_LINE    GPIO_NUM_14 // D5

// The size of the circular buffer
#define BUFFER_SIZE 2048

static DRAM_ATTR uint32_t buffer[BUFFER_SIZE];  // The circular buffer
static IRAM_ATTR bool     buffer_full;          // Indicates if the circular buffer is full
static IRAM_ATTR size_t   read_index;           // The read index of the circular buffer
static IRAM_ATTR size_t   write_index;          // The write index of the circular buffer

static IRAM_ATTR uint32_t data;                 // The data being captured from the MICOM interface
static IRAM_ATTR uint32_t ticks;                // Keeps control of the CLK ticks on the MICOM interface

static void IRAM_ATTR handle_int(void* arg) {
  uint32_t status = GPIO.status;
  uint32_t value  = GPIO.in;

  GPIO.status_w1tc = (1UL << CLK_LINE);
  GPIO.status_w1tc = (1UL << XLT_LINE);

  // Make sure there is room for at least 1 word
  if (write_index + 1 > BUFFER_SIZE && (BUFFER_SIZE - write_index) + read_index < 1) {
    _xt_isr_mask(1 << ETS_GPIO_INUM);

    buffer_full = true;

    return;
  }

  if (status & (1UL << CLK_LINE)) {
    data |= ((value >> DATA_LINE) & 1) << ticks++;
  }

  if (status & (1UL << XLT_LINE)) {
    buffer[write_index] = data;

    ticks = 0;
    data  = 0;

    if (++write_index == BUFFER_SIZE) {
      write_index = 0;
    }
  }
}

static void initialize() {
  portENTER_CRITICAL();

  data        = 0;
  ticks       = 0;

  buffer_full = false;
  read_index  = 0;
  write_index = 0;

  // Assign PINs
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U , FUNC_GPIO13);
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U , FUNC_GPIO12);
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U , FUNC_GPIO14);

  // Configure GPIOs
  gpio_set_direction(CLK_LINE , GPIO_MODE_INPUT);
  gpio_set_direction(DATA_LINE, GPIO_MODE_INPUT);
  gpio_set_direction(XLT_LINE , GPIO_MODE_INPUT);

  gpio_set_pull_mode(CLK_LINE , GPIO_FLOATING);
  gpio_set_pull_mode(DATA_LINE, GPIO_FLOATING);
  gpio_set_pull_mode(XLT_LINE , GPIO_FLOATING);

  // Define triggers
  gpio_set_intr_type(CLK_LINE , GPIO_INTR_POSEDGE);
  gpio_set_intr_type(XLT_LINE , GPIO_INTR_NEGEDGE);

  // Attach interrupt handler
  _xt_isr_attach(ETS_GPIO_INUM, handle_int, 0);
  _xt_isr_unmask(1 << ETS_GPIO_INUM);

  portEXIT_CRITICAL();
}

void run_sniffer() {
  initialize();

  while (!buffer_full) {
    while (read_index == write_index && !buffer_full) {
      vTaskDelay(5 / portTICK_RATE_MS);
    }

    while (read_index < (write_index < read_index ? BUFFER_SIZE : write_index)) {
      printf("%04x ", buffer[read_index++]);
    }

    if (read_index == BUFFER_SIZE) {
      read_index = 0;
    }

    fflush(stdout);
  }

  printf("\nPotential data loss as there was no more space left in the buffer - Exiting!\n");
}
