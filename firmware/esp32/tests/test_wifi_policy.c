#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "Bsp_WifiPolicy.h"

int main(void)
{
    const char boot[] = "12345678abcdef001122334455667788";
    assert(WifiPolicy_Deadline(100,101,boot,"12345678"));
    assert(WifiPolicy_Deadline(100,15100,boot,"12345678"));
    assert(!WifiPolicy_Deadline(100,15101,boot,"12345678"));
    assert(!WifiPolicy_Deadline(100,100,boot,"12345678"));
    assert(!WifiPolicy_Deadline(100,99,boot,"12345678"));
    assert(!WifiPolicy_Deadline(100,200,boot,"12345679"));
    assert(WifiPolicy_Deadline(UINT32_MAX-50,49,boot,"12345678"));
    assert(!WifiPolicy_Deadline(50,UINT32_MAX-49,boot,"12345678"));
    /* The same queued deadline must expire instead of acquiring a renewed TTL. */
    assert(WifiPolicy_Deadline(1000,2000,boot,"12345678"));
    assert(!WifiPolicy_Deadline(2000,2000,boot,"12345678"));
    const unsigned char expected[] = "Bearer unit-test-token";
    unsigned char received[sizeof(expected)] = {0};
    memcpy(received,expected,sizeof(expected));
    assert(WifiPolicy_SecretEqual(expected,sizeof(expected)-1,received,sizeof(expected)-1,true));
    assert(!WifiPolicy_SecretEqual(expected,sizeof(expected)-1,received,sizeof(expected)-2,true));
    assert(!WifiPolicy_SecretEqual(expected,sizeof(expected)-1,received,sizeof(expected),true));
    assert(!WifiPolicy_SecretEqual(expected,sizeof(expected)-1,received,sizeof(expected)-1,false));
    for (unsigned i=0;i<sizeof(expected)-1;++i) {
        received[i]^=1;
        assert(!WifiPolicy_SecretEqual(expected,sizeof(expected)-1,received,sizeof(expected)-1,true));
        received[i]^=1;
    }
    memset(received,0,sizeof(received));
    assert(!WifiPolicy_SecretEqual(expected,sizeof(expected)-1,received,SIZE_MAX,false));
    puts("wifi policy: deadline/boot/wrap/replay/auth cases passed");
    return 0;
}
