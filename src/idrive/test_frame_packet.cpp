#include "frame_packet.hpp"
#include <stdio.h>
#include <vector>
int main(int argc,char** argv) {
    if(argc!=2)return 1;
    FILE* f=fopen(argv[1],"rb");if(!f)return 2;
    std::vector<unsigned char> png(nbt::MaxPng+1);
    size_t n=fread(&png[0],1,png.size(),f);fclose(f);
    if(!nbt::validPng(&png[0],n))return 3;
    if(nbt::validPng(&png[0],n-1))return 4;
    png[n/2]^=1;if(nbt::validPng(&png[0],n))return 5;png[n/2]^=1;
    png[n]=0;if(nbt::validPng(&png[0],n+1))return 6;
    unsigned char h[32]={ 'N','B','T','F','R','M','1',0, 7,0,0,0, 1,0,0,0, 57,0,0,0 };
    if(!nbt::validHeader(h,7,89)||nbt::validHeader(h,8,89)||nbt::validHeader(h,7,90))return 7;
    h[24]=1;if(nbt::validHeader(h,7,89))return 8;
    puts("PASS frame validation: actual PNG, CRC corruption, truncation, trailing data, stale instance, length, reserved fields");
    return 0;
}
