/*
 * routes.c - gestione delle route IPv4.
 *
 * Strategia:
 *   1. Snapshot della tabella con GetIpForwardTable2.
 *   2. Creazione persistenta con route.exe -p (CreateProcessW, niente shell).
 *   3. Rinforzo nativo con CreateIpForwardEntry2 se la route non compare.
 *   4. Rimozione con route.exe delete + sweep nativo DeleteIpForwardEntry2.
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

BOOL routes_find_host(const RouteList *l, const char *dest,
                      unsigned long *ifindex, char *gateway, size_t gwsz)
{
    for (int i = 0; i < l->count; i++) {
        const HostRoute *hr = &l->items[i];
        if (hr->prefix_len == 32 && strcmp(hr->ip, dest) == 0) {
            if (ifindex)  *ifindex  = hr->ifindex;
            if (gateway && gwsz)
                snprintf(gateway, gwsz, "%s", hr->gateway);
            return TRUE;
        }
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

/* ------------------------------------------------------- esecuzione route */

static BOOL run_route_cli(const wchar_t *cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    /* CREATE_NO_WINDOW: nessuna console lampeggiante, nessuna shell. */
    if (!CreateProcessW(NULL, (LPWSTR)cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

/* Costruisce la command line di route.exe in modo sicuro:
 * gli unici parametri variabili sono IPv4 validate o numeri. */
static BOOL route_cli_persistent_add(const char *ip, const char *gateway,
                                     unsigned long ifindex)
{
    wchar_t cmd[512];
    swprintf(cmd, 512,
             L"route -p add %S mask 255.255.255.255 %S if %lu",
             ip, gateway, ifindex);
    return run_route_cli(cmd);
}

static BOOL route_cli_delete(const char *ip)
{
    wchar_t cmd[384];
    swprintf(cmd, 384, L"route delete %S mask 255.255.255.255", ip);
    run_route_cli(cmd);        /* exit code non affidabile se route assente */
    return TRUE;
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

static BOOL route_nio_delete_matching(const char *ip)
{
    MIB_IPFORWARD_TABLE2 *tab = NULL;
    if (GetIpForwardTable2(AF_INET, &tab) != NO_ERROR)
        return FALSE;

    BOOL ok = TRUE;
    for (DWORD i = 0; i < tab->NumEntries; i++) {
        MIB_IPFORWARD_ROW2 *r = &tab->Table[i];
        if (r->DestinationPrefix.PrefixLength != 32)
            continue;
        char dest[INET_ADDRSTRLEN];
        if (r->DestinationPrefix.Prefix.si_family != AF_INET)
            continue;
        InetNtopA(AF_INET, &r->DestinationPrefix.Prefix.Ipv4.sin_addr,
                  dest, sizeof(dest));
        if (strcmp(dest, ip) != 0)
            continue;
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

    /* 1) Se la route corretta e' gia' presente non serve fare nulla. */
    RouteList snap;
    if (routes_snapshot(&snap)) {
        unsigned long ii; char gw[NET_IP_MAX];
        if (routes_find_host(&snap, ip, &ii, gw, sizeof gw) &&
            strcmp(gw, gateway) == 0) {
            return TRUE;   /* gia' attiva e corretta (indipendente da ifindex) */
        }
    }

    /* 2) route.exe -p: route persistente che sopravvive a riavvio. */
    BOOL cli_ok = route_cli_persistent_add(ip, gateway, ifindex);

    /* 3) Verifica reale in tabella; se manca, rinforzo nativo. */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (routes_snapshot(&snap)) {
            unsigned long ii; char gw[NET_IP_MAX];
            if (routes_find_host(&snap, ip, &ii, gw, sizeof gw) &&
                strcmp(gw, gateway) == 0) {
                return TRUE;   /* presente e corretta */
            }
        }
        if (!route_nio_add(ip, gateway, ifindex))
            break;
    }

    if (err) {
        snprintf(err, errsz, "creazione route fallita (cli_ok=%d)",
                 cli_ok ? 1 : 0);
    }
    return FALSE;
}

BOOL route_delete(const char *ip, char *err, size_t errsz)
{
    if (err) err[0] = '\0';
    if (!net_valid_ipv4(ip)) {
        if (err) snprintf(err, errsz, "indirizzo non valido");
        return FALSE;
    }

    route_cli_delete(ip);          /* rimuove anche la voce persistente */
    route_nio_delete_matching(ip); /* sweep nativo della voce attiva     */
    return TRUE;
}

BOOL route_apply_rule(const char *ip, const char *gateway,
                      unsigned long ifindex, char *err, size_t errsz)
{
    route_delete(ip, NULL, 0);
    return route_add_persistent(ip, gateway, ifindex, err, errsz);
}