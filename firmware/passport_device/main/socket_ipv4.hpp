#pragma once
#include <cstdint>
#include <cstring>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace passport {
inline bool ipv4Matches(const sockaddr_storage& address,socklen_t length,uint32_t expected){
    if(address.ss_family==AF_INET&&length>=sizeof(sockaddr_in)){
        sockaddr_in ipv4{};std::memcpy(&ipv4,&address,sizeof(ipv4));return ipv4.sin_addr.s_addr==expected;
    }
    if(address.ss_family==AF_INET6&&length>=sizeof(sockaddr_in6)){
        sockaddr_in6 ipv6{};std::memcpy(&ipv6,&address,sizeof(ipv6));
        if(IN6_IS_ADDR_V4MAPPED(&ipv6.sin6_addr)){
            uint32_t ipv4;std::memcpy(&ipv4,ipv6.sin6_addr.s6_addr+12,sizeof(ipv4));return ipv4==expected;
        }
    }
    return false;
}
inline bool socketLocalIpv4(int socket,uint32_t expected){
    sockaddr_storage address{};socklen_t length=sizeof(address);
    return getsockname(socket,reinterpret_cast<sockaddr*>(&address),&length)==0&&ipv4Matches(address,length,expected);
}
}
