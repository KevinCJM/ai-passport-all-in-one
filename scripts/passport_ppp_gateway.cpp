// PPPoS peer + installed libslirp: uses host sockets, never changes host routes/PF.
#include <slirp/libslirp.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

using Bytes=std::vector<uint8_t>;
static volatile std::sig_atomic_t stopping=0;
static int64_t nanos() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static uint16_t crc(const uint8_t* data,size_t size) {
    uint16_t value=0xffff;
    for(size_t i=0;i<size;++i) { value^=data[i]; for(unsigned b=0;b<8;++b) value=(value>>1)^((value&1)?0x8408:0); }
    return value;
}
static uint16_t be16(const uint8_t* p) { return (p[0]<<8)|p[1]; }
static in_addr ip(const char* text) { in_addr value{}; assert(inet_pton(AF_INET,text,&value)==1); return value; }
static const std::array<uint8_t,6> guestMac{2,0,0,0,0,15}, gatewayMac{0x52,0x55,10,77,0,1};

class Peer {
    struct Timer { SlirpTimerCb callback; void* argument; int64_t at{-1}; };
    Slirp* slirp_{};
    // libslirp retains this pointer; callbacks must outlive every packet and timer.
    SlirpCb callbacks_{};
    int fd_;
    Bytes frame_,wire_;
    bool escape_{},discard_{},lcpSent_{},lcpAck_{},lcpPeer_{},ipcpSent_{},ipcpAck_{},ipcpPeer_{},up_{};
    uint8_t lcpId_{1},ipcpId_{1};
    Bytes lcpOptions_{1,4,5,220,2,6,0,0,0,0,5,6,0x31,0x59,0x26,0x53};
    Bytes ipcpOptions_{3,6,10,77,0,1};
    Bytes peerLcpOptions_;
    std::vector<Timer*> timers_;
    std::vector<pollfd> polls_;
    uint64_t framesIn_{},framesOut_{},ipIn_{},ipOut_{},badFcs_{},dropped_{};
    static int addPoll(slirp_os_socket fd,int flags,void* opaque) {
        auto& self=*static_cast<Peer*>(opaque); short events=0;
        if(flags&SLIRP_POLL_IN) events|=POLLIN;
        if(flags&SLIRP_POLL_OUT) events|=POLLOUT;
        if(flags&SLIRP_POLL_PRI) events|=POLLPRI;
        self.polls_.push_back({fd,events,0}); return self.polls_.size()-1;
    }
    static int getPoll(int index,void* opaque) {
        auto& self=*static_cast<Peer*>(opaque); const auto flags=self.polls_.at(index).revents; int result=0;
        if(flags&POLLIN) result|=SLIRP_POLL_IN;
        if(flags&POLLOUT) result|=SLIRP_POLL_OUT;
        if(flags&POLLPRI) result|=SLIRP_POLL_PRI;
        if(flags&POLLERR) result|=SLIRP_POLL_ERR;
        if(flags&POLLHUP) result|=SLIRP_POLL_HUP;
        return result;
    }
    void packet(uint16_t protocol,const Bytes& payload) {
        Bytes raw{255,3,uint8_t(protocol>>8),uint8_t(protocol)}; raw.insert(raw.end(),payload.begin(),payload.end());
        const auto check=uint16_t(~crc(raw.data(),raw.size())); raw.push_back(check&255);raw.push_back(check>>8);
        Bytes encoded{0x7e};
        for(auto byte:raw) { if(byte<0x20 || byte==0x7d || byte==0x7e) { encoded.push_back(0x7d);encoded.push_back(byte^0x20); } else encoded.push_back(byte); }
        encoded.push_back(0x7e);
        if(wire_.size()+encoded.size()>262144) { ++dropped_;return; }
        wire_.insert(wire_.end(),encoded.begin(),encoded.end());++framesOut_;
    }
    void cp(uint16_t protocol,uint8_t code,uint8_t identity,const Bytes& payload) {
        const auto size=payload.size()+4; Bytes data{code,identity,uint8_t(size>>8),uint8_t(size)};
        data.insert(data.end(),payload.begin(),payload.end());packet(protocol,data);
    }
    void announced() {
        if(up_ || !(ipcpAck_&&ipcpPeer_)) return;
        up_=true;
        // An ARP announcement seeds slirp's neighbor cache for the PPP guest.
        Bytes arp(42);std::fill_n(arp.begin(),6,255);std::copy(guestMac.begin(),guestMac.end(),arp.begin()+6);
        arp[12]=8;arp[13]=6;arp[15]=1;arp[16]=8;arp[18]=6;arp[19]=4;arp[21]=1;
        std::copy(guestMac.begin(),guestMac.end(),arp.begin()+22);
        const uint8_t own[]={10,77,0,15},host[]={10,77,0,1};std::copy_n(own,4,arp.begin()+28);std::copy_n(host,4,arp.begin()+38);
        slirp_input(slirp_,arp.data(),arp.size());stats();
    }
    void control(uint16_t protocol,const uint8_t* data,size_t size) {
        if(size<4 || be16(data+2)<4 || be16(data+2)>size) return;
        const auto code=data[0],identity=data[1];Bytes payload(data+4,data+be16(data+2));
        if(protocol==0xc021&&code==1&&payload!=peerLcpOptions_){
            if(!peerLcpOptions_.empty()){up_=false;lcpSent_=lcpAck_=lcpPeer_=ipcpSent_=ipcpAck_=ipcpPeer_=false;wire_.clear();++lcpId_;++ipcpId_;}
            peerLcpOptions_=payload;
        }
        if(protocol==0xc021 && !lcpSent_) { lcpSent_=true;cp(protocol,1,lcpId_,lcpOptions_); }
        if(code==1) {
            Bytes reject,nak;bool hasIp=false;
            for(size_t i=0;i<payload.size();) {
                if(i+2>payload.size() || payload[i+1]<2 || i+payload[i+1]>payload.size()) return;
                const auto kind=payload[i],length=payload[i+1];Bytes option(payload.begin()+i,payload.begin()+i+length);
                if(protocol==0xc021) {
                    if(!((kind==1&&length==4)||(kind==2&&length==6)||(kind==5&&length==6))) reject.insert(reject.end(),option.begin(),option.end());
                } else {
                    if((kind==3||kind==129||kind==131)&&length==6) {
                        hasIp|=kind==3;
                        const uint8_t desired[]={10,77,0,uint8_t(kind==3?15:3)};
                        if(memcmp(option.data()+2,desired,4)) { std::copy_n(desired,4,option.begin()+2);nak.insert(nak.end(),option.begin(),option.end()); }
                    } else reject.insert(reject.end(),option.begin(),option.end());
                }
                i+=length;
            }
            if(protocol==0x8021&&!hasIp) { const uint8_t address[]={3,6,10,77,0,15};nak.insert(nak.end(),address,address+6); }
            if(!reject.empty()) cp(protocol,4,identity,reject);
            else if(!nak.empty()) cp(protocol,3,identity,nak);
            else { cp(protocol,2,identity,payload);if(protocol==0xc021) lcpPeer_=true;else ipcpPeer_=true; }
        } else if(code==2) {
            if(protocol==0xc021 && identity==lcpId_ && payload==lcpOptions_) lcpAck_=true;
            if(protocol==0x8021 && identity==ipcpId_ && payload==ipcpOptions_) ipcpAck_=true;
        } else if((code==3||code==4)&&protocol==0xc021) {
            // Remove peer-rejected optional LCP options; retain an exact ACK match.
            for(size_t j=0;j+2<=payload.size();) {
                const auto length=payload[j+1];if(length<2||j+length>payload.size()) return;
                for(size_t i=0;i+2<=lcpOptions_.size();) { const auto n=lcpOptions_[i+1];if(n<2||i+n>lcpOptions_.size()) return;
                    if(lcpOptions_[i]==payload[j]) { lcpOptions_.erase(lcpOptions_.begin()+i,lcpOptions_.begin()+i+n);break; }i+=n; }
                j+=length;
            }
            cp(protocol,1,++lcpId_,lcpOptions_);
        } else if(code==5) { cp(protocol,6,identity,payload);up_=false; }
        else if(code==9&&protocol==0xc021) { if(payload.size()>=4) { payload[0]=0x31;payload[1]=0x59;payload[2]=0x26;payload[3]=0x53; }cp(protocol,10,identity,payload); }
        if(lcpAck_&&lcpPeer_&&!ipcpSent_) { ipcpSent_=true;cp(0x8021,1,ipcpId_,ipcpOptions_); }
        announced();
    }
    void frame() {
        if(frame_.size()<6 || crc(frame_.data(),frame_.size())!=0xf0b8) { if(!frame_.empty())++badFcs_;return; }
        ++framesIn_;size_t offset=frame_[0]==255&&frame_[1]==3?2:0;
        uint16_t protocol=frame_[offset++];if(!(protocol&1)) protocol=(protocol<<8)|frame_[offset++];
        const auto* payload=frame_.data()+offset;const auto size=frame_.size()-offset-2;
        if(protocol==0xc021||protocol==0x8021) { control(protocol,payload,size);return; }
        if(protocol==0x21 && up_ && size>=20 && (payload[0]>>4)==4 && be16(payload+2)==size) {
            Bytes ethernet(14);std::copy(gatewayMac.begin(),gatewayMac.end(),ethernet.begin());std::copy(guestMac.begin(),guestMac.end(),ethernet.begin()+6);
            ethernet[12]=8;ethernet[13]=0;ethernet.insert(ethernet.end(),payload,payload+size);++ipIn_;slirp_input(slirp_,ethernet.data(),ethernet.size());
        } else if(protocol!=0x21) { Bytes rejected{uint8_t(protocol>>8),uint8_t(protocol)};rejected.insert(rejected.end(),payload,payload+std::min<size_t>(size,64));cp(0xc021,8,++lcpId_,rejected); }
    }
public:
    explicit Peer(int fd,int forward,int audioForward=0):fd_(fd) {
        SlirpConfig config{};config.version=6;config.in_enabled=true;config.vnetwork=ip("10.77.0.0");config.vnetmask=ip("255.255.255.0");
        config.vhost=ip("10.77.0.1");config.vdhcp_start=ip("10.77.0.15");config.vnameserver=ip("10.77.0.3");
        config.if_mtu=config.if_mru=1500;config.disable_host_loopback=true;config.disable_dhcp=true;
        auto& callbacks=callbacks_;
        callbacks.send_packet=[](const void* data,size_t size,void* opaque)->ssize_t {
            auto& self=*static_cast<Peer*>(opaque);const auto* bytes=static_cast<const uint8_t*>(data);
            if(size>=34&&bytes[12]==8&&bytes[13]==0&&self.up_) { ++self.ipOut_;self.packet(0x21,Bytes(bytes+14,bytes+size)); }return size;
        };
        callbacks.guest_error=[](const char*,void*){};callbacks.clock_get_ns=[](void*) { return nanos(); };
        callbacks.timer_new=[](SlirpTimerCb cb,void* argument,void* opaque)->void* { auto* timer=new Timer{cb,argument};static_cast<Peer*>(opaque)->timers_.push_back(timer);return timer; };
        callbacks.timer_free=[](void* value,void* opaque) { auto& timers=static_cast<Peer*>(opaque)->timers_;auto* timer=static_cast<Timer*>(value);timers.erase(std::remove(timers.begin(),timers.end(),timer),timers.end());delete timer; };
        callbacks.timer_mod=[](void* value,int64_t at,void*) { static_cast<Timer*>(value)->at=at; };
        callbacks.register_poll_socket=[](slirp_os_socket,void*){};callbacks.unregister_poll_socket=[](slirp_os_socket,void*){};callbacks.notify=[](void*){};
        slirp_=slirp_new(&config,&callbacks,this);assert(slirp_);
        if(fd_>=0){lcpSent_=true;cp(0xc021,1,lcpId_,lcpOptions_);}
        if(forward) assert(slirp_add_hostfwd(slirp_,0,ip("127.0.0.1"),forward,ip("10.77.0.15"),80)==0);
        if(audioForward) assert(slirp_add_hostfwd(slirp_,0,ip("127.0.0.1"),audioForward,ip("10.77.0.15"),19546)==0);
    }
    ~Peer() { slirp_cleanup(slirp_);for(auto* timer:timers_) delete timer; }
    void testFraming() {
        const uint8_t corrupt[]={0x7e,1,2,3,4,5,6,0x7e};input(corrupt,sizeof(corrupt));
        Bytes oversized(5000,0x11);oversized.push_back(0x7e);input(oversized.data(),oversized.size());
        for(unsigned size=0;size<65;++size) {
            Bytes raw(size);for(unsigned i=0;i<size;++i)raw[i]=(i*37+size)&255;
            const auto fcs=uint16_t(~crc(raw.data(),raw.size()));raw.push_back(fcs&255);raw.push_back(fcs>>8);
            Bytes wire{0x7e};for(auto byte:raw) { if(byte<0x20||byte==0x7d||byte==0x7e) { wire.push_back(0x7d);wire.push_back(byte^0x20); }else wire.push_back(byte); }wire.push_back(0x7e);input(wire.data(),wire.size());
        }
        assert(badFcs_>=1&&dropped_==1);
    }
    void testRestart(){
        const uint8_t requestA[]={1,1,0,10,5,6,1,2,3,4};control(0xc021,requestA,sizeof(requestA));
        Bytes ack{2,lcpId_,0,uint8_t(lcpOptions_.size()+4)};ack.insert(ack.end(),lcpOptions_.begin(),lcpOptions_.end());control(0xc021,ack.data(),ack.size());
        const uint8_t ipRequest[]={1,3,0,10,3,6,10,77,0,15};control(0x8021,ipRequest,sizeof(ipRequest));
        ack={2,ipcpId_,0,uint8_t(ipcpOptions_.size()+4)};ack.insert(ack.end(),ipcpOptions_.begin(),ipcpOptions_.end());control(0x8021,ack.data(),ack.size());assert(up_);
        const uint8_t requestB[]={1,1,0,10,5,6,5,6,7,8};control(0xc021,requestB,sizeof(requestB));assert(!up_&&lcpSent_&&!lcpAck_&&lcpPeer_&&!ipcpSent_);
        control(0xc021,requestB,sizeof(requestB));assert(lcpPeer_&&!lcpAck_);puts("PPP_RESTART_OK cases=2");
    }
    void stats() { printf("PPP_PEER {\"up\":%s,\"frames_in\":%llu,\"frames_out\":%llu,\"ip_in\":%llu,\"ip_out\":%llu,\"bad_fcs\":%llu,\"dropped\":%llu}\n",up_?"true":"false",framesIn_,framesOut_,ipIn_,ipOut_,badFcs_,dropped_);fflush(stdout); }
    void input(const uint8_t* data,size_t size) {
        for(size_t i=0;i<size;++i) { const auto byte=data[i];
            if(byte==0x7e) { if(!discard_&&!escape_&&!frame_.empty()) frame();frame_.clear();escape_=discard_=false;continue; }
            if(discard_) continue;
            if(byte==0x7d) { escape_=true;continue; }
            frame_.push_back(escape_?byte^0x20:byte);escape_=false;
            if(frame_.size()>4096) { discard_=true;++dropped_; }
        }
    }
    void run() {
        auto last=nanos();
        while(!stopping) {
            polls_.clear();polls_.push_back({fd_,short(POLLIN|(wire_.empty()?0:POLLOUT)),0});uint32_t timeout=20;
            slirp_pollfds_fill_socket(slirp_,&timeout,addPoll,this);const int result=poll(polls_.data(),polls_.size(),std::min<uint32_t>(timeout,20));
            if(result<0&&errno!=EINTR) break;
            if(polls_[0].revents&(POLLHUP|POLLERR)) break;
            if(polls_[0].revents&POLLIN) { uint8_t data[4096];const auto size=::read(fd_,data,sizeof(data));if(size==0) break;if(size>0)input(data,size); }
            if((polls_[0].revents&POLLOUT)&&!wire_.empty()) { const auto size=::write(fd_,wire_.data(),wire_.size());if(size>0)wire_.erase(wire_.begin(),wire_.begin()+size);else if(size<0&&errno!=EAGAIN&&errno!=EINTR)break; }
            slirp_pollfds_poll(slirp_,result<0,getPoll,this);
            const auto now=nanos();auto timers=timers_;
            for(auto* timer:timers) if(timer->at>=0&&timer->at<=now/1000000) { timer->at=-1;timer->callback(timer->argument); }
            if(now-last>1000000000) {
                if(!up_&&lcpSent_&&!lcpAck_)cp(0xc021,1,lcpId_,lcpOptions_);
                if(!up_&&lcpAck_&&lcpPeer_&&ipcpSent_&&!ipcpAck_)cp(0x8021,1,ipcpId_,ipcpOptions_);
                stats();last=now;
            }
        }
        stats();
    }
};

int main(int argc,char** argv) {
    signal(SIGPIPE,SIG_IGN);signal(SIGTERM,[](int){stopping=1;});signal(SIGINT,[](int){stopping=1;});
    if(argc==2&&!strcmp(argv[1],"--self-test")) { const char* sample="123456789";assert(uint16_t(~crc(reinterpret_cast<const uint8_t*>(sample),9))==0x906e);Peer test(-1,0);test.testFraming();test.testRestart();puts("PPP_FCS_FRAMING_OK cases=67");return 0; }
    if((argc-1)%2)return 2;
    int port=19547,forward=18762,audioForward=0,fd=-1;std::string serial;
    for(int i=1;i+1<argc;i+=2) { if(!strcmp(argv[i],"--port"))port=atoi(argv[i+1]);else if(!strcmp(argv[i],"--forward"))forward=atoi(argv[i+1]);else if(!strcmp(argv[i],"--audio-forward"))audioForward=atoi(argv[i+1]);else if(!strcmp(argv[i],"--serial"))serial=argv[i+1];else return 2; }
    if(port<1024||port>65535||forward<1024||forward>65535) return 2;
    if(!serial.empty()) {
        if(serial.rfind("/dev/cu.",0)!=0) return 2;
        fd=open(serial.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK);if(fd<0)return 1;
        if(ioctl(fd,TIOCEXCL)) { close(fd);return 1; }
        termios attributes{};if(tcgetattr(fd,&attributes))return 1;cfmakeraw(&attributes);attributes.c_cflag|=CLOCAL|CREAD;attributes.c_cflag&=~HUPCL;cfsetspeed(&attributes,B115200);if(tcsetattr(fd,TCSANOW,&attributes))return 1;
    } else {
        const int server=socket(AF_INET,SOCK_STREAM,0);int yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr=ip("127.0.0.1");address.sin_port=htons(port);
        if(bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(server,1))return 1;
        puts("PPP_GATEWAY_READY");fflush(stdout);fd=accept(server,nullptr,nullptr);close(server);if(fd<0)return 1;
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
    }
    { Peer peer(fd,forward,audioForward);peer.run(); }close(fd);return 0;
}
