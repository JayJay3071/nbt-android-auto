#ifndef NBT_FRAME_PACKET_HPP
#define NBT_FRAME_PACKET_HPP
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace nbt {
enum { HeaderSize=32, MaxPng=512*1024 };
inline uint32_t le32(const unsigned char* p) {
    return p[0]|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
}
inline uint32_t crc32(const unsigned char* p,size_t n) {
    uint32_t c=~uint32_t(0);
    for(size_t i=0;i<n;++i) {
        c^=p[i];
        for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int(c&1)));
    }
    return ~c;
}
inline bool validPng(const unsigned char* p,size_t n) {
    static const unsigned char signature[]={137,80,78,71,13,10,26,10};
    if(n<57||n>MaxPng||memcmp(p,signature,8))return false;
    size_t pos=8; bool header=false,data=false;
    while(pos+12<=n) {
        uint32_t len=be32(p+pos);
        if(len>n-pos-12)return false;
        const unsigned char* type=p+pos+4;
        if(crc32(type,size_t(len)+4)!=be32(p+pos+8+len))return false;
        if(!memcmp(type,"IHDR",4)) {
            if(header||pos!=8||len!=13)return false;
            unsigned w=be32(p+pos+8),h=be32(p+pos+12);
            if(!w||w>800||!h||h>480||p[pos+16]!=8||
               (p[pos+17]!=2&&p[pos+17]!=6)||p[pos+18]||p[pos+19]||p[pos+20])return false;
            header=true;
        } else if(!memcmp(type,"IDAT",4))data=true;
        else if(!memcmp(type,"IEND",4))return header&&data&&len==0&&pos+12==n;
        pos+=len+12;
    }
    return false;
}
inline bool validHeader(const unsigned char* h,uint32_t instance,size_t total) {
    return !memcmp(h,"NBTFRM1",8)&&le32(h+8)==instance&&le32(h+12)!=0&&
        le32(h+16)<=MaxPng&&le32(h+16)>=57&&total==HeaderSize+le32(h+16)&&
        le32(h+24)==0&&le32(h+28)==0;
}
}
#endif
