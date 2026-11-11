#include "nbt_image_abi.hpp"
#include <stdio.h>
namespace bmw {
class AbstractHandler;
class IDispatcher {
public:
    virtual ~IDispatcher() {}
    virtual void scheduleHandler(AbstractHandler&) = 0;
};
class PluginAppHandler {
public:
    PluginAppHandler(IDispatcher&);
    ~PluginAppHandler();
    void start();
    void post(Runnable*, unsigned);
    bool dispatch();
private:
    uint32_t opaque_[128];
};
}
struct Dispatcher : bmw::IDispatcher {
    unsigned calls;
    Dispatcher():calls(0){}
    void scheduleHandler(bmw::AbstractHandler&) { ++calls; }
};
struct Job : bmw::Runnable {
    unsigned calls;
    static unsigned destroyed;
    Job():calls(0){}
    ~Job(){++destroyed;}
    void run(){++calls;}
};
unsigned Job::destroyed=0;
int main() {
    Dispatcher dispatcher;
    for(unsigned i=0;i<1000;++i) {
        Job job;
        {
            bmw::PluginAppHandler handler(dispatcher);
            handler.start();
            handler.post(&job,0);
            handler.dispatch();
            if(job.calls!=1||Job::destroyed!=i)return 2;
            // Leave one callback queued; native handler destruction must not
            // invoke or delete our externally-owned Runnable.
            handler.post(&job,60000);
        }
        if(job.calls!=1||Job::destroyed!=i)return 3;
    }
    if(Job::destroyed!=1000||dispatcher.calls<1000)return 4;
    puts("PASS native post/dispatch: 1000 cycles; Runnable caller-owned; queued teardown safe");
    return 0;
}
