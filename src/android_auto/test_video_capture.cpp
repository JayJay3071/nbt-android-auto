// Reuse the lab TLS phone and framing helpers, never a real USB endpoint.
#define main existing_protocol_test_main
#include "test_session_protocol.cpp"
#undef main
#include "aa_video_capture.h"
#include <fstream>
#include <iterator>
struct FakeUsb {int fd,result;const char* prefix;};
static int fake_transfer(void* v,int input,unsigned char* p,unsigned* size) {
    FakeUsb* f=(FakeUsb*)v;
    if(input) {
        fd_set set;FD_ZERO(&set);FD_SET(f->fd,&set);struct timeval t={2,0};
        if(select(f->fd+1,&set,0,0,&t)<=0)return 0;
        // Deliberately split TLS records and AA envelopes across USB transfers.
        ssize_t n=recv(f->fd,p,*size>127 ? 127 : *size,0);
        if(n<=0)return 0;*size=n;return 1;
    }
    unsigned left=*size;
    while(left) {ssize_t n=send(f->fd,p,left,0);if(n<=0)return 0;p+=n;left-=n;}
    return 1;
}
static void* capture_thread(void* p) {
    FakeUsb* f=(FakeUsb*)p;f->result=nbt_video_capture_run(fake_transfer,f,f->prefix);return 0;
}
static Bytes read_file(const char* p) {
    std::ifstream f(p,std::ios::binary);return Bytes(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());
}
static void send_fragmented_video(int fd,peer* server,peer* carrier,const Bytes& body) {
    for(size_t offset=0;offset<body.size();) {
        size_t size=body.size()-offset;if(size>8192)size=8192;
        require(carrier->used==0,"fragment TLS output empty");
        require(mbedtls_ssl_write(&server->ssl,&body[offset],size)==(int)size,"fragment TLS encrypt");
        unsigned flags=offset==0 ? 9 : offset+size==body.size() ? 10 : 8;
        Bytes wire;wire.push_back(6);wire.push_back(flags);
        wire.push_back(carrier->used>>8);wire.push_back(carrier->used);
        if(offset==0) {
            for(int shift=24;shift>=0;shift-=8)wire.push_back(body.size()>>shift);
        }
        wire.insert(wire.end(),carrier->input,carrier->input+carrier->used);carrier->used=0;
        send_bytes(fd,wire);offset+=size;
    }
}
int main() {
    alarm(40);signal(SIGPIPE,SIG_IGN);setvbuf(stdout,0,_IOLBF,0);
    check((int)psa_crypto_init(),"PSA init");configure_identity();
    setenv("NBT_RX_TIMEOUT_S","5",1);
    const unsigned char config_data[]={0,0,0,1,0x67,0x42,0,0x1f,0,0,0,1,0x68,0xab};
    const unsigned char video_data[]={0,0,0,1,0x65,0xaa,0xbb};
    for(unsigned scenario=0;scenario<3;++scenario) {
        const char* prefix=scenario==1 ? "capture-invalid" : scenario==2 ? "capture-fragmented" : "capture-valid";
        int sv[2];require(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0,"fake USB pair");
        FakeUsb fake={sv[0],-1,prefix};pthread_t thread;
        require(pthread_create(&thread,0,capture_thread,&fake)==0,"capture thread");
        Frame request=receive_frame(sv[1]);require(message(request.data)==1,"capture starts version negotiation");
        peer *server=(peer*)calloc(1,sizeof(peer)),*carrier=(peer*)calloc(1,sizeof(peer));
        require(server && carrier,"lab peers");server->other=carrier;initialize(server,1,NULL);
        negotiate(sv[1],server,carrier);
        send_encrypted(sv[1],server,carrier,0,msg(5));
        require(message(receive_encrypted(sv[1],server,0))==6,"capture capability response");
        send_encrypted(sv[1],server,carrier,6,msg(0x8000));
        require(message(receive_encrypted(sv[1],server,6))==0x8003,"video setup");
        require(message(receive_encrypted(sv[1],server,6))==0x8008,"video focus");
        Bytes start=msg(0x8001);start.push_back(8);start.push_back(7);start.push_back(16);start.push_back(0);
        send_encrypted(sv[1],server,carrier,6,start);
        Bytes cfg=msg(1);cfg.insert(cfg.end(),config_data,config_data+sizeof(config_data));
        send_encrypted(sv[1],server,carrier,6,cfg);
        require(message(receive_encrypted(sv[1],server,6))==0x8004,"config ack");
        Bytes frame=msg(0);frame.resize(10,0);frame[9]=42;
        frame.insert(frame.end(),video_data,video_data+sizeof(video_data));
        if(scenario==1)frame[10]=0xff;
        if(scenario==2) {frame.resize(32778,0x55);send_fragmented_video(sv[1],server,carrier,frame);}
        else send_encrypted(sv[1],server,carrier,6,frame);
        if(scenario!=1) {
            Bytes ack=receive_encrypted(sv[1],server,6);
            require(message(ack)==0x8004 && ack.size()>=6 && ack[2]==8 && ack[3]==7,"video ack preserves stream id");
            send_encrypted(sv[1],server,carrier,0,msg(15));
            require(message(receive_encrypted(sv[1],server,0))==16,"capture bye response");
        } else usleep(200000);
        shutdown(sv[1],SHUT_RDWR);close(sv[1]);pthread_join(thread,0);close(sv[0]);
        require(fake.result==(scenario==1 ? 3 : 0),"capture success/error result");
        release(server);free(carrier);
    }
    Bytes expected(config_data,config_data+sizeof(config_data));expected.insert(expected.end(),video_data,video_data+sizeof(video_data));
    require(read_file("capture-valid.h264")==expected,"exact config + media, no AA header/timestamp bytes");
    Bytes csv=read_file("capture-valid.csv");std::string text(csv.begin(),csv.end());
    require(text.find("frame,0,42,14,7\n")!=std::string::npos,"PTS and message boundary retained");
    require(read_file("capture-invalid.h264")==Bytes(config_data,config_data+sizeof(config_data)),"invalid media never stored");
    expected.resize(sizeof(config_data)+32768,0x55);
    require(read_file("capture-fragmented.h264")==expected,"large encrypted fragmented media exact");
    puts("AA_VIDEO_CAPTURE_SIMULATOR_OK authenticated_discovery_setup_focus_ack_pts_payload fragmented_32KiB malformed_media_rejected");
    return 0;
}
