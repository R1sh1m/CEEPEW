/* main/main.c - Minimal ESP32 Entry Point */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"

void app_main(void)
{
    printf("=== CEE-PEW Debug Test ===\n");
    printf("ESP32 Firmware Build Successful\n");
    
    /* Print chip info */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("Chip: %s (Rev %d)\n", CONFIG_IDF_TARGET, chip_info.revision);
    printf("Cores: %d\n", chip_info.cores);
    printf("Features: %d\n", chip_info.features);
    
    printf("\n=== Build Test Complete ===\n");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
