/* SPDX-License-Identifier: MIT */
#include "m1/runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint8_t data[0x40000], programmed[0x40000/64]; unsigned operations,fail_at; bool torn; } device;
static device d,committed,received;
static unsigned cases;
static bool fail(device *v) { return ++v->operations==v->fail_at; }
static bool read_flash(void *ctx,uint32_t at,uint8_t *p,size_t n)
{
    device *v=ctx; assert(at<=sizeof(v->data) && n<=sizeof(v->data)-at);
    if(fail(v)) return false;
    memcpy(p,v->data+at,n); return true;
}
static bool erase_flash(void *ctx,uint32_t at)
{
    device *v=ctx; assert(!(at&255) && at>=0x4000 && at+256<=sizeof(v->data));
    if(fail(v)) { if(v->torn) { memset(v->data+at,255,128); memset(v->programmed+at/64,0,2); } return false; }
    memset(v->data+at,255,256); memset(v->programmed+at/64,0,4); return true;
}
static bool write_flash(void *ctx,uint32_t at,const uint8_t p[64])
{
    device *v=ctx; assert(!(at&63) && at>=0x4000 && at+64<=sizeof(v->data));
    bool cut=fail(v); unsigned n=cut?(v->torn?32:0):64;
    if(n) { assert(!v->programmed[at/64]); v->programmed[at/64]=1; }
    for(unsigned i=0;i<n;++i) { assert((v->data[at+i]&p[i])==p[i]); v->data[at+i]&=p[i]; }
    return !cut;
}
static m1_flash io={&d,read_flash,erase_flash,write_flash};
static uint8_t image[65535];
static void image_init(size_t n)
{
    for(size_t i=0;i<n;++i) image[i]=(uint8_t)(i*13+9);
    const uint8_t vector[8]={0,0x80,0,0x20,0x41,0x40,0,0}; memcpy(image,vector,8);
}
static size_t block(uint8_t *p,unsigned index,size_t n)
{
    p[0]=0x23;p[1]=0x13;p[2]=(uint8_t)(index>>8);p[3]=(uint8_t)index;
    size_t offset=(index-1)*256,k=n>offset?n-offset:0;if(k>256) k=256;
    memcpy(p+4,image+offset,k);uint16_t crc=0;
    for(size_t i=0;i<k;++i) crc=m1_stock_crc_byte(crc,p[i+4]);
    p[k+4]=(uint8_t)(crc>>8);p[k+5]=(uint8_t)crc;return k+6;
}
static void receive(m1_update *u,size_t n)
{
    assert(m1_update_begin(u,(uint8_t)(n/256),(uint8_t)(n%256),1));
    uint8_t p[262],reply[20];
    for(unsigned i=1;i<=n/256+1;++i) {
        size_t k=block(p,i,n);m1_update_block(u,&io,p,k,2+i,reply);assert(reply[4]==1);
    }
    assert(u->complete);
}
static void fresh(size_t n)
{
    memset(&d,0,sizeof(d));memset(d.data,255,sizeof(d.data));memset(d.data+0x4000,0x5a,65536);memset(d.programmed+0x4000/64,1,65536/64);
    image_init(n);
}
static void transaction(size_t n)
{
    fresh(n);m1_update u={0};receive(&u,n);assert(!memcmp(d.data+0x22000,image,n));
    assert(m1_update_commit(&u,&io));assert(!u.active);
    assert(d.data[0x3ff00]==1);assert(m1_boot_apply(&io)==M1_BOOT_UPDATED);
    assert(!memcmp(d.data+0x4000,image,n));assert(d.data[0x3ff00]==1);
    assert(m1_boot_apply(&io)==M1_BOOT_NORMAL);cases++;
}
static void fault_injection(void)
{
    const size_t n=777;fresh(n);m1_update u={0};receive(&u,n);received=d;
    assert(m1_update_commit(&u,&io));committed=d;
    d.operations=0;assert(m1_boot_apply(&io)==M1_BOOT_UPDATED);unsigned ops=d.operations;
    for(unsigned torn=0;torn<2;++torn) for(unsigned cut=1;cut<=ops;++cut) {
        d=committed;d.operations=0;d.fail_at=cut;d.torn=torn;
        m1_boot_result first=m1_boot_apply(&io);assert(first==M1_BOOT_IO_ERROR);
        /* A reset loses all RAM state; only Flash survives. */
        d.fail_at=0;d.operations=0;
        m1_boot_result again=m1_boot_apply(&io);assert(again==M1_BOOT_UPDATED || again==M1_BOOT_NORMAL);
        assert(!memcmp(d.data+0x4000,image,n));cases++;
    }
    d=received;u=(m1_update){0};assert(m1_update_begin(&u,3,9,1));u.complete=true;
    d.operations=0;assert(m1_update_commit(&u,&io));ops=d.operations;
    for(unsigned torn=0;torn<2;++torn) for(unsigned cut=1;cut<=ops;++cut) {
        d=received;d.operations=0;d.fail_at=cut;d.torn=torn;
        m1_update transaction={.length=n,.active=true,.complete=true};
        assert(!m1_update_commit(&transaction,&io));d.fail_at=0;d.operations=0;
        m1_boot_result boot=m1_boot_apply(&io);
        assert(boot==M1_BOOT_NORMAL || boot==M1_BOOT_UPDATED || boot==M1_BOOT_INVALID);
        if(boot==M1_BOOT_UPDATED) assert(!memcmp(d.data+0x4000,image,n));
        else for(unsigned j=0;j<65536;++j) assert(d.data[0x4000+j]==0x5a);
        cases++;
    }
    /* Test a failed receive operation and resend, at each Flash I/O point. */
    fresh(300);uint8_t p[262],ack[20];size_t size=block(p,1,300);
    assert(m1_update_begin(&u,1,44,0));d.operations=0;m1_update_block(&u,&io,p,size,1,ack);ops=d.operations;
    for(unsigned torn=0;torn<2;++torn) for(unsigned cut=1;cut<=ops;++cut) {
        fresh(300);assert(m1_update_begin(&u,1,44,0));d.operations=0;d.fail_at=cut;d.torn=torn;
        m1_update_block(&u,&io,p,size,1,ack);assert(ack[4]==0);
        d.fail_at=0;m1_update_block(&u,&io,p,size,2,ack);assert(ack[4]==1);
        assert(!memcmp(d.data+0x22000,image,256));cases++;
    }
}
static uint8_t last_reply[20];static unsigned resets;static bool wifi_on;static unsigned wifi_low;
static bool uart(void *ctx,m1_channel channel,const uint8_t *p,size_t n)
{ (void)ctx;if(channel==M1_LINK) { assert(n==20);memcpy(last_reply,p,20); }return true; }
static bool i2c(void *c,unsigned b,uint8_t a,const uint8_t *t,size_t nt,uint8_t *rx,size_t nr)
{ (void)c;(void)b;(void)a;(void)t;(void)nt;(void)rx;(void)nr;return false; }
static bool rtc_read(void *c,m1_clock *v) { (void)c;(void)v;return false; }
static bool rtc_write(void *c,const m1_clock *v) { (void)c;(void)v;return false; }
static void buzzer(void *c,bool on) { (void)c;(void)on; }
static void wifi(void *c,bool enabled) { (void)c;wifi_on=enabled;if(!enabled) wifi_low++; }
static void reset(void *c) { (void)c;resets++; }
static void wire(m1_runtime *r,const uint8_t *p,size_t n,uint32_t now)
{ for(size_t i=0;i<n;++i) m1_runtime_rx(r,M1_LINK,p[i],now); }
static void wire_update(size_t length)
{
    fresh(length);m1_runtime r;m1_io callbacks={.uart=uart,.i2c=i2c,.rtc_read=rtc_read,.rtc_write=rtc_write,.buzzer=buzzer,.reset=reset,.wifi_enable=wifi,.flash=io};
    m1_runtime_init(&r,&callbacks,0);assert(last_reply[1]==0x17);
    uint8_t cmd[12]={0x23,0x12,(uint8_t)(length/256),(uint8_t)length,0,0,0,0,0,0,0,0x21};
    wire(&r,cmd,12,1);assert(r.update.active && last_reply[1]==0x14);
    uint8_t p[262];size_t n=block(p,1,length);
    wire(&r,p,10,2);wire(&r,p,n,103);assert(last_reply[1]==0x14 && last_reply[4]==1);
    wire(&r,p,n,104);assert(last_reply[1]==0x14 && last_reply[4]==1 && last_reply[5]==2);
    for(unsigned i=2;i<=length/256+1;++i) { n=block(p,i,length);wire(&r,p,n,104+i); }
    assert(r.update.complete && last_reply[1]==0x15 && last_reply[2]==1);
    cmd[1]=0x16;cmd[2]=0;wire(&r,cmd,12,500);assert(r.reset_pending);
    unsigned before=resets,low=wifi_low;assert(wifi_on);
    m1_runtime_poll(&r,4499,false);assert(resets==before && wifi_on);
    m1_runtime_poll(&r,4500,false);assert(resets==before && !wifi_on && wifi_low==low+1);
    /* Pending reset cannot be cancelled or replaced by a second transaction. */
    cmd[1]=0x12;wire(&r,cmd,12,4600);assert(!r.update.active);
    m1_runtime_poll(&r,5499,false);assert(resets==before);
    m1_runtime_poll(&r,5500,false);assert(resets==before+1 && wifi_low==low+1);
    assert(m1_boot_apply(&io)==M1_BOOT_UPDATED);assert(!memcmp(d.data+0x4000,image,length));cases++;
    /* An explicit cancellation must never submit metadata or reset. */
    m1_runtime_init(&r,&callbacks,1000);cmd[1]=0x12;cmd[2]=1;cmd[3]=0;wire(&r,cmd,12,1001);
    cmd[1]=0x16;cmd[2]=1;wire(&r,cmd,12,1002);
    assert(!r.update.active && !r.reset_pending && last_reply[1]==0x1a);
}
static void reset_timing(void)
{
    fresh(500);m1_runtime r;m1_io callbacks={.uart=uart,.i2c=i2c,.rtc_read=rtc_read,.rtc_write=rtc_write,.buzzer=buzzer,.reset=reset,.wifi_enable=wifi,.flash=io};
    uint32_t start=UINT32_MAX-2000u;
    m1_runtime_init(&r,&callbacks,start);
    uint8_t cmd[12]={0x23,0x16,0,0,0,0,0,0,0,0,0,0x21};
    unsigned low=wifi_low,before=resets;
    wire(&r,cmd,12,start);assert(!r.reset_pending && wifi_on && wifi_low==low);
    receive(&r.update,500);wire(&r,cmd,12,start);assert(r.reset_pending);
    m1_runtime_poll(&r,start+3999u,false);assert(wifi_on && resets==before);
    /* A late main-loop poll must still hold CHIP_EN low for a whole second. */
    m1_runtime_poll(&r,start+4500u,false);assert(!wifi_on && wifi_low==low+1);
    m1_runtime_poll(&r,start+5499u,false);assert(resets==before);
    m1_runtime_poll(&r,start+5500u,false);assert(resets==before+1);
}
static void built_image(const char *path)
{
    uint8_t payload[65536];FILE *file=fopen(path,"rb");assert(file);
    size_t n=fread(payload,1,sizeof(payload),file);assert(!ferror(file) && feof(file));fclose(file);
    assert(n>=164 && n<=65535);fresh(n);memcpy(image,payload,n);
    m1_update u={0};receive(&u,n);assert(m1_update_commit(&u,&io));
    assert(m1_boot_apply(&io)==M1_BOOT_UPDATED);assert(!memcmp(d.data+0x4000,payload,n));
    printf("built OTA image: %zu bytes received/committed/installed byte-for-byte PASS\n",n);
}
int main(int argc,char **argv)
{
    assert(~m1_crc32((const uint8_t *)"123456789",9,0xffffffff)==0xcbf43926);
    for(size_t n=164;n<=520;++n) transaction(n);
    transaction(65535);transaction(65534);transaction(65280);
    fresh(512);m1_update u={0};uint8_t p[262],ack[20];
    assert(!m1_update_begin(&u,0,163,0));assert(m1_update_begin(&u,2,0,0));
    size_t n=block(p,2,512);m1_update_block(&u,&io,p,n,1,ack);assert(ack[4]==0 && ack[5]==2);
    n=block(p,1,512);p[n-1]^=1;m1_update_block(&u,&io,p,n,1,ack);assert(ack[4]==0 && ack[5]==1);
    p[n-1]^=1;m1_update_block(&u,&io,p,n,2,ack);assert(ack[4]==1);
    m1_update_block(&u,&io,p,n,3,ack);assert(ack[4]==1 && ack[5]==2);
    p[8]^=1;m1_update_block(&u,&io,p,n,4,ack);assert(ack[4]==0);
    assert(!m1_update_commit(&u,&io));m1_update_abort(&u);assert(!u.active);
    fault_injection();wire_update(500);wire_update(512);reset_timing();
    d=committed;d.data[0x22020]^=1;assert(m1_boot_apply(&io)==M1_BOOT_INVALID);
    d=committed;d.data[0x3ff48]^=1;assert(m1_boot_apply(&io)==M1_BOOT_INVALID);
    d=committed;d.data[0x3ff00]=7;assert(m1_boot_apply(&io)==M1_BOOT_INVALID);
    if(argc==2) built_image(argv[1]);
    puts("update: sequence/CRC/size/duplicate checks PASS");printf("update: %u transactions and interruption/retry cases PASS\n",cases);
}
