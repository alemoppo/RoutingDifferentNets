/*
 * config.h - persistenza della configurazione (regole IP -> interfaccia).
 *
 * Per ogni regola salviamo:
 *   ip          : indirizzo IPv4 destinazione (host /32)
 *   interface   : nome Windows dell'interfaccia (solo per la visualizzazione)
 *   guid        : GUID stabile dell'adattatore (identita' persistente)
 *
 * NON salviamo ifIndex / IPv4 locali / gateway: vengono risolti al volo.
 * Formato del file: JSON minimale, scritto solo da questo modulo.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

typedef struct {
    char           ip[NET_IP_MAX];   /* destinazione                       */
    char           name[NET_NAME_MAX]; /* nome interfaccia per la GUI      */
    char           guid[NET_GUID_MAX]; /* identita' stabile dell'adattatore */
    /* Ultimi parametri noti con cui la route /32 fu creata (persistente):
     * se l'interfaccia e' assente al momento della rimozione servono per
     * cancellare SOLO la nostra voce e non quelle di terze parti. */
    char           last_gateway[NET_IP_MAX];
    unsigned long  last_ifindex;
} RouteConfigItem;

typedef struct {
    RouteConfigItem items[CONFIG_MAX_ROUTES];
    int             count;
    char            path[MAX_PATH];
} Config;

/* Percorso predefinito: %LOCALAPPDATA%\NetworkRouteManager\config.json */
BOOL cfg_default_path(char *out, size_t n);

/* Carica il file (se assente/corrotto: configurazione vuota, nessun errore).
 * Il percorso viene memorizzato in cfg->path per il salvataggio. */
void cfg_load(Config *c, const char *path);

/* Salva sul file configurato; crea la directory se mancante. */
BOOL cfg_save(const Config *c);

/* Aggiunge una regola (nessun duplicato di IP). Ritorna FALSE se duplicata. */
BOOL cfg_add(Config *c, const char *ip, const char *name, const char *guid);

/* In caso di modifica dell'interfaccia aggiorna la regola esistente. */
BOOL cfg_update(Config *c, const char *ip, const char *name, const char *guid);

/* Aggiorna gli ultimi parametri noti della route per quella regola.
 * Ritorna TRUE se il valore e' cambiato (per decidere se salvare). */
BOOL cfg_set_last(Config *c, const char *ip, const char *gateway,
                  unsigned long ifindex);

/* Ritorna TRUE se la regola ha parametri noti per una cancellazione esatta. */
BOOL cfg_last_known(const Config *c, const char *ip, char *gateway, size_t n,
                    unsigned long *ifindex);

/* Rimuove la regola con quell'IP (esistente o meno). */
void cfg_remove(Config *c, const char *ip);

/* Indice della regola con quell'IP, oppure -1. */
int cfg_find(const Config *c, const char *ip);

#endif /* CONFIG_H */