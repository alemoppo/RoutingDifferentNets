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

static void register_all(NetMon *m)
{
    cancel_all(m);
    /* AF_UNSPEC: intercetta cambiamenti IPv4/IPv6 in modo indistinto. */
    NotifyIpInterfaceChange(AF_UNSPEC, NULL, NULL, FALSE, &m->hNotify[0]);
    NotifyUnicastIpAddressChange(AF_UNSPEC, NULL, NULL, FALSE, &m->hNotify[1]);
    NotifyRouteChange2(AF_UNSPEC, NULL, NULL, FALSE, &m->hNotify[2]);
}

static DWORD WINAPI monitor_thread(LPVOID p)
{
    NetMon *m = (NetMon *)p;

    register_all(m);

    for (;;) {
        HANDLE waits[NOTIFY_COUNT + 1];
        waits[0] = m->hExit;
        for (int i = 0; i < NOTIFY_COUNT; i++)
            waits[1 + i] = m->hNotify[i] ? m->hNotify[i] : m->hExit;

        DWORD r = WaitForMultipleObjects(NOTIFY_COUNT + 1, waits,
                                         FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED)
            break;                       /* stop richiesto */

        /* Notifica di rete: la GUI rifarà un reconcile completo. */
        if (m->cb)
            m->cb(m->user);

        /* Ri-registra: le Notify* sono one-shot. */
        register_all(m);
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

    SetEvent(m->hExit);
    if (m->hThread) {
        WaitForSingleObject(m->hThread, 2000);
        CloseHandle(m->hThread);
    }
    cancel_all(m);
    if (m->hExit)
        CloseHandle(m->hExit);
    free(m);
}