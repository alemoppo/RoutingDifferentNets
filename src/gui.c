/*
 * gui.c - interfaccia SDL3 minimale.
 * Testo: Segoe UI di sistema renderizzata con SDL_ttf (anti-alias, no pixel
 * font). Backbuffer software ARGB8888 presentato SOLO sui cambiamenti
 * (rete / config / input): a riposo thread bloccato: ~0% CPU.
 */
#include "gui.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "common.h"
#include "network.h"
#include "routes.h"
#include "monitor.h"
#include "proc.h"

/* font di sistema caricati all'avvio (scelta: Segoe UI, fallback Arial) */
static TTF_Font *g_f_title;   /* 22px semibold - intestazioni              */
static TTF_Font *g_f_norm;    /* 17px      - testo principale (scale 2)    */
static TTF_Font *g_f_sm;      /* 12px      - testo compatto (scale 1)      */
static int g_ht;              /* altezza riga del font titolo              */
static int g_h2;              /* altezza riga del font principale          */
static int g_h1;              /* altezza riga del font compatto            */

/* Caratteri ammessi nella casella del dialogo: cifre/punti per un IP,
 * oppure caratteri di nome processo (alnum, '-', '_', '.', spazio). */
static int dlg_ok_char(char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z'))
        return 1;
    if (c == '.' || c == '-' || c == '_' || c == ' ')
        return 1;
    return 0;
}

typedef enum {
    CMD_NONE = 0,
    CMD_ADD,
    CMD_REFRESH,
    CMD_APPLY,
    CMD_REMOVE,
    CMD_EDIT,
    CMD_DLG_ADD,
    CMD_DLG_CANCEL,
    CMD_DLG_DROP,
    CMD_DLG_NET
} CmdId;

typedef struct { SDL_Rect r; CmdId cmd; int idx; } Hit;
#define MAX_HITS 512

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Surface  *surf;
    SDL_Texture  *canvas;
    int           w, h;
    int           redraw;
    int           need_recon;
    int           mouse_x, mouse_y;
    int           hover_hit;      /* hit sotto il cursore all'ultimo frame */
    int           scroll, rscroll, left_maxy, right_x;

    Config       *cfg;
    NetList       nets;
    RouteList     routes;
    RouteStatus   st[CONFIG_MAX_ROUTES];
    int           iface_of[CONFIG_MAX_ROUTES];
    char          actual_if[CONFIG_MAX_ROUTES][NET_NAME_MAX];
    char          reason[CONFIG_MAX_ROUTES][NET_NAME_MAX];
    char          summary[768];

    Hit           hits[MAX_HITS];
    int           nhits;

    BOOL          running;

    BOOL          dlg;
    int           dlg_edit;
    char          dlg_ip[NET_IP_MAX];
    BOOL          dlg_drop;
    int           dlg_net;
    char          dlg_msg[160];

    char          opmsg[200];
} App;

/* colori (0xAARRGGBB) */
#define C_BG      0xFF10141B
#define C_PANEL   0xFF161C25
#define C_PANEL2  0xFF1B2230
#define C_BORDER  0xFF26303D
#define C_TEXT    0xFFD9E0E8
#define C_DIM     0xFF8B94A1
#define C_BTN     0xFF293544
#define C_BTN_HI  0xFF37475E
#define C_ACCENT  0xFF5C9CE6
#define C_GREEN   0xFF57C878
#define C_RED     0xFFE4636A
#define C_AMBER   0xFFE0A955
#define C_GRAY    0xFF6A7480
#define C_WHITE   0xFFFFFFFF
#define C_INPUT   0xFF0C1015

#define TOP 46
#define BOTTOM 30
#define MARGIN 12
#define LIN 19

/* logo + sottotitolo: bordo inferiore dell'intestazione (blocco alto) */
#define HEADER_TOP 12
#define HEADER_BOT (HEADER_TOP + g_ht + 7 + g_h1 + 6)

static inline Uint32 bl(Uint32 dst, Uint32 src)
{
    unsigned sa = (src >> 24) & 0xFF, sr = (src >> 16) & 0xFF,
             sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    unsigned da = (dst >> 24) & 0xFF, dr = (dst >> 16) & 0xFF,
             dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    if (sa == 0xFF) return src;
    unsigned oa = sa + da * (255 - sa) / 255;
    if (!oa) return 0;
    return (oa << 24) |
           (((sr * sa + dr * da * (255 - sa) / 255) / oa) << 16) |
           (((sg * sa + dg * da * (255 - sa) / 255) / oa) << 8) |
           ((sb * sa + db * da * (255 - sa) / 255) / oa);
}

static void px(App *a, int x, int y, Uint32 col)
{
    if (x < 0 || y < 0 || x >= a->w || y >= a->h) return;
    Uint32 *p = (Uint32 *)((Uint8 *)a->surf->pixels + (size_t)y * a->surf->pitch
                           + (size_t)x * 4);
    *p = bl(*p, col);
}

static void fill_rect(App *a, int x, int y, int w, int h, Uint32 col)
{
    if (w <= 0 || h <= 0) return;

    /* Colore opaco: riempimento diretto, senza blend per-pixel. */
    if ((col >> 24) == 0xFF) {
        int x0 = x < 0 ? 0 : x;
        int y0 = y < 0 ? 0 : y;
        int x1 = x + w, y1 = y + h;
        if (x1 > a->w) x1 = a->w;
        if (y1 > a->h) y1 = a->h;
        for (int yy = y0; yy < y1; yy++) {
            Uint32 *row = (Uint32 *)((Uint8 *)a->surf->pixels +
                                     (size_t)yy * a->surf->pitch);
            for (int xx = x0; xx < x1; xx++)
                row[xx] = col;
        }
        return;
    }

    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            px(a, xx, yy, col);
}

static void frame_rect(App *a, int x, int y, int w, int h, Uint32 col)
{
    fill_rect(a, x, y, w, 1, col);
    fill_rect(a, x, y + h - 1, w, 1, col);
    fill_rect(a, x, y, 1, h, col);
    fill_rect(a, x + w - 1, y, 1, h, col);
}

static TTF_Font *font_for(int scale)
{
    return scale >= 2 ? g_f_norm : g_f_sm;
}

static int text_w(const char *s, int scale)
{
    int w = 0, h = 0;
    TTF_GetStringSize(font_for(scale), s, 0, &w, &h);
    return w;
}

static int line_h(int scale)
{
    return scale >= 2 ? g_h2 : g_h1;
}

static BOOL ui_fonts_init(void)
{
    if (!TTF_Init())
        return FALSE;

    const char *segoe[][2] = {
        { "C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\arialbd.ttf" },
        { "C:\\Windows\\Fonts\\segoeui.ttf",  "C:\\Windows\\Fonts\\arial.ttf"   },
    };

    g_f_title = TTF_OpenFont(segoe[0][0], 22);
    if (!g_f_title) g_f_title = TTF_OpenFont(segoe[0][1], 22);
    g_f_norm   = TTF_OpenFont(segoe[1][0], 17);
    if (!g_f_norm) g_f_norm = TTF_OpenFont(segoe[1][1], 17);
    g_f_sm     = TTF_OpenFont(segoe[1][0], 12);
    if (!g_f_sm) g_f_sm = TTF_OpenFont(segoe[1][1], 12);

    if (!g_f_norm || !g_f_sm) {
        fprintf(stderr, "TTF_OpenFont fallito: %s\n", SDL_GetError());
        TTF_Quit();
        return FALSE;
    }

    g_h2 = TTF_GetFontHeight(g_f_norm);
    g_h1 = TTF_GetFontHeight(g_f_sm);
    g_ht = g_f_title ? TTF_GetFontHeight(g_f_title) : g_h2;
    return TRUE;
}

static void ui_fonts_quit(void)
{
    if (g_f_title) TTF_CloseFont(g_f_title);
    if (g_f_norm)   TTF_CloseFont(g_f_norm);
    if (g_f_sm)     TTF_CloseFont(g_f_sm);
    TTF_Quit();
}

static void draw_text(App *a, int x, int y, const char *s, Uint32 col,
                      int scale)
{
    SDL_Color c = { (Uint8)((col >> 16) & 0xFF), (Uint8)((col >> 8) & 0xFF),
                    (Uint8)(col & 0xFF), (Uint8)((col >> 24) & 0xFF) };
    SDL_Surface *ts = TTF_RenderText_Blended(font_for(scale), s, 0, c);
    if (!ts) return;
    SDL_Rect dst = { x, y, ts->w, ts->h };
    SDL_BlitSurface(ts, NULL, a->surf, &dst);
    SDL_DestroySurface(ts);
}

static void draw_title(App *a, int x, int y, const char *s)
{
    if (!g_f_title) { draw_text(a, x, y, s, C_TEXT, 2); return; }
    SDL_Color c = { 0xD9, 0xE0, 0xE8, 0xFF };
    SDL_Surface *ts = TTF_RenderText_Blended(g_f_title, s, 0, c);
    if (!ts) { draw_text(a, x, y, s, C_TEXT, 2); return; }
    SDL_Rect dst = { x, y, ts->w, ts->h };
    SDL_BlitSurface(ts, NULL, a->surf, &dst);
    SDL_DestroySurface(ts);
}

/* numero di BYTE UTF-8 (sempre su confine di carattere) che entrano
 * entro max_px, accumulando larghezze dei caratteri */
static int text_fit_bytes(TTF_Font *f, const char *s, int max_px)
{
    int w = 0, bytes = 0;
    for (const char *p = s; *p;) {
        int cl = (unsigned char)*p < 0x80 ? 1 :
                     ((unsigned char)*p & 0xF0) == 0xF0 ? 4 :
                     ((unsigned char)*p & 0xE0) == 0xE0 ? 3 : 2;
        char m[5];
        memcpy(m, p, cl);
        m[cl] = 0;
        int cw = 0, ch = 0;
        TTF_GetStringSize(f, m, 0, &cw, &ch);
        if (w + cw > max_px)
            break;
        w += cw;
        p += cl;
        bytes += cl;
    }
    return bytes;
}

static void draw_text_cut(App *a, int x, int y, const char *s, Uint32 col,
                          int scale, int max_px)
{
    TTF_Font *f = font_for(scale);
    int w = 0, h = 0;
    TTF_GetStringSize(f, s, 0, &w, &h);
    if (w <= max_px) {
        draw_text(a, x, y, s, col, scale);
        return;
    }
    int ew = 0, eh = 0;
    TTF_GetStringSize(f, "...", 0, &ew, &eh);
    int n = text_fit_bytes(f, s, max_px - ew);
    if (n <= 0) {
        draw_text(a, x, y, "...", col, scale);
        return;
    }
    char buf[256];
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    strcat(buf, "...");
    draw_text(a, x, y, buf, col, scale);
}

static void hit_add(App *a, int x, int y, int w, int h, CmdId cmd, int idx)
{
    if (a->nhits >= MAX_HITS) return;
    a->hits[a->nhits].r = (SDL_Rect){x, y, w, h};
    a->hits[a->nhits].cmd = cmd;
    a->hits[a->nhits].idx = idx;
    a->nhits++;
}

/* hit sotto (x,y), priorita' all'ultimo registrato; -1 se nessuno */
static int hit_find(const App *a, int x, int y)
{
    for (int i = a->nhits - 1; i >= 0; i--) {
        const SDL_Rect *r = &a->hits[i].r;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return i;
    }
    return -1;
}

static BOOL hover(const App *a, int x, int y, int w, int h)
{
    return a->mouse_x >= x && a->mouse_x < x + w &&
           a->mouse_y >= y && a->mouse_y < y + h;
}

static void button(App *a, int x, int y, const char *label, CmdId cmd, int idx,
                   Uint32 accent)
{
    int w = 16 + text_w(label, 2), h = 26;
    BOOL hov = hover(a, x, y, w, h);
    fill_rect(a, x, y, w, h, hov ? C_BTN_HI : C_BTN);
    frame_rect(a, x, y, w, h, hov ? accent : C_BORDER);
    draw_text(a, x + 8, y + (h - line_h(2)) / 2, label, hov ? C_WHITE : C_TEXT,
              2);
    hit_add(a, x, y, w, h, cmd, idx);
}

static void status_style(RouteStatus s, Uint32 *col, const char **lbl)
{
    switch (s) {
    case ROUTE_STATUS_OK:              *col = C_GREEN; *lbl = "OK"; break;
    case ROUTE_STATUS_OFFLINE:         *col = C_GRAY;  *lbl = "OFFLINE"; break;
    case ROUTE_STATUS_WRONG_INTERFACE: *col = C_AMBER; *lbl = "WRONG"; break;
    case ROUTE_STATUS_MISSING:         *col = C_AMBER; *lbl = "MISSING"; break;
    default:                           *col = C_RED;   *lbl = "ERROR"; break;
    }
}

/* Accoda alla casella del dialogo i caratteri ammessi di 'txt'. */
static void dlg_append(App *a, const char *txt)
{
    for (const char *p = txt; *p; p++) {
        if (!dlg_ok_char(*p))
            continue;
        int n = (int)strlen(a->dlg_ip);
        if (n >= NET_IP_MAX - 2)
            break;
        a->dlg_ip[n] = *p;
        a->dlg_ip[n + 1] = '\0';
    }
}

/* ============================================================ reconcile */

static BOOL route_is_ours(const App *a, const HostRoute *hr,
                          const RouteConfigItem *it);

static void reconcile(App *a, int force)
{
    (void)force;   /* le correzioni sono automatiche ed event-driven */

    net_snapshot(&a->nets);
    routes_snapshot(&a->routes);

    unsigned long def = routes_default_ifindex(&a->routes);
    for (int i = 0; i < a->nets.count; i++)
        a->nets.items[i].has_default_route = (a->nets.items[i].ifindex == def);

    int nok = 0, nmiss = 0, nwrg = 0, noff = 0, nerr = 0;
    int cfg_changed = 0;

    for (int j = 0; j < a->cfg->count; j++) {
        RouteConfigItem *it = &a->cfg->items[j];
        const NetInterface *ni = net_resolve(&a->nets, it->guid, it->name);

        a->iface_of[j] = ni ? net_index_of(&a->nets, ni) : -1;
        a->actual_if[j][0] = '\0';
        a->reason[j][0] = '\0';

        if (!ni) {
            a->st[j] = ROUTE_STATUS_OFFLINE;
            snprintf(a->reason[j], sizeof(a->reason[j]),
                     "adattatore non presente");
            noff++;
            continue;
        }
        if (ni->state == NET_DISCONNECTED || !ni->ipv4[0] ||
            !ni->gateway[0] || ni->ifindex == 0) {
            a->st[j] = ROUTE_STATUS_OFFLINE;
            snprintf(a->reason[j], sizeof(a->reason[j]), "%.96s DISCONNECTED",
                     ni->friendly_name);
            noff++;
            continue;
        }

        /* Una route e' "gestita" e corretta solo se destination, prefix,
         * ifIndex E gateway coincidono con i parametri correnti. */
        if (routes_find_host_exact(&a->routes, it->ip, 32, ni->ifindex,
                                   ni->gateway)) {
            snprintf(a->actual_if[j], sizeof(a->actual_if[j]), "%s",
                     ni->friendly_name);
            a->st[j] = ROUTE_STATUS_OK; nok++;
            cfg_changed |= cfg_set_last(a->cfg, it->ip, ni->gateway,
                                        ni->ifindex);
            dbg("[ROUTE] %s/32 OK (if %lu gw %s)", it->ip, ni->ifindex,
                ni->gateway);
            continue;
        }

        /* Non esatta: rimuoviamo le eventuali route OBSOLETE che appartengono
         * alla nostra interfaccia (stesso GUID) o che puntano a un ifIndex non
         * piu' presente (es. cambiato dopo un tethering). Le route di terze
         * parti (es. VPN) NON vengono toccate. */
        char eb[256];
        int stale = 0;
        for (int k = 0; k < a->routes.count; k++) {
            const HostRoute *hr = &a->routes.items[k];
            if (hr->prefix_len != 32 || strcmp(hr->ip, it->ip) != 0)
                continue;
            if (!route_is_ours(a, hr, it))
                continue;   /* route di un'altra interfaccia: la lasciamo */
            route_delete(it->ip, hr->gateway, hr->ifindex, eb, sizeof(eb));
            stale = 1;
        }
        if (stale)
            routes_snapshot(&a->routes);   /* aggiorna dopo le cancellazioni */

        /* Ricrea con i parametri correnti dell'interfaccia. */
        if (route_add_persistent(it->ip, ni->gateway, ni->ifindex,
                                 eb, sizeof(eb))) {
            routes_snapshot(&a->routes);
            if (routes_find_host_exact(&a->routes, it->ip, 32, ni->ifindex,
                                       ni->gateway)) {
                snprintf(a->actual_if[j], sizeof(a->actual_if[j]), "%s",
                         ni->friendly_name);
                a->st[j] = ROUTE_STATUS_OK; nok++;
                cfg_changed |= cfg_set_last(a->cfg, it->ip, ni->gateway,
                                            ni->ifindex);
                dbg("[ROUTE] %s/32 corretta -> %s (if %lu gw %s)",
                    it->ip, ni->friendly_name, ni->ifindex, ni->gateway);
            } else {
                a->st[j] = stale ? ROUTE_STATUS_WRONG_INTERFACE
                                 : ROUTE_STATUS_MISSING;
                snprintf(a->reason[j], sizeof(a->reason[j]),
                         "route non confermata");
                if (stale) nwrg++; else nmiss++;
            }
        } else {
            a->st[j] = ROUTE_STATUS_ERROR;
            snprintf(a->reason[j], sizeof(a->reason[j]), "%s",
                     eb[0] ? eb : "creazione fallita");
            nerr++;
        }
    }

    /* I parametri last_* cambiano quando la route e' confermata con nuovi
     * gateway/ifIndex: salviamo subito, cosi' una successiva rimozione con
     * interfaccia assente puo' sempre contare su una delete esatta. */
    if (cfg_changed)
        cfg_save(a->cfg);

    if (a->cfg->count == 0) {
        snprintf(a->summary, sizeof(a->summary),
                 "STATUS: nessuna route configurata");
    } else {
        int p = snprintf(a->summary, sizeof(a->summary),
                         "STATUS: %d route configurate - OK %d, OFFLINE %d, "
                         "MANCANTI %d, CONFLITTO %d, ERRORI %d",
                         a->cfg->count, nok, noff, nmiss, nwrg, nerr);
        int shown = 0;
        for (int j = 0; j < a->cfg->count && p < (int)sizeof(a->summary) - 2;
             j++) {
            if (a->st[j] != ROUTE_STATUS_OFFLINE) continue;
            if (shown >= 2) {
                snprintf(a->summary + p, sizeof(a->summary) - (size_t)p,
                         "\n  - ...");
                break;
            }
            int w = snprintf(a->summary + p, sizeof(a->summary) - (size_t)p,
                             "\n  - %s", a->reason[j]);
            if (w <= 0) break;
            p += w; shown++;
        }
    }
}

/* ====================================================== pannello sinistro */

static void draw_left(App *a)
{
    int x = MARGIN;
    int w = (int)(a->w * 0.57f);
    if (w < 400) w = 400;
    if (w > a->w - 96) w = a->w - 96;

    a->left_maxy = HEADER_BOT;
    int y = HEADER_BOT - a->scroll;

    for (int i = 0; i < a->nets.count; i++) {
        const NetInterface *ni = &a->nets.items[i];

        int nass = 0;
        for (int j = 0; j < a->cfg->count; j++)
            if (a->iface_of[j] == i)
                nass++;

        int rh = 10 + 3 * LIN + (ni->has_default_route ? LIN : 0) +
                 (nass ? 10 + LIN + nass * LIN : 0) + 10;
        int cy = y;

        if (cy + rh < TOP - 2) {                 /* fuori schermo sopra */
            y += rh + 8;
            if (y > a->left_maxy) a->left_maxy = y;
            continue;
        }

        fill_rect(a, x, cy, w, rh, C_PANEL);
        frame_rect(a, x, cy, w, rh, C_BORDER);

        int tx = x + 8;
        Uint32 sc = (ni->state == NET_CONNECTED) ? C_GREEN : C_RED;
        const char *st = (ni->state == NET_CONNECTED) ? "[CONNECTED]"
                                                      : "[DISCONNECTED]";

        draw_text(a, tx, cy + 8, ni->friendly_name[0] ? ni->friendly_name : "?",
                  C_TEXT, 2);
        draw_text(a, x + w - 8 - text_w(st, 2), cy + 8, st, sc, 2);

        char tmp[NET_IP_MAX + 40];
        int y2 = cy + 8 + LIN;
        snprintf(tmp, sizeof(tmp), "IPv4: %s",
                 ni->ipv4[0] ? ni->ipv4 : "-");
        draw_text(a, tx, y2, tmp, ni->ipv4[0] ? C_TEXT : C_DIM, 2);
        snprintf(tmp, sizeof(tmp), "GW: %s",
                 ni->gateway[0] ? ni->gateway : "-");
        draw_text(a, tx + 18 * 16, y2, tmp, C_DIM, 2);

        int y3 = y2 + LIN;
        snprintf(tmp, sizeof(tmp), "ifIndex: %lu    Metric: %lu",
                 ni->ifindex, ni->metric);
        draw_text(a, tx, y3, tmp, C_DIM, 2);

        int y4 = y3;
        if (ni->has_default_route) {
            y4 += LIN;
            draw_text(a, tx, y4, "Default route: YES (0.0.0.0/0)", C_ACCENT, 2);
        }

        if (nass > 0) {
            y4 += LIN + 8;
            draw_text(a, tx, y4, "Assigned IPs:", C_DIM, 2);
            y4 += LIN;

            int row_h = LIN - 1;
            for (int j = 0; j < a->cfg->count && y4 < cy + rh; j++) {
                if (a->iface_of[j] != i) continue;
                RouteConfigItem *it = &a->cfg->items[j];
                Uint32 scol;
                const char *sl;
                status_style(a->st[j], &scol, &sl);

                int bx_edit = x + w - 8 - 56;
                int bx_rem  = x + w - 8 - 56 - 8 - 84;

                fill_rect(a, bx_rem, y4 + 1, 84, row_h - 2,
                          hover(a, bx_rem, y4, 84, row_h) ? C_BTN_HI : C_BTN);
                frame_rect(a, bx_rem, y4, 84, row_h,
                           hover(a, bx_rem, y4, 84, row_h) ? C_ACCENT
                                                           : C_BORDER);
                hit_add(a, bx_rem + 1, y4 + 1, 82, row_h - 2, CMD_REMOVE, j);
                draw_text(a, bx_rem + 12, y4 + (row_h - line_h(2)) / 2,
                          "REMOVE", C_RED, 2);

                fill_rect(a, bx_edit, y4 + 1, 56, row_h - 2,
                          hover(a, bx_edit, y4, 56, row_h) ? C_BTN_HI : C_BTN);
                frame_rect(a, bx_edit, y4, 56, row_h,
                           hover(a, bx_edit, y4, 56, row_h) ? C_ACCENT
                                                            : C_BORDER);
                hit_add(a, bx_edit + 1, y4 + 1, 54, row_h - 2, CMD_EDIT, j);
                draw_text(a, bx_edit + 10, y4 + (row_h - line_h(2)) / 2,
                          "EDIT", C_ACCENT, 2);

                draw_text(a, tx, y4 + (row_h - line_h(2)) / 2, it->ip, C_TEXT,
                          2);
                draw_text_cut(a, bx_rem - 6 - 96, y4 + (row_h - 8) / 2, sl,
                              scol, 1, 96);

                y4 += row_h + 1;
            }
        }

        y += rh + 8;
        if (y > a->left_maxy) a->left_maxy = y;
    }

    if (a->nets.count == 0)
        draw_text(a, x + 8, TOP, "Nessuna interfaccia trovata.", C_DIM, 2);

    /* -------------------------------------------------- regole orfane
     * Regole la cui interfaccia non e' presente (es. periferica scollegata).
     * Senza questa sezione non sarebbe possibile rimuoverle: i pulsanti
     * REMOVE/EDIT vivono solo dentro il blocco "Assigned IPs" di
     * un'interfaccia attiva. */
    int n_or = 0;
    for (int j = 0; j < a->cfg->count; j++)
        if (a->iface_of[j] == -1)
            n_or++;
    if (n_or > 0) {
        int rh = 10 + LIN + n_or * (LIN + 1) + 10;
        int cy = y;
        if (cy + rh >= TOP - 2) {
            fill_rect(a, x, cy, w, rh, C_PANEL2);
            frame_rect(a, x, cy, w, rh, C_BORDER);
            draw_text(a, x + 8, cy + 8, "Interfacce assenti:", C_AMBER, 2);
            int y4 = cy + 8 + LIN;
            int row_h = LIN - 1;
            for (int j = 0; j < a->cfg->count && y4 < cy + rh; j++) {
                if (a->iface_of[j] != -1) continue;
                RouteConfigItem *it = &a->cfg->items[j];
                Uint32 scol;
                const char *sl;
                status_style(a->st[j], &scol, &sl);

                int bx_edit = x + w - 8 - 56;
                int bx_rem  = x + w - 8 - 56 - 8 - 84;

                fill_rect(a, bx_rem, y4 + 1, 84, row_h - 2,
                          hover(a, bx_rem, y4, 84, row_h) ? C_BTN_HI : C_BTN);
                frame_rect(a, bx_rem, y4, 84, row_h,
                           hover(a, bx_rem, y4, 84, row_h) ? C_ACCENT
                                                           : C_BORDER);
                hit_add(a, bx_rem + 1, y4 + 1, 82, row_h - 2, CMD_REMOVE, j);
                draw_text(a, bx_rem + 12, y4 + (row_h - line_h(2)) / 2,
                          "REMOVE", C_RED, 2);

                fill_rect(a, bx_edit, y4 + 1, 56, row_h - 2,
                          hover(a, bx_edit, y4, 56, row_h) ? C_BTN_HI : C_BTN);
                frame_rect(a, bx_edit, y4, 56, row_h,
                           hover(a, bx_edit, y4, 56, row_h) ? C_ACCENT
                                                            : C_BORDER);
                hit_add(a, bx_edit + 1, y4 + 1, 54, row_h - 2, CMD_EDIT, j);
                draw_text(a, bx_edit + 10, y4 + (row_h - line_h(2)) / 2,
                          "EDIT", C_ACCENT, 2);

                draw_text(a, x + 8, y4 + (row_h - line_h(2)) / 2, it->ip,
                          C_TEXT, 2);
                draw_text_cut(a, bx_rem - 6 - 96, y4 + (row_h - 8) / 2, sl,
                              scol, 1, 96);

                y4 += row_h + 1;
            }
        }
        y += rh + 8;
        if (y > a->left_maxy) a->left_maxy = y;
    }
}

/* ====================================================== pannello destro */

/* a capo per parole: disegna 's' su righe che restano entro 'avail'.
 * Aggiorna ly all'ultima riga disegnata. */
static void text_wrap(App *a, int lx, int *ly, int avail, const char *s)
{
    while (*s) {
        const char *nl = strchr(s, '\n');
        const char *ep = nl ? nl : s + strlen(s);
        ptrdiff_t len = ep - s;
        if (len > 511) len = 511;
        char buf[512];
        memcpy(buf, s, len);
        buf[len] = '\0';

        char line[512];
        line[0] = '\0';
        char *q = buf;
        while (*q) {
            while (*q == ' ') q++;
            if (!*q) break;
            char *ws = q;
            while (*ws && *ws != ' ') ws++;

            /* parola [q, ws): la termino temporaneamente per misurarla */
            char save = *ws;
            if (save) *ws = '\0';
            char cand[600];
            if (line[0])
                snprintf(cand, sizeof cand, "%s %s", line, q);
            else
                snprintf(cand, sizeof cand, "%s", q);
            if (save) *ws = save;

            if (text_w(cand, 1) > avail) {
                /* la riga corrente non tiene la parola -> nuova riga */
                if (line[0]) {
                    draw_text(a, lx, *ly, line, C_TEXT, 1);
                    *ly += g_h1 + 2;
                    line[0] = '\0';
                    continue;   /* riprova la parola su una riga vuota */
                }
                /* parola singola piu' larga della riga: disegno e spezzo */
                char wb[512];
                snprintf(wb, sizeof wb, "%s", q);
                draw_text_cut(a, lx, *ly, wb, C_TEXT, 1, avail);
                *ly += g_h1 + 2;
                q = *ws ? ws + 1 : ws;
                continue;
            }
            snprintf(line, sizeof line, "%.*s", (int)sizeof(line) - 1, cand);
            q = *ws ? ws + 1 : ws;
        }
        if (line[0]) {
            draw_text(a, lx, *ly, line, C_TEXT, 1);
            *ly += g_h1 + 2;
        }

        s = ep;
        if (*s == '\n') s++;
    }
}

/* Pulsante in sequenza orizzontale: ritorna la x del successivo. */
static int flow_button(App *a, int x, int y, const char *label, CmdId cmd,
                       int idx, Uint32 accent)
{
    button(a, x, y, label, cmd, idx, accent);
    return x + 16 + text_w(label, 2) + 8;
}

static void draw_right(App *a)
{
    int rx = MARGIN + (int)(a->w * 0.57f);
    if (a->w - rx - MARGIN < 260) rx = a->w - 260 - MARGIN;
    int rw = a->w - rx - MARGIN;
    a->right_x = rx;

    draw_text(a, rx + 4, TOP - 30, "ROUTING RULES", C_TEXT, 2);

    int y = TOP;
    int bx = rx + 4;
    bx = flow_button(a, bx, y, "ADD IP", CMD_ADD, -1, C_ACCENT);
    bx = flow_button(a, bx, y, "REFRESH", CMD_REFRESH, -1, C_DIM);
    flow_button(a, bx, y, "APPLY", CMD_APPLY, -1, C_GREEN);

    int table_top = y + 34;
    int zone_bottom = a->h - BOTTOM - 44;
    int ROW = g_h1 + 4;

    int x1 = rx + 4, x2 = x1 + 15 * 8 + 12, x3 = x2 + 12 * 8 + 12;
    draw_text(a, x1, table_top, "IP", C_DIM, 1);
    draw_text(a, x2, table_top, "CONFIGURED", C_DIM, 1);
    draw_text(a, x3 + 8, table_top, "STATUS", C_DIM, 1);
    fill_rect(a, rx + 2, table_top + g_h1 + 8, rw - 4, 1, C_BORDER);

    int table_h = a->cfg->count * ROW + 14 + g_h1;
    int vis = zone_bottom - table_top - 12;
    int maxs = table_h - vis;
    if (maxs < 0) maxs = 0;
    if (a->rscroll > maxs) a->rscroll = maxs;

    int ry = table_top + 14 + g_h1 - a->rscroll;
    for (int j = 0; j < a->cfg->count; j++) {
        if (ry + ROW < TOP || ry > zone_bottom + ROW) { ry += ROW; continue; }
        RouteConfigItem *it = &a->cfg->items[j];
        const char *conf = it->name;
        if (a->iface_of[j] >= 0 &&
            a->nets.items[a->iface_of[j]].friendly_name[0])
            conf = a->nets.items[a->iface_of[j]].friendly_name;
        Uint32 scol;
        const char *stt;
        status_style(a->st[j], &scol, &stt);

        draw_text_cut(a, x1, ry, it->ip, C_TEXT, 1, 15 * 8);
        draw_text_cut(a, x2, ry, conf, C_DIM, 1, 12 * 8);
        draw_text_cut(a, x3 + 8, ry, stt, scol, 1, 10 * 8);
        ry += ROW;
    }

    if (a->cfg->count == 0)
        draw_text(a, x1, table_top + g_h1 + 8 + g_h1,
                  "(nessuna regola - usa ADD IP)", C_DIM, 1);

    /* pannello di stato */
    int sy = zone_bottom + 4;
    int sh = a->h - BOTTOM - sy - 4;
    fill_rect(a, rx, sy, rw, sh, C_PANEL);
    frame_rect(a, rx, sy, rw, sh, C_BORDER);

    int avail = rw - 14;
    int lx = rx + 7, ly = sy + 6;
    text_wrap(a, lx, &ly, avail, a->summary);
}

static void draw_statusbar(App *a)
{
    fill_rect(a, 0, a->h - BOTTOM, a->w, BOTTOM, C_PANEL2);
    draw_text(a, MARGIN, a->h - BOTTOM + 9,
              a->opmsg[0] ? a->opmsg
                          : "Pronto - le route persistenti vengono ricreate "
                            "automaticamente",
              C_DIM, 1);
    draw_text(a, a->w - 2 * MARGIN - text_w("F5 refresh", 1),
              a->h - BOTTOM + 9, "F5 refresh", C_GRAY, 1);
}

/* ============================================================== dialogo */

static void draw_dialog(App *a)
{
    int pw = 480, ph = 268;
    int px = (a->w - pw) / 2, py = (a->h - ph) / 2 - 20;

    fill_rect(a, 0, 0, a->w, a->h, 0xB010141B);

    fill_rect(a, px, py, pw, ph, C_PANEL2);
    frame_rect(a, px, py, pw, ph, C_BORDER);

    draw_title(a, px + 14, py + 10,
               a->dlg_edit >= 0 ? "EDIT ROUTE" : "ADD ROUTE");

    int y = py + 38;
    draw_text(a, px + 14, y, "IP ADDRESS:", C_DIM, 1);
    y += 14;
    int ix = px + 14, iw = pw - 28, ih = 22;
    fill_rect(a, ix, y, iw, ih, C_INPUT);
    frame_rect(a, ix, y, iw, ih, C_BORDER);
    draw_text(a, ix + 6, y + 3, a->dlg_ip[0] ? a->dlg_ip : ".", C_TEXT, 2);
    if (a->dlg_ip[0])
        fill_rect(a, ix + 6 + text_w(a->dlg_ip, 2), y + 3, 2, 14, C_ACCENT);
    if (a->dlg_edit < 0)
        draw_text(a, px + 14, y + ih + 3, "(IP oppure nome processo)",
                  C_DIM, 1);

    y += ih + 16 + (a->dlg_edit < 0 ? 18 : 0);
    draw_text(a, px + 14, y, "NETWORK INTERFACE:", C_DIM, 1);
    y += 14;
    int iw2 = pw - 28, ih2 = 22;
    const char *sel = a->dlg_net >= 0 && a->dlg_net < a->nets.count
                          ? a->nets.items[a->dlg_net].friendly_name
                          : "Seleziona interfaccia";
    BOOL dv = a->dlg_drop;
    fill_rect(a, ix, y, iw2, ih2, hover(a, ix, y, iw2, ih2) ? C_BTN_HI : C_BTN);
    frame_rect(a, ix, y, iw2, ih2, dv ? C_ACCENT : C_BORDER);
    hit_add(a, ix, y, iw2, ih2, CMD_DLG_DROP, -1);
    draw_text_cut(a, ix + 6, y + 3, sel, C_TEXT, 2, iw2 - 28);
    draw_text(a, ix + iw2 - 14, y + 4, dv ? "^" : "v", C_DIM, 2);

    int drop_bottom = y + ih2;
    if (dv) {
        int lh = 20;
        int max = a->nets.count;
        if (max > 8) max = 8;
        int ly = y + ih2;
        fill_rect(a, ix, ly, iw2, max * lh + 4, C_PANEL);
        frame_rect(a, ix, ly, iw2, max * lh + 4, C_ACCENT);
        for (int k = 0; k < max; k++) {
            const NetInterface *ni = &a->nets.items[k];
            char line[NET_NAME_MAX + 8];
            snprintf(line, sizeof(line), "%s%s", ni->friendly_name,
                     ni->state == NET_CONNECTED ? "  [OK]" : "");
            if (k == a->dlg_net)
                fill_rect(a, ix + 1, ly + 1 + k * lh, iw2 - 2, lh - 1,
                          C_BTN_HI);
            hit_add(a, ix + 1, ly + 1 + k * lh, iw2 - 2, lh - 1, CMD_DLG_NET,
                    k);
            draw_text_cut(a, ix + 6, ly + (lh - g_h1) / 2 + k * lh, line,
                          (k == a->dlg_net) ? C_WHITE : C_TEXT, 1, iw2 - 12);
        }
        drop_bottom = ly + max * lh + 4;
    }

    if (a->dlg_msg[0])
        draw_text_cut(a, px + 14, drop_bottom + 6, a->dlg_msg, C_RED, 1,
                      pw - 28);

    button(a, px + pw - 8 - 150, py + ph - 36, "ADD", CMD_DLG_ADD, -1,
           C_GREEN);
    button(a, px + pw - 8 - 82, py + ph - 36, "CANCEL", CMD_DLG_CANCEL, -1,
           C_DIM);
}

/* ====================================================== azioni dialogo */

static void delete_owned_routes(App *a, const char *ip, const char *guid);

static void dialog_open(App *a, int edit_idx)
{
    a->dlg = TRUE;
    a->dlg_edit = edit_idx;
    a->dlg_msg[0] = '\0';
    a->dlg_drop = FALSE;
    SDL_StartTextInput(a->win);

    if (edit_idx >= 0) {
        snprintf(a->dlg_ip, sizeof(a->dlg_ip), "%s",
                 a->cfg->items[edit_idx].ip);
        a->dlg_net = a->iface_of[edit_idx];
    } else {
        a->dlg_ip[0] = '\0';
        a->dlg_net = -1;
        for (int i = 0; i < a->nets.count; i++)
            if (a->nets.items[i].state == NET_CONNECTED) {
                a->dlg_net = i;
                break;
            }
    }
}

static void dialog_submit(App *a)
{
    const char *ip = a->dlg_ip;

    if (a->dlg_net < 0 || a->dlg_net >= a->nets.count) {
        snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                 "Selezionare un'interfaccia valida.");
        return;
    }
    const NetInterface *ni = &a->nets.items[a->dlg_net];

    /* EDIT: si lavora sempre su un singolo IP. */
    if (a->dlg_edit >= 0) {
        if (!net_valid_ipv4(ip)) {
            snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                     "Indirizzo non valido: '%s'", ip[0] ? ip : "(vuoto)");
            return;
        }
        RouteConfigItem *oit = &a->cfg->items[a->dlg_edit];
        const char *old = oit->ip;
        BOOL iface_changed = _stricmp(oit->guid, ni->guid) != 0;

        if (strcmp(old, ip) != 0) {
            if (cfg_find(a->cfg, ip) >= 0) {
                snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                         "IP gia' configurato: %s", ip);
                return;
            }
            /* L'IP cambia: la vecchia route non deve restare orfana. */
            delete_owned_routes(a, old, oit->guid);
            cfg_remove(a->cfg, old);
            if (!cfg_add(a->cfg, ip, ni->friendly_name, ni->guid)) {
                snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                         "Impossibile aggiungere %s", ip);
                return;
            }
        } else if (iface_changed) {
            /* Cambia solo l'interfaccia: rimuovi la route sulla vecchia
             * interfaccia, poi aggiorna la regola (nessuna seconda regola). */
            delete_owned_routes(a, old, oit->guid);
            cfg_update(a->cfg, ip, ni->friendly_name, ni->guid);
        } else {
            cfg_update(a->cfg, ip, ni->friendly_name, ni->guid);
        }
        if (cfg_save(a->cfg)) {
            a->dlg = FALSE;
            a->dlg_drop = FALSE;
            SDL_StopTextInput(a->win);
            reconcile(a, 0);
        } else {
            snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                     "ERRORE: salvataggio configurazione fallito.");
            dbg("[CFG] salvataggio fallito dopo EDIT");
        }
        return;
    }

    /* ADD: se l'input e' un IP valido si aggiunge una sola regola. */
    if (net_valid_ipv4(ip)) {
        if (cfg_find(a->cfg, ip) >= 0) {
            snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                     "IP gia' configurato: %s", ip);
            return;
        }
        if (!cfg_add(a->cfg, ip, ni->friendly_name, ni->guid)) {
            snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                     "Limite regole raggiunto.");
            return;
        }
        if (cfg_save(a->cfg)) {
            a->dlg = FALSE;
            a->dlg_drop = FALSE;
            SDL_StopTextInput(a->win);
            reconcile(a, 0);
        } else {
            snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                     "ERRORE: salvataggio configurazione fallito.");
            dbg("[CFG] salvataggio fallito dopo ADD");
        }
        return;
    }

    /* ADD: altrimenti l'input e' un nome di processo -> si risolvono gli
     * IP connessi e si crea una regola per ciascuno. */
    if (!ip[0]) {
        snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                 "Inserire un IP o il nome di un processo.");
        return;
    }
    char ips[PROC_MAX_IPS][NET_IP_MAX];
    int n = 0;
    if (proc_resolve_ips(ip, ips, &n) <= 0) {
        snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                 "Nessun IP trovato per il processo '%s'", ip);
        return;
    }

    int added = 0;
    for (int k = 0; k < n; k++) {
        if (cfg_find(a->cfg, ips[k]) >= 0)
            continue; /* gia' presente: salto */
        if (!cfg_add(a->cfg, ips[k], ni->friendly_name, ni->guid))
            break; /* limite regole raggiunto */
        added++;
    }
    if (added == 0) {
        snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                 "Tutti gli IP del processo sono gia' configurati.");
        return;
    }
    if (cfg_save(a->cfg)) {
        a->dlg_msg[0] = '\0';
        a->dlg = FALSE;
        a->dlg_drop = FALSE;
        SDL_StopTextInput(a->win);
        reconcile(a, 0);
    } else {
        snprintf(a->dlg_msg, sizeof(a->dlg_msg),
                 "ERRORE: salvataggio configurazione fallito.");
        dbg("[CFG] salvataggio fallito dopo ADD processo");
    }
}

/* ============================================================ azioni UI */

/* Adotta nella configurazione le route host /32 presenti nel sistema:
 * la correlazione route -> interfaccia avviene PRIMARIAMENTE tramite
 * route.ifIndex -> adapter -> GUID (il gateway e' solo una verifica
 * aggiuntiva, per il caso di piu' interfacce con gateway uguale).
 * Ritorna il numero di route adottate, oppure -1 se il salvataggio fallisce. */
static int import_system_routes(App *a)
{
    routes_snapshot(&a->routes);

    int added = 0;
    for (int i = 0; i < a->routes.count; i++) {
        const HostRoute *hr = &a->routes.items[i];
        if (hr->prefix_len != 32 || hr->ifindex == 0)
            continue;

        /* Correlazione primaria: ifIndex della route -> interfaccia nota. */
        int idx = net_index_of_ifindex(&a->nets, hr->ifindex);
        if (idx < 0)
            continue;
        const NetInterface *ni = &a->nets.items[idx];
        if (hr->gateway[0] && ni->gateway[0] &&
            strcmp(hr->gateway, ni->gateway) != 0)
            continue;   /* verifica aggiuntiva: gateway incompatibile */

        if (cfg_find(a->cfg, hr->ip) >= 0)
            continue; /* gia' adottata */
        if (cfg_add(a->cfg, hr->ip, ni->friendly_name, ni->guid))
            added++;
    }

    if (added > 0 && !cfg_save(a->cfg))
        return -1;
    return added;
}

/* Elimina le route /32 verso `ip` che appartengono all'interfaccia `guid`
 * (stesso GUID) oppure che puntano a un ifIndex non piu' presente nella rete.
 * Se l'interfaccia non e' piu' presente usa gli ULTIMI parametri noti salvati
 * in config per rimuovere anche la voce persistente in modo esatto. Le route
 * di terze parti (es. VPN) restano intatte. */
static BOOL route_is_ours(const App *a, const HostRoute *hr,
                          const RouteConfigItem *it)
{
    int idx = net_index_of_ifindex(&a->nets, hr->ifindex);
    if (idx >= 0) {
        const NetInterface *ni = net_resolve(&a->nets, it->guid, it->name);
        return ni && ni->ifindex == hr->ifindex;
    }
    /* ifIndex non piu' presente tra le interfacce: la route e' nostra SOLO
     * se coincide esattamente con gli ultimi parametri noti con cui fu
     * creata. Una route di terze parti (es. VPN disinstallata) verso lo
     * stesso IP non viene quindi cancellata per sbaglio. */
    if (hr->ifindex != it->last_ifindex)
        return FALSE;
    return it->last_gateway[0] &&
           _stricmp(hr->gateway, it->last_gateway) == 0;
}

static void delete_owned_routes(App *a, const char *ip, const char *guid)
{
    (void)guid;   /* la proprieta' e' decisa da route_is_ours */
    routes_snapshot(&a->routes);
    char eb[128];
    int removed_persistent = 0;
    for (int k = 0; k < a->routes.count; k++) {
        const HostRoute *hr = &a->routes.items[k];
        if (hr->prefix_len != 32 || strcmp(hr->ip, ip) != 0)
            continue;
        int idx = cfg_find(a->cfg, ip);
        if (idx < 0)
            continue;
        const RouteConfigItem *it = &a->cfg->items[idx];
        if (!route_is_ours(a, hr, it))
            continue;
        route_delete(ip, hr->gateway, hr->ifindex, eb, sizeof(eb));
        removed_persistent = 1;
    }
    /* Interfaccia assente: se non abbiamo appena eliminato una voce attiva,
     * rimuoviamo la voce PERSISTENTE usando gli ultimi parametri noti. */
    if (!removed_persistent) {
        char gw[NET_IP_MAX];
        unsigned long ifidx = 0;
        if (cfg_last_known(a->cfg, ip, gw, sizeof(gw), &ifidx))
            route_delete(ip, gw, ifidx, eb, sizeof(eb));
        else
            dbg("[GUI] %s/32: nessun parametro noto per delete esatta", ip);
    }
}

static void cmd_do(App *a, CmdId cmd, int idx)
{
    switch (cmd) {
    case CMD_ADD:
        dialog_open(a, -1);
        break;
    case CMD_EDIT:
        if (idx >= 0 && idx < a->cfg->count)
            dialog_open(a, idx);
        break;
    case CMD_APPLY:
        snprintf(a->opmsg, sizeof(a->opmsg),
                 "APPLY: correzione conflitti e route mancanti.");
        reconcile(a, 1);
        break;
    case CMD_REMOVE:
        if (idx >= 0 && idx < a->cfg->count) {
            RouteConfigItem *it = &a->cfg->items[idx];
            const char *ip = it->ip;
            const NetInterface *ni = net_resolve(&a->nets, it->guid, it->name);
            char eb[128];
            BOOL online = ni && ni->state == NET_CONNECTED &&
                          ni->gateway[0] && ni->ifindex != 0;
            if (online)
                route_delete(ip, ni->gateway, ni->ifindex, eb, sizeof(eb));
            else
                delete_owned_routes(a, ip, it->guid);
            cfg_remove(a->cfg, ip);
            if (cfg_save(a->cfg)) {
                snprintf(a->opmsg, sizeof(a->opmsg),
                         "Regola %s rimossa, route eliminata.", ip);
            } else {
                snprintf(a->opmsg, sizeof(a->opmsg),
                         "ERRORE: salvataggio configurazione fallito.");
                dbg("[CFG] salvataggio fallito dopo REMOVE");
            }
            reconcile(a, 0);
        }
        break;
    case CMD_REFRESH: {
        int n = import_system_routes(a);
        if (n < 0)
            snprintf(a->opmsg, sizeof(a->opmsg),
                     "ERRORE: salvataggio configurazione fallito.");
        else
            snprintf(a->opmsg, sizeof(a->opmsg),
                     n > 0 ? "REFRESH: adottate %d route esistenti dal sistema."
                           : "Refresh eseguito.",
                     n);
        reconcile(a, 0);
        break;
    }
    case CMD_DLG_ADD:
        dialog_submit(a);
        break;
    case CMD_DLG_CANCEL:
        a->dlg = FALSE;
        a->dlg_drop = FALSE;
        SDL_StopTextInput(a->win);
        break;
    case CMD_DLG_DROP:
        a->dlg_drop = !a->dlg_drop;
        break;
    case CMD_DLG_NET:
        if (idx >= 0 && idx < a->nets.count) {
            a->dlg_net = idx;
            a->dlg_drop = FALSE;
            a->dlg_msg[0] = '\0';
        }
        break;
    default:
        break;
    }
}

/* =========================================================== rendering */

static void draw_frame(App *a)
{
    a->nhits = 0;
    fill_rect(a, 0, 0, a->w, a->h, C_BG);

    draw_title(a, MARGIN, HEADER_TOP, "Network Route Manager");
    draw_text(a, MARGIN, HEADER_TOP + g_ht + 7,
              "route IP -> interfaccia (persistenti)", C_DIM, 1);

    draw_left(a);
    draw_right(a);
    draw_statusbar(a);

    if (a->dlg)
        draw_dialog(a);
}

static void render_present(App *a)
{
    SDL_UpdateTexture(a->canvas, NULL, a->surf->pixels, a->surf->pitch);
    SDL_SetRenderDrawColor(a->ren, 0x10, 0x14, 0x1B, 0xFF);
    SDL_RenderClear(a->ren);
    SDL_RenderTexture(a->ren, a->canvas, NULL, NULL);
    SDL_RenderPresent(a->ren);
    a->redraw = 0;
}

static BOOL resize(App *a, int w, int h)
{
    if (w < 320 || h < 220)
        return FALSE;
    a->w = w;
    a->h = h;
    a->mouse_x = a->mouse_y = 0;
    if (a->surf)
        SDL_DestroySurface(a->surf);
    if (a->canvas)
        SDL_DestroyTexture(a->canvas);
    a->surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (!a->surf)
        return FALSE;
    a->canvas = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
    return a->canvas != NULL;
}

/* ======================================================= eventi & main */

static void on_net_change(void *user)
{
    (void)user;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    e.user.code = 1;
    SDL_PushEvent(&e);
}

static void handle_event(App *a, SDL_Event *e)
{
    int w = a->w, h = a->h;

    switch (e->type) {
    case SDL_EVENT_QUIT:
        a->running = FALSE;
        break;

    case SDL_EVENT_WINDOW_RESIZED:
        SDL_GetWindowSize(a->win, &w, &h);
        if (resize(a, w, h))
            a->redraw = TRUE;
        break;

    case SDL_EVENT_MOUSE_MOTION:
        a->mouse_x = (int)e->motion.x;
        a->mouse_y = (int)e->motion.y;
        {
            /* ridisegna solo se cambia l'elemento sotto il cursore:
             * il movimento a riposo non ridisegna (CPU ~0%) */
            int hi = hit_find(a, a->mouse_x, a->mouse_y);
            if (hi != a->hover_hit) {
                a->hover_hit = hi;
                a->redraw = TRUE;
            }
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            int mx = (int)e->button.x, my = (int)e->button.y;
            int hi = hit_find(a, mx, my);
            if (hi >= 0) {
                Hit *h0 = &a->hits[hi];
                if (a->dlg) {
                    if (h0->cmd >= CMD_DLG_ADD && h0->cmd <= CMD_DLG_NET)
                        cmd_do(a, h0->cmd, h0->idx);
                } else {
                    cmd_do(a, h0->cmd, h0->idx);
                }
            }
            a->redraw = TRUE;
        }
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        if (a->dlg && a->dlg_drop) {
            int d = e->wheel.y < 0 ? 1 : -1;
            if (a->dlg_net < 0) a->dlg_net = 0;
            a->dlg_net += d;
            if (a->dlg_net < 0) a->dlg_net = 0;
            if (a->dlg_net >= a->nets.count) a->dlg_net = a->nets.count - 1;
        } else {
            int d = (int)(e->wheel.y * 24.0f);
            if ((int)e->motion.x < a->right_x) {
                a->scroll -= d;
                int maxs = (a->left_maxy - TOP) - (a->h - BOTTOM - TOP);
                if (maxs < 0) maxs = 0;
                if (a->scroll < 0) a->scroll = 0;
                if (a->scroll > maxs) a->scroll = maxs;
            } else {
                a->rscroll -= d;
                if (a->rscroll < 0) a->rscroll = 0;
            }
        }
        a->redraw = TRUE;
        break;

    case SDL_EVENT_KEY_DOWN:
        if (a->dlg) {
            if (e->key.key == SDLK_BACKSPACE) {
                int n = (int)strlen(a->dlg_ip);
                if (n > 0) a->dlg_ip[n - 1] = '\0';
                a->redraw = TRUE;
            } else if (e->key.key == SDLK_ESCAPE) {
                a->dlg = FALSE;
                a->dlg_drop = FALSE;
                SDL_StopTextInput(a->win);
                a->redraw = TRUE;
            } else if (e->key.key == SDLK_RETURN) {
                cmd_do(a, CMD_DLG_ADD, -1);
                a->redraw = TRUE;
            } else if (e->key.mod & SDL_KMOD_CTRL &&
                       e->key.key == SDLK_V) {          /* incolla */
                char *txt = SDL_GetClipboardText();
                if (txt) {
                    dlg_append(a, txt);
                    SDL_free(txt);
                }
                a->redraw = TRUE;
            } else if (e->key.mod & SDL_KMOD_CTRL &&
                       e->key.key == SDLK_C) {          /* copia */
                SDL_SetClipboardText(a->dlg_ip);
                a->redraw = TRUE;
            }
        }
        if (e->key.key == SDLK_F5) {
            cmd_do(a, CMD_REFRESH, -1);
            a->redraw = TRUE;
        }
        break;

    case SDL_EVENT_TEXT_INPUT:
        if (a->dlg) {
            dlg_append(a, e->text.text);
            a->redraw = TRUE;
        }
        break;

    case SDL_EVENT_USER:
        a->need_recon = 1;
        a->redraw = TRUE;
        break;

    default:
        break;
    }
}

void gui_run(Config *cfg)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init fallito: %s\n", SDL_GetError());
        return;
    }
    if (!ui_fonts_init()) {
        fprintf(stderr, "Inizializzazione font fallita: %s\n",
                SDL_GetError());
        SDL_Quit();
        return;
    }

    App a;
    memset(&a, 0, sizeof(a));
    a.cfg = cfg;
    a.dlg_net = -1;
    a.dlg_edit = -1;
    a.hover_hit = -2;   /* forza il ridisegno al primo movimento del mouse */
    a.running = TRUE;

    a.win = SDL_CreateWindow("Network Route Manager", 990, 720,
                             SDL_WINDOW_RESIZABLE);
    if (!a.win) {
        fprintf(stderr, "CreateWindow fallito: %s\n", SDL_GetError());
        SDL_Quit();
        return;
    }

    a.ren = SDL_CreateRenderer(a.win, NULL);
    if (!a.ren) {
        fprintf(stderr, "CreateRenderer fallito: %s\n", SDL_GetError());
        SDL_DestroyWindow(a.win);
        SDL_Quit();
        return;
    }

    int w, h;
    SDL_GetWindowSize(a.win, &w, &h);
    if (!resize(&a, w, h)) {
        SDL_DestroyRenderer(a.ren);
        SDL_DestroyWindow(a.win);
        SDL_Quit();
        return;
    }

    /* snap-compare iniziale: enumera e crea le route mancanti */
    reconcile(&a, 0);

    NetMon *mon = monitor_start(on_net_change, &a);

    while (a.running) {
        SDL_Event e;
        if (SDL_WaitEvent(&e))
            handle_event(&a, &e);

        /* drena altri eventi (coalesce le notifiche di rete) */
        SDL_Event nxt;
        while (a.running && SDL_PollEvent(&nxt))
            handle_event(&a, &nxt);

        if (a.need_recon) {
            reconcile(&a, 0);
            a.need_recon = 0;
        }

        if (a.redraw) {
            draw_frame(&a);
            render_present(&a);
        }
    }

    monitor_stop(mon);
    if (a.canvas) SDL_DestroyTexture(a.canvas);
    if (a.surf)   SDL_DestroySurface(a.surf);
    SDL_DestroyRenderer(a.ren);
    SDL_DestroyWindow(a.win);
    ui_fonts_quit();
    SDL_Quit();
}