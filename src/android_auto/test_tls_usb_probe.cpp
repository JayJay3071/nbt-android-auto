// Exercise the exact TLS USB adapter against an in-process TLS server.
#define main memory_test_main
#include "test_tls_memory.c"
#undef main
#include "tls_usb_probe.h"
#include <vector>
struct transport {
    peer *server, *carrier;
    std::vector<unsigned char> queued;
    bool malformed;
    unsigned mode, auth_count, shutdown_count;
    bool server_complete;
};
static int transfer(void* context,int input,unsigned char* data,unsigned* count) {
    transport* t=(transport*)context;
    if (!input) {
        if (*count==8 && data[0]==0 && data[1]==3 && data[4]==0 && data[5]==4) {
            const unsigned char expected[]={0,3,0,4,0,4,8,0};
            if (!t->server_complete || memcmp(data,expected,8) || ++t->auth_count!=1) return 0;
            unsigned char request[]={0,5,10,3,'N','B','T'};
            if (t->mode==5) request[3]=20; // Truncated protobuf length.
            if (t->mode==3) {
                const unsigned char header[]={0,3,0,sizeof(request)};
                t->queued.insert(t->queued.end(),header,header+4);
                t->queued.insert(t->queued.end(),request,request+sizeof(request));
            } else {
                if (mbedtls_ssl_write(&t->server->ssl,request,sizeof(request))!=(int)sizeof(request)) return 0;
                unsigned n=t->carrier->used;
                if (t->mode==4) t->carrier->input[n-1]^=1;
                t->queued.push_back(0); t->queued.push_back(11);
                t->queued.push_back(n>>8); t->queued.push_back(n&255);
                t->queued.insert(t->queued.end(),t->carrier->input,t->carrier->input+n);
                t->carrier->used=0;
                if (t->mode==6) t->queued.pop_back();
            }
            return 1;
        }
        if (*count>4 && data[0]==0 && data[1]==11 && t->auth_count==1) {
            if (*count-4>sizeof(t->server->input)-t->server->used) return 0;
            memcpy(t->server->input+t->server->used,data+4,*count-4); t->server->used+=*count-4;
            unsigned char plain[32];
            const unsigned char bye[]={0,15,8,1};
            int n=mbedtls_ssl_read(&t->server->ssl,plain,sizeof(plain));
            if (n!=4 || memcmp(plain,bye,4)) return 0;
            ++t->shutdown_count; return 1;
        }
        if (*count<6 || data[0]!=0 || data[1]!=3 || data[4]!=0 || data[5]!=3 ||
            (((unsigned)data[2]<<8)|data[3])!=*count-4 || *count-6>sizeof(t->server->input)-t->server->used) return 0;
        memcpy(t->server->input+t->server->used,data+6,*count-6);
        t->server->used+=*count-6;
        int rc=mbedtls_ssl_handshake(&t->server->ssl);
        if (!rc) t->server_complete=true;
        if (rc != 0 && rc != MBEDTLS_ERR_SSL_WANT_READ) return 0;
        unsigned n=t->carrier->used;
        if (n) {
            t->queued.push_back(0); t->queued.push_back(t->malformed ? 7 : 3);
            t->queued.push_back((n+2)>>8); t->queued.push_back((n+2)&255);
            t->queued.push_back(0); t->queued.push_back(3);
            t->queued.insert(t->queued.end(),t->carrier->input,t->carrier->input+n);
            t->carrier->used=0;
        }
        return 1;
    }
    if (t->queued.empty()) return 0;
    // Split even the AA header, but keep total exchanges inside the probe bound.
    unsigned n=t->queued.size();
    if (n>1024) n=1024;
    if (t->queued[0]==0 && n>1) n=1;
    if (n>*count) return 0;
    memcpy(data,&t->queued[0],n); t->queued.erase(t->queued.begin(),t->queued.begin()+n);
    *count=n; return 1;
}
int main(void) {
    alarm(30);
    setenv("NBT_AA_TLS_CERT","lab-client.pem",1);
    setenv("NBT_AA_TLS_KEY","lab-client-key.pem",1);
    setenv("NBT_AA_TLS_CA","lab-ca.pem",1);
    setenv("NBT_AA_TLS_PEER","qnx-aa-lab.local",1);
    for (unsigned mode=0;mode<7;++mode) {
        transport t;
        t.server=(peer*)calloc(1,sizeof(peer)); t.carrier=(peer*)calloc(1,sizeof(peer));
        require(t.server && t.carrier,"transport allocation");
        t.server->other=t.carrier; t.malformed=mode==1;
        t.mode=mode; t.auth_count=t.shutdown_count=0; t.server_complete=false;
        initialize(t.server,1,NULL);
        require(nbt_tls_probe_init(),"adapter init");
        int rc=nbt_tls_probe_run(transfer,&t,mode>=2);
        require(mode==0 || mode==2 ? rc==0 : rc!=0,"adapter handshake/discovery rejection");
        require(t.auth_count==(mode>=2 ? 1u : 0u),"auth exactly once and only after TLS");
        require(t.shutdown_count==(mode==2 ? 1u : 0u),"shutdown only after valid discovery");
        nbt_tls_probe_free(); release(t.server); free(t.carrier);
    }
    puts("AA_TLS_USB_ADAPTER_OK fragmented-handshake discovery shutdown plaintext tamper protobuf truncation rejected");
    return 0;
}
