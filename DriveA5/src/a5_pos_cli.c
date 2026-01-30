// Simple position command for Lichuan A5 via Modbus RTU (internal multi-segment)
// Uses P11 segment 1 and VDI to trigger PosInSen.
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <modbus.h>

// ---- Defaults
#define DEFAULT_SLAVE 1
#define DEFAULT_BAUD 115200
#define DEFAULT_PARITY 'E'
#define DEFAULT_DATABITS 8
#define DEFAULT_STOPBITS 1

// ---- Modbus registers (Pxx-yy -> 0xXXYY)
#define REG_P02_00 0x0200
#define REG_P05_00 0x0500
#define REG_P11_00 0x1100
#define REG_P11_01 0x1101
#define REG_P11_04 0x1104
#define REG_P11_12 0x110C  // 32-bit
#define REG_P11_14 0x110E
#define REG_P11_15 0x110F
#define REG_P11_16 0x1110

#define REG_P0B_09 0x0B09
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
#define FUNIN_CMD1    6
#define FUNIN_CMD2    7
#define FUNIN_CMD3    8
#define FUNIN_CMD4    9
#define FUNIN_POSINSEN 28

// ---- VDI bit mapping (used by this tool)
#define VDI_SON_BIT      0  // VDI1
#define VDI_CMD1_BIT     1  // VDI2
#define VDI_CMD2_BIT     2  // VDI3
#define VDI_CMD3_BIT     3  // VDI4
#define VDI_CMD4_BIT     4  // VDI5
#define VDI_POSINSEN_BIT 5  // VDI6

// ---- Timeouts
#define CMD_RESP_US 50000
#define CMD_BYTE_US 50000

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
    puts("  a5_pos_cli COMx --diag [--slave <id>] [--baud <n>] [--parity N|E|O]");
    puts("");
    puts("Notas:");
    puts("  - Usa posicao interna multi-segmento (P05-00=2) e segmento 1 (P11-12).");
    puts("  - Posicao em \"command unit\" (depende do encoder e da engrenagem eletronica).");
    puts("  - --setup configura VDI: VDI1=S-ON, VDI2..5=CMD1..CMD4, VDI6=PosInSen.");
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

static int write_u16(modbus_t *ctx, int reg, uint16_t v){
    if(modbus_write_register(ctx, reg, v) == -1){
        int err = errno;
        fprintf(stderr, "Falha ao escrever 0x%04X (errno=%d): %s\n",
                reg, err, modbus_strerror(err));
        return -1;
    }
    return 0;
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

static int configure_vdi(modbus_t *ctx){
    // VDI1=S-ON (level), VDI2..5=CMD1..CMD4 (level), VDI6=PosInSen (edge)
    if(write_u16(ctx, REG_P17_00, FUNIN_SON) != 0) return -1;
    if(write_u16(ctx, REG_P17_01, 0) != 0) return -1;

    if(write_u16(ctx, REG_P17_02, FUNIN_CMD1) != 0) return -1;
    if(write_u16(ctx, REG_P17_03, 0) != 0) return -1;
    if(write_u16(ctx, REG_P17_04, FUNIN_CMD2) != 0) return -1;
    if(write_u16(ctx, REG_P17_05, 0) != 0) return -1;
    if(write_u16(ctx, REG_P17_06, FUNIN_CMD3) != 0) return -1;
    if(write_u16(ctx, REG_P17_07, 0) != 0) return -1;
    if(write_u16(ctx, REG_P17_08, FUNIN_CMD4) != 0) return -1;
    if(write_u16(ctx, REG_P17_09, 0) != 0) return -1;

    if(write_u16(ctx, REG_P17_10, FUNIN_POSINSEN) != 0) return -1;
    if(write_u16(ctx, REG_P17_11, 1) != 0) return -1; // edge (0->1)

    return 0;
}

static int parse_args(int argc, char **argv, args_t *out){
    memset(out, 0, sizeof(*out));
    out->slave = DEFAULT_SLAVE;
    out->baud = DEFAULT_BAUD;
    out->parity = DEFAULT_PARITY;
    out->databits = DEFAULT_DATABITS;
    out->stopbits = DEFAULT_STOPBITS;
    out->abs_mode = true;

    int positional = 0;
    for(int i=1;i<argc;i++){
        const char *a = argv[i];
        if(a[0]=='-' && a[1]=='-'){
            if(strcmp(a,"--abs")==0){ out->abs_mode = true; continue; }
            if(strcmp(a,"--rel")==0){ out->abs_mode = false; continue; }
            if(strcmp(a,"--setup")==0){ out->setup_vdi = true; continue; }
            if(strcmp(a,"--diag")==0){ out->diag = true; continue; }
            if(strcmp(a,"--dry-run")==0){ out->dry_run = true; continue; }
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

    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}

static int run_command(const args_t *args){
    char port_path[64];
    make_port_path(args->port, port_path, sizeof(port_path));

    if(args->dry_run){
        printf("DRY RUN: port=%s slave=%d pos=%ld mode=%s\n",
               port_path, args->slave, (long)args->pos, args->abs_mode ? "abs":"rel");
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

    // Optional VDI setup
    if(args->setup_vdi){
        if(configure_vdi(ctx) != 0){
            modbus_close(ctx);
            modbus_free(ctx);
            return 1;
        }
    }

    // Enable communication VDI
    if(write_u16(ctx, REG_P0C_09, 1) != 0){
        modbus_close(ctx);
        modbus_free(ctx);
        return 1;
    }

    // Ensure VDI all low (servo disabled)
    if(write_u16(ctx, REG_P31_00, 0) != 0){
        modbus_close(ctx);
        modbus_free(ctx);
        return 1;
    }

    // Configure position mode + internal multi-segment
    if(write_u16(ctx, REG_P02_00, 1) != 0) goto done;
    if(write_u16(ctx, REG_P05_00, 2) != 0) goto done;
    if(write_u16(ctx, REG_P11_00, 0) != 0) goto done; // single operation
    if(write_u16(ctx, REG_P11_01, 1) != 0) goto done; // 1 segment
    if(write_u16(ctx, REG_P11_04, args->abs_mode ? 1 : 0) != 0) goto done;

    if(args->set_speed && write_u16(ctx, REG_P11_14, (uint16_t)args->speed) != 0) goto done;
    if(args->set_accel && write_u16(ctx, REG_P11_15, (uint16_t)args->accel) != 0) goto done;
    if(args->set_wait && write_u16(ctx, REG_P11_16, (uint16_t)args->wait_ms) != 0) goto done;

    // Determine 32-bit word order (P0C-26). Default is low-first (1).
    uint16_t order = 1;
    if(read_u16(ctx, REG_P0C_26, &order) != 0){
        order = 1;
    }

    if(write_s32(ctx, REG_P11_12, args->pos, (order != 0)) != 0) goto done;

    // Servo enable (VDI1)
    uint16_t vdi = (uint16_t)(1u << VDI_SON_BIT);
    if(write_u16(ctx, REG_P31_00, vdi) != 0) goto done;
    Sleep(50);

    // Trigger PosInSen (edge)
    uint16_t vdi_start = (uint16_t)(vdi | (1u << VDI_POSINSEN_BIT));
    if(write_u16(ctx, REG_P31_00, vdi_start) != 0) goto done;
    Sleep(50);
    (void)write_u16(ctx, REG_P31_00, vdi);

    printf("Position command sent: %ld (%s).\n", (long)args->pos, args->abs_mode ? "abs":"rel");

done:
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

    print_header();

    com_list_t L = com_list_detect();
    com_list_print(&L);

    char buf[128];
    if(!ask_line("Porta COM (ex: COM3): ", buf, sizeof(buf))){
        com_list_free(&L);
        return 1;
    }
    if(isdigit((unsigned char)buf[0]) && L.count > 0){
        int idx = atoi(buf);
        if(idx >= 1 && idx <= L.count){
            strncpy(args.port, L.items[idx-1], sizeof(args.port)-1);
        }else{
            com_list_free(&L);
            puts("Invalid index.");
            return 1;
        }
    }else{
        strncpy(args.port, buf, sizeof(args.port)-1);
    }
    args.port[sizeof(args.port)-1] = 0;
    com_list_free(&L);

    puts("");
    puts("Limites sugeridos pelo manual:");
    puts("  Posicao (P11-12): -1073741824 .. 1073741824");
    puts("  Velocidade max (P11-14): 1 .. 6000 rpm");
    puts("  Acel/Desac (P11-15): 0 .. 65535 ms");
    puts("  Tempo espera (P11-16): 0 .. 10000 ms");
    puts("");

    if(!ask_line("Rodar diagnostico de comunicacao? [s/N]: ", buf, sizeof(buf))) return 1;
    if(buf[0]=='s' || buf[0]=='S'){
        args.diag = true;
        (void)run_diag(&args);
        args.diag = false;
        puts("");
    }

    if(!ask_line("Configurar VDI agora? (P17) [s/N]: ", buf, sizeof(buf))) return 1;
    if(buf[0]=='s' || buf[0]=='S') args.setup_vdi = true;

    for(;;){
        int pos_i = 0;
        if(!ask_int_range("Posicao (command unit): ", -1073741824, 1073741824, &pos_i)){
            puts("Saindo.");
            return 0;
        }
        args.pos = (int32_t)pos_i;

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

        printf("\nAlvo: pos=%ld (%s), COM=%s, slave=%d\n",
               (long)args.pos, args.abs_mode ? "abs":"rel", args.port, args.slave);
        puts("Digite GO para enviar, ou Enter para cancelar.");
        if(!ask_line("Confirmar: ", buf, sizeof(buf))) return 1;
        if(strcmp(buf, "GO") != 0){
            puts("Cancelado.");
        }else{
            int rc = run_command(&args);
            if(rc != 0){
                puts("Falha ao enviar comando.");
                if(args.setup_vdi){
                    puts("Possiveis causas do erro no P17 (VDI):");
                    puts("- Servo habilitado (S-ON ativo) ou drive em RUN; P17 e efetivo em STOP.");
                    puts("- Parametro travado ou protegido por senha.");
                    puts("- Endereco Modbus diferente no firmware.");
                    puts("Tente desabilitar S-ON fisico e executar novamente, ou rode sem setup.");
                }
            }
        }

        if(!ask_line("\nEnviar outra posicao? [S/n]: ", buf, sizeof(buf))) break;
        if(buf[0]=='n' || buf[0]=='N') break;
        args.setup_vdi = false; // setup so uma vez
    }

    puts("Pressione Enter para sair.");
    (void)fgets(buf, sizeof(buf), stdin);
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
    return run_command(&args);
}
