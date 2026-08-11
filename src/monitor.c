/*
 * monitor.c - implementazione del monitor di rete.
 *
 * Pattern documentato: Notify*Functions con Callback=NULL e NotificationHandle.
 * Il HANDLE viene segnalato ad ogni cambiamento; dopo ogni segnalazione bisogna
 * cancellare e ri-registrare la notifica (CancelMibChangeNotify2).
 */
#include "common.h"
#include "monitor.h"
#include <stdlib.h>

#define NOTIFY_COUNT 3

struct NetMon {
    HANDLE           hExit;      /* evento esterno per lo stop            */
    HANDLE           hThread;    /* thread del monitor                    */
    HANDLE           hNotify[NOTIFY_COUNT];
    net_change_cb    cb;
    void            *user;
};

static void cancel_all(NetMon *m)
{
    for (int i = 0; i < NOTIFY_COUNT; i++) {
        if (m->hNotify[i]) {
            CancelMibChangeNotify2(m->hNotify[i]);
            m->hNotify[i] = NULL;
        }
    }
}

/* Registra le notifiche di rete. Ritorna FALSE se almeno una registrazione e'
 * fallita (in tal caso il chiamante programma un retry senza busy loop). Gli
 * handle falliti restano NULL e vengono sostituiti da hExit nel wait, cosi'
 * NON vengono inseriti handle duplicati o non validi nell'array. */
static BOOL register_all(NetMon *m)
{
    BOOL ok = TRUE;

    cancel_all(m);
    /* AF_UNSPEC: intercetta cambiamenti IPv4/IPv6 in modo indistinto. */
    if (NotifyIpInterfaceChange(AF_UNSPEC, NULL, NULL, FALSE, &m->hNotify[0])
            != NO_ERROR) {
        dbg("[MON] NotifyIpInterfaceChange fallito (%lu)", GetLastError());
        m->hNotify[0] = NULL;
        ok = FALSE;
    }
    if (NotifyUnicastIpAddressChange(AF_UNSPEC, NULL, NULL, FALSE,
                                     &m->hNotify[1]) != NO_ERROR) {
        dbg("[MON] NotifyUnicastIpAddressChange fallito (%lu)",
            GetLastError());
        m->hNotify[1] = NULL;
        ok = FALSE;
    }
    if (NotifyRouteChange2(AF_UNSPEC, NULL, NULL, FALSE, &m->hNotify[2])
            != NO_ERROR) {
        dbg("[MON] NotifyRouteChange2 fallito (%lu)", GetLastError());
        m->hNotify[2] = NULL;
        ok = FALSE;
    }
    return ok;
}

static DWORD WINAPI monitor_thread(LPVOID p)
{
    NetMon *m = (NetMon *)p;

    BOOL retry = !register_all(m);

    for (;;) {
        HANDLE waits[NOTIFY_COUNT + 1];
        waits[0] = m->hExit;
        for (int i = 0; i < NOTIFY_COUNT; i++)
            waits[1 + i] = m->hNotify[i] ? m->hNotify[i] : m->hExit;

        /* Se una notifica non e' registrata, usiamo un timeout di retry
         * (basso costo: il thread resta in wait, nessun busy loop di CPU).
         * Con TUTTE le notifiche attive restiamo su INFINITE -> ~0% CPU. */
        DWORD waitms = retry ? 2000 : INFINITE;
        DWORD r = WaitForMultipleObjects(NOTIFY_COUNT + 1, waits,
                                         FALSE, waitms);
        if (r == WAIT_OBJECT_0)
            break;                       /* stop richiesto */
        if (r == WAIT_FAILED) {
            dbg("[MON] WaitForMultipleObjects fallito (%lu)", GetLastError());
            retry = TRUE;
            continue;
        }

        /* WAIT_OBJECT_0+1 .. +NOTIFY_COUNT: notifica di rete reali.
         * La GUI rifarà un reconcile completo. */
        if (r - WAIT_OBJECT_0 > 0 && r - WAIT_OBJECT_0 <= NOTIFY_COUNT) {
            if (m->cb)
                m->cb(m->user);
        }

        /* Ri-registra: le Notify* sono one-shot. WAIT_TIMEOUT = retry. */
        retry = !register_all(m);
    }

    cancel_all(m);
    return 0;
}

NetMon *monitor_start(net_change_cb cb, void *user)
{
    NetMon *m = (NetMon *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;

    m->hExit = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!m->hExit) {
        free(m);
        return NULL;
    }
    m->cb   = cb;
    m->user = user;

    m->hThread = CreateThread(NULL, 0, monitor_thread, m, 0, NULL);
    if (!m->hThread) {
        CloseHandle(m->hExit);
        free(m);
        return NULL;
    }
    return m;
}

void monitor_stop(NetMon *m)
{
    if (!m)
        return;

    /* Segnala l'uscita e attende che il thread sia DAVVERO terminato prima
     * di toccare la struttura: la thread termina cancellando le notifiche e
     * ritornando; solo dopo possiamo liberare senza rischio di use-after-free.
     * Il thread si sblocca subito su hExit, quindi l'attesa e' immediata. */
    SetEvent(m->hExit);
    if (m->hThread) {
        WaitForSingleObject(m->hThread, INFINITE);
        CloseHandle(m->hThread);
        m->hThread = NULL;
    }
    cancel_all(m);
    if (m->hExit)
        CloseHandle(m->hExit);
    free(m);
}