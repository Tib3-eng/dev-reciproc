#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <direct.h>
#include <errno.h>
#include <stddef.h>

#pragma comment(lib, "ws2_32.lib")

/* ---------- Config ---------- */
static const char *DLG_IP = "192.168.1.100";
static const uint16_t DLG_PORT = 41401;

#define LOCAL_BIND_IP   ""
#define LOCAL_BIND_PORT 41402

#define IDX_FREQ     200.0f
#define BURSTS       1
#define NSIG         1
#define TIMEOUT_MS   1000
#define START_WAIT_MS 10000
#define START_RETRIES 5
#define CAPTURE_RCVTIMEO_MS 50
#define CAPTURE_NO_DATA_RESTART_MS 250
#define DRAIN_RCVTIMEO_MS 1

#define DEFAULT_IGAIN_IDX 4
#define DEFAULT_ILPF  0
#define DEFAULT_SENSPWR_IDX 0

static const int gain_values[] = { 1, 3, 10, 30, 100, 300, 1000, 3000 };
static const double vexc_values[] = { 1.0, 2.5, 3.3, 5.0 };

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
static void send_cmd(SOCKET s, const void *p, int len, const struct sockaddr_in *a) {
    sendto(s, (const char *)p, len, 0, (const struct sockaddr *)a, sizeof(*a));
}

static int open_udp(SOCKET *ps, struct sockaddr_in *addr) {
    *ps = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (*ps == INVALID_SOCKET) return -1;

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(LOCAL_BIND_PORT);
    if (LOCAL_BIND_IP[0] == '\0') {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, LOCAL_BIND_IP, &local.sin_addr) != 1) return -1;
    }
    if (bind(*ps, (const struct sockaddr *)&local, sizeof(local)) != 0) return -1;

    DWORD to = TIMEOUT_MS;
    setsockopt(*ps, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(DLG_PORT);
    if (inet_pton(AF_INET, DLG_IP, &addr->sin_addr) != 1) return -1;

    return 0;
}

static void stop_acq(SOCKET s, const struct sockaddr_in *addr) {
    PktHdr stop = { OP_ACQSTOP, 0 };
    send_cmd(s, &stop, sizeof(stop), addr);
}

static void configure_channel(SOCKET s, const struct sockaddr_in *addr, int ch, int tSensor,
                              int iLPF, int iGainIdx, int iSensPwr) {
    PktSetCh cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.code = OP_SETCHCFG;
    cfg.ch = (int16_t)ch;
    cfg.tSensor = (int16_t)tSensor;
    cfg.iGain = (int16_t)iGainIdx;
    cfg.iLPF = (int16_t)iLPF;
    cfg.iSensPwr = (int16_t)iSensPwr;
    send_cmd(s, &cfg, sizeof(cfg), addr);
}

static void acq_setup_single(SOCKET s, const struct sockaddr_in *addr, float freq_idx, int ch) {
    PktAcqSetup st;
    memset(&st, 0, sizeof(st));
    st.code = OP_ACQSETUP;
    st.f = freq_idx;
    st.bursts = BURSTS;
    st.nSig = NSIG;
    st.ICM[0] = (int16_t)ch;
    send_cmd(s, &st, sizeof(st), addr);
}

static void acq_start(SOCKET s, const struct sockaddr_in *addr) {
    PktHdr start = { OP_ACQSTART, 0 };
    send_cmd(s, &start, sizeof(start), addr);
}

static int test_comm(SOCKET s, const struct sockaddr_in *addr, int ch, int tSensor,
                     int iLPF, int iGainIdx, int iSensPwr) {
    stop_acq(s, addr);
    configure_channel(s, addr, ch, tSensor, iLPF, iGainIdx, iSensPwr);
    acq_setup_single(s, addr, IDX_FREQ, ch);
    acq_start(s, addr);

    printf("Teste de comunicacao: aguardando OP_ACQDATA ate %d ms...\n", START_WAIT_MS);
    if (wait_first_packet(s, addr, ch) != 0) {
        printf("FALHA: sem OP_ACQDATA. Verifique firewall, NIC e estado do DLG.\n");
        stop_acq(s, addr);
        return 0;
    }

    stop_acq(s, addr);
    printf("OK: dados recebidos do DLG.\n");
    return 1;
}

static int wait_first_packet(SOCKET s, const struct sockaddr_in *addr, int ch) {
    for (int attempt = 0; attempt < START_RETRIES; ++attempt) {
        DWORD t0 = GetTickCount();
        for (;;) {
            PktData pd;
            int n = recvfrom(s, (char *)&pd, sizeof(pd), 0, NULL, NULL);
            if (n >= 4 && pd.code == OP_ACQDATA) return 0;
            if ((GetTickCount() - t0) > (DWORD)START_WAIT_MS) break;
        }
        acq_setup_single(s, addr, IDX_FREQ, ch);
        acq_start(s, addr);
    }
    return -1;
}

static void drain_socket(SOCKET s) {
    DWORD prev_to = TIMEOUT_MS;
    DWORD drain_to = DRAIN_RCVTIMEO_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&drain_to, sizeof(drain_to));
    for (;;) {
        char tmp[256];
        int n = recvfrom(s, tmp, (int)sizeof(tmp), 0, NULL, NULL);
        if (n > 0) continue;
        if (n < 0 && WSAGetLastError() == WSAETIMEDOUT) break;
        break;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&prev_to, sizeof(prev_to));
}

static int restart_stream(SOCKET s, const struct sockaddr_in *addr, int ch) {
    acq_setup_single(s, addr, IDX_FREQ, ch);
    acq_start(s, addr);
    return wait_first_packet(s, addr, ch);
}

static int ensure_out_dir(void) {
    if (_mkdir("out") != 0 && errno != EEXIST) return -1;
    return 0;
}

static int read_line(char *buf, size_t size) {
    if (!fgets(buf, (int)size, stdin)) return 0;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return 1;
}

static int stdin_is_console(void) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || h == NULL) return 0;
    return GetConsoleMode(h, &mode) ? 1 : 0;
}

static int g_allocated_console = 0;

static int ensure_console(void) {
    if (stdin_is_console()) return 1;
    if (!AllocConsole()) return 0;
    g_allocated_console = 1;

    FILE *f = NULL;
    freopen_s(&f, "CONIN$", "r", stdin);
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    return stdin_is_console();
}

static void wait_before_exit(void) {
    if (stdin_is_console()) {
        printf("Pressione Enter para sair...\n");
        fflush(stdout);
        char tmp[8];
        fgets(tmp, (int)sizeof(tmp), stdin);
        return;
    }
    MessageBoxA(NULL,
                "CalibraDLG precisa ser executado via terminal (PowerShell/cmd).",
                "CalibraDLG",
                MB_OK | MB_ICONERROR);
}

static int exit_with_pause(int code) {
    wait_before_exit();
    return code;
}

static int ask_int(const char *prompt, int minv, int maxv, int *out) {
    char line[128];
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!read_line(line, sizeof(line))) return 0;
        char *end = NULL;
        long v = strtol(line, &end, 10);
        if (end != line && *end == '\0' && v >= minv && v <= maxv) {
            *out = (int)v;
            return 1;
        }
        printf("Entrada invalida. Faixa %d..%d\n", minv, maxv);
    }
}

static int ask_double(const char *prompt, double *out) {
    char line[128];
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!read_line(line, sizeof(line))) return 0;
        char *end = NULL;
        double v = strtod(line, &end);
        if (end != line && *end == '\0') {
            *out = v;
            return 1;
        }
        printf("Numero invalido.\n");
    }
}

static void print_sensor_menu(void) {
    printf("Tipo de sensor (tSensor):\n");
    printf(" 0 - tensao\n");
    printf(" 1 - corrente\n");
    printf(" 2 - ponte completa\n");
    printf(" 3 - ponte de quarto\n");
    printf(" 4 - ponte meia\n");
    printf(" 5 - ICP\n");
    printf(" 6 - termopar E\n");
    printf(" 7 - termopar J\n");
    printf(" 8 - termopar K\n");
    printf(" 9 - termopar T\n");
}

static void print_gain_menu(void) {
    printf("Ganho (iGain):\n");
    for (int i = 0; i < (int)(sizeof(gain_values) / sizeof(gain_values[0])); ++i) {
        printf(" %d - %d\n", i + 1, gain_values[i]);
    }
}

static void print_vexc_menu(void) {
    printf("Tensao de excitacao (iSensPwr):\n");
    for (int i = 0; i < (int)(sizeof(vexc_values) / sizeof(vexc_values[0])); ++i) {
        printf(" %d - %.1f V\n", i + 1, vexc_values[i]);
    }
}

static int capture_average_1s(SOCKET s, const struct sockaddr_in *addr, int ch,
                              double *avg, int *samples) {
    LARGE_INTEGER fq, t0, tnow;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    LARGE_INTEGER last_rx = t0;

    DWORD capture_to = CAPTURE_RCVTIMEO_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&capture_to, sizeof(capture_to));

    double sum = 0.0;
    int count = 0;

    for (;;) {
        PktData pkt;
        int n = recvfrom(s, (char *)&pkt, sizeof(pkt), 0, NULL, NULL);
        if (n >= 4 && pkt.code == OP_ACQDATA) {
            int header_sz = (int)offsetof(PktData, smp);
            int avail = (n - header_sz) / (int)sizeof(pkt.smp[0]);
            if (avail > 0) {
                int stride = (pkt.nSig > 0) ? pkt.nSig : 1;
                if (avail > (int)(sizeof(pkt.smp) / sizeof(pkt.smp[0]))) {
                    avail = (int)(sizeof(pkt.smp) / sizeof(pkt.smp[0]));
                }
                for (int i = 0; i < avail; i += stride) {
                    sum += (double)pkt.smp[i];
                    count++;
                }
                QueryPerformanceCounter(&last_rx);
            }
        }

        QueryPerformanceCounter(&tnow);
        double elapsed = (double)(tnow.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
        double no_data = (double)(tnow.QuadPart - last_rx.QuadPart) / (double)fq.QuadPart;
        if (no_data * 1000.0 >= (double)CAPTURE_NO_DATA_RESTART_MS) {
            acq_setup_single(s, addr, IDX_FREQ, ch);
            acq_start(s, addr);
            last_rx = tnow;
        }
        if (elapsed >= 1.0) break;
    }

    DWORD normal_to = TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&normal_to, sizeof(normal_to));
    if (count == 0) return 0;
    *avg = sum / (double)count;
    *samples = count;
    return 1;
}

static int write_json(const char *path, int ch, int tSensor, int iLPF,
                      int iGainIdx, int iSensPwr, double gain_nom, double vexc_nom,
                      int npoints, const double *raw, const double *ref,
                      double slope, double intercept, double r2) {
    if (ensure_out_dir() != 0) return 0;
    FILE *f = fopen(path, "w");
    if (!f) return 0;

    fprintf(f, "{\n");
    fprintf(f, "  \"channels\": [%d],\n", ch);
    fprintf(f, "  \"channel\": \"CH%d\",\n", ch);
    fprintf(f, "  \"tSensor\": %d,\n", tSensor);
    fprintf(f, "  \"iGain\": %d,\n", iGainIdx);
    fprintf(f, "  \"gain_nom\": %.10g,\n", gain_nom);
    fprintf(f, "  \"iLPF\": %d,\n", iLPF);
    fprintf(f, "  \"iSensPwr\": %d,\n", iSensPwr);
    fprintf(f, "  \"vexc_nom\": %.10g,\n", vexc_nom);
    fprintf(f, "  \"avg_window_s\": 1.0,\n");
    fprintf(f, "  \"points\": [\n");
    for (int i = 0; i < npoints; ++i) {
        fprintf(f, "    {\"raw\": %.10g, \"ref\": %.10g}%s\n",
                raw[i], ref[i], (i + 1 < npoints) ? "," : "");
    }
    fprintf(f, "  ],\n");
    fprintf(f, "  \"fit\": {\n");
    fprintf(f, "    \"slope\": %.10g,\n", slope);
    fprintf(f, "    \"intercept\": %.10g,\n", intercept);
    fprintf(f, "    \"r2\": %.6f\n", r2);
    fprintf(f, "  }\n");
    fprintf(f, "}\n");

    fclose(f);
    return 1;
}

int main(void) {
    if (!ensure_console()) {
        MessageBoxA(NULL,
                    "Nao foi possivel abrir o console.\n"
                    "Execute via PowerShell ou cmd.",
                    "CalibraDLG",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    printf("==== CalibraDLG ====\n");
    printf("DLG IP: %s  UDP: %u\n\n", DLG_IP, (unsigned)DLG_PORT);

    int ch = 0;
    int tSensor = 0;
    int npoints = 0;
    int iLPF = DEFAULT_ILPF;
    int iGainIdx = DEFAULT_IGAIN_IDX;
    int iSensPwr = DEFAULT_SENSPWR_IDX;
    double gain_nom = gain_values[DEFAULT_IGAIN_IDX];
    double vexc_nom = vexc_values[DEFAULT_SENSPWR_IDX];

    if (!ask_int("Qual canal deseja calibrar (1-8): ", 1, 8, &ch)) return exit_with_pause(1);
    print_sensor_menu();
    if (!ask_int("Selecione o tipo de sensor (0-9): ", 0, 9, &tSensor)) return exit_with_pause(1);
    print_gain_menu();
    int opt_gain = 0;
    if (!ask_int("Escolha o ganho (1-8): ", 1, 8, &opt_gain)) return exit_with_pause(1);
    iGainIdx = opt_gain - 1;
    gain_nom = gain_values[iGainIdx];
    printf("Filtro (iLPF):\n");
    printf(" 1 - Default (%d)\n", DEFAULT_ILPF);
    printf(" 2 - 0 (off)\n");
    printf(" 3 - 1\n");
    printf(" 4 - 2\n");
    printf(" 5 - 3\n");
    int opt_lpf = 1;
    if (!ask_int("Escolha o filtro (1-5): ", 1, 5, &opt_lpf)) return exit_with_pause(1);
    switch (opt_lpf) {
        case 1: iLPF = DEFAULT_ILPF; break;
        case 2: iLPF = 0; break;
        case 3: iLPF = 1; break;
        case 4: iLPF = 2; break;
        case 5: iLPF = 3; break;
        default: iLPF = DEFAULT_ILPF; break;
    }
    print_vexc_menu();
    int opt_vexc = 0;
    if (!ask_int("Escolha a tensao de excitacao (1-4): ", 1, 4, &opt_vexc)) return exit_with_pause(1);
    iSensPwr = opt_vexc - 1;
    vexc_nom = vexc_values[iSensPwr];
    if (!ask_int("Quantos pontos de calibracao (2-20): ", 2, 20, &npoints)) return exit_with_pause(1);

    double *raw = (double *)calloc((size_t)npoints, sizeof(double));
    double *ref = (double *)calloc((size_t)npoints, sizeof(double));
    if (!raw || !ref) {
        printf("Memoria insuficiente.\n");
        return exit_with_pause(1);
    }

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) {
        printf("Falha no WSAStartup.\n");
        return exit_with_pause(1);
    }
    SOCKET s;
    struct sockaddr_in addr;
    if (open_udp(&s, &addr) != 0) {
        printf("Falha ao abrir socket.\n");
        WSACleanup();
        return exit_with_pause(1);
    }

    if (!test_comm(s, &addr, ch, tSensor, iLPF, iGainIdx, iSensPwr)) {
        closesocket(s);
        WSACleanup();
        return exit_with_pause(1);
    }

    printf("Inicio da calibracao. Cada ponto usa media de 1 s.\n");
    for (int i = 0; i < npoints; ++i) {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "Valor de referencia do ponto %d: ", i + 1);
        if (!ask_double(prompt, &ref[i])) {
            printf("Entrada cancelada.\n");
            stop_acq(s, &addr);
            closesocket(s);
            WSACleanup();
            return exit_with_pause(1);
        }

        stop_acq(s, &addr);
        acq_setup_single(s, &addr, IDX_FREQ, ch);
        acq_start(s, &addr);

        drain_socket(s);
        if (restart_stream(s, &addr, ch) != 0) {
            printf("Sem dados para o ponto %d (stream inativo).\n", i + 1);
            stop_acq(s, &addr);
            closesocket(s);
            WSACleanup();
            return exit_with_pause(1);
        }

        double avg = 0.0;
        int samples = 0;
        if (!capture_average_1s(s, &addr, ch, &avg, &samples)) {
            printf("Sem amostras no ponto %d.\n", i + 1);
            stop_acq(s, &addr);
            closesocket(s);
            WSACleanup();
            return exit_with_pause(1);
        }
        raw[i] = avg;
        printf("  raw_avg=%.6f (samples=%d)\n", avg, samples);
        stop_acq(s, &addr);
    }
    closesocket(s);
    WSACleanup();

    double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0, sum_yy = 0.0;
    for (int i = 0; i < npoints; ++i) {
        sum_x += raw[i];
        sum_y += ref[i];
        sum_xx += raw[i] * raw[i];
        sum_xy += raw[i] * ref[i];
        sum_yy += ref[i] * ref[i];
    }

    double n = (double)npoints;
    double denom = n * sum_xx - sum_x * sum_x;
    double slope = 0.0;
    double intercept = 0.0;
    if (fabs(denom) > 1e-12) {
        slope = (n * sum_xy - sum_x * sum_y) / denom;
        intercept = (sum_y - slope * sum_x) / n;
    }
    double ss_tot = sum_yy - (sum_y * sum_y) / n;
    double ss_reg = slope * (sum_xy - sum_x * sum_y / n);
    double r2 = 0.0;
    if (ss_tot > 0.0) {
        r2 = ss_reg / ss_tot;
        if (r2 < 0.0) r2 = 0.0;
        if (r2 > 1.0) r2 = 1.0;
    }

    printf("\nAjuste: y = slope * raw + intercept\n");
    printf("slope=%.10g  intercept=%.10g  r2=%.6f\n", slope, intercept, r2);

    char out_path[128];
    snprintf(out_path, sizeof(out_path), "out/calib.json");
    if (!write_json(out_path, ch, tSensor, iLPF, iGainIdx, iSensPwr, gain_nom, vexc_nom,
                    npoints, raw, ref, slope, intercept, r2)) {
        printf("Falha ao gravar: %s\n", out_path);
        free(raw);
        free(ref);
        return exit_with_pause(1);
    }

    printf("Saida: %s\n", out_path);
    free(raw);
    free(ref);
    if (g_allocated_console) wait_before_exit();
    return 0;
}
