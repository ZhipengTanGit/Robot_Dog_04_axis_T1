#ifndef __BSP_BITBAND_H
#define __BSP_BITBAND_H

#include "stm32f10x.h"

/* 位带操作核心宏 */
#define BITBAND(addr, bitnum)   ((addr & 0xF0000000) + 0x02000000 + ((addr & 0x000FFFFF) << 5) + ((bitnum) << 2))
#define MEM_ADDR(addr)          (*((volatile unsigned long *)(addr)))
#define BIT_ADDR(addr, bitnum)  MEM_ADDR(BITBAND(addr, bitnum))

/* GPIO 输出寄存器地址 */
#define GPIOA_ODR_ADDR          (GPIOA_BASE + 0x0C)
#define GPIOB_ODR_ADDR          (GPIOB_BASE + 0x0C)
#define GPIOC_ODR_ADDR          (GPIOC_BASE + 0x0C)
#define GPIOD_ODR_ADDR          (GPIOD_BASE + 0x0C)
#define GPIOE_ODR_ADDR          (GPIOE_BASE + 0x0C)
#define GPIOF_ODR_ADDR          (GPIOF_BASE + 0x0C)
#define GPIOG_ODR_ADDR          (GPIOG_BASE + 0x0C)

/* GPIO 输入寄存器地址 */
#define GPIOA_IDR_ADDR          (GPIOA_BASE + 0x08)
#define GPIOB_IDR_ADDR          (GPIOB_BASE + 0x08)
#define GPIOC_IDR_ADDR          (GPIOC_BASE + 0x08)
#define GPIOD_IDR_ADDR          (GPIOD_BASE + 0x08)
#define GPIOE_IDR_ADDR          (GPIOE_BASE + 0x08)
#define GPIOF_IDR_ADDR          (GPIOF_BASE + 0x08)
#define GPIOG_IDR_ADDR          (GPIOG_BASE + 0x08)

/* GPIO 输出位操作 */
#define PAout(n)                BIT_ADDR(GPIOA_ODR_ADDR, n)
#define PBout(n)                BIT_ADDR(GPIOB_ODR_ADDR, n)
#define PCout(n)                BIT_ADDR(GPIOC_ODR_ADDR, n)
#define PDout(n)                BIT_ADDR(GPIOD_ODR_ADDR, n)
#define PEout(n)                BIT_ADDR(GPIOE_ODR_ADDR, n)
#define PFout(n)                BIT_ADDR(GPIOF_ODR_ADDR, n)
#define PGout(n)                BIT_ADDR(GPIOG_ODR_ADDR, n)

/* GPIO 输入位操作 */
#define PAin(n)                 BIT_ADDR(GPIOA_IDR_ADDR, n)
#define PBin(n)                 BIT_ADDR(GPIOB_IDR_ADDR, n)
#define PCin(n)                 BIT_ADDR(GPIOC_IDR_ADDR, n)
#define PDin(n)                 BIT_ADDR(GPIOD_IDR_ADDR, n)
#define PEin(n)                 BIT_ADDR(GPIOE_IDR_ADDR, n)
#define PFin(n)                 BIT_ADDR(GPIOF_IDR_ADDR, n)
#define PGin(n)                 BIT_ADDR(GPIOG_IDR_ADDR, n)

#endif

