/*
 * monitor.h - rilevatore dei cambiamenti di rete (event-driven, zero polling).
 *
 * Usa le notifiche native Windows:
 *   - NotifyIpInterfaceChange          (stato interfaccia / metriche)
 *   - NotifyUnicastIpAddressChange     (indirizzi IPv4)
 *   - NotifyRouteChange2               (tabella di routing)
 *
 * Un singolo thread molto leggero si blocca in WaitForMultipleObjects
 * (consumo CPU ~0%): quando arriva una notifica viene invocata la callback.
 */
#ifndef MONITOR_H
#define MONITOR_H

#include <stddef.h>

typedef void (*net_change_cb)(void *user);

typedef struct NetMon NetMon;

/* Avvia il monitor. La callback viene chiamata su un thread dedicato. */
NetMon *monitor_start(net_change_cb cb, void *user);

/* Ferma il monitor e rilascia le risorse. */
void monitor_stop(NetMon *m);

#endif /* MONITOR_H */