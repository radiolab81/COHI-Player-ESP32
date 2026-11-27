/*
 * autoplay.h - Playlist beim Einschalten ohne Konsolenaktion.
 *
 *   /autoplay.m3u auf der SD-Karte (Name: menuconfig) -> nach dem Start Abbruchfenster (3 s, Taste = Abbruch)
 *   -> Endlosschleife (oder einmal bei "#COHI once"); Einstellungen als "#COHI"-Direktiven in der Playlist.
 *   Ein Ueberwacher startet die Wiedergabe neu, wenn sie von selbst endet (Karte gezogen, nichts spielbar ...):
 *   Pausen 5, 10, 30 s, SD wird neu gemountet.  Die Konsole hat Vorrang: nach stop/play/umount ist der Autoplay
 *   bis zum naechsten Einschalten oder bis zum Befehl "autoplay" aus.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Nach SD-Mount und shell_init(), VOR shell_run(): Datei suchen und pruefen, Abbruchfenster abwarten.
 * true = Autoplay starten (danach autoplay_start() aufrufen). */
bool autoplay_boot_check(void);

/* Nach shell_run(): startet die Wiedergabe und den Ueberwacher (Task). */
void autoplay_start(void);

/* Konsole hat uebernommen (stop/play/umount): Ueberwachung aus. */
void autoplay_user_override(void);

/* Konsolenbefehl "autoplay": jetzt starten und Ueberwachung (wieder) einschalten. */
esp_err_t autoplay_request(void);

void autoplay_print_status(void);
