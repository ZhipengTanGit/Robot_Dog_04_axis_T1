#include "BSP_SysTick.h"

/* CMSIS V1.30 in this template does not declare the DWT register block. */
#define BSP_DWT_CTRL       (*((volatile uint32_t *)0xE0001000UL))
#define BSP_DWT_CYCCNT     (*((volatile uint32_t *)0xE0001004UL))
#define BSP_DWT_CYCCNTENA  0x00000001UL

static volatile uint32_t bsp_tick_ms;
static uint32_t bsp_cycles_per_us;

static void BSP_DelayClockInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    BSP_DWT_CTRL |= BSP_DWT_CYCCNTENA;
    bsp_cycles_per_us = SystemCoreClock / 1000000UL;
    if (bsp_cycles_per_us == 0UL)
    {
        bsp_cycles_per_us = 1UL;
    }
}

void SysTick_Init(void)
{
    SystemCoreClockUpdate();
    bsp_tick_ms = 0UL;
    BSP_DelayClockInit();
    (void)SysTick_Config(SystemCoreClock / 1000UL);
}

void BSP_SysTick_IncTick(void)
{
    ++bsp_tick_ms;
}

uint32_t BSP_Millis(void)
{
    return bsp_tick_ms;
}

void Delay_us(uint32_t us)
{
    uint32_t chunk_us;
    uint32_t start;
    uint32_t cycles;

    if (bsp_cycles_per_us == 0UL)
    {
        BSP_DelayClockInit();
    }
    while (us != 0UL)
    {
        /* Bounded chunks keep the multiplication and wrap subtraction safe. */
        chunk_us = (us > 1000000UL) ? 1000000UL : us;
        cycles = chunk_us * bsp_cycles_per_us;
        start = BSP_DWT_CYCCNT;
        while ((uint32_t)(BSP_DWT_CYCCNT - start) < cycles)
        {
        }
        us -= chunk_us;
    }
}

void Delay_ms(uint32_t ms)
{
    /* Also works before interrupts are enabled; never reprogram SysTick. */
    while (ms != 0UL)
    {
        Delay_us(1000UL);
        --ms;
    }
}

