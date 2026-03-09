/*
dlg_logger_ipc.c
----------------
Logger DLG4000 sem UI, usado pelo pipeline externo do supervisorio.

Objetivo geral:
- Capturar dados de ate 8 canais do DLG e gravar CSV em taxa fixa.
- Operar por IPC de linha unica (stdin/stdout) para integrar com Python.
- Entregar comportamento previsivel para ensaio automatizado (inclusive perdas).

Fluxo principal:
1) Parse de argumentos (saida, taxa, duracao, rede e modo IPC).
2) Setup do DLG (SETCH/SETUP/START) e sincronizacao do primeiro pacote valido.
3) Loop de captura por slots de tempo (QPC), com preenchimento de NULL quando faltar dado.
4) Tratamento de comandos IPC (START/PAUSE/RESUME/STOP) durante a execucao.
5) Encerramento com ACQSTOP e fechamento limpo dos recursos.

Variaveis/configuracoes principais:
- DLG_IP_DEFAULT/DLG_PORT_DEFAULT: destino UDP padrao do hardware.
- LOCAL_BIND_*: origem local de recepcao dos pacotes ACQDATA.
- DEFAULT_RATE_HZ/DEFAULT_DURATION_S/DEFAULT_CHANNELS: perfil padrao de log.
- WAIT_FIRST_MS/WAIT_FIRST_SAMPLES: criterios para declarar DATA_OK.
- DEFAULT_* de canal: fallback quando nao existe calibracao externa.

Resumo de funcoes:
- ring_*: buffer circular usado para desacoplar recepcao UDP do loop de slots.
- read_text_file/channel_* /parse_*: leitura e parse de calibracao por canal.
- gain_index_from_value: converte ganho nominal para indice valido no protocolo.
- get_exe_dir/ensure_out_dir_for_path: utilitarios de caminho para saida de arquivos.
- send_cmd/stop_acq/acq_setup_multi/acq_start: comandos de protocolo DLG.
- qpc_now_ticks/qpc_freq_ticks: base de tempo de alta resolucao no Windows.
- drain_packets_to_ring: coleta pacotes da rede e coloca no buffer circular.
- trim/ipc_poll_command: parse de comandos textuais via stdin.
- print_usage: ajuda de uso em linha de comando.
- main: orquestra todo o ciclo de vida do logger IPC.
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
#include <share.h>
#include <stdarg.h>

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

/*
Funcao: ring_init
Objetivo: Opera o buffer circular de amostras em memoria.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ring_init(ring_t *r, int cap){
    r->buf = (sample_t*)calloc((size_t)cap, sizeof(sample_t));
    r->cap = cap;
    r->head = r->tail = r->count = 0;
    r->overrun = 0;
}
/*
Funcao: ring_free
Objetivo: Opera o buffer circular de amostras em memoria.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ring_free(ring_t *r){
    free(r->buf);
    r->buf = NULL;
    r->cap = r->head = r->tail = r->count = 0;
}
/*
Funcao: ring_push
Objetivo: Opera o buffer circular de amostras em memoria.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ring_push(ring_t *r, const sample_t *s){
    if(r->count >= r->cap){
        r->overrun++;
        return; /* drop newest */
    }
    r->buf[r->head] = *s;
    r->head = (r->head + 1) % r->cap;
    r->count++;
}
/*
Funcao: ring_pop
Objetivo: Opera o buffer circular de amostras em memoria.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int ring_pop(ring_t *r, sample_t *out){
    if(r->count <= 0) return 0;
    *out = r->buf[r->tail];
    r->tail = (r->tail + 1) % r->cap;
    r->count--;
    return 1;
}
/*
Funcao: ring_clear
Objetivo: Opera o buffer circular de amostras em memoria.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ring_clear(ring_t *r){
    r->head = r->tail = r->count = 0;
}

static FILE *g_ev = NULL;

/*
Funcao: ev_close
Objetivo: Gerencia trilha de log de eventos do processo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ev_close(void){
    if(g_ev){
        fclose(g_ev);
        g_ev = NULL;
    }
}

/*
Funcao: ev_open_for_out
Objetivo: Gerencia trilha de log de eventos do processo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ev_open_for_out(const char *out_path){
    char ev_path[1024];
    const char *s1 = strrchr(out_path, '\\');
    const char *s2 = strrchr(out_path, '/');
    const char *s = (s1 && s2) ? ((s1 > s2) ? s1 : s2) : (s1 ? s1 : s2);
    if(s){
        size_t dlen = (size_t)(s - out_path);
        if(dlen > sizeof(ev_path) - 64) dlen = sizeof(ev_path) - 64;
        memcpy(ev_path, out_path, dlen);
        ev_path[dlen] = 0;
        strcat(ev_path, "\\dlg_logger_events.log");
    }else{
        _snprintf(ev_path, sizeof(ev_path), "dlg_logger_events.log");
    }
    g_ev = _fsopen(ev_path, "w", _SH_DENYNO);
}

/*
Funcao: ev_logf
Objetivo: Gerencia trilha de log de eventos do processo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ev_logf(const char *fmt, ...){
    if(!g_ev) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_ev, "[%02u:%02u:%02u.%03u] ",
            (unsigned)st.wHour, (unsigned)st.wMinute,
            (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_ev, fmt, ap);
    va_end(ap);
    fputc('\n', g_ev);
    fflush(g_ev);
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

/*
Funcao: channel_in_header
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: channel_matches
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int channel_matches(const char* buf, int ch){
    char needle[32];
    _snprintf(needle, sizeof(needle), "\"channel\": \"CH%d\"", ch);
    return strstr(buf, needle) != NULL;
}

/*
Funcao: parse_fit_value
Objetivo: Faz parse/validacao de entrada de dados.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: parse_int_value
Objetivo: Faz parse/validacao de entrada de dados.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: gain_index_from_value
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int gain_index_from_value(int value){
    for(int i = 0; i < (int)(sizeof(gain_values) / sizeof(gain_values[0])); ++i){
        if(gain_values[i] == value) return i;
    }
    return -1;
}

/*
Funcao: get_exe_dir
Objetivo: Resolve configuracao/caminho/estado auxiliar do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: ensure_out_dir_for_path
Objetivo: Resolve configuracao/caminho/estado auxiliar do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: send_cmd
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: stop_acq
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: acq_setup_multi
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: acq_start
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void acq_start(SOCKET s, const struct sockaddr_in *addr){
    PktHdr start = { OP_ACQSTART, 0 };
    send_cmd(s, &start, sizeof(start), addr);
}

/*
Funcao: qpc_now_ticks
Objetivo: Fornece base de tempo para sincronizacao do loop.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int64_t qpc_now_ticks(void){
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)c.QuadPart;
}

/*
Funcao: qpc_freq_ticks
Objetivo: Fornece base de tempo para sincronizacao do loop.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int64_t qpc_freq_ticks(void){
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (int64_t)f.QuadPart;
}

/*
Funcao: drain_packets_to_ring
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: trim
Objetivo: Normaliza texto/entrada para evitar inconsistencias.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void trim(char *s){
    size_t n = strlen(s);
    while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
    char *p = s; while(*p && isspace((unsigned char)*p)) p++;
    if(p != s) memmove(s, p, strlen(p)+1);
}

typedef enum {
    IPC_CMD_NONE = 0,
    IPC_CMD_STOP = 1,
    IPC_CMD_PAUSE = 2,
    IPC_CMD_RESUME = 3
} ipc_cmd_t;

/*
Funcao: ipc_poll_command
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static ipc_cmd_t ipc_poll_command(int use_ipc){
    static char pending[256];
    static size_t pending_len = 0;
    if(!use_ipc) return IPC_CMD_NONE;

    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if(h == NULL || h == INVALID_HANDLE_VALUE) return IPC_CMD_NONE;

    DWORD avail = 0;
    if(!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail == 0){
        return IPC_CMD_NONE;
    }

    char chunk[128];
    DWORD to_read = (avail < (DWORD)(sizeof(chunk) - 1)) ? avail : (DWORD)(sizeof(chunk) - 1);
    DWORD got = 0;
    if(!ReadFile(h, chunk, to_read, &got, NULL) || got == 0){
        return IPC_CMD_NONE;
    }
    chunk[got] = 0;

    if(got + pending_len >= sizeof(pending)){
        pending_len = 0;
    }
    memcpy(pending + pending_len, chunk, got);
    pending_len += got;
    pending[pending_len] = 0;

    for(size_t i = 0; i < pending_len; ++i){
        if(pending[i] != '\n') continue;

        size_t line_len = i;
        while(line_len > 0 && (pending[line_len - 1] == '\r' || pending[line_len - 1] == '\n')){
            line_len--;
        }

        char line[64];
        size_t copy_len = (line_len < sizeof(line) - 1) ? line_len : (sizeof(line) - 1);
        memcpy(line, pending, copy_len);
        line[copy_len] = 0;
        trim(line);

        size_t remain = pending_len - (i + 1);
        memmove(pending, pending + i + 1, remain);
        pending_len = remain;
        pending[pending_len] = 0;

        if(_stricmp(line, "STOP") == 0) return IPC_CMD_STOP;
        if(_stricmp(line, "PAUSE") == 0) return IPC_CMD_PAUSE;
        if(_stricmp(line, "RESUME") == 0) return IPC_CMD_RESUME;

        i = (size_t)-1;
    }

    return IPC_CMD_NONE;
}

/*
Funcao: print_usage
Objetivo: Exibe informacoes para operador/diagnostico.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void print_usage(void){
    puts("Uso:");
    puts("  dlg_logger_ipc --out <csv> --duration <s> [--rate <hz>] [--ipc]");
    puts("                [--ip <dlg_ip>] [--port <dlg_port>]");
    puts("                [--bind-ip <ip>] [--bind-port <port>]");
    puts("                [--force-normal <N>]");
}

/*
Funcao: main
Objetivo: Executa o fluxo principal do programa.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
int main(int argc, char **argv){
    const char *out_path = NULL;
    const char *dlg_ip = DLG_IP_DEFAULT;
    const char *bind_ip = LOCAL_BIND_IP_DEFAULT;
    uint16_t dlg_port = DLG_PORT_DEFAULT;
    uint16_t bind_port = LOCAL_BIND_PORT_DEFAULT;
    double rate_hz = DEFAULT_RATE_HZ;
    double duration_s = DEFAULT_DURATION_S;
    double force_normal_n = 0.0;
    int use_ipc = 0;

    for(int i = 1; i < argc; ++i){
        if(strcmp(argv[i], "--out") == 0 && i + 1 < argc){ out_path = argv[++i]; continue; }
        if(strcmp(argv[i], "--duration") == 0 && i + 1 < argc){ duration_s = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--rate") == 0 && i + 1 < argc){ rate_hz = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--ip") == 0 && i + 1 < argc){ dlg_ip = argv[++i]; continue; }
        if(strcmp(argv[i], "--port") == 0 && i + 1 < argc){ dlg_port = (uint16_t)atoi(argv[++i]); continue; }
        if(strcmp(argv[i], "--bind-ip") == 0 && i + 1 < argc){ bind_ip = argv[++i]; continue; }
        if(strcmp(argv[i], "--bind-port") == 0 && i + 1 < argc){ bind_port = (uint16_t)atoi(argv[++i]); continue; }
        if(strcmp(argv[i], "--force-normal") == 0 && i + 1 < argc){ force_normal_n = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--ipc") == 0){ use_ipc = 1; continue; }
        print_usage();
        return 1;
    }
    if(!out_path || duration_s <= 0 || rate_hz <= 0){
        print_usage();
        return 1;
    }
    ensure_out_dir_for_path(out_path);
    ev_open_for_out(out_path);
    ev_logf("START out=%s duration=%.3f rate=%.3f dlg=%s:%u bind=%s:%u ipc=%d force=%.6f",
            out_path, duration_s, rate_hz, dlg_ip, (unsigned)dlg_port,
            bind_ip ? bind_ip : "", (unsigned)bind_port, use_ipc, force_normal_n);
    FILE *f = _fsopen(out_path, "w", _SH_DENYNO);
    if(!f){
        fprintf(stderr, "Falha abrindo CSV: %s\n", out_path);
        ev_logf("ERROR open csv failed.");
        ev_close();
        return 1;
    }
    /* Desabilita buffering para tail em tempo real (graficos e agregador). */
    setvbuf(f, NULL, _IONBF, 0);
    fprintf(f, "idx,t_qpc,t_s,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,atrito,err\n");
    fflush(f);

    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2), &wsa) != 0){
        fprintf(stderr, "WSAStartup falhou.\n");
        ev_logf("ERROR WSAStartup failed.");
        fclose(f);
        ev_close();
        return 1;
    }

    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in addr;
    if(open_udp(&s, &addr, dlg_ip, dlg_port, bind_ip, bind_port) != 0){
        fprintf(stderr, "Falha ao abrir UDP.\n");
        ev_logf("ERROR open_udp failed.");
        WSACleanup();
        fclose(f);
        ev_close();
        return 1;
    }
    ev_logf("UDP open ok.");

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
    ev_logf("ACQSTOP sent; configuring channels.");
    for(int ch = 1; ch <= DEFAULT_CHANNELS; ++ch){
        int idx = ch - 1;
        configure_channel(s, &addr, ch,
                          tSensor[idx], iLPF[idx], iGainIdx[idx], iSensPwr[idx]);
    }
    acq_setup_multi(s, &addr, (float)rate_hz, DEFAULT_CHANNELS);
    acq_start(s, &addr);
    ev_logf("ACQSETUP/START sent.");

    if(use_ipc){
        puts("READY");
        fflush(stdout);
        ev_logf("READY emitted; waiting START.");
        char line[64];
        if(!fgets(line, sizeof(line), stdin)){
            ev_logf("IPC START read failed.");
            fclose(f);
            closesocket(s);
            WSACleanup();
            ev_close();
            return 1;
        }
        trim(line);
        if(_stricmp(line, "START") != 0){
            ev_logf("IPC START invalid token: %s", line);
            fclose(f);
            closesocket(s);
            WSACleanup();
            ev_close();
            return 1;
        }
        ev_logf("IPC START received.");
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
    ev_logf("WAIT_FIRST result=%d ring_count=%d", got_first, ring.count);
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
        ev_logf("DATA state emitted: %s", got_first ? "DATA_OK" : "DATA_TIMEOUT");
    }

    int idx = 0;
    int stop_requested = 0;
    int paused = 0;
    int64_t pause_start_ticks = 0;
    DWORD next_progress_log_ms = GetTickCount() + 1000;
    int n_have = 0;
    int n_null = 0;
    while(idx < total_samples){
        ipc_cmd_t ipc_cmd = ipc_poll_command(use_ipc);
        if(ipc_cmd == IPC_CMD_STOP){
            ev_logf("STOP received at idx=%d/%d", idx, total_samples);
            stop_requested = 1;
            break;
        }
        if(ipc_cmd == IPC_CMD_PAUSE && !paused){
            paused = 1;
            pause_start_ticks = qpc_now_ticks();
            ring_clear(&ring);
            ev_logf("PAUSE received at idx=%d.", idx);
        }else if(ipc_cmd == IPC_CMD_RESUME && paused){
            int64_t now_resume = qpc_now_ticks();
            int64_t paused_ticks = now_resume - pause_start_ticks;
            if(paused_ticks < 0) paused_ticks = 0;
            start_ticks += paused_ticks;
            next_ticks += paused_ticks;
            paused = 0;
            ev_logf("RESUME received at idx=%d paused_ticks=%lld.", idx, (long long)paused_ticks);
        }

        if(paused){
            (void)drain_packets_to_ring(s, &ring, DEFAULT_CHANNELS);
            ring_clear(&ring);
            Sleep(1);
            continue;
        }

        /* Drain all available packets */
        (void)drain_packets_to_ring(s, &ring, DEFAULT_CHANNELS);

        int64_t now = qpc_now_ticks();
        if(now >= next_ticks){
            sample_t sm;
            int have = ring_pop(&ring, &sm);
            double t_s = (double)idx / rate_hz;
            int64_t t_qpc = start_ticks + (int64_t)idx * dt_ticks;

            if(have){
                n_have++;
                char ch_buf[DEFAULT_CHANNELS][32];
                double ch0_value = 0.0;
                int ch0_valid = 0;
                for(int ch = 0; ch < DEFAULT_CHANNELS; ++ch){
                    if(has_calib[ch]){
                        double v = slope[ch] * (double)sm.ch[ch] + intercept[ch];
                        _snprintf(ch_buf[ch], sizeof(ch_buf[ch]), "%.6f", v);
                        if(ch == 0){
                            ch0_value = v;
                            ch0_valid = 1;
                        }
                    }else{
                        _snprintf(ch_buf[ch], sizeof(ch_buf[ch]), "%d", (int)sm.ch[ch]);
                        if(ch == 0){
                            ch0_value = (double)sm.ch[ch];
                            ch0_valid = 1;
                        }
                    }
                }
                if(force_normal_n > 0.0 && ch0_valid){
                    double atrito = ch0_value / force_normal_n;
                    fprintf(f, "%d,%lld,%.6f,%s,%s,%s,%s,%s,%s,%s,%s,%.6f,0\n",
                            idx, (long long)t_qpc, t_s,
                            ch_buf[0], ch_buf[1], ch_buf[2], ch_buf[3],
                            ch_buf[4], ch_buf[5], ch_buf[6], ch_buf[7],
                            atrito);
                }else{
                    fprintf(f, "%d,%lld,%.6f,%s,%s,%s,%s,%s,%s,%s,%s,NULL,0\n",
                            idx, (long long)t_qpc, t_s,
                            ch_buf[0], ch_buf[1], ch_buf[2], ch_buf[3],
                            ch_buf[4], ch_buf[5], ch_buf[6], ch_buf[7]);
                }
            }else{
                n_null++;
                fprintf(f, "%d,%lld,%.6f,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,1\n",
                        idx, (long long)t_qpc, t_s);
            }
            idx++;
            next_ticks += dt_ticks;
        }else{
            Sleep(1);
        }
        {
            DWORD now_ms = GetTickCount();
            if((LONG)(now_ms - next_progress_log_ms) >= 0){
                ev_logf("PROGRESS idx=%d/%d have=%d null=%d ring=%d overrun=%d paused=%d",
                        idx, total_samples, n_have, n_null, ring.count, ring.overrun, paused);
                next_progress_log_ms = now_ms + 1000;
            }
        }
    }

    if(stop_requested && use_ipc){
        puts("STOP requested (IPC).");
        fflush(stdout);
    }

    stop_acq(s, &addr);
    ev_logf("END idx=%d/%d stop=%d have=%d null=%d ring_overrun=%d",
            idx, total_samples, stop_requested, n_have, n_null, ring.overrun);
    ring_free(&ring);
    closesocket(s);
    WSACleanup();
    fclose(f);
    ev_close();
    return 0;
}
