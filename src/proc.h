#ifndef PROC_H
#define PROC_H

#include "common.h"

#define PROC_MAX_IPS 8

/* Queste funzioni si appoggiano a winsock2/iphlpapi: includere questo
 * header SOLO dopo che la sequenza winsock2 -> windows di common.h e'
 * stata inclusa. */

/* Risolve gli IP IPv4 raggiunti/connessi da ogni istanza del processo
 * il cui eseguibile corrisponde a `name` (con o senza estensione .exe).
 * Riempie `ips` con gli indirizzi unici e ritorna il numero trovato
 * (0 = nessun processo corrispondente o nessun IP). */
int proc_resolve_ips(const char *name,
                     char ips[PROC_MAX_IPS][NET_IP_MAX], int *count);

#endif /* PROC_H */