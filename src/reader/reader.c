// ESP8266
#include "rom/ets_sys.h"
#include "esp8266/spi_struct.h"
#include "esp8266/gpio_struct.h"
#include "esp8266/timer_struct.h"
#include "driver/gpio.h"
#include "driver/hw_timer.h"

// ESP SDK
#include "esp_attr.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

// C
#include <stdbool.h>
#include <string.h>

// GPIO Mappings
//
// - GPIO1 and GPIO3 are externally connected to the UART interface of CH340C
// - GPIO16 is internally connected to the RTC module

#define SCOR_PORT GPIO_NUM_5  // D1

// Macro for reversing a sequence of 4 bits
#define REVERSE(x) ((((x) >> 3) & 0x1) | \
                    (((x) >> 1) & 0x2) | \
                    (((x) << 1) & 0x4) | \
                    (((x) << 3) & 0x8))

// Macro for converting a 2 digit BCD encoded number to a decimal number
#define BCD2DEC(x) ((((x) >> 4) & 0xf) * 10 + ((x) & 0xf))

// Macro for converting a value given in uS to ticks for the FRC Timer
// Clock divider must be set to TIMER_CLKDIV_16
#define US_TO_TICKS(t) ((80000000 >> frc1.ctrl.div) / 1000000) * t

// The size of the circular buffer
#define BUFFER_SIZE 3 * 4096

static DRAM_ATTR uint32_t buffer[BUFFER_SIZE];  // The circular buffer
static IRAM_ATTR size_t   read_index;           // The read index of the circular buffer
static IRAM_ATTR size_t   write_index;          // The write index of the circular buffer

static void IRAM_ATTR frc_timer_isr_cb() {
  frc1.ctrl.en = 0;
}

static void IRAM_ATTR gpio_handler() {
  if (GPIO.status & BIT(SCOR_PORT)) {
    GPIO.status_w1tc = BIT(SCOR_PORT);

    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_GPIO12);

    if (((GPIO.in >> GPIO_NUM_12) & 0x1) == /* CRC OK */ 1) {
      PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_HSPIQ_MISO);

      // Enable the read phase
      SPI1.user.usr_miso         = 1;

      // Set the length of the data to read
      SPI1.user1.usr_miso_bitlen = 80 - 1;

      // Start the operation
      SPI1.cmd.usr               = 1;

      while (SPI1.cmd.usr == 1);

      if (write_index != read_index || read_index == 0) {
        if ((REVERSE(( SPI1.data_buf[0] >> 24) & 0xf)) == /* Mode 1 */ 1) {
          buffer[write_index++] = SPI1.data_buf[0];
          buffer[write_index++] = SPI1.data_buf[1];
          buffer[write_index++] = SPI1.data_buf[2];

          if (write_index == BUFFER_SIZE) {
            write_index = 0;
          }
        }
      }
    }
  }
}

static void IRAM_ATTR read_lead_in() {
  bool     in_lead_in     = true;
  uint8_t  tno_first      = 0;
  uint8_t  tno_last       = 0;
  uint8_t  min_lead_out   = 0;
  uint8_t  sec_lead_out   = 0;
  uint8_t  frame_lead_out = 0;
  uint8_t  toc_i          = 0;
  uint8_t  toc_n          = 0;
  uint8_t* toc_content    = NULL;

  while (in_lead_in) {
    if (read_index == BUFFER_SIZE) {
      read_index = 0;
    }

    while (
      (read_index + 3 < write_index) ||
      (write_index < read_index && read_index < BUFFER_SIZE)
    ) {
      uint32_t q0  = buffer[read_index++];
      uint32_t q1  = buffer[read_index++];
      uint32_t q2  = buffer[read_index++] >> 16;
      uint8_t  adr = (REVERSE((q0 >> 24) & 0xf));

      if (adr == /* Mode 1 */ 1) {
        uint8_t tno = (REVERSE((q0 >> 20) & 0xf) << 4)
                    | (REVERSE((q0 >> 16) & 0xf));

        if (tno == 0) {
          uint8_t point   = (REVERSE((q0 >> 12) & 0xf) << 4)
                          | (REVERSE((q0 >> 8) & 0xf));
          uint8_t p_min   = (REVERSE((q1 >>  4) & 0xf) << 4)
                          | (REVERSE((q1 >> 0) & 0xf));
          uint8_t p_sec   = (REVERSE((q2 >> 12) & 0xf) << 4)
                          | (REVERSE((q2 >> 8) & 0xf));
          uint8_t p_frame = (REVERSE((q2 >>  4) & 0xf) << 4)
                          | (REVERSE((q2 >> 0) & 0xf));

          switch (point) {
          case 0xA0:
            tno_first = BCD2DEC(p_min);
            break;

          case 0xA1:
            tno_last  = BCD2DEC(p_min);
            break;

          case 0xA2:
            min_lead_out   = BCD2DEC(p_min  );
            sec_lead_out   = BCD2DEC(p_sec  );
            frame_lead_out = BCD2DEC(p_frame);
            break;

          default:
            if (toc_content != NULL) {
              point = BCD2DEC(point) - 1;

              if (point < toc_n && toc_content[point * 4] == 0) {
                toc_content[(point * 4) + 0] = point + 1;
                toc_content[(point * 4) + 1] = BCD2DEC(p_min  );
                toc_content[(point * 4) + 2] = BCD2DEC(p_sec  );
                toc_content[(point * 4) + 3] = BCD2DEC(p_frame);

                if (++toc_i == toc_n) {
                  printf("\033[1mTracks\033[22m: %d - "
                         "\033[1mTime\033[22m: %02d:%02d.%02d\n",
                    toc_n,
                    min_lead_out,
                    sec_lead_out,
                    frame_lead_out
                  );

                  for (size_t i = 0; i < toc_n; i++) {
                    uint8_t  tno    = toc_content[(i * 4) + 0];
                    uint8_t  amin   = toc_content[(i * 4) + 1];
                    uint8_t  asec   = toc_content[(i * 4) + 2];
                    uint8_t  aframe = toc_content[(i * 4) + 3];
                    uint16_t length_sec;

                    if (i + 1 == toc_n) {
                      length_sec = min_lead_out * 60 + sec_lead_out;
                    } else {
                      length_sec = toc_content[((i + 1) * 4) + 1] * 60
                                 + toc_content[((i + 1) * 4) + 2];
                    }

                    length_sec -= amin * 60 + asec;

                    printf("Track %02d %02d:%02d at ATIME %02d:%02d.%02d\n",
                      tno,
                      length_sec / 60,
                      length_sec % 60,
                      amin,
                      asec,
                      aframe
                    );
                  }

                  free(toc_content);
                }
              }
            }
          }

          if (tno_first != 0 && tno_last != 0 && toc_content == NULL) {
            toc_n = tno_last - tno_first + 1;

            if (toc_n > 0 && toc_n <= /* Maximum tracks */ 99) {
              toc_content = (uint8_t*) malloc(sizeof(uint8_t) * 4 * toc_n);

              if (toc_content != NULL) {
                for (size_t i = 0; i < sizeof(uint8_t) * 4 * toc_n; i++) {
                  toc_content[i] = 0;
                }
              }
            }
          }
        } else {
          read_index = (read_index == 0 ? BUFFER_SIZE : read_index) - 3;
          in_lead_in = false;

          break;
        }
      }
    }
  }

  printf("\n");
}

static void IRAM_ATTR read_program() {
  bool     in_program    = true;
  uint8_t  last_tno      = 0;
  uint8_t  last_min      = 0;
  uint8_t  last_sec      = 0;
  uint8_t  last_amin     = 0;
  uint8_t  last_asec     = 0;
  uint16_t frame_counter = 0;
  uint16_t stuck_errors  = 0;
  uint16_t jump_errors   = 0;

  while (in_program) {
    if (read_index == BUFFER_SIZE) {
      read_index = 0;
    }

    while (
      (read_index + 3 < write_index) ||
      (write_index < read_index && read_index < BUFFER_SIZE)
    ) {
      uint32_t q0  = buffer[read_index++];
      uint32_t q1  = buffer[read_index++];
      uint32_t q2  = buffer[read_index++] >> 16;
      uint8_t  adr = (REVERSE((q0 >> 24) & 0xf));

      if (adr == /* Mode 1 */ 1) {
        uint8_t tno = (REVERSE((q0 >> 20) & 0xf) << 4)
                    | (REVERSE((q0 >> 16) & 0xf));

        if (tno == 0x00 || tno == 0xaa) {
          in_program = false;
          read_index = (read_index == 0 ? BUFFER_SIZE : read_index) - 3;

          break;
        } else {
          uint8_t min  = (REVERSE((q0 >>  4) & 0xf) << 4)
                       | (REVERSE((q0 >>  0) & 0xf));
          uint8_t sec  = (REVERSE((q1 >> 28) & 0xf) << 4)
                       | (REVERSE((q1 >> 24) & 0xf));
          uint8_t amin = (REVERSE((q1 >>  4) & 0xf) << 4)
                       | (REVERSE((q1 >>  0) & 0xf));
          uint8_t asec = (REVERSE((q2 >> 12) & 0xf) << 4)
                       | (REVERSE((q2 >>  8) & 0xf));

          tno  = BCD2DEC(tno);
          min  = BCD2DEC(min);
          sec  = BCD2DEC(sec);
          amin = BCD2DEC(amin);
          asec = BCD2DEC(asec);

          if (amin != last_amin || asec != last_asec) {
            if (
              (last_asec == 59 && asec != 0)             ||
              (last_asec != 59 && last_asec + 1 != asec) ||
              (last_asec == 59 && last_amin + 1 != amin)
            ) {
              printf("\aJump detected from %02d %02d:%02d to %02d %02d:%02d\n",
                last_tno,
                last_min,
                last_sec,
                tno,
                min,
                sec
              );

              jump_errors++;
            }

            printf("\033[1mPlaying\033[22m: %02d %02d:%02d\n\033[1A",
              tno,
              min,
              sec
            );

            last_tno      = tno;
            last_min      = min;
            last_sec      = sec;
            last_amin     = amin;
            last_asec     = asec;
            frame_counter = 0;
          } else if (++frame_counter > 75) {
            if (frame_counter == 76) {
              printf("\aStuck at %02d %02d:%02d\n", tno, min, sec);

              stuck_errors++;
            }
          }
        }
      }
    }
  }

  printf("\033[2KJump Errors : %5d\nStuck Errors: %5d\n\n",
    jump_errors,
    stuck_errors
  );
}

static void IRAM_ATTR read_lead_out() {
  bool in_lead_out = true;

  frc1.load.data = US_TO_TICKS(1000000);
  frc1.ctrl.en   = 1;

  while (in_lead_out) {
    if (read_index == BUFFER_SIZE) {
      read_index = 0;
    }

    if (frc1.ctrl.en == 0) {
      printf("\a");
      fflush(stdout);

      frc1.load.data = US_TO_TICKS(1000000);
      frc1.ctrl.en   = 1;
    }

    while (
      (read_index + 3 < write_index) ||
      (write_index < read_index && read_index < BUFFER_SIZE)
    ) {
      uint32_t q0  = buffer[read_index++];
      uint8_t  adr = (REVERSE((q0 >> 24) & 0xf));

      read_index++;
      read_index++;

      if (adr == /* Mode 1 */ 1) {
        uint8_t tno = (REVERSE((q0 >> 20) & 0xf) << 4)
                    | (REVERSE((q0 >> 16) & 0xf));

        if (tno != 0xaa) {
          in_lead_out = false;
          read_index  = (read_index == 0 ? BUFFER_SIZE : read_index) - 3;

          break;
        }
      }
    }
  }

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
  gpio_set_direction(SCOR_PORT, GPIO_MODE_INPUT);
  gpio_set_pull_mode(SCOR_PORT, GPIO_FLOATING);

  _xt_isr_attach(ETS_GPIO_INUM, gpio_handler, NULL);
  _xt_isr_unmask(1 << ETS_GPIO_INUM);

  GPIO.pin[SCOR_PORT].int_type = GPIO_INTR_NEGEDGE;
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
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_HSPI_CLK);    // SQCK
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_HSPIQ_MISO);  // SQDT

  // Set CPOL and CPHA
  SPI1.pin.ck_idle_edge      = 1; // CPOL
  SPI1.user.ck_out_edge      = 0; // CPHA

  // Disable CS
  SPI1.pin.cs0_dis           = 1;
  SPI1.pin.cs1_dis           = 1;
  SPI1.pin.cs2_dis           = 1;

  // Set endianess
  SPI1.ctrl.rd_bit_order     = 0; // 1: LE 0: BE
  SPI1.user.rd_byte_order    = 1; // 1: BE 0: LE

  // Set clock frequency
  CLEAR_PERI_REG_MASK(PERIPHS_IO_MUX_CONF_U, SPI1_CLK_EQU_SYS_CLK);

  SPI1.clock.clk_equ_sysclk  = 0;
  SPI1.clock.clkdiv_pre      = 1;   // 80 / ( 1 + 1) = 40
  SPI1.clock.clkcnt_n        = 39;  // 40 / (39 + 1) = 1 MHz
  SPI1.clock.clkcnt_h        = 19;
  SPI1.clock.clkcnt_l        = 39;

  // Set MISO signal delay configuration
  SPI1.user.ck_out_edge      = 0;
  SPI1.ctrl2.miso_delay_mode = 0;
  SPI1.ctrl2.miso_delay_num  = 0;
}

static void configure() {
  portENTER_CRITICAL();

  configure_timer();
  configure_gpio ();
  configure_spi  ();

  portEXIT_CRITICAL();
}

void run_reader() {
  read_index  = 0;
  write_index = 0;

  configure();

  while (true) {
    read_lead_in ();
    read_program ();
    read_lead_out();
  }
}
