/*
 * gui.h - interfaccia grafica SDL3 del Network Route Manager.
 */
#ifndef GUI_H
#define GUI_H

#include "config.h"

/* Avvia il loop grafico (blocca fino alla chiusura della finestra). */
void gui_run(Config *cfg);

#endif /* GUI_H */