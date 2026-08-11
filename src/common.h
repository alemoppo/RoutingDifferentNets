/*
 * common.h - header condiviso dal progetto "Network Route Manager".
 *
 * Include centralizzati Windows / Winsock / IP Helper e costanti comuni.
 * Ordine degli include importante: <winsock2.h> PRIMA di <windows.h>.
 */
#ifndef COMMON_H
#define COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdbool.h>

/* Capacita' dei buffer di testo. */
#define NET_IP_MAX      64        /* stringa IPv4 (max 15 + '\0') */
#define NET_NAME_MAX    256       /* nome interfaccia "Ethernet 2" */
#define NET_DESC_MAX    256       /* descrizione adattatore */
#define NET_GUID_MAX    128       /* GUID adattatore "{...}" */

/* Numero massimo di route configurabili. */
#define CONFIG_MAX_ROUTES 256

/* Numero massimo di voci della tabella di routing snapshotted. */
#define ROUTE_SNAP_MAX 1024

/* Conteggio massimo di interfacce visibili. */
#define NET_MAX_IFACES 32

/* ------------------------------------------------------------ logging debug
 * Log diagnostico minimale, attivo SOLO con -DNRM_DEBUG in compilazione.
 * Nella build Release (senza il flag) le chiamate dbg() sono no-op senza
 * alcun costo: nessun I/O su disco. Non introduce polling: vengono loggati
 * solo gli eventi che il programma gestisce comunque. */
#ifdef NRM_DEBUG
#include <stdarg.h>
static inline void dbg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char p[MAX_PATH];
    const char *t = getenv("TEMP");
    snprintf(p, sizeof p, "%s\\nrm_debug.log", (t && *t) ? t : ".");
    FILE *f = fopen(p, "a");
    if (f) {
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fclose(f);
    }
    va_end(ap);
}
#else
#define dbg(...) ((void)0)
#endif

#endif /* COMMON_H */