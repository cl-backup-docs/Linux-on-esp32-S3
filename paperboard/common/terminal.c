#include "terminal.h"
#include <string.h>
#include "font8x16_basic.h"
static void blank(PbCell* c) { c->ch = ' '; c->inverse = 0; }
void pb_init(PbTerminal* t) {
    memset(t, 0, sizeof(*t)); t->cursor_visible = true;
    for (unsigned y=0;y<PB_ROWS;y++) for(unsigned x=0;x<PB_COLS;x++) blank(&t->cells[y][x]);
}
static void lf(PbTerminal* t) {
    if (++t->y == PB_ROWS) {
        memmove(t->cells[0],t->cells[1],sizeof(t->cells)-sizeof(t->cells[0]));
        t->y=PB_ROWS-1;
        for(unsigned x=0;x<PB_COLS;x++) blank(&t->cells[t->y][x]);
    }
}
static unsigned clamp(unsigned v,unsigned max) { return v>max ? max:v; }
static void erase(PbTerminal* t,unsigned first,unsigned last) {
    for(unsigned i=first;i<=last;i++) { PbCell* c=&t->cells[i/PB_COLS][i%PB_COLS]; blank(c); c->inverse=t->inverse; }
}
static void csi(PbTerminal* t,uint8_t c) {
    unsigned n=t->params[0]?t->params[0]:1;
    unsigned pos=t->y*PB_COLS+t->x;
    if(t->private_seq) {
        if(t->params[0]==25 && (c=='h'||c=='l')) t->cursor_visible=c=='h';
        return;
    }
    switch(c) {
    case 'A': t->y=n>t->y?0:t->y-n; break;
    case 'B': t->y=clamp(t->y+n,PB_ROWS-1); break;
    case 'C': t->x=clamp(t->x+n,PB_COLS-1); break;
    case 'D': t->x=n>t->x?0:t->x-n; break;
    case 'G': t->x=clamp(n-1,PB_COLS-1); break;
    case 'd': t->y=clamp(n-1,PB_ROWS-1); break;
    case 'H': case 'f': t->y=clamp(n-1,PB_ROWS-1); t->x=clamp(t->params[1]?t->params[1]-1:0,PB_COLS-1); break;
    case 'J':
        if(t->params[0]==0) erase(t,pos,PB_ROWS*PB_COLS-1);
        else if(t->params[0]==1) erase(t,0,pos);
        else if(t->params[0]==2) erase(t,0,PB_ROWS*PB_COLS-1);
        break;
    case 'K':
        if(t->params[0]==0) erase(t,pos,(t->y+1)*PB_COLS-1);
        else if(t->params[0]==1) erase(t,t->y*PB_COLS,pos);
        else if(t->params[0]==2) erase(t,t->y*PB_COLS,(t->y+1)*PB_COLS-1);
        break;
    case 'm': for(unsigned i=0;i<=t->nparam;i++) {
        if(t->params[i]==0||t->params[i]==27) t->inverse=false;
        else if(t->params[i]==7) t->inverse=true;
    } break;
    case 's': t->saved_x=t->x; t->saved_y=t->y; break;
    case 'u': t->x=t->saved_x; t->y=t->saved_y; break;
    }
    if(c!='m') t->wrap_pending=false;
}
void pb_feed(PbTerminal* t,const uint8_t* data,size_t len) {
    while(len--) {
        uint8_t c=*data++;
        // OSC strings end with BEL or ESC backslash. Never print their contents.
        if(t->state==3) { if(c==7)t->state=0; else if(c==27)t->state=4; continue; }
        if(t->state==4) { t->state=c=='\\'?0:3; continue; }
        if(c==27) { t->state=1; continue; }
        if(t->state==1) {
            t->state=0;
            if(c=='[') { t->state=2;t->nparam=0;t->private_seq=false;memset(t->params,0,sizeof(t->params)); }
            else if(c==']') t->state=3;
            else if(c=='7') {t->saved_x=t->x;t->saved_y=t->y;}
            else if(c=='8') {t->x=t->saved_x;t->y=t->saved_y;t->wrap_pending=false;}
            else if(c=='c') pb_init(t);
            continue;
        }
        if(t->state==2) {
            if(c>='0'&&c<='9') t->params[t->nparam]=clamp(t->params[t->nparam]*10+c-'0',9999);
            else if(c==';') { if(t->nparam<7) ++t->nparam; else t->state=5; }
            else if(c=='?') t->private_seq=true;
            else if(c>=0x40&&c<=0x7e) {csi(t,c);t->state=0;}
            continue;
        }
        if(t->state==5) {if(c>=0x40&&c<=0x7e)t->state=0;continue;}
        if(c=='\r') {t->x=0;t->wrap_pending=false;}
        else if(c=='\n') {lf(t);t->wrap_pending=false;}
        else if(c=='\b') {if(t->x)--t->x;t->wrap_pending=false;}
        else if(c=='\t') {t->x=clamp((t->x/8+1)*8,PB_COLS-1);t->wrap_pending=false;}
        else if(c>=32 && c!=127) {
            // ASCII first milestone: one replacement per UTF-8 leading byte.
            if((c&0xc0)==0x80) continue;
            if(c>=128)c='?';
            if(t->wrap_pending) {t->x=0;lf(t);t->wrap_pending=false;}
            t->cells[t->y][t->x]=(PbCell){c,t->inverse};
            if(t->x==PB_COLS-1)t->wrap_pending=true;else ++t->x;
        }
    }
}
void pb_render_cell_row(const PbTerminal* t,unsigned row,uint16_t* pixels) {
    for(unsigned dy=0;dy<PB_CELL_H;dy++) {
        for(unsigned col=0;col<PB_COLS;col++) {
            PbCell c=t->cells[row][col];
            // font8x16_basic bit 0 is the leftmost pixel.
            uint8_t bits=font8x16_basic[c.ch<128?c.ch:'?'][dy];
            if(c.inverse) bits=~bits;
            // The cursor is a two-scanline underline below the glyph.
            if(t->cursor_visible && row==t->y && col==t->x && dy>=PB_CELL_H-2) bits=c.inverse?0x00:0xff;
            for(unsigned bit=0;bit<8;bit++) *pixels++=(bits&(1u<<bit))?PB_COLOR_FG:PB_COLOR_BG;
        }
    }
}
void pb_dirty_rows(const PbTerminal* next,const PbTerminal* shown,bool dirty[PB_ROWS]) {
    for(unsigned row=0;row<PB_ROWS;row++) {
        bool changed=memcmp(next->cells[row],shown->cells[row],sizeof(next->cells[row]))!=0;
        if(next->cursor_visible&&(next->y==row)) changed=true;
        if(shown->cursor_visible&&(shown->y==row)) changed=true;
        dirty[row]=changed;
    }
}
