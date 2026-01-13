/* DLGlogger.c — Console UI: test comms + 1 Hz monitor (CH1, half-bridge)
 * Build via CMake (target: DLGlogger). Logs ASCII only.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

/* ---------- Config ---------- */
static const char*    DLG_IP        = "192.168.1.100";
static const uint16_t DLG_PORT      = 41401;

/* Bind local: porta fixa garante que o firewall saiba para onde liberar.
   Se precisar forcar a NIC, preencha LOCAL_BIND_IP com o IP da interface (ex.: "192.168.1.10").
   Deixe "" para bind em todas (0.0.0.0). */
#define LOCAL_BIND_IP   ""          /* "" or "192.168.1.10" */
#define LOCAL_BIND_PORT 41402       /* fixed local port */

/* Acquisition */
#define IDX_FREQ       200.0f
#define BURSTS         1
#define NSIG           1

/* Timeouts and retries */
#define TIMEOUT_MS     1000         /* recvfrom timeout */
#define START_WAIT_MS  10000        /* window to wait first OP_ACQDATA */
#define START_RETRIES  5            /* re-send START/SETUP attempts */

/* ---------- Protocol ---------- */
#pragma pack(push,1)
typedef struct { uint16_t code, res; } PktHdr;
typedef struct { uint16_t code, res, err; } PktResp;

typedef struct {
    uint16_t code, res;
    float    f;
    uint16_t clkDiv, period, preSc, useAdj;
    int16_t  bursts, nSig, ICM[8];
} PktAcqSetup;

typedef struct {
    uint16_t code, res;
    int32_t  frame, ts;
    float    f;
    int16_t  bursts, nSig;
    int32_t  r2, r3, r4;
    int16_t  smp[720];
} PktData;

typedef struct {
    uint16_t code, res, err;
    int16_t  ch, tSensor, iGain, iLPF, iSensPwr, balance;
    uint16_t fDC, fAC, fUseBal;
} PktSetCh;
#pragma pack(pop)

/* OpCodes */
#define OP_ACQSTOP   0x1006
#define OP_ACQSETUP  0x1000
#define OP_ACQSTART  0x1002
#define OP_ACQDATA   0x100B
#define OP_SETCHCFG  0x2002

/* ---------- Utils ---------- */
static void send_cmd(SOCKET s, const void* p, int len, const struct sockaddr_in* a) {
    sendto(s, (const char*)p, len, 0, (const struct sockaddr*)a, sizeof(*a));
}

static int open_udp(SOCKET* ps, struct sockaddr_in* addr)
{
    *ps = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (*ps == INVALID_SOCKET) return -1;

    /* 1) Bind local (fixed port and optional local IP) */
    struct sockaddr_in local; memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port   = htons(LOCAL_BIND_PORT);
    if (LOCAL_BIND_IP[0] == '\0') {
        local.sin_addr.s_addr = htonl(INADDR_ANY);  /* 0.0.0.0 */
    } else {
        if (inet_pton(AF_INET, LOCAL_BIND_IP, &local.sin_addr) != 1) return -1;
    }
    if (bind(*ps, (const struct sockaddr*)&local, sizeof(local)) != 0) return -1;

    /* 2) recv timeout */
    DWORD to = TIMEOUT_MS;
    setsockopt(*ps, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    /* 3) DLG destination */
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port   = htons(DLG_PORT);
    if (inet_pton(AF_INET, DLG_IP, &addr->sin_addr) != 1) return -1;

    return 0;
}

static void stop_acq(SOCKET s, const struct sockaddr_in* addr) {
    PktHdr stop = { OP_ACQSTOP, 0 };
    send_cmd(s, &stop, sizeof(stop), addr);
}

static void configure_loadcell_ch1(SOCKET s, const struct sockaddr_in* addr) {
    /* CH1: half-bridge (tSensor=4), gain=100, LPF=0 */
    PktSetCh ch; memset(&ch, 0, sizeof(ch));
    ch.code    = OP_SETCHCFG;
    ch.ch      = 1;   /* physical channel 1 (second channel) */
    ch.tSensor = 4;   /* half-bridge (3=quarter, 4=half) */
    ch.iGain   = 100; /* 1,3,10,30,100,300,1000,3000 */
    ch.iLPF    = 0;
    /* other fields keep zero by memset */
    send_cmd(s, &ch, sizeof(ch), addr);
}

static void acq_setup_ch1(SOCKET s, const struct sockaddr_in* addr, float freq_idx) {
    PktAcqSetup st; memset(&st, 0, sizeof(st));
    st.code   = OP_ACQSETUP;
    st.f      = freq_idx;
    st.bursts = BURSTS;
    st.nSig   = NSIG;
    st.ICM[0] = 1; /* CH1 */
    send_cmd(s, &st, sizeof(st), addr);
}

static void acq_start(SOCKET s, const struct sockaddr_in* addr) {
    PktHdr start = { OP_ACQSTART, 0 };
    send_cmd(s, &start, sizeof(start), addr);
}

/* Wait for first OP_ACQDATA; if not, re-send START/SETUP up to START_RETRIES */
static int wait_first_packet(SOCKET s, const struct sockaddr_in* addr) {
    for (int attempt = 0; attempt < START_RETRIES; ++attempt) {
        DWORD t0 = GetTickCount();
        for (;;) {
            PktData pd; int n = recvfrom(s, (char*)&pd, sizeof(pd), 0, NULL, NULL);
            if (n >= 4 && pd.code == OP_ACQDATA) return 0; /* ok */
            if ((GetTickCount() - t0) > (DWORD)START_WAIT_MS) break;
        }
        /* re-send START and SETUP to wake the stream */
        acq_setup_ch1(s, addr, IDX_FREQ);
        acq_start(s, addr);
    }
    return -1;
}

/* ---------- UI Actions ---------- */
static void do_test_comm(void) {
    printf("=== Test communication ===\n");
    WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w)) { printf("WSAStartup failed\n"); return; }
    SOCKET s; struct sockaddr_in addr;
    if (open_udp(&s, &addr)) { printf("socket/open failed\n"); WSACleanup(); return; }

    stop_acq(s, &addr);
    configure_loadcell_ch1(s, &addr);
    acq_setup_ch1(s, &addr, IDX_FREQ);
    acq_start(s, &addr);

    printf("Waiting first data up to %d ms (retries=%d)...\n", START_WAIT_MS, START_RETRIES);
    if (wait_first_packet(s, &addr) == 0) {
        printf("OK: data is flowing.\n");
    } else {
        printf("FAIL: no OP_ACQDATA within timeout.\n");
        printf("Hints: firewall IN localport=%d, NIC/route, START_WAIT_MS.\n", (int)LOCAL_BIND_PORT);
    }

    stop_acq(s, &addr);
    closesocket(s);
    WSACleanup();
    printf("Done.\n\n");
}

static void do_monitor_1hz(void) {
    printf("=== Monitor 1 Hz (CH1) — press Q to stop ===\n");
    WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w)) { printf("WSAStartup failed\n"); return; }
    SOCKET s; struct sockaddr_in addr;
    if (open_udp(&s, &addr)) { printf("socket/open failed\n"); WSACleanup(); return; }

    stop_acq(s, &addr);
    configure_loadcell_ch1(s, &addr);
    acq_setup_ch1(s, &addr, IDX_FREQ);
    acq_start(s, &addr);

    if (wait_first_packet(s, &addr) != 0) {
        printf("No data to monitor (timeout).\n");
        stop_acq(s, &addr); closesocket(s); WSACleanup(); return;
    }

    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LARGE_INTEGER t_win0; QueryPerformanceCounter(&t_win0);
    double sum = 0.0; int cnt = 0; long long lost = 0;
    int32_t prev_frame = -1;

    printf("Time(s)  count  avg_raw  lost\n");
    for (;;) {
        if (_kbhit()) { int c = _getch(); if (c=='q'||c=='Q') break; }

        PktData pkt; int n = recvfrom(s, (char*)&pkt, sizeof(pkt), 0, NULL, NULL);
        if (n >= 4 && pkt.code == OP_ACQDATA) {
            if (prev_frame >= 0 && pkt.frame - prev_frame > 1) lost += (pkt.frame - prev_frame - 1);
            prev_frame = pkt.frame;
            int total = pkt.bursts * pkt.nSig;
            for (int i = 0; i < total; i += pkt.nSig) { sum += (double)pkt.smp[i]; cnt++; }
        }

        LARGE_INTEGER t_now; QueryPerformanceCounter(&t_now);
        double elapsed = (double)(t_now.QuadPart - t_win0.QuadPart) / (double)fq.QuadPart;
        if (elapsed >= 1.0) {
            double avg = (cnt > 0) ? (sum / (double)cnt) : 0.0;
            printf("%7.3f  %5d  %7.2f  %4lld\n", elapsed, cnt, avg, lost);
            t_win0 = t_now; sum = 0.0; cnt = 0;
        }
    }

    stop_acq(s, &addr);
    closesocket(s);
    WSACleanup();
    printf("Monitor stopped.\n\n");
}

/* ---------- Main ---------- */
int main(void) {
    for (;;) {
        printf("==== DLG4000 Console ====\n");
        printf("[1] Test communication\n");
        printf("[2] Monitor CH1 (1 Hz print)\n");
        printf("[Q] Quit\n");
        printf("> ");
        fflush(stdout);

        int c = getchar();
        if (c == '\n' || c == '\r') continue;
        int d; while ((d = getchar()) != '\n' && d != '\r' && d != EOF) {}

        if (c == '1')       do_test_comm();
        else if (c == '2')  do_monitor_1hz();
        else if (c == 'q' || c == 'Q') break;
        else printf("Unknown option.\n\n");
    }
    return 0;
}
