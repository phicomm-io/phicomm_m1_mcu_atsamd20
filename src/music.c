/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1/music.h"
#include "m1/memory.h"
_Static_assert(M1_MUSIC_ADDRESS >= M1_STAGING_ADDRESS+0x10100u,"music overlaps OTA staging");
_Static_assert(M1_MUSIC_ADDRESS+2*M1_MUSIC_SLOT <= M1_UPDATE_METADATA_ADDRESS,"music overlaps OTA metadata");
static bool due(uint32_t now,uint32_t at){return (int32_t)(now-at)>=0;}
static bool space(char c){return c==' '||c=='\t'||c=='\r'||c=='\n';}
static void ws(const char *s,size_t n,size_t *p){while(*p<n && space(s[*p]))++*p;}
static bool number(const char *s,size_t n,size_t *p,unsigned *v)
{
    size_t start=*p;*v=0;
    while(*p<n && s[*p]>='0' && s[*p]<='9'){
        *v=*v*10u+(unsigned)(s[(*p)++]-'0');if(*v>65535)return false;
    }
    return *p!=start;
}
static bool duration_ok(unsigned d){return d==1||d==2||d==4||d==8||d==16||d==32;}
static bool header(const char *s,size_t n,size_t *p,uint8_t *d,uint8_t *o,uint16_t *b)
{
    *d=4;*o=6;*b=63;*p=0;
    while(*p<n && s[*p]!=':') {if((unsigned char)s[*p]<32||s[*p]>126||*p>=32)return false;++*p;}
    if(!*p||*p==n)return false;
    ++*p;
    unsigned seen=0;ws(s,n,p);
    while(*p<n && s[*p]!=':'){
        char k=s[(*p)++];unsigned bit=k=='d'?1:k=='o'?2:k=='b'?4:0,v;
        if(!bit||(seen&bit))return false;
        seen|=bit;
        if(*p==n||s[(*p)++]!='='||!number(s,n,p,&v))return false;
        if(k=='d'){if(!duration_ok(v))return false;*d=(uint8_t)v;}
        if(k=='o'){if(v<4||v>7)return false;*o=(uint8_t)v;}
        if(k=='b'){if(v<25||v>900)return false;*b=(uint16_t)v;}
        ws(s,n,p);if(*p<n && s[*p]==','){++*p;ws(s,n,p);if(*p==n||s[*p]==':')return false;}
        else if(*p==n||s[*p]!=':')return false;
    }
    if(*p==n)return false;
    ++*p;ws(s,n,p);return *p<n;
}
static bool note(const char *s,size_t n,size_t *p,unsigned def,unsigned oct,unsigned bpm,uint16_t *hz,uint32_t *ms)
{
    static const uint16_t frequencies[12]={262,277,294,311,330,349,370,392,415,440,466,494};
    ws(s,n,p);unsigned d=def;
    if(*p<n && s[*p]>='0'&&s[*p]<='9' && !number(s,n,p,&d))return false;
    if(!duration_ok(d)||*p==n)return false;
    char c=s[(*p)++];if(c>='A'&&c<='Z')c=(char)(c-'A'+'a');
    int pitch=c=='c'?0:c=='d'?2:c=='e'?4:c=='f'?5:c=='g'?7:c=='a'?9:c=='b'?11:c=='p'?-1:-2;
    if(pitch==-2)return false;
    if(*p<n && s[*p]=='#'){if(pitch<0||pitch==4||pitch==11)return false;++pitch;++*p;}
    bool dotted=false;
    if(*p<n && s[*p]=='.'){dotted=true;++*p;}
    if(*p<n && s[*p]>='0'&&s[*p]<='9'){oct=(unsigned)(s[(*p)++]-'0');if(oct<4||oct>7)return false;}
    if(*p<n && s[*p]=='.'){if(dotted)return false;dotted=true;++*p;}
    ws(s,n,p);if(*p<n){if(s[(*p)++]!=',')return false;ws(s,n,p);if(*p==n)return false;}
    *ms=240000u/(bpm*d);if(dotted)*ms+=*ms/2;
    *hz=pitch<0?0:(uint16_t)(frequencies[pitch]<<(oct-4));return *ms!=0;
}
bool m1_rtttl_validate(const char *s,size_t n,uint32_t *duration)
{
    if(!s||!n||n>M1_MUSIC_MAX)return false;
    for(size_t i=0;i<n;++i)if(!s[i]||((unsigned char)s[i]>126)||((unsigned char)s[i]<32&&!space(s[i])))return false;
    size_t p;uint8_t d,o;uint16_t b,hz;uint32_t total=0,ms;unsigned notes=0;
    if(!header(s,n,&p,&d,&o,&b))return false;
    while(p<n){if(!note(s,n,&p,d,o,b,&hz,&ms)||++notes>256||total+ms>180000)return false;total+=ms;}
    if(!notes)return false;
    *duration=total;return true;
}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint16_t get16(const uint8_t *p){return (uint16_t)((unsigned)p[0]<<8|p[1]);}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(24-i*8));}
static void put16(uint8_t *p,unsigned n){p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static bool zeros(const uint8_t *p,size_t n){while(n--)if(*p++)return false;return true;}
static bool available(const m1_flash *f){return f->read && f->erase && f->write;}
static uint32_t checksum(const char *s,size_t n){return ~m1_crc32((const uint8_t *)s,n,0xffffffffu);}
static bool load_slot(m1_music *m,const m1_flash *f,unsigned slot)
{
    uint8_t h[64];uint32_t base=M1_MUSIC_ADDRESS+slot*M1_MUSIC_SLOT,duration;
    if(!f->read(f->context,base,h,64)||memcmp(h,"RTT1",4))return false;
    unsigned n=get16(h+8);uint32_t crc=get32(h+12),generation=get32(h+4);
    if(!n||n>M1_MUSIC_MAX||get32(h+16)!=~crc||get32(h+20)!=~generation)return false;
    if(!f->read(f->context,base+64,(uint8_t *)m->pending,n)||checksum(m->pending,n)!=crc||
       !m1_rtttl_validate(m->pending,n,&duration))return false;
    if(!m->present || (int32_t)(generation-m->generation)>0){
        memcpy(m->text,m->pending,n);m->text[n]=0;m->length=(uint16_t)n;m->duration=duration;
        m->generation=generation;m->slot=(uint8_t)slot;m->crc=crc;m->present=true;
    }
    return true;
}
void m1_music_load(m1_music *m,const m1_flash *f)
{
    memset(m,0,sizeof(*m));if(!available(f))return;
    (void)load_slot(m,f,0);(void)load_slot(m,f,1);
}
static bool commit(m1_music *m,const m1_flash *f)
{
    unsigned slot=m->present?1u-m->slot:0;uint32_t base=M1_MUSIC_ADDRESS+slot*M1_MUSIC_SLOT;
    uint8_t page[64],check[64];
    for(unsigned i=0;i<M1_MUSIC_SLOT;i+=256)if(!f->erase(f->context,base+i))return false;
    for(unsigned i=0;i<m->expected;i+=64){
        memset(page,255,64);unsigned n=m->expected-i;if(n>64)n=64;memcpy(page,m->pending+i,n);
        if(!f->write(f->context,base+64+i,page)||!f->read(f->context,base+64+i,check,64)||memcmp(page,check,64))return false;
    }
    memset(page,255,64);memcpy(page,"RTT1",4);put32(page+4,m->generation+1);put16(page+8,m->expected);
    put32(page+12,m->upload_crc);put32(page+16,~m->upload_crc);put32(page+20,~(m->generation+1));
    if(!f->write(f->context,base,page)||!f->read(f->context,base,check,64)||memcmp(page,check,64))return false;
    /* Header is the final commit point. Old slot is never modified. */
    return load_slot(m,f,slot);
}
void m1_music_stop(m1_music *m){m->playing=false;}
uint8_t m1_music_command(m1_music *m,const m1_flash *f,const uint8_t a[7],uint8_t out[14],uint32_t now)
{
    if(!available(f))return 3;
    if(m->uploading && (uint32_t)(now-m->upload_at)>=30000)m->uploading=false;
    unsigned offset=get16(a+1),n;
    switch(a[0]){
    case 0:
        if(!zeros(a+1,6))return 1;
        out[0]=m->present;out[1]=m->playing;put16(out+2,m->length);put32(out+4,m->crc);
        put32(out+8,m->duration);out[12]=m->uploading;out[13]=1;return 0;
    case 1:
        if(!offset||offset>M1_MUSIC_MAX)return 1;
        if(m->playing||m->uploading)return 2;
        m->expected=(uint16_t)offset;m->upload_crc=get32(a+3);m->received=0;m->upload_at=now;m->uploading=true;return 0;
    case 2:
        if(!m->uploading)return 2;
        if(offset>m->received||offset>=m->expected)return 1;
        n=m->expected-offset;if(n>4)n=4;
        if(!zeros(a+3+n,4-n))return 1;
        if(offset<m->received){if(offset+n>m->received||memcmp(m->pending+offset,a+3,n))return 1;}
        else {memcpy(m->pending+offset,a+3,n);m->received=(uint16_t)(m->received+n);}
        m->upload_at=now;put16(out,m->received);return 0;
    case 3:{
        uint32_t duration;
        if(!zeros(a+1,6))return 1;
        if(!m->uploading||m->received!=m->expected)return 2;
        if(checksum(m->pending,m->expected)!=m->upload_crc || !m1_rtttl_validate(m->pending,m->expected,&duration))return 1;
        bool ok=commit(m,f);m->uploading=false;return ok?0:4;
    }
    case 4:
        n=a[3];if(!zeros(a+4,3)||!n||n>10||offset>m->length||n>m->length-offset)return 1;
        if(!m->present)return 3;
        put16(out,offset);out[2]=(uint8_t)n;memcpy(out+3,m->text+offset,n);return 0;
    case 5:{
        size_t p;
        if(!offset||offset>180||a[3]>1||!zeros(a+4,3))return 1;
        if(!m->present)return 3;
        if(m->uploading)return 2;
        if(!header(m->text,m->length,&p,&m->denominator,&m->octave,&m->bpm))return 1;
        m->position=(uint16_t)p;m->playing=true;m->repeat=a[3]!=0;m->until=now+offset*1000u;m->note_at=now;return 0;
    }
    case 6:if(!zeros(a+1,6))return 1;m1_music_stop(m);return 0;
    case 7:if(!zeros(a+1,6))return 1;m->uploading=false;return 0;
    default:return 1;
    }
}
unsigned m1_music_tick(m1_music *m,uint32_t now,uint16_t *hz)
{
    if(m->uploading && (uint32_t)(now-m->upload_at)>=30000)m->uploading=false;
    if(!m->playing)return 0;
    if(due(now,m->until)){m->playing=false;return 0;}
    if(!due(now,m->note_at))return 1;
    size_t p=m->position;
    if(p>=m->length){
        if(!m->repeat){m->playing=false;return 0;}
        if(!header(m->text,m->length,&p,&m->denominator,&m->octave,&m->bpm)){m->playing=false;return 0;}
    }
    uint32_t ms;
    if(!note(m->text,m->length,&p,m->denominator,m->octave,m->bpm,hz,&ms)){m->playing=false;return 0;}
    m->position=(uint16_t)p;m->note_at=now+ms;return 2;
}
