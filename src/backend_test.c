/*
 * backend_test.c - verifica console del backend (senza GUI).
 *
 * Utilita' di sviluppo: enumera interfacce e route usando le stesse funzioni
 * usate dalla GUI, utile per validare i dati restituiti dalle API Windows.
 *
 * Compilazione rapida (MinGW):
 *   gcc -O2 -o backend_test.exe backend_test.c network.c routes.c config.c -liphlpapi
 *
 * Uso:
 *   backend_test.exe                -> elenco interfacce + route IPv4
 *   backend_test.exe routes         -> solo tabella di routing
 *   backend_test.exe config <file>  -> mostra le regole salvate in <file>
 */
#include "network.h"
#include "routes.h"
#include "config.h"

static void print_interfaces(const NetList *n)
{
    printf("NETWORK INTERFACES\n");
    printf("------------------\n");
    for (int i = 0; i < n->count; i++) {
        const NetInterface *ni = &n->items[i];
        printf("[%s] %s\n",
               ni->state == NET_CONNECTED ? "CONNECTED" : "DISCONNECTED",
               ni->friendly_name);
        if (ni->friendly_name[0])
            printf("    Name:     %s\n", ni->friendly_name);
        if (ni->guid[0])
            printf("    GUID:     %s\n", ni->guid);
        printf("    ifIndex:  %lu\n", ni->ifindex);
        printf("    IPv4:     %s\n", ni->ipv4[0] ? ni->ipv4 : "-");
        printf("    Gateway:  %s\n", ni->gateway[0] ? ni->gateway : "-");
        printf("    Metric:   %lu\n", ni->metric);
        if (ni->state == NET_CONNECTED && ni->has_default_route)
            printf("    Default:  YES\n");
        if (ni->description[0])
            printf("    HW:       %s\n", ni->description);
        printf("\n");
    }
}

static void print_routes(const RouteList *r)
{
    printf("IPv4 ROUTES\n");
    printf("-----------\n");
    printf("%-18s %-5s %-10s %-16s %s\n",
           "DEST", "MASK", "IF", "GATEWAY", "METRIC");
    for (int i = 0; i < r->count; i++) {
        const HostRoute *hr = &r->items[i];
        if (hr->prefix_len == 0) {
            printf("%-18s %-5s %-10lu %-16s %lu\n",
                   "0.0.0.0/0", "0", hr->ifindex,
                   hr->gateway[0] ? hr->gateway : "-", hr->metric);
        } else if (hr->prefix_len == 32) {
            printf("%-18s %-5s %-10lu %-16s %lu\n",
                   hr->ip, "32", hr->ifindex,
                   hr->gateway[0] ? hr->gateway : "-", hr->metric);
        } else {
            printf("%-18s %-5s %-10lu %-16s %lu\n",
                   hr->ip, "x", hr->ifindex,
                   hr->gateway[0] ? hr->gateway : "-", hr->metric);
        }
    }
}

static void print_config(const Config *cfg)
{
    printf("CONFIG (%s) - %d routes\n", cfg->path, cfg->count);
    printf("-----------------------\n");
    for (int i = 0; i < cfg->count; i++) {
        const RouteConfigItem *it = &cfg->items[i];
        printf("%-15s -> %-20s  [%s]\n", it->ip, it->name, it->guid);
    }
}

/* Test veloce della validazione IPv4. */
static void test_ipv4_validation(void)
{
    const char *cases[] = {
        "137.221.105.232", "8.8.8.8", "192.168.1.1",
        "999.999.1.1", "abc.def.ghi.jkl", "1.2.3.256", "1.2.3",
        "1.2.3.4.5", "", NULL
    };
    printf("IP VALIDATION CHECK\n");
    printf("-------------------\n");
    for (int i = 0; cases[i]; i++)
        printf("  %-16s -> %s\n", cases[i],
               net_valid_ipv4(cases[i]) ? "OK" : "INVALID");
    printf("\n");
}

int main(int argc, char **argv)
{
    NetList nets;

    net_snapshot(&nets);

    if (argc > 1 && strcmp(argv[1], "config") == 0) {
        Config cfg;
        cfg_load(&cfg, argv[2] ? argv[2] : "config.json");
        print_config(&cfg);
        return 0;
    }

    /* Determina le default route per la stampa. */
    RouteList routes;
    routes_snapshot(&routes);
    unsigned long def_if = routes_default_ifindex(&routes);
    for (int i = 0; i < nets.count; i++)
        nets.items[i].has_default_route = (nets.items[i].ifindex == def_if);

    test_ipv4_validation();
    print_interfaces(&nets);

    if (argc > 1 && strcmp(argv[1], "routes") == 0)
        print_routes(&routes);

    printf("Default route via ifIndex %lu\n", def_if);
    return 0;
}