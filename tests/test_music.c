/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1/music.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t flash[4096],saved[4096],written[64];
static unsigned operations,fail_at;static bool torn;
static bool fail(void){return ++operations==fail_at;}
static bool rd(void *c,uint32_t at,uint8_t *p,size_t n)
{(void)c;assert(at>=M1_MUSIC_ADDRESS && at+n<=M1_MUSIC_ADDRESS+4096);if(fail())return false;memcpy(p,flash+at-M1_MUSIC_ADDRESS,n);return true;}
static bool er(void *c,uint32_t at)
{(void)c;assert(at%256==0 && at>=M1_MUSIC_ADDRESS && at+256<=M1_MUSIC_ADDRESS+4096);unsigned i=at-M1_MUSIC_ADDRESS;if(fail()){if(torn){memset(flash+i,255,128);memset(written+i/64,0,2);}return false;}memset(flash+i,255,256);memset(written+i/64,0,4);return true;}
static bool wr(void *c,uint32_t at,const uint8_t *p)
{(void)c;assert(at%64==0 && at>=M1_MUSIC_ADDRESS && at+64<=M1_MUSIC_ADDRESS+4096);unsigned k=at-M1_MUSIC_ADDRESS;bool cut=fail();unsigned n=cut?(torn?32:0):64;if(n){assert(!written[k/64]);written[k/64]=1;}for(unsigned i=0;i<n;i++){assert((flash[k+i]&p[i])==p[i]);flash[k+i]&=p[i];}return !cut;}
static m1_flash f={0,rd,er,wr};
static const char first[]="Test:d=4,o=5,b=120:c,p,8d.,c#6";
static const char second[]="Second:d=8,o=4,b=240:a,b,c5,p";
static uint8_t call(m1_music *m,unsigned op,unsigned offset,const uint8_t *data,unsigned n,uint32_t now)
{uint8_t a[7]={0},out[14]={0};a[0]=(uint8_t)op;a[1]=(uint8_t)(offset>>8);a[2]=(uint8_t)offset;if(n)memcpy(a+3,data,n);return m1_music_command(m,&f,a,out,now);}
static void upload(m1_music *m,const char *text)
{
 unsigned n=strlen(text);uint32_t crc=~m1_crc32((const uint8_t *)text,n,0xffffffff);uint8_t d[4]={(uint8_t)(crc>>24),(uint8_t)(crc>>16),(uint8_t)(crc>>8),(uint8_t)crc};
 assert(call(m,1,n,d,4,1)==0);
 assert(call(m,2,1,(const uint8_t *)text,4,2)==1); /* holes rejected */
 for(unsigned i=0;i<n;i+=4){unsigned k=n-i;if(k>4)k=4;assert(call(m,2,i,(const uint8_t *)text+i,k,2+i)==0);assert(call(m,2,i,(const uint8_t *)text+i,k,3+i)==0);}
}
int main(void)
{
 uint32_t ms;assert(m1_rtttl_validate(first,strlen(first),&ms)&&ms==1875);
 assert(m1_rtttl_validate("Defaults::c",11,&ms));
 const char *bad[]={"","x::","x:d=0:c","x:d=3:c","x:o=3:c","x:o=8:c","x:b=0:c","x:b=901:c","x:d=4,d=8:c","x:d=4,:c","x::c,","x::c,,d","x::h","x::e#","x::b#","x::p#","x::c3","x::c8","x::c..","x::c45","x::0c","x::999999999999c","x:b=25:1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!m1_rtttl_validate(bad[i],strlen(bad[i]),&ms));
 memset(flash,255,sizeof(flash));m1_music m; m1_music_load(&m,&f);assert(!m.present);
 upload(&m,first);assert(call(&m,3,0,0,0,100)==0&&m.present);assert(!strcmp(m.text,first));
 m1_music reload;m1_music_load(&reload,&f);assert(reload.present&&!strcmp(reload.text,first));
 uint8_t play[1]={0};uint16_t hz;
 assert(call(&m,5,10,play,1,0)==0);assert(m1_music_tick(&m,0,&hz)==2&&hz==524);
 assert(m1_music_tick(&m,499,&hz)==1);assert(m1_music_tick(&m,500,&hz)==2&&hz==0);
 assert(m1_music_tick(&m,1000,&hz)==2&&hz==588);assert(m1_music_tick(&m,1375,&hz)==2&&hz==1108);
 assert(m1_music_tick(&m,1875,&hz)==0&&!m.playing);
 play[0]=1;assert(call(&m,5,2,play,1,UINT32_MAX-500)==0);
 assert(m1_music_tick(&m,UINT32_MAX-500,&hz)==2);assert(m1_music_tick(&m,1499,&hz)==0);
 upload(&m,second);assert(call(&m,3,0,0,0,100)==0);assert(!strcmp(m.text,second));
 /* Every flash read/erase/program failure, including partial erase/write,
  * leaves either the previous song or a completely validated replacement. */
 memcpy(saved,flash,sizeof(flash));upload(&m,first);operations=0;assert(call(&m,3,0,0,0,200)==0);unsigned count=operations;
 for(unsigned t=0;t<2;t++)for(unsigned cut=1;cut<=count;cut++){
  memcpy(flash,saved,sizeof(flash));memset(written,1,sizeof(written));fail_at=0;m1_music_load(&m,&f);upload(&m,first);operations=0;fail_at=cut;torn=t;
  (void)call(&m,3,0,0,0,200);fail_at=0;m1_music_load(&reload,&f);
  assert(reload.present&&(!strcmp(reload.text,second)||!strcmp(reload.text,first)));
 }
 fail_at=0;m1_music_load(&m,&f);upload(&m,second);m.pending[0]^=1;assert(call(&m,3,0,0,0,100)==1);assert(call(&m,7,0,0,0,101)==0);
 upload(&m,second);assert(call(&m,3,0,0,0,40000)==2&&!m.uploading);
 /* Bounded arbitrary input parse smoke test under ASan/UBSan. */
 char fuzz[1024];uint32_t random=1;for(unsigned i=0;i<10000;i++){unsigned n=i%1024+1;for(unsigned j=0;j<n;j++){random=random*1664525+1013904223;fuzz[j]=(char)(random>>24);} (void)m1_rtttl_validate(fuzz,n,&ms);}
 puts("music: RTTTL timing/rests/bounds/wrap, upload CRC/sequence/retry, dual-slot persistence and every flash cut PASS");
}
