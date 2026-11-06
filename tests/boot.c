/* boot.c — запуск игры без записи: N кадров, нажатия по простому сценарию, сохранение картинок.
 *   boot <rom> <кадров> <png_каждые_N> <папка> [сценарий: кадр:кнопки,...]  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/nes.h"
static uint8_t *read_file(const char *p, long *s){FILE*f=fopen(p,"rb");if(!f)return 0;fseek(f,0,2);*s=ftell(f);fseek(f,0,0);uint8_t*d=malloc(*s);if(fread(d,1,*s,f)!=(size_t)*s){fclose(f);return 0;}fclose(f);return d;}
static void save_ppm(Nes *n,const char*dir,int fr){char p[512];snprintf(p,sizeof p,"%s/boot_%06d.ppm",dir,fr);FILE*f=fopen(p,"wb");if(!f)return;fprintf(f,"P6 256 240 255\n");for(int i=0;i<256*240;i++){uint32_t c=NES_PALETTE[n->ppu.framebuffer[i]&63];uint8_t rgb[3]={c>>16,c>>8,c};fwrite(rgb,1,3,f);}fclose(f);}
int main(int argc,char**argv){
    long sz; uint8_t*rom=read_file(argv[1],&sz); int frames=atoi(argv[2]), every=atoi(argv[3]); const char*dir=argv[4];
    const char*script=argc>5?argv[5]:"";
    Nes*n=calloc(1,sizeof(Nes)); int e=nes_load_rom(n,rom,(int)sz); if(e){printf("load err %d\n",e);return 1;}
    nes_reset(n);
    for(int f=1;f<=frames;f++){
        /* сценарий "120:08,130:00" = в кадре 120 нажать Start(0x08), в 130 отпустить */
        const char*s=script; while(*s){int fr=0,b=0; if(sscanf(s,"%d:%x",&fr,&b)==2 && fr==f) n->pad_state=(uint8_t)b; const char*c=strchr(s,','); if(!c)break; s=c+1;}
        nes_run_frame(n);
        if(every && f%every==0) save_ppm(n,dir,f);
        if(n->cpu.jammed){printf("CPU JAMMED at frame %d pc=%04X op=%02X bank=%d\n",f,n->cpu.pc,nes_read(n,n->cpu.pc),n->bank);break;}
    }
    printf("done frame=%llu pc=%04X bank=%d ctrl=%02X mask=%02X\n",(unsigned long long)n->frame,n->cpu.pc,n->bank,n->ppu.ctrl,n->ppu.mask);
    return 0;
}
