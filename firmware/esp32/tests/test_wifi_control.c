#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "WifiControlPolicy.h"
int main(void)
{
    const char *valid[] = {"PERIPH STATUS", "PERIPH STOP", "PERIPH STOP 9",
        "PERIPH KEEP 2 4294967295", "PERIPH LIVE 3 100 0 500",
        "PERIPH LIVE 4 100 3 2500", "PERIPH TEST 5 100 15 500 2500 0"};
    for (unsigned i=0;i<sizeof(valid)/sizeof(valid[0]);++i) assert(WifiControl_PeripheralLine(valid[i]));
    const char *bad[] = {"PERIPH LIVE 0 100 0 1500", "PERIPH LIVE 1 100 4 1500",
        "PERIPH LIVE 1 100 0 499", "PERIPH LIVE 1 100 0 2501", "PERIPH LIVE -1 100 0 1500",
        "PERIPH KEEP 4294967296 100", "PERIPH STOP 1\nPERIPH LIVE 2 100 0 1500",
        "PERIPH STOP 1 extra", "PERIPH TEST 1 100 0 500 2500 0",
        "PERIPH TEST 1 100 1 1700 1300 0", "PERIPH TEST 1 100 1 500 2500 10001",
        "BAT CURRENT 1", "PERIPH SERVO 0 2500", "PERIPH KEEP 1  100"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) assert(!WifiControl_PeripheralLine(bad[i]));
    assert(WifiControl_Deadline(100,101)); assert(WifiControl_Deadline(100,15100));
    assert(!WifiControl_Deadline(100,15101)); assert(!WifiControl_Deadline(100,100));
    assert(!WifiControl_Deadline(100,99)); assert(WifiControl_Deadline(UINT32_MAX-10,10));
    puts("WiFi control: whitelist, bounds, injection, deadline and wrap passed");
}
