#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "WifiStoredProfile.h"

int main(void)
{
    WifiStoredProfile p={.version=WIFI_STORED_PROFILE_VERSION};
    strcpy(p.ssid,"paired-2.4G");strcpy(p.password,"test-pass-123");
    memset(p.token,'a',64);p.token[64]=0;
    assert(WifiStoredProfile_Valid(&p));
    WifiStoredProfile rebooted;memcpy(&rebooted,&p,sizeof(p));
    assert(WifiStoredProfile_Valid(&rebooted));
    p.version=0;assert(!WifiStoredProfile_Valid(&p));p=rebooted;
    memset(p.token,0,sizeof(p.token));assert(!WifiStoredProfile_Valid(&p));p=rebooted;
    memset(p.ssid,'x',sizeof(p.ssid));assert(!WifiStoredProfile_Valid(&p));p=rebooted;
    memset(p.password,'x',sizeof(p.password));assert(!WifiStoredProfile_Valid(&p));p=rebooted;
    strcpy(p.password,"short");assert(!WifiStoredProfile_Valid(&p));
    p.password[0]=0;assert(WifiStoredProfile_Valid(&p));
    memset(p.password,'g',64);p.password[64]=0;assert(!WifiStoredProfile_Valid(&p));
    memset(p.password,'f',64);assert(WifiStoredProfile_Valid(&p));
    puts("Stored WiFi profile: reboot copy, version, credentials, paired key and bounded validation passed.");
    return 0;
}
