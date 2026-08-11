/*
 * routes.c - gestione delle route IPv4.
 *
 * Concetto: RoutingDifferentNets gestisce ESCLUSIVAMENTE gli IP configurati
 * dall'utente. Una route e' considerata "corretta" solo se destination,
 * prefix, ifIndex E gateway coincidono con i parametri correnti dell'
 * interfaccia configurata. Se Windows contiene piu' route /32 verso lo stesso
 * IP (es. una VPN), quella di RoutingDifferentNets coesiste senza cancellare
 * le altre: la regola configurata viene preferita grazie alla massima
 * specificita' (/32) e alla metrica bassa.
 *
 * Strategia:
 *   1. Snapshot della tabella con GetIpForwardTable2.
 *   2. Creazione dell'route attiva con CreateIpForwardEntry2 (API nativa,
 *      nessuna attesa bloccante).
 *   3. Persistenza con route.exe -p lanciato in modo ASINCRONO
 *      (CreateProcessW, CREATE_NO_WINDOW, niente shell, nessun blocco GUI).
 *   4. Rimozione con corrispondenza ESATTA tramite DeleteIpForwardEntry2 e
 *      route.exe delete specifico per gateway.
 *
 * Tutti i parametri testuali (indirizzi IPv4) passano attraverso
 * net_valid_ipv4 prima di finire nella command line, quindi non e' possibile
 * alcuna injection: la command line contiene solo numeri e punti.
 */
#include "routes.h"
#include <iprtrmib.h>

#ifndef MIB_IPPROTO_NETMGMT
#define MIB_IPPROTO_NETMGMT 13
#endif

/* ---------------------------------------------------------------- snapshot */

BOOL routes_snapshot(RouteList *out)
{
    out->count = 0;

    MIB_IPFORWARD_TABLE2 *tab = NULL;
    if (GetIpForwardTable2(AF_INET, &tab) != NO_ERROR)
        return FALSE;

    for (DWORD i = 0; i < tab->NumEntries && out->count < ROUTE_SNAP_MAX; i++) {
        const MIB_IPFORWARD_ROW2 *r = &tab->Table[i];
        HostRoute *hr = &out->items[out->count];
        memset(hr, 0, sizeof(*hr));

        hr->ifindex    = r->InterfaceIndex;
        hr->metric     = r->Metric;
        hr->prefix_len = r->DestinationPrefix.PrefixLength;

        if (r->DestinationPrefix.Prefix.si_family == AF_INET) {
            InetNtopA(AF_INET, &r->DestinationPrefix.Prefix.Ipv4.sin_addr,
                      hr->ip, sizeof(hr->ip));
            /* "0.0.0.0" = default route, usata solo per verifica GUI. */
            if (hr->prefix_len == 0)
                snprintf(hr->ip, sizeof(hr->ip), "0.0.0.0/0");
        }
        if (r->NextHop.si_family == AF_INET) {
            InetNtopA(AF_INET, &r->NextHop.Ipv4.sin_addr,
                      hr->gateway, sizeof(hr->gateway));
        }
        out->count++;
    }

    FreeMibTable(tab);
    return TRUE;
}

/* Corrispondenza esatta: destination + prefix + ifIndex + gateway. */
BOOL routes_find_host_exact(const RouteList *l, const char *dest,
                            unsigned long prefix, unsigned long ifindex,
                            const char *gateway)
{
    for (int i = 0; i < l->count; i++) {
        const HostRoute *hr = &l->items[i];
        if (hr->prefix_len != prefix)
            continue;
        if (strcmp(hr->ip, dest) != 0)
            continue;
        if (hr->ifindex != ifindex)
            continue;
        if (gateway && gateway[0] && strcmp(hr->gateway, gateway) != 0)
            continue;
        return TRUE;
    }
    return FALSE;
}

unsigned long routes_default_ifindex(const RouteList *l)
{
    for (int i = 0; i < l->count; i++) {
        const HostRoute *hr = &l->items[i];
        if (hr->prefix_len == 0)      /* 0.0.0.0/0 */
            return hr->ifindex;
    }
    return 0;
}

BOOL net_valid_ipv4(const char *s)
{
    if (!s || !*s)
        return FALSE;
    struct in_addr a;
    return InetPtonA(AF_INET, s, &a) == 1;
}

/* ------------------------------------------------------------ route.exe (async)
 * route.exe viene lanciato SEMPRE in modo ASINCRONO e MAI dal thread GUI.
 * Non esiste alcun wait sincrono: le operazioni vengono accodate in una coda
 * e avviate solo quando (a) nessun altro route.exe e' in volo per lo stesso
 * destination IP (serializzazione per-IP) e (b) c'e' un process-slot libero.
 * Se non ci sono le condizioni il comando resta PENDING e viene ripartito
 * alla prossima routes_cli_poll()/cli_drain(): niente operazioni perse.
 *
 * NOTA sulla semantica: la coda NON e' una FIFO globale. La proprieta' reale
 * garantita e' la serializzazione PER IP: per lo stesso destination le
 * operazioni partono nell'ordine in cui sono state richieste (ADD/DELETE/ADD),
 * mentre IP differenti possono essere in volo in parallelo, fino a
 * MAX_CONCURRENT_CLI. Esempio valido:
 *   ADD A -> running, ADD B -> running, DELETE A -> pending, DELETE B -> pending
 *   (DELETE A parte appena ADD A termina, DELETE B appena ADD B termina)
 *
 * Questo garantisce:
 *   - serializzazione per IP (mai due route.exe sullo stesso IP);
 *   - parallelismo tra IP differenti (fino a MAX_CONCURRENT_CLI);
 *   - GUI mai bloccata da route.exe;
 *   - controllo dell'exit code recuperato in cli_reap().
 */

/* Numero massimo di route.exe CONTEMPORANEAMENTE in volo (parallelismo).
 * 16 basta per servire piu' IP in parallelo ma evita di lanciare centinaia di
 * processi in un reconcile massivo: ogni route.exe e' veloce (ordine dei ms),
 * quindi con 16 slot le code fluiscono senza saturarsi. NON limita la
 * capacita' di accodamento (vedi MAX_CLI_OP): i comandi eccedenti restano
 * pending. */
#define MAX_CONCURRENT_CLI 16

/* Capacita' totale della coda (in volo + pending). Indipendente da
 * MAX_CONCURRENT_CLI: dimensionata abbondante rispetto al numero massimo di
 * route *per* le operazioni chiuse in serie sullo stesso IP (es. una
 * reconcile che fa molti delete+add), cosi' da poter accodare tutto senza
 * mai perdere operazioni. */
#define MAX_CLI_OP (CONFIG_MAX_ROUTES * 2 + 16)

typedef enum { CLI_ADD, CLI_DELETE } CliKind;

typedef struct {
    char           ip[NET_IP_MAX];
    char           gw[NET_IP_MAX];
    unsigned long  ifindex;
    CliKind        kind;
    BOOL           started;   /* TRUE: route.exe lanciato (handle valido) */
    HANDLE         h;         /* handle processo, valido solo se started */
} CliOp;

static CliOp g_cli[MAX_CLI_OP];
static int g_ncli = 0;          /* operazioni in coda (pending + started) */

/* TRUE se un'operazione e' in VOLO (processo vivo) per lo stesso IP. */
static BOOL cli_ip_busy(const char *ip)
{
    for (int i = 0; i < g_ncli; i++)
        if (g_cli[i].started && strcmp(g_cli[i].ip, ip) == 0)
            return TRUE;
    return FALSE;
}

/* Numero di route.exe correntemente in volo. */
static int cli_inflight(void)
{
    int n = 0;
    for (int i = 0; i < g_ncli; i++)
        if (g_cli[i].started)
            n++;
    return n;
}

/* Raccoglie i processi gia' terminati (rimuove l'operazione) e recupera il
 * loro exit code reale. Non blocca mai (wait timeout = 0). */
static void cli_reap(void)
{
    int w = 0;
    for (int i = 0; i < g_ncli; i++) {
        if (g_cli[i].started &&
            WaitForSingleObject(g_cli[i].h, 0) == WAIT_OBJECT_0) {
            DWORD ec = 0;
            GetExitCodeProcess(g_cli[i].h, &ec);
            /* route.exe delete e' idempotente: "impossibile trovare elemento"
             * termina comunque con exit 0, quindi NOT_FOUND resta successo.
             * Un exit != 0 e' un errore reale e viene segnalato. */
            if (g_cli[i].kind == CLI_DELETE && ec != 0)
                dbg("[ROUTE] delete %s/32 exit=%lu (ERRORE reale)", g_cli[i].ip, ec);
            else
                dbg("[ROUTE] route.exe terminato exit=%lu (%s)", ec, g_cli[i].ip);
            CloseHandle(g_cli[i].h);
            continue;   /* operazione consumata */
        }
        g_cli[w++] = g_cli[i];
    }
    g_ncli = w;
}

/* Costruisce la riga di comando per l'operazione accodata. */
static int cli_build_cmd(const CliOp *op, wchar_t *cmd, size_t n)
{
    if (op->kind == CLI_ADD)
        return swprintf(cmd, n, L"route -p add %hs mask 255.255.255.255 %hs if %lu",
                        op->ip, op->gw, op->ifindex);
    if (op->ifindex != 0)
        return swprintf(cmd, n, L"route delete %hs mask 255.255.255.255 %hs if %lu",
                        op->ip, op->gw, op->ifindex);
    return swprintf(cmd, n, L"route delete %hs mask 255.255.255.255 %hs",
                    op->ip, op->gw);
}

/* Prova a lanciare un'operazione pending: avviata SOLO se l'IP e' libero e
 * c'e' un process-slot. Se fallisce, resta pending. Ritorna TRUE se avviata. */
static BOOL cli_start_op(CliOp *op)
{
    if (cli_ip_busy(op->ip))
        return FALSE;                       /* serializzazione per IP */
    if (cli_inflight() >= MAX_CONCURRENT_CLI)
        return FALSE;                       /* backpressure non bloccante */

    wchar_t cmd[512];
    if (cli_build_cmd(op, cmd, 512) < 0)
        return FALSE;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    /* CREATE_NO_WINDOW: nessuna console lampeggiante, nessuna shell. */
    if (!CreateProcessW(NULL, (LPWSTR)cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        dbg("[ROUTE] CreateProcessW FALLITO (%lu): %S", GetLastError(), cmd);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    op->started = TRUE;
    op->h = pi.hProcess;
    return TRUE;
}

/* Avvia quante piu' operazioni pending possibile, nel rispetto della
 * serializzazione per-IP e del limite MAX_CONCURRENT_CLI (NOTA: non FIFO
 * globale -- IP differenti possono girare in parallelo). Chiamato dopo ogni
 * accodamento e da routes_cli_poll(). Non blocca mai. */
static void cli_drain(void)
{
    cli_reap();
    for (int pass = 0; pass < g_ncli; pass++) {
        int advanced = 0;
        for (int i = 0; i < g_ncli; i++) {
            if (!g_cli[i].started && cli_start_op(&g_cli[i]))
                advanced = 1;
        }
        if (!advanced)
            break;
    }
}

/* Accoda un'operazione route.exe. Ritorna TRUE se accodata (potrebbe ancora
 * essere pending). FALSE solo se la coda e' piena: in tal caso logga (mai
 * drop silenzioso) e non perde l'informazione che l'operazione non e' stata
 * neppure programmata. */
static BOOL cli_enqueue(const char *ip, const char *gw, unsigned long ifindex,
                        CliKind kind)
{
    if (g_ncli >= MAX_CLI_OP) {
        dbg("[ROUTE] coda CLI piena (%d): op %s non accodata",
            MAX_CLI_OP, kind == CLI_DELETE ? "delete" : "add");
        return FALSE;
    }
    CliOp *op = &g_cli[g_ncli];
    memset(op, 0, sizeof(*op));
    snprintf(op->ip, sizeof(op->ip), "%s", ip);
    snprintf(op->gw, sizeof(op->gw), "%s", gw ? gw : "");
    op->ifindex = ifindex;
    op->kind = kind;
    g_ncli++;
    cli_drain();   /* tenta l'avvio immediato se possibile */
    return TRUE;
}

/* route -p add: voce persistente che sopravvive al riavvio (asincrono).
 * Ritorna TRUE se l'operazione e' stata ACCODATA per l'esecuzione (potrebbe
 * essere ancora pending), FALSE solo se la coda era piena: in tal caso la
 * persistenza NON e' programmata e verra' ritentata da un successivo
 * reconcile (che ri-accoda la persistenza per ogni route in stato OK). */
static BOOL route_cli_persistent_add(const char *ip, const char *gateway,
                                     unsigned long ifindex)
{
    return cli_enqueue(ip, gateway, ifindex, CLI_ADD);
}

/* route delete SPECIFICO: destination + mask + gateway [+ if]. Mai senza
 * gateway: una cancellazione alla cieca potrebbe rimuovere la voce
 * persistente di terze parti verso lo stesso IP.
 *
 * ASINCRONO: accoda route.exe senza bloccare la GUI; l'esito reale viene
 * recuperato in cli_reap(). Il NOT_FOUND (exit 0) e' idempotente ed e'
 * considerato successo. Un fallimento di spawn (CreateProcessW) o un exit
 * != 0 va considerato errore reale.
 *
 * Semantica del return: TRUE = operazione ACCETTATA in coda (NON significa
 * che route.exe sia gia' completato con exit 0). FALSE = coda piena, quindi
 * il delete persistente non e' nemmeno stato accodato: il chiamante (route_delete)
 * deve propagarlo. */
static BOOL route_cli_delete(const char *ip, const char *gateway,
                             unsigned long ifindex)
{
    return cli_enqueue(ip, gateway, ifindex, CLI_DELETE);
}

/* ------------------------------------------------------ API native (netio) */

static BOOL route_nio_add(const char *ip, const char *gateway,
                          unsigned long ifindex)
{
    if (!net_valid_ipv4(ip) || !net_valid_ipv4(gateway))
        return FALSE;

    MIB_IPFORWARD_ROW2 row;
    memset(&row, 0, sizeof(row));

    row.DestinationPrefix.PrefixLength = 32;
    row.DestinationPrefix.Prefix.si_family = AF_INET;
    if (InetPtonA(AF_INET, ip, &row.DestinationPrefix.Prefix.Ipv4.sin_addr) != 1)
        return FALSE;

    row.InterfaceLuid.Value = 0;
    row.InterfaceIndex      = ifindex;
    /* Metrica route minima (1): Windows seleziona la route vincente sommando
     * metrica interfaccia + metrica route, quindi la /32 e' preferita ma una
     * route VPN verso lo stesso IP con metrica combinata inferiore puo'
     * comunque prevalere (decisione dello stack di routing). */
    row.Metric              = 1;

    row.NextHop.si_family = AF_INET;
    if (InetPtonA(AF_INET, gateway, &row.NextHop.Ipv4.sin_addr) != 1)
        return FALSE;

    row.Protocol         = (NL_ROUTE_PROTOCOL)MIB_IPPROTO_NETMGMT;
    row.Loopback         = FALSE;
    row.AutoconfigureAddress = FALSE;
    row.Publish          = FALSE;
    row.Immortal         = FALSE;
    row.Age              = 0;
    row.Origin           = (NL_ROUTE_ORIGIN)0;

    return CreateIpForwardEntry2(&row) == NO_ERROR;
}

/* Elimina le route attive che corrispondono ESATTAMENTE a
 * destination+prefix+ifIndex+gateway: non tocca altre route verso lo stesso
 * IP (es. quelle di una VPN). */
static BOOL route_nio_delete_exact(const char *ip, unsigned long prefix,
                                   unsigned long ifindex, const char *gateway)
{
    MIB_IPFORWARD_TABLE2 *tab = NULL;
    if (GetIpForwardTable2(AF_INET, &tab) != NO_ERROR)
        return FALSE;

    BOOL ok = TRUE;
    for (DWORD i = 0; i < tab->NumEntries; i++) {
        MIB_IPFORWARD_ROW2 *r = &tab->Table[i];
        if (r->DestinationPrefix.PrefixLength != prefix)
            continue;
        if (r->InterfaceIndex != ifindex)
            continue;
        if (r->DestinationPrefix.Prefix.si_family != AF_INET)
            continue;
        char dest[INET_ADDRSTRLEN];
        InetNtopA(AF_INET, &r->DestinationPrefix.Prefix.Ipv4.sin_addr,
                  dest, sizeof(dest));
        if (strcmp(dest, ip) != 0)
            continue;
        if (gateway && gateway[0]) {
            char g[INET_ADDRSTRLEN];
            if (r->NextHop.si_family != AF_INET)
                continue;
            InetNtopA(AF_INET, &r->NextHop.Ipv4.sin_addr, g, sizeof(g));
            if (strcmp(g, gateway) != 0)
                continue;
        }
        if (DeleteIpForwardEntry2(r) != NO_ERROR)
            ok = FALSE;
    }

    FreeMibTable(tab);
    return ok;
}

/* Recupera gli exit code dei route.exe già terminati e avvia le operazioni
 * ancora PENDING (backpressure non bloccante). Chiamato periodicamente dal
 * loop GUI: cosi' le operazioni accodate partono appena liberi il process-slot
 * o l'IP, e ogni esito viene sempre letto. Non blocca mai (wait timeout = 0). */
void routes_cli_poll(void)
{
    cli_drain();
}

/* ---------------------------------------------------------------- API pub */

BOOL route_add_persistent(const char *ip, const char *gateway,
                          unsigned long ifindex, char *err, size_t errsz)
{
    if (err) err[0] = '\0';
    if (!net_valid_ipv4(ip) || !net_valid_ipv4(gateway) || ifindex == 0) {
        if (err) snprintf(err, errsz, "parametri route non validi");
        return FALSE;
    }

    /* 1) Se la route attiva e' gia' esattamente corretta, non serve ricrearla,
     *    ma la PERSISTENZA va comunque garantita: la ri-iscriviamo via
     *    route.exe -p, operazione idempotente che non crea duplicati. Se la
     *    route attiva NON e' presente, si procede al punto 2 (creazione
     *    nativa). */
    RouteList snap;
    if (routes_snapshot(&snap) &&
        routes_find_host_exact(&snap, ip, 32, ifindex, gateway)) {
        dbg("[ROUTE] %s/32 OK (gia' presente) - assicuro persistenza", ip);
        if (!route_cli_persistent_add(ip, gateway, ifindex))
            dbg("[ROUTE] %s/32: enqueue persistenza fallita (coda piena), "
                "ritentata al prossimo reconcile", ip);
        return TRUE;
    }

    /* 2) Creazione nativa della route attiva (senza attese bloccanti). */
    BOOL ok = FALSE;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (route_nio_add(ip, gateway, ifindex)) {
            if (routes_snapshot(&snap) &&
                routes_find_host_exact(&snap, ip, 32, ifindex, gateway)) {
                ok = TRUE;
                break;
            }
        }
    }

    /* 3) Persistenza asincrona: non bloccante per la GUI. L'eventuale
     * fallimento di accodamento (coda piena) non annulla la route attiva gia'
     * creata: viene solo loggato e la persistenza e' ritentata da un
     * successivo reconcile. */
    if (ok) {
        if (!route_cli_persistent_add(ip, gateway, ifindex))
            dbg("[ROUTE] %s/32: enqueue persistenza fallita (coda piena), "
                "sara' ritentato da reconcile", ip);
    }

    if (ok) {
        dbg("[ROUTE] %s/32 aggiunta via %s (if %lu)", ip, gateway, ifindex);
        return TRUE;
    }
    if (err)
        snprintf(err, errsz, "creazione route %s/32 fallita", ip);
    dbg("[ROUTE] %s/32 ERRORE in creazione", ip);
    return FALSE;
}

/* Rimuove una route (attiva con API nativa + persistente con route.exe).
 * Rimane ASINCRONO per la parte persistente: il return TRUE significa
 * "delete attivo eseguito E delete persistente ACCODATA" -- NON che route.exe
 * sia gia' terminato con exit 0 (quello lo recupera cli_reap()). FALSE = la
 * delete attiva e' fallita, oppure la parte persistente non e' stata accodata
 * (coda piena): in entrambi i casi il chiamante NON deve considerare la route
 * rimossa. Un eventuale errore asincrono di route.exe non lascia stato
 * incoerente: la config resta presente e il successivo reconcile() riconcilia. */
BOOL route_delete(const char *ip, const char *gateway, unsigned long ifindex,
                  char *err, size_t errsz)
{
    if (err) err[0] = '\0';
    if (!net_valid_ipv4(ip)) {
        if (err) snprintf(err, errsz, "indirizzo non valido");
        return FALSE;
    }

    /* Cancellazione ESATTA (attiva + persistente) SOLO se conosciamo i
     * parametri con cui la route fu creata. Senza gateway/ifIndex una
     * rimozione alla cieca potrebbe colpire una route di terze parti verso
     * lo stesso IP (es. VPN): quindi rifiutiamo. */
    if (!gateway || !gateway[0] || ifindex == 0) {
        if (err)
            snprintf(err, errsz, "parametri di cancellazione non noti");
        dbg("[ROUTE] %s/32: cancellazione rifiutata (gw/if ignoti)", ip);
        return FALSE;
    }

    /* 1) Rimozione della route ATTIVA (API nativa). */
    BOOL nio_ok = route_nio_delete_exact(ip, 32, ifindex, gateway);

    /* 2) Rimozione della voce PERSISTENTE (route.exe). */
    BOOL cli_ok = route_cli_delete(ip, gateway, ifindex);

    if (!nio_ok || !cli_ok) {
        if (err) {
            snprintf(err, errsz,
                     "errore rimozione %s/32 (nio:%s cli:%s)", ip,
                     nio_ok ? "ok" : "FAIL",
                     cli_ok ? "ok" : "FAIL");
        }
        dbg("[ROUTE] %s/32 ERRORE in rimozione (nio:%d cli:%d)",
            ip, nio_ok, cli_ok);
        return FALSE;
    }

    dbg("[ROUTE] %s/32 eliminata (if %lu gw %s)", ip, ifindex, gateway);
    return TRUE;
}
