/* SPDX-License-Identifier: MIT */
#include "m1/update.h"
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]<<8|p[1]); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static bool equal(const uint8_t *a,const uint8_t *b,size_t n)
{ for(size_t i=0;i<n;++i) if(a[i]!=b[i]) return false; return true; }
uint32_t m1_crc32(const uint8_t *p,size_t n,uint32_t crc)
{
    while(n--) { crc^=*p++; for(unsigned i=0;i<8;++i) crc=(crc>>1)^((crc&1)?0xedb88320u:0); }
    return crc;
}
bool m1_image_vector_valid(const uint8_t v[8],uint32_t length)
{
    uint32_t sp=le32(v),pc=le32(v+4);
    return length>=164 && length<=M1_MAX_IMAGE && sp>0x20000000 && sp<=0x20008000 &&
        !(sp&7) && (pc&1) && (pc&~1u)>=M1_APPLICATION_ADDRESS &&
        (pc&~1u)+2<=M1_APPLICATION_ADDRESS+length;
}
bool m1_update_begin(m1_update *u,uint8_t full,uint8_t tail,uint32_t now)
{
    uint32_t n=(uint32_t)full*256+tail;
    if(n<164 || n>M1_MAX_IMAGE) return false;
    *u=(m1_update){.length=n,.last_activity=now,.next_block=1,.full_blocks=full,.tail=tail,.active=true};
    return true;
}
size_t m1_update_frame_size(const m1_update *u,uint16_t index)
{
    if(!u->active || index<1 || index>(unsigned)u->full_blocks+1) return 0;
    return index<=u->full_blocks?262:(size_t)u->tail+6;
}
void m1_update_abort(m1_update *u) { *u=(m1_update){0}; }
void m1_update_block(m1_update *u,const m1_flash *f,const uint8_t *p,size_t size,
                     uint32_t now,uint8_t reply[20])
{
    m1_event(0x14,reply);
    if(size<6) return;
    uint16_t block=be16(p+2); reply[2]=p[2]; reply[3]=p[3];
    if(p[0]!=0x23 || p[1]!=0x13 || size!=m1_update_frame_size(u,block)) return;
    size_t n=size-6; uint16_t crc=0;
    for(size_t i=0;i<n;++i) crc=m1_stock_crc_byte(crc,p[i+4]);
    if(crc!=be16(p+size-2)) { reply[5]=1; return; }
    uint32_t at=M1_STAGING_ADDRESS+(uint32_t)(block-1)*256;
    uint8_t page[64],check[64];
    if(block<u->next_block) {
        for(size_t i=0;i<n;i+=64) {
            size_t k=n-i<64?n-i:64;
            if(!f->read(f->context,at+(uint32_t)i,check,k)||!equal(check,p+4+i,k)) return;
        }
        reply[4]=1; reply[5]=2; u->last_activity=now; return;
    }
    if(block!=u->next_block || u->complete) { reply[5]=2; return; }
    if(block==1 && !f->erase(f->context,M1_UPDATE_METADATA_ADDRESS)) return;
    if(n && !f->erase(f->context,at)) return;
    for(size_t i=0;i<n;i+=64) {
        size_t k=n-i<64?n-i:64;
        for(unsigned j=0;j<64;++j) page[j]=j<k?p[4+i+j]:0xff;
        if(!f->write(f->context,at+(uint32_t)i,page) ||
           !f->read(f->context,at+(uint32_t)i,check,64) || !equal(page,check,64)) return;
    }
    u->next_block++; u->last_activity=now; reply[4]=1;
    if(block==(unsigned)u->full_blocks+1) u->complete=true;
}
static bool image_crc(const m1_flash *f,uint32_t base,uint32_t length,uint32_t *out)
{
    uint8_t p[64]; uint32_t crc=0xffffffff;
    for(uint32_t i=0;i<length;i+=64) {
        size_t n=length-i<64?length-i:64;
        if(!f->read(f->context,base+i,p,n)) return false;
        crc=m1_crc32(p,n,crc);
    }
    *out=~crc; return true;
}
bool m1_update_commit(m1_update *u,const m1_flash *f)
{
    uint8_t page[64],check[64],v[8]; uint32_t crc;
    if(!u->active || !u->complete || !f->read(f->context,M1_STAGING_ADDRESS,v,8) ||
       !m1_image_vector_valid(v,u->length) || !image_crc(f,M1_STAGING_ADDRESS,u->length,&crc)) return false;
    /* Original Boot copies (length/64+4) pages. Keep that padding erased. */
    uint32_t end=(u->length/64+4)*64;
    for(uint32_t i=(u->length+255)&~255u;i<end;i+=256)
        if(!f->erase(f->context,M1_STAGING_ADDRESS+i)) return false;
    /* Invalidate the old transaction before creating a new journal. */
    if(!f->erase(f->context,M1_UPDATE_METADATA_ADDRESS)) return false;
    for(unsigned i=0;i<64;++i) page[i]=0xff;
    page[0]='M'; page[1]='1'; page[2]='U'; page[3]='P';
    put32(page+4,u->length); put32(page+8,crc); put32(page+12,~crc);
    put32(page+16,~u->length);
    if(!f->write(f->context,M1_UPDATE_METADATA_ADDRESS+64,page) ||
       !f->read(f->context,M1_UPDATE_METADATA_ADDRESS+64,check,64)||!equal(page,check,64)) return false;
    for(unsigned i=0;i<64;++i) page[i]=0xff;
    page[0]=1; page[1]=(uint8_t)(u->length>>8); page[2]=(uint8_t)u->length;
    if(!f->write(f->context,M1_UPDATE_METADATA_ADDRESS,page) ||
       !f->read(f->context,M1_UPDATE_METADATA_ADDRESS,check,64)||!equal(page,check,64)) return false;
    u->active=false; return true;
}
m1_boot_result m1_boot_apply(const m1_flash *f)
{
    uint8_t meta[64],journal[64],page[64],check[64]; uint32_t crc;
    if(!f->read(f->context,M1_UPDATE_METADATA_ADDRESS,meta,64)) return M1_BOOT_IO_ERROR;
    if(meta[0]==0xff || meta[0]==0) return M1_BOOT_NORMAL;
    if(meta[0]!=1) return M1_BOOT_INVALID;
    uint32_t n=be16(meta+1);
    if(!f->read(f->context,M1_UPDATE_METADATA_ADDRESS+64,journal,64) ||
       !f->read(f->context,M1_STAGING_ADDRESS,page,8)) return M1_BOOT_IO_ERROR;
    if(!equal(journal,(const uint8_t *)"M1UP",4)||le32(journal+4)!=n||le32(journal+16)!=~n||
       le32(journal+12)!=~le32(journal+8)||!m1_image_vector_valid(page,n)) return M1_BOOT_INVALID;
    if(!image_crc(f,M1_STAGING_ADDRESS,n,&crc)) return M1_BOOT_IO_ERROR;
    if(crc!=le32(journal+8)) return M1_BOOT_INVALID;
    /* Keep the committed journal intact. CRC equality identifies an already
     * installed transaction without programming a previously written page. */
    uint32_t installed;
    if(!image_crc(f,M1_APPLICATION_ADDRESS,n,&installed)) return M1_BOOT_IO_ERROR;
    if(installed==crc) return M1_BOOT_NORMAL;
    for(uint32_t row=0;row<n;row+=256) {
        if(!f->erase(f->context,M1_APPLICATION_ADDRESS+row)) return M1_BOOT_IO_ERROR;
        for(uint32_t p=0;p<256;p+=64) {
            for(unsigned j=0;j<64;++j) page[j]=0xff;
            size_t k=n>row+p?n-row-p:0; if(k>64) k=64;
            if(k && !f->read(f->context,M1_STAGING_ADDRESS+row+p,page,k)) return M1_BOOT_IO_ERROR;
            if(!f->write(f->context,M1_APPLICATION_ADDRESS+row+p,page) ||
               !f->read(f->context,M1_APPLICATION_ADDRESS+row+p,check,64)||!equal(page,check,64)) return M1_BOOT_IO_ERROR;
        }
    }
    if(!image_crc(f,M1_APPLICATION_ADDRESS,n,&crc)) return M1_BOOT_IO_ERROR;
    if(crc!=le32(journal+8)) return M1_BOOT_INVALID;
    /* No completion erase or second programming of the metadata page. A reset
     * mid-copy restarts from staging; a completed copy is recognized by CRC. */
    return M1_BOOT_UPDATED;
}
