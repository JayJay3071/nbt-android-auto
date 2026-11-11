// Native frame-update prototype. No USB, graphics driver, threads or firmware changes.
#include "nbt_image_abi.hpp"
#include "frame_packet.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <new>
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
    void post(Runnable*,unsigned);
    void updateModel(unsigned,const Image&);
    void addHandler(unsigned,unsigned,const ActionDelegate&);
private:
    uint32_t opaque_[1024];
};
}
namespace {
const char* const kFrame="/fs/ram3/nbt-live-frame.bin";
const char* const kStatus="/fs/ram3/nbt-live-status.txt";
class Receiver : public bmw::Runnable {
public:
    bmw::PluginApp* app;
    uint32_t instance,sequence;
    unsigned polls,frames,rejected;
    bool stopping,started,active;
    unsigned char data[nbt::MaxPng];
    Receiver(bmw::PluginApp* p):app(p),sequence(0),polls(0),frames(0),rejected(0),stopping(false),started(false),active(false) {
        struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
        instance=uint32_t(ts.tv_nsec)^uint32_t(ts.tv_sec)^uint32_t(getpid());
        if(!instance)instance=1;
        status("waiting_for_focus");
    }
    static bool focus(void* object,const bmw::Parcel& parcel) {
        Receiver* self=static_cast<Receiver*>(object);
        bool focused=false;
        parcel.readBool(focused);
        self->active=focused;
        if(!focused)self->status("inactive");
        if(focused&&!self->stopping&&!self->started) {
            self->started=true;
            self->status("focused");
            self->app->post(self,200);
        }
        return true;
    }
    void status(const char* state) {
        FILE* f=fopen("/fs/ram3/nbt-live-status.tmp","w");
        if(!f)return;
        fprintf(f,"instance=%u\npid=%d\nstate=%s\nframes=%u\nrejected=%u\nsequence=%u\npolls=%u\n",
                instance,int(getpid()),state,frames,rejected,sequence,polls);
        fclose(f);rename("/fs/ram3/nbt-live-status.tmp",kStatus);
    }
    static bool readAll(int fd,unsigned char* p,size_t n) {
        while(n) { ssize_t got=read(fd,p,n);if(got<=0)return false;p+=got;n-=got; }
        return true;
    }
    void run() {
        if(stopping)return;
        if(!active){started=false;return;}
        // Poll only while the page is focused. No artificial session expiry.
        ++polls;
        if(polls==1)status("polling");
        int fd=open(kFrame,O_RDONLY|O_NONBLOCK);
        if(fd>=0) {
            struct stat st;unsigned char h[nbt::HeaderSize];
            bool valid=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>=nbt::HeaderSize&&
                st.st_size<=nbt::HeaderSize+nbt::MaxPng&&readAll(fd,h,sizeof(h))&&
                nbt::validHeader(h,instance,size_t(st.st_size));
            if(valid&&nbt::le32(h+12)!=sequence) {
                unsigned size=nbt::le32(h+16);
                valid=readAll(fd,data,size)&&nbt::crc32(data,size)==nbt::le32(h+20)&&nbt::validPng(data,size);
                if(valid) {
                    // Copying ownership and destructor order verified against native libs in VM.
                    bmw::Image image(data,size);
                    app->updateModel(6,image);
                    sequence=nbt::le32(h+12);++frames;
                    status("updated"); // means update submitted, not visual acknowledgement
                }
            }
            close(fd);
            if(!valid){++rejected;if(rejected==1||rejected%100==0)status("rejected");}
        }
        if(!stopping)app->post(this,200);
    }
};
Receiver* receiver=0;
bmw::IPluginApp* create(bmw::IDispatcher& d,const char* name,const bmw::Bundle& p,const bmw::Bundle& c) {
    if(receiver)return 0;
    bmw::PluginApp* app=new bmw::PluginApp(d,bmw::BasicString<char>(name),p,c);
    receiver=new Receiver(app);
    // Start on the page's focus event, after initialization/registration.
    // Factory-time delayed jobs did not execute in the first vehicle test.
    bmw::ActionDelegate handler={receiver,&Receiver::focus};
    app->addHandler(10,1,handler);
    return reinterpret_cast<bmw::IPluginApp*>(app);
}
void destroy(bmw::IPluginApp* p) {
    if(!receiver)return;
    receiver->stopping=true;
    delete reinterpret_cast<bmw::PluginApp*>(p); // native handler removes queued callbacks
    receiver->status("stopped");
    delete receiver;receiver=0;
}
}
struct PluginAppInfo {
    uint16_t major,minor;const char* name;const char* vendor;
    bmw::IPluginApp*(*create)(bmw::IDispatcher&,const char*,const bmw::Bundle&,const bmw::Bundle&);
    void(*destroy)(bmw::IPluginApp*);
};
typedef char InfoSize[(sizeof(PluginAppInfo)==20)?1:-1];
extern "C" { PluginAppInfo pluginAppInfo={1,0,"NbtLiveImage","NBT-Project",create,destroy}; }
