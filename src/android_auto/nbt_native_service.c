/* Manual-start standalone prototype. Owns only its children and RAM files.
 * The stock io-nme process is a shared service and is never signalled.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static volatile sig_atomic_t stopping;
static void stopped(int s){(void)s;stopping=1;}
static long long ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
typedef struct {pid_t pid;int status;} Child;
static int alive(Child* c){int s;pid_t r;if(!c->pid)return 0;r=waitpid(c->pid,&s,WNOHANG);if(r==c->pid){c->pid=0;c->status=s;return 0;}return r==0;}
static void end(Child* c){long long until;if(!alive(c))return;kill(-c->pid,SIGTERM);until=ms()+5000;
 while(alive(c)&&ms()<until)usleep(20000);
 if(alive(c)){kill(-c->pid,SIGKILL);while(waitpid(c->pid,&c->status,0)<0&&errno==EINTR){};c->pid=0;}}
static Child spawn(char*const args[],const char* input,const char* log){Child c={0,0};c.pid=fork();
 if(!c.pid){int fd;if(setpgid(0,0))_exit(125);signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);
  fd=open(input?input:"/dev/null",O_RDONLY);if(fd<0)_exit(126);dup2(fd,0);if(fd>2)close(fd);
  fd=open(log?log:"/dev/null",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)_exit(126);dup2(fd,1);dup2(fd,2);if(fd>2)close(fd);
  execv(args[0],args);_exit(127);
 }
 if(c.pid<0)c.pid=0;else setpgid(c.pid,c.pid);return c;}
/* Status for the HMI page (aa_entry_plugin): the private status.txt plus a world-readable copy /fs/ram3/nbt-aa-state
 * ("state=..", "phone=0|1"), rewritten at least every 2 s while the service runs, so a stale file means "service down". */
static int g_phone_present;
static long long g_last_report;static char g_last_state[32];
static void report(const char* state,unsigned session){FILE*f=fopen("status.tmp","w");
 if(f){fprintf(f,"pid=%d\nstate=%s\nsession=%u\nphone=%d\n",(int)getpid(),state,session,g_phone_present);fclose(f);rename("status.tmp","status.txt");}
 f=fopen("/fs/ram3/nbt-aa-state.tmp","w");
 if(f){fprintf(f,"state=%s\nphone=%d\nsession=%u\n",state,g_phone_present,session);fclose(f);chmod("/fs/ram3/nbt-aa-state.tmp",0644);
  rename("/fs/ram3/nbt-aa-state.tmp","/fs/ram3/nbt-aa-state");}
 snprintf(g_last_state,sizeof(g_last_state),"%s",state);g_last_report=ms();}
static void keep_reported(unsigned session){if(ms()-g_last_report>=2000)report(g_last_state,session);}
/* Generic phone detection (any Android phone, no list): every USB device except
 *   - the head unit's own devices: PEIKER telematics 2239:f004 (vendor 0x2239) is always on bus 0 address 1
 *   - Apple (0x05ac, no Android Auto), hubs (class 0x09), communication/HID/audio/mass-storage/video device classes
 *   - devices that already answered the AOA probe with "not supported" (ignored until they are unplugged)
 * is a candidate. Accessory mode is 18d1:2d00, or 18d1:2d01 with USB debugging on. Exactly one candidate is required. */
typedef struct {int bus,addr;unsigned v,p;} UsbDev;
static UsbDev g_ignored[8];static int g_ignored_n;
static int ignored(const UsbDev* u){int i;for(i=0;i<g_ignored_n;++i)if(g_ignored[i].bus==u->bus&&g_ignored[i].addr==u->addr&&g_ignored[i].v==u->v&&g_ignored[i].p==u->p)return 1;return 0;}
static void ignore_dev(int bus,int addr,unsigned v,unsigned p){if(g_ignored_n<8){UsbDev u={bus,addr,v,p};g_ignored[g_ignored_n++]=u;}}
static unsigned found_v,found_p,found_aoa;
static int find_phone(int* bus,int* device){
 FILE*f=popen("LD_LIBRARY_PATH=/lib:/usr/lib:/opt/sys/lib usb","r");char line[256];int b=-1,result=0,matches=0,n=0,i,j;
 UsbDev dev[16];unsigned cls[16];int have_cls[16];
 if(!f)return 0;
 while(fgets(line,sizeof(line),f)){
  int d;unsigned x;
  if(sscanf(line,"USB %d",&b)==1)continue;
  if(sscanf(line,"Device Address : %d",&d)==1){if(n<16&&b>=0&&d>0){dev[n].bus=b;dev[n].addr=d;dev[n].v=dev[n].p=0;cls[n]=0;have_cls[n]=0;++n;}continue;}
  if(!n)continue;
  if(sscanf(line,"Vendor : 0x%x",&x)==1)dev[n-1].v=x;
  else if(sscanf(line,"Product : 0x%x",&x)==1)dev[n-1].p=x;
  else if(!have_cls[n-1]&&sscanf(line,"Class : 0x%x",&x)==1){cls[n-1]=x;have_cls[n-1]=1;}
 }
 pclose(f);
 /* forget ignored devices that are gone */
 for(i=0;i<g_ignored_n;){int seen=0;for(j=0;j<n;++j)if(dev[j].bus==g_ignored[i].bus&&dev[j].addr==g_ignored[i].addr&&dev[j].v==g_ignored[i].v&&dev[j].p==g_ignored[i].p)seen=1;
  if(seen)++i;else g_ignored[i]=g_ignored[--g_ignored_n];}
 g_phone_present=0;
 for(i=0;i<n;++i){
  UsbDev*u=&dev[i];int type=0;unsigned c=cls[i];
  if(!u->v||!u->p||u->v==0x2239||u->v==0x05ac)continue;
  if(c==0x01||c==0x02||c==0x03||c==0x08||c==0x09||c==0x0e)continue;
  if(u->v==0x18d1&&(u->p==0x2d00||u->p==0x2d01))type=2;
  else if(!ignored(u))type=1;
  if(!type)continue;
  g_phone_present=1;++matches;*bus=u->bus;*device=u->addr;result=type;
  if(type==1){found_v=u->v;found_p=u->p;}else found_aoa=u->p;
 }
 return matches==1?result:0;
}
static int log_has(const char* path,const char* token){FILE*f=fopen(path,"r");char l[256];int hit=0;if(!f)return 0;
 while(!hit&&fgets(l,sizeof(l),f))hit=strstr(l,token)!=0;fclose(f);return hit;}
static void drain(int fd){char b[65536];while(read(fd,b,sizeof(b))>0){}}
/* split a line into at most max-1 space separated tokens in place (for the optional wal.args file) */
static int split(char*s,char**v,int max){int n=0;char*p=s;while(*p&&n<max-1){while(*p==' ')p++;if(!*p)break;v[n++]=p;while(*p&&*p!=' ')p++;if(*p)*p++=0;}v[n]=0;return n;}
/* center800: nbt_hwvideo hides the iDrive HMI window while Android Auto is in front; make sure it is visible again after
 * nbt_hwvideo ended (also after a crash or SIGKILL of it) */
static void hmi_visible(void){int r=system("LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_hwvideo --hmi-visible >/dev/null 2>&1");(void)r;}
int main(int argc,char**argv){
 Child lease={0,0},http={0,0},decoder={0,0},frames={0,0},capture={0,0},knob={0,0},hwv={0,0};
 (void)argv;
 int guard=-1,lock=-1,bus,device,type,attempts=0,live=0,wal=0,gate=0,hw=0,res720=0,center=0,ana=0,right=0;unsigned session=0;char b[16],d[16],shadow[200],walargs[160],*wargv[16];
 if(argc!=1||geteuid()!=0||chdir("/fs/ram3/nbt-aa-native"))return 2;
 signal(SIGTERM,stopped);signal(SIGINT,stopped);signal(SIGPIPE,SIG_IGN);
 lock=open("service.lock",O_WRONLY|O_CREAT|O_EXCL,0600);if(lock<0)return 2;close(lock);
 if(mkfifo("annexb.fifo",0600)||mkfifo("raw.fifo",0600))goto done;
 guard=open("raw.fifo",O_RDONLY|O_NONBLOCK);if(guard<0)goto done;fcntl(guard,F_SETFD,FD_CLOEXEC);
 setenv("LD_LIBRARY_PATH","/opt/mm/lib/cinemo:/opt/mm/lib:/lib:/usr/lib",1);
 setenv("NBT_AA_TLS_CERT","/fs/ram3/nbt-aa-native/cert.pem",1);
 setenv("NBT_AA_TLS_KEY","/fs/ram3/nbt-aa-native/key.pem",1);
 setenv("NBT_AA_TLS_CA","/fs/ram3/nbt-aa-native/ca.pem",1);
 /* Any phone: the phone's certificate must chain to ca.pem = Google Automotive Link root (valid until 2044), the root
  * behind every Android Auto phone; validity and key usage are still checked. No per-app leaf pin any more (the OnePlus
  * leaf sha256:893145f5... would break with another phone or an Android Auto app update). peer.pin restores a pin. */
 setenv("NBT_AA_TLS_PEER","chain",1);
 {FILE*f=fopen("peer.pin","r");char pin[80];if(f){if(fgets(pin,sizeof(pin),f)){size_t n=strlen(pin);while(n&&(pin[n-1]=='\n'||pin[n-1]=='\r'))pin[--n]=0;
   if(n==71&&!strncmp(pin,"sha256:",7))setenv("NBT_AA_TLS_PEER",pin,1);}fclose(f);}}
 setenv("NBT_AA_CAPTURE_PREFIX","/fs/ram3/nbt-aa-native/session",1);
 setenv("NBT_AA_LIVE_FIFO","/fs/ram3/nbt-aa-native/annexb.fifo",1);
 setenv("NBT_SDR_IDENTITY","1",1);setenv("NBT_CAR_MAKE","BMW",1);setenv("NBT_CAR_MODEL","F07",1);
 /* Phone canvas 800x480 (NBT_VID_RES=1 in capture). The reference default insets 0,240,0,0 are meant for a 1280x720 canvas
  * and left the phone only the top 240 rows (vehicle 2026-11-06: picture in the upper half only). 0,180,0,0 leaves
  * 800x300 = the 8:3 shape of the 1280x480 display; nbt_wal_present --mode full --rows 300 stretches it 1.6x both ways.
  * Not overwritten when the caller sets it (car-wal-start.sh). */
 setenv("NBT_VID_INSETS","0,180,0,0",0);
 /* pad each H.264 access unit to 8 KB with a filler NAL so the decoder's byte buffers fill quickly (input lag) */
 setenv("NBT_AA_PAD","8192",0);
 setenv("NBT_CAR_YEAR","2014",1);setenv("NBT_HU_MODEL","HU_NBT",1);setenv("NBT_SDR_DROP","2,4,9",1);setenv("NBT_RX_TIMEOUT_S","5",1);
 /* Optional native mode: if shadow.path names a file under /fs/usb0/, nbt_live_file shadows that file with the live
  * H.264 stream so the STOCK USB video player plays it (HMI video page, VideoApp, capture, WAL). In that mode
  * fifo_http, native_frames and decode_runner are not started. Without shadow.path nothing changes. */
 shadow[0]=0;
 {FILE*f=fopen("shadow.path","r");if(f){if(fgets(shadow,sizeof(shadow),f)){size_t n=strlen(shadow);
   while(n&&(shadow[n-1]=='\n'||shadow[n-1]=='\r'||shadow[n-1]==' '))shadow[--n]=0;
   if((strncmp(shadow,"/fs/usb0/",9)&&strncmp(shadow,"/fs/pfs0/",9))||n<=9)shadow[0]=0;}fclose(f);}}
 live=shadow[0]!=0;
 /* Optional WAL overlay mode: if wal.args exists, nbt_wal_present (decoded picture as a WAL sprite in front of the HMI,
  * arguments from that file, may be empty) replaces native_frames (3 fps PNG into the HMI image page). */
 walargs[0]=0;
 {FILE*f=fopen("wal.args","r");if(f){wal=1;if(fgets(walargs,sizeof(walargs),f)){size_t n=strlen(walargs);while(n&&(walargs[n-1]=='\n'||walargs[n-1]=='\r'))walargs[--n]=0;}fclose(f);}}
 if(wal){wargv[0]="./nbt_wal_present";split(walargs,wargv+1,15);}
 gate=access("gate.mode",F_OK)==0;
 /* Hardware video mode (hw.mode, car-proven path 2026-11-07): the decoder renders into the stock video window
  * (decode_runner "screen"), nbt_hwvideo asks VideoApp for the video connection and shows it in front of iDrive.
  * No YUV FIFO, no RGBA conversion, no WAL sprites. Takes precedence over wal.args. */
 hw=access("hw.mode",F_OK)==0&&access("nbt_hwvideo",X_OK)==0;
 /* res720 (hardware mode only): the phone sends 1280x720 and draws into rows 0..479 (1280x480, the display shape);
  * the decoder crops these rows. Sharper vertically than 800x300; the capture stays 800x480. */
 res720=hw&&access("res720",F_OK)==0;
 if(res720){setenv("NBT_VID_RES","2",1);setenv("NBT_VID_INSETS","0,240,0,0",1);}
 /* center800 (hardware mode only, wins over res720; user request 2026-11-08): the phone draws the whole 800x480 canvas,
  * shown 1:1 in the middle of the 1280x480 display (black bars of 240 px left and right). No scaling at all: as sharp as
  * the video path allows (the capture is 800x480 anyway), instead of 800x300 stretched 1.6x both ways. */
 center=hw&&access("center800",F_OK)==0;
 /* split800 (user idea 2026-11-08): same 1:1 picture at the LEFT edge (0..799); iDrive stays visible on the right
  * 480 px, exactly where its own split screen (480x480 windows) sits */
 right=hw&&!center&&access("split800",F_OK)==0;
 if(right)center=1;
 if(center){res720=0;setenv("NBT_VID_RES","1",1);setenv("NBT_VID_INSETS","0,0,0,0",1);}
 /* anamorph (hardware mode only, user idea 2026-11-08): the phone is told that the pixels are 1.6x wider than tall
  * (VideoConfig pixel_aspect_ratio_e4 = 16000) and draws its whole 800x480 canvas squeezed horizontally; the GPU
  * stretches 800 -> 1280 only horizontally, so the picture fills the display with 480 instead of 300 rows */
 ana=hw&&!center&&access("anamorph",F_OK)==0;
 if(ana){res720=0;setenv("NBT_VID_RES","1",1);setenv("NBT_VID_INSETS","0,0,0,0",1);setenv("NBT_VID_PAR","16000",0);}
 if(live){char*args[]={"./nbt_live_file","--path",shadow,"--fifo","annexb.fifo","--ready","http.ready","--seconds","0",0};
  unlink("http.ready");http=spawn(args,0,"live_file.log");
  {long long limit=ms()+5000;while(!stopping&&access("http.ready",F_OK)&&alive(&http)&&ms()<limit)usleep(100000);}
  if(access("http.ready",F_OK)||!alive(&http))goto done;}
 {char* args[]={"./accessory_window","--owned-window",0};lease=spawn(args,0,"window.log");}
 usleep(500000);if(!alive(&lease))goto done;
 /* Optional iDrive controller (nbt_knob present): observe-only MOST client, writes key lines into input.fifo, which the
  * live AA session (oneplus_native --capture-video) reads. Runs for the whole service life, independent of sessions. */
 /* nbt_knob needs /opt/sys/lib (libsys_dsi_colibry.so.3 and its dependencies; car 2026-11-07: "Library cannot be found"
  * with the Cinemo library path), so it gets its own library path for the fork. */
 if(!access("nbt_knob",X_OK)&&(!mkfifo("input.fifo",0600)||errno==EEXIST)){char*args[]={"./nbt_knob",0};
  setenv("LD_LIBRARY_PATH","/opt/sys/lib:/lib:/usr/lib",1);knob=spawn(args,0,"knob.log");
  setenv("LD_LIBRARY_PATH","/opt/mm/lib/cinemo:/opt/mm/lib:/lib:/usr/lib",1);}
 while(!stopping&&alive(&lease)){
  /* Optional HMI gate (file gate.mode in the runtime directory): the phone is only touched after the Android Auto
   * page in the HMI was opened, which creates /fs/ram3/nbt-aa-request (see src/idrive/aa_entry_plugin.cpp). */
  if(gate&&access("/fs/ram3/nbt-aa-request",F_OK)){find_phone(&bus,&device);report("waiting_for_hmi",session);sleep(1);continue;}
  type=find_phone(&bus,&device);
  if(!type){attempts=0;report(g_phone_present?"several_devices":"waiting_for_phone",session);sleep(1);continue;}
  if(attempts>=3){report("replug_required",session);sleep(2);continue;}
  if(type==1){Child sw;char*args[]={"./oneplus_native","--switch-aa",b,d,0};char id[16];
   report("connecting",session);
   snprintf(id,sizeof(id),"%04x:%04x",found_v,found_p);setenv("NBT_PHONE_ID",id,1);
   snprintf(b,sizeof(b),"%d",bus);snprintf(d,sizeof(d),"%d",device);sw=spawn(args,0,"switch.log");
   while(!stopping&&alive(&sw))usleep(100000);end(&sw);if(stopping)break;
   /* no AOA answer at all: not an Android phone (USB stick, other device): leave it alone until it is unplugged */
   if(!log_has("switch.log","AOA_PROTOCOL_OK")){ignore_dev(bus,device,found_v,found_p);report("unsupported_device",session);sleep(1);continue;}
   sleep(1);if(find_phone(&bus,&device)!=2){++attempts;continue;}
  }
  report("connecting",session);
  ++attempts;++session;report("starting_decoder",session);drain(guard);
  /* no leftover client of the shared io-nme from an earlier, abruptly ended session (only our own NmeCmdLine copy
   * exists: the stock player works through mme/io-media-cinemo, never NmeCmdLine) */
  if(hw){int r=system("slay -f -Q -s TERM NmeCmdLine >/dev/null 2>&1");(void)r;usleep(500000);
   r=system("slay -f -Q -s KILL NmeCmdLine >/dev/null 2>&1");(void)r;}
  if(found_aoa==0x2d01)setenv("NBT_AOA_PRODUCT","2d01",1);else unsetenv("NBT_AOA_PRODUCT");
  if(!live){
   unlink("http.ready");
   {char*args[]={"./fifo_http","annexb.fifo","http.ready",0};http=spawn(args,0,0);}
   if(hw){}
   else if(wal)frames=spawn(wargv,"raw.fifo","present.log");
   else{char*args[]={"./native_frames","--seconds","0","--fps","3",0};frames=spawn(args,"raw.fifo","frames.log");}
   {char*args[]={"./decode_runner","0","/fs/ram3/nbt-aa-native/NmeCmdLine","http://127.0.0.1:18765/live.h264",hw?((center||ana)?"screencenter":res720?"screen720":"screen"):"/fs/ram3/nbt-aa-native/raw.fifo",0};char decname[40];snprintf(decname,sizeof(decname),"decoder-%u.log",session);decoder=spawn(args,0,decname);}
   {long long limit=ms()+10000;while(!stopping&&access("http.ready",F_OK)&&alive(&http)&&alive(&decoder)&&ms()<limit)usleep(100000);}
   /* the decoder already crops to the content rows (-vr), so the window is the plain 800x480 output */
   if(hw&&!stopping){char*args[]={"./nbt_hwvideo","--rows","480",center?"--viewport":0,right?"0,0,800,480":"240,0,800,480",right?"--keep-hmi":0,0};char hwname[40];snprintf(hwname,sizeof(hwname),"hwvideo-%u.log",session);
    setenv("LD_LIBRARY_PATH","/opt/sys/lib:/lib:/usr/lib",1);hwv=spawn(args,0,hwname);
    setenv("LD_LIBRARY_PATH","/opt/mm/lib/cinemo:/opt/mm/lib:/lib:/usr/lib",1);}
  }
  /* live mode: nbt_live_file (kept in 'http') runs for the whole service life; no decoder or PNG converter. */
  if(!stopping&&(live?alive(&http):(!access("http.ready",F_OK)&&alive(&decoder)&&alive(&http)))){
   char*args[]={"./oneplus_native","--capture-video",b,d,0};snprintf(b,sizeof(b),"%d",bus);snprintf(d,sizeof(d),"%d",device);
   {char capname[40];snprintf(capname,sizeof(capname),"capture-%u.log",session);capture=spawn(args,0,capname);}report("running",session);
   while(!stopping&&alive(&capture)&&alive(&http)&&(live||(alive(&decoder)&&(hw||alive(&frames))))){usleep(200000);keep_reported(session);}
  }
  report("draining",session);end(&hwv);if(center)hmi_visible();end(&capture);if(!live)end(&http);
  {long long limit=ms()+3000;while(alive(&decoder)&&ms()<limit)usleep(20000);}
  end(&decoder);
  {long long limit=ms()+2000;while(alive(&frames)&&ms()<limit)usleep(20000);}
  end(&frames);drain(guard);if(!live)unlink("http.ready");
  // Clear only the FIFO bytes left by this completed session (live mode: nbt_live_file owns the FIFO reader).
  if(!live){int fd=open("annexb.fifo",O_RDONLY|O_NONBLOCK);if(fd>=0){drain(fd);close(fd);}}
  if(!stopping)sleep(2);
 }
done:
 end(&hwv);if(center)hmi_visible();end(&capture);end(&http);end(&decoder);end(&frames);end(&knob);end(&lease);unlink("input.fifo");
 if(guard>=0){drain(guard);close(guard);}unlink("annexb.fifo");unlink("raw.fifo");unlink("http.ready");unlink("service.lock");report("stopped",session);
 unlink("/fs/ram3/nbt-aa-state");
 return 0;
}
