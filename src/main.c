/*
 * main.c - punto di ingresso del Network Route Manager.
 *
 * Richiede privilegi amministrativi (vedi resources.rc / app.manifest):
 * il processo viene avviato da Windows elevato via UAC. Come doppia
 * sicurezza, se l'esecuzione avviene senza i privilegi necessari mostriamo
 * un errore chiaro e usciamo.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "common.h"
#include "config.h"
#include "gui.h"

static BOOL process_is_elevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elev;
    DWORD sz = 0;
    BOOL ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return FALSE;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz))
        ok = elev.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* UAC/manifest gia' richiede l'elevazione; qui gestiamo il caso in cui
     * l'app venga lanciata comunque senza privilegi (es. test senza
     * manifest): le route non sarebbero modificabili. */
    if (!process_is_elevated()) {
        MessageBoxW(NULL,
                    L"Network Route Manager richiede privilegi amministrativi "
                    L"per modificare la tabella di routing.\n"
                    L"Avviare il programma con 'Run as administrator' "
                    L"(UAC).",
                    L"Network Route Manager - privilegi",
                    MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    /* caricamento configurazione persistente (JSON minimale) */
    Config cfg;
    char path[MAX_PATH];
    cfg_default_path(path, sizeof(path));
    cfg_load(&cfg, path);

    gui_run(&cfg);

    return 0;
}