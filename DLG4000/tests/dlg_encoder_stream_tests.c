/* Real Windows producer/writer threads + fake UDP DLG. No hardware access. */
#define WIN32_LEAN_AND_MEAN
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlg_encoder_stream.h"
#include "dlg_sample_clock.h"
#include "encoder_control_protocol.h"

static volatile LONG gate_once,gate_active;
static DWORD gate_delay;
static int failures;
#define CHECK(test) do {if(!(test)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#test);failures++;}}while(0)

void dlg_stream_test_before_write(void){
    if(InterlockedCompareExchange(&gate_once,1,0)==0){
        InterlockedExchange(&gate_active,1);
        Sleep(gate_delay);
        InterlockedExchange(&gate_active,0);
    }
}

typedef struct {
    SOCKET sender,receiver;
    struct sockaddr_in acquisition_address;
    volatile LONG stop;
    int packets,while_writer_blocked,failed;
} fake_context_t;

static DWORD WINAPI source_thread(LPVOID argument){
    fake_context_t *f=(fake_context_t *)argument;
    dlg_data_packet_t p={0};
    p.code=0x100B;p.bursts=3;p.signals=8;p.rate_hz=200;
    for(int i=0;i<3;++i)p.samples[i*8+2]=2000;
    while(!InterlockedCompareExchange(&f->stop,0,0)){
        sendto(f->sender,(const char *)&p,(int)(offsetof(dlg_data_packet_t,samples)+48),0,
            (const struct sockaddr *)&f->acquisition_address,sizeof(f->acquisition_address));
        p.frame++;p.ts+=15;
        Sleep(15);
    }
    return 0;
}

static DWORD WINAPI monitor_thread(LPVOID argument){
    fake_context_t *f=(fake_context_t *)argument;
    while(!InterlockedCompareExchange(&f->stop,0,0)){
        encoder_control_packet_t p;
        int n=recvfrom(f->receiver,(char *)&p,sizeof(p),0,NULL,NULL);
        if(n==sizeof(p)){
            f->packets++;
            if(InterlockedCompareExchange(&gate_active,0,0))f->while_writer_blocked++;
            if(p.health==ENCODER_HEALTH_FAILED)f->failed++;
        }else Sleep(1);
    }
    return 0;
}

static SOCKET loopback_socket(struct sockaddr_in *address){
    int size=sizeof(*address);
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    u_long nonblocking=1;
    memset(address,0,sizeof(*address));address->sin_family=AF_INET;
    address->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(s==INVALID_SOCKET || bind(s,(const struct sockaddr *)address,sizeof(*address))!=0 ||
       getsockname(s,(struct sockaddr *)address,&size)!=0 || ioctlsocket(s,FIONBIO,&nonblocking)!=0){
        if(s!=INVALID_SOCKET)closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static int no_command(int use_ipc){(void)use_ipc;return 0;}
static void no_log(const char *format,...){(void)format;}

static void run_case(const char *base,const char *name,DWORD delay,int expected_rc){
    dlg_encoder_stream_config_t c={0};
    encoder_state_t encoder;
    encoder_state_config_t ec;
    fake_context_t fake={0};
    struct sockaddr_in address;
    LARGE_INTEGER frequency;
    HANDLE source,monitor;
    char folder[MAX_PATH],path[MAX_PATH];
    int rc;
    _snprintf(folder,sizeof(folder),"%s/%s",base,name);CreateDirectoryA(folder,NULL);
    _snprintf(path,sizeof(path),"%s/dlg.csv",folder);c.data_file=fopen(path,"w");
    _snprintf(path,sizeof(path),"%s/encoder_state.csv",folder);c.encoder_file=fopen(path,"w");
    _snprintf(path,sizeof(path),"%s/dlg_acquisition.csv",folder);c.timing_file=fopen(path,"w");
    CHECK(c.data_file && c.encoder_file && c.timing_file);
    if(!c.data_file||!c.encoder_file||!c.timing_file)exit(2);
    c.acquisition_socket=loopback_socket(&fake.acquisition_address);
    c.control_socket=loopback_socket(&address);
    fake.receiver=loopback_socket(&c.control_address);
    fake.sender=loopback_socket(&address);
    CHECK(c.acquisition_socket!=INVALID_SOCKET && c.control_socket!=INVALID_SOCKET &&
          fake.sender!=INVALID_SOCKET && fake.receiver!=INVALID_SOCKET);
    QueryPerformanceFrequency(&frequency);c.frequency=frequency.QuadPart;
    encoder_state_config_reciprocating(&ec);ec.radius_mm=10;ec.max_physical_speed_mm_s=40;
    CHECK(encoder_state_init(&encoder,&ec,c.frequency,NULL));
    c.encoder=&encoder;c.slope[2]=.05;c.calibrated[2]=c.normalize_degrees[2]=1;
    c.rate_hz=200;c.total_rows=200;c.session=91;c.poll_command=no_command;c.log_event=no_log;
    gate_delay=delay;gate_once=gate_active=0;
    source=CreateThread(NULL,0,source_thread,&fake,0,NULL);
    monitor=CreateThread(NULL,0,monitor_thread,&fake,0,NULL);
    CHECK(source!=NULL && monitor!=NULL);
    rc=dlg_encoder_stream_run(&c);
    Sleep(20);
    InterlockedExchange(&fake.stop,1);
    WaitForSingleObject(source,2000);WaitForSingleObject(monitor,2000);
    CloseHandle(source);CloseHandle(monitor);
    CHECK(rc==expected_rc);
    CHECK(fake.while_writer_blocked>=15);
    if(expected_rc==0){CHECK(fake.packets>=200 && fake.failed==0);}
    else CHECK(fake.failed>=1);
    printf("stream_%s rc=%d packets=%d while_writer_blocked=%d failed=%d\n",
           name,rc,fake.packets,fake.while_writer_blocked,fake.failed);
    closesocket(c.acquisition_socket);closesocket(c.control_socket);
    closesocket(fake.receiver);closesocket(fake.sender);
    fclose(c.data_file);fclose(c.encoder_file);fclose(c.timing_file);
}

int main(int argc,char **argv){
    WSADATA wsa;
    if(argc!=2 || WSAStartup(MAKEWORD(2,2),&wsa)!=0)return 2;
    CreateDirectoryA(argv[1],NULL);
    run_case(argv[1],"writer_150ms",150,0);
    /* Test target uses a 64-record archive queue; production has 8192.
       Exhaustion must send FAILED instead of letting control silently run. */
    run_case(argv[1],"archive_overflow",650,2);
    WSACleanup();
    if(failures)return 1;
    puts("dlg_encoder_stream_tests: OK (writer isolation and fail-closed overflow)");
    return 0;
}
