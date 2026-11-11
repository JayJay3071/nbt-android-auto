// Bounded-memory YUV420P -> RGB PNG -> native RHMI packet publisher.
// Decoder input is a pipe; raw frames are never accumulated on disk.
#include "../idrive/frame_packet.hpp"
#include <zlib.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>
static volatile sig_atomic_t stop=0;
static void stopped(int){stop=1;}
static long long ms(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static void big(std::vector<unsigned char>& v,unsigned n){v.push_back(n>>24);v.push_back(n>>16);v.push_back(n>>8);v.push_back(n);}
static void little(unsigned char* p,unsigned n){for(int i=0;i<4;++i)p[i]=(n>>(i*8))&255;}
static void chunk(std::vector<unsigned char>& png,const char* type,const unsigned char* data,unsigned size){
 big(png,size);size_t begin=png.size();png.insert(png.end(),type,type+4);
 if(size)png.insert(png.end(),data,data+size);
 big(png,::crc32(0,&png[begin],size+4));
}
static unsigned char clip(int n){return n<0?0:n>255?255:n;}
static bool pngFrame(const unsigned char* yuv,std::vector<unsigned char>& png){
 const unsigned w=480,h=288,iw=800,ih=480;std::vector<unsigned char> rgb((w*3+1)*h);
 for(unsigned y=0;y<h;++y){unsigned sy=y*ih/h;unsigned char* row=&rgb[y*(w*3+1)];*row++=0;
  for(unsigned x=0;x<w;++x){unsigned sx=x*iw/w;int yy=int(yuv[sy*iw+sx])-16;
   int u=int(yuv[iw*ih+(sy/2)*(iw/2)+sx/2])-128;
   int v=int(yuv[iw*ih*5/4+(sy/2)*(iw/2)+sx/2])-128;
   *row++=clip((298*yy+409*v+128)>>8);*row++=clip((298*yy-100*u-208*v+128)>>8);*row++=clip((298*yy+516*u+128)>>8);
  }
 }
 std::vector<unsigned char> compressed(compressBound(rgb.size()));uLongf size=compressed.size();
 if(compress2(&compressed[0],&size,&rgb[0],rgb.size(),1)!=Z_OK)return false;
 const unsigned char signature[]={137,80,78,71,13,10,26,10};png.assign(signature,signature+8);
 std::vector<unsigned char> ihdr;big(ihdr,w);big(ihdr,h);ihdr.push_back(8);ihdr.push_back(2);ihdr.push_back(0);ihdr.push_back(0);ihdr.push_back(0);
 chunk(png,"IHDR",&ihdr[0],ihdr.size());chunk(png,"IDAT",&compressed[0],size);chunk(png,"IEND",0,0);
 return png.size()<=nbt::MaxPng;
}
static bool writeAll(int fd,const unsigned char* p,size_t n){while(n){ssize_t k=write(fd,p,n);if(k<=0)return false;p+=k;n-=k;}return true;}
static bool status(unsigned& instance,unsigned& sequence){
 FILE* f=fopen("/fs/ram3/nbt-live-status.txt","r");if(!f)return false;
 char line[128],state[64]="";instance=sequence=0;
 while(fgets(line,sizeof(line),f)){sscanf(line,"instance=%u",&instance);sscanf(line,"sequence=%u",&sequence);sscanf(line,"state=%63s",state);}fclose(f);
 return instance&&(!strcmp(state,"focused")||!strcmp(state,"polling")||!strcmp(state,"updated")||!strcmp(state,"rejected"));
}
int main(int argc,char**argv){
 const char* output="/fs/ram3/nbt-live-frame.bin";unsigned testInstance=0,seconds=60,fps=3;
 for(int i=1;i<argc;++i){if(i+1>=argc)return 2;
  if(!strcmp(argv[i],"--output"))output=argv[++i];
  else if(!strcmp(argv[i],"--instance"))testInstance=strtoul(argv[++i],0,10);
  else if(!strcmp(argv[i],"--seconds"))seconds=strtoul(argv[++i],0,10);
  else if(!strcmp(argv[i],"--fps"))fps=strtoul(argv[++i],0,10);
  else return 2;
 }
 if(fps>5||seconds>3600||(getuid()==0&&strncmp(output,"/fs/ram3/nbt-",13)))return 2;
 signal(SIGINT,stopped);signal(SIGTERM,stopped);signal(SIGPIPE,SIG_IGN);setvbuf(stdout,0,_IOLBF,0);
 std::vector<unsigned char> frame(800*480*3/2),png;size_t used=0;unsigned frames=0,published=0,sequence=0,instance=0;
 long long start=ms(),last=0;char temp[512];
 if(snprintf(temp,sizeof(temp),"%s.%d.tmp",output,int(getpid()))>=(int)sizeof(temp))return 2;
 while(!stop&&(!seconds||ms()-start<seconds*1000LL)){
  fd_set fds;FD_ZERO(&fds);FD_SET(0,&fds);timeval timeout={1,0};int ready=select(1,&fds,0,0,&timeout);
  if(ready<0){if(stop)break;return 3;}if(!ready)continue;
  ssize_t n=read(0,&frame[used],frame.size()-used);if(!n)break;if(n<0)return 3;used+=n;
  if(used!=frame.size())continue;used=0;++frames;
  if(fps&&last&&ms()-last<1000/fps)continue;
  unsigned current=testInstance,seen=0;if(!testInstance&&!status(current,seen))continue;
  if(current!=instance){instance=current;sequence=seen;}
  if(!pngFrame(&frame[0],png))return 4;
  unsigned char header[32]={ 'N','B','T','F','R','M','1',0 };
  if(!++sequence)++sequence;
  little(header+8,instance);little(header+12,sequence);little(header+16,png.size());little(header+20,::crc32(0,&png[0],png.size()));
  int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return 5;
  bool ok=writeAll(fd,header,sizeof(header))&&writeAll(fd,&png[0],png.size());if(close(fd))ok=false;
  if(!ok||rename(temp,output)){unlink(temp);return 5;}++published;last=ms();
 }
 printf("NATIVE_FRAMES_END input=%u published=%u partial=%u instance=%u sequence=%u elapsed_ms=%lld\n",frames,published,unsigned(used),instance,sequence,ms()-start);
 return used&&!stop?6:0;
}
