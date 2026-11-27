/*
 * COHI-Player-ESP32 - Cohiradia-WAV von SD-Karte -> HF ueber PARLIO (Waveshare ESP32-P4)
 * Bedienung ueber die serielle Konsole: ls, cd, pwd, play, stop, status ...c
 * Autoplay: /autoplay.m3u auf der SD-Karte wird beim Einschalten ohne Konsolenaktion abgespielt (autoplay.h).
 */
#include <stdio.h>
#include "esp_log.h"
#include "esp_idf_version.h"
#include "sdcard.h"
#include "player.h"
#include "shell.h"
#include "autoplay.h"
#include "version.h"

static const char *TAG = "main";

void app_main(void)
{
    player_idle_pins();                      /* 1. Datenpins/Takt sofort auf Stille */
    ESP_LOGI(TAG, "COHI-Player-ESP32 %s (%s), gebaut %s, ESP-IDF %s", COHI_STAGE, COHI_VERSION, COHI_BUILD_DATE, esp_get_idf_version());
    if (player_init() != ESP_OK) {
        ESP_LOGE(TAG, "player_init fehlgeschlagen - Abbruch");
        return;
    }
    if (sdcard_mount() != ESP_OK)
        ESP_LOGW(TAG, "keine SD-Karte - nach dem Einlegen 'mount' eingeben");

    shell_init();                            /* 2. Konsole anlegen (UART-Treiber installiert, noch nicht lesend) */
    bool go = autoplay_boot_check();         /* 3. autoplay.m3u suchen, Abbruchfenster (Taste = Abbruch) */
    shell_run();                             /* 4. Konsole starten */
    if (go) autoplay_start();                /* 5. Wiedergabe + Ueberwachung */
}
