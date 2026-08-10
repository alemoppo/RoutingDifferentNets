/*
 * routes.h - gestione delle route IPv4 (host /32).
 *
 *   - Enumeration:  GetIpForwardTable2 / FreeMibTable
 *   - Creazione:    CreateIpForwardEntry2 (nativa) + route.exe -p (persistente)
 *   - Rimozione:    DeleteIpForwardEntry2 (nativa) + route.exe delete
 *
 * La persistenza oltre il riavvio passa da route.exe -p (CreateProcessW,
 * MAI attraverso cmd.exe / system(), quindi nessuna shell injection).
 */
#ifndef ROUTES_H
#define ROUTES_H

#include "common.h"

typedef enum {
    ROUTE_STATUS_OK = 0,
    ROUTE_STATUS_MISSING,
    ROUTE_STATUS_WRONG_INTERFACE,
    ROUTE_STATUS_OFFLINE,
    ROUTE_STATUS_ERROR
} RouteStatus;

typedef struct {
    char          ip[NET_IP_MAX];    /* destinazione   */
    unsigned long prefix_len;        /* 32 per host    */
    unsigned long ifindex;           /* interfaccia    */
    unsigned long metric;            /* metrica route  */
    char          gateway[NET_IP_MAX]; /* next hop      */
} HostRoute;

typedef struct {
    HostRoute items[ROUTE_SNAP_MAX];
    int       count;
} RouteList;

/* Snapshot della tabella di routing IPv4 corrente. */
BOOL routes_snapshot(RouteList *out);

/* Cerca una route di host (mask /32) verso 'dest'. Ritorna TRUE se esiste e
 * riempie ifindex/gateway. */
BOOL routes_find_host(const RouteList *l, const char *dest,
                      unsigned long *ifindex, char *gateway, size_t gwsz);

/* ifindex dell'interfaccia sulla quale passa la default route (0.0.0.0/0),
 * oppure 0 se non presente. */
unsigned long routes_default_ifindex(const RouteList *l);

/* Verifica che una stringa sia un indirizzo IPv4 valido. */
BOOL net_valid_ipv4(const char *s);

/* Crea una route host persistenta e attiva: ip/32 -> gateway via ifindex.
 * Verifica la presenza effettiva in tabella e, se necessario, usa
 * CreateIpForwardEntry2 come rinforzo nativo. */
BOOL route_add_persistent(const char *ip, const char *gateway,
                          unsigned long ifindex, char *err, size_t errsz);

/* Rimuove la route host verso 'ip' (persistente e attiva). */
BOOL route_delete(const char *ip, char *err, size_t errsz);

/* Remove + add: usata per correggere una route conflittuale. */
BOOL route_apply_rule(const char *ip, const char *gateway,
                      unsigned long ifindex, char *err, size_t errsz);

#endif /* ROUTES_H */