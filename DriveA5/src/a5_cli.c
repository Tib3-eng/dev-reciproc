// ---- C std
// ---- Commit teste
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

// ---- Windows (order matters)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

// ---- Third-party
#include <modbus.h>

/*
  Simplified project:
  - Only reads P0B-09 (position 0..65535, wraps each revolution)
  - No probing, no PPR, no zeroing, no calibration, no old stress tests

  Menu:
    1) Select COM
    2) Connect
    3) Disconnect
    4) RUN
    5) STOP
    6) Set RPM
    7) Read position (P0B-09)
    8) Test: 10 rpm, 120 s, target 200 Hz -> CSV (t_s,pos,rev)
*/

enum {
    SLAVE_ID      = 1,
    REG_CTRL      = 12544,   // 12545-1 : RUN/RDY
    REG_RPM_CMD   = 1539,    // 1540-1  : RPM setpoint (int16)
    REG_P0B_09    = 0x0B09   // P0B-09 (position 0..65535 wrap)
};

static const uint16_t WORD_RUN = 0x0001;
static const uint16_t WORD_RDY = 0x0000;

// Default timeouts for command writes (RPM/RUN/STOP)
#define CMD_RESP_US 50000
#define CMD_BYTE_US 50000

// Fast timeouts for sampling reads (try to keep high rate)
#define FAST_RESP_US 3000
#define FAST_BYTE_US 2000

// ----------------- helpers
static void trim(char *s){
    size_t n = strlen(s);
    while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
    char *p = s; while(*p && isspace((unsigned char)*p)) p++;
    if(p!=s) memmove(s,p,strlen(p)+1);
}

static int ask_int(const char *prompt, int def, int *out){
    char b[128];
    printf("%s [%d]: ", prompt, def);
    if(!fgets(b,sizeof(b),stdin)) return 0;
    trim(b);
    if(!b[0]) { *out = def; return 1; }
    char *e = 0;
    long v = strtol(b,&e,10);
    if(e==b || *e) return 0;
    *out = (int)v;
    return 1;
}

static double qpc_now_s(void){
    static LARGE_INTEGER f = {0};
    LARGE_INTEGER c;
    if(!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void sleep_until(double t_target_s){
    for(;;){
        double now = qpc_now_s();
        double dt = t_target_s - now;
        if(dt <= 0) break;
        if(dt > 0.010) Sleep((DWORD)((dt - 0.005) * 1000.0));
        // short spin last few ms
    }
}

static void set_timeouts_us(modbus_t *ctx, int resp_us, int byte_us){
    modbus_set_response_timeout(ctx, 0, resp_us);
    modbus_set_byte_timeout(ctx,     0, byte_us);
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

static void make_port_path(const char *shortName, char *out, size_t outsz){
    int num = 0;
    if(_strnicmp(shortName,"COM",3)==0) num = atoi(shortName+3);
    if(num >= 10) _snprintf(out, outsz, "\\\\.\\%s", shortName);
    else          _snprintf(out, outsz, "%s", shortName);
}

// ----------------- session
typedef struct {
    modbus_t *ctx;
    char port_path[64];
    int is_connected;

    // How to read P0B-09: 3 = holding (FC03), 4 = input (FC04)
    int pos_fc;
} session_t;

static void sess_close(session_t *S){
    if(S->ctx){
        if(S->is_connected) modbus_close(S->ctx);
        modbus_free(S->ctx);
    }
    memset(S,0,sizeof(*S));
}

static int sess_open(session_t *S, const char *port_short){
    if(S->ctx) sess_close(S);

    make_port_path(port_short, S->port_path, sizeof(S->port_path));

    modbus_t *ctx = modbus_new_rtu(S->port_path, 115200, 'E', 8, 1);
    if(!ctx){ fprintf(stderr,"modbus_new_rtu failed\n"); return -1; }

    modbus_set_slave(ctx, SLAVE_ID);

    // Robust recovery
    modbus_set_error_recovery(ctx,
        MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);

    // Use command-friendly timeouts by default
    set_timeouts_us(ctx, CMD_RESP_US, CMD_BYTE_US);

    if(modbus_connect(ctx)==-1){
        fprintf(stderr,"connect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return -1;
    }

    S->ctx = ctx;
    S->is_connected = 1;
    S->pos_fc = 3; // try holding first

    // quick detect for P0B-09
    uint16_t tmp = 0;
    int rc = modbus_read_registers(S->ctx, REG_P0B_09, 1, &tmp);
    if(rc == 1){
        S->pos_fc = 3;
    }else{
        rc = modbus_read_input_registers(S->ctx, REG_P0B_09, 1, &tmp);
        if(rc == 1) S->pos_fc = 4;
        else{
            S->pos_fc = 0;
            puts("Warning: could not read P0B-09 via FC03/FC04 right now.");
        }
    }

    printf("Connected on %s (115200 8E1, slave %d). P0B-09 via FC%02d.\n",
           S->port_path, SLAVE_ID, (S->pos_fc ? S->pos_fc : 3));
    return 0;
}

// ----------------- commands: RUN / STOP / RPM
static int cmd_run(session_t *S){
    if(!S->is_connected){ puts("Not connected."); return -1; }
    set_timeouts_us(S->ctx, CMD_RESP_US, CMD_BYTE_US);

    if(modbus_write_register(S->ctx, REG_CTRL, WORD_RUN)==-1){
        fprintf(stderr,"RUN failed: %s\n", modbus_strerror(errno));
        return -1;
    }
    puts("RUN sent.");
    return 0;
}

static int cmd_stop(session_t *S){
    if(!S->is_connected){ puts("Not connected."); return -1; }
    set_timeouts_us(S->ctx, CMD_RESP_US, CMD_BYTE_US);

    // Retry a few times (line noise / timing)
    for(int k=0;k<3;++k){
        if(modbus_write_register(S->ctx, REG_CTRL, WORD_RDY)!=-1){
            puts("STOP sent.");
            return 0;
        }
        Sleep(30);
    }
    fprintf(stderr,"STOP failed: %s\n", modbus_strerror(errno));
    return -1;
}

static int cmd_rpm(session_t *S, int rpm){
    if(!S->is_connected){ puts("Not connected."); return -1; }
    if(rpm < -32768 || rpm > 32767){ puts("RPM out of range."); return -1; }
    set_timeouts_us(S->ctx, CMD_RESP_US, CMD_BYTE_US);

    uint16_t v = (uint16_t)(rpm & 0xFFFF);

    // Retry helps in some noisy setups; but "Invalid data" will persist if drive rejects
    for(int k=0;k<2;++k){
        if(modbus_write_register(S->ctx, REG_RPM_CMD, v)!=-1){
            printf("RPM sent: %+d\n", rpm);
            return 0;
        }
        Sleep(20);
    }

    fprintf(stderr,"Set RPM failed: %s\n", modbus_strerror(errno));
    return -1;
}

// ----------------- read P0B-09 (0..65535)
static int read_pos_p0b09(session_t *S, uint16_t *out){
    if(!S->is_connected) return -1;
    uint16_t v = 0;

    int fc = (S->pos_fc ? S->pos_fc : 3);
    int rc = -1;

    if(fc == 3){
        rc = modbus_read_registers(S->ctx, REG_P0B_09, 1, &v);
        if(rc != 1){
            rc = modbus_read_input_registers(S->ctx, REG_P0B_09, 1, &v);
            if(rc == 1) S->pos_fc = 4;
        }
    }else{
        rc = modbus_read_input_registers(S->ctx, REG_P0B_09, 1, &v);
        if(rc != 1){
            rc = modbus_read_registers(S->ctx, REG_P0B_09, 1, &v);
            if(rc == 1) S->pos_fc = 3;
        }
    }

    if(rc == 1){
        *out = v;
        return 0;
    }
    return -1;
}

// Fast read for sampling (short timeouts)
static int read_pos_fast(session_t *S, uint16_t *out){
    if(!S->is_connected) return -1;
    set_timeouts_us(S->ctx, FAST_RESP_US, FAST_BYTE_US);
    return read_pos_p0b09(S, out);
}

// ----------------- TEST: 10 rpm, 120 s, target 200 Hz
static void cmd_test_10rpm_120s_200hz(session_t *S){
    if(!S->is_connected){ puts("Not connected."); return; }

    const int RPM_CMD = 10;
    const double DURATION_S = 120.0;
    const double FS_HZ = 200.0;
    const double PERIOD_S = 1.0 / FS_HZ;

    puts("Test: +10 rpm, 120 s, target 200 Hz. CSV: t_s,pos,rev");

    // 1) Send commands with command-friendly timeouts
    if(cmd_rpm(S, RPM_CMD) < 0){
        puts("Note: 'Invalid data' here usually means drive is not accepting speed setpoint via Modbus in current mode/source.");
        return;
    }
    if(cmd_run(S) < 0){
        (void)cmd_stop(S);
        return;
    }

    Sleep(600); // settle

    // 2) Prepare CSV
    FILE *f = fopen("pos_10rpm_120s_200hz.csv","w");
    if(!f){
        puts("Could not open CSV file for writing.");
        (void)cmd_stop(S);
        return;
    }
    fprintf(f, "t_s,pos,rev\n");

    // 3) Init pos
    uint16_t prev = 0;
    if(read_pos_fast(S, &prev) != 0){
        puts("Initial position read failed.");
        fclose(f);
        (void)cmd_stop(S);
        return;
    }

    uint32_t rev = 0;
    uint32_t samples = 0;

    double t0 = qpc_now_s();
    double t_end = t0 + DURATION_S;
    double next_t = t0;

    // 4) Time-based loop (always ends ~120 s)
    while(1){
        double now = qpc_now_s();
        if(now >= t_end) break;

        if(now < next_t) sleep_until(next_t);

        now = qpc_now_s();
        double t_rel = now - t0;

        uint16_t pos = prev;
        if(read_pos_fast(S, &pos) != 0){
            pos = prev; // hold last
        }

        // Wrap detection (robust): high -> low
        if(prev > 60000 && pos < 5000) rev++;
        prev = pos;

        fprintf(f, "%.6f,%u,%u\n", t_rel, (unsigned)pos, (unsigned)rev);

        samples++;
        next_t += PERIOD_S;

        // If we fall behind too much, resync to avoid backlog
        now = qpc_now_s();
        if(next_t < now - 0.050) next_t = now;
    }

    fclose(f);

    // 5) Stop with command-friendly timeouts
    (void)cmd_stop(S);

    double elapsed = qpc_now_s() - t0;
    double achieved = (elapsed > 0.0) ? ((double)samples / elapsed) : 0.0;

    printf("CSV saved: pos_10rpm_120s_200hz.csv\n");
    printf("Elapsed: %.3f s | samples=%u | achieved=%.1f Hz | last_pos=%u | rev=%u\n",
           elapsed, (unsigned)samples, achieved, (unsigned)prev, (unsigned)rev);

    // restore command timeouts for menu
    set_timeouts_us(S->ctx, CMD_RESP_US, CMD_BYTE_US);
}

// ----------------- UI
static void print_header(void){
    puts("==============================================");
    puts("  Lichuan A5 - Modbus RTU Console (RS-485)    ");
    puts("  115200 8E1 | ctrl@12544 | rpm@1539         ");
    puts("  pos(P0B-09)=0..65535 wrap per revolution   ");
    puts("==============================================");
}

static void print_menu(const session_t *S, const char *sel){
    printf("\n[Status] Port=%s | Connected=%s | P0B-09 via FC%02d\n",
           (sel && *sel) ? sel : "(none)",
           S->is_connected ? "yes" : "no",
           (S->pos_fc ? S->pos_fc : 3));
    puts("--------------------------------------------------");
    puts("  1) Select COM port");
    puts("  2) Connect");
    puts("  3) Disconnect");
    puts("  4) RUN");
    puts("  5) STOP");
    puts("  6) Set RPM");
    puts("  7) Read position (P0B-09 0..65535)");
    puts("  8) Test: 10 rpm, 120 s, target 200 Hz -> CSV");
    puts("  q) Quit");
    puts("--------------------------------------------------");
    printf("Choice: ");
}

int main(void){
    print_header();

    session_t S = {0};
    char sel[32] = "";

    for(;;){
        print_menu(&S, sel);

        char ln[64];
        if(!fgets(ln,sizeof(ln),stdin)) break;
        trim(ln);
        if(!ln[0]) continue;

        if(ln[0]=='q' || ln[0]=='Q') break;

        switch(ln[0]){
            case '1': {
                com_list_t L = com_list_detect();
                com_list_print(&L);

                puts("Type list number or COMx:");
                printf("Your choice: ");
                if(fgets(ln,sizeof(ln),stdin)){
                    trim(ln);
                    if(isdigit((unsigned char)ln[0])){
                        int idx = atoi(ln);
                        if(idx>=1 && idx<=L.count){
                            strncpy(sel, L.items[idx-1], sizeof(sel)-1);
                            sel[sizeof(sel)-1]=0;
                        }else{
                            puts("Invalid index.");
                        }
                    }else if(_strnicmp(ln,"COM",3)==0){
                        strncpy(sel, ln, sizeof(sel)-1);
                        sel[sizeof(sel)-1]=0;
                    }else{
                        puts("Invalid input.");
                    }
                }
                com_list_free(&L);
            } break;

            case '2':
                if(!sel[0]){ puts("Select COM first."); break; }
                if(S.is_connected){ puts("Already connected."); break; }
                (void)sess_open(&S, sel);
                break;

            case '3':
                if(S.is_connected){ sess_close(&S); puts("Disconnected."); }
                else puts("Already disconnected.");
                break;

            case '4': (void)cmd_run(&S); break;
            case '5': (void)cmd_stop(&S); break;

            case '6': {
                int r=0;
                if(!ask_int("Target RPM (-3000..3000)", 0, &r)) puts("Invalid input.");
                else (void)cmd_rpm(&S, r);
            } break;

            case '7': {
                if(!S.is_connected){ puts("Not connected."); break; }
                uint16_t pos=0;
                set_timeouts_us(S.ctx, CMD_RESP_US, CMD_BYTE_US);
                if(read_pos_p0b09(&S, &pos)==0){
                    printf("P0B-09 pos = %u\n", (unsigned)pos);
                }else{
                    puts("Position read failed.");
                }
            } break;

            case '8':
                cmd_test_10rpm_120s_200hz(&S);
                break;

            default:
                puts("Invalid option.");
                break;
        }
    }

    sess_close(&S);
    puts("Exit.");
    return 0;
}
