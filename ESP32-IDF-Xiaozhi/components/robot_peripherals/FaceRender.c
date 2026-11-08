#include "FaceRender.h"
#include <math.h>
#include <string.h>

typedef struct { unsigned r,g,b; } Color;
static const Color mint = {91,226,239}, pearl = {191,255,251}, blush = {235,123,164};
static Color dim(Color c, float k) { return (Color){(unsigned)(c.r*k),(unsigned)(c.g*k),(unsigned)(c.b*k)}; }
static void paint(uint8_t *p, int x, int y, Color c, bool maximum)
{
    x -= FACE_X; y -= FACE_Y;
    if ((unsigned)x >= FACE_W || (unsigned)y >= FACE_H) return;
    unsigned at = ((FACE_W - 1U - (unsigned)x) * FACE_H + (unsigned)y) * 2U;
    if (maximum) {
        unsigned old=((unsigned)p[at]<<8)|p[at+1];
        unsigned r=(old>>11)*255U/31U,g=((old>>5)&63U)*255U/63U,b=(old&31U)*255U/31U;
        if(c.r<r)c.r=r;
        if(c.g<g)c.g=g;
        if(c.b<b)c.b=b;
    }
    uint16_t v = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    p[at] = (uint8_t)(v >> 8); p[at+1] = (uint8_t)v;
}
static void pixel(uint8_t *p,int x,int y,Color c) { paint(p,x,y,c,false); }
static void ellipse_blend(uint8_t *p, float cx, float cy, float rx, float ry, Color c, bool maximum)
{
    if (rx < 0.5f || ry < 0.5f) return;
    for (int y=(int)floorf(cy-ry); y<=(int)ceilf(cy+ry); ++y) {
        float yy=(y-cy)/ry, v=1.0f-yy*yy;
        if (v<0) continue;
        float half=rx*sqrtf(v), left=cx-half, right=cx+half;
        for (int x=(int)floorf(left); x<=(int)ceilf(right); ++x) {
            float coverage=fminf(1.0f,fminf(x-left+1.0f,right-x+1.0f));
            paint(p,x,y,dim(c,fmaxf(0,coverage)),maximum);
        }
    }
}
static void ellipse(uint8_t *p,float cx,float cy,float rx,float ry,Color c)
{ ellipse_blend(p,cx,cy,rx,ry,c,false); }
static void pill(uint8_t *p,float cx,float cy,float w,float h,Color c)
{
    float r=fminf(w,h)*0.5f;
    for(int y=(int)floorf(cy-h*0.5f);y<=(int)ceilf(cy+h*0.5f);++y) {
        float d=fmaxf(0,fabsf(y-cy)-(h*0.5f-r));
        float half=w*0.5f-r+sqrtf(fmaxf(0,r*r-d*d));
        if(fabsf(y-cy)>h*0.5f)continue;
        for(int x=(int)floorf(cx-half);x<=(int)ceilf(cx+half);++x)
            pixel(p,x,y,dim(c,fminf(1,fminf(x-(cx-half)+1,cx+half-x+1))));
    }
}
static void arc(uint8_t *p,float cx,float cy,float width,float bend,float radius,Color c)
{
    for(int i=0;i<=40;++i) {
        float t=(float)i/20.0f-1.0f;
        ellipse_blend(p,cx+t*width*0.5f,cy+bend*(1-t*t),radius,radius,c,true);
    }
}
static void closed_eye(uint8_t *p,float x,float y,Color c)
{
    arc(p,x,y,76,-19,10,dim(c,0.07f));
    arc(p,x,y,76,-19,7,dim(c,0.25f));
    arc(p,x,y,76,-19,4.5f,c);
}
static void open_eye(uint8_t *p,float x,float y,float w,float h,Color c)
{
    pill(p,x,y,w+12,h+12,dim(c,0.025f));
    pill(p,x,y,w+6,h+6,dim(c,0.12f));
    pill(p,x,y,w,h,c);
    if(h>25) {
        pill(p,x-8,y-h*0.18f,w*0.47f,h*0.29f,dim(pearl,0.88f));
    }
}
static void heart(uint8_t *p,float cx,float cy,float size,Color c)
{
    ellipse(p,cx-size*0.27f,cy,size*0.32f,size*0.32f,c);
    ellipse(p,cx+size*0.27f,cy,size*0.32f,size*0.32f,c);
    for(int y=0;y<(int)(size*0.78f);++y) {
        float half=size*0.56f*(1-y/(size*0.78f));
        for(int x=(int)-half;x<=(int)half;++x)pixel(p,(int)cx+x,(int)cy+y,c);
    }
}
void FaceRender_Draw(uint8_t *p,FaceMood mood,uint32_t now)
{
    memset(p,0,FACE_BYTES);
    float breathe=sinf((float)(now%3200U)*6.2831853f/3200.0f);
    float glance=sinf((float)(now%9600U)*6.2831853f/9600.0f)*5;
    float open=1;
    unsigned blink=now%4600U;
    if(blink>3900U && blink<4200U)open=fabsf((float)blink-4050.0f)/150.0f;
    float y=180+breathe*1.7f;
    if(mood==FACE_COMFORT || mood==FACE_JOY) {
        Color c=mood==FACE_COMFORT?(Color){123,239,217}:mint;
        closed_eye(p,157,y+6,c);closed_eye(p,345,y+6,c);
        for(int side=0;side<2;++side) {
            float cx=side?368:134;
            for(int k=0;k<3;++k)pill(p,cx+k*9-9,219,4,10,dim(blush,0.6f));
        }
        arc(p,251,242,34,10+breathe,3.5f,dim(c,0.85f));
        if(mood==FACE_COMFORT)heart(p,251,108-breathe*2,16,dim(blush,0.85f));
        else {ellipse(p,251,252,9,6,dim(blush,0.85f));}
    } else if(mood==FACE_SLEEPY) {
        open_eye(p,157,y,82,fmaxf(7,28*open),dim(mint,0.65f));
        open_eye(p,345,y+2,82,fmaxf(7,28*open),dim(mint,0.65f));
        ellipse(p,251,248,5,4,dim(mint,0.7f));
        ellipse(p,391,123-breathe*2,3,3,dim(mint,0.22f));
    } else {
        float right=mood==FACE_CURIOUS?111:92;
        open_eye(p,157+glance,y,76,fmaxf(7,92*open),mint);
        open_eye(p,345+glance,y-(mood==FACE_CURIOUS?5:0),76,fmaxf(7,right*open),mint);
        if(mood==FACE_CURIOUS)arc(p,345,111,45,-5,2.4f,dim(mint,0.45f));
        arc(p,251,244,26,4,2.5f,dim(mint,0.65f));
    }
}
