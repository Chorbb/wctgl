/* Короткие проверки процессора на маленьких программах. */
#include <stdio.h>
#include <string.h>
#include "../core/cpu6502.h"
static uint8_t mem[65536];
static Cpu *cur;
static uint8_t rd(void *c, uint16_t a){(void)c;cur->cycles++;return mem[a];}
static void wr(void *c, uint16_t a, uint8_t v){(void)c;cur->cycles++;mem[a]=v;}
static int fails=0;
#define CHECK(cond,msg) do{ if(!(cond)){printf("FAIL: %s\n",msg);fails++;} }while(0)
static void run(Cpu *c, const uint8_t *prog, int n, int steps){
    memset(mem,0,sizeof mem); memcpy(mem+0x8000,prog,n);
    mem[0xFFFC]=0x00; mem[0xFFFD]=0x80;
    cur=c; cpu_init(c,rd,wr,NULL); cpu_reset(c);
    for(int i=0;i<steps && !c->jammed;i++) cpu_step(c);
}
int main(void){
    Cpu c;
    /* сумма 1..10 в цикле: LDA #0; LDX #10; loop: STX $00; CLC; ADC $00; DEX; BNE loop; BRK-less stop via JAM(0x02) */
    uint8_t p1[]={0xA9,0x00,0xA2,0x0A,0x86,0x00,0x18,0x65,0x00,0xCA,0xD0,0xF8,0x02};
    run(&c,p1,sizeof p1,200); CHECK(c.a==55,"sum loop"); CHECK(c.jammed,"jam stops");
    /* ADC переполнение: 0x50+0x50 = 0xA0, V=1 N=1 C=0 */
    uint8_t p2[]={0x18,0xA9,0x50,0x69,0x50,0x02};
    run(&c,p2,sizeof p2,20); CHECK(c.a==0xA0 && (c.p&FLAG_V) && (c.p&FLAG_N) && !(c.p&FLAG_C),"adc overflow");
    /* SBC: 0x50-0xF0 с C=1 -> 0x60, C=0, V=0 */
    uint8_t p3[]={0x38,0xA9,0x50,0xE9,0xF0,0x02};
    run(&c,p3,sizeof p3,20); CHECK(c.a==0x60 && !(c.p&FLAG_C) && !(c.p&FLAG_V),"sbc borrow");
    /* JSR/RTS и стек */
    uint8_t p4[]={0x20,0x06,0x80,0xA2,0x07,0x02, 0xA0,0x09,0x60};
    run(&c,p4,sizeof p4,20); CHECK(c.x==7 && c.y==9 && c.s==0xFA,"jsr/rts");
    /* JMP ($80FF) — ошибка страницы: берёт старший байт из $8000 */
    uint8_t p5[0x200]={0}; p5[0]=0x6C; p5[1]=0xFF; p5[2]=0x80; p5[0xFF]=0x34;
    run(&c,p5,sizeof p5,1); CHECK(c.pc==((0x6C<<8)|0x34),"jmp indirect page bug");
    /* (zp),Y с переходом страницы: +1 такт */
    uint8_t p6[]={0xA0,0x10,0xB1,0x40,0x02};
    memset(mem,0,sizeof mem);
    run(&c,p6,sizeof p6,0); mem[0x40]=0xF8; mem[0x41]=0x20; mem[0x2108]=0x77;
    cpu_step(&c); uint64_t c0=c.cycles; cpu_step(&c); int cy=(int)(c.cycles-c0); CHECK(c.a==0x77 && cy==6,"indy page cross");
    /* ROL/ROR через перенос */
    uint8_t p7[]={0x38,0xA9,0x80,0x2A,0x6A,0x02};
    run(&c,p7,sizeof p7,20); CHECK(c.a==0x80 && (c.p&FLAG_C)==1,"rol/ror");
    /* такты: LDA abs,X без/с переходом страницы, STA abs,X, INC abs, JSR, RTS, взятый переход */
    { uint8_t p8[]={0xA2,0x01, 0xBD,0x00,0x30, 0xBD,0xFF,0x30, 0x9D,0x00,0x30, 0xEE,0x00,0x30, 0x20,0x14,0x80, 0xF0,0x00, 0x02, 0x60};
      int want[]={2,4,5,5,6,6,6};
      run(&c,p8,sizeof p8,0);
      for(int i=0;i<7;i++){ uint64_t c0=c.cycles; cpu_step(&c); int cy=(int)(c.cycles-c0);
        char msg[64]; snprintf(msg,sizeof msg,"cycles of instr %d: got %d want %d",i,cy,want[i]); CHECK(cy==want[i],msg);} }
    printf(fails? "%d FAILED\n":"cpu smoke: all ok\n", fails);
    return fails!=0;
}
