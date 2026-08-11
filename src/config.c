/*
 * config.c - implementazione della persistenza.
 *
 * Formato minimale (JSON essenziale, senza librerie esterne):
 *
 *   {
 *     "routes": [
 *       { "ip": "37.244.28.101", "interface": "Ethernet 2", "guid": "{...}",
 *         "last_gateway": "192.168.42.129", "last_ifindex": 11 }
 *     ]
 *   }
 */
#include "config.h"
#include "routes.h"   /* net_valid_ipv4: valida gli IP letti dal file */

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

/* Scrive in out[n] la versione JSON-escapata di `s`. Lega i caratteri
 * speciali e i control-char (<0x20) come \u00XX. Ritorna il numero di byte
 * scritti (escluso '\0'), o -1 se il buffer non e' sufficiente: in tal caso
 * out[] contiene solo il terminatore e il chiamante NON deve proseguire. */
static int json_escape_string(char *out, size_t n, const char *s)
{
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const char *esc = NULL;
        switch (*p) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            default:
                if (*p < 0x20) {
                    if (k + 7 >= n) return -1;   /* \u00XX + '\0' */
                    k += (size_t)snprintf((char *)out + k, n - k,
                                          "\\u%04X", *p);
                } else {
                    if (k + 1 >= n) return -1;
                    out[k++] = (char)*p;
                }
                continue;
        }
        size_t el = strlen(esc);
        if (k + el + 1 > n)
            return -1;
        memcpy(out + k, esc, el);
        k += el;
    }
    if (k + 1 > n)
        return -1;
    out[k] = '\0';
    return (int)k;
}

/* Decodifica un'escape JSON (\", \\, \n, \r, \t, \b, \f, \uXXXX) nella
 * posizione [c, end). Ritorna il carattere e avanzare c di conseguenza,
 * oppure FALSE se l'escape e' malformata. */
static BOOL js_unescape_one(const char **c, const char *end, unsigned char *out)
{
    if (*c >= end)
        return FALSE;
    unsigned char ch = (unsigned char)*(*c)++;
    switch (ch) {
        case '"':  *out = '"';  return TRUE;
        case '\\': *out = '\\'; return TRUE;
        case 'n':  *out = '\n'; return TRUE;
        case 'r':  *out = '\r'; return TRUE;
        case 't':  *out = '\t'; return TRUE;
        case 'b':  *out = '\b'; return TRUE;
        case 'f':  *out = '\f'; return TRUE;
        case 'u': {
            unsigned v = 0;
            for (int i = 0; i < 4; i++) {
                if (*c >= end)
                    return FALSE;
                char hex = (char)*(*c)++;
                v <<= 4;
                if (hex >= '0' && hex <= '9')      v |= (unsigned)(hex - '0');
                else if (hex >= 'a' && hex <= 'f') v |= (unsigned)(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') v |= (unsigned)(hex - 'A' + 10);
                else return FALSE;
            }
            if (v > 0xFF)      /* questi campi sono solo ASCII/UTF-8 a 1 byte */
                return FALSE;
            *out = (unsigned char)v;
            return TRUE;
        }
        default:
            return FALSE;      /* escape sconosciuta -> JSON malformato */
    }
}

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
            return FALSE;   /* stringa senza virgoletta di chiusura: malformata */
        c++; /* apostrofo iniziale */
        size_t k = 0;
        while (c < end && *c != '"') {
            if (k + 2 > n)
                return FALSE;   /* overflow: troncheremmo il valore */
            if (*c == '\\') {
                c++;
                unsigned char d = 0;
                if (!js_unescape_one(&c, end, &d))
                    return FALSE;
                out[k++] = (char)d;
            } else {
                out[k++] = *c++;
            }
        }
        if (c >= end || *c != '"')
            return FALSE;   /* valore non terminato -> JSON corrotto */
        out[k] = '\0';
        return TRUE;
    }
    return FALSE;
}

static const char *obj_close(const char *beg, const char *end)
{
    /* `beg` e' il primo carattere DOPO una '{': profondita' iniziale 1.
     * Le stringhe vengono saltate: le '{'/'}' dentro un valore stringa
     * (es. un GUID "{...}") NON contribuiscono al bilanciamento. */
    int depth = 1;
    for (const char *p = beg; p < end; p++) {
        if (*p == '"') {
            for (p++; p < end; p++) {
                if (*p == '\\') {
                    p++;
                } else if (*p == '"') {
                    break;
                }
            }
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}' && --depth == 0) {
            return p;
        }
    }
    return NULL;
}

/* Cerca la chiave `key` e parsa un intero decimale non negativo. */
static BOOL js_field_num(const char *beg, const char *end, const char *key,
                         unsigned long *out)
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
        c++;
        while (c < end && (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r'))
            c++;
        unsigned long v = 0;
        int digits = 0;
        while (c < end && *c >= '0' && *c <= '9') {
            v = v * 10 + (unsigned long)(*c - '0');
            c++;
            digits++;
        }
        if (digits == 0)
            return FALSE;
        *out = v;
        return TRUE;
    }
    return FALSE;
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

    /* Trova l'oggetto radice (bilanciato: la GUID annidata non lo spezza),
     * poi scandisce i singoli oggetti-voce contenuti. */
    const char *root_open = strchr(pos, '{');
    const char *root_close = root_open ? obj_close(root_open + 1, end) : NULL;
    if (root_open && root_close) {
        const char *item = root_open + 1;
        while (item < root_close) {
            const char *open = strchr(item, '{');
            if (!open || open >= root_close)
                break;
            const char *close = obj_close(open + 1, root_close);
            if (!close)
                break;

            char ip[NET_IP_MAX], name[NET_NAME_MAX], guid[NET_GUID_MAX];
            char gw[NET_IP_MAX];
            unsigned long ifidx = 0;
            ip[0] = name[0] = guid[0] = gw[0] = '\0';
            BOOL has_ip = js_field(open + 1, close, "ip", ip, sizeof(ip));
            js_field(open + 1, close, "interface", name, sizeof(name));
            js_field(open + 1, close, "guid", guid, sizeof(guid));
            js_field(open + 1, close, "last_gateway", gw, sizeof(gw));
            js_field_num(open + 1, close, "last_ifindex", &ifidx);

            /* Only destinazioni IPv4 valide. `last_*` sono opzionali e
             * servono solo a una cancellazione esatta quando l'interfaccia
             * e' assente; accettati solo se coerenti. */
            if (has_ip && ip[0] && net_valid_ipv4(ip)) {
                cfg_add(c, ip, name, guid);
                if (gw[0] && net_valid_ipv4(gw) && ifidx != 0)
                    cfg_set_last(c, ip, gw, ifidx);
            }

            item = close + 1;
        }
    }

    free(buf);
}

BOOL cfg_save(const Config *c)
{
    if (!c->path[0])
        return FALSE;

    /* Crea la directory se assente (solo se il path contiene una cartella). */
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", c->path);
    char *slash = strrchr(dir, '\\');
    if (slash) {
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
    }

    /* Serializza in un buffer in memoria (mai direttamente sul file). Ogni
     * stringa viene JSON-escapata al volo (max 6 byte per char: \u00XX), quindi
     * dimensioniamo per il caso peggiore e verifichiamo di nuovo per sicurezza.
     */
    size_t cap = 256;
    for (int i = 0; i < c->count; i++) {
        const RouteConfigItem *it = &c->items[i];
        cap += (size_t)strlen(it->ip)     * 6 + 1;
        cap += (size_t)strlen(it->name)   * 6 + 1;
        cap += (size_t)strlen(it->guid)   * 6 + 1;
        cap += (size_t)strlen(it->last_gateway) * 6 + 1;
        cap += 80;
    }
    char *buf = (char *)malloc(cap);
    if (!buf)
        return FALSE;

    /* Buffer temporanei per l'escaping di ogni campo stringa. */
    char eip[NET_IP_MAX * 6 + 4], ename[NET_NAME_MAX * 6 + 4];
    char eguid[NET_GUID_MAX * 6 + 4], egw[NET_IP_MAX * 6 + 4];

    size_t p = 0;
    int r = snprintf(buf + p, cap - p, "{\n  \"routes\": [\n");
    if (r < 0 || (size_t)r >= cap - p) { free(buf); return FALSE; }
    p += (size_t)r;

    for (int i = 0; i < c->count; i++) {
        const RouteConfigItem *it = &c->items[i];
        if (json_escape_string(eip,   sizeof(eip),   it->ip)          < 0 ||
            json_escape_string(ename, sizeof(ename), it->name)        < 0 ||
            json_escape_string(eguid, sizeof(eguid), it->guid)        < 0 ||
            json_escape_string(egw,   sizeof(egw),   it->last_gateway) < 0) {
            free(buf);
            return FALSE;
        }
        if (it->last_gateway[0] && it->last_ifindex != 0)
            r = snprintf(buf + p, cap - p,
                 "    { \"ip\": \"%s\", \"interface\": \"%s\", \"guid\": \"%s\","
                 " \"last_gateway\": \"%s\", \"last_ifindex\": %lu }%s\n",
                 eip, ename, eguid, egw,
                 it->last_ifindex, (i + 1 < c->count) ? "," : "");
        else
            r = snprintf(buf + p, cap - p,
                 "    { \"ip\": \"%s\", \"interface\": \"%s\", \"guid\": \"%s\" }%s\n",
                 eip, ename, eguid,
                 (i + 1 < c->count) ? "," : "");
        if (r < 0 || (size_t)r >= cap - p) { free(buf); return FALSE; }
        p += (size_t)r;
    }

    r = snprintf(buf + p, cap - p, "  ]\n}\n");
    if (r < 0 || (size_t)r >= cap - p) { free(buf); return FALSE; }
    p += (size_t)r;
    size_t len = p;

    /* Scrittura atomica: config.json.tmp -> flush -> MoveFileExW.
     * Se un qualsiasi passo fallisce il vecchio config.json resta intatto. */
    wchar_t wpath[MAX_PATH + 16], wtmp[MAX_PATH + 16];
    int wlen = MultiByteToWideChar(CP_ACP, 0, c->path, -1, wpath,
                                   (int)(sizeof(wpath) / sizeof(wpath[0])));
    if (wlen <= 0) {
        dbg("[CFG] MultiByteToWideChar fallito (err=%lu)", GetLastError());
        free(buf);
        return FALSE;
    }
    if (swprintf(wtmp, MAX_PATH + 16, L"%s.tmp", wpath) < 0) {
        free(buf);
        return FALSE;
    }

    HANDLE h = CreateFileW(wtmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(buf);
        return FALSE;
    }

    DWORD wrote = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)len, &wrote, NULL) &&
              wrote == (DWORD)len && FlushFileBuffers(h);
    CloseHandle(h);

    if (!ok || !MoveFileExW(wtmp, wpath,
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wtmp);
        free(buf);
        return FALSE;
    }

    free(buf);
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

BOOL cfg_set_last(Config *c, const char *ip, const char *gateway,
                  unsigned long ifindex)
{
    int i = cfg_find(c, ip);
    if (i < 0)
        return FALSE;
    if (c->items[i].last_ifindex == ifindex &&
        strcmp(c->items[i].last_gateway, gateway ? gateway : "") == 0)
        return FALSE;   /* invariato: nessun salvataggio necessario */
    snprintf(c->items[i].last_gateway, sizeof(c->items[i].last_gateway), "%s",
             gateway ? gateway : "");
    c->items[i].last_ifindex = ifindex;
    return TRUE;
}

BOOL cfg_last_known(const Config *c, const char *ip, char *gateway, size_t n,
                    unsigned long *ifindex)
{
    int i = cfg_find(c, ip);
    if (i < 0)
        return FALSE;
    if (!c->items[i].last_gateway[0] || c->items[i].last_ifindex == 0)
        return FALSE;
    snprintf(gateway, n, "%s", c->items[i].last_gateway);
    *ifindex = c->items[i].last_ifindex;
    return TRUE;
}

int cfg_find(const Config *c, const char *ip)
{
    for (int i = 0; i < c->count; i++)
        if (strcmp(c->items[i].ip, ip) == 0)
            return i;
    return -1;
}