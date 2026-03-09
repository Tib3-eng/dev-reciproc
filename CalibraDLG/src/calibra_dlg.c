/*
calibra_dlg.c
-------------
Ferramenta de calibracao do DLG4000 (modo interativo e modo IPC).

Objetivo geral:
- Capturar valores brutos do DLG em pontos de referencia informados pelo usuario/UI.
- Ajustar reta de calibracao (a, b) e salvar JSON para consumo posterior do logger.
- Permitir integracao com CalibraDLG_UI via protocolo JSON lines em stdin/stdout.

Fluxo principal:
1) Inicializa rede/sockets e aplica configuracao de canal.
2) Inicia stream do DLG e aguarda primeiro pacote valido.
3) Para cada ponto, coleta amostras, calcula bruto medio e registra referencia.
4) Ajusta modelo linear e salva arquivo de calibracao em disco.
5) Finaliza aquisicao e encerra recursos.

Variaveis/configuracoes principais:
- DLG_IP/DLG_PORT: endpoint UDP do hardware.
- LOCAL_BIND_*: endereco local de recepcao (porta de retorno do DLG).
- IDX_FREQ/BURSTS/NSIG: parametros de aquisicao usados na calibracao.
- CAPTURE_* e DRAIN_*: timeouts para coleta por ponto e limpeza de socket.
- DEFAULT_IGAIN_IDX/DEFAULT_ILPF/DEFAULT_SENSPWR_IDX: config padrao de canal.

Resumo de funcoes:
- send_cmd/open_udp/stop_acq/acq_setup_single/acq_start: camada de protocolo DLG.
- wait_first_packet/drain_socket/restart_stream: sincronizacao e recuperacao de stream.
- ensure_out_dir* e read_line: utilitarios de arquivo/console.
- stdin_is_console/ensure_console/wait_before_exit/exit_with_pause: UX de terminal.
- ask_int/ask_double/print_*_menu: entrada guiada no modo interativo.
- json_get_*: parser leve de JSON para protocolo IPC.
- ipc_send_error/ipc_send_ok/run_ipc: loop de servico para UI externa.
- run_interactive: fluxo de calibracao assistido por prompts.
- main: escolhe modo de execucao e trata ciclo de vida do processo.
*/
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

#define DEFAULT_IGAIN_IDX 5
#define DEFAULT_ILPF  0
#define DEFAULT_SENSPWR_IDX 2
#define TC_CJC_INTERNAL 0
#define TC_CJC_EXTERNAL 1

static const int gain_values[] = { 1, 3, 10, 30, 100, 300, 1000, 3000 };
static const double vexc_values[] = { 1.0, 2.5, 3.3, 5.0, 0.0 };

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

/*
Funcao: open_udp
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: stop_acq
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: acq_setup_single
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: acq_start
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: wait_first_packet
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: drain_socket
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: restart_stream
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int restart_stream(SOCKET s, const struct sockaddr_in *addr, int ch) {
    acq_setup_single(s, addr, IDX_FREQ, ch);
    acq_start(s, addr);
    return wait_first_packet(s, addr, ch);
}

/*
Funcao: ensure_out_dir
Objetivo: Resolve configuracao/caminho/estado auxiliar do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int ensure_out_dir(void) {
    if (_mkdir("out") != 0 && errno != EEXIST) return -1;
    return 0;
}

/*
Funcao: ensure_out_dir_for_path
Objetivo: Resolve configuracao/caminho/estado auxiliar do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int ensure_out_dir_for_path(const char *path) {
    const char *a = strrchr(path, '\\');
    const char *b = strrchr(path, '/');
    const char *last = a;
    if (b && (!last || b > last)) last = b;
    if (!last) return 0;

    size_t len = (size_t)(last - path);
    if (len == 0) return 0;

    char dir[260];
    if (len >= sizeof(dir)) return -1;
    memcpy(dir, path, len);
    dir[len] = '\0';

    if (_mkdir(dir) != 0 && errno != EEXIST) return -1;
    return 0;
}

/*
Funcao: read_line
Objetivo: Realiza leitura de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int read_line(char *buf, size_t size) {
    if (!fgets(buf, (int)size, stdin)) return 0;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return 1;
}

/*
Funcao: stdin_is_console
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int stdin_is_console(void) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || h == NULL) return 0;
    return GetConsoleMode(h, &mode) ? 1 : 0;
}

static int g_allocated_console = 0;

/*
Funcao: ensure_console
Objetivo: Resolve configuracao/caminho/estado auxiliar do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: wait_before_exit
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: exit_with_pause
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int exit_with_pause(int code) {
    wait_before_exit();
    return code;
}

/*
Funcao: ask_int
Objetivo: Faz parse/validacao de entrada de dados.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: ask_double
Objetivo: Faz parse/validacao de entrada de dados.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: print_sensor_menu
Objetivo: Exibe informacoes para operador/diagnostico.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
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

/*
Funcao: print_gain_menu
Objetivo: Exibe informacoes para operador/diagnostico.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void print_gain_menu(void) {
    printf("Ganho (iGain):\n");
    for (int i = 0; i < (int)(sizeof(gain_values) / sizeof(gain_values[0])); ++i) {
        printf(" %d - %d\n", i + 1, gain_values[i]);
    }
}

/*
Funcao: print_vexc_menu
Objetivo: Exibe informacoes para operador/diagnostico.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void print_vexc_menu(void) {
    printf("Tensao de excitacao (iSensPwr):\n");
    for (int i = 0; i < (int)(sizeof(vexc_values) / sizeof(vexc_values[0])); ++i) {
        if (i == 4) {
            printf(" %d - user\n", i + 1);
        } else {
            printf(" %d - %.1f V\n", i + 1, vexc_values[i]);
        }
    }
}

static void compute_fit(int npoints, const double *raw, const double *ref,
                        double *slope, double *intercept, double *r2) {
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
    double s = 0.0;
    double b = 0.0;
    if (fabs(denom) > 1e-12) {
        s = (n * sum_xy - sum_x * sum_y) / denom;
        b = (sum_y - s * sum_x) / n;
    }
    double ss_tot = sum_yy - (sum_y * sum_y) / n;
    double ss_reg = s * (sum_xy - sum_x * sum_y / n);
    double r = 0.0;
    if (ss_tot > 0.0) {
        r = ss_reg / ss_tot;
        if (r < 0.0) r = 0.0;
        if (r > 1.0) r = 1.0;
    }

    if (slope) *slope = s;
    if (intercept) *intercept = b;
    if (r2) *r2 = r;
}

/*
Funcao: is_thermocouple_sensor
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int is_thermocouple_sensor(int tSensor) {
    return (tSensor >= 6 && tSensor <= 13) ? 1 : 0;
}

static const char *tc_cjc_mode_name(int mode) {
    return (mode == TC_CJC_EXTERNAL) ? "external" : "internal";
}

/*
Funcao: build_tcmeta_path
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int build_tcmeta_path(const char *main_path, char *out, size_t outsz) {
    const char *dot = strrchr(main_path, '.');
    size_t base_len = strlen(main_path);
    if (dot && strcmp(dot, ".json") == 0) {
        base_len = (size_t)(dot - main_path);
    }
    if (base_len + strlen("_tcmeta.json") + 1 > outsz) return 0;
    memcpy(out, main_path, base_len);
    out[base_len] = '\0';
    strcat(out, "_tcmeta.json");
    return 1;
}

/*
Funcao: write_tcmeta_json
Objetivo: Realiza escrita de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int write_tcmeta_json(const char *main_path, int ch, int tSensor, int tc_cjc_mode) {
    char meta_path[320];
    if (!build_tcmeta_path(main_path, meta_path, sizeof(meta_path))) return 0;
    if (ensure_out_dir_for_path(meta_path) != 0) return 0;

    FILE *f = fopen(meta_path, "w");
    if (!f) return 0;

    fprintf(f, "{\n");
    fprintf(f, "  \"channel\": \"CH%d\",\n", ch);
    fprintf(f, "  \"tSensor\": %d,\n", tSensor);
    fprintf(f, "  \"tc_cjc_mode\": %d,\n", tc_cjc_mode);
    fprintf(f, "  \"tc_cjc_mode_name\": \"%s\"\n", tc_cjc_mode_name(tc_cjc_mode));
    fprintf(f, "}\n");

    fclose(f);
    return 1;
}

/*
Funcao: json_get_string
Objetivo: Faz parse/validacao de entrada de dados.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int json_get_string(const char *line, const char *key, char *out, size_t outsz) {
    const char *p = strstr(line, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '\"' && i + 1 < outsz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '\"') ? 1 : 0;
}

/*
Funcao: json_get_double
Objetivo: Faz parse/validacao de entrada de dados.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int json_get_double(const char *line, const char *key, double *out) {
    const char *p = strstr(line, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

/*
Funcao: json_get_int
Objetivo: Faz parse/validacao de entrada de dados.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int json_get_int(const char *line, const char *key, int *out) {
    const char *p = strstr(line, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

/*
Funcao: ipc_send_error
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ipc_send_error(const char *msg) {
    printf("{\"op\":\"error\",\"message\":\"%s\"}\n", msg);
    fflush(stdout);
}

/*
Funcao: ipc_send_ok
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ipc_send_ok(const char *op) {
    printf("{\"op\":\"%s\"}\n", op);
    fflush(stdout);
}

/*
Funcao: run_ipc
Objetivo: Controla uma etapa operacional do ensaio.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int run_ipc(void) {
    char line[512];
    int configured = 0;

    int ch = 0;
    int tSensor = 0;
    int iLPF = DEFAULT_ILPF;
    int iGainIdx = DEFAULT_IGAIN_IDX;
    int iSensPwr = DEFAULT_SENSPWR_IDX;
    int tc_cjc_mode = TC_CJC_INTERNAL;
    double gain_nom = gain_values[DEFAULT_IGAIN_IDX];
    double vexc_nom = vexc_values[DEFAULT_SENSPWR_IDX];
    char out_path[256] = "out/calib.json";

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) {
        ipc_send_error("wsa_startup_failed");
        return 1;
    }

    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in addr;

    double *raw = NULL;
    double *ref = NULL;
    size_t cap = 0;
    size_t npoints = 0;

    while (read_line(line, sizeof(line))) {
        char op[32] = {0};
        if (!json_get_string(line, "\"op\"", op, sizeof(op))) {
            ipc_send_error("missing_op");
            continue;
        }

        if (strcmp(op, "config") == 0) {
            if (!json_get_int(line, "\"ch\"", &ch) || ch < 1 || ch > 8) {
                ipc_send_error("invalid_ch");
                continue;
            }
            if (!json_get_int(line, "\"tSensor\"", &tSensor)) {
                ipc_send_error("missing_tSensor");
                continue;
            }
            json_get_int(line, "\"iLPF\"", &iLPF);
            json_get_int(line, "\"iGain\"", &iGainIdx);
            json_get_int(line, "\"iSensPwr\"", &iSensPwr);
            json_get_int(line, "\"tc_cjc_mode\"", &tc_cjc_mode);
            json_get_string(line, "\"out_path\"", out_path, sizeof(out_path));

            if (iGainIdx < 0 || iGainIdx > 7) iGainIdx = DEFAULT_IGAIN_IDX;
            if (iSensPwr < 0 || iSensPwr > 4) iSensPwr = DEFAULT_SENSPWR_IDX;
            if (tc_cjc_mode != TC_CJC_INTERNAL && tc_cjc_mode != TC_CJC_EXTERNAL) {
                tc_cjc_mode = TC_CJC_INTERNAL;
            }
            gain_nom = gain_values[iGainIdx];
            vexc_nom = vexc_values[iSensPwr];

            if (open_udp(&s, &addr) != 0) {
                ipc_send_error("socket_open_failed");
                continue;
            }
            if (!test_comm(s, &addr, ch, tSensor, iLPF, iGainIdx, iSensPwr)) {
                closesocket(s);
                s = INVALID_SOCKET;
                ipc_send_error("test_comm_failed");
                continue;
            }

            configured = 1;
            ipc_send_ok("config_ok");
            continue;
        }

        if (!configured) {
            ipc_send_error("not_configured");
            continue;
        }

        if (strcmp(op, "point") == 0) {
            double ref_val = 0.0;
            if (!json_get_double(line, "\"ref\"", &ref_val)) {
                ipc_send_error("missing_ref");
                continue;
            }

            stop_acq(s, &addr);
            acq_setup_single(s, &addr, IDX_FREQ, ch);
            acq_start(s, &addr);

        drain_socket(s);
        if (restart_stream(s, &addr, ch) != 0) {
            stop_acq(s, &addr);
            ipc_send_error("stream_inactive");
            continue;
        }

            double avg = 0.0;
            int samples = 0;
        if (!capture_average_1s(s, &addr, ch, &avg, &samples)) {
            stop_acq(s, &addr);
            ipc_send_error("no_samples");
            continue;
        }
            stop_acq(s, &addr);

            if (npoints + 1 > cap) {
                size_t new_cap = (cap == 0) ? 8 : cap * 2;
                double *new_raw = (double *)realloc(raw, new_cap * sizeof(double));
                double *new_ref = (double *)realloc(ref, new_cap * sizeof(double));
                if (!new_raw || !new_ref) {
                    free(new_raw);
                    free(new_ref);
                    ipc_send_error("out_of_memory");
                    continue;
                }
                raw = new_raw;
                ref = new_ref;
                cap = new_cap;
            }
            raw[npoints] = avg;
            ref[npoints] = ref_val;
            npoints++;

            printf("{\"op\":\"point_result\",\"ref\":%.10g,\"raw\":%.10g,\"samples\":%d}\n",
                   ref_val, avg, samples);
            fflush(stdout);
            continue;
        }

        if (strcmp(op, "finish") == 0) {
            if (npoints < 2) {
                ipc_send_error("need_at_least_2_points");
                continue;
            }

            double slope = 0.0, intercept = 0.0, r2 = 0.0;
            compute_fit((int)npoints, raw, ref, &slope, &intercept, &r2);

            if (!write_json(out_path, ch, tSensor, iLPF, iGainIdx, iSensPwr,
                            gain_nom, vexc_nom, (int)npoints, raw, ref, slope, intercept, r2)) {
                ipc_send_error("write_failed");
                continue;
            }

            if (is_thermocouple_sensor(tSensor)) {
                (void)write_tcmeta_json(out_path, ch, tSensor, tc_cjc_mode);
            }

            printf("{\"op\":\"done\",\"slope\":%.10g,\"intercept\":%.10g,\"r2\":%.6f,\"out_path\":\"%s\"}\n",
                   slope, intercept, r2, out_path);
            fflush(stdout);
            break;
        }

        if (strcmp(op, "cancel") == 0) {
            ipc_send_ok("cancelled");
            break;
        }

        ipc_send_error("unknown_op");
    }

    if (s != INVALID_SOCKET) {
        stop_acq(s, &addr);
        closesocket(s);
    }
    WSACleanup();
    free(raw);
    free(ref);
    return 0;
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
    if (ensure_out_dir_for_path(path) != 0) return 0;
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

/*
Funcao: run_interactive
Objetivo: Controla uma etapa operacional do ensaio.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int run_interactive(void) {
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
    int tc_cjc_mode = TC_CJC_INTERNAL;
    double gain_nom = gain_values[DEFAULT_IGAIN_IDX];
    double vexc_nom = vexc_values[DEFAULT_SENSPWR_IDX];

    if (!ask_int("Qual canal deseja calibrar (1-8): ", 1, 8, &ch)) return exit_with_pause(1);
    print_sensor_menu();
    if (!ask_int("Selecione o tipo de sensor (0-9): ", 0, 9, &tSensor)) return exit_with_pause(1);
    if (is_thermocouple_sensor(tSensor)) {
        int opt_cjc = 1;
        printf("Junta fria (termopar):\n");
        printf(" 1 - interna\n");
        printf(" 2 - externa (TEDs)\n");
        if (!ask_int("Escolha a junta fria (1-2): ", 1, 2, &opt_cjc)) return exit_with_pause(1);
        tc_cjc_mode = (opt_cjc == 2) ? TC_CJC_EXTERNAL : TC_CJC_INTERNAL;
    }
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
    if (!ask_int("Escolha a tensao de excitacao (1-5): ", 1, 5, &opt_vexc)) return exit_with_pause(1);
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

    double slope = 0.0, intercept = 0.0, r2 = 0.0;
    compute_fit(npoints, raw, ref, &slope, &intercept, &r2);

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
    if (is_thermocouple_sensor(tSensor)) {
        (void)write_tcmeta_json(out_path, ch, tSensor, tc_cjc_mode);
    }

    printf("Saida: %s\n", out_path);
    free(raw);
    free(ref);
    if (g_allocated_console) wait_before_exit();
    return 0;
}

/*
Funcao: main
Objetivo: Executa o fluxo principal do programa.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
int main(int argc, char **argv) {
    int ipc = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ipc") == 0) {
            ipc = 1;
        }
    }
    if (ipc) return run_ipc();
    return run_interactive();
}
