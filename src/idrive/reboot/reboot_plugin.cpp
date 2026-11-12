// libNbtReboot.so - "Restart iDrive" entry under Multimedia (HMI plugin, /var/opt/hmi/data/rw/Plugins/NbtReboot).
//
// Page 10 asks ("Restart the head unit now?") and has one button, "Restart now", which leads to the inert page 20.
// When page 20 gets the focus, this plugin runs nbt_reset from the plugin folder: UDS 11 01 (hardReset) to the head
// unit through the gateway's HSFZ port, the same request as Tool32's STEUERGERAETE_RESET. Car 2026-11-12: run from the
// head unit with the gateway address, the reset was accepted and the head unit rebooted. No HMI process is touched.
//
// Guards (the HMI may restore the last page after a reboot, which must never restart the head unit again):
//   - nothing within the first 30 s after boot
//   - only when page 10 got the focus less than 2 minutes before (the driver opened the entry and pushed the button)
//   - at most one start per 30 s
// The gateway address comes from the file "gateway" in the plugin folder (one line, e.g. 169.254.102.32); without
// it nbt_reset looks for the gateway with the HSFZ broadcast. Log: /fs/ram3/nbt-reboot.log (RAM, gone after the
// restart).
// ABI as aa_entry_plugin.cpp (car-tested PluginApp ctor, addHandler(state, 1 = focus, ...)).
#include "../nbt_image_abi.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <spawn.h>
#include <signal.h>
#include <new>
extern char** environ;
namespace bmw {
class IDispatcher; class Bundle; class IPluginApp;
class Parcel { public: bool readBool(bool&) const; };
template<class R,class A1,class A2,class A3,class A4> struct Delegate {
    void* object;
    R (*stub)(void*,A1);
};
typedef Delegate<bool,const Parcel&,void,void,void> ActionDelegate;
class PluginApp {
public:
    PluginApp(IDispatcher&,BasicString<char>,const Bundle&,const Bundle&);
    ~PluginApp();
    void addHandler(unsigned,unsigned,const ActionDelegate&);
private:
    uint32_t opaque_[1024];
};
}
namespace {
const unsigned kAskState=10, kGoState=20, kFocus=1;
const char* const kTool="/var/opt/hmi/data/rw/Plugins/NbtReboot/nbt_reset";
const char* const kGatewayFile="/var/opt/hmi/data/rw/Plugins/NbtReboot/gateway";
const char* const kLog="/fs/ram3/nbt-reboot.log";
const long kBootGuard=30, kAskWindow=120, kRepeatGuard=30;
long g_askFocus=-100000, g_lastRun=-100000;

long uptime_s() { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (long)t.tv_sec; }

void plog(const char* what) {
    int fd=open(kLog,O_WRONLY|O_CREAT|O_APPEND,0644);
    if(fd<0)return;
    char line[128]; int n=snprintf(line,sizeof(line),"%ld plugin: %s\n",uptime_s(),what);
    if(n>0)write(fd,line,(size_t)n);
    close(fd);
}

// first line of the gateway file, digits and dots only; empty if missing
void readGateway(char* out,size_t size) {
    out[0]=0;
    int fd=open(kGatewayFile,O_RDONLY);
    if(fd<0)return;
    char buf[32]; ssize_t n=read(fd,buf,sizeof(buf)-1); close(fd);
    if(n<=0)return;
    size_t i=0;
    while(i<(size_t)n&&i+1<size&&((buf[i]>='0'&&buf[i]<='9')||buf[i]=='.')){out[i]=buf[i];++i;}
    out[i]=0;
}

void runReset() {
    char gw[20]; readGateway(gw,sizeof(gw));
    int out=open(kLog,O_WRONLY|O_CREAT|O_APPEND,0644);
    int nul=open("/dev/null",O_RDWR);
    if(nul<0){if(out>=0)close(out);plog("cannot open /dev/null");return;}
    int fdmap[3]={nul,out>=0?out:nul,out>=0?out:nul};
    struct inheritance inh;
    memset(&inh,0,sizeof(inh));
    inh.flags=SPAWN_SETGROUP|SPAWN_SETSIGDEF|SPAWN_SETSIGMASK|SPAWN_NOZOMBIE;
    inh.pgroup=SPAWN_NEWPGROUP;
    sigfillset(&inh.sigdefault);
    sigemptyset(&inh.sigmask);
    char* argv[]={(char*)kTool,gw[0]?gw:0,0};
    // the HMI's own environment: its LD_LIBRARY_PATH finds libsocket.so.3 (car 2026-11-12: with PATH only, the loader
    // failed with "Could not load library libsocket.so.3"; HmiMain3D has it loaded)
    if(spawn(kTool,3,fdmap,&inh,argv,environ)<0)plog("could not start nbt_reset");
    close(nul);
    if(out>=0)close(out);
}

bool askFocus(void*,const bmw::Parcel& p) {
    bool focused=false;p.readBool(focused);
    if(focused){g_askFocus=uptime_s();plog("page 10 opened");}
    return true;
}

bool goFocus(void*,const bmw::Parcel& p) {
    bool focused=false;p.readBool(focused);
    if(!focused)return true;
    const long t=uptime_s();
    if(t<kBootGuard){plog("ignored: less than 30 s after boot (restored page)");return true;}
    if(t-g_askFocus>kAskWindow){plog("ignored: page 10 was not opened just before");return true;}
    if(t-g_lastRun<kRepeatGuard){plog("ignored: restart already started");return true;}
    g_lastRun=t;
    plog("restart requested, running nbt_reset");
    runReset();
    return true;
}

bmw::PluginApp* g_app=0;
// car 2026-11-12: with a null delegate object the focus handlers never ran (no log line at all), while the Android Auto
// plugin's handlers, registered with a real object, do: the HMI apparently treats a null object as an empty delegate
int g_handlerObject=0;
bmw::IPluginApp* create(bmw::IDispatcher& d,const char* name,const bmw::Bundle& p,const bmw::Bundle& c) {
    if(g_app)return 0;
    g_app=new bmw::PluginApp(d,bmw::BasicString<char>(name),p,c);
    plog("plugin created");
    bmw::ActionDelegate onAsk={&g_handlerObject,&askFocus};
    bmw::ActionDelegate onGo={&g_handlerObject,&goFocus};
    g_app->addHandler(kAskState,kFocus,onAsk);
    g_app->addHandler(kGoState,kFocus,onGo);
    return reinterpret_cast<bmw::IPluginApp*>(g_app);
}
void destroy(bmw::IPluginApp* p) {
    if(!g_app)return;
    delete reinterpret_cast<bmw::PluginApp*>(p);
    g_app=0;
}
}
struct PluginAppInfo {
    uint16_t major,minor;const char* name;const char* vendor;
    bmw::IPluginApp*(*create)(bmw::IDispatcher&,const char*,const bmw::Bundle&,const bmw::Bundle&);
    void(*destroy)(bmw::IPluginApp*);
};
typedef char InfoSize[(sizeof(PluginAppInfo)==20)?1:-1];
extern "C" { PluginAppInfo pluginAppInfo={1,0,"NbtReboot","NBT-Project",create,destroy}; }
