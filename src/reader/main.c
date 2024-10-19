#include "reader.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

void app_main() {
  run_reader();

  vTaskDelete(NULL);
}
