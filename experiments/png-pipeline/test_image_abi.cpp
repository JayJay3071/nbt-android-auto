#include "nbt_image_abi.hpp"
#include <stdio.h>
#include <string.h>
#include <new>
int main() {
    unsigned char input[65536], expected[65536], output[65536];
    for(unsigned i=0;i<sizeof(input);++i) expected[i]=(unsigned char)(i*37);
    for(unsigned cycle=0;cycle<2000;++cycle) {
        memcpy(input,expected,sizeof(input));
        struct Guard { uint32_t before; uint32_t image[8]; uint32_t after; } g;
        memset(&g,0,sizeof(g));g.before=0x12345678;g.after=0x87654321;
        bmw::Image* image=new(g.image) bmw::Image(input,sizeof(input));
        memset(input,0,sizeof(input)); // native constructor must retain its own bytes
        if(memcmp(image->getImageStream().getBuffer(),expected,sizeof(expected))) return 2;
        {
            bmw::Image copy(*image);
            unsigned count=0;
            copy.getImageStream().read2(output,sizeof(output),&count);
            if(count!=sizeof(output)||memcmp(output,expected,sizeof(output)))return 3;
        }
        image->~Image();
        if(g.before!=0x12345678||g.after!=0x87654321)return 4;
    }
    puts("PASS native image ABI: 2000 construct/copy/read/destroy cycles, owned bytes, intact guards");
    return 0;
}
