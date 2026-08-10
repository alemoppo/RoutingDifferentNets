/*
 * network.h - enumerazione delle interfacce di rete IPv4.
 *
 * L'identita' stabile di un adattatore e' il suo GUID (campo AdapterName di
 * GetAdaptersAddresses). L'ifIndex ed il gateway corrente vengono rilevati
 * dinamicamente ad ogni snapshot: la configurazione non deve MAI salvare
 * ifIndex / IPv4 come identita' permanente.
 */
#ifndef NETWORK_H
#define NETWORK_H

#include "common.h"

typedef enum {
    NET_DISCONNECTED = 0,
    NET_CONNECTED    = 1
} NetState;

typedef struct {
    char          friendly_name[NET_NAME_MAX]; /* "Wi-Fi", "Ethernet 2", ... */
    char          guid[NET_GUID_MAX];          /* GUID stabile dell'adattatore  */
    char          description[NET_DESC_MAX];   /* descrizione hardware          */
    unsigned long ifindex;                     /* indice corrente (0 se assente) */
    NetState      state;                       /* connesso / scollegato         */
    char          ipv4[NET_IP_MAX];            /* indirizzo IPv4 locale          */
    char          gateway[NET_IP_MAX];         /* gateway IPv4 corrente         */
    unsigned long metric;                      /* metrica dell'interfaccia      */
    BOOL          has_default_route;           /* 0.0.0.0/0 passa di qui?       */
} NetInterface;

typedef struct {
    NetInterface items[NET_MAX_IFACES];
    int          count;
} NetList;

/* Enumerazione completa delle interfacce IPv4 (ad eccezione del loopback).
 * Connesse per prime, poi ordinate per nome. */
void net_snapshot(NetList *out);

/* Ricerca per GUID stabile; fallback per nome. Ritornano NULL se assenti. */
const NetInterface *net_find_by_guid(const NetList *l, const char *guid);
const NetInterface *net_find_by_name(const NetList *l, const char *name);
const NetInterface *net_resolve(const NetList *l, const char *guid, const char *name);

/* Indice (0..count-1) dell'interfaccia puntata da p, oppure -1. */
int net_index_of(const NetList *l, const NetInterface *p);

#endif /* NETWORK_H */