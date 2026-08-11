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

/* ------------------------------------------------------------ route.exe (async) */

/* Lancia route.exe con la command line data, SENZA attendere il termine:
 * la route attiva viene creata/rimossa con le API native; route.exe serve
 * solo alla persistenza tra riavvii e non deve mai bloccare la GUI. */
static void run_route_cli_async(const wchar_t *cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    /* CREATE_NO_WINDOW: nessuna console lampeggiante, nessuna shell. */
    if (!CreateProcessW(NULL, (LPWSTR)cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* route -p add: voce persistente che sopravvive al riavvio. */
static void route_cli_persistent_add(const char *ip, const char *gateway,
                                     unsigned long ifindex)
{
    wchar_t cmd[512];
    swprintf(cmd, 512,
             L"route -p add %S mask 255.255.255.255 %S if %lu",
             ip, gateway, ifindex);
    run_route_cli_async(cmd);
}

/* route delete con gateway: rimuove la voce persistente che corrisponde
 * a destination+mask+gateway, lasciando intatte eventuali route persistenti
 * di terze parti verso lo stesso IP ma con gateway diverso. */
static void route_cli_delete(const char *ip, const char *gateway)
{
    wchar_t cmd[512];
    if (gateway && gateway[0])
        swprintf(cmd, 512, L"route delete %S mask 255.255.255.255 %S",
                 ip, gateway);
    else
        swprintf(cmd, 512, L"route delete %S mask 255.255.255.255", ip);
    run_route_cli_async(cmd);
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
    /* Metrica minima: tra piu' route /32 verso lo stesso IP vince la nostra. */
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

    /* 1) Route gia' esattamente corretta: nessuna operazione. */
    RouteList snap;
    if (routes_snapshot(&snap) &&
        routes_find_host_exact(&snap, ip, 32, ifindex, gateway)) {
        dbg("[ROUTE] %s/32 OK (gia' presente)", ip);
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

    if (gateway && gateway[0] && ifindex != 0) {
        /* Rimozione precisa della sola route gestita (attiva + persistente). */
        route_nio_delete_exact(ip, 32, ifindex, gateway);
        route_cli_delete(ip, gateway);
        dbg("[ROUTE] %s/32 eliminata (if %lu gw %s)", ip, ifindex, gateway);
    } else {
        /* ifIndex/gateway non noti (interfaccia assente): si rimuove solo la
         * voce persistente, senza cancellare route attive di terze parti. */
        route_cli_delete(ip, NULL);
        dbg("[ROUTE] %s/32: voce persistente rimossa (gw ignoto)", ip);
    }
    return TRUE;
}
