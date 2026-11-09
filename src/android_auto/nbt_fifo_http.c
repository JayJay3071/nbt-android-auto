/* Single-client loopback H.264 source with bounded buffering and backpressure. */
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static volatile sig_atomic_t stop;
static void stopped(int s){(void)s;stop=1;}
static int send_all(int fd,const char* p,unsigned n){
 while(n&&!stop){fd_set set;struct timeval t={2,0};int k;FD_ZERO(&set);FD_SET(fd,&set);
  if(select(fd+1,0,&set,0,&t)<=0)return 0;k=send(fd,p,n,0);if(k<0&&(errno==EINTR||errno==EAGAIN))continue;if(k<=0)return 0;p+=k;n-=k;
 }return !n;
}
int main(int argc,char**argv){
 int fifo,server,client,one=1,used=0;char request[4096],data[65536];struct sockaddr_in addr;
 const char* header="HTTP/1.0 200 OK\r\nContent-Type: video/h264\r\nConnection: close\r\n\r\n";
 if(argc!=3)return 2;
 signal(SIGTERM,stopped);signal(SIGINT,stopped);signal(SIGPIPE,SIG_IGN);
 fifo=open(argv[1],O_RDWR|O_NONBLOCK);if(fifo<0)return 2;
 server=socket(AF_INET,SOCK_STREAM,0);if(server<0)return 2;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
 memset(&addr,0,sizeof(addr));addr.sin_family=AF_INET;addr.sin_port=htons(18765);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(bind(server,(struct sockaddr*)&addr,sizeof(addr))||listen(server,1))return 2;
 client=accept(server,0,0);close(server);if(client<0)return 3;
 fcntl(client,F_SETFL,fcntl(client,F_GETFL)|O_NONBLOCK);
 while(used<(int)sizeof(request)-1&&!stop){fd_set set;struct timeval t={5,0};int n;FD_ZERO(&set);FD_SET(client,&set);
  if(select(client+1,&set,0,0,&t)<=0)return 3;n=recv(client,request+used,sizeof(request)-1-used,0);if(n<=0)return 3;
  used+=n;request[used]=0;if(strstr(request,"\r\n\r\n"))break;
 }
 if(!strstr(request,"\r\n\r\n")||!send_all(client,header,strlen(header)))return 3;
 {int ready=open(argv[2],O_WRONLY|O_CREAT|O_EXCL,0600);if(ready<0)return 3;write(ready,"ready\n",6);close(ready);}
 while(!stop){fd_set set;struct timeval t={0,200000};int n;FD_ZERO(&set);FD_SET(fifo,&set);
  if(select(fifo+1,&set,0,0,&t)<0){if(errno==EINTR)continue;break;}
  if(!FD_ISSET(fifo,&set))continue;n=read(fifo,data,sizeof(data));if(n>0&&!send_all(client,data,n))break;
 }
 close(client);close(fifo);return 0;
}
