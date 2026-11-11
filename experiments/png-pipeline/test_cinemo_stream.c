/* VM-only loopback HTTP source. Unknown length, paced Annex-B bytes, bounded
 * lifetime. Observe output before EOF to distinguish streaming from buffering.
 * This is a diagnostic source, not an Android Auto transport implementation.
 */
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

static int send_all(int fd,const char *data,int size) {
    while(size>0) {
        int n=send(fd,data,size,0);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return -1;
        data+=n;size-=n;
    }
    return 0;
}
int main(int argc,char **argv) {
    int listener,client,one=1,attempt;
    int known_length=argc==2 && !strcmp(argv[1],"--known-length");
    int end_markers=argc==2 && !strcmp(argv[1],"--end-markers");
    int chunked=argc==2 && !strcmp(argv[1],"--chunked");
    int reconnect=argc==2 && !strcmp(argv[1],"--reconnect");
    struct sockaddr_in address;
    if(getuid()!=100 || (argc!=1 && !known_length && !end_markers && !chunked && !reconnect))return 2;
    if(reconnect)known_length=1;
    setvbuf(stdout,0,_IONBF,0);
    signal(SIGPIPE,SIG_IGN);alarm(13);
    listener=socket(AF_INET,SOCK_STREAM,0);
    if(listener<0)return 2;
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    memset(&address,0,sizeof(address));address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(18765);
    if(bind(listener,(struct sockaddr*)&address,sizeof(address)) || listen(listener,2))return 2;
    printf("STREAM_SOURCE_READY loopback=127.0.0.1:18765 unknown_length=%d\n",!known_length);
    for(attempt=0;attempt<6;++attempt) {
        char request[4096],data[1024],header[512];int used=0,fd,round,broken=0,observed=0;long total=0;
        struct stat input;
        if(stat("pattern-800x480.h264",&input))return 2;
        if(chunked)
            strcpy(header,"HTTP/1.1 200 OK\r\nContent-Type: video/h264\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        else if(known_length)
            snprintf(header,sizeof(header),"HTTP/1.0 200 OK\r\nContent-Type: video/h264\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",(long long)input.st_size*3);
        else strcpy(header,"HTTP/1.0 200 OK\r\nContent-Type: video/h264\r\nConnection: close\r\n\r\n");
        client=accept(listener,0,0);if(client<0)break;
        while(used<(int)sizeof(request)-1) {
            int n=recv(client,request+used,sizeof(request)-1-used,0);
            if(n<=0)break;used+=n;request[used]=0;
            if(strstr(request,"\r\n\r\n"))break;
        }
        request[used]=0;printf("STREAM_REQUEST %s\n",request);
        if(send_all(client,header,strlen(header))) {close(client);continue;}
        fd=open("pattern-800x480.h264",O_RDONLY);if(fd<0)return 2;
        for(round=0;round<3 && !broken;++round) {
            int n;lseek(fd,0,SEEK_SET);
            while((n=read(fd,data,sizeof(data)))>0) {
                if(chunked) {
                    char size_line[32];int size_len=snprintf(size_line,sizeof(size_line),"%x\r\n",n);
                    if(send_all(client,size_line,size_len)) {broken=1;break;}
                }
                if(send_all(client,data,n)) {broken=1;break;}
                if(chunked && send_all(client,"\r\n",2)) {broken=1;break;}
                total+=n;usleep(reconnect && attempt>0 ? 1000 : 10000);
                if(!observed) {
                    struct stat output;
                    if(stat("http-frames.raw",&output)==0 && output.st_size>0) {
                        printf("STREAM_FIRST_OUTPUT sent=%ld of=%lld output_bytes=%lld\n",total,(long long)input.st_size*3,(long long)output.st_size);
                        observed=1;
                    }
                }
                if(reconnect && attempt==0 && total>=160000) {
                    puts("STREAM_INTENTIONAL_DISCONNECT");broken=1;break;
                }
            }
        }
        close(fd);
        if(!broken && chunked) {
            broken=send_all(client,"0\r\n\r\n",5)!=0;
            puts("STREAM_CHUNKED_END_SENT");
        }
        if(!broken && end_markers) {
            /* H.264 end_of_sequence/end_of_stream NALs with trailing bits. */
            const char end[]={0,0,0,1,10,(char)0x80,0,0,0,1,11,(char)0x80};
            broken=send_all(client,end,sizeof(end))!=0;
            puts("STREAM_END_MARKERS_SENT");
        }
        if(!broken) {
            struct stat st;
            sleep(2);
            printf("STREAM_BEFORE_EOF sent=%ld decoded_output_bytes=%lld\n",total,
                   stat("http-frames.raw",&st)==0 ? (long long)st.st_size : -1LL);
        }
        close(client);printf("STREAM_CONNECTION_CLOSED broken=%d\n",broken);
        if(!broken)break;
    }
    close(listener);return 0;
}

