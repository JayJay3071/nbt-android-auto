// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
// Bounded TLS/discovery-request adapter. No capabilities or media advertised.
#include "tls_usb_probe.h"
#include "aa_cryptor.hpp"
#include <stdio.h>
#include <string.h>
static nbt::AACryptor* crypt = NULL;
extern "C" void nbt_tls_probe_free(void) { delete crypt; crypt = NULL; }
extern "C" int nbt_tls_probe_init(void) {
    nbt_tls_probe_free(); crypt = new nbt::AACryptor;
    if (crypt->init()) return 1;
    printf("AA_TLS_INIT_FAILED %s\n",crypt->lastError());
    nbt_tls_probe_free(); return 0;
}
static bool varint(const std::vector<uint8_t>& b, size_t& p, uint64_t& value) {
    value=0;
    for (unsigned n=0;n<10 && p<b.size();++n) {
        unsigned c=b[p++];
        if (n==9 && c>1) return false;
        value |= uint64_t(c&127)<<(n*7);
        if (!(c&128)) return true;
    }
    return false;
}
static bool discovery_body(const std::vector<uint8_t>& b) {
    size_t p=2; unsigned fields=0;
    if (b.size()<2 || b.size()>16384 || b[0]!=0 || b[1]!=5) return false;
    while (p<b.size()) {
        uint64_t tag, value;
        if (++fields>256 || !varint(b,p,tag) || !(tag>>3) || (tag>>3)>0x1fffffff) return false;
        switch(tag&7) {
        case 0: if (!varint(b,p,value)) return false; break;
        case 1: if (b.size()-p<8) return false; p+=8; break;
        case 2: if (!varint(b,p,value) || value>b.size()-p) return false; p+=(size_t)value; break;
        case 5: if (b.size()-p<4) return false; p+=4; break;
        default: return false;
        }
    }
    printf("AA_SERVICE_DISCOVERY_REQUEST_OK encrypted=1 protobuf_bytes=%u fields=%u no_capabilities_sent=1\n",
        (unsigned)b.size()-2,fields);
    return true;
}
static int discover(nbt_tls_transfer transfer, void* context, std::vector<uint8_t>& pending) {
    unsigned char buffer[65536];
    const unsigned char auth[]={0,3,0,4,0,4,8,0};
    memcpy(buffer,auth,sizeof(auth)); unsigned count=sizeof(auth);
    if (!transfer(context,0,buffer,&count) || count!=sizeof(auth)) return 3;
    puts("AA_AUTH_COMPLETE_SENT status=0 after_verified_tls=1");
    unsigned total=0;
    for (unsigned step=0;step<32;++step) {
        if (pending.size()>=4) {
            unsigned n=(pending[2]<<8)|pending[3];
            if (pending[0]!=0 || pending[1]!=11 || !n || n>18432) {
                puts("AA_DISCOVERY_FRAME_REFUSED expected_encrypted_control=1"); return 3;
            }
            if (pending.size()>=n+4) {
                std::vector<uint8_t> plain;
                bool ready=crypt->decrypt(&pending[4],n,plain);
                pending.erase(pending.begin(),pending.begin()+n+4);
                if (!crypt->isHandshakeComplete()) { puts("AA_DISCOVERY_TLS_FAILED"); return 3; }
                if (ready) {
                    if (!discovery_body(plain)) { puts("AA_DISCOVERY_MESSAGE_REFUSED"); return 3; }
                    // End this diagnostic explicitly; do not advertise unimplemented services.
                    const unsigned char bye[]={0,15,8,1};
                    std::vector<uint8_t> encrypted;
                    if (!crypt->encrypt(bye,sizeof(bye),encrypted) || encrypted.size()>18432) return 3;
                    count=encrypted.size()+4;
                    buffer[0]=0; buffer[1]=11; buffer[2]=encrypted.size()>>8; buffer[3]=encrypted.size()&255;
                    memcpy(buffer+4,&encrypted[0],encrypted.size());
                    if (!transfer(context,0,buffer,&count) || count!=encrypted.size()+4) return 3;
                    puts("AA_DISCOVERY_PROBE_OK shutdown_request_sent=1 shutdown_ack_checked=0 no_projection=1");
                    return 0;
                }
                continue;
            }
        }
        count=sizeof(buffer);
        if (!transfer(context,1,buffer,&count) || !count || count>sizeof(buffer) ||
            count>65536-total || pending.size()+count>65536) return 3;
        total+=count; pending.insert(pending.end(),buffer,buffer+count);
    }
    puts("AA_DISCOVERY_LIMIT_REACHED"); return 3;
}
extern "C" int nbt_tls_probe_run(nbt_tls_transfer transfer, void* context, int discovery) {
    if (!crypt || !transfer) return 3;
    std::vector<uint8_t> outgoing, pending;
    unsigned char buffer[65536];
    unsigned total = 0, frames = 0;
    bool ok = crypt->startHandshake(outgoing);
    for (unsigned step=0; ok && step<32; ++step) {
        if (!outgoing.empty()) {
            if (outgoing.size()>16384) break;
            unsigned count=outgoing.size()+6;
            buffer[0]=0; buffer[1]=3;
            buffer[2]=(count-4)>>8; buffer[3]=(count-4)&255;
            buffer[4]=0; buffer[5]=3;
            memcpy(buffer+6,&outgoing[0],outgoing.size());
            if (!transfer(context,0,buffer,&count) || count != outgoing.size()+6) break;
            outgoing.clear();
        }
        if (crypt->isHandshakeComplete()) {
            puts("AA_USB_TLS12_HANDSHAKE_OK peer_verified=1 no_auth_complete=1 no_projection=1");
            return discovery ? discover(transfer,context,pending) : 0;
        }
        if (pending.size()>=4) {
            unsigned length=(pending[2]<<8)|pending[3];
            if (pending[0]!=0 || pending[1]!=3 || length<2 || length>16386) {
                puts("AA_TLS_FRAME_REFUSED"); break;
            }
            if (pending.size()>=length+4) {
                if (++frames>16 || pending[4]!=0 || pending[5]!=3) break;
                ok=crypt->processHandshakeData(&pending[6],length-2,outgoing);
                pending.erase(pending.begin(),pending.begin()+length+4);
                continue;
            }
        }
        unsigned count=sizeof(buffer);
        if (!transfer(context,1,buffer,&count) || !count || count>sizeof(buffer) ||
            count>262144-total || pending.size()+count>65536) break;
        total+=count; pending.insert(pending.end(),buffer,buffer+count);
    }
    printf("AA_USB_TLS_FAILED frames=%u bytes=%u detail=%s\n",frames,total,crypt->lastError());
    return 3;
}
