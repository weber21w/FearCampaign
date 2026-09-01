#include <string.h>
#include "highscore.h"
#include "input.h"
#include "framebuffer.h"
#include "missile_config.h"

#include <uzebox.h>
#include <avr/pgmspace.h>


/* One Uzebox EEPROM block provides exactly 30 application bytes. Pack five
 * six-digit scores as 24-bit values (15 bytes) plus five 3-letter initials
 * (15 bytes), so the whole top-five table fits one block with no extra EEPROM
 * allocation. */
static u8 hs[30];
/* Rank 0..4 and position 0..2 fit one byte. */
static u8 entryState;
#define ENTRY_RANK() ((u8)(entryState>>2))
#define ENTRY_POS()  ((u8)(entryState&3u))
#define SET_ENTRY_RANK(v) (entryState=(u8)((entryState&3u)|((u8)(v)<<2)))
#define SET_ENTRY_POS(v)  (entryState=(u8)((entryState&0xfcu)|((u8)(v)&3u)))

static uint32_t getScore(u8 rank){
    u8 *p=&hs[(u8)(rank*3u)];
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16);
}
static void setScore(u8 rank,uint32_t s){
    u8 *p=&hs[(u8)(rank*3u)];
    if(s>999999UL) s=999999UL;
    p[0]=(u8)s; p[1]=(u8)(s>>8); p[2]=(u8)(s>>16);
}
static u8 *initialPtr(u8 rank){ return &hs[(u8)(15u+rank*3u)]; }

static void defaults(void){
    memset(hs,0,sizeof(hs));
    setScore(0u,10000UL); { u8 *p=initialPtr(0u); p[0]='L'; p[1]='E'; p[2]='E'; }
    setScore(1u, 9000UL); { u8 *p=initialPtr(1u); p[0]='D'; p[1]='B'; p[2]='D'; }
    setScore(2u, 8000UL); { u8 *p=initialPtr(2u); p[0]='C'; p[1]='N'; p[2]='F'; }
    setScore(3u, 7000UL); { u8 *p=initialPtr(3u); p[0]='J'; p[1]='B'; p[2]='N'; }
    setScore(4u, 6000UL); { u8 *p=initialPtr(4u); p[0]='A'; p[1]='L'; p[2]='C'; }
}

static u8 valid(void){
    u8 i,j;
    uint32_t prev=1000000UL;
    for(i=0;i<FEARCAMPAIGN_HIGHSCORE_COUNT;i++){
        uint32_t s=getScore(i);
        u8 *p=initialPtr(i);
        if(s>999999UL || s>prev) return 0u;
        prev=s;
        for(j=0;j<3u;j++) if(!((p[j]>='A'&&p[j]<='Z')||p[j]=='-')) return 0u;
    }
    return 1u;
}

static void save(void){
    struct EepromBlockStruct b;
    b.id=FEARCAMPAIGN_EEPROM_ID;
    memcpy(b.data,hs,sizeof(hs));
    (void)EepromWriteBlock(&b);
}

void HighScore_Init(void){
    struct EepromBlockStruct b;
    if(EepromReadBlock(FEARCAMPAIGN_EEPROM_ID,&b)==EEPROM_OK){
        memcpy(hs,b.data,sizeof(hs));
        if(valid()) return;
    }
    defaults();
}

u8 HighScore_Qualifies(uint32_t score){
    return score!=0u && score>getScore(FEARCAMPAIGN_HIGHSCORE_COUNT-1u);
}

void HighScore_BeginEntry(uint32_t score){
    u8 r,i;
    for(r=0;r<FEARCAMPAIGN_HIGHSCORE_COUNT;r++) if(score>getScore(r)) break;
    if(r>=FEARCAMPAIGN_HIGHSCORE_COUNT) r=FEARCAMPAIGN_HIGHSCORE_COUNT-1u;
    for(i=FEARCAMPAIGN_HIGHSCORE_COUNT-1u;i>r;i--){
        u8 *dst=initialPtr(i),*src=initialPtr((u8)(i-1u));
        setScore(i,getScore((u8)(i-1u)));
        dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2];
    }
    setScore(r,score);
    {
        u8 *p=initialPtr(r); p[0]='A'; p[1]='A'; p[2]='A';
    }
    entryState=(u8)(r<<2);
}

static void bumpLetter(s8 delta){
    u8 *p=initialPtr(ENTRY_RANK());
    s16 v=(s16)p[ENTRY_POS()]-(s16)'A'+delta;
    while(v<0) v+=26;
    while(v>=26) v-=26;
    p[ENTRY_POS()]=(u8)('A'+v);
}

u8 HighScore_UpdateEntry(void){
    if(Input_pressed(INPUT_UP) || Input_mouseDY()<0) bumpLetter(1);
    if(Input_pressed(INPUT_DOWN) || Input_mouseDY()>0) bumpLetter(-1);
    if(Input_pressed(INPUT_LEFT) || Input_mouseDX()<-3){ if(ENTRY_POS()) SET_ENTRY_POS((u8)(ENTRY_POS()-1u)); }
    if(Input_pressed(INPUT_RIGHT) || Input_mouseDX()>3){ if(ENTRY_POS()<2u) SET_ENTRY_POS((u8)(ENTRY_POS()+1u)); }
    if(Input_pressed(INPUT_B) || Input_mouseRightPressed()){
        if(ENTRY_POS()) SET_ENTRY_POS((u8)(ENTRY_POS()-1u));
        return 0u;
    }
    if(Input_pressed(INPUT_A) || Input_mouseLeftPressed()){
        if(ENTRY_POS()<2u){ SET_ENTRY_POS((u8)(ENTRY_POS()+1u)); return 0u; }
        save(); return 1u;
    }
    if(Input_pressed(INPUT_START)){
        save(); return 1u;
    }
    return 0u;
}

static void drawScore6(s16 x,s16 y,uint32_t s,u8 color){
    Font_Number(x,y,(u16)(s/1000UL),3u,color);
    Font_Number((s16)(x+12),y,(u16)(s%1000UL),3u,color);
}

void HighScore_DrawTable(u8 highlightRank){
    u8 i;
    Font_TextP(58,8,PSTR("HIGH SCORES"),3);
    for(i=0;i<FEARCAMPAIGN_HIGHSCORE_COUNT;i++){
        char init[4];
        s16 y=(s16)(20u+i*9u);
        u8 c=(i==highlightRank)?3u:1u;
        Font_Number(43,y,(u16)(i+1u),1u,c);
        HighScore_Initials(i,init);
        Font_Text(51,y,init,c);
        drawScore6(96,y,getScore(i),c);
    }
}

void HighScore_DrawEntry(void){
    char init[4];
    u8 i;
    s16 ix;

    /* The cabinet clears the playfield and presents GREAT SCORE plus
     * ENTER YOUR INITIALS before accepting three padded letters.  Keep that
     * hierarchy rather than embedding entry in the high-score list; adapt the
     * trackball/fire instructions to the joypad and mouse controls. */
    Font_TextP(58,5,PSTR("GREAT SCORE"),3);
    drawScore6(68,14,getScore(ENTRY_RANK()),1);
    Font_TextP(42,25,PSTR("ENTER YOUR INITIALS"),3);

    HighScore_Initials(ENTRY_RANK(),init);
    for(i=0u;i<3u;i++){
        char one[2]={init[i],0};
        Font_Text((s16)(68u+i*10u),37,one,1);
    }
    ix=(s16)(68u+ENTRY_POS()*10u);
    FB_HLine(ix,43,3u,3u);

    Font_TextP(50,51,PSTR("UP/DOWN CHANGE"),1);
    Font_TextP(56,59,PSTR("A/LMB SELECT"),1);
    Font_TextP(56,67,PSTR("B/RMB BACK"),1);
    Font_TextP(108,67,PSTR("START DONE"),1);
}

uint32_t HighScore_Score(u8 rank){
    return rank<FEARCAMPAIGN_HIGHSCORE_COUNT?getScore(rank):0u;
}
void HighScore_Initials(u8 rank,char out[4]){
    if(rank<FEARCAMPAIGN_HIGHSCORE_COUNT){
        u8 *p=initialPtr(rank); out[0]=(char)p[0]; out[1]=(char)p[1]; out[2]=(char)p[2];
    }else out[0]=out[1]=out[2]='-';
    out[3]=0;
}
