#include "terminal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
_Static_assert(PB_COLS*PB_CELL_W==PB_WIDTH&&PB_ROWS*PB_CELL_H==PB_HEIGHT,"geometry");
_Static_assert(PB_WIDTH==240&&PB_HEIGHT==240,"JLX130-026 panel size");
static void send(PbTerminal* t,const char* s){pb_feed(t,(const uint8_t*)s,strlen(s));}
static unsigned fg_count(const uint16_t* p,unsigned n){unsigned c=0;for(unsigned i=0;i<n;i++)c+=p[i]==PB_COLOR_FG;return c;}
int main(void) {
    PbTerminal a,b;
    static uint16_t row[PB_CELL_H*PB_WIDTH];
    bool dirty[PB_ROWS];
    pb_init(&a);send(&a,"abc\rZ");assert(a.cells[0][0].ch=='Z'&&a.cells[0][1].ch=='b');
    // Writing into the last cell wraps and scrolls at the bottom row.
    send(&a,"\033[15;30HQ!");assert(a.cells[13][29].ch=='Q'&&a.cells[14][0].ch=='!');
    send(&a,"\033[2J\033[H\033[7mA\033[0mB");
    assert(a.cells[0][0].inverse&& !a.cells[0][1].inverse);
    send(&a,"\033[1;2H\033[K");assert(a.cells[0][1].ch==' '&&a.cells[0][0].ch=='A');
    send(&a,"\033]0;hidden\007X");assert(a.cells[0][1].ch=='X');
    send(&a,"\033[999999999999999999;99999H");assert(a.x==PB_COLS-1&&a.y==PB_ROWS-1);
    // Fragmented escape sequences produce identical results.
    pb_init(&a);pb_init(&b);
    const char* text="hello\033[2;3Hworld\033[7m!\033[0m\r\nnext\033[?25l";
    send(&a,text);for(size_t i=0;i<strlen(text);i++)pb_feed(&b,(const uint8_t*)text+i,1);
    assert(!memcmp(&a,&b,sizeof(a)));
    // Rendering: blank cells are background, inverse flips the whole cell.
    pb_init(&a);a.cursor_visible=false;
    send(&a," \033[7m ");
    pb_render_cell_row(&a,0,row);
    for(unsigned i=0;i<8;i++)assert(row[i]==PB_COLOR_BG);
    for(unsigned i=8;i<16;i++)assert(row[i]==PB_COLOR_FG);
    // Glyphs carry ink in the first cell only, not a full block and not empty.
    pb_init(&a);a.cursor_visible=false;send(&a,"A");
    pb_render_cell_row(&a,0,row);
    unsigned ink=fg_count(row,PB_CELL_H*PB_WIDTH);assert(ink>0&&ink<PB_CELL_H*PB_WIDTH);
    for(unsigned y=0;y<PB_CELL_H;y++)for(unsigned x=8;x<PB_WIDTH;x++)assert(row[y*PB_WIDTH+x]==PB_COLOR_BG);
    // The cursor is a two-scanline underline at the cursor cell only.
    pb_init(&a);
    pb_render_cell_row(&a,0,row);
    for(unsigned i=0;i<8;i++) {
        assert(row[(PB_CELL_H-2)*PB_WIDTH+i]==PB_COLOR_FG);
        assert(row[(PB_CELL_H-1)*PB_WIDTH+i]==PB_COLOR_FG);
        assert(row[(PB_CELL_H-3)*PB_WIDTH+i]==PB_COLOR_BG);
    }
    pb_render_cell_row(&a,1,row);
    for(unsigned i=0;i<PB_CELL_H*PB_WIDTH;i++)assert(row[i]==PB_COLOR_BG);
    // Dirty rows: changed cells and either cursor row.
    pb_init(&a);pb_init(&b);a.cursor_visible=b.cursor_visible=false;
    send(&a,"\033[5;3Hx");
    pb_dirty_rows(&a,&b,dirty);
    assert(dirty[4]&&!dirty[0]&&!dirty[3]&&!dirty[5]);
    b.cursor_visible=true;b.y=10;
    pb_dirty_rows(&a,&b,dirty);assert(dirty[4]&&dirty[10]&&!dirty[2]);
    a.cursor_visible=true;a.y=2;
    pb_dirty_rows(&a,&b,dirty);assert(dirty[2]&&dirty[4]&&dirty[10]&&!dirty[1]&&!dirty[11]);
    // Arbitrary serial bytes cannot escape grid/parameter bounds (run with ASan/UBSan).
    unsigned seed=123;for(unsigned i=0;i<200000;i++) {
        seed=1664525*seed+1013904223;uint8_t ch=seed>>24;pb_feed(&a,&ch,1);
        assert(a.x<PB_COLS&&a.y<PB_ROWS&&a.nparam<8);
    }
    printf("terminal tests passed; two states: %zu bytes\n",2*sizeof(PbTerminal));
    return 0;
}
