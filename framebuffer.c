#include <string.h>
#include <avr/pgmspace.h>
#include "framebuffer.h"

u8 displayBuffer[FB_BYTES];

#include "data/ui.inc"

static inline void put_px(u8 *row, u8 x, u8 c){
    u8 *p=row+(x>>2);
    u8 shift=(u8)((3u-(x&3u))*2u);
    u8 mask=(u8)(3u<<shift);
    *p=(u8)((*p&~mask)|((c&3u)<<shift));
}

void FB_Clear(u8 color){
    u8 c=(u8)(color&3u);
    c=(u8)(c|(c<<2));
    c=(u8)(c|(c<<4));
    memset(displayBuffer,c,FB_BYTES);
}

void FB_Pixel(s16 x, s16 y, u8 color){
    if((u16)x>=FB_WIDTH || (u16)y>=FB_HEIGHT) return;
    put_px(displayBuffer+(u16)y*FB_STRIDE,(u8)x,color);
}

void FB_HLine(s16 x, s16 y, s16 w, u8 color){
    u8 c,packed;
    u8 *dst;
    if(w<=0 || y<0 || y>=FB_HEIGHT) return;
    if(x<0){ w+=x; x=0; }
    if(x+w>FB_WIDTH) w=FB_WIDTH-x;
    if(w<=0) return;
    c=(u8)(color&3u);
    packed=(u8)(c|(c<<2)); packed=(u8)(packed|(packed<<4));
    while(w>0 && (x&3)){ put_px(displayBuffer+(u16)y*FB_STRIDE,(u8)x,c); x++; w--; }
    dst=displayBuffer+(u16)y*FB_STRIDE+((u8)x>>2);
    while(w>=4){ *dst++=packed; x+=4; w-=4; }
    while(w-->0) put_px(displayBuffer+(u16)y*FB_STRIDE,(u8)x++,c);
}

void FB_VLine(s16 x, s16 y, s16 h, u8 color){
    u8 c=(u8)(color&3u);
    u8 *row;
    if(h<=0 || x<0 || x>=FB_WIDTH) return;
    if(y<0){ h+=y; y=0; }
    if(y+h>FB_HEIGHT) h=FB_HEIGHT-y;
    if(h<=0) return;
    row=displayBuffer+(u16)y*FB_STRIDE;
    while(h-->0){
        put_px(row,(u8)x,c);
        row+=FB_STRIDE;
    }
}

/* Fast in-bounds Bresenham for the game renderer. All missile/battery
 * endpoints are already clipped before drawing, so keep X/Y at 8 bits and
 * carry the packed 2bpp mask/value along with the framebuffer pointer. This
 * avoids the generic line path's 16-bit coordinate work plus two variable
 * shifts for every trail pixel. */
void FB_LineInside(u8 x0,u8 y0,u8 x1,u8 y1,u8 color){
    u8 dx=(u8)(x0<x1?x1-x0:x0-x1);
    s8 sx=(s8)(x0<x1?1:-1);
    u8 ady=(u8)(y0<y1?y1-y0:y0-y1);
    s8 sy=(s8)(y0<y1?1:-1);
    s16 dy=-(s16)ady;
    s16 err=(s16)dx+dy;
    u8 c=(u8)(color&3u);
    u8 shift=(u8)((3u-(x0&3u))*2u);
    u8 mask=(u8)(3u<<shift);
    u8 bits=(u8)(c<<shift);
    u8 *dst=displayBuffer+(u16)y0*FB_STRIDE+(x0>>2);
    s16 rowStep=(s16)(sy*FB_STRIDE);

    for(;;){
        *dst=(u8)((*dst&~mask)|bits);
        if(x0==x1 && y0==y1) break;
        {
            s16 e2=(s16)(err*2);
            if(e2>=dy){
                err+=dy; x0=(u8)(x0+sx);
                if(sx>0){
                    if(shift==0u){
                        shift=6u; mask=0xc0u; bits=(u8)(c<<6); dst++;
                    }else{
                        shift=(u8)(shift-2u); mask>>=2; bits>>=2;
                    }
                }else{
                    if(shift==6u){
                        shift=0u; mask=0x03u; bits=c; dst--;
                    }else{
                        shift=(u8)(shift+2u); mask<<=2; bits<<=2;
                    }
                }
            }
            if(e2<=(s16)dx){ err+=(s16)dx; y0=(u8)(y0+sy); dst+=rowStep; }
        }
    }
}

void FB_FillRect(s16 x, s16 y, s16 w, s16 h, u8 color){
    while(h-->0) FB_HLine(x,y++,w,color);
}

/* Ammo stockpiles are eight pixels wide and placed on four-pixel boundaries,
 * so one source row becomes exactly two read/modify/write bytes. */
void FB_BlitMask8_P(u8 x,u8 y,const u8 *rows,u8 h,u8 color){
    u8 packed=(u8)(color&3u);
    u8 *dst;
    if((x&3u) || x>FB_WIDTH-8u || y>=FB_HEIGHT) return;
    packed=(u8)(packed|(packed<<2));
    packed=(u8)(packed|(packed<<4));
    if((u16)y+h>FB_HEIGHT) h=(u8)(FB_HEIGHT-y);
    dst=displayBuffer+(u16)y*FB_STRIDE+(x>>2);
    while(h--){
        u8 bits=pgm_read_byte(rows++);
        u8 m0=pgm_read_byte(&maskNibble2bpp[(bits>>4)&15u]);
        u8 m1=pgm_read_byte(&maskNibble2bpp[bits&15u]);
        dst[0]=(u8)((dst[0]&~m0)|(packed&m0));
        dst[1]=(u8)((dst[1]&~m1)|(packed&m1));
        dst+=FB_STRIDE;
    }
}

/* Compact 3x5 text drawing. */
static u8 glyphIndex(char c){
    if(c>='a'&&c<='z') c=(char)(c-'a'+'A');
    if(c>='A'&&c<='Z') return (u8)(c-'A');
    if(c>='0'&&c<='9') return (u8)(26+c-'0');
    if(c=='-') return 36;
    if(c=='/') return 37;
    if(c==':') return 38;
    if(c=='.') return 39;
    if(c=='!') return 40;
    if(c=='<') return 41;
    if(c=='>') return 42;
    if(c=='\'') return 43;
    if(c=='"') return 44;
    if(c=='(') return 45;
    if(c==')') return 46;
    if(c==',') return 47;
    if(c=='?') return 48;
    if(c=='%') return 49;
    if(c=='&') return 50;
    if(c=='*') return 51;
    if(c=='_') return 52;
    return 255;
}

static void drawChar(s16 x,s16 y,char c,u8 color){
    u8 gi=glyphIndex(c),ry;
    if(gi==255) return;
    for(ry=0;ry<5;ry++){
        u8 row=pgm_read_byte(&glyphs[gi][ry]);
        if(row&4u) FB_Pixel(x,(s16)(y+ry),color);
        if(row&2u) FB_Pixel((s16)(x+1),(s16)(y+ry),color);
        if(row&1u) FB_Pixel((s16)(x+2),(s16)(y+ry),color);
    }
}

void Font_Text(s16 x,s16 y,const char *s,u8 color){
    char c;
    while((c=*s++)!=0){ if(c!=' ') drawChar(x,y,c,color); x+=4; }
}
void Font_TextP(s16 x,s16 y,const char *s,u8 color){
    char c;
    while((c=(char)pgm_read_byte(s++))!=0){ if(c!=' ') drawChar(x,y,c,color); x+=4; }
}
void Font_Number(s16 x,s16 y,u16 value,u8 minDigits,u8 color){
    char buf[6]; u8 n=0,i;
    do{ buf[n++]=(char)('0'+(value%10u)); value/=10u; }while(value && n<5);
    while(n<minDigits && n<5) buf[n++]='0';
    for(i=0;i<n;i++) drawChar((s16)(x+(n-1u-i)*4u),y,buf[i],color);
}
