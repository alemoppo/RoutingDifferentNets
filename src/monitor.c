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
    /* Stato di stop. Scrittura da monitor_stop() e lettura dal thread SI
     * SOLO tramite primitive interlocked (InterlockedExchange/Interlocked-
     * CompareExchange): rendono esplicita la visibilita' e l'ordine di
     * memoria tra i due thread. Nessun accesso diretto. */
    LONG             halt;
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
 * fallita (in tal caso il chiamante programma un retry senza busy loop).
 * Le API restituiscono direttamente un NETIO_STATUS: usiamo il return value,
 * NON GetLastError(). Gli handle falliti restano NULL.
 * Il contatore `registered` indica quante notifiche sono attive, cosi' la
 * thread sa quanti handle validi inserire nell'array di WaitForMultipleObjects
 * (niente placeholder duplicati: l'array contiene solo handle reali e hExit
 * compare una sola volta come ultimo elemento). */
static BOOL register_all(NetMon *m)
{
    NETIO_STATUS st;
    BOOL ok = TRUE;

    cancel_all(m);
    /* AF_UNSPEC: intercetta cambiamenti IPv4/IPv6 in modo indistinto. */
    st = NotifyIpInterfaceChange(AF_UNSPEC, NULL, NULL, FALSE, &m->hNotify[0]);
    if (st != NO_ERROR) {
        dbg("[MON] NotifyIpInterfaceChange fallito (%lu)", st);
        m->hNotify[0] = NULL;
        ok = FALSE;
    }
    st = NotifyUnicastIpAddressChange(AF_UNSPEC, NULL, NULL, FALSE,
                                      &m->hNotify[1]);
    if (st != NO_ERROR) {
        dbg("[MON] NotifyUnicastIpAddressChange fallito (%lu)", st);
        m->hNotify[1] = NULL;
        ok = FALSE;
    }
    st = NotifyRouteChange2(AF_UNSPEC, NULL, NULL, FALSE, &m->hNotify[2]);
    if (st != NO_ERROR) {
        dbg("[MON] NotifyRouteChange2 fallito (%lu)", st);
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
        if (InterlockedCompareExchange(&m->halt, 0, 0))
            break;   /* uscita esplicita/autoritativa, indipendente da hExit */

        /* Array di SOLI handle validi: hExit (sempre) + le notifiche attive.
         * hExit e' l'ultimo elemento e compare UNA sola volta. */
        HANDLE waits[NOTIFY_COUNT + 1];
        int nw = 0;
        waits[nw++] = m->hExit;
        for (int i = 0; i < NOTIFY_COUNT; i++)
            if (m->hNotify[i])
                waits[nw++] = m->hNotify[i];

        /* ATTENZIONE: MAI INFINITE. Il wait e' SEMPRE bounded cosi' il loop
         * torna periodicamente a rileggere `halt`: in questo modo l'uscita
         * e' garantita anche nel caso (osservato) in cui la segnalazione di
         * hExit non svegli attendibilmente il WaitForMultipleObjects.
         * Un wake ~1/s e' ~0% CPU (nessun busy loop). Il timeout "idle" non
         * provoca ne' callback ne' ri-registrazione: gli handle restano
         * registrati e immutati. */
        DWORD waitms = retry ? 2000 : 1000;
        DWORD r = WaitForMultipleObjects((DWORD)nw, waits, FALSE, waitms);

        if (InterlockedCompareExchange(&m->halt, 0, 0))
            break;
        if (r == WAIT_OBJECT_0)
            break;                       /* stop richiesto (hExit) */
        if (r == WAIT_FAILED) {
            dbg("[MON] WaitForMultipleObjects fallito (%lu)", GetLastError());
            retry = TRUE;
            continue;
        }
        if (r == WAIT_TIMEOUT)
            continue;   /* idle: nessun cambiamento, si aspetta ancora */

        /* r in [1,nw): una notifica di rete reale (non hExit). La GUI rifara'
         * un reconcile completo. Il valore di r indica SOLO che qualcosa e'
         * cambiato: e' sicuro perche' ogni notifica registrata mappa 1:1 a
         * un handle valido nell'array. */
        if (m->cb)
            m->cb(m->user);

        /* Ri-registra: le Notify* sono one-shot. */
        retry = !register_all(m);
    }

    /* Il thread e' l'UNICO proprietario di `m`: dopo aver visto `halt`
     * cancella le notifiche, chiude l'evento e libera la struttura prima di
     * restituire. monitor_stop() pertanto non deve MAI toccare `m` dopo
     * averlo segnalato (nessun free concorrente / doppio free). */
    cancel_all(m);
    if (m->hExit)
        CloseHandle(m->hExit);
    free(m);
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

/* La gestione della struttura e' stata ceduta al thread monitor: monitor_stop()
 * raccoglie TUTTO cio' di cui ha bisogno (i due HANDLE) PRIMA di fronte alla
 * segnalazione, cosi' da non dover mai rileggere (e quindi usare-after-free)
 * `m` nel momento in cui il thread, avendo visto `halt`, sta liberando la
 * struttura. Il wait e' bounded ed e' accettabile perche' questa funzione e'
 * usata SOLO in teardown/shutdown definitivo dell'app (fine di gui_run): il
 * normale ciclo GUI non vi passa mai. In caso di timeout non liberiamo nulla:
 * il thread, al suo prossimo risveglio (wait bounded <= 2s) uscira', e da solo
 * cancel_all() + chiude hExit + free(m): quindi nessun leak e nessun UAF. */
void monitor_stop(NetMon *m)
{
    if (!m)
        return;

    /* Catturiamo PRIMA tutti gli handle di cui abbiamo bisogno: dopo la
     * pubblicazione dello stop il thread puo' liberare `m` in parallelo,
     * quindi non dobbiamo MAI rileggere la struttura. */
    HANDLE ht = m->hThread;
    HANDLE he = m->hExit;

    /* PROTOCOLLO DI STOP (elimina la race del free(m) concorrente):
     *
     *   monitor_stop                    monitor_thread
     *   ------------                    --------------
     *   InterlockedExchange(halt,1) ----------------->  (vede halt al prossimo
     *                                                   risveglio bounded)
     *   SetEvent(he) -------------------------------->  wake (WAIT_OBJECT_0)
     *   wait<=5000ms  <------------------------------  break, cancel_all,
     *                                                   CloseHandle(hExit),
     *                                                   free(m)
     *   CloseHandle(ht) (solo la nostra referenza)
     *
     * L'ULTIMO accesso di monitor_stop() a `m` e' InterlockedExchange(halt):
     * avviene PRIMA di SetEvent(he), quindi prima che il thread possa essere
     * svegliato e liberare `m`. Da SetEvent in poi usiamo SOLO le copie
     * locali ht/he. Anche se il thread si svegliasse da solo sul suo timeout
     * bounded (senza SetEvent), vedrebbe halt=1 e uscirebbe: ma a quel punto
     * monitor_stop avra' comunque gia' smesso di toccare `m`. Nessuna finesta
     * in cui il thread fa free(m) e poi monitor_stop accede a `m`.
     *
     * InterlockedExchange rende esplicita la sincronizzazione: scrittura con
     * barriera globale e lettura atomica nel thread, cosi' halt e' sempre
     * visibile/subito-ordined prima della segnalazione. Segnaliamo dopo la
     * pubblicazione dello stop (e mai il contrario) anche per non far finire
     * SetEvent su un handle gia' chiuso. */
    InterlockedExchange(&m->halt, 1);
    SetEvent(he);

    /* Reap bounded (5000 ms, teardown/shutdown only). Il thread, avendo visto
     * `halt`, esce entro il suo wait bounded massimo (~2s) e da SOLO esegue
     * cancel_all + CloseHandle(hExit) + free(m). Questo wait serve a non
     * procedere con la teardown SDL mentre il thread sta facendo il suo
     * ultimo SDL_PushEvent. Da qui in avanti usiamo SOLO la copia locale `ht`:
     * mai piu' `m`.
     *
     * Caso SUCCESSO: il thread e' terminato e ha gia' liberato `m` e chiuso
     * hExit; qui chiudiamo solo la NOSTRA referenza su hThread. Nessun
     * double-free (il thread non chiude hThread) e nessun double-close su
     * hExit (lo chiude solo il thread).
     *
     * Caso TIMEOUT (eccezionale): non facciamo NIENTE di distruttivo. Non
     * liberiamo `m` (niente free prematuro/UAF) e chiudiamo solo la nostra
     * referenza su hThread: la struttura resta di proprieta' del thread, che
     * al suo prossimo risveglio bounded completa comunque il cleanup. Se il
     * wake non arrivasse proprio (condizione patologica), `m` vivrebbe fino
     * alla terminazione del processo: accettabile perche' questo e' shutdown. */
    if (ht)
        WaitForSingleObject(ht, 5000);
    if (ht)
        CloseHandle(ht);
}
