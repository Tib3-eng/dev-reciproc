/*
a5_speed_logger.c
-----------------
Logger sem UI para ensaios em modo velocidade no Drive A5.

Objetivo geral:
- Executar um schedule de segmentos (rpm,duration_s) e registrar telemetria.
- Coletar posicao + rpm em taxa fixa, com slots perdidos marcados como NULL.
- Integrar com supervisorio via IPC textual (START/PAUSE/RESUME/STOP).

Fluxo principal:
1) Parse de argumentos e carregamento do schedule CSV.
2) Setup opcional do drive para modo velocidade (P06-03 como fonte unica).
3) Espera START (quando --ipc) e entra no loop de segmentos com rampa.
4) Leitura periodica de posicao/rpm por deadline real (QPC), sem drift.
5) STOP reforcado no fim/interrupcao e fechamento do CSV.

Variaveis/configuracoes principais:
- REG_*: registradores usados para controle de velocidade e telemetria.
- WORD_RUN/WORD_RDY: palavras Modbus para RUN e parada imediata.
- CMD_* e FAST_*: timeouts para escrita/comando e leitura rapida.
- RAMP_TIME_S: duracao da rampa linear entre setpoints.
- seg_t: segmento de schedule (rpm + duracao + tempo final acumulado).
- rpm_ramp_t: estado da rampa ativa de transicao de velocidade.

Resumo de funcoes:
- set_timeouts_us/trim/make_port_path: utilitarios de IO serial e texto.
- read_*: leitura de registradores com fallback/cache de modo FC03/FC04.
- read_s32: leitura 32-bit respeitando ordem de palavras.
- setup_speed_mode: aplica parametros para estabilizar modo velocidade.
- cmd_run/cmd_stop/cmd_vdi_stop/cmd_rpm: comandos de atuacao no drive.
- qpc_now_ticks/qpc_freq_ticks: temporizacao de alta precisao para deadlines.
- stop_drive_now: rotina de parada segura com rampa curta e reforco de STOP.
- ipc_poll_command: parse de comandos recebidos por stdin.
- round_to_int/ramp_begin/ramp_eval: utilitarios da logica de rampa.
- parse_two_numbers/load_schedule: parse de linhas do schedule.
- print_usage/main: entrada principal do executavel.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <share.h>
#include <stdarg.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <modbus.h>

enum {
    REG_CTRL    = 12544,  // 12545-1 : RUN/RDY
    REG_RPM_CMD = 1539,   // 1540-1  : RPM setpoint (int16)
    REG_P02_00  = 0x0200, // control mode (speed/pos/torque)
    REG_P03_02  = 0x0302, // DI1 func (disable)
    REG_P06_00  = 0x0600, // main speed source A
    REG_P06_01  = 0x0601, // aux speed source B
    REG_P06_02  = 0x0602, // speed command source
    REG_P0C_09  = 0x0C09, // comm VDI enable
    REG_P31_00  = 0x3100, // VDI virtual
    REG_P0B_07  = 0x0B07, // P0B-07 (abs pos, 32-bit) - historical naming
    REG_P0B_09  = 0x0B09, // P0B-09 (pos per revolution, 0..65535)
    REG_P0B_00  = 0x0B00, // P0B-00 (actual motor speed, rpm)
    REG_P05_02  = 0x0502, // P05-02 (command units per revolution)
    REG_P0C_26  = 0x0C26  // word order
};

static const uint16_t WORD_RUN = 0x0001;
static const uint16_t WORD_RDY = 0x0000;

#define CMD_RESP_US 50000
#define CMD_BYTE_US 50000
#define FAST_RESP_US 3000
#define FAST_BYTE_US 2000
#define RAMP_TIME_S 3.0
#define ENCODER_CAL_PREFLIGHT_READS 3
#define ENCODER_CAL_PREFLIGHT_ATTEMPTS 30
#define ENCODER_CAL_POSITION_LOSS_S 2.0

typedef struct {
    int rpm;
    double dur_s;
    double t_end;
} seg_t;

typedef struct {
    int start_rpm;
    int target_rpm;
    int active;
    int64_t t_start;
    int64_t t_end;
} rpm_ramp_t;

typedef struct {
    int have_position;
    uint16_t last_pos_raw;
    int have_previous_raw;
    uint16_t previous_raw;
    int64_t unwrapped_counts;
    int64_t last_valid_ticks;
    ULONGLONG next_emit_ms;
} encoder_drive_status_t;

typedef struct {
    int enabled;
    double course_mm;
    double total_mm;
    double raio_mm;
    double relacao;
    int tol_counts;
    uint32_t pos_mod;
    int initialized;
    int direction;
    int stop_pending;
    int done;
    int abort;
    int miss_count;
    int miss_limit;
    int32_t last_mod_pos;
    int64_t start_pos;
    int64_t end_pos;
    int64_t stroke_start_pos;
    int64_t pos_unwrapped;
    int64_t stroke_counts;
    int64_t total_counts;
    int64_t stop_target;
    double accum_counts;
    char abort_msg[192];
} recip_state_t;

static int recip_target_reached(const recip_state_t *r, int64_t pos, int64_t target);
static int recip_stroke_limit_reached(const recip_state_t *r, int64_t pos);

static FILE *g_ev = NULL;
static volatile LONG g_console_stop_requested = 0;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type){
    switch(ctrl_type){
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            InterlockedExchange(&g_console_stop_requested, 1);
            return TRUE;
        default:
            return FALSE;
    }
}

static int console_stop_requested(void){
    return InterlockedCompareExchange(
        &g_console_stop_requested,
        0,
        0
    ) != 0;
}

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
        strcat(ev_path, "\\a5_speed_events.log");
    }else{
        _snprintf(ev_path, sizeof(ev_path), "a5_speed_events.log");
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

/*
Funcao: set_timeouts_us
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void set_timeouts_us(modbus_t *ctx, int resp_us, int byte_us){
    modbus_set_response_timeout(ctx, 0, resp_us);
    modbus_set_byte_timeout(ctx, 0, byte_us);
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

/*
Funcao: make_port_path
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void make_port_path(const char *shortName, char *out, size_t outsz){
    int num = 0;
    if(_strnicmp(shortName, "COM", 3) == 0) num = atoi(shortName + 3);
    if(num >= 10) _snprintf(out, outsz, "\\\\.\\%s", shortName);
    else _snprintf(out, outsz, "%s", shortName);
}

/*
Funcao: read_u16
Objetivo: Realiza leitura de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int read_u16(modbus_t *ctx, int reg, uint16_t *out){
    uint16_t v = 0;
    if(modbus_read_registers(ctx, reg, 1, &v) != 1){
        return -1;
    }
    *out = v;
    return 0;
}

/*
Funcao: read_u16_in
Objetivo: Realiza leitura de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int read_u16_in(modbus_t *ctx, int reg, uint16_t *out){
    uint16_t v = 0;
    if(modbus_read_input_registers(ctx, reg, 1, &v) != 1){
        return -1;
    }
    *out = v;
    return 0;
}

/*
Funcao: read_p0b09
Objetivo: Realiza leitura de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int read_p0b09(modbus_t *ctx, uint16_t *out){
    if(read_u16(ctx, REG_P0B_09, out) == 0) return 0;
    if(read_u16_in(ctx, REG_P0B_09, out) == 0) return 0;
    return -1;
}

/*
Funcao: read_u16_cached
Objetivo: Realiza leitura de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int read_u16_cached(modbus_t *ctx, int reg, uint16_t *out, int *mode){
    /* mode: 0=unknown, 3=FC03 holding, 4=FC04 input */
    if(mode && *mode == 3){
        if(read_u16(ctx, reg, out) == 0) return 0;
        if(read_u16_in(ctx, reg, out) == 0){ *mode = 4; return 0; }
        return -1;
    }
    if(mode && *mode == 4){
        if(read_u16_in(ctx, reg, out) == 0) return 0;
        if(read_u16(ctx, reg, out) == 0){ *mode = 3; return 0; }
        return -1;
    }

    if(read_u16(ctx, reg, out) == 0){
        if(mode) *mode = 3;
        return 0;
    }
    if(read_u16_in(ctx, reg, out) == 0){
        if(mode) *mode = 4;
        return 0;
    }
    return -1;
}

static int read_u16_cached_retry(modbus_t *ctx, int reg, uint16_t *out,
                                 int *mode, int retries, int retry_ms){
    for(int i = 0; i < retries; ++i){
        if(read_u16_cached(ctx, reg, out, mode) == 0) return 0;
        if(i + 1 < retries) Sleep(retry_ms);
    }
    return -1;
}

/*
Funcao: read_p0b09_cached
Objetivo: Realiza leitura de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int read_p0b09_cached(modbus_t *ctx, uint16_t *out, int *mode){
    return read_u16_cached(ctx, REG_P0B_09, out, mode);
}

/*
Funcao: read_rpm_cached
Objetivo: Realiza leitura de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int read_rpm_cached(modbus_t *ctx, int16_t *out, int *mode){
    uint16_t u = 0;
    if(read_u16_cached(ctx, REG_P0B_00, &u, mode) != 0) return -1;
    *out = (int16_t)u;
    return 0;
}

static int write_u16_seq(modbus_t *ctx, int reg, uint16_t v, const char *label,
                         int retries, int retry_ms){
    for(int i = 0; i < retries; ++i){
        if(modbus_write_register(ctx, reg, v) != -1){
            if(label){
                printf("WRITE 0x%04X %s = %u -> OK\n", reg, label, (unsigned)v);
                ev_logf(
                    "SETUP_WRITE reg=0x%04X label=%s value=%u result=OK",
                    reg,
                    label,
                    (unsigned)v
                );
            }
            return 0;
        }
        if(i + 1 < retries) Sleep(retry_ms);
    }
    if(label){
        printf("WRITE 0x%04X %s = %u -> FAIL\n", reg, label, (unsigned)v);
        ev_logf(
            "SETUP_WRITE reg=0x%04X label=%s value=%u result=FAIL",
            reg,
            label,
            (unsigned)v
        );
    }
    return -1;
}

static int verify_u16_value(
    modbus_t *ctx,
    int reg,
    uint16_t expected,
    const char *label
){
    for(int i = 0; i < 3; ++i){
        uint16_t actual = 0;
        int read_ok;

        set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
        read_ok =
            read_u16(ctx, reg, &actual) == 0 ||
            read_u16_in(ctx, reg, &actual) == 0;
        if(read_ok && actual == expected){
            printf(
                "READBACK 0x%04X %s = %u -> OK\n",
                reg,
                label ? label : "",
                (unsigned)actual
            );
            ev_logf(
                "SETUP_READBACK reg=0x%04X label=%s "
                "actual=%u expected=%u result=OK",
                reg,
                label ? label : "",
                (unsigned)actual,
                (unsigned)expected
            );
            return 0;
        }
        if(read_ok){
            printf(
                "READBACK 0x%04X %s = %u, esperado %u -> MISMATCH\n",
                reg,
                label ? label : "",
                (unsigned)actual,
                (unsigned)expected
            );
            ev_logf(
                "SETUP_READBACK reg=0x%04X label=%s "
                "actual=%u expected=%u result=MISMATCH",
                reg,
                label ? label : "",
                (unsigned)actual,
                (unsigned)expected
            );
        }
        if(i + 1 < 3) Sleep(30);
    }
    printf(
        "READBACK 0x%04X %s = indisponivel/incorreto -> FAIL\n",
        reg,
        label ? label : ""
    );
    ev_logf(
        "SETUP_READBACK reg=0x%04X label=%s expected=%u result=FAIL",
        reg,
        label ? label : "",
        (unsigned)expected
    );
    return -1;
}

static int setup_speed_parameter(
    modbus_t *ctx,
    int reg,
    uint16_t value,
    const char *label,
    int strict_readback
){
    if(write_u16_seq(ctx, reg, value, label, 3, 30) != 0){
        return -1;
    }
    if(strict_readback &&
       verify_u16_value(ctx, reg, value, label) != 0){
        return -1;
    }
    return 0;
}

/*
Funcao: read_s32
Objetivo: Realiza leitura de dados de hardware/arquivo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int read_s32(modbus_t *ctx, int reg, int low_first, int32_t *out){
    uint16_t regs[2] = {0, 0};
    if(modbus_read_registers(ctx, reg, 2, regs) != 2) return -1;
    uint32_t u;
    if(low_first){
        u = ((uint32_t)regs[1] << 16) | regs[0];
    }else{
        u = ((uint32_t)regs[0] << 16) | regs[1];
    }
    *out = (int32_t)u;
    return 0;
}

/*
Funcao: setup_speed_mode
Objetivo: Configura a fonte de velocidade e, no modo estrito, confirma
          cada valor por readback antes de permitir movimento.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: 0 quando todas as escritas/confirmacoes passam; -1 em falha.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int setup_speed_mode(modbus_t *ctx, int strict_readback){
    int failures = 0;

    // NOTE: Ajusta parametros para aceitar RPM via Modbus.
    // P02-00 = 0 (modo velocidade)
    // P06-00 = 0 (fonte A = P06-03 / given number)
    // P06-01 = 3 (fonte B = 0/no effect)
    // P06-02 = 0 (seletor = source A only)
    // P03-02 = 0 (desabilita DI1)
    // P0C-09 = 1 (VDI via comunicacao)
    puts("Setup speed mode (P02-00/P06-00/P06-01/P06-02/P03-02/P0C-09)...");
    failures += setup_speed_parameter(
        ctx, REG_P31_00, 0, "P31-00 (VDI STOP)", strict_readback
    ) != 0;
    failures += setup_speed_parameter(
        ctx, REG_P0C_09, 1, "P0C-09 (Comm VDI)", strict_readback
    ) != 0;
    failures += setup_speed_parameter(
        ctx, REG_P03_02, 0, "P03-02 (DI1 func)", strict_readback
    ) != 0;
    failures += setup_speed_parameter(
        ctx, REG_P02_00, 0, "P02-00 (control mode)", strict_readback
    ) != 0;
    failures += setup_speed_parameter(
        ctx, REG_P06_00, 0, "P06-00 (A src=given)", strict_readback
    ) != 0;
    failures += setup_speed_parameter(
        ctx, REG_P06_01, 3, "P06-01 (B src=0)", strict_readback
    ) != 0;
    failures += setup_speed_parameter(
        ctx, REG_P06_02, 0, "P06-02 (sel=A)", strict_readback
    ) != 0;
    return failures == 0 ? 0 : -1;
}

/*
Funcao: cmd_run
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int cmd_run(modbus_t *ctx){
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
    if(modbus_write_register(ctx, REG_CTRL, WORD_RUN) == -1) return -1;
    return 0;
}

/*
Funcao: cmd_stop
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int cmd_stop(modbus_t *ctx){
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
    for(int k=0;k<3;++k){
        if(modbus_write_register(ctx, REG_CTRL, WORD_RDY) != -1) return 0;
        Sleep(30);
    }
    return -1;
}

/*
Funcao: cmd_vdi_stop
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int cmd_vdi_stop(modbus_t *ctx){
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
    return write_u16_seq(ctx, REG_P31_00, 0, NULL, 3, 30);
}

/*
Funcao: cmd_rpm
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int cmd_rpm(modbus_t *ctx, int rpm){
    if(rpm < -32768 || rpm > 32767) return -1;
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);
    uint16_t v = (uint16_t)(rpm & 0xFFFF);
    for(int k=0;k<2;++k){
        if(modbus_write_register(ctx, REG_RPM_CMD, v) != -1) return 0;
        Sleep(20);
    }
    return -1;
}

static int round_to_int(double v);

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
Funcao: stop_drive_now
Objetivo: Envia comando para controle do fluxo de aquisicao/drive.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int stop_drive_now(modbus_t *ctx, int *current_cmd_rpm, int64_t ramp_ticks){
    /* Soft stop vector: ramp to zero setpoint, then force RDY/VDI stop. */
    int start_rpm = (current_cmd_rpm != NULL) ? *current_cmd_rpm : 0;
    int rpm_zero_ok;
    int ctrl_rdy_ok = 0;
    int vdi_stop_ok = 0;
    if(ramp_ticks > 0 && start_rpm != 0){
        int64_t t0 = qpc_now_ticks();
        int64_t t1 = t0 + ramp_ticks;
        int last_cmd = start_rpm;

        for(;;){
            int64_t now = qpc_now_ticks();
            if(now >= t1) break;
            double alpha = (double)(now - t0) / (double)(t1 - t0);
            if(alpha < 0.0) alpha = 0.0;
            if(alpha > 1.0) alpha = 1.0;
            int cmd = round_to_int((double)start_rpm * (1.0 - alpha));
            if(cmd != last_cmd){
                (void)cmd_rpm(ctx, cmd);
                last_cmd = cmd;
            }
            Sleep(20);
        }
    }

    rpm_zero_ok = cmd_rpm(ctx, 0) == 0;
    if(current_cmd_rpm) *current_cmd_rpm = 0;
    for(int i = 0; i < 4; ++i){
        if(cmd_stop(ctx) == 0) ctrl_rdy_ok = 1;
        if(cmd_vdi_stop(ctx) == 0) vdi_stop_ok = 1;
        Sleep(20);
    }
    return rpm_zero_ok && ctrl_rdy_ok && vdi_stop_ok;
}

static void emit_encoder_stopped(
    int encoder_calibration,
    int stop_confirmed,
    int *emitted
){
    if(!encoder_calibration || !emitted || *emitted) return;
    puts(stop_confirmed ? "STOPPED" : "STOP_UNCONFIRMED");
    fflush(stdout);
    ev_logf(
        stop_confirmed
            ? "STOPPED emitted."
            : "STOP_UNCONFIRMED emitted."
    );
    *emitted = 1;
}

static void encoder_status_accept_position(
    encoder_drive_status_t *status,
    uint16_t pos_raw,
    int64_t now_ticks
){
    if(!status) return;

    if(status->have_previous_raw){
        int32_t delta =
            (int32_t)pos_raw -
            (int32_t)status->previous_raw;
        if(delta > 32768){
            delta -= 65536;
        }else if(delta < -32768){
            delta += 65536;
        }
        status->unwrapped_counts += (int64_t)delta;
    }

    status->have_position = 1;
    status->last_pos_raw = pos_raw;
    status->have_previous_raw = 1;
    status->previous_raw = pos_raw;
    status->last_valid_ticks = now_ticks;
}

static void encoder_status_emit(
    encoder_drive_status_t *status,
    int current_cmd_rpm,
    int errors_total,
    int64_t now_ticks,
    int64_t qpc_freq,
    int force
){
    ULONGLONG now_ms;
    ULONGLONG last_age_ms;
    int comm_active;
    double motor_turns;

    if(!status || !status->have_position || qpc_freq <= 0) return;

    now_ms = GetTickCount64();
    if(!force){
        if(status->next_emit_ms == 0){
            status->next_emit_ms = now_ms + 1000ULL;
            return;
        }
        if(now_ms < status->next_emit_ms) return;
    }

    if(now_ticks < status->last_valid_ticks){
        last_age_ms = 0;
    }else{
        last_age_ms = (ULONGLONG)(
            ((double)(now_ticks - status->last_valid_ticks) * 1000.0) /
            (double)qpc_freq
        );
    }
    comm_active = last_age_ms < 2000ULL;
    motor_turns =
        (double)status->unwrapped_counts /
        65536.0;

    printf(
        "STATUS_DRIVE comm_active=%d cmd_rpm=%d "
        "pos_p0b09=%u unwrapped_counts=%lld "
        "motor_turns=%.9f errors_total=%d last_age_ms=%llu\n",
        comm_active,
        current_cmd_rpm,
        (unsigned)status->last_pos_raw,
        (long long)status->unwrapped_counts,
        motor_turns,
        errors_total,
        (unsigned long long)last_age_ms
    );
    fflush(stdout);
    ev_logf(
        "STATUS_DRIVE comm_active=%d cmd_rpm=%d "
        "pos_p0b09=%u unwrapped_counts=%lld "
        "motor_turns=%.9f errors_total=%d last_age_ms=%llu",
        comm_active,
        current_cmd_rpm,
        (unsigned)status->last_pos_raw,
        (long long)status->unwrapped_counts,
        motor_turns,
        errors_total,
        (unsigned long long)last_age_ms
    );

    status->next_emit_ms = now_ms + 1000ULL;
}

typedef enum {
    IPC_CMD_NONE = 0,
    IPC_CMD_STOP = 1,
    IPC_CMD_PAUSE = 2,
    IPC_CMD_RESUME = 3,
    IPC_CMD_START = 4
} ipc_cmd_t;

static ipc_cmd_t ipc_take_pending_command(
    char *pending,
    size_t *pending_len
){
    if(!pending || !pending_len) return IPC_CMD_NONE;

    for(size_t i = 0; i < *pending_len; ++i){
        if(pending[i] != '\n') continue;

        size_t line_len = i;
        while(line_len > 0 &&
              (pending[line_len - 1] == '\r' ||
               pending[line_len - 1] == '\n')){
            line_len--;
        }

        char line[64];
        size_t copy_len =
            (line_len < sizeof(line) - 1)
                ? line_len
                : (sizeof(line) - 1);
        memcpy(line, pending, copy_len);
        line[copy_len] = 0;
        trim(line);

        size_t remain = *pending_len - (i + 1);
        memmove(pending, pending + i + 1, remain);
        *pending_len = remain;
        pending[*pending_len] = 0;

        if(_stricmp(line, "STOP") == 0) return IPC_CMD_STOP;
        if(_stricmp(line, "PAUSE") == 0) return IPC_CMD_PAUSE;
        if(_stricmp(line, "RESUME") == 0) return IPC_CMD_RESUME;
        if(_stricmp(line, "START") == 0) return IPC_CMD_START;

        i = (size_t)-1;
    }
    return IPC_CMD_NONE;
}

static int run_internal_self_test(void){
    char pending[256];
    size_t pending_len;
    encoder_drive_status_t status = {0};

    strcpy_s(pending, sizeof(pending), "START\nSTOP\n");
    pending_len = strlen(pending);
    if(ipc_take_pending_command(pending, &pending_len) !=
           IPC_CMD_START ||
       ipc_take_pending_command(pending, &pending_len) !=
           IPC_CMD_STOP ||
       ipc_take_pending_command(pending, &pending_len) !=
           IPC_CMD_NONE){
        puts("SELF-TEST FALHOU: fila START/STOP.");
        return 0;
    }

    strcpy_s(
        pending,
        sizeof(pending),
        "PAUSE\r\nRESUME\n"
    );
    pending_len = strlen(pending);
    if(ipc_take_pending_command(pending, &pending_len) !=
           IPC_CMD_PAUSE ||
       ipc_take_pending_command(pending, &pending_len) !=
           IPC_CMD_RESUME ||
       pending_len != 0){
        puts("SELF-TEST FALHOU: fila PAUSE/RESUME.");
        return 0;
    }

    encoder_status_accept_position(&status, 65000u, 100);
    encoder_status_accept_position(&status, 100u, 200);
    if(status.unwrapped_counts != 636 ||
       status.last_pos_raw != 100u ||
       status.last_valid_ticks != 200){
        puts("SELF-TEST FALHOU: unwrap positivo P0B-09.");
        return 0;
    }
    encoder_status_accept_position(&status, 65000u, 300);
    if(status.unwrapped_counts != 0 ||
       status.last_pos_raw != 65000u ||
       status.last_valid_ticks != 300){
        puts("SELF-TEST FALHOU: unwrap negativo P0B-09.");
        return 0;
    }

    recip_state_t recip_test = {0};
    recip_test.stroke_counts = 1000;
    recip_test.stroke_start_pos = 0;
    recip_test.direction = 1;
    if(recip_target_reached(&recip_test, 999, 1000) ||
       !recip_target_reached(&recip_test, 1000, 1000) ||
       recip_stroke_limit_reached(&recip_test, 1999) ||
       !recip_stroke_limit_reached(&recip_test, 2000)){
        puts("SELF-TEST FALHOU: limites reciprocantes positivos.");
        return 0;
    }
    recip_test.stroke_start_pos = 5000;
    recip_test.direction = -1;
    if(recip_target_reached(&recip_test, 4001, 4000) ||
       !recip_target_reached(&recip_test, 4000, 4000) ||
       recip_stroke_limit_reached(&recip_test, 3001) ||
       !recip_stroke_limit_reached(&recip_test, 3000)){
        puts("SELF-TEST FALHOU: limites reciprocantes negativos.");
        return 0;
    }

    puts(
        "SELF-TEST OK: parser IPC, unwrap P0B-09 e limites reciprocantes."
    );
    return 1;
}

/*
Funcao: ipc_poll_command
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static ipc_cmd_t ipc_poll_command(
    int use_ipc,
    int stop_on_pipe_failure
){
    static char pending[256];
    static size_t pending_len = 0;
    ipc_cmd_t buffered;

    if(console_stop_requested()) return IPC_CMD_STOP;
    if(!use_ipc) return IPC_CMD_NONE;

    buffered = ipc_take_pending_command(
        pending,
        &pending_len
    );
    if(buffered != IPC_CMD_NONE) return buffered;

    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if(h == NULL || h == INVALID_HANDLE_VALUE){
        return stop_on_pipe_failure
            ? IPC_CMD_STOP
            : IPC_CMD_NONE;
    }

    DWORD input_type = GetFileType(h);
    int stop_if_stream_breaks =
        stop_on_pipe_failure ||
        input_type == FILE_TYPE_PIPE;
    DWORD avail = 0;
    if(!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)){
        return stop_if_stream_breaks
            ? IPC_CMD_STOP
            : IPC_CMD_NONE;
    }
    if(avail == 0){
        return IPC_CMD_NONE;
    }

    char chunk[128];
    DWORD to_read = (avail < (DWORD)(sizeof(chunk) - 1)) ? avail : (DWORD)(sizeof(chunk) - 1);
    DWORD got = 0;
    if(!ReadFile(h, chunk, to_read, &got, NULL) || got == 0){
        return stop_if_stream_breaks
            ? IPC_CMD_STOP
            : IPC_CMD_NONE;
    }
    chunk[got] = 0;

    if(got + pending_len >= sizeof(pending)){
        pending_len = 0;
    }
    memcpy(pending + pending_len, chunk, got);
    pending_len += got;
    pending[pending_len] = 0;

    return ipc_take_pending_command(
        pending,
        &pending_len
    );
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
Funcao: round_to_int
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int round_to_int(double v){
    return (v >= 0.0) ? (int)(v + 0.5) : (int)(v - 0.5);
}

/*
Funcao: ramp_begin
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Nao retorna valor.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static void ramp_begin(rpm_ramp_t *r, int start_rpm, int target_rpm, int64_t now_ticks, int64_t ramp_ticks){
    if(!r) return;
    r->start_rpm = start_rpm;
    r->target_rpm = target_rpm;
    r->t_start = now_ticks;
    r->t_end = now_ticks + ((ramp_ticks > 0) ? ramp_ticks : 0);
    if(ramp_ticks <= 0 || start_rpm == target_rpm){
        r->active = 0;
        r->t_end = now_ticks;
    }else{
        r->active = 1;
    }
}

/*
Funcao: ramp_eval
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int ramp_eval(const rpm_ramp_t *r, int64_t now_ticks){
    if(!r) return 0;
    if(!r->active) return r->target_rpm;
    if(now_ticks <= r->t_start) return r->start_rpm;
    if(now_ticks >= r->t_end) return r->target_rpm;

    int64_t span = r->t_end - r->t_start;
    int64_t elapsed = now_ticks - r->t_start;
    double alpha = (span > 0) ? ((double)elapsed / (double)span) : 1.0;
    double v = (double)r->start_rpm + ((double)(r->target_rpm - r->start_rpm) * alpha);
    return round_to_int(v);
}

static int rpm_abs_safe(int rpm){
    if(rpm == -32768) return 32767;
    return (rpm < 0) ? -rpm : rpm;
}

static int64_t i64_abs_local(int64_t v){
    return (v < 0) ? -v : v;
}

static int64_t round_to_i64(double v){
    return (v >= 0.0) ? (int64_t)(v + 0.5) : (int64_t)(v - 0.5);
}

static int32_t scale_pos_raw(uint16_t pos_raw, uint16_t cmd_units_per_rev){
    if(cmd_units_per_rev > 0){
        return (int32_t)(((uint32_t)pos_raw * (uint32_t)cmd_units_per_rev) / 65536u);
    }
    return (int32_t)pos_raw;
}

static int recip_target_rpm(const recip_state_t *r, const seg_t *segs, int seg_idx){
    int mag = rpm_abs_safe(segs[seg_idx].rpm);
    if(!r || !r->enabled || mag <= 0) return 0;
    return (r->direction >= 0) ? mag : -mag;
}

static int recip_in_band(const recip_state_t *r, int64_t pos, int64_t target){
    if(!r) return 0;
    return i64_abs_local(pos - target) <= (int64_t)r->tol_counts;
}

static int recip_target_reached(const recip_state_t *r, int64_t pos, int64_t target){
    if(!r) return 0;
    return (r->direction >= 0) ? (pos >= target) : (pos <= target);
}

static int recip_stroke_limit_reached(const recip_state_t *r, int64_t pos){
    if(!r || r->stroke_counts <= 0) return 0;
    return i64_abs_local(pos - r->stroke_start_pos) >= (r->stroke_counts * 2);
}

static int64_t recip_counts_from_mm(double mm, double relacao, double raio_mm, uint32_t pos_mod){
    const double pi = 3.14159265358979323846;
    if(mm <= 0.0 || relacao <= 0.0 || raio_mm <= 0.0 || pos_mod == 0) return 0;
    double revs_motor = (mm * relacao) / (2.0 * pi * raio_mm);
    return round_to_i64(revs_motor * (double)pos_mod);
}

static int recip_init(
    recip_state_t *r,
    int32_t initial_pos,
    uint32_t pos_mod,
    double rate_hz
){
    if(!r || !r->enabled) return 0;
    if(pos_mod == 0) pos_mod = 65536u;
    r->pos_mod = pos_mod;
    r->stroke_counts = recip_counts_from_mm(r->course_mm, r->relacao, r->raio_mm, pos_mod);
    r->total_counts = recip_counts_from_mm(r->total_mm, r->relacao, r->raio_mm, pos_mod);
    if(r->stroke_counts <= 0 || r->total_counts <= 0){
        snprintf(r->abort_msg, sizeof(r->abort_msg), "parametros reciprocantes invalidos: curso/total geraram counts <= 0");
        return -1;
    }
    if(r->tol_counts <= 0){
        snprintf(r->abort_msg, sizeof(r->abort_msg), "tolerancia reciprocante invalida: %d", r->tol_counts);
        return -1;
    }
    r->initialized = 1;
    r->direction = 1;
    r->stop_pending = 0;
    r->done = 0;
    r->abort = 0;
    r->miss_count = 0;
    r->miss_limit = (int)(rate_hz + 0.5);
    if(r->miss_limit < 5) r->miss_limit = 5;
    r->last_mod_pos = initial_pos;
    r->start_pos = (int64_t)initial_pos;
    r->end_pos = r->start_pos + r->stroke_counts;
    r->stroke_start_pos = r->start_pos;
    r->pos_unwrapped = r->start_pos;
    r->stop_target = r->end_pos;
    r->accum_counts = 0.0;
    r->abort_msg[0] = '\0';
    return 0;
}

static int64_t recip_update_unwrapped(recip_state_t *r, int32_t pos_mod_now){
    if(!r || !r->initialized || r->pos_mod == 0) return 0;
    int64_t delta = (int64_t)pos_mod_now - (int64_t)r->last_mod_pos;
    int64_t half = (int64_t)r->pos_mod / 2;
    if(r->direction >= 0){
        if(delta < -half) delta += (int64_t)r->pos_mod;
    }else{
        if(delta > half) delta -= (int64_t)r->pos_mod;
    }
    r->pos_unwrapped += delta;
    r->last_mod_pos = pos_mod_now;
    r->accum_counts += (double)i64_abs_local(delta);
    return delta;
}

static void recip_abort(recip_state_t *r, const char *msg){
    if(!r || r->abort) return;
    r->abort = 1;
    snprintf(r->abort_msg, sizeof(r->abort_msg), "%s", msg ? msg : "falha reciprocante");
}

/*
Funcao: parse_two_numbers
Objetivo: Faz parse/validacao de entrada de dados.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int parse_two_numbers(const char *line, double *a, double *b){
    char *end = NULL;
    double v1 = strtod(line, &end);
    if(end == line) return 0;
    while(*end && (*end == ' ' || *end == '\t')) end++;
    if(*end == ',' || *end == ';') end++;
    while(*end && (*end == ' ' || *end == '\t')) end++;
    double v2 = strtod(end, &end);
    if(end == NULL) return 0;
    *a = v1;
    *b = v2;
    return 1;
}

/*
Funcao: load_schedule
Objetivo: Executa responsabilidade especifica dentro do modulo.
Quando usar: Chamada pelo fluxo interno deste arquivo.
Entradas: Parametros declarados na assinatura.
Retorno: Retorna status/valor conforme contrato da funcao.
Efeitos colaterais: Pode alterar estado interno, IO ou logs conforme implementacao.
*/
static int load_schedule(const char *path, seg_t **out, int *count, double *total_s){
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    seg_t *arr = NULL;
    int n = 0;
    double total = 0.0;
    char line[256];
    while(fgets(line, sizeof(line), f)){
        trim(line);
        if(!line[0]) continue;
        if(line[0] == '#') continue;
        /* skip header line */
        if(isalpha((unsigned char)line[0])) continue;
        double a = 0.0, b = 0.0;
        if(!parse_two_numbers(line, &a, &b)) continue;
        int rpm = (int)(a >= 0 ? a + 0.5 : a - 0.5);
        double dur = b;
        if(dur <= 0) continue;
        seg_t s; s.rpm = rpm; s.dur_s = dur; s.t_end = total + dur;
        seg_t *tmp = (seg_t*)realloc(arr, (n + 1) * sizeof(seg_t));
        if(!tmp){ free(arr); fclose(f); return 0; }
        arr = tmp;
        arr[n++] = s;
        total += dur;
    }
    fclose(f);
    if(n <= 0){ free(arr); return 0; }
    *out = arr;
    *count = n;
    *total_s = total;
    return 1;
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
    puts("  a5_speed_logger --port COM4 --out <csv> --schedule <csv>");
    puts("                 [--rate <hz>] [--duration <s>] [--slave <id>]");
    puts("                 [--baud <n>] [--parity N|E|O] [--ipc] [--setup]");
    puts("                 [--encoder-calibration]");
    puts("                 [--reciprocating --recip-course-mm <mm> --recip-total-mm <mm>]");
    puts("                 [--recip-radius-mm <mm> --recip-ratio <i> --recip-tol-counts <n>]");
    puts("  --encoder-calibration exige --ipc e coleta somente P0B-09.");
    puts("  --self-test valida o parser IPC sem acessar hardware.");
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
    const char *port = "COM4";
    const char *out_path = NULL;
    const char *sched_path = NULL;
    int slave = 1;
    int baud = 115200;
    char parity = 'E';
    int databits = 8;
    int stopbits = 1;
    double rate_hz = 200.0;
    double duration_s = 0.0;
    int use_ipc = 0;
    int do_setup = 0;
    int encoder_calibration = 0;
    int self_test = 0;
    recip_state_t recip = {0};
    recip.relacao = 1.0;

    for(int i = 1; i < argc; ++i){
        if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0){
            print_usage();
            return 0;
        }
        if(strcmp(argv[i], "--self-test") == 0){
            self_test = 1;
            continue;
        }
        if(strcmp(argv[i], "--port") == 0 && i + 1 < argc){ port = argv[++i]; continue; }
        if(strcmp(argv[i], "--out") == 0 && i + 1 < argc){ out_path = argv[++i]; continue; }
        if(strcmp(argv[i], "--schedule") == 0 && i + 1 < argc){ sched_path = argv[++i]; continue; }
        if(strcmp(argv[i], "--rate") == 0 && i + 1 < argc){ rate_hz = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--duration") == 0 && i + 1 < argc){ duration_s = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--slave") == 0 && i + 1 < argc){ slave = atoi(argv[++i]); continue; }
        if(strcmp(argv[i], "--baud") == 0 && i + 1 < argc){ baud = atoi(argv[++i]); continue; }
        if(strcmp(argv[i], "--parity") == 0 && i + 1 < argc){ parity = (char)toupper((unsigned char)argv[++i][0]); continue; }
        if(strcmp(argv[i], "--ipc") == 0){ use_ipc = 1; continue; }
        if(strcmp(argv[i], "--setup") == 0){ do_setup = 1; continue; }
        if(strcmp(argv[i], "--encoder-calibration") == 0){ encoder_calibration = 1; continue; }
        if(strcmp(argv[i], "--reciprocating") == 0){ recip.enabled = 1; continue; }
        if(strcmp(argv[i], "--recip-course-mm") == 0 && i + 1 < argc){ recip.course_mm = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--recip-total-mm") == 0 && i + 1 < argc){ recip.total_mm = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--recip-radius-mm") == 0 && i + 1 < argc){ recip.raio_mm = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--recip-ratio") == 0 && i + 1 < argc){ recip.relacao = atof(argv[++i]); continue; }
        if(strcmp(argv[i], "--recip-tol-counts") == 0 && i + 1 < argc){ recip.tol_counts = atoi(argv[++i]); continue; }
        print_usage();
        return 1;
    }
    if(self_test){
        return run_internal_self_test() ? 0 : 1;
    }
    if(!out_path || !sched_path){
        print_usage();
        return 1;
    }
    if(encoder_calibration && !use_ipc){
        fprintf(stderr, "--encoder-calibration exige --ipc.\n");
        return 1;
    }
    if(encoder_calibration && !do_setup){
        fprintf(
            stderr,
            "--encoder-calibration exige --setup para validar "
            "o modo de velocidade antes de READY.\n"
        );
        return 1;
    }
    if(encoder_calibration && recip.enabled){
        fprintf(stderr, "--encoder-calibration nao pode ser combinado com --reciprocating.\n");
        return 1;
    }
    if(encoder_calibration &&
       !SetConsoleCtrlHandler(console_ctrl_handler, TRUE)){
        fprintf(stderr, "Falha instalando handler de Ctrl+C.\n");
        return 1;
    }
    ev_open_for_out(out_path);
    ev_logf("START port=%s out=%s schedule=%s rate=%.3f duration=%.3f slave=%d baud=%d parity=%c ipc=%d setup=%d encoder_calibration=%d",
            port, out_path, sched_path, rate_hz, duration_s, slave, baud, parity, use_ipc, do_setup,
            encoder_calibration);
    if(encoder_calibration){
        ev_logf(
            "ENCODER_QPC_MODE valid_position=modbus_read_midpoint "
            "missing_slot=nominal_deadline"
        );
    }
    if(recip.enabled){
        ev_logf("RECIP_CONFIG course_mm=%.6f total_mm=%.6f radius_mm=%.6f ratio=%.6f tol_counts=%d",
                recip.course_mm, recip.total_mm, recip.raio_mm, recip.relacao, recip.tol_counts);
        if(recip.course_mm <= 0.0 || recip.total_mm <= 0.0 || recip.raio_mm <= 0.0 ||
           recip.relacao <= 0.0 || recip.tol_counts <= 0){
            fprintf(stderr, "Parametros reciprocantes invalidos.\n");
            ev_logf("ERROR invalid reciprocating parameters.");
            ev_close();
            return 1;
        }
    }

    seg_t *segs = NULL;
    int seg_count = 0;
    double sched_total = 0.0;
    if(!load_schedule(sched_path, &segs, &seg_count, &sched_total)){
        fprintf(stderr, "Falha lendo schedule: %s\n", sched_path);
        ev_logf("ERROR load schedule failed: %s", sched_path);
        ev_close();
        return 1;
    }
    if(duration_s <= 0.0) duration_s = sched_total;
    if(duration_s <= 0.0 || rate_hz <= 0.0){
        free(segs);
        ev_logf("ERROR invalid duration/rate after schedule parse.");
        ev_close();
        return 1;
    }

    FILE *f = _fsopen(out_path, "w", _SH_DENYNO);
    if(!f){
        fprintf(stderr, "Falha abrindo CSV: %s\n", out_path);
        free(segs);
        ev_logf("ERROR open csv failed.");
        ev_close();
        return 1;
    }
    /* Desabilita buffering para consumo em tempo real por UI/agregador. */
    setvbuf(f, NULL, _IONBF, 0);
    fprintf(f, "idx,t_qpc,t_s,pos,rpm,pos_err,rpm_err,pos_mod\n");
    fflush(f);

    char port_path[64];
    make_port_path(port, port_path, sizeof(port_path));

    modbus_t *ctx = modbus_new_rtu(port_path, baud, parity, databits, stopbits);
    if(!ctx){
        fprintf(stderr, "modbus_new_rtu failed\n");
        fclose(f);
        free(segs);
        ev_logf("ERROR modbus_new_rtu failed.");
        ev_close();
        return 1;
    }
    modbus_set_slave(ctx, slave);
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);

    if(modbus_connect(ctx) == -1){
        fprintf(stderr, "connect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        fclose(f);
        free(segs);
        ev_logf("ERROR modbus connect failed: %s", modbus_strerror(errno));
        ev_close();
        return 1;
    }
    ev_logf("Modbus connected.");

    if(do_setup){
        int setup_result;
        int setup_stop_rpm = 0;
        int pre_setup_stop_ok = 1;
        int post_setup_stop_ok = 1;

        ev_logf("Applying setup_speed_mode.");
        if(encoder_calibration){
            pre_setup_stop_ok =
                stop_drive_now(ctx, &setup_stop_rpm, 0);
            ev_logf(
                "ENCODER_SETUP_PRE_STOP confirmed=%d.",
                pre_setup_stop_ok
            );
        }
        setup_result =
            pre_setup_stop_ok
                ? setup_speed_mode(ctx, encoder_calibration)
                : -1;
        if(encoder_calibration){
            post_setup_stop_ok =
                stop_drive_now(ctx, &setup_stop_rpm, 0);
            ev_logf(
                "ENCODER_SETUP_POST_STOP confirmed=%d.",
                post_setup_stop_ok
            );
        }
        if(setup_result != 0 ||
           !pre_setup_stop_ok ||
           !post_setup_stop_ok){
            fprintf(
                stderr,
                encoder_calibration
                    ? "Falha configurando/confirmando o modo de "
                      "velocidade; READY nao sera emitido.\n"
                    : "Aviso: uma ou mais escritas do setup falharam.\n"
            );
            ev_logf(
                "ERROR SETUP_SPEED_MODE result=%d pre_stop=%d "
                "post_stop=%d strict=%d.",
                setup_result,
                pre_setup_stop_ok,
                post_setup_stop_ok,
                encoder_calibration
            );
            if(encoder_calibration){
                modbus_close(ctx);
                modbus_free(ctx);
                fclose(f);
                free(segs);
                ev_close();
                return 2;
            }
        }
    }

    /* Read word order once (used only if we ever need 32-bit regs) */
    uint16_t order = 1;
    int p0c26_mode = 0;
    if(read_u16_cached_retry(ctx, REG_P0C_26, &order, &p0c26_mode, 3, 20) != 0){
        order = 1;
    }
    int low_first = (order != 0);

    /* Read command units per revolution (P05-02) once for scaling. */
    uint16_t cmd_units_per_rev = 0;
    int p0502_mode = 0;
    if(read_u16_cached_retry(ctx, REG_P05_02, &cmd_units_per_rev, &p0502_mode, 5, 20) != 0){
        cmd_units_per_rev = 0;
    }

    int p0b09_mode = 0;
    int rpm_mode = 0;
    int encoder_stopped_emitted = 0;
    encoder_drive_status_t encoder_status = {0};

    if(encoder_calibration){
        int preflight_stop_rpm = 0;
        int preflight_stop_ok;
        int valid_reads = 0;
        int attempts = 0;
        uint16_t preflight_pos = 0;

        /*
        READY significa Drive conhecido e parado. Reforce RPM=0,
        CTRL RDY e P31-00=0 antes de validar P0B-09.
        */
        preflight_stop_ok =
            stop_drive_now(
                ctx,
                &preflight_stop_rpm,
                0
            );
        ev_logf(
            "ENCODER_PREFLIGHT_STOP confirmed=%d before READY.",
            preflight_stop_ok
        );
        if(!preflight_stop_ok){
            fprintf(
                stderr,
                "Falha confirmando os comandos de parada antes de READY.\n"
            );
            emit_encoder_stopped(
                encoder_calibration,
                0,
                &encoder_stopped_emitted
            );
            modbus_close(ctx);
            modbus_free(ctx);
            fclose(f);
            free(segs);
            ev_close();
            return 2;
        }
        set_timeouts_us(ctx, FAST_RESP_US, FAST_BYTE_US);
        while(valid_reads < ENCODER_CAL_PREFLIGHT_READS &&
              attempts < ENCODER_CAL_PREFLIGHT_ATTEMPTS &&
              !console_stop_requested()){
            attempts++;
            if(read_p0b09_cached(ctx, &preflight_pos, &p0b09_mode) == 0){
                valid_reads++;
            }else{
                valid_reads = 0;
            }
            if(valid_reads < ENCODER_CAL_PREFLIGHT_READS) Sleep(20);
        }
        if(valid_reads < ENCODER_CAL_PREFLIGHT_READS){
            int stopped_rpm = 0;
            int stop_ok;
            int cancelled = console_stop_requested();
            fprintf(
                stderr,
                cancelled
                    ? "Calibracao do encoder cancelada antes de READY.\n"
                    : "Falha: P0B-09 nao forneceu 3 leituras validas antes de READY.\n"
            );
            ev_logf(
                "ERROR ENCODER_PREFLIGHT valid=%d required=%d attempts=%d cancelled=%d",
                valid_reads,
                ENCODER_CAL_PREFLIGHT_READS,
                attempts,
                cancelled
            );
            stop_ok =
                stop_drive_now(ctx, &stopped_rpm, 0);
            emit_encoder_stopped(
                encoder_calibration,
                stop_ok,
                &encoder_stopped_emitted
            );
            modbus_close(ctx);
            modbus_free(ctx);
            fclose(f);
            free(segs);
            ev_close();
            return cancelled && stop_ok ? 0 : 2;
        }
        ev_logf(
            "ENCODER_PREFLIGHT_OK reads=%d attempts=%d last_pos=%u",
            valid_reads,
            attempts,
            (unsigned)preflight_pos
        );
        encoder_status_accept_position(
            &encoder_status,
            preflight_pos,
            qpc_now_ticks()
        );
    }

    if(use_ipc){
        puts("READY");
        fflush(stdout);
        ev_logf("READY emitted; waiting START.");
        if(encoder_calibration){
            ipc_cmd_t startup_cmd = IPC_CMD_NONE;
            while(startup_cmd == IPC_CMD_NONE){
                startup_cmd = ipc_poll_command(1, 1);
                if(startup_cmd == IPC_CMD_NONE) Sleep(10);
            }
            if(startup_cmd != IPC_CMD_START){
                int stopped_rpm = 0;
                int stop_ok;
                ev_logf(
                    "IPC STOP before START command=%d console_stop=%d",
                    (int)startup_cmd,
                    console_stop_requested()
                );
                stop_ok =
                    stop_drive_now(ctx, &stopped_rpm, 0);
                emit_encoder_stopped(
                    encoder_calibration,
                    stop_ok,
                    &encoder_stopped_emitted
                );
                modbus_close(ctx);
                modbus_free(ctx);
                fclose(f);
                free(segs);
                ev_close();
                return stop_ok ? 0 : 2;
            }
        }else{
            char line[64];
            if(!fgets(line, sizeof(line), stdin)){
                modbus_close(ctx);
                modbus_free(ctx);
                fclose(f);
                free(segs);
                ev_logf("IPC START read failed.");
                ev_close();
                return 1;
            }
            trim(line);
            if(_stricmp(line, "START") != 0){
                modbus_close(ctx);
                modbus_free(ctx);
                fclose(f);
                free(segs);
                ev_logf("IPC START invalid token: %s", line);
                ev_close();
                return 1;
            }
        }
        ev_logf("IPC START received.");
    }

    if(recip.enabled){
        uint16_t init_raw = 0;
        int init_ok = -1;
        for(int k = 0; k < 10; ++k){
            if(read_p0b09_cached(ctx, &init_raw, &p0b09_mode) == 0){
                init_ok = 0;
                break;
            }
            Sleep(20);
        }
        if(init_ok != 0){
            fprintf(stderr, "Falha lendo posicao inicial para modo reciprocante.\n");
            ev_logf("ERROR RECIP_INIT no initial P0B-09.");
            modbus_close(ctx);
            modbus_free(ctx);
            fclose(f);
            free(segs);
            ev_close();
            return 1;
        }
        uint32_t pos_mod_for_recip = (cmd_units_per_rev > 0) ? (uint32_t)cmd_units_per_rev : 65536u;
        int32_t init_pos = scale_pos_raw(init_raw, cmd_units_per_rev);
        if(recip_init(&recip, init_pos, pos_mod_for_recip, rate_hz) != 0){
            fprintf(stderr, "Falha inicializando modo reciprocante: %s\n", recip.abort_msg);
            ev_logf("ERROR RECIP_INIT %s", recip.abort_msg);
            modbus_close(ctx);
            modbus_free(ctx);
            fclose(f);
            free(segs);
            ev_close();
            return 1;
        }
        ev_logf("RECIP_INIT pos=%ld pos_mod=%u start=%lld end=%lld stroke_counts=%lld total_counts=%lld miss_limit=%d",
                (long)init_pos,
                (unsigned)recip.pos_mod,
                (long long)recip.start_pos,
                (long long)recip.end_pos,
                (long long)recip.stroke_counts,
                (long long)recip.total_counts,
                recip.miss_limit);
    }

    int seg_idx = 0;
    int schedule_idx = 0;
    int total_samples = (int)(duration_s * rate_hz + 0.5);
    int64_t qpc_freq = qpc_freq_ticks();
    int64_t dt_ticks = (int64_t)((double)qpc_freq / rate_hz + 0.5);
    int64_t ramp_ticks = (int64_t)(RAMP_TIME_S * (double)qpc_freq + 0.5);
    int64_t start_ticks = qpc_now_ticks();
    int64_t next_ticks = start_ticks;

    int stop_requested = 0;
    int stop_sent = 0;
    int paused = 0;
    int64_t pause_start_ticks = 0;
    int64_t hard_stop_ticks = start_ticks + (int64_t)(duration_s * (double)qpc_freq);
    int64_t recip_done_ticks = 0;
    int64_t recip_postroll_limit_ticks = (int64_t)(2.0 * (double)qpc_freq + 0.5);
    int current_cmd_rpm = 0;
    int desired_seg_rpm = recip.enabled ? recip_target_rpm(&recip, segs, 0) : segs[0].rpm;
    rpm_ramp_t rpm_ramp = {0, 0, 0, start_ticks, start_ticks};
    DWORD next_progress_log_ms = GetTickCount() + 1000;
    int n_pos_err = 0;
    int n_rpm_err = 0;
    int encoder_abort = 0;
    int encoder_stop_confirmed = 1;
    int encoder_consecutive_misses = 0;
    int encoder_ramp_write_failures = 0;
    int64_t encoder_last_pos_ok_ticks = start_ticks;
    int64_t stop_ramp_ticks =
        recip.enabled ? 0 : ramp_ticks;

    /* Smooth first engagement: start at 0 rpm and ramp to the first segment target. */
    if(encoder_calibration){
        if(cmd_rpm(ctx, 0) != 0){
            fprintf(stderr, "Falha zerando setpoint antes do RUN.\n");
            ev_logf("FATAL ENCODER_START setpoint_zero_failed.");
            encoder_abort = 1;
            stop_requested = 1;
            goto finish_run;
        }
        if(cmd_run(ctx) != 0){
            fprintf(stderr, "Falha RUN; calibracao abortada.\n");
            ev_logf("FATAL ENCODER_START cmd_run_failed.");
            encoder_abort = 1;
            stop_requested = 1;
            goto finish_run;
        }
        current_cmd_rpm = 0;
        ramp_begin(
            &rpm_ramp,
            current_cmd_rpm,
            desired_seg_rpm,
            start_ticks,
            ramp_ticks
        );
        ev_logf(
            "RUN started encoder calibration; target rpm=%d "
            "ramp_s=%.3f total_samples=%d",
            desired_seg_rpm,
            RAMP_TIME_S,
            total_samples
        );
        puts("STARTED");
        fflush(stdout);
        ev_logf("STARTED emitted.");
        encoder_status.next_emit_ms =
            GetTickCount64() + 1000ULL;
    }else if(recip.enabled){
        (void)cmd_rpm(ctx, 0);
        if(cmd_run(ctx) != 0){
            fprintf(stderr, "Falha RUN.\n");
            ev_logf("WARN cmd_run failed at start.");
        }
        if(cmd_rpm(ctx, desired_seg_rpm) == 0){
            current_cmd_rpm = desired_seg_rpm;
        }
        ramp_begin(&rpm_ramp, current_cmd_rpm, current_cmd_rpm, start_ticks, 0);
        ev_logf("RUN started reciprocating; first target rpm=%d total_samples=%d", desired_seg_rpm, total_samples);
    }else{
        (void)cmd_rpm(ctx, 0);
        if(cmd_run(ctx) != 0){
            fprintf(stderr, "Falha RUN.\n");
            ev_logf("WARN cmd_run failed at start.");
        }
        ramp_begin(&rpm_ramp, current_cmd_rpm, desired_seg_rpm, start_ticks, ramp_ticks);
        ev_logf("RUN started; first target rpm=%d total_samples=%d", desired_seg_rpm, total_samples);
    }

    for(int idx = 0; idx < total_samples; ){
        if(console_stop_requested()){
            ev_logf("Console stop requested at idx=%d/%d", idx, total_samples);
            stop_requested = 1;
            break;
        }
        ipc_cmd_t ipc_cmd =
            ipc_poll_command(
                use_ipc,
                encoder_calibration
            );
        if(ipc_cmd == IPC_CMD_STOP){
            ev_logf("STOP received at idx=%d/%d", idx, total_samples);
            stop_requested = 1;
            break;
        }
        if(ipc_cmd == IPC_CMD_PAUSE && encoder_calibration){
            ev_logf(
                "PAUSE treated as STOP in encoder calibration "
                "at idx=%d/%d",
                idx,
                total_samples
            );
            stop_requested = 1;
            break;
        }
        if(ipc_cmd == IPC_CMD_PAUSE && !paused){
            paused = 1;
            pause_start_ticks = qpc_now_ticks();
            stop_drive_now(ctx, &current_cmd_rpm, stop_ramp_ticks);
            current_cmd_rpm = 0;
            ramp_begin(&rpm_ramp, 0, 0, pause_start_ticks, 0);
            ev_logf("PAUSE received at idx=%d", idx);
        }else if(ipc_cmd == IPC_CMD_RESUME && paused){
            int64_t now_resume = qpc_now_ticks();
            int64_t paused_ticks = now_resume - pause_start_ticks;
            if(paused_ticks < 0) paused_ticks = 0;
            start_ticks += paused_ticks;
            next_ticks += paused_ticks;
            hard_stop_ticks += paused_ticks;
            paused = 0;
            if(!stop_sent){
                desired_seg_rpm = recip.enabled ? recip_target_rpm(&recip, segs, seg_idx) : segs[seg_idx].rpm;
                if(encoder_calibration){
                    if(cmd_rpm(ctx, 0) != 0 ||
                       cmd_run(ctx) != 0){
                        fprintf(
                            stderr,
                            "Falha retomando calibracao do encoder.\n"
                        );
                        ev_logf(
                            "FATAL ENCODER_RESUME rpm=%d",
                            desired_seg_rpm
                        );
                        encoder_abort = 1;
                        stop_requested = 1;
                        break;
                    }
                    current_cmd_rpm = 0;
                    encoder_consecutive_misses = 0;
                    encoder_ramp_write_failures = 0;
                    encoder_last_pos_ok_ticks = now_resume;
                    ramp_begin(
                        &rpm_ramp,
                        current_cmd_rpm,
                        desired_seg_rpm,
                        now_resume,
                        ramp_ticks
                    );
                    puts("STARTED");
                    fflush(stdout);
                    ev_logf("STARTED emitted after RESUME.");
                }else if(recip.enabled){
                    (void)cmd_run(ctx);
                    if(cmd_rpm(ctx, desired_seg_rpm) == 0){
                        current_cmd_rpm = desired_seg_rpm;
                    }
                    ramp_begin(&rpm_ramp, current_cmd_rpm, current_cmd_rpm, now_resume, 0);
                }else{
                    (void)cmd_run(ctx);
                    ramp_begin(&rpm_ramp, current_cmd_rpm, desired_seg_rpm, now_resume, ramp_ticks);
                }
            }
            ev_logf("RESUME received at idx=%d seg=%d target_rpm=%d", idx, seg_idx, desired_seg_rpm);
        }

        if(paused){
            if(encoder_calibration){
                encoder_status_emit(
                    &encoder_status,
                    current_cmd_rpm,
                    n_pos_err,
                    qpc_now_ticks(),
                    qpc_freq,
                    0
                );
            }
            Sleep(2);
            continue;
        }

        int64_t now = qpc_now_ticks();
        if(!stop_sent && now >= hard_stop_ticks){
            stop_drive_now(ctx, &current_cmd_rpm, stop_ramp_ticks);
            current_cmd_rpm = 0;
            ramp_begin(&rpm_ramp, 0, 0, now, 0);
            stop_sent = 1;
            ev_logf("Hard stop deadline reached at idx=%d.", idx);
            if(recip.enabled && !recip.done && recip.accum_counts < (double)recip.total_counts){
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "RECIP_DISTANCE_NOT_REACHED idx=%d accum_counts=%.0f total_counts=%lld",
                         idx, recip.accum_counts, (long long)recip.total_counts);
                recip_abort(&recip, msg);
                fprintf(stderr, "%s\n", recip.abort_msg);
                ev_logf("FATAL %s", recip.abort_msg);
                stop_requested = 1;
                break;
            }
        }
        if(now < next_ticks){
            if(encoder_calibration){
                encoder_status_emit(
                    &encoder_status,
                    current_cmd_rpm,
                    n_pos_err,
                    now,
                    qpc_freq,
                    0
                );
            }
            Sleep(1);
            continue;
        }

        if(!stop_sent && !recip.enabled){
            int rpm_cmd_now = ramp_eval(&rpm_ramp, now);
            if(rpm_ramp.active && now >= rpm_ramp.t_end){
                rpm_ramp.active = 0;
                rpm_cmd_now = rpm_ramp.target_rpm;
            }
            if(rpm_cmd_now != current_cmd_rpm){
                if(cmd_rpm(ctx, rpm_cmd_now) == 0){
                    current_cmd_rpm = rpm_cmd_now;
                    if(encoder_calibration){
                        encoder_ramp_write_failures = 0;
                    }
                }else if(encoder_calibration){
                    encoder_ramp_write_failures++;
                    if(encoder_ramp_write_failures >= 3){
                        fprintf(
                            stderr,
                            "Falha repetida aplicando a rampa; "
                            "calibracao abortada.\n"
                        );
                        ev_logf(
                            "FATAL ENCODER_RAMP_WRITE "
                            "target=%d failures=%d",
                            rpm_cmd_now,
                            encoder_ramp_write_failures
                        );
                        encoder_abort = 1;
                        stop_requested = 1;
                        break;
                    }
                }
            }
        }else if(!stop_sent && recip.enabled && !recip.stop_pending && !recip.done){
            int rpm_cmd_now = recip_target_rpm(&recip, segs, seg_idx);
            if(rpm_cmd_now != current_cmd_rpm){
                if(cmd_rpm(ctx, rpm_cmd_now) == 0){
                    current_cmd_rpm = rpm_cmd_now;
                }
            }
        }

        int sampled_pos_ok = 0;
        int sampled_rpm_ok = 0;
        int32_t sampled_pos = 0;
        int16_t sampled_rpm = 0;
        int64_t sampled_pos_qpc = 0;
        int64_t pos_read_start_qpc;
        int64_t pos_read_end_qpc;
        int pos_read_result;
        set_timeouts_us(ctx, FAST_RESP_US, FAST_BYTE_US);
        uint16_t pos_raw = 0;
        pos_read_start_qpc = qpc_now_ticks();
        pos_read_result =
            read_p0b09_cached(ctx, &pos_raw, &p0b09_mode);
        pos_read_end_qpc = qpc_now_ticks();
        if(pos_read_result == 0){
            sampled_pos_qpc =
                pos_read_start_qpc +
                (pos_read_end_qpc - pos_read_start_qpc) / 2;
            if(cmd_units_per_rev > 0){
                /* Escala 0..65535 -> 0..(P05-02-1) */
                sampled_pos = (int32_t)(((uint32_t)pos_raw * (uint32_t)cmd_units_per_rev) / 65536u);
            }else{
                sampled_pos = (int32_t)pos_raw;
            }
            sampled_pos_ok = 1;
            if(encoder_calibration){
                encoder_status_accept_position(
                    &encoder_status,
                    pos_raw,
                    sampled_pos_qpc
                );
            }
        }
        if(!encoder_calibration &&
           read_rpm_cached(ctx, &sampled_rpm, &rpm_mode) == 0){
            sampled_rpm_ok = 1;
        }
        if(!sampled_pos_ok) n_pos_err++;
        if(!encoder_calibration && !sampled_rpm_ok) n_rpm_err++;

        if(encoder_calibration && !stop_sent){
            if(sampled_pos_ok){
                encoder_consecutive_misses = 0;
                encoder_last_pos_ok_ticks = now;
            }else{
                double missing_s;
                encoder_consecutive_misses++;
                missing_s =
                    (double)(now - encoder_last_pos_ok_ticks) /
                    (double)qpc_freq;
                if(missing_s >= ENCODER_CAL_POSITION_LOSS_S){
                    fprintf(
                        stderr,
                        "Perda de P0B-09 por %.3f s; "
                        "calibracao abortada.\n",
                        missing_s
                    );
                    ev_logf(
                        "FATAL ENCODER_POSITION_LOSS "
                        "consecutive_misses=%d missing_s=%.3f limit_s=%.3f",
                        encoder_consecutive_misses,
                        missing_s,
                        ENCODER_CAL_POSITION_LOSS_S
                    );
                    encoder_abort = 1;
                    stop_requested = 1;
                    stop_drive_now(ctx, &current_cmd_rpm, 0);
                    current_cmd_rpm = 0;
                    stop_sent = 1;
                    encoder_status_emit(
                        &encoder_status,
                        current_cmd_rpm,
                        n_pos_err,
                        qpc_now_ticks(),
                        qpc_freq,
                        1
                    );
                    break;
                }
            }
        }

        if(encoder_calibration){
            encoder_status_emit(
                &encoder_status,
                current_cmd_rpm,
                n_pos_err,
                qpc_now_ticks(),
                qpc_freq,
                0
            );
        }

        if(recip.enabled && !stop_sent){
            if(sampled_pos_ok){
                recip.miss_count = 0;
                (void)recip_update_unwrapped(&recip, sampled_pos);

                int64_t stroke_travel_counts = i64_abs_local(
                    recip.pos_unwrapped - recip.stroke_start_pos
                );
                if(recip_stroke_limit_reached(&recip, recip.pos_unwrapped)){
                    char msg[192];
                    snprintf(msg, sizeof(msg),
                             "RECIP_STROKE_LIMIT idx=%d travel_counts=%lld limit_counts=%lld pos=%lld start=%lld",
                             idx,
                             (long long)stroke_travel_counts,
                             (long long)(recip.stroke_counts * 2),
                             (long long)recip.pos_unwrapped,
                             (long long)recip.stroke_start_pos);
                    recip_abort(&recip, msg);
                }else if(recip.stop_pending){
                    int old_dir = recip.direction;
                    int completed_seg_idx = seg_idx;
                    int64_t completed_start = recip.stroke_start_pos;
                    int64_t completed_counts = i64_abs_local(
                        recip.pos_unwrapped - completed_start
                    );
                    int64_t error_counts = completed_counts - recip.stroke_counts;
                    int64_t endpoint_error = recip.pos_unwrapped - recip.stop_target;
                    int within_tol = recip_in_band(
                        &recip, recip.pos_unwrapped, recip.stop_target
                    );
                    recip.stop_pending = 0;
                    if(recip.accum_counts >= (double)recip.total_counts){
                        stop_drive_now(ctx, &current_cmd_rpm, 0);
                        current_cmd_rpm = 0;
                        stop_sent = 1;
                        recip.done = 1;
                        recip_done_ticks = now;
                        ev_logf("RECIP_DONE idx=%d accum_counts=%.0f total_counts=%lld pos=%lld target=%lld stroke_start=%lld stroke_counts_actual=%lld error_counts=%lld endpoint_error=%lld within_tol=%d tol=%d completed_segment=%d target_rpm_abs=%d postroll_max_s=2.000",
                                idx,
                                recip.accum_counts,
                                (long long)recip.total_counts,
                                (long long)recip.pos_unwrapped,
                                (long long)recip.stop_target,
                                (long long)completed_start,
                                (long long)completed_counts,
                                (long long)error_counts,
                                (long long)endpoint_error,
                                within_tol,
                                recip.tol_counts,
                                completed_seg_idx,
                                rpm_abs_safe(segs[completed_seg_idx].rpm));
                        puts("RECIP_MOTION_DONE");
                        fflush(stdout);
                    }else{
                        recip.stroke_start_pos = recip.pos_unwrapped;
                        recip.direction = -recip.direction;
                        if(schedule_idx != seg_idx){
                            ev_logf("RECIP_SEGMENT_APPLY idx=%d previous=%d next=%d boundary=stroke",
                                    idx, seg_idx, schedule_idx);
                            seg_idx = schedule_idx;
                        }
                        desired_seg_rpm = recip_target_rpm(&recip, segs, seg_idx);
                        if(cmd_run(ctx) != 0){
                            ev_logf("WARN RECIP cmd_run failed before reverse.");
                        }
                        if(cmd_rpm(ctx, desired_seg_rpm) == 0){
                            current_cmd_rpm = desired_seg_rpm;
                        }
                        ev_logf("RECIP_REVERSE idx=%d old_dir=%d new_dir=%d pos=%lld target=%lld rpm=%d accum_counts=%.0f stroke_start=%lld stroke_counts_actual=%lld error_counts=%lld endpoint_error=%lld within_tol=%d tol=%d completed_segment=%d next_segment=%d target_rpm_abs=%d",
                                idx, old_dir, recip.direction,
                                (long long)recip.pos_unwrapped,
                                (long long)recip.stop_target,
                                desired_seg_rpm,
                                recip.accum_counts,
                                (long long)completed_start,
                                (long long)completed_counts,
                                (long long)error_counts,
                                (long long)endpoint_error,
                                within_tol,
                                recip.tol_counts,
                                completed_seg_idx,
                                seg_idx,
                                rpm_abs_safe(segs[completed_seg_idx].rpm));
                    }
                }else{
                    int64_t target = (recip.direction >= 0) ? recip.end_pos : recip.start_pos;
                    int reached_target = recip_target_reached(
                        &recip, recip.pos_unwrapped, target
                    );

                    if(reached_target){
                        if(cmd_rpm(ctx, 0) == 0){
                            current_cmd_rpm = 0;
                        }
                        recip.stop_pending = 1;
                        recip.stop_target = target;
                        ev_logf("RECIP_STOP_TRIGGER idx=%d dir=%d pos=%lld target=%lld endpoint_error=%lld accum_counts=%.0f",
                                idx,
                                recip.direction,
                                (long long)recip.pos_unwrapped,
                                (long long)target,
                                (long long)(recip.pos_unwrapped - target),
                                recip.accum_counts);
                    }
                }
            }else{
                recip.miss_count++;
                if(recip.miss_count > recip.miss_limit){
                    char msg[192];
                    snprintf(msg, sizeof(msg),
                             "RECIP_POSITION_LOSS idx=%d consecutive_misses=%d limit=%d",
                             idx, recip.miss_count, recip.miss_limit);
                    recip_abort(&recip, msg);
                }
            }

            if(recip.abort){
                fprintf(stderr, "%s\n", recip.abort_msg);
                ev_logf("FATAL %s", recip.abort_msg);
                stop_drive_now(ctx, &current_cmd_rpm, 0);
                current_cmd_rpm = 0;
                stop_requested = 1;
                break;
            }
        }

        /* Lost slots are explicit NULL. We never copy position values. */
        while(idx < total_samples && now >= (next_ticks + dt_ticks)){
            double t_s = (double)idx / rate_hz;
            int64_t t_qpc = start_ticks + (int64_t)idx * dt_ticks;

            while(schedule_idx + 1 < seg_count && t_s >= segs[schedule_idx].t_end){
                schedule_idx++;
            }
            {
                int next_desired;
                if(!recip.enabled) seg_idx = schedule_idx;
                next_desired = recip.enabled ? recip_target_rpm(&recip, segs, seg_idx) : segs[seg_idx].rpm;
                if(!stop_sent && next_desired != desired_seg_rpm){
                    desired_seg_rpm = next_desired;
                    if(recip.enabled){
                        if(!recip.stop_pending && !recip.done && cmd_rpm(ctx, desired_seg_rpm) == 0){
                            current_cmd_rpm = desired_seg_rpm;
                        }
                    }else{
                        ramp_begin(&rpm_ramp, current_cmd_rpm, desired_seg_rpm, now, ramp_ticks);
                    }
                }
            }

            fprintf(f, "%d,%lld,%.6f,NULL,NULL,1,%d,%u\n",
                    idx, (long long)t_qpc, t_s,
                    encoder_calibration ? 0 : 1,
                    (unsigned)(cmd_units_per_rev > 0 ? cmd_units_per_rev : 65536u));
            idx++;
            next_ticks += dt_ticks;
        }

        if(idx < total_samples && now >= next_ticks){
            double t_s = (double)idx / rate_hz;
            int64_t nominal_qpc =
                start_ticks + (int64_t)idx * dt_ticks;
            int64_t t_qpc =
                encoder_calibration &&
                sampled_pos_ok &&
                sampled_pos_qpc > 0
                    ? sampled_pos_qpc
                    : nominal_qpc;

            while(schedule_idx + 1 < seg_count && t_s >= segs[schedule_idx].t_end){
                schedule_idx++;
            }
            {
                int next_desired;
                if(!recip.enabled) seg_idx = schedule_idx;
                next_desired = recip.enabled ? recip_target_rpm(&recip, segs, seg_idx) : segs[seg_idx].rpm;
                if(!stop_sent && next_desired != desired_seg_rpm){
                    desired_seg_rpm = next_desired;
                    if(recip.enabled){
                        if(!recip.stop_pending && !recip.done && cmd_rpm(ctx, desired_seg_rpm) == 0){
                            current_cmd_rpm = desired_seg_rpm;
                        }
                    }else{
                        ramp_begin(&rpm_ramp, current_cmd_rpm, desired_seg_rpm, now, ramp_ticks);
                    }
                }
            }

            if(sampled_pos_ok) fprintf(f, "%d,%lld,%.6f,%ld,", idx, (long long)t_qpc, t_s, (long)sampled_pos);
            else               fprintf(f, "%d,%lld,%.6f,NULL,", idx, (long long)t_qpc, t_s);

            if(sampled_rpm_ok) fprintf(f, "%d,", (int)sampled_rpm);
            else               fprintf(f, "NULL,");

            fprintf(f, "%d,%d,%u\n",
                    sampled_pos_ok ? 0 : 1,
                    encoder_calibration ? 0 : (sampled_rpm_ok ? 0 : 1),
                    (unsigned)(cmd_units_per_rev > 0 ? cmd_units_per_rev : 65536u));
            idx++;
            next_ticks += dt_ticks;
        }
        {
            DWORD now_ms = GetTickCount();
            if((LONG)(now_ms - next_progress_log_ms) >= 0){
                ev_logf("PROGRESS idx=%d/%d seg=%d cmd_rpm=%d stop_sent=%d paused=%d pos_err=%d rpm_err=%d",
                        idx, total_samples, seg_idx, current_cmd_rpm, stop_sent, paused, n_pos_err, n_rpm_err);
                next_progress_log_ms = now_ms + 1000;
            }
        }
        if(recip.enabled && recip.done && recip_done_ticks > 0 &&
           (now - recip_done_ticks) >= recip_postroll_limit_ticks){
            ev_logf("RECIP_POSTROLL_TIMEOUT idx=%d elapsed_s=%.3f",
                    idx, (double)(now - recip_done_ticks) / (double)qpc_freq);
            break;
        }
    }

finish_run:
    if(stop_requested){
        puts(
            (encoder_abort || recip.abort)
                ? "STOP requested (internal failure)."
                : console_stop_requested()
                ? "STOP requested (console)."
                : "STOP requested (IPC)."
        );
        fflush(stdout);
    }
    if(encoder_calibration){
        encoder_stop_confirmed =
            stop_drive_now(
                ctx,
                &current_cmd_rpm,
                stop_sent ? 0 : stop_ramp_ticks
            );
        stop_sent = 1;
        if(!encoder_stop_confirmed){
            fprintf(
                stderr,
                "Falha confirmando os comandos reforcados de parada.\n"
            );
            ev_logf("FATAL ENCODER_STOP_UNCONFIRMED.");
            encoder_abort = 1;
        }
    }else if(!stop_sent){
        (void)stop_drive_now(
            ctx,
            &current_cmd_rpm,
            stop_ramp_ticks
        );
    }
    emit_encoder_stopped(
        encoder_calibration,
        encoder_stop_confirmed,
        &encoder_stopped_emitted
    );
    ev_logf("END stop_requested=%d stop_sent=%d pos_err=%d rpm_err=%d recip_abort=%d encoder_abort=%d msg=%s",
            stop_requested, stop_sent, n_pos_err, n_rpm_err, recip.abort, encoder_abort,
            recip.abort_msg);
    modbus_close(ctx);
    modbus_free(ctx);
    fclose(f);
    free(segs);
    ev_close();
    return (recip.abort || encoder_abort) ? 2 : 0;
}
