/*
 * network.c - implementazione dell'enumerazione delle interfacce IPv4.
 *
 * API usate:
 *   - GetAdaptersAddresses(AF_INET, ...)  -> elenco adattatori + IPv4 + gateway
 *   - GetIpInterfaceEntry(...)            -> metrica dell'interfaccia
 *
 * NOTA: L'ifIndex NON e' un identificatore stabile e cambia tra riavvii;
 * l'identita' persistente e' il GUID dell'adattatore (pa->AdapterName).
 */
#include "network.h"

/* Converte una stringa wide (UTF-16) in UTF-8 con troncatura sicura:
 * mai a meta' di un carattere multibyte. */
static void wstr_to_utf8(const WCHAR *in, char *out, size_t n)
{
    if (!out || n == 0)
        return;
    out[0] = '\0';
    if (!in || !*in)
        return;

    int need = WideCharToMultiByte(CP_UTF8, 0, in, -1, NULL, 0, NULL, NULL);
    if (need <= 0)
        return;

    if ((size_t)need <= n) {
        WideCharToMultiByte(CP_UTF8, 0, in, -1, out, need, NULL, NULL);
        return;
    }

    /* Troppo lunga: convertiamo in un buffer temporaneo e poi copiamo
     * troncata; se l'ultimo carattere e' multibyte arretriamo al confine
     * precedente per non produrre UTF-8 non valido. */
    char *tmp = (char *)malloc((size_t)need);
    if (!tmp)
        return;
    if (WideCharToMultiByte(CP_UTF8, 0, in, -1, tmp, need, NULL, NULL) > 0) {
        size_t k = n - 1;
        while (k > 0 && ((unsigned char)tmp[k] & 0xC0) == 0x80)
            k--;
        memcpy(out, tmp, k);
        out[k] = '\0';
    }
    free(tmp);
}

/* Ordinamento: interfacce connesse per prime, poi per nome, poi ifIndex. */
static BOOL net_less(const NetInterface *a, const NetInterface *b)
{
    if (a->state != b->state)
        return a->state > b->state;
    int c = strcmp(a->friendly_name, b->friendly_name);
    if (c)
        return c < 0;
    return a->ifindex < b->ifindex;
}

static void net_sort(NetList *out)
{
    for (int i = 1; i < out->count; i++) {
        NetInterface tmp = out->items[i];
        int j = i - 1;
        while (j >= 0 && net_less(&tmp, &out->items[j])) {
            out->items[j + 1] = out->items[j];
            j--;
        }
        out->items[j + 1] = tmp;
    }
}

void net_snapshot(NetList *out)
{
    out->count = 0;

    /* Primo tentativo con dimensione ragionevole; GetAdaptersAddresses
     * segnala ERROR_BUFFER_OVERFLOW se il buffer e' troppo piccolo. */
    ULONG sz = 16 * 1024;
    IP_ADAPTER_ADDRESSES *buf = (IP_ADAPTER_ADDRESSES *)malloc(sz);
    if (!buf)
        return;

    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS,
                                    NULL, buf, &sz);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        free(buf);
        buf = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (!buf)
            return;
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS,
                                  NULL, buf, &sz);
    }
    if (rc != ERROR_SUCCESS) {
        free(buf);
        return;
    }

    for (IP_ADAPTER_ADDRESSES *pa = buf; pa; pa = pa->Next) {
        if (out->count >= NET_MAX_IFACES)
            break;

        /* Il loopback (127.0.0.1) non e' una rete da gestire. */
        if (pa->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        NetInterface *ni = &out->items[out->count];
        memset(ni, 0, sizeof(*ni));

        wstr_to_utf8(pa->FriendlyName, ni->friendly_name, sizeof(ni->friendly_name));
        wstr_to_utf8(pa->Description,  ni->description,  sizeof(ni->description));

        if (pa->AdapterName && *pa->AdapterName)
            snprintf(ni->guid, sizeof(ni->guid), "%s", pa->AdapterName);

        ni->ifindex = (unsigned long)pa->IfIndex;
        ni->state   = (pa->OperStatus == IfOperStatusUp) ? NET_CONNECTED
                                                         : NET_DISCONNECTED;

        /* Primo indirizzo unicast IPv4. */
        if (pa->FirstUnicastAddress) {
            struct sockaddr_in *sa = (struct sockaddr_in *)pa->FirstUnicastAddress->Address.lpSockaddr;
            if (sa && sa->sin_family == AF_INET)
                InetNtopA(AF_INET, &sa->sin_addr, ni->ipv4, sizeof(ni->ipv4));
        }

        /* Primo gateway IPv4. */
        if (pa->FirstGatewayAddress) {
            struct sockaddr_in *sa = (struct sockaddr_in *)pa->FirstGatewayAddress->Address.lpSockaddr;
            if (sa && sa->sin_family == AF_INET)
                InetNtopA(AF_INET, &sa->sin_addr, ni->gateway, sizeof(ni->gateway));
        }

        /* Metrica dell'interfaccia (GetIpInterfaceEntry). */
        MIB_IPINTERFACE_ROW row;
        memset(&row, 0, sizeof(row));
        row.Family        = AF_INET;
        row.InterfaceIndex = ni->ifindex;
        if (GetIpInterfaceEntry(&row) == NO_ERROR)
            ni->metric = row.Metric;

        out->count++;
    }

    free(buf);
    net_sort(out);
}

const NetInterface *net_find_by_guid(const NetList *l, const char *guid)
{
    if (!guid || !*guid)
        return NULL;
    for (int i = 0; i < l->count; i++)
        if (l->items[i].guid[0] && _stricmp(l->items[i].guid, guid) == 0)
            return &l->items[i];
    return NULL;
}

const NetInterface *net_find_by_name(const NetList *l, const char *name)
{
    if (!name || !*name)
        return NULL;
    for (int i = 0; i < l->count; i++)
        if (l->items[i].friendly_name[0] &&
            _stricmp(l->items[i].friendly_name, name) == 0)
            return &l->items[i];
    return NULL;
}

const NetInterface *net_resolve(const NetList *l, const char *guid, const char *name)
{
    const NetInterface *p = net_find_by_guid(l, guid);
    if (!p)
        p = net_find_by_name(l, name);
    return p;
}

int net_index_of(const NetList *l, const NetInterface *p)
{
    if (!p)
        return -1;
    return (int)(p - l->items);
}

