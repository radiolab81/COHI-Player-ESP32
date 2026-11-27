#include "sdcard.h"
#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;

esp_err_t sdcard_mount(void)
{
    if (s_card) return ESP_OK;

    /* Kartenversorgung (P-FET-Gate) explizit einschalten - GPIO45 darf nie PARLIO-Pin sein */
    gpio_config_t pw = { .pin_bit_mask = 1ULL << BOARD_SD_PWR_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pw);
    gpio_set_level(BOARD_SD_PWR_GPIO, BOARD_SD_PWR_ON_LEVEL);

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;      /* 40 MHz, 4 Bit -> bis ~20 MB/s */
    host.flags &= ~SDMMC_HOST_FLAG_DDR;

    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = BOARD_SDMMC_LDO_CHAN };
        esp_err_t e = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
        if (e != ESP_OK) { ESP_LOGE(TAG, "LDO%d: %s", BOARD_SDMMC_LDO_CHAN, esp_err_to_name(e)); return e; }
    }
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;

    esp_err_t e = esp_vfs_fat_sdmmc_mount(BOARD_SD_MOUNT_POINT, &host, &slot, &mc, &s_card);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Mount fehlgeschlagen: %s (Karte eingelegt? FAT32/exFAT?)", esp_err_to_name(e));
        s_card = NULL;
        return e;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sdcard_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT, s_card);
    s_card = NULL;
}

bool sdcard_is_mounted(void) { return s_card != NULL; }
sdmmc_card_t *sdcard_get(void) { return s_card; }
