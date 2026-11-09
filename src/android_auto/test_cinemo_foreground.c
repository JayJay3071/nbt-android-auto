/* VM-only adapter, never deploy to NBT. Preserve child ownership and redirect
 * the Cinemo resource node into the unprivileged test user's private folder.
 * This tests an adapted VM runtime, not stock vehicle integration.
 */
#include <sys/dispatch.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dlfcn.h>
static const char *mapped(const char *path) {
 const char *node=getenv("NBT_VM_CINEMO_NODE");
 if(strcmp(path,"/dev/io-nme"))return path;
 if((getuid()!=100 && getuid()!=0) || !node || strncmp(node,"/home/nbtbuild/.nbt-build/",24)) {errno=EPERM;return 0;}
 return node;
}
int procmgr_daemon(int status,unsigned flags) {
 (void)status;
 if((getuid()!=100 && getuid()!=0) || flags!=15) {errno=EPERM;return -1;}
 alarm(20);return 0;
}
int open(const char *path,int flags,...) {
 int (*real_open)(const char*,int,...)=dlsym(RTLD_NEXT,"open");
 mode_t mode=0;const char *p=mapped(path);
 if(!p || !real_open)return -1;
 if(flags&O_CREAT) {va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}
 return real_open(p,flags,mode);
}
int resmgr_attach(dispatch_t *d,resmgr_attr_t *a,const char *path,enum _file_type t,unsigned flags,const resmgr_connect_funcs_t *c,const resmgr_io_funcs_t *i,RESMGR_HANDLE_T *h) {
 int (*real_attach)(dispatch_t*,resmgr_attr_t*,const char*,enum _file_type,unsigned,const resmgr_connect_funcs_t*,const resmgr_io_funcs_t*,RESMGR_HANDLE_T*)=dlsym(RTLD_NEXT,"resmgr_attach");
 const char *p=mapped(path);
 if(!p || !real_attach)return -1;
 return real_attach(d,a,p,t,flags,c,i,h);
}

