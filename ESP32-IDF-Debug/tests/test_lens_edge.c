#include "LensEdge.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    for(unsigned v=0;v<256;++v)for(unsigned c=0;c<5;++c) {
        assert(LensEdge_Value(v,c,0)==v);
        for(unsigned profile=0;profile<4;++profile) {
            if(c==2)assert(LensEdge_Value(v,c,profile)==v);
            if(profile==1)assert((LensEdge_Value(v,c,profile)&240)==(v&240));
            if(profile==2 && c>=2)assert(LensEdge_Value(v,c,profile)==v);
            if(profile==3 && c<=2)assert(LensEdge_Value(v,c,profile)==v);
        }
    }
    assert(LensEdge_Value(0xff,4,3)==0xff);
    assert(LensEdge_Value(0x88,4,1)==0x89);
    assert(LensEdge_Value(0xaa,4,3)==0xdf);
    assert(LensEdge_Value(0xaa,0,2)==0xdf);
    puts("lens edge: baseline, center preservation, independent sides and saturated gains PASS");
    return 0;
}
