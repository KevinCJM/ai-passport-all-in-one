#pragma once
#include <cerrno>
namespace passport {
constexpr unsigned networkAttempts=3;
// ESP-IDF error values from esp_tls_errors.h and esp_http_client.h.
inline bool retryNetwork(unsigned attempt,int result,int tls,int flags,int socket,int http){
    if(attempt>=networkAttempts||flags||result==0x101||http==401||http==403)return false;
    return http==429||(http>=500&&http<600)||tls==0x8001||tls==0x8004||tls==0x8006||
        result==0x7007||result==0x7008||socket==ECONNRESET||socket==ETIMEDOUT||socket==ECONNREFUSED||socket==EHOSTUNREACH;
}
constexpr unsigned weatherRefreshSeconds(bool known){return known?600:30;}
}
