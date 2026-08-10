/*
 * config.c - implementazione della persistenza.
 *
 * Formato minimale (JSON essenziale, senza librerie esterne):
 *
 *   {
 *     "routes": [
 *       { "ip": "37.244.28.101", "interface": "Ethernet 2", "guid": "{...}" }
 *     ]
 *   }
 */
#include "config.h"

BOOL cfg_default_path(char *out, size_t n)
{
    const char *p = getenv("LOCALAPPDATA");
    if (!p || !*p)
        p = getenv("APPDATA");
    if (!p || !*p) {
        snprintf(out, n, "config.json");
        return FALSE;
    }
    snprintf(out, n, "%s\\NetworkRouteManager\\config.json", p);
    return TRUE;
}

/* ------------------------------------------------------------ mini-JSON */

/* Cerca la chiave `key` all'interno del buffer [beg,end) e copia il valore
 * stringa in out[n]. Ritorna TRUE se trovata. */
static BOOL js_field(const char *beg, const char *end, const char *key,
                     char *out, size_t n)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *q = beg;
    while (q < end) {
        const char *p = strstr(q, pat);
        if (!p || p + strlen(pat) >= end)
            return FALSE;
        const char *c = p + strlen(pat);
        while (c < end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r'))
            c++;
        if (c >= end || *c != ':')
            return FALSE;
        c++; /* ':' */
        while (c < end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r'))
            c++;
        if (c >= end || *c != '"')
            return FALSE;
        c++; /* apostrofo iniziale */
        size_t k = 0;
        while (c < end && *c != '"' && k + 1 < n)
            out[k++] = *c++;
        out[k] = '\0';
        return TRUE;
    }
    return FALSE;
}

static const char *obj_close(const char *beg, const char *end)
{
    for (const char *p = beg; p < end; p++)
        if (*p == '}')
            return p;
    return NULL;
}

void cfg_load(Config *c, const char *path)
{
    memset(c, 0, sizeof(*c));
    if (path)
        snprintf(c->path, sizeof(c->path), "%s", path);

    FILE *f = fopen(c->path, "rb");
    if (!f)
        return;                              /* assente -> config vuota */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (16L * 1024 * 1024)) {
        fclose(f);
        return;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    const char *pos = buf;
    const char *end = buf + rd;
    while (pos < end) {
        const char *open = strchr(pos, '{');
        if (!open)
            break;
        const char *close = obj_close(open + 1, end);
        if (!close)
            break;

        char ip[NET_IP_MAX], name[NET_NAME_MAX], guid[NET_GUID_MAX];
        ip[0] = name[0] = guid[0] = '\0';
        BOOL has_ip = js_field(open + 1, close, "ip", ip, sizeof(ip));
        js_field(open + 1, close, "interface", name, sizeof(name));
        js_field(open + 1, close, "guid", guid, sizeof(guid));

        if (has_ip && ip[0])
            cfg_add(c, ip, name, guid);

        pos = close + 1;
    }

    free(buf);
}

BOOL cfg_save(const Config *c)
{
    if (!c->path[0])
        return FALSE;

    /* Crea la directory se assente. */
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", c->path);
    char *slash = strrchr(dir, '\\');
    if (slash)
        *slash = '\0';
    if (dir[0]) {
        CreateDirectoryA(dir, NULL);
        /* Directory annidate: crea ogni componente mancante. */
        for (char *p = dir + 1; *p; p++)
            if (*p == '\\') {
                *p = '\0';
                CreateDirectoryA(dir, NULL);
                *p = '\\';
            }
        /* 'N:' -> directory mendante; ok */
        CreateDirectoryA(dir, NULL);
    }

    FILE *f = fopen(c->path, "w");
    if (!f)
        return FALSE;

    fprintf(f, "{\n  \"routes\": [\n");
    for (int i = 0; i < c->count; i++) {
        const RouteConfigItem *it = &c->items[i];
        fprintf(f, "    { \"ip\": \"%s\", \"interface\": \"%s\", \"guid\": \"%s\" }%s\n",
                it->ip, it->name, it->guid,
                (i + 1 < c->count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return TRUE;
}

BOOL cfg_add(Config *c, const char *ip, const char *name, const char *guid)
{
    if (c->count >= CONFIG_MAX_ROUTES)
        return FALSE;
    if (cfg_find(c, ip) >= 0)
        return FALSE;                  /* duplicato */

    RouteConfigItem *it = &c->items[c->count];
    snprintf(it->ip,   sizeof(it->ip),   "%s", ip);
    snprintf(it->name, sizeof(it->name), "%s", name ? name : "");
    snprintf(it->guid, sizeof(it->guid), "%s", guid ? guid : "");
    c->count++;
    return TRUE;
}

BOOL cfg_update(Config *c, const char *ip, const char *name, const char *guid)
{
    int i = cfg_find(c, ip);
    if (i < 0)
        return cfg_add(c, ip, name, guid);
    snprintf(c->items[i].name, sizeof(c->items[i].name), "%s", name ? name : "");
    snprintf(c->items[i].guid, sizeof(c->items[i].guid), "%s", guid ? guid : "");
    return TRUE;
}

void cfg_remove(Config *c, const char *ip)
{
    int i = cfg_find(c, ip);
    if (i < 0)
        return;
    for (int j = i; j + 1 < c->count; j++)
        c->items[j] = c->items[j + 1];
    c->count--;
}

int cfg_find(const Config *c, const char *ip)
{
    for (int i = 0; i < c->count; i++)
        if (strcmp(c->items[i].ip, ip) == 0)
            return i;
    return -1;
}