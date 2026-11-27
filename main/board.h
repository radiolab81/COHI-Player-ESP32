/* Boardauswahl: genau EINE Zeile aktivieren. */
//#include "esp32_p4_module_dev_kit.h"
//#include "esp32_p4_nano.h"
//#include "esp32_p4_wifi6_poe_ethernet.h"
//#include "esp32_p4_wifi6_dev_kit.h"
#include "esp32_p4_eth.h"

/* Gemeinsam fuer alle Waveshare-P4-Boards (Schaltplaene): SD-Kartenversorgung = P-FET, Gate GPIO45 (low = AN).
 * GPIO45 darf deshalb KEIN PARLIO-Pin sein (siehe player_width_ok()). */
#ifndef BOARD_SD_PWR_GPIO
#define BOARD_SD_PWR_GPIO      45
#define BOARD_SD_PWR_ON_LEVEL  0
#endif
#ifndef BOARD_NAME
#define BOARD_NAME "Waveshare ESP32-P4 (siehe board.h)"
#endif
