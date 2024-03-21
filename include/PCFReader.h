#include "esp_err.h"
#include "ili9341Driver.h"

esp_err_t PCFReaderInit();
esp_err_t PCFPrintStringToILI9341(const ili9341_config_t device, uint16_t x1, uint16_t y1, char* string, size_t len);