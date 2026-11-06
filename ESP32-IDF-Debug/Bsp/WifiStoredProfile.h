#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "WifiSessionAuth.h"

#define WIFI_STORED_PROFILE_VERSION 0x31505752UL
typedef struct {
    uint32_t version;
    char ssid[33], password[65], token[65];
} WifiStoredProfile;

static inline size_t WifiStoredProfile_Length(const char *s, size_t capacity)
{ size_t n=0; while(n<capacity && s[n])++n; return n; }

static inline bool WifiStoredProfile_Valid(const WifiStoredProfile *p)
{
    if(!p || p->version!=WIFI_STORED_PROFILE_VERSION)return false;
    size_t s=WifiStoredProfile_Length(p->ssid,sizeof(p->ssid));
    size_t n=WifiStoredProfile_Length(p->password,sizeof(p->password));
    if(!s || s>32 || n>64 || (n && n<8))return false;
    if(n==64)for(size_t i=0;i<n;++i) {
        char c=p->password[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;
    }
    return WifiSessionAuth_TokenValid(p->token);
}
