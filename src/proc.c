/*
 * proc.c - risoluzione degli IP IPv4 remoti raggiunti da un processo.
 *
 * Porting di procedure e criteri dal progetto "ip_graph" (casella di testo
 * che accetta un IP o un nome di processo): se l'input non e' un IP, vengono
 * cercati tutti i processi con quel nome e, attraverso la tabella TCP con
 * owner PID, raccolti gli indirizzi IP REMOTI ad essi associati.
 *
 * La tabella UDP NON viene usata: espone solo l'indirizzo LOCALE del socket,
 * non il peer remoto. Creare una route verso un indirizzo locale della
 * macchina sarebbe errato (il programma deve instradare IP remoti), quindi la
 * funzionalita' processo -> IP e' limitata ai peer TCP. Questo e' voluto:
 * la correttezza e' piu' importante della completezza.
 */

#include "proc.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>

static int winsock_ready = 0;

static void ensure_winsock(void)
{
    if (winsock_ready)
        return;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0)
        winsock_ready = 1;
}

static int add_unique(char ips[PROC_MAX_IPS][NET_IP_MAX], int *count,
                      const char *ip)
{
    if (*count >= PROC_MAX_IPS)
        return 0;
    for (int i = 0; i < *count; i++) {
        if (strcmp(ips[i], ip) == 0)
            return 0;
    }
    snprintf(ips[*count], NET_IP_MAX, "%s", ip);
    (*count)++;
    return 1;
}

/* Scarta indirizzi inutili: 0.x, loopback, broadcast, multicast/riservati,
 * link-local 169.254.x. */
static int is_useful_ipv4(DWORD addr)
{
    unsigned int b0 = addr & 0xff;
    unsigned int b1 = (addr >> 8) & 0xff;
    if (b0 == 0)
        return 0;
    if (b0 == 127)
        return 0;
    if (b0 == 255)
        return 0;
    if (b0 >= 224 && b0 <= 239)
        return 0;
    if (b0 == 169 && b1 == 254)
        return 0;
    return 1;
}

static void addr_to_str(DWORD addr, char *out, size_t len)
{
    struct in_addr in;
    in.s_addr = addr;
    if (!InetNtopA(AF_INET, &in, out, (DWORD)len))
        out[0] = '\0';
}

static int name_matches(const char *exe, const char *input)
{
    if (_stricmp(exe, input) == 0)
        return 1;
    char with_exe[260];
    snprintf(with_exe, sizeof(with_exe), "%s.exe", input);
    return _stricmp(exe, with_exe) == 0;
}

int proc_resolve_ips(const char *name,
                     char ips[PROC_MAX_IPS][NET_IP_MAX], int *count)
{
    *count = 0;
    if (!name || !name[0])
        return 0;

    ensure_winsock();

    DWORD pids[256];
    int npid = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (name_matches(pe.szExeFile, name)) {
                if (npid < 256)
                    pids[npid++] = pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);

    if (npid == 0)
        return 0;

    char found[PROC_MAX_IPS][NET_IP_MAX];
    int nfound = 0;

    ULONG size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) ==
            ERROR_INSUFFICIENT_BUFFER &&
        size > 0) {
        MIB_TCPTABLE_OWNER_PID *tbl = (MIB_TCPTABLE_OWNER_PID *)malloc(size);
        if (tbl) {
            if (GetExtendedTcpTable(tbl, &size, FALSE, AF_INET,
                                    TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
                for (DWORD i = 0; i < tbl->dwNumEntries; i++) {
                    MIB_TCPROW_OWNER_PID *r = &tbl->table[i];
                    if (!is_useful_ipv4(r->dwRemoteAddr))
                        continue;
                    int match = 0;
                    for (int k = 0; k < npid; k++) {
                        if (r->dwOwningPid == pids[k]) {
                            match = 1;
                            break;
                        }
                    }
                    if (!match)
                        continue;
                    char buf[NET_IP_MAX];
                    addr_to_str(r->dwRemoteAddr, buf, sizeof(buf));
                    if (buf[0])
                        add_unique(found, &nfound, buf);
                }
            }
            free(tbl);
        }
    }

    /* (UDP volutamente assente: la tabella UDP riporta solo l'IP locale del
     * socket, che NON e' un peer remoto. Vedere commento in testa al file.) */

    for (int i = 0; i < nfound; i++)
        memcpy(ips[i], found[i], NET_IP_MAX);
    *count = nfound;
    return nfound;
}