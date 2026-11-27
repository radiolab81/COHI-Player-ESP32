idf_component_register(
    SRCS main.c sdcard.c shell.c player.c playlist.c cohi_cfg.c autoplay_logic.c autoplay.c sdr_dsp.c sdr_plan.c wav_info.c
    INCLUDE_DIRS "."
    REQUIRES console fatfs sdmmc esp_driver_sdmmc esp_driver_parlio esp_driver_gpio
             esp_timer esp_mm esp_psram esp_hw_support esp_driver_uart vfs esp_common
)
# DSP-Kern: Schleifen ueber SDR_HB*_K vollstaendig entrollen lassen
set_source_files_properties(sdr_dsp.c PROPERTIES COMPILE_OPTIONS "-O3")
# Pfad-Zusammensetzungen sind laengengeprueft; GCC-Truncation-Heuristik ist hier zu streng
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
