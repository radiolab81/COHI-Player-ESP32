#pragma once
#include "esp_err.h"
#include "sdmmc_cmd.h"

esp_err_t sdcard_mount(void);      /* mountet /sdcard (SDMMC Slot 0, 4 Bit, LDO4) */
void      sdcard_unmount(void);
bool      sdcard_is_mounted(void);
sdmmc_card_t *sdcard_get(void);
