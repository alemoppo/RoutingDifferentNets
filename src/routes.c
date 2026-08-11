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
 * route.exe viene lanciato in modo ASINCRONO per non bloccare la GUI, ma per
 * lo STESSO destination IP le operazioni vengono serializzate: prima di
 * lanciare un comando attendiamo il completamento di un eventuale comando
 * precedente ancora in volo (route.exe termina in pochi ms, quindi l'attesa
 * e' trascurabile). Questo evita la race "add persistente" vs "delete
 * persistente" dove l'ordine di completamento non sarebbe garantito. */

#define MAX_PENDING_CLI 32
typedef struct {
    char   ip[NET_IP_MAX];
    HANDLE h;
} PendingCli;

static PendingCli g_pending[MAX_PENDING_CLI];
static int g_npending = 0;

/* Raccoglie i processi gia' terminati e compatta la lista. */
static void cli_reap(void)
{
    for (int i = g_npending - 1; i >= 0; i--) {
        if (g_pending[i].h &&
            WaitForSingleObject(g_pending[i].h, 0) == WAIT_OBJECT_0) {
            DWORD ec = 0;
            GetExitCodeProcess(g_pending[i].h, &ec);
            dbg("[ROUTE] route.exe terminato exit=%lu (%s)", ec, g_pending[i].ip);
            CloseHandle(g_pending[i].h);
            g_pending[i].h = NULL;
        }
    }
    int w = 0;
    for (int i = 0; i < g_npending; i++)
        if (g_pending[i].h)
            g_pending[w++] = g_pending[i];
    g_npending = w;
}

/* Attende (con timeout di sicurezza) il completamento di ogni route.exe
 * ancora in volo per lo stesso IP destinazione. */
static void cli_wait_dest(const char *ip)
{
    for (int i = 0; i < g_npending; i++) {
        if (g_pending[i].h && strcmp(g_pending[i].ip, ip) == 0) {
            WaitForSingleObject(g_pending[i].h, 3000);
            CloseHandle(g_pending[i].h);
            g_pending[i].h = NULL;
        }
    }
    int w = 0;
    for (int i = 0; i < g_npending; i++)
        if (g_pending[i].h)
            g_pending[w++] = g_pending[i];
    g_npending = w;
}

/* Lancia route.exe: prima serializza con eventuali operazioni in corso sullo
 * stesso IP, poi parte senza bloccare la GUI. Il handle resta tracciato per
 * evitare race con operazioni successive sullo stesso IP. */
static void cli_run(const wchar_t *cmdline, const char *ip)
{
    cli_reap();
    cli_wait_dest(ip);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    /* CREATE_NO_WINDOW: nessuna console lampeggiante, nessuna shell. */
    if (!CreateProcessW(NULL, (LPWSTR)cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        dbg("[ROUTE] CreateProcessW FALLITO (%lu): %S", GetLastError(), cmdline);
        return;
    }
    CloseHandle(pi.hThread);

    if (g_npending < MAX_PENDING_CLI) {
        snprintf(g_pending[g_npending].ip, sizeof(g_pending[g_npending].ip),
                 "%s", ip);
        g_pending[g_npending].h = pi.hProcess;
        g_npending++;
    } else {
        CloseHandle(pi.hProcess);
    }
}

/* Variante SINCRONA di route.exe: attende il completamento (route.exe termina
 * in pochi ms) e ritorna l'exit code, oppure 0xFFFFFFFF se il processo non e'
 * stato nemmeno lanciato. Usata solo per le cancellazioni, dove serve
 * conoscere l'esito reale di route.exe per propagarlo al chiamante. */
#define CLI_EXIT_SPAWN_FAIL 0xFFFFFFFFu
static DWORD cli_run_sync(const wchar_t *cmdline, const char *ip)
{
    cli_reap();
    cli_wait_dest(ip);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, (LPWSTR)cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        dbg("[ROUTE] CreateProcessW FALLITO (%lu): %S", GetLastError(), cmdline);
        return CLI_EXIT_SPAWN_FAIL;
    }
    CloseHandle(pi.hThread);

    DWORD rc = CLI_EXIT_SPAWN_FAIL;
    if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    dbg("[ROUTE] route.exe sincrono exit=%lu (%s)", rc, ip);
    return rc;
}

/* route -p add: voce persistente che sopravvive al riavvio. */
static void route_cli_persistent_add(const char *ip, const char *gateway,
                                     unsigned long ifindex)
{
    wchar_t cmd[512];
    swprintf(cmd, 512,
             L"route -p add %hs mask 255.255.255.255 %hs if %lu",
             ip, gateway, ifindex);
    cli_run(cmd, ip);
}

/* route delete SPECIFICO: destination + mask + gateway [+ if]. Mai senza
 * gateway: una cancellazione alla cieca potrebbe rimuovere la voce
 * persistente di terze parti verso lo stesso IP. Eseguito in modo SINCRONO
 * perche' dobbiamo conoscere l'esito reale di route.exe e propagarlo.
 * Ritorna TRUE se route.exe e' partito e ha concluso con exit code 0. */
static BOOL route_cli_delete(const char *ip, const char *gateway,
                             unsigned long ifindex)
{
    wchar_t cmd[512];
    if (ifindex != 0)
        swprintf(cmd, 512, L"route delete %hs mask 255.255.255.255 %hs if %lu",
                 ip, gateway, ifindex);
    else
        swprintf(cmd, 512, L"route delete %hs mask 255.255.255.255 %hs",
                 ip, gateway);
    DWORD rc = cli_run_sync(cmd, ip);
    return rc != CLI_EXIT_SPAWN_FAIL && rc == 0;
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
        route_cli_persistent_add(ip, gateway, ifindex);
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

    /* 3) Persistenza asincrona: non bloccante per la GUI. */
    if (ok)
        route_cli_persistent_add(ip, gateway, ifindex);

    if (ok) {
        dbg("[ROUTE] %s/32 aggiunta via %s (if %lu)", ip, gateway, ifindex);
        return TRUE;
    }
    if (err)
        snprintf(err, errsz, "creazione route %s/32 fallita", ip);
    dbg("[ROUTE] %s/32 ERRORE in creazione", ip);
    return FALSE;
}

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
