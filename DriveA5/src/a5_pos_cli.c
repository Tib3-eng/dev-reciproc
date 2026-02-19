// Simple position command for Lichuan A5 via Modbus RTU (internal multi-segment)
// Uses P11 segment 1 and VDI to trigger PosInSen.
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>

#include <modbus.h>

// ---- Defaults
#define DEFAULT_SLAVE 1
#define DEFAULT_BAUD 115200
#define DEFAULT_PARITY 'E'
#define DEFAULT_DATABITS 8
#define DEFAULT_STOPBITS 1

// ---- Modbus registers (Pxx-yy -> 0xXXYY)
#define REG_P02_00 0x0200
#define REG_P03_02 0x0302
#define REG_P05_00 0x0500
#define REG_P05_02 0x0502
#define REG_P05_30 0x0530
#define REG_P11_00 0x1100
#define REG_P11_01 0x1101
#define REG_P11_04 0x1104
#define REG_P11_12 0x110C  // 32-bit
#define REG_P11_14 0x110E
#define REG_P11_15 0x110F
#define REG_P11_16 0x1110

#define REG_P0B_09 0x0B09
#define REG_P0B_03 0x0B03
#define REG_P0B_13 0x0B0D  // 32-bit
#define REG_P0B_15 0x0B0F  // 32-bit
#define REG_P0B_33 0x0B21
#define REG_P0B_34 0x0B22
#define REG_P0C_00 0x0C00
#define REG_P0C_02 0x0C02
#define REG_P0C_03 0x0C03
#define REG_P0C_09 0x0C09
#define REG_P0C_26 0x0C26

#define REG_P17_00 0x1700
#define REG_P17_01 0x1701
#define REG_P17_02 0x1702
#define REG_P17_03 0x1703
#define REG_P17_04 0x1704
#define REG_P17_05 0x1705
#define REG_P17_06 0x1706
#define REG_P17_07 0x1707
#define REG_P17_08 0x1708
#define REG_P17_09 0x1709
#define REG_P17_10 0x170A
#define REG_P17_11 0x170B

#define REG_P31_00 0x3100

// ---- FunIN codes (manual)
#define FUNIN_SON     1
#define FUNIN_POSINSEN 28

// ---- VDI bit mapping (used by this tool)
#define VDI_SON_BIT      0  // VDI1
#define VDI_POSINSEN_BIT 1  // VDI2 (per position-parameter doc)

// ---- Timeouts
#define CMD_RESP_US 50000
#define CMD_BYTE_US 50000

static int32_t g_zero_offset = 0;
static bool g_zero_valid = false;
static uint16_t g_word_order = 1;
static bool g_word_order_valid = false;
static int32_t g_units_per_rev = 0;
static bool g_units_valid = false;
static FILE *g_log = NULL;
static ULONGLONG g_log_t0 = 0;
static int32_t g_last_req = 0;
static int32_t g_last_meas = 0;
static int32_t g_last_err = 0;
static bool g_last_valid = false;

typedef struct {
    char port[64];
    int slave;
    int baud;
    char parity;
    int databits;
    int stopbits;
    int32_t pos;
    bool abs_mode;
    bool setup_vdi;
    bool dry_run;
    bool set_speed;
    bool set_accel;
    bool set_wait;
    int speed;
    int accel;
    int wait_ms;
    bool diag;
    bool osc_mode;
    int32_t osc_pos_a;
    int32_t osc_pos_b;
    int osc_cycles;
    int osc_dwell_ms;
    int verify_tol;
    int verify_timeout_ms;
} args_t;

static void trim(char *s){
    size_t n = strlen(s);
    while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
    char *p = s; while(*p && isspace((unsigned char)*p)) p++;
    if(p!=s) memmove(s,p,strlen(p)+1);
}

static int ask_line(const char *prompt, char *out, size_t outsz){
    printf("%s", prompt);
    if(!fgets(out, (int)outsz, stdin)) return 0;
    trim(out);
    return 1;
}

static int ask_int_default(const char *prompt, int *out, bool *is_set){
    char buf[64];
    if(!ask_line(prompt, buf, sizeof(buf))) return 0;
    if(buf[0] == 0){
        *is_set = false;
        return 1;
    }
    char *e = NULL;
    long v = strtol(buf, &e, 10);
    if(e == buf || *e) return 0;
    *out = (int)v;
    *is_set = true;
    return 1;
}

static void log_open(void){
    if(g_log) return;
    _mkdir("out");
    g_log = fopen("out\\a5_pos_log.csv", "a");
    if(!g_log) return;
    if(ftell(g_log) == 0){
        fprintf(g_log, "t_ms,tag,req_pos,send_pos,p0b07_raw,p0b07_logical,err,dev,cmd\n");
        fflush(g_log);
    }
    g_log_t0 = GetTickCount64();
}

static void log_close(void){
    if(!g_log) return;
    fclose(g_log);
    g_log = NULL;
}

static void log_sample(const char *tag, int32_t req_pos, int32_t send_pos,
                       int32_t raw_pos, int32_t logical_pos, int32_t err,
                       int32_t dev, int32_t cmd){
    if(!g_log) return;
    ULONGLONG t = GetTickCount64() - g_log_t0;
    fprintf(g_log, "%llu,%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
            (unsigned long long)t, tag ? tag : "",
            (long)req_pos, (long)send_pos, (long)raw_pos, (long)logical_pos,
            (long)err, (long)dev, (long)cmd);
    fflush(g_log);
}

// ----------------- COM list (QueryDosDevice)
typedef struct { char **items; int count; } com_list_t;

static void com_list_free(com_list_t *L){
    if(!L) return;
    for(int i=0;i<L->count;++i) free(L->items[i]);
    free(L->items);
    L->items = NULL;
    L->count = 0;
}

static com_list_t com_list_detect(void){
    com_list_t L = {0};
    DWORD cap = 64*1024;
    char *buf = (char*)malloc(cap);
    if(!buf) return L;

    DWORD n = QueryDosDeviceA(NULL, buf, cap);
    if(n==0){ free(buf); return L; }

    for(char *p=buf; *p; p += strlen(p)+1){
        if(strncmp(p,"COM",3)==0 && isdigit((unsigned char)p[3])){
            L.items = (char**)realloc(L.items, (L.count+1)*sizeof(char*));
            L.items[L.count] = _strdup(p);
            L.count++;
        }
    }
    free(buf);
    return L;
}

static void com_list_print(const com_list_t *L){
    if(!L || !L->count){ puts("No COM ports found."); return; }
    puts("Detected COM ports:");
    for(int i=0;i<L->count;++i) printf("  [%d] %s\n", i+1, L->items[i]);
}

static void print_usage(void){
    puts("Uso:");
    puts("  a5_pos_cli COMx <pos> [--abs|--rel] [--speed <rpm>] [--accel <ms>] [--wait <ms>]");
    puts("             [--setup] [--slave <id>] [--baud <n>] [--parity N|E|O] [--dry-run]");
    puts("  a5_pos_cli COMx --osc [--cycles <n>] [--dwell <ms>] [--pos-a <n>] [--pos-b <n>]");
    puts("  a5_pos_cli COMx --diag [--slave <id>] [--baud <n>] [--parity N|E|O]");
    puts("");
    puts("Notas:");
    puts("  - Usa posicao interna multi-segmento (P05-00=2) e segmento 1 (P11-12).");
    puts("  - Posicao em \"command unit\" (depende do encoder e da engrenagem eletronica).");
    puts("  - --setup configura VDI: VDI1=S-ON, VDI2=PosInSen (nivel).");
    puts("  - O comando aplica parametros em sequencia com log antes de RUN.");
    puts("  - --osc alterna entre posicoes (padrao: 0 e 15000).");
    puts("  - --diag faz testes de leitura (P0C e P0B-09) para validar comunicacao.");
}

static void set_timeouts_us(modbus_t *ctx, int resp_us, int byte_us){
    modbus_set_response_timeout(ctx, 0, resp_us);
    modbus_set_byte_timeout(ctx, 0, byte_us);
}

static void make_port_path(const char *shortName, char *out, size_t outsz){
    int num = 0;
    if(_strnicmp(shortName,"COM",3)==0) num = atoi(shortName+3);
    if(num >= 10) _snprintf(out, outsz, "\\\\.\\%s", shortName);
    else          _snprintf(out, outsz, "%s", shortName);
}

static int get_word_order(modbus_t *ctx, uint16_t *out){
    uint16_t v = 1;
    if(read_u16(ctx, REG_P0C_26, &v) == 0){
        g_word_order = v;
        g_word_order_valid = true;
        *out = v;
        return 0;
    }
    if(g_word_order_valid){
        *out = g_word_order;
        puts("WARN: P0C-26 read failed, using cached word order.");
        return 0;
    }
    *out = 1;
    puts("WARN: P0C-26 read failed, using default word order=1.");
    return -1;
}

static int get_units_per_rev(modbus_t *ctx, uint16_t order, int32_t *out){
    int32_t p05 = 0;
    if(read_s32_seq(ctx, REG_P05_02, (order != 0), &p05, 3, 80) == 0 && p05 > 0){
        g_units_per_rev = p05;
        g_units_valid = true;
        *out = p05;
        return 0;
    }
    uint16_t p05_u16 = 0;
    if(read_u16_seq(ctx, REG_P05_02, &p05_u16, 3, 80) == 0 && p05_u16 > 0){
        g_units_per_rev = (int32_t)p05_u16;
        g_units_valid = true;
        *out = g_units_per_rev;
        return 0;
    }
    if(g_units_valid){
        *out = g_units_per_rev;
        puts("WARN: P05-02 read failed, using cached units_per_rev.");
        return 0;
    }
    *out = 0;
    puts("WARN: P05-02 read failed, units_per_rev desconhecido.");
    return -1;
}

static int compute_auto_timeout_ms(int32_t start_pos, int32_t req_pos, int speed_rpm, int32_t units_per_rev){
    if(speed_rpm <= 0 || units_per_rev <= 0) return 0;
    double diff = (double)llabs((long long)req_pos - (long long)start_pos);
    double revs = diff / (double)units_per_rev;
    double ms = (revs * 60000.0) / (double)speed_rpm;
    ms = ms * 1.5 + 500.0; // margem para acel/settle
    if(ms < 500.0) ms = 500.0;
    if(ms > 600000.0) ms = 600000.0;
    return (int)(ms + 0.5);
}

static int write_u16(modbus_t *ctx, int reg, uint16_t v){
    if(modbus_write_register(ctx, reg, v) == -1){
        int err = errno;
        fprintf(stderr, "Falha ao escrever 0x%04X (errno=%d): %s\n",
                reg, err, modbus_strerror(err));
        return -1;
    }
    return 0;
}

static int write_u16_logged(modbus_t *ctx, int reg, uint16_t v, const char *label, bool verify){
    int rc = modbus_write_register(ctx, reg, v);
    if(rc == -1){
        int err = errno;
        fprintf(stderr, "WRITE 0x%04X %s = %u -> FAIL (errno=%d): %s\n",
                reg, label ? label : "", (unsigned)v, err, modbus_strerror(err));
        return -1;
    }
    printf("WRITE 0x%04X %s = %u -> OK\n", reg, label ? label : "", (unsigned)v);
    if(verify){
        uint16_t rb = 0;
        if(read_u16(ctx, reg, &rb) == 0){
            printf("READ  0x%04X %s = %u\n", reg, label ? label : "", (unsigned)rb);
        }
    }
    return 0;
}

static int write_u16_seq(modbus_t *ctx, int reg, uint16_t v, const char *label,
                         bool verify, int retries, int retry_ms){
    for(int i = 0; i < retries; ++i){
        int rc = modbus_write_register(ctx, reg, v);
        if(rc != -1){
            printf("WRITE 0x%04X %s = %u -> OK\n", reg, label ? label : "", (unsigned)v);
            if(verify){
                uint16_t rb = 0;
                if(read_u16(ctx, reg, &rb) == 0){
                    printf("READ  0x%04X %s = %u\n", reg, label ? label : "", (unsigned)rb);
                }
            }
            return 0;
        }
        int err = errno;
        fprintf(stderr, "WRITE 0x%04X %s = %u -> FAIL (try %d/%d, errno=%d): %s\n",
                reg, label ? label : "", (unsigned)v, i+1, retries, err, modbus_strerror(err));
        if(i+1 < retries) Sleep(retry_ms);
    }
    if(verify){
        uint16_t rb = 0;
        if(read_u16(ctx, reg, &rb) == 0){
            printf("READ  0x%04X %s = %u\n", reg, label ? label : "", (unsigned)rb);
            if(rb == v){
                puts("WARN: write failed, but readback matches desired value.");
                return 0;
            }
        }
    }
    return -1;
}

static int read_u16(modbus_t *ctx, int reg, uint16_t *out){
    uint16_t v = 0;
    if(modbus_read_registers(ctx, reg, 1, &v) != 1){
        fprintf(stderr, "Read 0x%04X failed: %s\n", reg, modbus_strerror(errno));
        return -1;
    }
    *out = v;
    return 0;
}

static int read_u16_seq(modbus_t *ctx, int reg, uint16_t *out, int retries, int retry_ms){
    for(int i = 0; i < retries; ++i){
        uint16_t v = 0;
        if(modbus_read_registers(ctx, reg, 1, &v) == 1){
            *out = v;
            return 0;
        }
        int err = errno;
        fprintf(stderr, "Read 0x%04X failed (try %d/%d): %s\n",
                reg, i+1, retries, modbus_strerror(err));
        if(i+1 < retries) Sleep(retry_ms);
    }
    return -1;
}

static int read_p0b09(modbus_t *ctx, uint16_t *out){
    uint16_t v = 0;
    if(modbus_read_registers(ctx, REG_P0B_09, 1, &v) == 1){
        *out = v;
        return 0;
    }
    if(modbus_read_input_registers(ctx, REG_P0B_09, 1, &v) == 1){
        *out = v;
        return 0;
    }
    return -1;
}

static int read_s32(modbus_t *ctx, int reg, int low_first, int32_t *out){
    uint16_t regs[2] = {0, 0};
    if(modbus_read_registers(ctx, reg, 2, regs) != 2){
        fprintf(stderr, "Read32 0x%04X failed: %s\n", reg, modbus_strerror(errno));
        return -1;
    }
    uint32_t u;
    if(low_first){
        u = ((uint32_t)regs[1] << 16) | regs[0];
    }else{
        u = ((uint32_t)regs[0] << 16) | regs[1];
    }
    *out = (int32_t)u;
    return 0;
}

static int read_p0b07(modbus_t *ctx, int low_first, int32_t *out){
    return read_s32(ctx, 0x0B07, low_first, out);
}

static int read_s32_seq(modbus_t *ctx, int reg, int low_first, int32_t *out, int retries, int retry_ms){
    for(int i = 0; i < retries; ++i){
        uint16_t regs[2] = {0, 0};
        if(modbus_read_registers(ctx, reg, 2, regs) == 2){
            uint32_t u;
            if(low_first){
                u = ((uint32_t)regs[1] << 16) | regs[0];
            }else{
                u = ((uint32_t)regs[0] << 16) | regs[1];
            }
            *out = (int32_t)u;
            return 0;
        }
        int err = errno;
        fprintf(stderr, "Read32 0x%04X failed (try %d/%d): %s\n",
                reg, i+1, retries, modbus_strerror(err));
        if(i+1 < retries) Sleep(retry_ms);
    }
    return -1;
}

static int write_s32(modbus_t *ctx, int reg, int32_t v, int low_first){
    uint16_t regs[2];
    uint32_t u = (uint32_t)v;
    uint16_t lo = (uint16_t)(u & 0xFFFF);
    uint16_t hi = (uint16_t)((u >> 16) & 0xFFFF);
    if(low_first){
        regs[0] = lo;
        regs[1] = hi;
    }else{
        regs[0] = hi;
        regs[1] = lo;
    }
    if(modbus_write_registers(ctx, reg, 2, regs) != 2){
        fprintf(stderr, "Write32 0x%04X failed: %s\n", reg, modbus_strerror(errno));
        return -1;
    }
    return 0;
}

static int write_s32_logged(modbus_t *ctx, int reg, int32_t v, int low_first, const char *label, bool verify){
    int rc = write_s32(ctx, reg, v, low_first);
    if(rc != 0){
        fprintf(stderr, "WRITE 0x%04X %s = %ld -> FAIL\n",
                reg, label ? label : "", (long)v);
        return -1;
    }
    printf("WRITE 0x%04X %s = %ld -> OK\n", reg, label ? label : "", (long)v);
    if(verify){
        int32_t rb = 0;
        if(read_s32(ctx, reg, low_first, &rb) == 0){
            printf("READ  0x%04X %s = %ld\n", reg, label ? label : "", (long)rb);
        }
    }
    return 0;
}

static int write_s32_seq(modbus_t *ctx, int reg, int32_t v, int low_first, const char *label,
                         bool verify, int retries, int retry_ms){
    for(int i = 0; i < retries; ++i){
        int rc = write_s32(ctx, reg, v, low_first);
        if(rc == 0){
            printf("WRITE 0x%04X %s = %ld -> OK\n", reg, label ? label : "", (long)v);
            if(verify){
                int32_t rb = 0;
                if(read_s32(ctx, reg, low_first, &rb) == 0){
                    printf("READ  0x%04X %s = %ld\n", reg, label ? label : "", (long)rb);
                }
            }
            return 0;
        }
        fprintf(stderr, "WRITE 0x%04X %s = %ld -> FAIL (try %d/%d)\n",
                reg, label ? label : "", (long)v, i+1, retries);
        if(i+1 < retries) Sleep(retry_ms);
    }
    if(verify){
        int32_t rb = 0;
        if(read_s32(ctx, reg, low_first, &rb) == 0){
            printf("READ  0x%04X %s = %ld\n", reg, label ? label : "", (long)rb);
            if(rb == v){
                puts("WARN: write failed, but readback matches desired value.");
                return 0;
            }
        }
    }
    return -1;
}

static int probe_connection(const args_t *args){
    char port_path[64];
    make_port_path(args->port, port_path, sizeof(port_path));

    modbus_t *ctx = modbus_new_rtu(port_path, args->baud, args->parity, args->databits, args->stopbits);
    if(!ctx){
        puts("Comunicacao: falha ao criar contexto Modbus.");
        return 1;
    }
    modbus_set_slave(ctx, args->slave);
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);

    if(modbus_connect(ctx)==-1){
        printf("Comunicacao: falha ao conectar (%s).\n", modbus_strerror(errno));
        modbus_free(ctx);
        return 1;
    }

    uint16_t v = 0;
    if(read_u16(ctx, REG_P0C_00, &v) == 0){
        printf("Comunicacao: conectado (addr=%u).\n", v);
    }else{
        puts("Comunicacao: conectado (sem leitura de addr).");
    }
    if(read_u16(ctx, REG_P0C_26, &v) == 0){
        g_word_order = v;
        g_word_order_valid = true;
        printf("Comunicacao: word order=%u.\n", v);
    }
    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}

static int wait_pos_settle(modbus_t *ctx, int low_first, int thresh, int timeout_ms){
    const int step_ms = 100;
    const int stable_need = 5;
    int stable = 0;
    int elapsed = 0;
    while(elapsed < timeout_ms){
        int32_t dev = 0;
        if(read_s32(ctx, REG_P0B_15, low_first, &dev) == 0){
            if(dev < 0) dev = -dev;
            if(dev <= thresh){
                stable++;
                if(stable >= stable_need) return 0;
            }else{
                stable = 0;
            }
        }
        Sleep(step_ms);
        elapsed += step_ms;
    }
    return -1;
}

static int configure_vdi(modbus_t *ctx){
    // VDI1=S-ON (level), VDI2=PosInSen (level per doc)
    if(write_u16_seq(ctx, REG_P17_00, FUNIN_SON, "P17-00 (VDI1 func)", true, 3, 50) != 0) return -1;
    Sleep(20);
    if(write_u16_seq(ctx, REG_P17_01, 0, "P17-01 (VDI1 logic)", true, 3, 50) != 0) return -1;
    Sleep(20);
    if(write_u16_seq(ctx, REG_P17_02, FUNIN_POSINSEN, "P17-02 (VDI2 func)", true, 3, 50) != 0) return -1;
    Sleep(20);
    if(write_u16_seq(ctx, REG_P17_03, 0, "P17-03 (VDI2 logic)", true, 3, 50) != 0) return -1; // level active

    return 0;
}

static int vdi_mapping_ok(modbus_t *ctx){
    uint16_t v = 0;
    if(read_u16(ctx, REG_P17_00, &v) != 0 || v != FUNIN_SON) return 0;
    if(read_u16(ctx, REG_P17_02, &v) != 0 || v != FUNIN_POSINSEN) return 0;
    // Logic: VDI1/2 level
    if(read_u16(ctx, REG_P17_01, &v) != 0 || v != 0) return 0;
    if(read_u16(ctx, REG_P17_03, &v) != 0 || v != 0) return 0;
    return 1;
}

static int32_t logical_pos_from_raw(int32_t raw){
    if(!g_zero_valid) return raw;
    return (int32_t)((int64_t)raw - (int64_t)g_zero_offset);
}

static int watch_and_verify(modbus_t *ctx, int low_first, int32_t req_pos, int32_t send_pos,
                            int tol, int timeout_ms){
    const int step_ms = 100;
    int elapsed = 0;
    int stable = 0;
    int32_t raw_pos = 0;
    int32_t logical_pos = 0;
    int32_t cmd_cnt = 0;
    int32_t dev = 0;
    int32_t err = 0;

    if(tol < 0) tol = 0;
    if(timeout_ms < step_ms) timeout_ms = step_ms;

    puts("Watch: P0B-07 (abs pos), P0B-13 (cmd count), P0B-15 (pos deviation)");
    while(elapsed <= timeout_ms){
        bool ok = true;
        if(read_p0b07(ctx, low_first, &raw_pos) != 0) ok = false;
        if(read_s32(ctx, REG_P0B_13, low_first, &cmd_cnt) != 0) ok = false;
        if(read_s32(ctx, REG_P0B_15, low_first, &dev) != 0) ok = false;

        if(ok){
            logical_pos = logical_pos_from_raw(raw_pos);
            err = (int32_t)((int64_t)logical_pos - (int64_t)req_pos);
            if(g_zero_valid){
                printf("  t=%4d ms | pos=%ld raw=%ld | cmd=%ld | dev=%ld | err=%ld\n",
                       elapsed, (long)logical_pos, (long)raw_pos, (long)cmd_cnt, (long)dev, (long)err);
            }else{
                printf("  t=%4d ms | pos=%ld | cmd=%ld | dev=%ld | err=%ld\n",
                       elapsed, (long)logical_pos, (long)cmd_cnt, (long)dev, (long)err);
            }
            log_sample("watch", req_pos, send_pos, raw_pos, logical_pos, err, dev, cmd_cnt);

            if(labs((long)err) <= tol){
                stable++;
            }else{
                stable = 0;
            }
            if(stable >= 3) break;
        }

        Sleep(step_ms);
        elapsed += step_ms;
    }

    g_last_req = req_pos;
    g_last_meas = logical_pos;
    g_last_err = err;
    g_last_valid = true;

    if(labs((long)err) <= tol){
        printf("Verificacao: OK (err=%ld <= tol=%d).\n", (long)err, tol);
        return 0;
    }
    printf("Verificacao: timeout (err=%ld > tol=%d).\n", (long)err, tol);
    return -1;
}

static void diag_dump_config(modbus_t *ctx){
    uint16_t v = 0;
    uint16_t order = 1;
    int32_t s32 = 0;

    puts("Config (position / internal multi-segment):");
    if(read_u16(ctx, REG_P02_00, &v) == 0) printf("  P02-00 (control mode) = %u\n", v);
    if(read_u16(ctx, REG_P03_02, &v) == 0) printf("  P03-02 (DI1 func) = %u\n", v);
    if(read_u16(ctx, REG_P05_00, &v) == 0) printf("  P05-00 (pos cmd source) = %u\n", v);
    if(read_u16(ctx, REG_P11_00, &v) == 0) printf("  P11-00 (op mode) = %u\n", v);
    if(read_u16(ctx, REG_P11_01, &v) == 0) printf("  P11-01 (segments) = %u\n", v);
    if(read_u16(ctx, REG_P11_04, &v) == 0) printf("  P11-04 (abs=1/rel=0) = %u\n", v);
    if(read_u16(ctx, REG_P0C_09, &v) == 0) printf("  P0C-09 (comm VDI) = %u\n", v);
    if(read_u16(ctx, REG_P0C_26, &order) == 0) printf("  P0C-26 (word order) = %u\n", order);

    if(read_s32(ctx, REG_P11_12, (order != 0), &s32) == 0){
        printf("  P11-12 (seg1 pos) = %ld\n", (long)s32);
    }
    if(read_u16(ctx, REG_P11_14, &v) == 0) printf("  P11-14 (seg1 speed) = %u\n", v);
    if(read_u16(ctx, REG_P11_15, &v) == 0) printf("  P11-15 (seg1 accel) = %u\n", v);
    if(read_u16(ctx, REG_P11_16, &v) == 0) printf("  P11-16 (seg1 wait) = %u\n", v);

    puts("VDI mapping (P17):");
    if(read_u16(ctx, REG_P17_00, &v) == 0) printf("  P17-00 (VDI1 func) = %u\n", v);
    if(read_u16(ctx, REG_P17_01, &v) == 0) printf("  P17-01 (VDI1 logic) = %u\n", v);
    if(read_u16(ctx, REG_P17_02, &v) == 0) printf("  P17-02 (VDI2 func) = %u\n", v);
    if(read_u16(ctx, REG_P17_03, &v) == 0) printf("  P17-03 (VDI2 logic) = %u\n", v);
}

static void diag_dump_runtime(modbus_t *ctx){
    uint16_t order = 1;
    uint16_t di = 0;
    uint16_t fsel = 0;
    uint16_t fcode = 0;
    int32_t cmd_cnt = 0;
    int32_t dev = 0;

    if(read_u16(ctx, REG_P0C_26, &order) != 0) order = 1;

    puts("Runtime monitors:");
    if(read_u16(ctx, REG_P0B_03, &di) == 0){
        printf("  P0B-03 (DI status) = 0x%04X\n", di);
    }
    if(read_s32(ctx, REG_P0B_13, (order != 0), &cmd_cnt) == 0){
        printf("  P0B-13 (input cmd count) = %ld\n", (long)cmd_cnt);
    }
    if(read_s32(ctx, REG_P0B_15, (order != 0), &dev) == 0){
        printf("  P0B-15 (pos deviation) = %ld\n", (long)dev);
    }
    if(read_u16(ctx, REG_P0B_33, &fsel) == 0){
        printf("  P0B-33 (fault sel) = %u\n", fsel);
    }
    if(read_u16(ctx, REG_P0B_34, &fcode) == 0){
        printf("  P0B-34 (fault code) = %u\n", fcode);
    }
}

static void diag_warn_mismatch(modbus_t *ctx){
    uint16_t v = 0;
    bool ok = true;
    if(read_u16(ctx, REG_P02_00, &v) == 0 && v != 1){
        printf("WARN: P02-00=%u (expected 1 for position mode)\n", v);
        ok = false;
    }
    if(read_u16(ctx, REG_P03_02, &v) == 0 && v != 0){
        printf("WARN: P03-02=%u (expected 0 to disable DI1)\n", v);
        ok = false;
    }
    if(read_u16(ctx, REG_P05_00, &v) == 0 && v != 2){
        printf("WARN: P05-00=%u (expected 2 for internal multi-segment)\n", v);
        ok = false;
    }
    if(read_u16(ctx, REG_P11_00, &v) == 0 && v != 2){
        printf("WARN: P11-00=%u (expected 2 for DI switching operation)\n", v);
        ok = false;
    }
    if(read_u16(ctx, REG_P0C_09, &v) == 0 && v != 1){
        printf("WARN: P0C-09=%u (expected 1 to enable communication VDI)\n", v);
        ok = false;
    }

    if(!vdi_mapping_ok(ctx)){
        puts("WARN: P17 VDI mapping not set for internal multi-segment.");
        puts("      Expected: VDI1=S-ON, VDI2=PosInSen (per position-parameter doc).");
        ok = false;
    }
    if(!ok){
        puts("WARN: Drive may enter RUN but not move with position commands.");
    }
}

static int parse_args(int argc, char **argv, args_t *out){
    memset(out, 0, sizeof(*out));
    out->slave = DEFAULT_SLAVE;
    out->baud = DEFAULT_BAUD;
    out->parity = DEFAULT_PARITY;
    out->databits = DEFAULT_DATABITS;
    out->stopbits = DEFAULT_STOPBITS;
    out->abs_mode = true;
    out->osc_pos_a = 0;
    out->osc_pos_b = 15000;
    out->osc_cycles = 5;
    out->osc_dwell_ms = 200;
    out->verify_tol = 5;
    out->verify_timeout_ms = 5000;

    int positional = 0;
    for(int i=1;i<argc;i++){
        const char *a = argv[i];
        if(a[0]=='-' && a[1]=='-'){
            if(strcmp(a,"--abs")==0){ out->abs_mode = true; continue; }
            if(strcmp(a,"--rel")==0){ out->abs_mode = false; continue; }
            if(strcmp(a,"--setup")==0){ out->setup_vdi = true; continue; }
            if(strcmp(a,"--diag")==0){ out->diag = true; continue; }
            if(strcmp(a,"--dry-run")==0){ out->dry_run = true; continue; }
            if(strcmp(a,"--osc")==0){ out->osc_mode = true; continue; }
            if(strcmp(a,"--cycles")==0 && i+1<argc){
                out->osc_cycles = atoi(argv[++i]);
                continue;
            }
            if(strcmp(a,"--dwell")==0 && i+1<argc){
                out->osc_dwell_ms = atoi(argv[++i]);
                continue;
            }
            if(strcmp(a,"--pos-a")==0 && i+1<argc){
                out->osc_pos_a = (int32_t)strtol(argv[++i], NULL, 10);
                continue;
            }
            if(strcmp(a,"--pos-b")==0 && i+1<argc){
                out->osc_pos_b = (int32_t)strtol(argv[++i], NULL, 10);
                continue;
            }
            if(strcmp(a,"--speed")==0 && i+1<argc){
                out->speed = atoi(argv[++i]);
                out->set_speed = true;
                continue;
            }
            if(strcmp(a,"--accel")==0 && i+1<argc){
                out->accel = atoi(argv[++i]);
                out->set_accel = true;
                continue;
            }
            if(strcmp(a,"--wait")==0 && i+1<argc){
                out->wait_ms = atoi(argv[++i]);
                out->set_wait = true;
                continue;
            }
            if(strcmp(a,"--slave")==0 && i+1<argc){
                out->slave = atoi(argv[++i]);
                continue;
            }
            if(strcmp(a,"--baud")==0 && i+1<argc){
                out->baud = atoi(argv[++i]);
                continue;
            }
            if(strcmp(a,"--parity")==0 && i+1<argc){
                out->parity = toupper((unsigned char)argv[++i][0]);
                continue;
            }
            fprintf(stderr, "Unknown option: %s\n", a);
            return -1;
        }else{
            if(positional == 0){
                strncpy(out->port, a, sizeof(out->port)-1);
                out->port[sizeof(out->port)-1] = 0;
                positional++;
            }else if(positional == 1){
                out->pos = (int32_t)strtol(a, NULL, 10);
                positional++;
            }else{
                fprintf(stderr, "Unexpected arg: %s\n", a);
                return -1;
            }
        }
    }
    if(out->diag){
        return (positional >= 1) ? 0 : -1;
    }
    if(out->osc_mode){
        return (positional >= 1) ? 0 : -1;
    }
    return (positional >= 2) ? 0 : -1;
}

static int run_diag(const args_t *args){
    char port_path[64];
    make_port_path(args->port, port_path, sizeof(port_path));

    modbus_t *ctx = modbus_new_rtu(port_path, args->baud, args->parity, args->databits, args->stopbits);
    if(!ctx){
        fprintf(stderr,"modbus_new_rtu failed\n");
        return 1;
    }
    modbus_set_slave(ctx, args->slave);
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);

    if(modbus_connect(ctx)==-1){
        fprintf(stderr,"connect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return 1;
    }

    puts("Diagnostico Modbus (leitura):");
    printf("Porta=%s | Slave=%d | %d %c %d\n", port_path, args->slave,
           args->baud, args->parity, args->stopbits);

    uint16_t v = 0;
    if(read_u16(ctx, REG_P0C_00, &v) == 0) printf("P0C-00 (addr): %u\n", v);
    if(read_u16(ctx, REG_P0C_02, &v) == 0) printf("P0C-02 (baud idx): %u\n", v);
    if(read_u16(ctx, REG_P0C_03, &v) == 0) printf("P0C-03 (parity idx): %u\n", v);
    if(read_u16(ctx, REG_P0C_26, &v) == 0) printf("P0C-26 (word order): %u\n", v);

    uint16_t pos = 0;
    if(modbus_read_registers(ctx, REG_P0B_09, 1, &pos) == 1){
        printf("P0B-09 (FC03): %u\n", pos);
    }else if(modbus_read_input_registers(ctx, REG_P0B_09, 1, &pos) == 1){
        printf("P0B-09 (FC04): %u\n", pos);
    }else{
        fprintf(stderr, "Falha lendo P0B-09 via FC03/FC04.\n");
    }

    puts("");
    diag_dump_config(ctx);
    puts("");
    diag_warn_mismatch(ctx);
    puts("");
    diag_dump_runtime(ctx);

    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}

static int run_stop(const args_t *args){
    char port_path[64];
    make_port_path(args->port, port_path, sizeof(port_path));

    if(args->dry_run){
        printf("DRY RUN: stop port=%s slave=%d\n", port_path, args->slave);
        return 0;
    }

    modbus_t *ctx = modbus_new_rtu(port_path, args->baud, args->parity, args->databits, args->stopbits);
    if(!ctx){
        fprintf(stderr,"modbus_new_rtu failed\n");
        return 1;
    }
    modbus_set_slave(ctx, args->slave);
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);

    if(modbus_connect(ctx)==-1){
        fprintf(stderr,"connect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return 1;
    }

    if(write_u16(ctx, REG_P0C_09, 1) != 0){
        puts("Aviso: falha ao escrever P0C-09 (continuando).");
    }

    // Retry a few times (line noise / timing)
    bool ok = false;
    for(int k = 0; k < 3; ++k){
        if(write_u16(ctx, REG_P31_00, 0) == 0){
            ok = true;
            break;
        }
        Sleep(30);
    }

    if(ok) puts("STOP enviado (VDI=0).");
    else puts("STOP falhou (VDI=0).");

done:
    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}

static int run_command(const args_t *args){
    char port_path[64];
    make_port_path(args->port, port_path, sizeof(port_path));

    int32_t target_pos = args->pos;
    if(args->abs_mode && g_zero_valid){
        int64_t t = (int64_t)args->pos + (int64_t)g_zero_offset;
        if(t > INT32_MAX || t < INT32_MIN){
            puts("Erro: posicao com offset excede limite 32-bit.");
            return 1;
        }
        target_pos = (int32_t)t;
        printf("Zero software: req=%ld offset=%ld -> send=%ld\n",
               (long)args->pos, (long)g_zero_offset, (long)target_pos);
    }

    if(args->dry_run){
        printf("DRY RUN: port=%s slave=%d pos=%ld mode=%s\n",
               port_path, args->slave, (long)target_pos, args->abs_mode ? "abs":"rel");
        return 0;
    }

    modbus_t *ctx = modbus_new_rtu(port_path, args->baud, args->parity, args->databits, args->stopbits);
    if(!ctx){
        fprintf(stderr,"modbus_new_rtu failed\n");
        return 1;
    }
    modbus_set_slave(ctx, args->slave);
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);

    if(modbus_connect(ctx)==-1){
        fprintf(stderr,"connect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return 1;
    }

    uint16_t order = 1;
    (void)get_word_order(ctx, &order);
    int32_t start_raw = 0;
    int32_t start_logical = 0;
    bool have_start = (read_p0b07(ctx, (order != 0), &start_raw) == 0);
    if(have_start){
        start_logical = logical_pos_from_raw(start_raw);
    }
    int32_t verify_req = args->pos;
    if(!args->abs_mode){
        if(have_start){
            verify_req = (int32_t)((int64_t)start_logical + (int64_t)args->pos);
        }else{
            puts("WARN: nao foi possivel ler P0B-07 antes do movimento; verificacao relativa pode ficar imprecisa.");
        }
    }

    // Optional VDI setup
    if(args->setup_vdi){
        if(configure_vdi(ctx) != 0){
            modbus_close(ctx);
            modbus_free(ctx);
            return 1;
        }
    }else{
        if(!vdi_mapping_ok(ctx)){
            puts("WARN: P17 VDI mapping not configured for internal multi-segment.");
            puts("      Run with --setup or set P17 in the drive software.");
        }
    }

    // Write parameters 1-by-1 (log every write/readback), then RUN
    puts("Aplicando parametros (sequencial):");
    const int retries = 3;
    const int retry_ms = 80;
    uint16_t v = 0;
    if(write_u16_seq(ctx, REG_P31_00, 0, "P31-00 (VDI virtual STOP)", false, retries, retry_ms) != 0){
        puts("Aviso: falha ao escrever P31-00=0 (continuando).");
    }
    Sleep(200);

    if(write_u16_seq(ctx, REG_P0C_09, 1, "P0C-09 (Comm VDI)", true, retries, retry_ms) != 0){
        puts("Aviso: falha ao escrever P0C-09 (continuando).");
    }
    Sleep(30);
    if(write_u16_seq(ctx, REG_P03_02, 0, "P03-02 (DI1 func)", true, retries, retry_ms) != 0){
        puts("Aviso: falha ao escrever P03-02 (continuando).");
    }
    Sleep(30);

    // Configure position mode + internal multi-segment
    if(write_u16_seq(ctx, REG_P02_00, 1, "P02-00 (control mode)", true, retries, retry_ms) != 0) goto done;
    Sleep(20);
    if(write_u16_seq(ctx, REG_P05_00, 2, "P05-00 (pos cmd source)", true, retries, retry_ms) != 0) goto done;
    Sleep(20);
    if(write_u16_seq(ctx, REG_P11_00, 2, "P11-00 (op mode)", true, retries, retry_ms) != 0) goto done; // DI switching
    Sleep(20);
    if(write_u16_seq(ctx, REG_P11_01, 1, "P11-01 (segments)", true, retries, retry_ms) != 0) goto done;
    Sleep(20);
    if(write_u16_seq(ctx, REG_P11_04, args->abs_mode ? 1 : 0, "P11-04 (abs/rel)", true, retries, retry_ms) != 0) goto done;
    Sleep(20);

    if(args->set_speed){
        if(write_u16_seq(ctx, REG_P11_14, (uint16_t)args->speed, "P11-14 (seg1 speed)", true, retries, retry_ms) != 0) goto done;
        Sleep(20);
    }
    if(args->set_accel){
        if(write_u16_seq(ctx, REG_P11_15, (uint16_t)args->accel, "P11-15 (seg1 accel)", true, retries, retry_ms) != 0) goto done;
        Sleep(20);
    }
    if(args->set_wait){
        if(write_u16_seq(ctx, REG_P11_16, (uint16_t)args->wait_ms, "P11-16 (seg1 wait)", true, retries, retry_ms) != 0) goto done;
        Sleep(20);
    }

    if(write_s32_seq(ctx, REG_P11_12, target_pos, (order != 0), "P11-12 (seg1 pos)", true, retries, retry_ms) != 0) goto done;

    // Servo enable (VDI1)
    uint16_t vdi = (uint16_t)(1u << VDI_SON_BIT);
    if(write_u16_seq(ctx, REG_P31_00, vdi, "P31-00 (VDI1=S-ON)", false, retries, retry_ms) != 0) goto done;
    Sleep(50);

    // Trigger PosInSen (edge or level based on P17-03 for VDI2)
    uint16_t pos_logic = 0;
    if(read_u16(ctx, REG_P17_03, &pos_logic) != 0){
        pos_logic = 0;
    }

    uint16_t vdi_start = (uint16_t)(vdi | (1u << VDI_POSINSEN_BIT));
    if(write_u16_seq(ctx, REG_P31_00, vdi_start, "P31-00 (VDI1+VDI2)", false, retries, retry_ms) != 0) goto done;

    if(pos_logic != 0){
        // Edge active: pulse and return to VDI1 only
        Sleep(50);
        (void)write_u16(ctx, REG_P31_00, vdi);
    }else{
        // Level active: keep PosInSen ON to allow motion; STOP clears it.
        puts("PosInSen em nivel (VDI2 mantido ON). Use STOP para limpar.");
    }

    printf("Position command sent: %ld (%s).\n", (long)target_pos, args->abs_mode ? "abs":"rel");

    int speed_rpm = args->set_speed ? args->speed : 0;
    if(speed_rpm <= 0){
        uint16_t v = 0;
        if(read_u16(ctx, REG_P11_14, &v) == 0) speed_rpm = (int)v;
    }
    int32_t units_rev = 0;
    (void)get_units_per_rev(ctx, order, &units_rev);
    int auto_ms = 0;
    int32_t diff_units = 0;
    if(have_start){
        diff_units = (int32_t)llabs((long long)verify_req - (long long)start_logical);
        auto_ms = compute_auto_timeout_ms(start_logical, verify_req, speed_rpm, units_rev);
    }
    int eff_ms = args->verify_timeout_ms;
    if(auto_ms > eff_ms){
        eff_ms = auto_ms;
        if(auto_ms > 0){
            printf("Timeout auto ajustado para %d ms (diff=%ld, rpm=%d, P05-02=%ld).\n",
                   eff_ms, (long)diff_units, speed_rpm, (long)units_rev);
        }
    }
    (void)watch_and_verify(ctx, (order != 0), verify_req, target_pos,
                           args->verify_tol, eff_ms);

done:
    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}

static int run_oscillate(const args_t *args){
    if(args->osc_cycles < 1){
        puts("Oscilacao: ciclos deve ser >= 1.");
        return 1;
    }

    args_t cmd = *args;
    cmd.abs_mode = true; // oscilacao padrao em absoluto

    puts("Oscilacao: 0 <-> 15000 (ajustavel via --pos-a/--pos-b).");
    printf("Ciclos=%d | Dwell=%d ms\n", args->osc_cycles, args->osc_dwell_ms);

    for(int i = 0; i < args->osc_cycles; ++i){
        printf("\n[Osc] ciclo %d/%d -> posA=%ld\n",
               i+1, args->osc_cycles, (long)args->osc_pos_a);
        cmd.pos = args->osc_pos_a;
        if(run_command(&cmd) != 0) return 1;
        if(args->osc_dwell_ms > 0) Sleep((DWORD)args->osc_dwell_ms);

        printf("\n[Osc] ciclo %d/%d -> posB=%ld\n",
               i+1, args->osc_cycles, (long)args->osc_pos_b);
        cmd.pos = args->osc_pos_b;
        if(run_command(&cmd) != 0) return 1;
        if(args->osc_dwell_ms > 0) Sleep((DWORD)args->osc_dwell_ms);

        cmd.setup_vdi = false; // apenas no primeiro, se habilitado
    }
    return 0;
}

static int run_position_update(const args_t *args){
    char port_path[64];
    make_port_path(args->port, port_path, sizeof(port_path));

    int32_t target_pos = args->pos;
    if(args->abs_mode && g_zero_valid){
        int64_t t = (int64_t)args->pos + (int64_t)g_zero_offset;
        if(t > INT32_MAX || t < INT32_MIN){
            puts("Erro: posicao com offset excede limite 32-bit.");
            return 1;
        }
        target_pos = (int32_t)t;
        printf("Zero software: req=%ld offset=%ld -> send=%ld\n",
               (long)args->pos, (long)g_zero_offset, (long)target_pos);
    }

    if(args->dry_run){
        printf("DRY RUN: update pos=%ld (%s)\n", (long)target_pos,
               args->abs_mode ? "abs" : "rel");
        return 0;
    }

    modbus_t *ctx = modbus_new_rtu(port_path, args->baud, args->parity, args->databits, args->stopbits);
    if(!ctx){
        fprintf(stderr,"modbus_new_rtu failed\n");
        return 1;
    }
    modbus_set_slave(ctx, args->slave);
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);

    if(modbus_connect(ctx)==-1){
        fprintf(stderr,"connect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return 1;
    }

    uint16_t order = 1;
    (void)get_word_order(ctx, &order);
    int32_t start_raw = 0;
    int32_t start_logical = 0;
    bool have_start = (read_p0b07(ctx, (order != 0), &start_raw) == 0);
    if(have_start){
        start_logical = logical_pos_from_raw(start_raw);
    }
    int32_t verify_req = args->pos;
    if(!args->abs_mode){
        if(have_start){
            verify_req = (int32_t)((int64_t)start_logical + (int64_t)args->pos);
        }else{
            puts("WARN: nao foi possivel ler P0B-07 antes do movimento; verificacao relativa pode ficar imprecisa.");
        }
    }

    const int retries = 3;
    const int retry_ms = 80;

    if(write_u16_seq(ctx, REG_P31_00, 0, "P31-00 (VDI virtual STOP)", false, retries, retry_ms) != 0){
        puts("Aviso: falha ao escrever P31-00=0 (continuando).");
    }
    Sleep(120);

    if(write_u16_seq(ctx, REG_P11_04, args->abs_mode ? 1 : 0, "P11-04 (abs/rel)", true, retries, retry_ms) != 0){
        modbus_close(ctx);
        modbus_free(ctx);
        return 1;
    }
    Sleep(20);

    if(write_s32_seq(ctx, REG_P11_12, target_pos, (order != 0), "P11-12 (seg1 pos)", true, retries, retry_ms) != 0){
        modbus_close(ctx);
        modbus_free(ctx);
        return 1;
    }

    uint16_t vdi = (uint16_t)(1u << VDI_SON_BIT);
    if(write_u16_seq(ctx, REG_P31_00, vdi, "P31-00 (VDI1=S-ON)", false, retries, retry_ms) != 0){
        modbus_close(ctx);
        modbus_free(ctx);
        return 1;
    }
    Sleep(50);

    uint16_t vdi_start = (uint16_t)(vdi | (1u << VDI_POSINSEN_BIT));
    if(write_u16_seq(ctx, REG_P31_00, vdi_start, "P31-00 (VDI1+VDI2)", false, retries, retry_ms) != 0){
        modbus_close(ctx);
        modbus_free(ctx);
        return 1;
    }

    printf("Position update sent: %ld (%s).\n", (long)target_pos, args->abs_mode ? "abs":"rel");
    int speed_rpm = args->set_speed ? args->speed : 0;
    if(speed_rpm <= 0){
        uint16_t v = 0;
        if(read_u16(ctx, REG_P11_14, &v) == 0) speed_rpm = (int)v;
    }
    int32_t units_rev = 0;
    (void)get_units_per_rev(ctx, order, &units_rev);
    int auto_ms = 0;
    int32_t diff_units = 0;
    if(have_start){
        diff_units = (int32_t)llabs((long long)verify_req - (long long)start_logical);
        auto_ms = compute_auto_timeout_ms(start_logical, verify_req, speed_rpm, units_rev);
    }
    int eff_ms = args->verify_timeout_ms;
    if(auto_ms > eff_ms){
        eff_ms = auto_ms;
        if(auto_ms > 0){
            printf("Timeout auto ajustado para %d ms (diff=%ld, rpm=%d, P05-02=%ld).\n",
                   eff_ms, (long)diff_units, speed_rpm, (long)units_rev);
        }
    }
    (void)watch_and_verify(ctx, (order != 0), verify_req, target_pos,
                           args->verify_tol, eff_ms);

    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}

static int run_zero_now(const args_t *args){
    char port_path[64];
    make_port_path(args->port, port_path, sizeof(port_path));

    if(args->dry_run){
        puts("DRY RUN: zerar agora (P05-30=6).");
        return 0;
    }

    modbus_t *ctx = modbus_new_rtu(port_path, args->baud, args->parity, args->databits, args->stopbits);
    if(!ctx){
        fprintf(stderr,"modbus_new_rtu failed\n");
        return 1;
    }
    modbus_set_slave(ctx, args->slave);
    modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);

    if(modbus_connect(ctx)==-1){
        fprintf(stderr,"connect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return 1;
    }

    const int retries = 3;
    const int retry_ms = 80;

    if(write_u16_seq(ctx, REG_P31_00, 0, "P31-00 (VDI virtual STOP)", false, retries, retry_ms) != 0){
        puts("Aviso: falha ao escrever P31-00=0 (continuando).");
    }
    Sleep(120);

    uint16_t order = 1;
    (void)get_word_order(ctx, &order);

    int32_t before = 0;
    if(read_p0b07(ctx, (order != 0), &before) == 0){
        printf("P0B-07 antes = %ld\n", (long)before);
    }

    if(write_u16_seq(ctx, REG_P05_30, 6, "P05-30 (home=current pos)", true, retries, retry_ms) != 0){
        puts("Falha ao escrever P05-30=6. Usando zero por software.");
        g_zero_offset = before;
        g_zero_valid = true;
        modbus_close(ctx);
        modbus_free(ctx);
        printf("Zero software aplicado (offset=%ld).\n", (long)g_zero_offset);
        return 0;
    }

    Sleep(200);
    int32_t after = 0;
    if(read_p0b07(ctx, (order != 0), &after) == 0){
        printf("P0B-07 depois = %ld\n", (long)after);
    }

    g_zero_valid = false;
    puts("Zero aplicado (P05-30=6).");

    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}

static void print_header(void){
    puts("==============================================");
    puts("  Lichuan A5 - Position CLI (Modbus RTU)      ");
    puts("  Internal multi-segment, VDI trigger         ");
    puts("==============================================");
}

static int ask_int_range(const char *prompt, int min_v, int max_v, int *out){
    char buf[64];
    for(;;){
        if(!ask_line(prompt, buf, sizeof(buf))) return 0;
        if(buf[0] == 0) return 0;
        char *e = NULL;
        long v = strtol(buf, &e, 10);
        if(e == buf || *e) { puts("Entrada invalida."); continue; }
        if(v < min_v || v > max_v){
            printf("Fora do limite (%d..%d).\n", min_v, max_v);
            continue;
        }
        *out = (int)v;
        return 1;
    }
}

static int interactive_mode(void){
    args_t args;
    memset(&args, 0, sizeof(args));
    args.slave = DEFAULT_SLAVE;
    args.baud = DEFAULT_BAUD;
    args.parity = DEFAULT_PARITY;
    args.databits = DEFAULT_DATABITS;
    args.stopbits = DEFAULT_STOPBITS;
    args.abs_mode = true;
    const int pos_min = -1073741824;
    const int pos_max = 1073741824;

    print_header();

    strncpy(args.port, "COM4", sizeof(args.port)-1);
    args.port[sizeof(args.port)-1] = 0;

    probe_connection(&args);
    log_open();

    char buf[128];
    for(;;){
        if(!ask_line("\nMenu: [T] testa posicao / [O] oscilatorio / [Z] zerar agora / [S] stop / [N] sair: ", buf, sizeof(buf))) break;
        if(buf[0]=='n' || buf[0]=='N') break;

        if(buf[0]=='s' || buf[0]=='S'){
            (void)run_stop(&args);
            continue;
        }
        if(buf[0]=='z' || buf[0]=='Z'){
            (void)run_zero_now(&args);
            continue;
        }
        if(buf[0]=='o' || buf[0]=='O'){
            int cycles = args.osc_cycles;
            int dwell = args.osc_dwell_ms;
            bool set = false;
            if(ask_int_default("Ciclos (enter=5): ", &cycles, &set)){
                if(!set) cycles = 5;
                if(cycles < 1) cycles = 1;
            }
            if(ask_int_default("Dwell entre movimentos ms (enter=200): ", &dwell, &set)){
                if(!set) dwell = 200;
                if(dwell < 0) dwell = 0;
            }

            args.osc_mode = true;
            args.osc_pos_a = 0;
            args.osc_pos_b = 15000;
            args.osc_cycles = cycles;
            args.osc_dwell_ms = dwell;
            (void)run_oscillate(&args);
            args.osc_mode = false;
            continue;
        }
        if(buf[0]=='t' || buf[0]=='T'){
            int pos_i = 0;
            bool set = false;

            if(ask_line("Modo [A]bsoluto / [R]elativo (A): ", buf, sizeof(buf))){
                if(buf[0]=='r' || buf[0]=='R') args.abs_mode = false;
                else args.abs_mode = true;
            }
            (void)ask_int_default("Velocidade max rpm (enter = manter): ", &args.speed, &args.set_speed);
            if(args.set_speed && (args.speed < 1 || args.speed > 6000)){
                puts("Velocidade fora do limite (1..6000). Ignorando.");
                args.set_speed = false;
            }
            (void)ask_int_default("Acel/Desac ms (enter = manter): ", &args.accel, &args.set_accel);
            if(args.set_accel && (args.accel < 0 || args.accel > 65535)){
                puts("Acel/Desac fora do limite (0..65535). Ignorando.");
                args.set_accel = false;
            }
            (void)ask_int_default("Espera apos movimento ms (enter = manter): ", &args.wait_ms, &args.set_wait);
            if(args.set_wait && (args.wait_ms < 0 || args.wait_ms > 10000)){
                puts("Espera fora do limite (0..10000). Ignorando.");
                args.set_wait = false;
            }
            (void)ask_int_default("Tolerancia pos (enter=5): ", &args.verify_tol, &set);
            if(!set) args.verify_tol = 5;
            if(args.verify_tol < 0) args.verify_tol = 0;
            (void)ask_int_default("Timeout verif ms (enter=5000): ", &args.verify_timeout_ms, &set);
            if(!set) args.verify_timeout_ms = 5000;
            if(args.verify_timeout_ms < 100) args.verify_timeout_ms = 100;

            if(!ask_int_range("Posicao inicial (command unit): ", pos_min, pos_max, &pos_i)){
                puts("Cancelado.");
                continue;
            }
            args.pos = (int32_t)pos_i;
            printf("\nAlvo: pos=%ld (%s)\n", (long)args.pos, args.abs_mode ? "abs":"rel");
            if(run_command(&args) != 0){
                puts("Falha ao enviar comando inicial.");
                continue;
            }
            if(args.abs_mode && g_last_valid && labs((long)g_last_err) > args.verify_tol){
                if(ask_line("Aplicar offset software para casar P0B-07? [s/N]: ", buf, sizeof(buf))){
                    if(buf[0]=='s' || buf[0]=='S'){
                        g_zero_offset = (int32_t)((int64_t)g_zero_offset - (int64_t)g_last_err);
                        g_zero_valid = true;
                        printf("Offset aplicado: %ld (err ajustado=%ld).\n",
                               (long)g_zero_offset, (long)g_last_err);
                    }
                }
            }

            for(;;){
                if(!ask_line("\nTesta posicao (enter para menu, STOP para parar): ", buf, sizeof(buf))) return 1;
                if(buf[0] == 0) break;
                if(_stricmp(buf, "stop") == 0 || _stricmp(buf, "parar") == 0){
                    (void)run_stop(&args);
                    break;
                }
                char *e = NULL;
                long v = strtol(buf, &e, 10);
                if(e == buf || *e){
                    puts("Entrada invalida.");
                    continue;
                }
                if(v < pos_min || v > pos_max){
                    printf("Fora do limite (%d..%d).\n", pos_min, pos_max);
                    continue;
                }
                args.pos = (int32_t)v;
                (void)run_position_update(&args);
            }
            continue;
        }
        puts("Opcao invalida.");
    }

    puts("Pressione Enter para sair.");
    (void)fgets(buf, sizeof(buf), stdin);
    log_close();
    return 0;
}

int main(int argc, char **argv){
    args_t args;
    if(parse_args(argc, argv, &args) != 0){
        return interactive_mode();
    }
    if(args.diag){
        return run_diag(&args);
    }
    if(args.osc_mode){
        return run_oscillate(&args);
    }
    return run_command(&args);
}
