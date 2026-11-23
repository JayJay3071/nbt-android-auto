// libNbtAndroidAuto.so - the "Android Auto" entry under Multimedia (HMI plugin, in the installed package).
//
// The picture is not drawn by this page: nbt_hwvideo shows the hardware video window in front of the HMI (the very
// first version used a WAL sprite, see experiments/wal-overlay; some comments below still say so). This page
//   - opening it creates /fs/ram3/nbt-aa-request (native_service waits for it only with gate.mode, now opt-in: $P/gate-on)
//   - shows the connection status (from /fs/ram3/nbt-aa-state, written by native_service at least every 2 s):
//       no phone / phone found, starting / replug needed / device without Android Auto / several devices / service down
//   - brings Android Auto to the front as soon as it runs: iDrive is moved to the inert hold state 13 (focusEvent 52),
//     where controller turns / pushes / tilts do nothing in iDrive (nbt_knob forwards them to the phone), and the overlay
//     is shown (/fs/ram3/nbt-aa-hidden removed). Same idea as the NBT-AAD reference rhmiapp state 13 and iDrive's own
//     ST_CarPlay state.
//   - while Android Auto runs, the menu entry itself leads straight to the deepest hold state (2026-11-12): its action
//     takes the target from model 5 (raIntModel), which a poller sets to 20 or 10. Car 2026-11-12: after a while iDrive
//     ignored every focus event 52/54 (the entry page lost the focus, no hold state got it, until a reboot), so opening
//     the entry no longer depends on them. The poller starts with the first focus event of one of our pages (posts from
//     create() did not run on this HMI); until then the entry page and its focus event do the job as before.
//   - BACK (or a tilt iDrive treats as back) pops iDrive from 13 to the entry page 10: while Android Auto runs the press
//     belonged to the phone, so iDrive is put straight back on 13 (the presenter waits 300 ms before hiding: no blink).
//   - MENU / MEDIA / NAV / COM / MAP / OPTION leave the page the iDrive way: the overlay hides, the session keeps running
//     in the background. Multimedia -> Android Auto then brings the running session to the front at once.
//   - "Start at boot: on/off" (inert page 35): switches the flag file start-at-boot.off in the package; nbt_knob reads it
//     when the first session of a boot runs and then brings the picture to the front (show request) - or not.
//   - "Restart system" (page 33 asks, its button leads to the inert page 34): reboots the head unit with nbt_reset
//     (UDS 11 01 through the gateway), formerly the separate NbtReboot plugin ("Restart iDrive" under Multimedia).
// Hold states 13..20 are nested (see "hold states" below). Loop guard: at most 10 hold events per minute.
// ABI as in experiments/png-pipeline/dynamic_image_plugin.cpp (vehicle-tested PluginApp ctor, addHandler(state,1,...) focus events, post()).
// Checked in this car's libraries: PluginApp::triggerEvent(unsigned), updateModel(unsigned, TextID const&),
// TextID(unsigned) = ID(id, 0x28) (libCoreFramework.A.so); focusEvent/parentId are known to this HmiMain3D.
#include "nbt_image_abi.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <spawn.h>
#include <signal.h>
#include <pthread.h>
#include <new>
extern char** environ;
namespace bmw {
class IDispatcher; class Bundle; class IPluginApp;
// Parcel (libCoreFramework): 0x14 bytes {data, size, capacity, allocator, position} (Parcel() @0xbd46). Parcel(bool)
// is only used for setProperty below; those parcels are created once and kept (no exported destructor).
class Parcel { public: Parcel(bool); bool readBool(bool&) const; private: uint32_t storage_[5]; };
template<class R,class A1,class A2,class A3,class A4> struct Delegate {
    void* object;
    R (*stub)(void*,A1);
};
typedef Delegate<bool,const Parcel&,void,void,void> ActionDelegate;
typedef Delegate<bool,const BasicString<unsigned short>&,void,void,void> CdsDelegate;
class TextID {
public:
    explicit TextID(unsigned id);   // _ZN3bmw6TextIDC1Ej -> ID(id, 0x28): {u32 id, u32 type}, trivially destructible
private:
    uint32_t storage_[4];           // 8 bytes used, extra room is harmless
};
class PluginApp {
public:
    PluginApp(IDispatcher&,BasicString<char>,const Bundle&,const Bundle&);
    ~PluginApp();
    void post(Runnable*,unsigned);
    void triggerEvent(unsigned);
    void updateModel(unsigned,const TextID&);
    void updateModel(unsigned,int);   // _ZN3bmw9PluginApp11updateModelEji (in this car's libAppFramework.A.so)
    void addHandler(unsigned,unsigned,const ActionDelegate&);
    // this car's libAppFramework.A.so (2026-11-17): start() = state 0/4 -> 1 + PluginAppHandler::start() (the queue of
    // post()ed runnables is served from then on); the HMI calls it only when the page is first opened.
    bool start();
    void cdsAddPropertyChangedEventHandler(BasicString<char>&,unsigned,const CdsDelegate&);
    // _ZN3bmw9PluginApp11setPropertyEjjRKNS_6ParcelE: component, RHMI property id (1 enabled, 2 selectable, 3 visible,
    // as in BimmerGestalt's RHMIProperty), value
    void setProperty(unsigned,unsigned,const Parcel&);
private:
    uint32_t opaque_[1024];
};
}
namespace {
const char* const kRequest="/fs/ram3/nbt-aa-request";
const char* const kHidden="/fs/ram3/nbt-aa-hidden";
const char* const kState="/fs/ram3/nbt-aa-state";
const char* const kLeave="/fs/ram3/nbt-aa-leave";
// BACK (car 2026-11-17): iDrive's BACK goes back in iDrive's history, also from the deepest hold level (log: "hold focus
// 0, level 7" -> "page left", never level 6), so it left Android Auto while the phone went back as well. nbt_knob writes
// this flag on the driver's BACK while the picture is on screen; when the page is left within 2 s of it, the Checker
// brings Android Auto back with one hold event (hold(): 1 per s, 10 per minute).
const char* const kBackKey="/fs/ram3/nbt-aa-back";
const char* const kGuard="/fs/ram3/nbt-aa-guard";   // nbt_knob's BACK guard (tilt right on the driver's BACK)
const unsigned kEntryState=10, kHoldState=13, kEvHold=52, kFocus=1, kVisible=11, kStatusModel=4;
// picture mode selection (2026-11-08): buttons on page 10 lead to the inert states 30 (16:9) / 31 (widescreen); their
// focus handler stores the mode, restarts the service and sends focus event 53 back to page 10
const unsigned kMode169State=30, kModeWideState=31, kEvEntry=53, kModel169=6, kModelWide=7;
const unsigned kRestartState=32;                     // "Android Auto neu starten" -> inert page 32, back to 10
const unsigned kBootState=35, kBootModel=91;         // "Start at boot" -> inert page 35, back to 10
const char* const kBootOff="/var/opt/hmi/data/rw/NbtAndroidAutoRT/start-at-boot.off";
bool bootOn() { return access(kBootOff,F_OK)!=0; }   // default: on (no file)
// "Show widget at startup" (user 2026-11-23, was "Trip computer"): only in full width (the box is not shown in split screen anyway; the
// button is made invisible there, RHMI property 3). On: panel-at-start exists in the package, and nbt_decoder shows the
// box at every session start (off: hidden until long NAV). Default: off (no file).
const unsigned kPanelState=36, kPanelModel=97, kPanelButton=96, kPropVisible=3;
const char* const kPanelAtStart="/var/opt/hmi/data/rw/NbtAndroidAutoRT/panel-at-start";
bool panelOn() { return access(kPanelAtStart,F_OK)==0; }
// "Decoder: Software / Hardware" (user 2026-11-23, default Software): decoder.hw in the package. nbt_decoder runs for the
// whole service lifetime, so the switch restarts the Android Auto service (as the picture mode does): boot.sh copies
// the flag and native_service starts nbt_decoder with or without --hw; the phone reconnects to the new decoder.
const unsigned kDecoderState=37, kDecoderModel=103;
const char* const kDecoderHw="/var/opt/hmi/data/rw/NbtAndroidAutoRT/decoder.hw";
bool decoderHw() { return access(kDecoderHw,F_OK)==0; }
// "Widget size: Full width / Small / Trip only / Car only" (user 2026-11-23): full width only, like "Show widget at
// startup". decoder.panelsize in the package: "full" = the full box (384 px), none (default) or "small" = 256 px with
// the pages Trip and Car (long NAV: page 1, page 2, off), "trip" / "car" = 256 px with only that page (long NAV: on,
// off). Each press takes the next one; nbt_decoder reads it when a session starts, nbt_knob at each long NAV.
const unsigned kSizeState=38, kSizeModel=108, kSizeButton=107;
const char* const kPanelSize="/var/opt/hmi/data/rw/NbtAndroidAutoRT/decoder.panelsize";
int panelSize() {   // 0 full width, 1 small, 2 trip only, 3 car only
    char b[16]={0};
    const int fd=open(kPanelSize,O_RDONLY);
    if(fd<0)return 1;                                  // default: small
    const int n=read(fd,b,sizeof(b)-1);
    close(fd);
    if(n<=0)return 1;
    return !strncmp(b,"full",4)?0:!strncmp(b,"trip",4)?2:!strncmp(b,"car",3)?3:1;
}
void setPanelSize(int v) {
    static const char* const name[4]={"full\n","","trip\n","car\n"};
    if(v<0||v>3||v==1){unlink(kPanelSize);return;}     // small: the default, no file
    const int fd=open(kPanelSize,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0){write(fd,name[v],strlen(name[v]));close(fd);}
}
const unsigned kEvHoldAlt=54;                        // focus event to hold state 13 (fallback when 52 is ignored)
// The real reason iDrive "ignored" 52 / 54 (car 2026-11-18, HmiMain3D CFocusEvent::perform @0x88fd70b): every focus
// event object counts its performs and refuses from the 12th on ("FocusEvent counter > 10"; never reset, only apps with
// the RHMI permission rhmi.unlimitedfocusevents are exempt). So the XML has a pool of 500 focus events (200..699, all
// to hold state 13); hold() uses each 10 times, then the next, and moves on at once after an ignored one.
const unsigned kEvPoolFirst=200, kEvPoolCount=500, kEvPoolUses=10;   // 500 (user 2026-11-18: long drives), was 100
// text ids in NbtAndroidAuto_ALL_Texts.zip (make_aa_entry_resources.py)
enum { kTxtConnectUsb=2, kTxtNoPhone=3, kTxtStarting=4, kTxtRunning=5, kTxtReplug=6, kTxtUnsupported=7,
       kTxtServiceDown=8, kTxtSeveral=9, kTxt169=11, kTxtWide=12, kTxt169On=16, kTxtWideOn=17,
       kTxtBootOn=31, kTxtBootOff=32, kTxtPanelOn=35, kTxtPanelOff=36,
       kTxtDecoderSw=38, kTxtDecoderHw=39, kTxtSizeFull=41 /* 41..44: full width, small, trip only, car only */,
       kTxtReboot=46 };
void touch(const char* path) { int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0)close(fd); }
// --- automatic start of the Android Auto service (boot.sh of the persistent package), like the NBT-AAD reference:
// QNX spawn() into a new process group, no zombie, stdin/out/err /dev/null. Rate-limited to one start per 30 s.
const char* const kBoot="/var/opt/hmi/data/rw/NbtAndroidAutoRT/boot.sh";
long g_lastLaunch=0;
long mono_s() { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (long)t.tv_sec; }
void launchService() {
    long t=mono_s();
    if(g_lastLaunch&&t-g_lastLaunch<30)return;
    g_lastLaunch=t;
    if(access(kBoot,R_OK)!=0)return;
    int nul=open("/dev/null",O_RDWR);
    if(nul<0)return;
    int fdmap[3]={nul,nul,nul};
    struct inheritance inh;
    memset(&inh,0,sizeof(inh));
    inh.flags=SPAWN_SETGROUP|SPAWN_SETSIGDEF|SPAWN_SETSIGMASK|SPAWN_NOZOMBIE;
    inh.pgroup=SPAWN_NEWPGROUP;
    sigfillset(&inh.sigdefault);
    sigemptyset(&inh.sigmask);
    char* argv[]={(char*)"/bin/sh",(char*)kBoot,0};
    char* envp[]={(char*)"PATH=/bin:/usr/bin:/sbin:/usr/sbin:/opt/sys/bin",0};
    spawn("/bin/sh",3,fdmap,&inh,argv,envp);
    close(nul);
}
// --- picture mode: flag file in the persistent package (read by boot.sh). split800 = "16:9" (1:1 at the left, iDrive
// split screen on the right), anamorph = "widescreen" (default when no flag exists). The service is restarted so the
// new mode applies; the phone negotiates its video format at the next connection.
const char* const kPkg="/var/opt/hmi/data/rw/NbtAndroidAutoRT";
bool modeIs169() { return access("/var/opt/hmi/data/rw/NbtAndroidAutoRT/split800",F_OK)==0; }
long g_lastMode=0;
// set when page 10 got the focus in this boot: the mode / restart buttons sit on it, so a real press always comes after
// it, while a mode page the HMI restores at boot does not (replaces the former "nothing in the first 60 s" guard)
bool g_entrySeen=false;
void plog(const char* what,int a);
// restart of the Android Auto service (not the head unit): stop native_service, wait for its lock to go, run boot.sh.
// flag = new picture mode flag file, or 0 to keep the mode (restart button, user request 2026-11-08: recovery when
// Android Auto shows "connected" but the picture does not come)
void restartService(const char* flag) {
    long t=mono_s();
    if(!g_entrySeen){plog("restart ignored: page 10 not opened in this boot (restored state)",0);return;}
    if(g_lastMode&&t-g_lastMode<10){plog("restart ignored: less than 10 s after the last one",0);return;}
    g_lastMode=t;
    int nul=open("/dev/null",O_RDWR);
    if(nul<0)return;
    int fdmap[3]={nul,nul,nul};
    struct inheritance inh;
    memset(&inh,0,sizeof(inh));
    inh.flags=SPAWN_SETGROUP|SPAWN_SETSIGDEF|SPAWN_SETSIGMASK|SPAWN_NOZOMBIE;
    inh.pgroup=SPAWN_NEWPGROUP;
    sigfillset(&inh.sigdefault);
    sigemptyset(&inh.sigmask);
    char cmd[512];
    char modeCmd[96]="";
    if(flag)snprintf(modeCmd,sizeof(modeCmd),"rm -f anamorph center800 split800 classic; touch %s; ",flag);
    snprintf(cmd,sizeof(cmd),"cd %s || exit 1; %s"
             "slay -f -Q -s TERM native_service; i=0; while test -e /fs/ram3/nbt-aa-native/service.lock -a $i -lt 20; "
             "do sleep 1; i=$((i+1)); done; slay -f -Q -s KILL native_service; rm -f /fs/ram3/nbt-aa-native/service.lock; "
             "sh %s",kPkg,modeCmd,kBoot);
    char* argv[]={(char*)"/bin/sh",(char*)"-c",cmd,0};
    // restart.sh of the package (2026-11-09) does the same after a diagnostic snapshot; the inline command is the fallback
    const char* const script="/var/opt/hmi/data/rw/NbtAndroidAutoRT/restart.sh";
    char* argvScript[]={(char*)"/bin/sh",(char*)script,(char*)(flag?flag:""),0};
    char* envp[]={(char*)"PATH=/bin:/usr/bin:/sbin:/usr/sbin:/opt/sys/bin",0};
    spawn("/bin/sh",3,fdmap,&inh,access(script,R_OK)==0?argvScript:argv,envp);
    close(nul);
}
void setMode(bool split) { restartService(split?"split800":"anamorph"); }
// started once from create(): factory-time posts did not run on this HMI (2026-11-06), so a detached thread waits
// no delay (user 2026-11-12): boot.sh is started as soon as the HMI has created the plugin
void* autostartThread(void*) { launchService(); return 0; }
long now_s() { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (long)t.tv_sec; }
long long now_ms() { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (long long)t.tv_sec*1000+t.tv_nsec/1000000; }
// small diagnostic log (car 2026-11-07: after a while MENU no longer left Android Auto); bounded to 64 KB
void plog(const char* what,int a=0) {
    const char* const path="/fs/ram3/nbt-aa-plugin.log";
    struct stat st;
    int flags=O_WRONLY|O_CREAT|O_APPEND;
    if(stat(path,&st)==0&&st.st_size>65536)flags=O_WRONLY|O_CREAT|O_TRUNC;
    int fd=open(path,flags,0644);
    if(fd<0)return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    char line[96]; int n=snprintf(line,sizeof(line),"%ld.%03ld %s %d\n",(long)t.tv_sec,t.tv_nsec/1000000,what,a);
    if(n>0)write(fd,line,(size_t)n);
    close(fd);
}
// the status text for native_service's state file
static unsigned statusText(const char* buf, bool* running) {
    const bool phone=strstr(buf,"phone=1")!=0;
    if(strstr(buf,"state=running")){*running=true;return kTxtRunning;}
    if(strstr(buf,"state=connecting")||strstr(buf,"state=starting_decoder")||strstr(buf,"state=draining")) return kTxtStarting;
    if(strstr(buf,"state=replug_required")) return kTxtReplug;
    if(strstr(buf,"state=unsupported_device")) return kTxtUnsupported;
    if(strstr(buf,"state=several_devices")) return kTxtSeveral;
    if(strstr(buf,"state=waiting_for_hmi")) return phone?kTxtStarting:kTxtNoPhone;   // request just written
    if(strstr(buf,"state=waiting_for_phone")) return kTxtNoPhone;
    return kTxtConnectUsb;
}
// reads native_service's public status; returns the text to show, *running = Android Auto session active
unsigned readStatus(bool* running) {
    *running=false;
    struct stat st;
    if(stat(kState,&st)!=0||time(0)-st.st_mtime>8) return kTxtServiceDown;   // rewritten every 2 s while alive
    char buf[160]; int fd=open(kState,O_RDONLY); if(fd<0) return kTxtServiceDown;
    ssize_t n=read(fd,buf,sizeof(buf)-1); close(fd); if(n<=0) return kTxtServiceDown; buf[n]=0;
    const unsigned text=statusText(buf,running);
    // 2026-11-24: a setting change started nbt_decoder again, its picture comes back only after a reboot ("Restart
    // system" below on the same page)
    return strstr(buf,"reboot=1")?kTxtReboot:text;
}
class Entry;
// --- hold states (car 2026-11-07, second design) ---------------------------------------------------------------------
// Problem: every tilt LEFT acted as BACK and every tilt RIGHT opened the split screen; each was undone with a focus event
// (triggerEvent 52). After a few dozen of them the HMI stopped honouring the event (picture gone until a reboot).
// Now: kHoldCount nested inert states (13 .. 13+N-1, each the parent of the next, focus event 52 targets the deepest).
// 2026-11-17: one hold state only - BACK goes back in iDrive's history (it never climbed the levels) and tilt left is
// kept on the hold state by nbt_knob's guards (tilt right, then BACK). If a guard misses, the entry page is reached and
// one hold event brings Android Auto back (below).
// Only when the entry page (10) is reached
// one event goes out. Side panels / popups: at most one re-hold per 1.5 s. Never more than 10 events per minute, and no
// periodic re-hold any more. The split screen itself is best switched off in iDrive (OPTION -> split screen).
const unsigned kHoldCount=1;   // 2026-11-17: one hold state (was 8 nested ones; nbt_knob's BACK / tilt guards)
// Decides, shortly after a hold state lost focus, what happened:
//   another hold state has the focus (BACK climbed a level)         -> nothing
//   a hard key was pressed (flag from nbt_knob)                      -> hide
//   a hold state is still visible (side panel / popup)              -> one re-hold (rate-limited), else wait
//   entry page focused (all hold levels used up)                    -> one re-hold
//   else (MENU, MEDIA, ... left the page)                           -> hide
class Checker : public bmw::Runnable {
public:
    Entry* e; bool pending;
    Checker():e(0),pending(false) {}
    void run();
};
// Safety net: 2.5 s after a re-hold attempt a hold state must have the focus; otherwise the picture is hidden, so turns
// and presses never drive an invisible iDrive page underneath.
class Verifier : public bmw::Runnable {
public:
    Entry* e; bool pending;
    Verifier():e(0),pending(false) {}
    void run();
};
struct HoldSlot { Entry* e; unsigned idx; };
// sends focus event 53 (back to page 10) after a mode choice
class BackToEntry : public bmw::Runnable {
public:
    bmw::PluginApp* app;
    BackToEntry():app(0) {}
    void run() { if(app){plog("mode chosen -> back to page 10");app->triggerEvent(kEvEntry);} }
};
// Show request (2026-11-17, to replace favourite 8): a program creates /fs/ram3/nbt-aa-show; the poll below removes it
// and, while Android Auto runs, sends the hold focus event (52/54, through hold(): rate limit and loop guard apply) to
// bring the picture to the front from wherever iDrive is. Open: whether iDrive honours the focus event while another
// application is in front: car 2026-11-17, yes - brought to the front at once. The poll starts 5 s after the plugin is
// created (so it also serves "Start at boot", before the page was ever opened) and again from the focus handlers.
const char* const kShowRequest="/fs/ram3/nbt-aa-show";
// Early start (2026-11-17): car log of 2026-11-17 - nothing post()ed ran before the page was first opened (the HMI calls
// PluginApp::start() only then), so the show request did nothing at boot. create() now calls start() itself (a later
// start() from the HMI finds state 1 and just returns true; init()/onInit still come from the HMI as before).
// Car data: CDS properties driving.gear and sensors.PDCStatus (names from NBT-AAD's EVO app), subscribed 3 s after
// create; every change goes to the plugin log and to /fs/ram3/nbt-aa-gear / nbt-aa-pdc. Car 2026-11-17: both exist on
// this NBT; '{"PDCStatus":1}' = parking assistant on screen, 2 = off; gear read '{"gear":15}' in P.
// Back after parking (user 2026-11-17: after the parking assistant iDrive returned to its last screen, not to Android
// Auto): when PDCStatus becomes 1 while the picture is on screen (no nbt-aa-hidden yet: the PDC value comes ~150 ms
// before the page loses focus), its end (2) creates the show request, so Android Auto comes back.
void cdsText(const bmw::BasicString<unsigned short>& v,char* out,size_t size) {
    const unsigned short* p=*reinterpret_cast<const unsigned short* const*>(&v);   // data_ = the UTF-16 text itself
    size_t n=0;
    while(p&&p[n]&&n<size-1){out[n]=p[n]<0x80?(char)p[n]:'?';++n;}
    out[n]=0;
}
void cdsLog(const char* name,const char* file,const bmw::BasicString<unsigned short>& v) {
    char text[160];cdsText(v,text,sizeof(text));
    char line[200];snprintf(line,sizeof(line),"CDS %s = '%s'",name,text);
    plog(line,0);
    int fd=open(file,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0){size_t l=strlen(text);text[l]='\n';write(fd,text,l+1);close(fd);}
}
bool onGear(void*,const bmw::BasicString<unsigned short>& v){cdsLog("driving.gear","/fs/ram3/nbt-aa-gear",v);return true;}
// Night mode (user 2026-11-17: Waze stayed white at night): the core advertises the NIGHT_MODE sensor but nothing fed it,
// so the phone always got day. NBT-AAD's vehicle.dayNightChangeOver answers '{"error":400}' on this NBT: HmiMain3D's CDS
// table has no day/night property (car 2026-11-17). controls.headlights ('{"headlights":%d}') is used instead and goes
// to /fs/ram3/nbt-aa-headlights, read by the session: headlights on = night.
bool onHeadlights(void*,const bmw::BasicString<unsigned short>& v){cdsLog("controls.headlights","/fs/ram3/nbt-aa-headlights",v);return true;}
// Parking assistant over Android Auto (car 2026-11-19, decoder mode):
// - on: the Checker sometimes saw the hold page still visible under it and took the focus back, so the picture covered
//   the parking assistant. Now the picture is hidden at once and nothing is held while it is on.
// - "was Android Auto on screen": iDrive took the focus from our page ~0.9 s BEFORE PDCStatus became 1, so the hidden flag
//   alone said no. Our page losing the focus with the picture shown less than 3 s before counts too.
// - off with its button: iDrive stays on its inactive parking screen (on other pages it returns) and ignores our focus
//   events there (log: 5 events, none taken); the driver's NAV there brings Android Auto back. So nbt_knob is asked for
//   one NAV (/fs/ram3/nbt-aa-hmi-nav), handled like the driver's: show request, and the Checker takes the focus back
//   from iDrive's navigation.
bool g_pdcFromAa=false;       // the parking assistant came up over the Android Auto picture
bool g_pdcActive=false;       // parking assistant on: never hold, picture hidden
long long g_pdcOffMs=-100000; // when it last went off over Android Auto
long long g_aaLeftMs=-100000; // our page last lost the focus with the picture shown
bool onPdc(void*,const bmw::BasicString<unsigned short>& v){
    cdsLog("sensors.PDCStatus","/fs/ram3/nbt-aa-pdc",v);
    char text[160];cdsText(v,text,sizeof(text));
    const char* d=text;while(*d&&(*d<'0'||*d>'9'))++d;
    const int st=*d?atoi(d):-1;
    if(st==1){
        if(!g_pdcActive){
            g_pdcFromAa=access(kHidden,F_OK)!=0||now_ms()-g_aaLeftMs<3000||now_ms()-g_pdcOffMs<3000;
            plog("parking assistant on, Android Auto was on screen",g_pdcFromAa?1:0);
        }
        g_pdcActive=true;
        touch(kHidden);
    }
    else if(st==2){
        g_pdcActive=false;
        if(g_pdcFromAa){
            g_pdcFromAa=false;
            g_pdcOffMs=now_ms();
            unlink(kLeave);
            touch("/fs/ram3/nbt-aa-hmi-nav");                // nbt_knob: NAV, as the driver would press it
            plog("parking assistant off: NAV -> back to Android Auto",0);
        }
    }
    return true;
}
// iDrive's call screens (user 2026-11-20: during calls iDrive takes the screen from Android Auto; NAV brought it back).
// A call that starts (connection 090 in the 100E table; nbt_knob: /fs/ram3/nbt-aa-call while it is up) with Android
// Auto on screen, or left less than 3 s before (the call screen can come first), is "over Android Auto" until 6 s after
// it ends. Meanwhile our page losing the focus for 1 s without an iDrive key is one of iDrive's call screens (car
// 2026-11-20: one when the call is made, another ~8 s after the other party answered, which the Checker's hold did not
// get back). iDrive ignores our focus events while its call screen is up, but the driver's NAV gets Android Auto back
// (the key leaves the call screen for iDrive's navigation, which the Checker takes back after the show request). So the
// same: nbt_knob sends one NAV to iDrive (/fs/ram3/nbt-aa-hmi-nav, as after the parking assistant), the show request
// follows 250 ms later; at most every 3 s. An iDrive key (the driver chose iDrive) ends it for this call.
// communication.currentCallInfo (CDS) is only logged (/fs/ram3/nbt-aa-callinfo): it did not change during a call.
const char* const kCall="/fs/ram3/nbt-aa-call";
long long g_callStartMs=0,g_callEndMs=0,g_callShowMs=-100000,g_driverLeftMs=-100000,g_callShowAtMs=0,g_lostSinceMs=0;
bool g_callUp=false,g_callOverAa=false;
bool onCallInfo(void*,const bmw::BasicString<unsigned short>& v){
    cdsLog("communication.currentCallInfo","/fs/ram3/nbt-aa-callinfo",v);
    return true;
}
// Trip computer box (user 2026-11-23: a box over Android Auto instead of iDrive's split screen; nbt_decoder draws it).
// CDS properties as BMW's apps read them (names from BimmerGestalt's IDriveConnectKit, all in this HmiMain3D's CDS
// table). Car 2026-11-23: driving.averageConsumption {"averageConsumption":{"averageConsumption1":6.7,
// "averageConsumption2":7.4,"unit":1}} (2 = the trip computer's), driving.averageSpeed (the same with averageSpeed1/2),
// sensors.fuel {"fuel":{"range":833,"reserve":0,"tanklevel":52}}, sensors.temperatureExterior {"temperatureExterior":
// 11.5}, engine.temperature {"temperature":{"engine":80,"oil":73}}, engine.consumption {"consumption":0}.
// All latest values go to /fs/ram3/nbt-aa-trip, one line "name json" each, rewritten on every change (RAM); only the
// first value of each property goes to the plugin log. Trip start, duration and distance are not in CDS: nbt_knob
// reads them from the cluster (/fs/ram3/nbt-aa-tripkombi).
// Car 2026-11-23: engine.consumption is a counter (rises at idle, wraps at 65536), not a rate: nbt_decoder turns it
// into L/h, and with driving.speedActual into L/100 km.
const char* const kTripNames[]={"driving.averageConsumption","driving.averageSpeed","sensors.fuel",
    "sensors.temperatureExterior","engine.temperature","engine.consumption","driving.speedActual",
    "sensors.battery"};
const int kTrips=sizeof(kTripNames)/sizeof(kTripNames[0]);
char g_trip[kTrips][160];
bool g_tripLogged[kTrips];
template<int I> bool onTrip(void*,const bmw::BasicString<unsigned short>& v){
    cdsText(v,g_trip[I],sizeof(g_trip[I]));
    if(!g_tripLogged[I]){
        g_tripLogged[I]=true;
        char line[220];snprintf(line,sizeof(line),"CDS %s = '%s' (first value)",kTripNames[I],g_trip[I]);
        plog(line,0);
    }
    char buf[kTrips*200];size_t n=0;
    for(int i=0;i<kTrips;++i)
        if(g_trip[i][0])n+=snprintf(buf+n,sizeof(buf)-n,"%s %s\n",kTripNames[i],g_trip[i]);
    int fd=open("/fs/ram3/nbt-aa-trip.new",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0){write(fd,buf,n);close(fd);rename("/fs/ram3/nbt-aa-trip.new","/fs/ram3/nbt-aa-trip");}
    return true;
}
bool (*const kTripFns[kTrips])(void*,const bmw::BasicString<unsigned short>&)={&onTrip<0>,&onTrip<1>,&onTrip<2>,
    &onTrip<3>,&onTrip<4>,&onTrip<5>,&onTrip<6>,&onTrip<7>};
class CdsSubscribe : public bmw::Runnable {
public:
    bmw::PluginApp* app;
    CdsSubscribe():app(0) {}
    void run() {
        if(!app)return;
        bmw::BasicString<char> g("driving.gear"),p("sensors.PDCStatus"),d("controls.headlights"),c("communication.currentCallInfo");
        bmw::CdsDelegate dg={app,&onGear},dp={app,&onPdc},dd={app,&onHeadlights},dc={app,&onCallInfo};
        app->cdsAddPropertyChangedEventHandler(g,0,dg);
        app->cdsAddPropertyChangedEventHandler(p,0,dp);
        app->cdsAddPropertyChangedEventHandler(d,0,dd);
        app->cdsAddPropertyChangedEventHandler(c,0,dc);
        for(int i=0;i<kTrips;++i){
            bmw::BasicString<char> t(kTripNames[i]);
            bmw::CdsDelegate dt={app,kTripFns[i]};
            app->cdsAddPropertyChangedEventHandler(t,0,dt);
        }
        plog("CDS handlers for driving.gear, sensors.PDCStatus, controls.headlights, communication.currentCallInfo and the trip computer registered",0);
    }
};
CdsSubscribe g_cds;
// HMI thread, every 250 ms once started: the entry's target (model 5) follows the service state; show requests
const unsigned kTargetModel=5;
class TargetPoll : public bmw::Runnable {
public:
    bmw::PluginApp* app; Entry* e; bool active; int target;
    TargetPoll():app(0),e(0),active(false),target(10) {}
    void start() { if(!active&&app){active=true;app->post(this,0);} }
    void run();
};
class Entry : public bmw::Runnable {
public:
    bmw::PluginApp* app;
    bool stopping, polling, entryFocused, running;
    unsigned focusMask, visibleMask;
    unsigned shownText; int shownMode, shownBoot, shownPanel, shownPanelVis, shownDecoder, shownSize; long long lastBootToggleMs, lastPanelToggleMs, lastSizeToggleMs; long window; unsigned holds, ignoredHolds; long lastSideHold, lastAutoHold; long long lastSideHoldMs, lastHoldMs, lastFocusMs;
    Checker checker;
    Verifier verifier;
    BackToEntry back;
    TargetPoll target;
    HoldSlot slots[kHoldCount];
    Entry():app(0),stopping(false),polling(false),entryFocused(false),running(false),focusMask(0),visibleMask(0),
            shownText(0),shownMode(0),shownBoot(0),shownPanel(0),shownPanelVis(-1),shownDecoder(0),shownSize(0),lastBootToggleMs(-100000),lastPanelToggleMs(-100000),lastSizeToggleMs(-100000),window(0),holds(0),ignoredHolds(0),lastSideHold(0),lastAutoHold(0),lastSideHoldMs(-100000),lastHoldMs(-100000),lastFocusMs(0),showUntilMs(0),showRetryAtMs(0),evIdx(kEvPoolCount-1),evUses(kEvPoolUses),evIgnoredSeen(0) {
        checker.e=this; verifier.e=this; target.e=this;
        for(unsigned i=0;i<kHoldCount;++i){slots[i].e=this;slots[i].idx=i;}
    }
    bool holdFocused() const { return focusMask!=0; }
    bool holdVisible() const { return visibleMask!=0; }
    void check(unsigned delay) { if(!checker.pending&&!stopping&&app){checker.pending=true;app->post(&checker,delay);} }
    // 5 s (car 2026-11-08: with the focus in the split screen iDrive gave it back only after ~4.4 s; 2.5 s hid the picture
    // in between)
    void verify() { if(!verifier.pending&&!stopping&&app){verifier.pending=true;app->post(&verifier,5000);} }
    // counted=false: a show request and its one follow-up (NAV key, parking assistant, AUX): one per driver action, so
    // they do not use up the loop guard (car 2026-11-18: a few NAV presses -> "refused by loop guard" for a minute)
    void hold(const char* why,bool counted=true) {
        // review 2026-11-09: on every entry with Android Auto running the checker (+30 ms) and the status poll (+50 ms)
        // each sent the event, 50 ms apart and in the middle of iDrive's page transition; in the log of 2026-11-09 the
        // first ignored event was exactly such a pair. One event per second at most.
        if(g_pdcActive){plog("hold skipped: parking assistant on");touch(kHidden);return;}
        const long long tm=now_ms();
        if(tm-lastHoldMs<1000){plog("hold skipped (one was just sent)");return;}
        lastHoldMs=tm;lastAutoHold=now_s();
        long t=now_s();
        if(t-window>=60){window=t;holds=0;}
        if(counted&&++holds>10){plog("hold refused by loop guard (10/min)");touch(kHidden);return;}
        // car 2026-11-09: iDrive sometimes ignores 52 for the rest of the drive (entry page shown, picture hidden): after
        // an ignored attempt the next one uses 54 (to hold state 13), then 52 again, ...
        if(ignoredHolds!=evIgnoredSeen){evIgnoredSeen=ignoredHolds;evUses=kEvPoolUses;}   // ignored: next event
        if(evUses>=kEvPoolUses){evIdx=(evIdx+1)%kEvPoolCount;evUses=0;}
        ++evUses;
        const unsigned ev=kEvPoolFirst+evIdx;
        plog(why,(int)holds);
        plog("  focus event",(int)ev);
        plog("  ms since the last focus change",(int)(tm-lastFocusMs>100000?100000:tm-lastFocusMs));
        app->triggerEvent(ev);
        verify();
    }
    // HMI thread, every 500 ms while our page has focus: status text; a slow retry (every 10 s) when the entry page shows
    // while Android Auto runs and the last hold attempt did not take
    void run() {
        if(stopping||!app){polling=false;return;}
        if(!entryFocused&&!holdFocused()){polling=false;return;}
        unsigned text=readStatus(&running);
        if(text!=shownText){bmw::TextID tx(text);app->updateModel(kStatusModel,tx);shownText=text;}
        int mode=modeIs169()?1:2;
        if(mode!=shownMode){
            bmw::TextID a(mode==1?kTxt169On:kTxt169),b(mode==2?kTxtWideOn:kTxtWide);
            app->updateModel(kModel169,a);app->updateModel(kModelWide,b);shownMode=mode;
        }
        int boot=bootOn()?1:2;
        if(boot!=shownBoot){bmw::TextID tb(boot==1?kTxtBootOn:kTxtBootOff);app->updateModel(kBootModel,tb);shownBoot=boot;}
        int dec=decoderHw()?1:2;
        if(dec!=shownDecoder){bmw::TextID td(dec==1?kTxtDecoderHw:kTxtDecoderSw);app->updateModel(kDecoderModel,td);shownDecoder=dec;}
        int panel=panelOn()?1:2;
        if(panel!=shownPanel){bmw::TextID tp(panel==1?kTxtPanelOn:kTxtPanelOff);app->updateModel(kPanelModel,tp);shownPanel=panel;}
        int size=panelSize()+1;
        if(size!=shownSize){bmw::TextID tz(kTxtSizeFull+size-1);app->updateModel(kSizeModel,tz);shownSize=size;}
        const int vis=mode==2?1:0;   // only in full width
        if(vis!=shownPanelVis){
            static bmw::Parcel* yes=new bmw::Parcel(true);
            static bmw::Parcel* no=new bmw::Parcel(false);
            app->setProperty(kPanelButton,kPropVisible,vis?*yes:*no);
            app->setProperty(kSizeButton,kPropVisible,vis?*yes:*no);
            shownPanelVis=vis;
            plog("widget buttons visible",vis);
        }
        long t=now_s();
        if(running&&entryFocused&&!holdFocused()&&t-lastAutoHold>=10){lastAutoHold=t;hold("hold (entry page, retry)");}
        app->post(this,500);
    }
    void startPolling(unsigned delay) { if(!polling&&!stopping){polling=true;app->post(this,delay);} }
    static bool entryFocus(void* self,const bmw::Parcel& p) {
        Entry* e=static_cast<Entry*>(self);
        bool focused=false;p.readBool(focused);
        e->entryFocused=focused;e->lastFocusMs=now_ms();
        e->target.start();
        plog("entry focus",focused);
        if(focused)g_entrySeen=true;
        if(focused&&!e->stopping){
            unlink(kLeave);                    // the driver came back on purpose
            touch(kRequest);
            if(readStatus(&e->running)==kTxtServiceDown)launchService();   // service not running: start it now
            e->startPolling(50);if(e->running)e->check(30);
        }
        return true;
    }
    // mode pages 30 / 31: store the mode, restart the service, back to page 10
    static bool mode169Focus(void* self,const bmw::Parcel& p) { return modeFocus(static_cast<Entry*>(self),p,true); }
    static bool modeWideFocus(void* self,const bmw::Parcel& p) { return modeFocus(static_cast<Entry*>(self),p,false); }
    static bool modeFocus(Entry* e,const bmw::Parcel& p,bool split) {
        bool focused=false;p.readBool(focused);
        plog(split?"mode page split screen focus":"mode page full width focus",focused);
        if(focused&&!e->stopping){setMode(split);e->shownMode=0;e->app->post(&e->back,400);}
        return true;
    }
    // page 37: switch the decoder (Software / Hardware), restart the service, back to page 10 (guards as restartService)
    static bool decoderFocus(void* self,const bmw::Parcel& p) {
        Entry* e=static_cast<Entry*>(self);
        bool focused=false;p.readBool(focused);
        plog("decoder page focus",focused);
        if(!focused||e->stopping)return true;
        if(!g_entrySeen)plog("decoder ignored: page 10 not opened in this boot (restored state)",0);
        else if(g_lastMode&&now_s()-g_lastMode<10)plog("decoder ignored: less than 10 s after the last restart",0);
        else{
            if(decoderHw())unlink(kDecoderHw); else touch(kDecoderHw);
            plog("decoder hardware now",decoderHw()?1:0);
            restartService(0);
            e->holds=0;e->window=now_s();e->lastSideHoldMs=-100000;
        }
        e->shownDecoder=0;e->app->post(&e->back,400);
        return true;
    }
    // page 36: switch "Trip computer at start", back to page 10 (as page 35)
    static bool panelFocus(void* self,const bmw::Parcel& p) {
        Entry* e=static_cast<Entry*>(self);
        bool focused=false;p.readBool(focused);
        plog("trip computer page focus",focused);
        if(!focused||e->stopping)return true;
        const long long tm=now_ms();
        if(!g_entrySeen)plog("trip computer ignored: page 10 not opened in this boot (restored state)",0);
        else if(tm-e->lastPanelToggleMs<2000)plog("trip computer ignored: switched just before",0);
        else{
            e->lastPanelToggleMs=tm;
            if(panelOn())unlink(kPanelAtStart); else touch(kPanelAtStart);
            plog("trip computer at start now",panelOn()?1:0);
        }
        e->shownPanel=0;e->app->post(&e->back,400);
        return true;
    }
    // page 38: the next widget size, back to page 10 (as page 36)
    static bool sizeFocus(void* self,const bmw::Parcel& p) {
        Entry* e=static_cast<Entry*>(self);
        bool focused=false;p.readBool(focused);
        plog("widget size page focus",focused);
        if(!focused||e->stopping)return true;
        const long long tm=now_ms();
        if(!g_entrySeen)plog("widget size ignored: page 10 not opened in this boot (restored state)",0);
        else if(tm-e->lastSizeToggleMs<2000)plog("widget size ignored: switched just before",0);
        else{
            e->lastSizeToggleMs=tm;
            setPanelSize((panelSize()+1)%4);
            plog("widget size now",panelSize());
        }
        e->shownSize=0;e->app->post(&e->back,400);
        return true;
    }
    // page 35: switch "Start at boot", back to page 10 (not for a page the HMI restores at boot, not twice within 2 s)
    static bool bootFocus(void* self,const bmw::Parcel& p) {
        Entry* e=static_cast<Entry*>(self);
        bool focused=false;p.readBool(focused);
        plog("start at boot page focus",focused);
        if(!focused||e->stopping)return true;
        const long long tm=now_ms();
        if(!g_entrySeen)plog("start at boot ignored: page 10 not opened in this boot (restored state)",0);
        else if(tm-e->lastBootToggleMs<2000)plog("start at boot ignored: switched just before",0);
        else{
            e->lastBootToggleMs=tm;
            if(bootOn())touch(kBootOff); else unlink(kBootOff);
            plog("start at boot now",bootOn()?1:0);
        }
        e->shownBoot=0;e->app->post(&e->back,400);
        return true;
    }
    static bool restartFocus(void* self,const bmw::Parcel& p) {
        Entry* e=static_cast<Entry*>(self);
        bool focused=false;p.readBool(focused);
        plog("restart page focus",focused);
        if(focused&&!e->stopping){
            restartService(0);
            e->holds=0;e->window=now_s();e->lastSideHoldMs=-100000;   // fresh loop guard for the new session
            e->app->post(&e->back,400);
        }
        return true;
    }
    long long showUntilMs;   // a show request was handled: until then, a lost focus is taken back once (Checker)
    // car 2026-11-18: iDrive ignores every second focus event (after one that took, the next is ignored; after an ignored
    // one, the other event number takes). A show request's event is checked ~1.1 s later (TargetPoll) and, not taken,
    // sent again at once with the other number instead of hiding after the Verifier's 5 s. Once: the retry itself gets
    // no retry (car 2026-11-18: retrying the retry sent an event every 1.26 s for as long as iDrive ignored them all).
    long long showRetryAtMs;
    unsigned evIdx,evUses,evIgnoredSeen;   // focus event pool (kEvPoolFirst..): current event, its uses, ignoredHolds seen
    void showHold(const char* why,bool retry=true) {
        const long long before=lastHoldMs;
        hold(why,false);
        if(retry&&lastHoldMs!=before)showRetryAtMs=lastHoldMs+1100;   // only when an event actually went out
    }
    static bool holdFocus(void* self,const bmw::Parcel& p) {
        HoldSlot* s=static_cast<HoldSlot*>(self); Entry* e=s->e;
        bool focused=false;p.readBool(focused);
        if(focused)e->focusMask|=1u<<s->idx; else e->focusMask&=~(1u<<s->idx);
        e->lastFocusMs=now_ms();
        plog(focused?"hold focus 1, level":"hold focus 0, level",(int)s->idx);
        e->target.start();
        if(focused){unlink(kLeave);if(!g_pdcActive)unlink(kHidden);touch(kRequest);e->ignoredHolds=0;e->startPolling(500);}
        else {
            if(access(kHidden,F_OK)!=0)g_aaLeftMs=now_ms();   // lost the focus with the picture on screen
            e->check(150);             // decide after the HMI has finished its transition (see Checker)
        }
        return true;
    }
    static bool holdVisibility(void* self,const bmw::Parcel& p) {
        HoldSlot* s=static_cast<HoldSlot*>(self); Entry* e=s->e;
        bool visible=false;p.readBool(visible);
        if(visible)e->visibleMask|=1u<<s->idx; else e->visibleMask&=~(1u<<s->idx);
        return true;
    }
};
// Decoder mode at boot (car 2026-11-20): shown before about 34-40 s of uptime, nbt_decoder's window stalls for good
// (docs/DECODER.md, Boot). Only nbt_knob's "start at boot" waited; NAV at 33.5 s brought the picture up at 34.6 s and
// iDrive's own screen stayed. Every show request now waits until decoder.showdelay (default 40) s of uptime: the request
// file stays and is taken by the first poll after that.
bool showTooEarly() {
    if(access("/fs/ram3/nbt-aa-native/decoder.mode",F_OK)!=0)return false;
    static long minUp=-1;
    if(minUp<0){
        minUp=40;
        if(FILE* d=fopen("/var/opt/hmi/data/rw/NbtAndroidAutoRT/decoder.showdelay","r")){
            long v;
            if(fscanf(d,"%ld",&v)==1&&v>=0&&v<=300)minUp=v;
            fclose(d);
        }
    }
    if(now_s()>=minUp)return false;
    static bool told=false;
    if(!told){told=true;plog("show request held until this many s of uptime (decoder mode)",(int)minUp);}
    return true;
}
void TargetPoll::run() {
    if(!app||!e||e->stopping){active=false;return;}
    bool running=false;
    readStatus(&running);
    const int want=running?(int)(kHoldState+kHoldCount-1):(int)kEntryState;
    if(want!=target){app->updateModel(kTargetModel,want);target=want;plog("entry target ->",want);}
    {   // iDrive's call screens (see kCall)
        const long long now=now_ms();
        const bool up=access(kCall,F_OK)==0;
        if(up!=g_callUp){
            g_callUp=up;
            plog(up?"call: connection 090 up":"call: connection 090 gone",0);
            if(up){
                g_callStartMs=now;
                g_callOverAa=running&&(access(kHidden,F_OK)!=0||now-g_aaLeftMs<3000);
                if(g_callOverAa)plog("call over Android Auto: iDrive's call screens are sent back until 6 s after it",0);
            }
            else g_callEndMs=now;
        }
        if(running&&!e->holdFocused()){if(!g_lostSinceMs)g_lostSinceMs=now;}
        else g_lostSinceMs=0;
        if(g_callShowAtMs&&now>=g_callShowAtMs){g_callShowAtMs=0;touch(kShowRequest);plog("call screen: show request after the NAV",0);}
        if(g_callOverAa){
            if(g_driverLeftMs>g_callStartMs){g_callOverAa=false;g_callShowAtMs=0;plog("call screen: the driver chose iDrive",0);}
            else if(!g_callUp&&now-g_callEndMs>6000){g_callOverAa=false;plog("call screens: over",0);}
            else if(running&&!g_pdcActive&&g_lostSinceMs&&now-g_lostSinceMs>=1000&&now-g_callShowMs>=3000){
                g_callShowMs=now;
                touch(kHidden);   // the call screen is on screen; and nbt_knob must not take our NAV for NAV in Android Auto
                unlink(kLeave);
                touch("/fs/ram3/nbt-aa-hmi-nav");
                g_callShowAtMs=now+250;
                plog("call screen over Android Auto -> NAV to iDrive",0);
            }
        }
    }
    if(access(kShowRequest,F_OK)==0&&!showTooEarly()&&unlink(kShowRequest)==0){
        // NAV (car 2026-11-18): iDrive opens its navigation ~1.3 s after the key, after the show request has already
        // brought Android Auto back, and so took the screen again; with Android Auto already on screen (NAV pressed in
        // Android Auto) it always did. For 4 s the Checker takes the focus back once more.
        if(running)e->showUntilMs=now_ms()+4000;
        unlink(kLeave);                    // an iDrive key from before this request (written also while hidden)
        if(!running)plog("show request ignored: Android Auto not running",0);
        else if(e->holdFocused()){
            if(!g_pdcActive&&unlink(kHidden)==0)plog("show request: page still focused, picture shown again",0);
            else plog("show request: already on screen",0);
        }
        else e->showHold("show request");
    }

    if(e->showRetryAtMs&&now_ms()>=e->showRetryAtMs){
        e->showRetryAtMs=0;
        // the driver left Android Auto with an iDrive key in between (car 2026-11-18: RADIO right after NAV was pulled
        // back): no retry, no follow-up
        if(access(kLeave,F_OK)==0){e->showUntilMs=0;plog("show request retry dropped: Android Auto left",0);}
        else if(running&&!e->holdFocused()){
            ++e->ignoredHolds;                                     // next event: the other number
            e->showHold("show request not taken -> other focus event",false);
        }
    }
    app->post(this,250);   // 250 ms: show requests (NAV key, parking assistant) take effect quickly
}
void Checker::run() {
    pending=false;
    if(!e||e->stopping||!e->app||e->holdFocused())return;    // BACK only climbed one hold level: nothing to do
    readStatus(&e->running);
    if(unlink(kLeave)==0){plog("check: leave key");g_driverLeftMs=now_ms();touch(kHidden);e->showUntilMs=0;e->showRetryAtMs=0;return;}
    {   // BACK guard (nbt_knob): its tilt right moved the focus into the split screen; the driver's BACK release brings it
        // back. Wait for that instead of sending a hold event (up to 1.5 s after the guard).
        struct stat st;
        if(stat(kGuard,&st)==0&&time(0)-st.st_mtime<=1&&now_ms()-e->lastFocusMs<1500){e->check(200);return;}
    }
    {
        struct stat st;
        const bool back=stat(kBackKey,&st)==0&&time(0)-st.st_mtime<=2;
        unlink(kBackKey);
        if(back&&e->running){
            const long long wait=1000-(now_ms()-e->lastHoldMs);
            if(wait>0){touch(kBackKey);e->check((unsigned)wait+50);plog("check: BACK left the page, hold in ms",(int)wait);return;}
            plog("check: BACK left the page -> back to Android Auto");e->hold("hold (after BACK)");return;
        }
    }
    if(e->running&&now_ms()<e->showUntilMs){
        const long long wait=1000-(now_ms()-e->lastHoldMs);       // hold() sends at most one event per second
        if(wait>0){e->check((unsigned)wait+50);plog("check: focus taken after a show request, hold in ms",(int)wait);return;}
        e->showUntilMs=0;
        e->showHold("check: focus taken after a show request (NAV: iDrive navigation) -> hold");
        return;
    }
    if(e->running&&e->holdVisible()){
        // tilt right puts the iDrive focus into the split screen: taken back at once, so a normal tilt right is not noticed
        // in Android Auto; a second tilt within 1.5 s still reaches the split screen menu (user request 2026-11-08: not
        // aggressive, the HMI stopped honouring a flood of these events before)
        long long tm=now_ms();
        if(tm-e->lastSideHoldMs>=1500){e->lastSideHoldMs=tm;e->hold("check: side panel/popup -> hold");}
        else plog("check: side panel/popup, waiting");
        return;
    }
    if(e->running&&e->entryFocused){e->hold("check: entry page reached -> hold");return;}
    plog("check: page left");
    touch(kHidden);                                        // the page was left: hide the picture
}
void Verifier::run() {
    pending=false;
    if(!e||e->stopping||!e->app||e->holdFocused())return;
    plog("verify: no hold state focused -> hide");
    ++e->ignoredHolds;
    touch(kHidden);
}
// --- "Restart system" (moved from the NbtReboot plugin, 2026-11-12) ---------------------------------------------------
// Page 33 asks ("Restart the head unit now?"), its only button leads to the inert page 34. When page 34 gets the focus,
// nbt_reset from the package sends UDS 11 01 (hardReset) to the head unit through the gateway's HSFZ port, the same
// request as Tool32's STEUERGERAETE_RESET (car 2026-11-12: accepted, the head unit rebooted). No HMI process is touched.
// Guards (the HMI may restore the last page after a reboot, which must never restart the head unit again):
//   - nothing within the first 30 s after boot (kept here: a mistake would be a reboot loop)
//   - only when page 33 got the focus less than 2 minutes before (the driver opened it and pushed the button)
//   - at most one start per 30 s (lifted again when the attempt failed)
// Page 34 says why nothing happens: a guard's reason, or nbt_reset's result. nbt_reset runs through /bin/sh, which
// writes its exit code to /fs/ram3/nbt-reboot.rc; a Runnable on the HMI thread polls that file every 500 ms for 20 s.
// The gateway address comes from the file "gateway" in the package (one line, e.g. 169.254.102.32); without it
// nbt_reset tries the HSFZ broadcast. nbt_reset's output: /fs/ram3/nbt-reboot.log.
const unsigned kSysAskState=33, kSysGoState=34, kSysModel=85;
enum { kTxtSysRestarting=24, kTxtSysTooEarly=25, kTxtSysRunning=26, kTxtSysOpenAgain=27, kTxtSysNoGateway=28,
       kTxtSysRefused=29, kTxtSysNoAnswer=30 };
const char* const kSysRc="/fs/ram3/nbt-reboot.rc";
const char* const kSysLog="/fs/ram3/nbt-reboot.log";
const char* const kSysTool="/var/opt/hmi/data/rw/NbtAndroidAutoRT/nbt_reset";
const char* const kSysGatewayFile="/var/opt/hmi/data/rw/NbtAndroidAutoRT/gateway";
const long kSysBootGuard=30, kSysAskWindow=120, kSysRepeatGuard=30;
long g_sysAsk=-100000, g_sysLastRun=-100000;
bmw::PluginApp* g_sysApp=0;
void sysShow(unsigned text) { if(g_sysApp){bmw::TextID tx(text);g_sysApp->updateModel(kSysModel,tx);} }

// first line of the gateway file, digits and dots only; empty if missing
void sysGateway(char* out,size_t size) {
    out[0]=0;
    int fd=open(kSysGatewayFile,O_RDONLY);
    if(fd<0)return;
    char buf[32]; ssize_t n=read(fd,buf,sizeof(buf)-1); close(fd);
    if(n<=0)return;
    size_t i=0;
    while(i<(size_t)n&&i+1<size&&((buf[i]>='0'&&buf[i]<='9')||buf[i]=='.')){out[i]=buf[i];++i;}
    out[i]=0;
}

bool sysRunReset() {
    char gw[20]; sysGateway(gw,sizeof(gw));
    int out=open(kSysLog,O_WRONLY|O_CREAT|O_APPEND,0644);
    int nul=open("/dev/null",O_RDWR);
    if(nul<0){if(out>=0)close(out);return false;}
    int fdmap[3]={nul,out>=0?out:nul,out>=0?out:nul};
    struct inheritance inh;
    memset(&inh,0,sizeof(inh));
    inh.flags=SPAWN_SETGROUP|SPAWN_SETSIGDEF|SPAWN_SETSIGMASK|SPAWN_NOZOMBIE;
    inh.pgroup=SPAWN_NEWPGROUP;
    sigfillset(&inh.sigdefault);
    sigemptyset(&inh.sigmask);
    // through /bin/sh only to get the exit code into kSysRc (gw: digits and dots only, see sysGateway)
    char cmd[256];
    snprintf(cmd,sizeof(cmd),"%s %s; echo $? > %s",kSysTool,gw,kSysRc);
    char* argv[]={(char*)"/bin/sh",(char*)"-c",cmd,0};
    // the HMI's own environment: its LD_LIBRARY_PATH finds libsocket.so.3 (car 2026-11-12: with PATH only, the loader
    // failed with "Could not load library libsocket.so.3")
    bool ok=spawn("/bin/sh",3,fdmap,&inh,argv,environ)>=0;
    close(nul);
    if(out>=0)close(out);
    return ok;
}

// HMI thread: waits for nbt_reset's exit code and shows the result on page 34
class SysPoll : public bmw::Runnable {
public:
    long started; bool active;
    SysPoll():started(0),active(false) {}
    void start() { started=mono_s(); if(!active&&g_sysApp){active=true;g_sysApp->post(this,500);} }
    void run() {
        active=false;
        if(!g_sysApp)return;
        int fd=open(kSysRc,O_RDONLY);
        if(fd<0){
            if(mono_s()-started<20){active=true;g_sysApp->post(this,500);return;}
            plog("system restart: no result from nbt_reset within 20 s",0);
            sysShow(kTxtSysNoAnswer);g_sysLastRun=-100000;
            return;
        }
        char buf[8]; ssize_t n=read(fd,buf,sizeof(buf)-1); close(fd);
        int rc=n>0&&buf[0]>='0'&&buf[0]<='9'?buf[0]-'0':-1;
        if(rc==0){plog("system restart: reset accepted",0);return;}     // the head unit restarts: "Restarting ..." stays
        plog("system restart failed, nbt_reset exit",rc);
        sysShow(rc==2?kTxtSysRefused:rc==3?kTxtSysNoAnswer:kTxtSysNoGateway);
        g_sysLastRun=-100000;                                          // failed: a new attempt is allowed at once
    }
};
SysPoll g_sysPoll;

int g_sysHandlerObject=0;   // a null delegate object makes the HMI skip the handler (car 2026-11-12)
bool sysAskFocus(void*,const bmw::Parcel& p) {
    bool focused=false;p.readBool(focused);
    if(focused){g_sysAsk=mono_s();plog("system restart page focus",1);}
    return true;
}
bool sysGoFocus(void*,const bmw::Parcel& p) {
    bool focused=false;p.readBool(focused);
    if(!focused)return true;
    const long t=mono_s();
    if(t<kSysBootGuard){plog("system restart ignored: less than 30 s after boot",0);sysShow(kTxtSysTooEarly);return true;}
    if(t-g_sysAsk>kSysAskWindow){plog("system restart ignored: page 33 not opened just before",0);sysShow(kTxtSysOpenAgain);return true;}
    if(t-g_sysLastRun<kSysRepeatGuard){plog("system restart ignored: already started",0);sysShow(kTxtSysRunning);return true;}
    g_sysLastRun=t;
    plog("system restart requested, running nbt_reset",0);
    sysShow(kTxtSysRestarting);
    unlink(kSysRc);
    if(!sysRunReset()){plog("system restart: could not start nbt_reset",0);sysShow(kTxtSysNoGateway);g_sysLastRun=-100000;return true;}
    g_sysPoll.start();
    return true;
}

Entry entry;
bool created=false;
bmw::IPluginApp* create(bmw::IDispatcher& d,const char* name,const bmw::Bundle& p,const bmw::Bundle& c) {
    if(created)return 0;
    bmw::PluginApp* app=new bmw::PluginApp(d,bmw::BasicString<char>(name),p,c);
    entry.app=app;entry.stopping=false;entry.polling=false;entry.checker.e=&entry;entry.checker.pending=false;entry.verifier.e=&entry;entry.verifier.pending=false;
    entry.focusMask=entry.visibleMask=0;
    bmw::ActionDelegate onEntry={&entry,&Entry::entryFocus};
    app->addHandler(kEntryState,kFocus,onEntry);
    entry.back.app=app;entry.shownMode=0;
    entry.target.app=app;entry.target.e=&entry;entry.target.active=false;entry.target.target=kEntryState;
    bmw::ActionDelegate onDecoder={&entry,&Entry::decoderFocus};
    app->addHandler(kDecoderState,kFocus,onDecoder);
    entry.shownDecoder=0;
    bmw::ActionDelegate onPanel={&entry,&Entry::panelFocus};
    app->addHandler(kPanelState,kFocus,onPanel);
    entry.shownPanel=0;entry.shownPanelVis=-1;
    bmw::ActionDelegate onSize={&entry,&Entry::sizeFocus};
    app->addHandler(kSizeState,kFocus,onSize);
    entry.shownSize=0;
    bmw::ActionDelegate on169={&entry,&Entry::mode169Focus};
    bmw::ActionDelegate onWide={&entry,&Entry::modeWideFocus};
    app->addHandler(kMode169State,kFocus,on169);
    app->addHandler(kModeWideState,kFocus,onWide);
    bmw::ActionDelegate onRestart={&entry,&Entry::restartFocus};
    app->addHandler(kRestartState,kFocus,onRestart);
    bmw::ActionDelegate onBoot={&entry,&Entry::bootFocus};
    app->addHandler(kBootState,kFocus,onBoot);
    entry.shownBoot=0;
    for(unsigned i=0;i<kHoldCount;++i){
        bmw::ActionDelegate onHold={&entry.slots[i],&Entry::holdFocus};
        bmw::ActionDelegate onHoldVisible={&entry.slots[i],&Entry::holdVisibility};
        app->addHandler(kHoldState+i,kFocus,onHold);
        app->addHandler(kHoldState+i,kVisible,onHoldVisible);  // type 11 = VISIBLE, as BMW's ActiveSeating registers it
    }
    g_sysApp=app;
    bmw::ActionDelegate onSysAsk={&g_sysHandlerObject,&sysAskFocus};
    bmw::ActionDelegate onSysGo={&g_sysHandlerObject,&sysGoFocus};
    app->addHandler(kSysAskState,kFocus,onSysAsk);
    app->addHandler(kSysGoState,kFocus,onSysGo);
    created=true;
    touch(kHidden);   // nothing is shown before the page is opened
    unlink(kShowRequest);                       // a request left from before this HMI start
    plog("app start at create",app->start()?1:0);
    entry.target.active=true;app->post(&entry.target,5000);   // show requests (Start at boot) before the page is opened
    g_cds.app=app;app->post(&g_cds,3000);
    {
        pthread_attr_t at; pthread_t th;
        pthread_attr_init(&at); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
        pthread_create(&th,&at,autostartThread,0);
        pthread_attr_destroy(&at);
    }
    return reinterpret_cast<bmw::IPluginApp*>(app);
}
void destroy(bmw::IPluginApp* p) {
    if(!created)return;
    entry.stopping=true;
    touch(kHidden);
    entry.checker.e=0;entry.verifier.e=0;entry.back.app=0;entry.target.app=0;g_sysApp=0;
    delete reinterpret_cast<bmw::PluginApp*>(p);   // native handler drops the queued runnable (entry is static)
    entry.app=0;
    created=false;
}
}
struct PluginAppInfo {
    uint16_t major,minor;const char* name;const char* vendor;
    bmw::IPluginApp*(*create)(bmw::IDispatcher&,const char*,const bmw::Bundle&,const bmw::Bundle&);
    void(*destroy)(bmw::IPluginApp*);
};
typedef char InfoSize[(sizeof(PluginAppInfo)==20)?1:-1];
extern "C" { PluginAppInfo pluginAppInfo={1,0,"NbtAndroidAuto","NBT-Project",create,destroy}; }
