#ifndef ESP32_P4_ETH
#define ESP32_P4_ETH  

/* --- Konfiguration Waveshare ESP32_P4_ETH ---
 *
 * PARLIO-Datenbus + Pixeltakt auf dem 40-Pin-Header (identisch zu parlioSDR,
 * esp32_p4_eth.h).  Ethernet wird in diesem Projekt NICHT benoetigt.
 *
 * SD-Karte (TF-Slot, SDMMC Slot 0, feste IO_MUX-Pins):
 *   D0=39 D1=40 D2=41 D3=42 CLK=43 CMD=44
 *   Pull-ups an LDO_VO4 (3,3 V) -> on-chip LDO Kanal 4 einschalten.
 *   Kartenversorgung: P-FET mit Gate an GPIO45 (10k nach GND) -> Karte ist
 *   standardmaessig AN; GPIO45 deshalb nicht verwenden.
 */

/*
 * PARLIO 16-Bit Pin-Belegung auf dem:
 * --------------------------------------------------
 * Daten-Bits (D0-D15):
*/
#define SD0 2
#define SD1 3
#define SD2 4
#define SD3 5
#define SD4 6
#define SD5 14
#define SD6 15
#define SD7 16

#define SD8 17
#define SD9 18
#define SD10 19
#define SD11 54
#define SD12 20
#define SD13 21
#define SD14 22
#define SD15 23

/* Steuer-Signale: (Taktet die Daten in den DAC, wenn benötigt) */
#define BOARD_NAME "Waveshare ESP32-P4-ETH"
#define DAC_CLK_PIN 26
#define ADC_CLK_PIN -1
#define DATA_VALID_PIN -1

// SMI Pins für Rev 1.3
#define  SMI_GPIO_MDC_NUM 31
#define  SMI_GPIO_MDIO_NUM 52

// RMII Pins (Slot 0)
#define TX_EN_NUM 49
#define TXD0_NUM 34
#define TXD1_NUM 35
#define CRS_DV_NUM 28
#define RXD0_NUM 29 
#define RXD1_NUM 30 

#define REF_CLK_NUM 50
#define PHY_RESET_NUM 51

// SD
#define BOARD_SDMMC_LDO_CHAN  4
#define BOARD_SD_MOUNT_POINT  "/sdcard"

#endif
