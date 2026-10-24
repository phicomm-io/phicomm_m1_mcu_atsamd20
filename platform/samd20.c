/* SPDX-License-Identifier: MIT
 * SAM D20, deliberately not the superficially similar SAM D21 register map. */
#include "samd20g18.h"
#include "samd20.h"
#define CPU_HZ 8000000u
#define SPINS 200000u
#define RX_SIZE 512u
#define TX_SIZE 256u
static volatile uint32_t milliseconds;
uint32_t SystemCoreClock=CPU_HZ;
static uint32_t lock(void) { uint32_t p=__get_PRIMASK(); __disable_irq(); return p; }
static void unlock(uint32_t p) { __set_PRIMASK(p); }
static bool gsync(void)
{ for(unsigned i=0;i<SPINS;++i) if(!(GCLK_REGS->GCLK_STATUS&GCLK_STATUS_SYNCBUSY_Msk)) return true; return false; }
static bool clock_channel(uint16_t id,uint16_t gen)
{ GCLK_REGS->GCLK_CLKCTRL=id|gen|GCLK_CLKCTRL_CLKEN_Msk; return gsync(); }
static bool wsync(void)
{ for(unsigned i=0;i<SPINS;++i) if(!(WDT_REGS->WDT_STATUS&WDT_STATUS_SYNCBUSY_Msk)) return true; return false; }
void m1_platform_watchdog(void)
{ if(!(WDT_REGS->WDT_STATUS&WDT_STATUS_SYNCBUSY_Msk)) WDT_REGS->WDT_CLEAR=0xa5; }
bool m1_platform_clock(void)
{
    /* Retain factory oscillator calibration. One wait state also makes warm
     * entry safe if a preceding boot stage was clocked faster than 8 MHz. */
    NVMCTRL_REGS->NVMCTRL_CTRLB=NVMCTRL_CTRLB_RWS(1)|NVMCTRL_CTRLB_MANW_Msk;
    PM_REGS->PM_AHBMASK|=PM_AHBMASK_NVMCTRL_Msk;
    PM_REGS->PM_APBBMASK|=PM_APBBMASK_PORT_Msk|PM_APBBMASK_NVMCTRL_Msk;
    PM_REGS->PM_APBAMASK|=PM_APBAMASK_GCLK_Msk|PM_APBAMASK_WDT_Msk;
    SYSCTRL_REGS->SYSCTRL_OSC8M=(SYSCTRL_REGS->SYSCTRL_OSC8M&~SYSCTRL_OSC8M_PRESC_Msk)|SYSCTRL_OSC8M_ENABLE_Msk;
    unsigned i; for(i=0;i<SPINS;++i) if(SYSCTRL_REGS->SYSCTRL_PCLKSR&SYSCTRL_PCLKSR_OSC8MRDY_Msk) break;
    if(i==SPINS) return false;
    GCLK_REGS->GCLK_GENDIV=GCLK_GENDIV_ID(0)|GCLK_GENDIV_DIV(1); if(!gsync()) return false;
    GCLK_REGS->GCLK_GENCTRL=GCLK_GENCTRL_ID(0)|GCLK_GENCTRL_SRC_OSC8M|GCLK_GENCTRL_GENEN_Msk;
    if(!gsync()) return false;
    PM_REGS->PM_CPUSEL=0; PM_REGS->PM_APBASEL=0; PM_REGS->PM_APBBSEL=0; PM_REGS->PM_APBCSEL=0;
    /* OSCULP32K / 32 = nominal 1024 Hz; 8192 watchdog ticks ~8 seconds. */
    GCLK_REGS->GCLK_GENDIV=GCLK_GENDIV_ID(2)|GCLK_GENDIV_DIV(32); if(!gsync()) return false;
    GCLK_REGS->GCLK_GENCTRL=GCLK_GENCTRL_ID(2)|GCLK_GENCTRL_SRC_OSCULP32K|GCLK_GENCTRL_GENEN_Msk;
    if(!gsync() || !clock_channel(GCLK_CLKCTRL_ID_WDT,GCLK_CLKCTRL_GEN_GCLK2)) return false;
    if(!(WDT_REGS->WDT_CTRL&WDT_CTRL_ALWAYSON_Msk)) {
        WDT_REGS->WDT_CTRL=0; if(!wsync()) return false;
        WDT_REGS->WDT_CONFIG=WDT_CONFIG_PER(10);
        WDT_REGS->WDT_CTRL=WDT_CTRL_ENABLE_Msk; if(!wsync()) return false;
    }
    m1_platform_watchdog();
    milliseconds=0;
    return SysTick_Config(CPU_HZ/1000)==0;
}
void SysTick_Handler(void) { milliseconds++; }
uint32_t m1_platform_millis(void) { return milliseconds; }
static port_group_registers_t *port(unsigned pin) { return &PORT_REGS->GROUP[pin/32]; }
static void out(unsigned pin,bool high)
{ if(high) port(pin)->PORT_OUTSET=1u<<(pin%32); else port(pin)->PORT_OUTCLR=1u<<(pin%32); }
static void gpio_output(unsigned pin,bool high)
{ port(pin)->PORT_PINCFG[pin%32]=0; out(pin,high); port(pin)->PORT_DIRSET=1u<<(pin%32); }
static void gpio_input(unsigned pin,bool pull)
{
    port(pin)->PORT_DIRCLR=1u<<(pin%32); out(pin,true);
    port(pin)->PORT_PINCFG[pin%32]=PORT_PINCFG_INEN_Msk|(pull?PORT_PINCFG_PULLEN_Msk:0);
}
static bool input(unsigned pin) { return (port(pin)->PORT_IN&(1u<<(pin%32)))!=0; }
static void mux(unsigned pin,unsigned fn)
{
    port_group_registers_t *p=port(pin); unsigned n=pin%32,shift=(n&1)*4;
    p->PORT_PMUX[n/2]=(uint8_t)((p->PORT_PMUX[n/2]&~(15u<<shift))|(fn<<shift));
    p->PORT_PINCFG[n]|=PORT_PINCFG_PMUXEN_Msk|PORT_PINCFG_INEN_Msk;
}
static void delay_us(unsigned us)
{ /* Conservative bit-bang timing at fixed 8 MHz, independent of SysTick. */
    for(unsigned i=0;i<us;++i) for(unsigned j=0;j<8;++j) __NOP();
}
static sercom_usart_int_registers_t *uart_regs(unsigned c)
{ return c==M1_LINK?&SERCOM3_REGS->USART_INT:c==M1_HCHO?&SERCOM4_REGS->USART_INT:&SERCOM1_REGS->USART_INT; }
typedef struct {
    uint8_t rx[RX_SIZE],tx[TX_SIZE]; uint32_t stamp[RX_SIZE];
    volatile uint16_t rh,rt,th,tt; volatile bool error;
} serial_queue;
static serial_queue serial[3];
static bool usync(sercom_usart_int_registers_t *u)
{ for(unsigned i=0;i<SPINS;++i) if(!(u->SERCOM_STATUS&SERCOM_USART_INT_STATUS_SYNCBUSY_Msk) && !(u->SERCOM_CTRLA&SERCOM_USART_INT_CTRLA_SWRST_Msk)) return true; return false; }
static bool uart_init(unsigned c,unsigned tx,unsigned rx,unsigned fn,unsigned baud)
{
    sercom_usart_int_registers_t *u=uart_regs(c); mux(tx,fn); mux(rx,fn);
    u->SERCOM_CTRLA=SERCOM_USART_INT_CTRLA_SWRST_Msk;
    if(!usync(u)) return false;
    u->SERCOM_CTRLA=SERCOM_USART_INT_CTRLA_MODE_USART_INT_CLK|SERCOM_USART_INT_CTRLA_DORD_LSB|
        SERCOM_USART_INT_CTRLA_RXPO_PAD1|SERCOM_USART_INT_CTRLA_TXPO_PAD0;
    /* 16x arithmetic baud, round rather than truncate. */
    u->SERCOM_BAUD=(uint16_t)(65536u-((UINT64_C(65536)*16*baud+CPU_HZ/2)/CPU_HZ));
    u->SERCOM_CTRLB=SERCOM_USART_INT_CTRLB_TXEN_Msk|SERCOM_USART_INT_CTRLB_RXEN_Msk;
    if(!usync(u)) return false;
    u->SERCOM_CTRLA|=SERCOM_USART_INT_CTRLA_ENABLE_Msk; if(!usync(u)) return false;
    u->SERCOM_INTENSET=SERCOM_USART_INT_INTENSET_RXC_Msk;
    return true;
}
#ifndef M1_BOOTLOADER
static void uart_irq(unsigned c)
{
    sercom_usart_int_registers_t *u=uart_regs(c); serial_queue *q=&serial[c];
    if(u->SERCOM_STATUS&(SERCOM_USART_INT_STATUS_PERR_Msk|SERCOM_USART_INT_STATUS_FERR_Msk|SERCOM_USART_INT_STATUS_BUFOVF_Msk)) {
        q->error=true;
        u->SERCOM_STATUS=SERCOM_USART_INT_STATUS_PERR_Msk|SERCOM_USART_INT_STATUS_FERR_Msk|SERCOM_USART_INT_STATUS_BUFOVF_Msk;
    }
    if(u->SERCOM_INTFLAG&SERCOM_USART_INT_INTFLAG_RXC_Msk) {
        uint8_t b=(uint8_t)u->SERCOM_DATA; uint16_t next=(uint16_t)((q->rh+1)&(RX_SIZE-1));
        if(next==q->rt) q->error=true;
        else { q->rx[q->rh]=b; q->stamp[q->rh]=milliseconds; __DMB(); q->rh=next; }
    }
    if((u->SERCOM_INTENSET&SERCOM_USART_INT_INTENSET_DRE_Msk) &&
       (u->SERCOM_INTFLAG&SERCOM_USART_INT_INTFLAG_DRE_Msk)) {
        if(q->tt!=q->th) { u->SERCOM_DATA=q->tx[q->tt]; q->tt=(q->tt+1)&(TX_SIZE-1); }
        else u->SERCOM_INTENCLR=SERCOM_USART_INT_INTENCLR_DRE_Msk;
    }
}
void SERCOM1_Handler(void) { uart_irq(M1_PM); }
void SERCOM3_Handler(void) { uart_irq(M1_LINK); }
void SERCOM4_Handler(void) { uart_irq(M1_HCHO); }
#endif
static bool uart_send(void *ctx,m1_channel c,const uint8_t *p,size_t n)
{
    (void)ctx; serial_queue *q=&serial[c]; uint32_t irq=lock();
    size_t free=(q->tt-q->th-1)&(TX_SIZE-1);
    if(n>free) { unlock(irq); return false; }
    for(size_t i=0;i<n;++i) { q->tx[q->th]=p[i]; q->th=(q->th+1)&(TX_SIZE-1); }
    __DMB(); uart_regs(c)->SERCOM_INTENSET=SERCOM_USART_INT_INTENSET_DRE_Msk;
    unlock(irq); return true;
}
void m1_platform_drain(m1_runtime *r)
{
    /* Bounded work per channel prevents noisy UARTs starving the scheduler. */
    for(unsigned c=0;c<3;++c) {
        serial_queue *q=&serial[c]; uint32_t irq=lock(); bool error=q->error;
        if(error) { q->rt=q->rh; q->error=false; } unlock(irq);
        if(error) m1_runtime_rx_error(r,(m1_channel)c);
        for(unsigned n=0;n<128 && q->rt!=q->rh;++n) {
            uint16_t i=q->rt; __DMB(); uint8_t b=q->rx[i]; uint32_t at=q->stamp[i];
            q->rt=(i+1)&(RX_SIZE-1); m1_runtime_rx(r,(m1_channel)c,b,at);
        }
    }
}
static sercom_i2cm_registers_t *i2c_regs(unsigned bus)
{ return bus==0?&SERCOM0_REGS->I2CM:&SERCOM2_REGS->I2CM; }
static bool isync(sercom_i2cm_registers_t *s)
{ for(unsigned i=0;i<SPINS;++i) if(!(s->SERCOM_STATUS&SERCOM_I2CM_STATUS_SYNCBUSY_Msk) && !(s->SERCOM_CTRLA&SERCOM_I2CM_CTRLA_SWRST_Msk)) return true; return false; }
static bool i2c_setup(unsigned bus)
{
    unsigned sda=bus==0?M1_DISPLAY_SDA:M1_CLIMATE_SDA,scl=sda+1;
    mux(sda,2); mux(scl,2); sercom_i2cm_registers_t *s=i2c_regs(bus);
    s->SERCOM_CTRLA=SERCOM_I2CM_CTRLA_SWRST_Msk; if(!isync(s)) return false;
    s->SERCOM_CTRLA=SERCOM_I2CM_CTRLA_MODE_I2C_MASTER;
    s->SERCOM_BAUD=35; /* <=100 kHz at 8 MHz including rise time. */
    s->SERCOM_CTRLB=0;
    s->SERCOM_CTRLA|=SERCOM_I2CM_CTRLA_ENABLE_Msk; if(!isync(s)) return false;
    s->SERCOM_STATUS=SERCOM_I2CM_STATUS_BUSSTATE(1); return isync(s);
}
static bool wait_i2c(sercom_i2cm_registers_t *s,uint8_t flag)
{
    for(unsigned i=0;i<SPINS;++i) {
        if(s->SERCOM_STATUS&(SERCOM_I2CM_STATUS_BUSERR_Msk|SERCOM_I2CM_STATUS_ARBLOST_Msk)) return false;
        if(s->SERCOM_INTFLAG&flag) return true;
    }
    return false;
}
static void release_line(unsigned p) { gpio_input(p,true); }
static void low_line(unsigned p) { gpio_output(p,false); }
static void i2c_recover(unsigned bus)
{
    unsigned sda=bus==0?M1_DISPLAY_SDA:M1_CLIMATE_SDA,scl=sda+1;
    i2c_regs(bus)->SERCOM_CTRLA=0; (void)isync(i2c_regs(bus));
    release_line(sda); release_line(scl);
    /* Open drain only: never drive either bus line high. */
    for(unsigned i=0;i<9;++i) { low_line(scl); delay_us(5); release_line(scl); delay_us(10); }
    low_line(scl); low_line(sda); delay_us(5); release_line(scl); delay_us(10); release_line(sda);
    (void)i2c_setup(bus);
}
static bool i2c_transfer(void *ctx,unsigned bus,uint8_t address,const uint8_t *tx,size_t nt,uint8_t *rx,size_t nr)
{
    (void)ctx; if((bus!=0 && bus!=2)||(!nt&&!nr)) return false;
    sercom_i2cm_registers_t *s=i2c_regs(bus);
    if(nt) {
        s->SERCOM_ADDR=(uint32_t)address<<1;
        if(!wait_i2c(s,SERCOM_I2CM_INTFLAG_MB_Msk) || (s->SERCOM_STATUS&SERCOM_I2CM_STATUS_RXNACK_Msk)) goto fail;
        for(size_t i=0;i<nt;++i) {
            s->SERCOM_DATA=tx[i];
            if(!wait_i2c(s,SERCOM_I2CM_INTFLAG_MB_Msk) || (s->SERCOM_STATUS&SERCOM_I2CM_STATUS_RXNACK_Msk)) goto fail;
        }
    }
    if(nr) {
        s->SERCOM_CTRLB&=~SERCOM_I2CM_CTRLB_ACKACT_Msk; if(!isync(s)) goto fail;
        s->SERCOM_ADDR=((uint32_t)address<<1)|1u;
        for(size_t i=0;i<nr;++i) {
            if(!wait_i2c(s,SERCOM_I2CM_INTFLAG_SB_Msk)) goto fail;
            if(i+1==nr) s->SERCOM_CTRLB|=SERCOM_I2CM_CTRLB_ACKACT_Msk;
            else s->SERCOM_CTRLB&=~SERCOM_I2CM_CTRLB_ACKACT_Msk;
            if(!isync(s)) goto fail;
            rx[i]=(uint8_t)s->SERCOM_DATA;
            s->SERCOM_CTRLB=(s->SERCOM_CTRLB&~SERCOM_I2CM_CTRLB_CMD_Msk)|SERCOM_I2CM_CTRLB_CMD(i+1==nr?3:2);
            if(!isync(s)) goto fail;
        }
    } else { s->SERCOM_CTRLB|=SERCOM_I2CM_CTRLB_CMD(3); if(!isync(s)) goto fail; }
    return true;
fail:
    i2c_recover(bus); return false;
}
/* DS1302-compatible three-wire RTC: command/data are shifted least significant
 * bit first. RX is sampled on rising edges after the command's final falling edge. */
static void rtc_begin(void) { gpio_output(M1_RTC_CLOCK,false); gpio_output(M1_RTC_CE,true); delay_us(4); }
static void rtc_end(void) { out(M1_RTC_CE,false); delay_us(4); gpio_input(M1_RTC_DATA,false); }
static void rtc_put(uint8_t value,bool reading)
{
    gpio_output(M1_RTC_DATA,false);
    for(unsigned b=0;b<8;++b) {
        out(M1_RTC_DATA,(value&(1u<<b))!=0); delay_us(2);
        out(M1_RTC_CLOCK,true); delay_us(2);
        /* Release data before the final falling edge of a read command. */
        if(b==7 && reading) gpio_input(M1_RTC_DATA,false);
        out(M1_RTC_CLOCK,false); delay_us(2);
    }
}
static uint8_t rtc_get(void)
{
    uint8_t v=0; gpio_input(M1_RTC_DATA,false);
    for(unsigned b=0;b<8;++b) {
        if(input(M1_RTC_DATA)) v|=(uint8_t)(1u<<b);
        out(M1_RTC_CLOCK,true); delay_us(2); out(M1_RTC_CLOCK,false); delay_us(2);
    }
    return v;
}
static void rtc_reg_write(uint8_t reg,uint8_t value)
{ rtc_begin(); rtc_put(reg,false); rtc_put(value,false); rtc_end(); }
static bool rtc_read(void *ctx,m1_clock *c)
{
    (void)ctx; uint8_t data[8]; rtc_begin(); rtc_put(0xbf,true);
    for(unsigned i=0;i<8;++i) data[i]=rtc_get();
    rtc_end(); return m1_rtc_decode(data,c);
}
static bool rtc_write(void *ctx,const m1_clock *c)
{
    uint8_t data[8]; if(!m1_rtc_encode(c,data)) return false;
    rtc_reg_write(0x8e,0); rtc_begin(); rtc_put(0xbe,false);
    for(unsigned i=0;i<8;++i) rtc_put(data[i],false);
    rtc_end();
    m1_clock check; return rtc_read(ctx,&check) && check.year==c->year && check.month==c->month && check.day==c->day &&
        check.hour==c->hour && check.minute==c->minute && (check.second==c->second || check.second==(c->second+1)%60);
}
static bool tcsync(void)
{ for(unsigned i=0;i<SPINS;++i) if(!(TC1_REGS->COUNT16.TC_STATUS&TC_STATUS_SYNCBUSY_Msk) && !(TC1_REGS->COUNT16.TC_CTRLA&TC_CTRLA_SWRST_Msk)) return true; return false; }
static void wifi_enable(void *ctx,bool enabled)
{ (void)ctx; gpio_output(M1_WIFI_ENABLE,enabled); }
static uint8_t buzzer_duty=50;
static bool buzzer_pwm(void *ctx,uint16_t hz,uint8_t duty)
{
    (void)ctx;
    if(hz<100 || hz>10000 || duty>100) return false;
    gpio_output(M1_BUZZER,false);
    TC1_REGS->COUNT16.TC_CTRLA&=~TC_CTRLA_ENABLE_Msk;
    if(!tcsync()) return false;
    uint16_t period=(uint16_t)(CPU_HZ/2/hz);
    TC1_REGS->COUNT16.TC_COUNT=0; if(!tcsync()) return false;
    TC1_REGS->COUNT16.TC_CC[0]=period-1; if(!tcsync()) return false;
    TC1_REGS->COUNT16.TC_CC[1]=(uint16_t)((uint32_t)period*duty/100); if(!tcsync()) return false;
    TC1_REGS->COUNT16.TC_CTRLA|=TC_CTRLA_ENABLE_Msk;
    if(!tcsync()) return false;
    buzzer_duty=duty; return true;
}
static void buzzer(void *ctx,bool enabled)
{
    (void)ctx;
    if(enabled && buzzer_duty==100) gpio_output(M1_BUZZER,true);
    else if(enabled) mux(M1_BUZZER,4); else gpio_output(M1_BUZZER,false);
}
void EIC_Handler(void) { EIC_REGS->EIC_INTFLAG=1u<<10; }
static bool esync(void)
{ for(unsigned i=0;i<SPINS;++i) if(!(EIC_REGS->EIC_STATUS&EIC_STATUS_SYNCBUSY_Msk)) return true; return false; }
bool m1_platform_button(void) { return !input(M1_BUTTON); }
static void reset(void *ctx) { (void)ctx; NVIC_SystemReset(); }
bool m1_platform_init(m1_io *io)
{
    if(!m1_platform_clock()) return false;
    wifi_enable(0,true);
    /* Stock startup: 1000 x 1500 decrement/branch iterations after CHIP_EN
     * release, before UART initialization. No invented millisecond constant. */
    __asm__ volatile(".syntax unified\n"
                     "movs r0, #250\n"
                     "lsls r0, r0, #2\n"
                     "b 3f\n"
                     "1: subs r1, r1, #1\n"
                     "cmp r1, #0\n"
                     "bne 1b\n"
                     "subs r0, r0, #1\n"
                     "cmp r0, #0\n"
                     "beq 4f\n"
                     "3: ldr r1, =1500\n"
                     "b 1b\n"
                     "4:\n"
                     ".syntax divided\n" ::: "r0","r1","cc","memory");
    m1_platform_watchdog();
    PM_REGS->PM_APBCMASK|=PM_APBCMASK_SERCOM0_Msk|PM_APBCMASK_SERCOM1_Msk|PM_APBCMASK_SERCOM2_Msk|
        PM_APBCMASK_SERCOM3_Msk|PM_APBCMASK_SERCOM4_Msk|PM_APBCMASK_TC1_Msk;
    PM_REGS->PM_APBAMASK|=PM_APBAMASK_EIC_Msk;
    for(unsigned i=0;i<5;++i) if(!clock_channel(GCLK_CLKCTRL_ID_SERCOM0_CORE+i,GCLK_CLKCTRL_GEN_GCLK0)) return false;
    if(!clock_channel(GCLK_CLKCTRL_ID_SERCOMX_SLOW,GCLK_CLKCTRL_GEN_GCLK2) ||
       !clock_channel(GCLK_CLKCTRL_ID_TC0_TC1,GCLK_CLKCTRL_GEN_GCLK0)||
       !clock_channel(GCLK_CLKCTRL_ID_EIC,GCLK_CLKCTRL_GEN_GCLK0)) return false;
    if(!uart_init(M1_LINK,M1_WIFI_TX,M1_WIFI_RX,2,115200)||
       !uart_init(M1_PM,M1_PM_TX,M1_PM_RX,2,9600)||
       !uart_init(M1_HCHO,M1_HCHO_TX,M1_HCHO_RX,3,9600)||!i2c_setup(0)||!i2c_setup(2)) return false;
    NVIC_SetPriority(SERCOM3_IRQn,1); NVIC_SetPriority(SERCOM1_IRQn,1); NVIC_SetPriority(SERCOM4_IRQn,1);
    NVIC_EnableIRQ(SERCOM3_IRQn); NVIC_EnableIRQ(SERCOM1_IRQn); NVIC_EnableIRQ(SERCOM4_IRQn);
    gpio_input(M1_BUTTON,true); mux(M1_BUTTON,0);
    EIC_REGS->EIC_CTRL=0;
    if(!esync()) return false;
    EIC_REGS->EIC_CONFIG[1]=EIC_CONFIG_SENSE2_BOTH|EIC_CONFIG_FILTEN2_Msk;
    EIC_REGS->EIC_INTFLAG=1u<<10; EIC_REGS->EIC_INTENSET=1u<<10; EIC_REGS->EIC_CTRL=EIC_CTRL_ENABLE_Msk;
    if(!esync()) return false;
    NVIC_SetPriority(EIC_IRQn,2); NVIC_EnableIRQ(EIC_IRQn);
    gpio_output(M1_BUZZER,false);
    TC1_REGS->COUNT16.TC_CTRLA=TC_CTRLA_SWRST_Msk; if(!tcsync()) return false;
    TC1_REGS->COUNT16.TC_CTRLA=TC_CTRLA_MODE_COUNT16|TC_CTRLA_WAVEGEN_MPWM|TC_CTRLA_PRESCALER_DIV2;
    TC1_REGS->COUNT16.TC_CC[0]=1999; if(!tcsync()) return false;
    TC1_REGS->COUNT16.TC_CC[1]=1000; if(!tcsync()) return false;
    TC1_REGS->COUNT16.TC_CTRLA|=TC_CTRLA_ENABLE_Msk; if(!tcsync()) return false;
    gpio_output(M1_RTC_CE,false); gpio_output(M1_RTC_CLOCK,false); gpio_input(M1_RTC_DATA,false);
    *io=(m1_io){.uart=uart_send,.i2c=i2c_transfer,.rtc_read=rtc_read,.rtc_write=rtc_write,.buzzer=buzzer,.reset=reset,.wifi_enable=wifi_enable,.buzzer_pwm=buzzer_pwm,.flash=m1_platform_flash()};
    return true;
}
static bool flash_read(void *ctx,uint32_t at,uint8_t *p,size_t n)
{
    (void)ctx; if(at>0x40000u || n>0x40000u-at) return false;
    const volatile uint8_t *s=(const volatile uint8_t *)(uintptr_t)at;
    for(size_t i=0;i<n;++i) p[i]=s[i];
    return true;
}
static bool mutable_address(uint32_t at,size_t n)
{
    if(at>=0x40000u || n>0x40000u-at) return false;
    if(at>=M1_UPDATE_METADATA_ADDRESS && at+n<=0x40000u) return true;
    if(at>=M1_STAGING_ADDRESS && at+n<=M1_STAGING_ADDRESS+0x10100u) return true;
#ifdef M1_BOOTLOADER
    if(at>=M1_APPLICATION_ADDRESS && at+n<=M1_APPLICATION_ADDRESS+0x10000u) return true;
#endif
    return false;
}
static bool nvm_ready(void)
{ for(unsigned i=0;i<2000000;++i) if(NVMCTRL_REGS->NVMCTRL_INTFLAG&NVMCTRL_INTFLAG_READY_Msk) return true; return false; }
static bool nvm_command(uint16_t cmd,uint32_t at)
{
    if(!nvm_ready()) return false;
    NVMCTRL_REGS->NVMCTRL_STATUS=NVMCTRL_STATUS_PROGE_Msk|NVMCTRL_STATUS_LOCKE_Msk|NVMCTRL_STATUS_NVME_Msk;
    NVMCTRL_REGS->NVMCTRL_ADDR=at/2;
    NVMCTRL_REGS->NVMCTRL_CTRLA=NVMCTRL_CTRLA_CMDEX_KEY|cmd;
    if(!nvm_ready()) return false;
    return !(NVMCTRL_REGS->NVMCTRL_STATUS&(NVMCTRL_STATUS_PROGE_Msk|NVMCTRL_STATUS_LOCKE_Msk|NVMCTRL_STATUS_NVME_Msk));
}
static bool flash_erase(void *ctx,uint32_t at)
{
    (void)ctx; if((at&255)||!mutable_address(at,256)) return false;
    m1_platform_watchdog(); uint32_t irq=lock(); bool ok=nvm_command(NVMCTRL_CTRLA_CMD_ER,at); unlock(irq); return ok;
}
static bool flash_write(void *ctx,uint32_t at,const uint8_t p[64])
{
    (void)ctx; if((at&63)||!mutable_address(at,64)) return false;
    m1_platform_watchdog(); uint32_t irq=lock(); bool ok=nvm_command(NVMCTRL_CTRLA_CMD_PBC,at);
    if(ok) {
        volatile uint16_t *d=(volatile uint16_t *)(uintptr_t)at;
        for(unsigned i=0;i<32;++i) d[i]=(uint16_t)(p[i*2]|(uint16_t)p[i*2+1]<<8);
        /* Datasheet 20.6.4.2.1: read AHB0 after loading the page buffer. */
        __DMB(); (void)*(volatile uint32_t *)(uintptr_t)at; __DSB();
        ok=nvm_command(NVMCTRL_CTRLA_CMD_WP,at);
    }
    unlock(irq); return ok;
}
m1_flash m1_platform_flash(void) { return (m1_flash){.read=flash_read,.erase=flash_erase,.write=flash_write}; }
__attribute__((naked,noreturn)) static void branch(uint32_t sp,uint32_t pc)
{ (void)sp; (void)pc; __asm volatile("msr msp, r0\n\tbx r1"); }
void m1_platform_jump(void)
{
    __disable_irq(); SysTick->CTRL=0; NVIC->ICER[0]=0xffffffff; NVIC->ICPR[0]=0xffffffff;
    SCB->ICSR=SCB_ICSR_PENDSTCLR_Msk|SCB_ICSR_PENDSVCLR_Msk;
    SCB->VTOR=M1_APPLICATION_ADDRESS; __DSB(); __ISB();
    const uint32_t *v=(const uint32_t *)M1_APPLICATION_ADDRESS;
    branch(v[0],v[1]);
}
