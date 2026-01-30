/* DLGlogger.c — Console UI: test comms + 1 Hz monitor (CH1, half-bridge)
 * Build via CMake (target: DLGlogger). Logs ASCII only.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>

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

/* Default channel config (used when no calibration file is present) */
#define DEFAULT_TSENSOR 4
#define DEFAULT_ILPF    0
#define DEFAULT_IGAIN_IDX 4
#define DEFAULT_SENSPWR_IDX 0

static const int gain_values[] = { 1, 3, 10, 30, 100, 300, 1000, 3000 };
static const double vexc_values[] = { 1.0, 2.5, 3.3, 5.0, 0.0 };

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

static void configure_channel(SOCKET s, const struct sockaddr_in* addr,
                              int ch, int tSensor, int iLPF, int iGainIdx, int iSensPwr) {
    PktSetCh cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.code    = OP_SETCHCFG;
    cfg.ch      = (int16_t)ch;
    cfg.tSensor = (int16_t)tSensor;
    cfg.iGain   = (int16_t)iGainIdx;
    cfg.iLPF    = (int16_t)iLPF;
    cfg.iSensPwr = (int16_t)iSensPwr;
    send_cmd(s, &cfg, sizeof(cfg), addr);
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

static char* read_text_file(const char* path, size_t* out_sz) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_sz) *out_sz = n;
    return buf;
}

static int channel_in_header(const char* buf, int ch) {
    const char* p = strstr(buf, "\"channels\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    while (*p && *p != ']') {
        while (*p && !isdigit((unsigned char)*p) && *p != '-' && *p != ']') p++;
        if (!*p || *p == ']') break;
        char* end = NULL;
        long v = strtol(p, &end, 10);
        if (end != p) {
            if ((int)v == ch) return 1;
            p = end;
        } else {
            p++;
        }
    }
    return 0;
}

static int channel_matches(const char* buf, int ch) {
    char needle[32];
    _snprintf(needle, sizeof(needle), "\"channel\": \"CH%d\"", ch);
    return strstr(buf, needle) != NULL;
}

static int parse_fit_value(const char* buf, const char* key, double* out) {
    const char* p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    char* end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static int parse_int_value(const char* buf, const char* key, int* out) {
    const char* p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    char* end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

static int gain_index_from_value(int value) {
    for (int i = 0; i < (int)(sizeof(gain_values) / sizeof(gain_values[0])); ++i) {
        if (gain_values[i] == value) return i;
    }
    return -1;
}

static int load_calib_for_channel(int ch, double* slope, double* intercept,
                                  int* tSensor, int* iLPF, int* iGainIdx, int* iSensPwr) {
    char path_ch[64];
    char path_out_ch[80];
    _snprintf(path_ch, sizeof(path_ch), "calib_CH%d.json", ch);
    _snprintf(path_out_ch, sizeof(path_out_ch), "out\\calib_CH%d.json", ch);
    const char* paths[] = { "calib.json", "calib", path_ch, path_out_ch };
    char* buf = NULL;
    for (int i = 0; i < 4 && !buf; ++i) {
        buf = read_text_file(paths[i], NULL);
    }
    if (!buf) return 0;

    int has_header = channel_in_header(buf, ch);
    int has_channel = channel_matches(buf, ch);
    if (!has_header && !has_channel) {
        free(buf);
        return 0;
    }

    double s = 0.0;
    double b = 0.0;
    int ts = DEFAULT_TSENSOR;
    int lpf = DEFAULT_ILPF;
    int gain_idx = DEFAULT_IGAIN_IDX;
    int senspwr_idx = DEFAULT_SENSPWR_IDX;

    int ok_s = parse_fit_value(buf, "\"slope\"", &s);
    int ok_b = parse_fit_value(buf, "\"intercept\"", &b);
    parse_int_value(buf, "\"tSensor\"", &ts);
    parse_int_value(buf, "\"iLPF\"", &lpf);
    if (parse_int_value(buf, "\"iGain\"", &gain_idx)) {
        if (gain_idx > 7) {
            int mapped = gain_index_from_value(gain_idx);
            if (mapped >= 0) gain_idx = mapped;
            else gain_idx = DEFAULT_IGAIN_IDX;
        }
    } else {
        gain_idx = DEFAULT_IGAIN_IDX;
    }
    parse_int_value(buf, "\"iSensPwr\"", &senspwr_idx);
    free(buf);
    if (!ok_s || !ok_b) return 0;

    *slope = s;
    *intercept = b;
    if (tSensor) *tSensor = ts;
    if (iLPF) *iLPF = lpf;
    if (iGainIdx) *iGainIdx = gain_idx;
    if (iSensPwr) *iSensPwr = senspwr_idx;
    return 1;
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
    printf("=== Teste de comunicacao ===\n");
    WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w)) { printf("Falha no WSAStartup\n"); return; }
    SOCKET s; struct sockaddr_in addr;
    if (open_udp(&s, &addr)) { printf("Falha ao abrir socket\n"); WSACleanup(); return; }

    stop_acq(s, &addr);
    configure_channel(s, &addr, 1, DEFAULT_TSENSOR, DEFAULT_ILPF, DEFAULT_IGAIN_IDX, DEFAULT_SENSPWR_IDX);
    acq_setup_ch1(s, &addr, IDX_FREQ);
    acq_start(s, &addr);

    printf("Aguardando primeiro dado ate %d ms (tentativas=%d)...\n", START_WAIT_MS, START_RETRIES);
    if (wait_first_packet(s, &addr) == 0) {
        printf("OK: dados recebidos.\n");
    } else {
        printf("FALHA: sem OP_ACQDATA dentro do tempo.\n");
        printf("Dicas: firewall IN localport=%d, NIC/rota, START_WAIT_MS.\n", (int)LOCAL_BIND_PORT);
    }

    stop_acq(s, &addr);
    closesocket(s);
    WSACleanup();
    printf("Concluido.\n\n");
}

static void do_monitor_1hz(void) {
    printf("=== Monitor 1 Hz (CH1) - pressione Q para parar ===\n");
    WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w)) { printf("Falha no WSAStartup\n"); return; }
    SOCKET s; struct sockaddr_in addr;
    if (open_udp(&s, &addr)) { printf("Falha ao abrir socket\n"); WSACleanup(); return; }

    double slope = 0.0;
    double intercept = 0.0;
    int tSensor = DEFAULT_TSENSOR;
    int iLPF = DEFAULT_ILPF;
    int iGainIdx = DEFAULT_IGAIN_IDX;
    int iSensPwr = DEFAULT_SENSPWR_IDX;
    int has_calib = load_calib_for_channel(1, &slope, &intercept, &tSensor, &iLPF, &iGainIdx, &iSensPwr);
    if (has_calib) {
        int gain_nom = gain_values[(iGainIdx >= 0 && iGainIdx < (int)(sizeof(gain_values) / sizeof(gain_values[0])))
                                   ? iGainIdx : DEFAULT_IGAIN_IDX];
        double vexc_nom = vexc_values[(iSensPwr >= 0 && iSensPwr < (int)(sizeof(vexc_values) / sizeof(vexc_values[0])))
                                      ? iSensPwr : DEFAULT_SENSPWR_IDX];
        printf("Calibracao carregada (CH1): slope=%.10g intercept=%.10g\n", slope, intercept);
        printf("Config do canal: tSensor=%d iGain=%d (%d) iLPF=%d iSensPwr=%d (%.1fV)\n",
               tSensor, iGainIdx, gain_nom, iLPF, iSensPwr, vexc_nom);
    } else {
        printf("Sem calibracao para CH1 (arquivo calib.json/calib nao encontrado ou sem CH1).\n");
    }

    stop_acq(s, &addr);
    configure_channel(s, &addr, 1, tSensor, iLPF, iGainIdx, iSensPwr);
    acq_setup_ch1(s, &addr, IDX_FREQ);
    acq_start(s, &addr);

    if (wait_first_packet(s, &addr) != 0) {
        printf("Sem dados para monitorar (timeout).\n");
        stop_acq(s, &addr); closesocket(s); WSACleanup(); return;
    }

    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LARGE_INTEGER t_win0; QueryPerformanceCounter(&t_win0);
    double sum = 0.0; int cnt = 0; long long lost = 0;
    int32_t prev_frame = -1;

    if (has_calib) {
        printf("Tempo(s)  cont  media_raw  media_cal  perdidos\n");
    } else {
        printf("Tempo(s)  cont  media_raw  perdidos\n");
    }
    for (;;) {
        if (_kbhit()) { int c = _getch(); if (c=='q'||c=='Q') break; }

        PktData pkt; int n = recvfrom(s, (char*)&pkt, sizeof(pkt), 0, NULL, NULL);
        if (n >= 4 && pkt.code == OP_ACQDATA) {
            if (prev_frame >= 0 && pkt.frame - prev_frame > 1) lost += (pkt.frame - prev_frame - 1);
            prev_frame = pkt.frame;
            int header_sz = (int)offsetof(PktData, smp);
            int avail = (n - header_sz) / (int)sizeof(pkt.smp[0]);
            if (avail > 0) {
                int stride = (pkt.nSig > 0) ? pkt.nSig : 1;
                if (avail > (int)(sizeof(pkt.smp) / sizeof(pkt.smp[0]))) {
                    avail = (int)(sizeof(pkt.smp) / sizeof(pkt.smp[0]));
                }
                for (int i = 0; i < avail; i += stride) { sum += (double)pkt.smp[i]; cnt++; }
            }
        }

        LARGE_INTEGER t_now; QueryPerformanceCounter(&t_now);
        double elapsed = (double)(t_now.QuadPart - t_win0.QuadPart) / (double)fq.QuadPart;
        if (elapsed >= 1.0) {
            double avg = (cnt > 0) ? (sum / (double)cnt) : 0.0;
            if (has_calib) {
                double avg_cal = slope * avg + intercept;
                printf("%7.3f  %5d  %7.2f  %7.2f  %4lld\n", elapsed, cnt, avg, avg_cal, lost);
            } else {
                printf("%7.3f  %5d  %7.2f  %4lld\n", elapsed, cnt, avg, lost);
            }
            t_win0 = t_now; sum = 0.0; cnt = 0;
        }
    }

    stop_acq(s, &addr);
    closesocket(s);
    WSACleanup();
    printf("Monitor encerrado.\n\n");
}

/* ---------- Main ---------- */
int main(void) {
    for (;;) {
        printf("==== DLG4000 Console ====\n");
        printf("[1] Teste de comunicacao\n");
        printf("[2] Monitor CH1 (print 1 Hz)\n");
        printf("[Q] Sair\n");
        printf("> ");
        fflush(stdout);

        int c = getchar();
        if (c == '\n' || c == '\r') continue;
        int d; while ((d = getchar()) != '\n' && d != '\r' && d != EOF) {}

        if (c == '1')       do_test_comm();
        else if (c == '2')  do_monitor_1hz();
        else if (c == 'q' || c == 'Q') break;
        else printf("Opcao invalida.\n\n");
    }
    return 0;
}
