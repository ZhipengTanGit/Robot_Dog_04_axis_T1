#include "FaceLogic.h"
#include "FaceRender.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void tap(FaceLogic *s,uint32_t t)
{
    FaceLogic_Screen(s,t,true,true); FaceLogic_Screen(s,t+80,true,false);
}
int main(int argc,char **argv)
{
    FaceLogic s; FaceLogic_Init(&s,100);
    assert(!s.face_page);tap(&s,200);assert(s.face_page && s.taps==1);
    tap(&s,600);assert(s.selected==FACE_CURIOUS);
    FaceLogic_Screen(&s,1000,true,true);FaceLogic_Update(&s,2200);
    assert(!s.face_page);FaceLogic_Screen(&s,2250,true,false);assert(!s.face_page);
    tap(&s,2600);assert(s.face_page);
    FaceLogic_Screen(&s,3000,true,true);FaceLogic_Screen(&s,3050,false,false);
    assert(s.taps==3); /* A failed touch read cannot create a release/tap. */
    FaceLogic_Head(&s,3100,10000,false,70);assert(s.head_events==0);
    FaceLogic_Head(&s,3200,10250,true,71);FaceLogic_Update(&s,3200);
    assert(s.head_events==1 && FaceLogic_Mood(&s,3200)==FACE_COMFORT);
    FaceLogic_Head(&s,3300,10250,true,71);assert(s.head_at==3200);
    FaceLogic_Head(&s,3500,10500,false,71);FaceLogic_Update(&s,3500);
    assert(FaceLogic_Mood(&s,7000)==FACE_COMFORT);
    assert(FaceLogic_Mood(&s,7201)==FACE_CURIOUS);
    FaceLogic_Head(&s,7300,50,false,0);assert(s.head_events==1); /* MCU reset is not a pet. */
    FaceLogic_Head(&s,7400,300,true,1);assert(s.head_events==2);
    FaceLogic_Update(&s,10000);assert(FaceLogic_Mood(&s,11500)==FACE_CURIOUS);
    FaceLogic_Init(&s,UINT32_MAX-100);tap(&s,UINT32_MAX-60);assert(s.face_page);
    uint8_t *guard=malloc(FACE_BYTES+32);assert(guard);memset(guard,0xa5,FACE_BYTES+32);
    unsigned previous=0;
    for(unsigned mood=0;mood<=FACE_COMFORT;++mood) {
        FaceRender_Draw(guard+16,(FaceMood)mood,1500);
        for(unsigned i=0;i<16;++i)assert(guard[i]==0xa5 && guard[FACE_BYTES+16+i]==0xa5);
        unsigned lit=0,hash=2166136261U;
        for(unsigned i=0;i<FACE_BYTES;++i){lit+=guard[16+i]!=0;hash=(hash^guard[16+i])*16777619U;}
        assert(lit>1000 && (mood==0 || hash!=previous));previous=hash;
        if(argc>1) {
            char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",argv[1],FaceLogic_Name((FaceMood)mood));
            FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n502 410\n255\n");
            for(unsigned y=0;y<410;++y)for(unsigned x=0;x<502;++x) {
                uint8_t rgb[3]={0};
                if(x>=FACE_X && x<FACE_X+FACE_W && y>=FACE_Y && y<FACE_Y+FACE_H) {
                    unsigned at=((FACE_W-1-(x-FACE_X))*FACE_H+y-FACE_Y)*2+16;
                    unsigned v=(guard[at]<<8)|guard[at+1];
                    rgb[0]=(uint8_t)((v>>11)*255/31);rgb[1]=(uint8_t)(((v>>5)&63)*255/63);rgb[2]=(uint8_t)((v&31)*255/31);
                }
                fwrite(rgb,1,3,f);
            }
            fclose(f);
        }
    }
    free(guard);
    puts("face: navigation, long hold, sensor freshness, pet response/expiry/reset/wrap, five bounded renderers PASS");
    return 0;
}
