#pragma once
void shell_init(void);   /* Konsole anlegen, Befehle registrieren */
void shell_run(void);    /* Konsole starten (ab hier werden Eingaben gelesen) */
void shell_start(void);  /* init + run */
