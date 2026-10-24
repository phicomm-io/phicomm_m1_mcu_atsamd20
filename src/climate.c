/* SPDX-License-Identifier: MIT */
#include "m1/climate.h"
#include <limits.h>
/* Arithmetic floor shift, including negative values, without signed shifts. */
static int64_t shr(int64_t n, unsigned bits)
{ return n>=0 ? n/(INT64_C(1)<<bits) : -1-((-1-n)/(INT64_C(1)<<bits)); }
/* Stock uses low 64 bits of a product. Express wrap explicitly, never C signed overflow. */
static int64_t wrapmul(int64_t a,int64_t b)
{
    uint64_t u=(uint64_t)a*(uint64_t)b;
    return u<=INT64_MAX?(int64_t)u:-1-(int64_t)(UINT64_MAX-u);
}
static int64_t mulq(int64_t a, int64_t b) { return shr(a*b,19); }
bool m1_climate_convert(m1_climate *m, uint16_t rt, uint16_t rh, uint32_t aux,
                        bool display, int32_t *temperature, uint32_t *humidity)
{
    /* Four-state thermal model, coefficients and Q shifts recovered from
     * 0x58c4. Keep state in the same integer domain; no vendor code executes. */
    static const int32_t a[4][4] = {
        {71487,150048,89825,41398}, {150048,351971,188538,-47309},
        {89825,188538,112867,52018}, {41398,-47309,52018,510391}};
    static const int32_t offset[4] = {-8255252,-7073093,-7075018,-8256213};
    static const int32_t heat[4] = {0, -71621,0,259590};
    for (unsigned i=0;i<4;++i)
        if (m->x[i]<-100000000 || m->x[i]>100000000) return false;
    if (m->heat < -100000 || m->heat>100000 || m->aux < -100000 || m->aux>100000) return false;
    int64_t input=display?1000:0;
    if (m->initialized) {
        int64_t old[4]; for (unsigned i=0;i<4;++i) old[i]=m->x[i];
        for (unsigned i=0;i<4;++i) {
            int64_t next=offset[i]+mulq(m->heat,i==0||i==3?INT64_C(0x70000000):INT64_C(0x60000000));
            for (unsigned j=0;j<4;++j) next+=mulq(old[j],a[i][j]);
            next+=mulq(input,heat[i]);
            /* Reject corrupt/pathological state instead of overflowing on the next call. */
            if (next < -100000000 || next > 100000000) return false;
            m->x[i]=next;
        }
        m->heat=shr(m->aux*8,19)+2304;
        m->aux=shr((int64_t)aux*6,19)-8;
    } else {
        m->initialized=true; m->heat=2303;
    }
    static const int32_t tc[4]={-1874,7288,-2355,-41761};
    static const int32_t nc[4]={8030,-31220,10090,178898};
    static const int32_t dc[4]={-455,1771,-572,-10153};
    int64_t temp=-737340+mulq(rt,22937600)+mulq(input,-21708);
    int64_t numerator=-24320+mulq(m->aux,-1610612736)+mulq(aux,18432)+mulq(input,92992);
    int64_t denominator=591472+mulq(rt,11726618)+mulq(input,-5277);
    for (unsigned i=0;i<4;++i) {
        temp+=mulq(m->x[i],tc[i]); numerator+=mulq(m->x[i],nc[i]); denominator+=mulq(m->x[i],dc[i]);
    }
    if (denominator<400000 || denominator>2500000) return false;
    int64_t reciprocal=6554;
    for (unsigned i=0;i<8;++i) {
        if (reciprocal < -1000000 || reciprocal > 1000000) return false;
        reciprocal=2*reciprocal-shr(mulq(reciprocal,reciprocal)*denominator,14);
    }
    if (reciprocal < -1000000 || reciprocal > 1000000) return false;
    int64_t power=shr(numerator*reciprocal,27)+16384;
    for (unsigned i=0;i<8;++i) {
        power=shr(wrapmul(power,power),14);
    }
    int64_t corrected=shr(wrapmul(rh,power),14);
    if (corrected>65536) corrected=65536;
    if (corrected<0) corrected=0;
    temp=shr(temp*1000,14);
    if (temp< -50000 || temp>130000) return false;
    *temperature=(int32_t)temp;
    *humidity=(uint32_t)shr(corrected*25000,14);
    return true;
}
