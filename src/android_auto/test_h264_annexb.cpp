#include "h264_annexb.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef std::vector<uint8_t> Bytes;
struct Output { std::vector<Bytes> nals; bool reject; Output():reject(false){} };
static bool collect(void* context,const uint8_t* bytes,size_t n) {
    Output* out=(Output*)context;
    if (out->reject) return false;
    out->nals.push_back(Bytes(bytes,bytes+n)); return true;
}
static void require(bool ok,const char* message) {
    if (!ok) { fprintf(stderr,"FAIL %s\n",message); exit(1); }
}
static void negative_cases() {
    const uint8_t invalid[][8]={
        {1,2,3,0,0,1,0x65,1}, {0,0,1,0x80,1,1,1,1},
        {0,0,1,0,1,1,1,1}, {0,0,1,0x78,1,1,1,1},
        {0,0,1,0,0,1,0x65,1}};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        Output out; nbt::AnnexBInput input(64,collect,&out);
        require(!input.feed(invalid[i],8) || !input.finish(),"bad stream rejected");
        require(input.failed() && !input.feed(NULL,0),"failure sticky");
    }
    Output out; nbt::AnnexBInput input(8,collect,&out);
    const uint8_t exact[]={0,0,1,0x65,1,1,1,1,1,1,1,0,0,0,1,0x41,2};
    require(input.feed(exact,sizeof(exact)) && input.finish(),"exact limit followed by delimiter accepted"); input.reset();
    const uint8_t good[]={0,0,1,0x65,1};
    require(!input.feed(NULL,1),"null nonempty rejected"); input.reset();
    Bytes oversized(40,1); oversized[0]=oversized[1]=0;
    require(!input.feed(&oversized[0],oversized.size()),"NAL overflow rejected"); input.reset();
    Bytes zeros(40,0); require(!input.feed(&zeros[0],zeros.size()),"zero overflow rejected"); input.reset();
    require(input.feed(good,sizeof(good)) && input.finish(),"reset recovers");
    require(!input.finish(),"duplicate finish rejected"); input.reset();
    out.reject=true; require(input.feed(good,sizeof(good)) && !input.finish(),"callback refusal propagated");
}
int main(int argc,char**argv) {
    alarm(15); negative_cases();
    if (argc!=3) return 2;
    FILE* f=fopen(argv[1],"rb"); require(f!=NULL,"open fixture");
    Bytes data; uint8_t buffer[4096]; size_t n;
    while ((n=fread(buffer,1,sizeof(buffer),f))) {
        require(data.size()+n<=2*1024*1024,"fixture bound"); data.insert(data.end(),buffer,buffer+n);
    }
    require(!ferror(f),"fixture read"); fclose(f);
    Output baseline;
    const size_t chunks[]={1,2,3,7,31,1024,65536};
    for (unsigned c=0;c<sizeof(chunks)/sizeof(chunks[0]);++c) {
        Output out; nbt::AnnexBInput input(1024*1024,collect,&out);
        for (size_t p=0;p<data.size();) {
            size_t take=data.size()-p; if (take>chunks[c]) take=chunks[c];
            require(input.feed(&data[p],take),"fixture chunks accepted"); p+=take;
        }
        require(input.finish(),"fixture complete");
        if (!c) baseline=out; else require(out.nals==baseline.nals,"packetization independence");
    }
    unsigned counts[32]={0};
    for (size_t i=0;i<baseline.nals.size();++i) ++counts[baseline.nals[i][0]&31];
    require(counts[7] && counts[8] && counts[5],"fixture SPS PPS IDR present");
    f=fopen(argv[2],"wb"); require(f!=NULL,"open normalized output");
    const uint8_t start[]={0,0,0,1};
    for (size_t i=0;i<baseline.nals.size();++i) {
        require(fwrite(start,1,4,f)==4,"write prefix");
        require(fwrite(&baseline.nals[i][0],1,baseline.nals[i].size(),f)==baseline.nals[i].size(),"write NAL");
    }
    require(fclose(f)==0,"close output");
    printf("H264_ANNEXB_OK chunks=7 nals=%u sps=%u pps=%u idr=%u bounds malformed reset callback-rejection\n",
        (unsigned)baseline.nals.size(),counts[7],counts[8],counts[5]);
    return 0;
}
