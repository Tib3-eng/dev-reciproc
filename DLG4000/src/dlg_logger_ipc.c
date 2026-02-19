/* dlg_logger_ipc.c
 * Headless DLG4000 logger (8 channels) -> CSV, 200 Hz target.
 * - Waits for START on stdin when --ipc is used.
 * - Logs fixed number of rows (duration * rate), inserts NULL on missing samples.
 * - Uses QPC for local timebase (t_qpc/t_s).
 *
 * NOTE: This does NOT change DLGlogger.c (reference tool).
 *       It is a dedicated headless logger for the supervisor pipeline.
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
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>

#pragma comment(lib, "ws2_32.lib")

/* ---------- Defaults ---------- */
static const char *DLG_IP_DEFAULT = "192.168.1.100";
static const uint16_t DLG_PORT_DEFAULT = 41401;
#define LOCAL_BIND_IP_DEFAULT   ""     /* "" = 0.0.0.0 */
#define LOCAL_BIND_PORT_DEFAULT 41402

#define DEFAULT_RATE_HZ 200.0
#define DEFAULT_DURATION_S 10.0
#define DEFAULT_CHANNELS 8
#define WAIT_FIRST_MS 3000
#define WAIT_FIRST_SAMPLES 3

/* Default channel config (same as DLGlogger reference) */
#define DEFAULT_TSENSOR     4
#define DEFAULT_ILPF        0
#define DEFAULT_IGAIN_IDX   5
#define DEFAULT_SENSPWR_IDX 2

/* Gain / excitation tables (same as DLGlogger reference) */
static const int gain_values[] = { 1, 3, 10, 30, 100, 300, 1000, 3000 };

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

/* ---------- Ring buffer ---------- */
typedef struct { int16_t ch[DEFAULT_CHANNELS]; } sample_t;
typedef struct {
    sample_t *buf;
    int cap;
    int head;
    int tail;
    int count;
    int overrun;
} ring_t;

static void ring_init(ring_t *r, int cap){
    r->buf = (sample_t*)calloc((size_t)cap, sizeof(sample_t));
    r->cap = cap;
    r->head = r->tail = r->count = 0;
    r->overrun = 0;
}
static void ring_free(ring_t *r){
    free(r->buf);
    r->buf = NULL;
    r->cap = r->head = r->tail = r->count = 0;
}
static void ring_push(ring_t *r, const sample_t *s){
    if(r->count >= r->cap){
        r->overrun++;
        return; /* drop newest */
    }
    r->buf[r->head] = *s;
    r->head = (r->head + 1) % r->cap;
    r->count++;
}
static int ring_pop(ring_t *r, sample_t *out){
    if(r->count <= 0) return 0;
    *out = r->buf[r->tail];
    r->tail = (r->tail + 1) % r->cap;
    r->count--;
    return 1;
}
static void ring_clear(ring_t *r){
    r->head = r->tail = r->count = 0;
}

/* ---------- Utils ---------- */
static char* read_text_file(const char* path, size_t* out_sz){
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;
    if(fseek(f, 0, SEEK_END) != 0){ fclose(f); return NULL; }
    long sz = ftell(f);
    if(sz < 0){ fclose(f); return NULL; }
    if(fseek(f, 0, SEEK_SET) != 0){ fclose(f); return NULL; }

    char* buf = (char*)malloc((size_t)sz + 1);
    if(!buf){ fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if(out_sz) *out_sz = n;
    return buf;
}

static int channel_in_header(const char* buf, int ch){
    const char* p = strstr(buf, "\"channels\"");
    if(!p) return 0;
    p = strchr(p, '[');
    if(!p) return 0;
    p++;
    while(*p && *p != ']'){
        while(*p && !isdigit((unsigned char)*p) && *p != '-' && *p != ']') p++;
        if(!*p || *p == ']') break;
        char* end = NULL;
        long v = strtol(p, &end, 10);
        if(end != p){
            if((int)v == ch) return 1;
            p = end;
        }else{
            p++;
        }
    }
    return 0;
}

static int channel_matches(const char* buf, int ch){
    char needle[32];
    _snprintf(needle, sizeof(needle), "\"channel\": \"CH%d\"", ch);
    return strstr(buf, needle) != NULL;
}

static int parse_fit_value(const char* buf, const char* key, double* out){
    const char* p = strstr(buf, key);
    if(!p) return 0;
    p = strchr(p, ':');
    if(!p) return 0;
    p++;
    while(*p && isspace((unsigned char)*p)) p++;
    char* end = NULL;
    double v = strtod(p, &end);
    if(end == p) return 0;
    *out = v;
    return 1;
}

static int parse_int_value(const char* buf, const char* key, int* out){
    const char* p = strstr(buf, key);
    if(!p) return 0;
    p = strchr(p, ':');
    if(!p) return 0;
    p++;
    while(*p && isspace((unsigned char)*p)) p++;
    char* end = NULL;
    long v = strtol(p, &end, 10);
    if(end == p) return 0;
    *out = (int)v;
    return 1;
}

static int gain_index_from_value(int value){
    for(int i = 0; i < (int)(sizeof(gain_values) / sizeof(gain_values[0])); ++i){
        if(gain_values[i] == value) return i;
    }
    return -1;
}

static int get_exe_dir(char *out, size_t out_sz){
    if(!out || out_sz == 0) return 0;
    out[0] = '\0';
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_sz);
    if(n == 0 || n >= out_sz) return 0;
    for(size_t i = n; i > 0; --i){
        if(out[i-1] == '\\' || out[i-1] == '/'){
            out[i-1] = '\0';
            return 1;
        }
    }
    out[0] = '\0';
    return 0;
}

static int load_calib_for_channel(int ch, double* slope, double* intercept,
                                  int* tSensor, int* iLPF, int* iGainIdx, int* iSensPwr){
    char path_ch[64];
    char path_out_ch[80];
    char exe_dir[MAX_PATH];
    char path_exe_calib[256];
    char path_exe_calib2[256];
    char path_exe_ch[256];
    char path_exe_out_ch[256];
    const char* paths[8];
    int npaths = 0;

    _snprintf(path_ch, sizeof(path_ch), "calib_CH%d.json", ch);
    _snprintf(path_out_ch, sizeof(path_out_ch), "out\\calib_CH%d.json", ch);
    paths[npaths++] = "calib.json";
    paths[npaths++] = "calib";
    paths[npaths++] = path_ch;
    paths[npaths++] = path_out_ch;

    if(get_exe_dir(exe_dir, sizeof(exe_dir))){
        _snprintf(path_exe_calib, sizeof(path_exe_calib), "%s\\calib.json", exe_dir);
        _snprintf(path_exe_calib2, sizeof(path_exe_calib2), "%s\\calib", exe_dir);
        _snprintf(path_exe_ch, sizeof(path_exe_ch), "%s\\calib_CH%d.json", exe_dir, ch);
        _snprintf(path_exe_out_ch, sizeof(path_exe_out_ch), "%s\\out\\calib_CH%d.json", exe_dir, ch);
        paths[npaths++] = path_exe_calib;
        paths[npaths++] = path_exe_calib2;
        paths[npaths++] = path_exe_ch;
        paths[npaths++] = path_exe_out_ch;
    }
    char* buf = NULL;
    for(int i = 0; i < npaths && !buf; ++i){
        buf = read_text_file(paths[i], NULL);
    }
    if(!buf) return 0;

    int has_header = channel_in_header(buf, ch);
    int has_channel = channel_matches(buf, ch);
    if(!has_header && !has_channel){
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
    if(parse_int_value(buf, "\"iGain\"", &gain_idx)){
        if(gain_idx > 7){
            int mapped = gain_index_from_value(gain_idx);
            if(mapped >= 0) gain_idx = mapped;
            else gain_idx = DEFAULT_IGAIN_IDX;
        }
    }else{
        gain_idx = DEFAULT_IGAIN_IDX;
    }
    parse_int_value(buf, "\"iSensPwr\"", &senspwr_idx);
    free(buf);
    if(!ok_s || !ok_b) return 0;

    *slope = s;
    *intercept = b;
    if(tSensor) *tSensor = ts;
    if(iLPF) *iLPF = lpf;
    if(iGainIdx) *iGainIdx = gain_idx;
    if(iSensPwr) *iSensPwr = senspwr_idx;
    return 1;
}

static int ensure_out_dir_for_path(const char *path){
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if(len >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    for(size_t i = 0; i < len; ++i){
        if(tmp[i] == '\\' || tmp[i] == '/'){
            char c = tmp[i];
            tmp[i] = '\0';
            if(tmp[0]){
                CreateDirectoryA(tmp, NULL);
            }
            tmp[i] = c;
        }
    }
    return 0;
}

static void send_cmd(SOCKET s, const void *p, int len, const struct sockaddr_in *a){
    sendto(s, (const char*)p, len, 0, (const struct sockaddr*)a, sizeof(*a));
}

static int open_udp(SOCKET *ps, struct sockaddr_in *addr,
                    const char *dlg_ip, uint16_t dlg_port,
                    const char *bind_ip, uint16_t bind_port){
    *ps = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(*ps == INVALID_SOCKET) return -1;

    struct sockaddr_in local; memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port   = htons(bind_port);
    if(bind_ip && bind_ip[0]){
        if(inet_pton(AF_INET, bind_ip, &local.sin_addr) != 1) return -1;
    }else{
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if(bind(*ps, (const struct sockaddr*)&local, sizeof(local)) != 0) return -1;

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(dlg_port);
    if(inet_pton(AF_INET, dlg_ip, &addr->sin_addr) != 1) return -1;
    return 0;
}

static void stop_acq(SOCKET s, const struct sockaddr_in *addr){
    PktHdr stop = { OP_ACQSTOP, 0 };
    send_cmd(s, &stop, sizeof(stop), addr);
}

static void configure_channel(SOCKET s, const struct sockaddr_in *addr,
                              int ch, int tSensor, int iLPF, int iGainIdx, int iSensPwr){
    PktSetCh cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.code = OP_SETCHCFG;
    cfg.ch = (int16_t)ch;
    cfg.tSensor = (int16_t)tSensor;
    cfg.iGain = (int16_t)iGainIdx;
    cfg.iLPF = (int16_t)iLPF;
    cfg.iSensPwr = (int16_t)iSensPwr;
    send_cmd(s, &cfg, sizeof(cfg), addr);
}

static void acq_setup_multi(SOCKET s, const struct sockaddr_in *addr, float freq_idx, int nSig){
    PktAcqSetup st; memset(&st, 0, sizeof(st));
    st.code = OP_ACQSETUP;
    st.f = freq_idx;
    st.bursts = 1;
    st.nSig = (int16_t)nSig;
    for(int i = 0; i < nSig && i < 8; ++i){
        st.ICM[i] = (int16_t)(i + 1); /* CH1..CH8 */
    }
    send_cmd(s, &st, sizeof(st), addr);
}

static void acq_start(SOCKET s, const struct sockaddr_in *addr){
    PktHdr start = { OP_ACQSTART, 0 };
    send_cmd(s, &start, sizeof(start), addr);
}

static int64_t qpc_now_ticks(void){
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)c.QuadPart;
}

static int64_t qpc_freq_ticks(void){
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (int64_t)f.QuadPart;
}

static int drain_packets_to_ring(SOCKET s, ring_t *ring, int nSig){
    int got = 0;
    for(;;){
        PktData pkt;
        int n = recvfrom(s, (char*)&pkt, sizeof(pkt), 0, NULL, NULL);
        if(n < 0){
            int err = WSAGetLastError();
            if(err == WSAEWOULDBLOCK) break;
            break;
        }
        if(n >= 4 && pkt.code == OP_ACQDATA){
            int header_sz = (int)offsetof(PktData, smp);
            int avail = (n - header_sz) / (int)sizeof(pkt.smp[0]);
            if(avail <= 0) continue;
            int stride = (pkt.nSig > 0) ? pkt.nSig : nSig;
            int samples_per_ch = avail / stride;
            if(samples_per_ch <= 0) continue;
            got = 1;
            for(int sidx = 0; sidx < samples_per_ch; ++sidx){
                sample_t sm;
                for(int ch = 0; ch < DEFAULT_CHANNELS; ++ch){
                    int pos = sidx * stride + ch;
                    if(pos < avail) sm.ch[ch] = pkt.smp[pos];
                    else sm.ch[ch] = 0;
                }
                ring_push(ring, &sm);
            }
        }
    }
    return got;
}

static int wait_first_samples(SOCKET s, ring_t *ring, int nSig, int min_samples,
                              int timeout_ms, int64_t *t_ready){
    DWORD t0 = GetTickCount();
    while((GetTickCount() - t0) < (DWORD)timeout_ms){
        (void)drain_packets_to_ring(s, ring, nSig);
        if(ring->count >= min_samples){
            if(t_ready) *t_ready = qpc_now_ticks();
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

static void trim(char *s){
    size_t n = strlen(s);
    while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
    char *p = s; while(*p && isspace((unsigned char)*p)) p++;
    if(p != s) memmove(s, p, strlen(p)+1);
}

static void print_usage(void){
    puts("Uso:");
    puts("  dlg_logger_ipc --out <csv> --duration <s> [--rate <hz>] [--ipc]");
    puts("                [--ip <dlg_ip>] [--port <dlg_port>]");
    puts("                [--bind-ip <ip>] [--bind-port <port>]");
}

int main(int argc, char **argv){
    const char *out_path = NULL;
    const char *dlg_ip = DLG_IP_DEFAULT;
    const char *bind_ip = LOCAL_BIND_IP_DEFAULT;
    uint16_t dlg_port = DLG_PORT_DEFAULT;
    uint16_t bind_port = LOCAL_BIND_PORT_DEFAULT;
    double rate_hz = DEFAULT_RATE_HZ;
    double duration_s = DEFAULT_DURATION_S;
    int use_ipc = 0;

    for(int i = 1; i < argc; ++i){
        if(strcmp(argv[i], "--out") == 0 && i + 1 < argc){ out_path = argv[++i]; continue; }
        if(strcmp(argv[i], "--duration") == 0 && i + 1 < argc){ duration_s = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--rate") == 0 && i + 1 < argc){ rate_hz = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--ip") == 0 && i + 1 < argc){ dlg_ip = argv[++i]; continue; }
        if(strcmp(argv[i], "--port") == 0 && i + 1 < argc){ dlg_port = (uint16_t)atoi(argv[++i]); continue; }
        if(strcmp(argv[i], "--bind-ip") == 0 && i + 1 < argc){ bind_ip = argv[++i]; continue; }
        if(strcmp(argv[i], "--bind-port") == 0 && i + 1 < argc){ bind_port = (uint16_t)atoi(argv[++i]); continue; }
        if(strcmp(argv[i], "--ipc") == 0){ use_ipc = 1; continue; }
        print_usage();
        return 1;
    }
    if(!out_path || duration_s <= 0 || rate_hz <= 0){
        print_usage();
        return 1;
    }

    ensure_out_dir_for_path(out_path);
    FILE *f = fopen(out_path, "w");
    if(!f){
        fprintf(stderr, "Falha abrindo CSV: %s\n", out_path);
        return 1;
    }
    fprintf(f, "idx,t_qpc,t_s,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,err\n");
    fflush(f);

    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2), &wsa) != 0){
        fprintf(stderr, "WSAStartup falhou.\n");
        fclose(f);
        return 1;
    }

    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in addr;
    if(open_udp(&s, &addr, dlg_ip, dlg_port, bind_ip, bind_port) != 0){
        fprintf(stderr, "Falha ao abrir UDP.\n");
        WSACleanup();
        fclose(f);
        return 1;
    }

    /* Non-blocking socket to drain packets quickly */
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    /* Calibracao por canal (quando existir) */
    double slope[DEFAULT_CHANNELS];
    double intercept[DEFAULT_CHANNELS];
    int tSensor[DEFAULT_CHANNELS];
    int iLPF[DEFAULT_CHANNELS];
    int iGainIdx[DEFAULT_CHANNELS];
    int iSensPwr[DEFAULT_CHANNELS];
    int has_calib[DEFAULT_CHANNELS];
    for(int ch = 0; ch < DEFAULT_CHANNELS; ++ch){
        slope[ch] = 1.0;
        intercept[ch] = 0.0;
        tSensor[ch] = DEFAULT_TSENSOR;
        iLPF[ch] = DEFAULT_ILPF;
        iGainIdx[ch] = DEFAULT_IGAIN_IDX;
        iSensPwr[ch] = DEFAULT_SENSPWR_IDX;
        has_calib[ch] = 0;

        double s = 0.0, b = 0.0;
        int ts = DEFAULT_TSENSOR, lpf = DEFAULT_ILPF, g = DEFAULT_IGAIN_IDX, pwr = DEFAULT_SENSPWR_IDX;
        if(load_calib_for_channel(ch + 1, &s, &b, &ts, &lpf, &g, &pwr)){
            slope[ch] = s;
            intercept[ch] = b;
            tSensor[ch] = ts;
            iLPF[ch] = lpf;
            iGainIdx[ch] = g;
            iSensPwr[ch] = pwr;
            has_calib[ch] = 1;
        }
    }

    stop_acq(s, &addr);
    for(int ch = 1; ch <= DEFAULT_CHANNELS; ++ch){
        int idx = ch - 1;
        configure_channel(s, &addr, ch,
                          tSensor[idx], iLPF[idx], iGainIdx[idx], iSensPwr[idx]);
    }
    acq_setup_multi(s, &addr, (float)rate_hz, DEFAULT_CHANNELS);
    acq_start(s, &addr);

    if(use_ipc){
        puts("READY");
        fflush(stdout);
        char line[64];
        if(!fgets(line, sizeof(line), stdin)){
            fclose(f);
            closesocket(s);
            WSACleanup();
            return 1;
        }
        trim(line);
        if(_stricmp(line, "START") != 0){
            fclose(f);
            closesocket(s);
            WSACleanup();
            return 1;
        }
    }

    int total_samples = (int)(duration_s * rate_hz + 0.5);
    int64_t qpc_freq = qpc_freq_ticks();
    int64_t dt_ticks = (int64_t)((double)qpc_freq / rate_hz + 0.5);
    int64_t start_ticks = 0;
    int64_t next_ticks = 0;

    ring_t ring;
    ring_init(&ring, 8192);

    /* Gatilho de inicio: espera N amostras antes de iniciar o tempo. */
    int64_t t_first = 0;
    int got_first = wait_first_samples(s, &ring, DEFAULT_CHANNELS,
                                       WAIT_FIRST_SAMPLES, WAIT_FIRST_MS, &t_first);
    if(got_first){
        start_ticks = t_first;
    }else{
        start_ticks = qpc_now_ticks();
    }
    next_ticks = start_ticks;
    /* Descarta amostras coletadas antes do inicio efetivo. */
    ring_clear(&ring);

    if(use_ipc){
        if(got_first) puts("DATA_OK");
        else puts("DATA_TIMEOUT");
        fflush(stdout);
    }

    int idx = 0;
    while(idx < total_samples){
        /* Drain all available packets */
        (void)drain_packets_to_ring(s, &ring, DEFAULT_CHANNELS);

        int64_t now = qpc_now_ticks();
        if(now >= next_ticks){
            sample_t sm;
            int have = ring_pop(&ring, &sm);
            double t_s = (double)idx / rate_hz;
            int64_t t_qpc = start_ticks + (int64_t)idx * dt_ticks;

            if(have){
                char ch_buf[DEFAULT_CHANNELS][32];
                for(int ch = 0; ch < DEFAULT_CHANNELS; ++ch){
                    if(has_calib[ch]){
                        double v = slope[ch] * (double)sm.ch[ch] + intercept[ch];
                        _snprintf(ch_buf[ch], sizeof(ch_buf[ch]), "%.6f", v);
                    }else{
                        _snprintf(ch_buf[ch], sizeof(ch_buf[ch]), "%d", (int)sm.ch[ch]);
                    }
                }
                fprintf(f, "%d,%lld,%.6f,%s,%s,%s,%s,%s,%s,%s,%s,0\n",
                        idx, (long long)t_qpc, t_s,
                        ch_buf[0], ch_buf[1], ch_buf[2], ch_buf[3],
                        ch_buf[4], ch_buf[5], ch_buf[6], ch_buf[7]);
            }else{
                fprintf(f, "%d,%lld,%.6f,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,1\n",
                        idx, (long long)t_qpc, t_s);
            }
            idx++;
            next_ticks += dt_ticks;
        }else{
            Sleep(1);
        }
    }

    stop_acq(s, &addr);
    ring_free(&ring);
    closesocket(s);
    WSACleanup();
    fclose(f);
    return 0;
}
