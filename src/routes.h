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

/* Cerca una route esattamente corrispondente a destination+prefix+ifIndex+
 * gateway. Windows puo' contenere piu' route /32 verso lo stesso IP (es. una
 * VPN): una route gestita da RoutingDifferentNets e' "corretta" SOLO se TUTTI
 * i parametri coincidono. Ritorna TRUE se esiste una corrispondenza esatta. */
BOOL routes_find_host_exact(const RouteList *l, const char *dest,
                            unsigned long prefix, unsigned long ifindex,
                            const char *gateway);

/* ifindex dell'interfaccia sulla quale passa la default route (0.0.0.0/0),
 * oppure 0 se non presente. */
unsigned long routes_default_ifindex(const RouteList *l);

/* Verifica che una stringa sia un indirizzo IPv4 valido. */
BOOL net_valid_ipv4(const char *s);

/* Crea una route host attiva: ip/32 -> gateway via ifindex.
 * - Verifica la presenza ESATTA in tabella e, se assente, la crea con
 *   CreateIpForwardEntry2 (nativo, senza bloccare la GUI).
 * - La persistenza oltre il riavvio viene delegata a route.exe -p lanciato
 *   in modo asincrono (nessuna attesa bloccante).
 * Se esiste gia' una route diversa verso lo stesso IP (es. VPN) non viene
 * toccata: le route possono coesistere e la /32 con metrica bassa prevale. */
BOOL route_add_persistent(const char *ip, const char *gateway,
                          unsigned long ifindex, char *err, size_t errsz);

/* Rimuove SOLO la route gestita, con corrispondenza esatta
 * destination+prefix+ifIndex+gateway (attiva e persistente).
 * Se gateway/ifIndex non sono noti (es. interfaccia assente) viene rimossa
 * l'eventuale voce persistente con `route.exe delete`, lasciando intatte le
 * route attive di terze parti. */
BOOL route_delete(const char *ip, const char *gateway, unsigned long ifindex,
                  char *err, size_t errsz);

/* Recupera e logga gli exit code dei route.exe asincroni pendenti. Da
 * chiamare periodicamente dal loop GUI (senza attese bloccanti). */
void routes_cli_poll(void);

#endif /* ROUTES_H */