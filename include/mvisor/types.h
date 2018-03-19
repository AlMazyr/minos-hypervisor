#ifndef _MVISOR_TYPES_H_
#define _MVISOR_TYPES_H_

#include <asm/asm_types.h>

typedef __u32 uint32_t;
typedef __s32 int32_t;
typedef __u16 uint16_t;
typedef __s16 int16_t;
typedef __u8 uint8_t;
typedef __s8 int8_t;
typedef __u64 uint64_t;
typedef __s64 int64_t;

typedef unsigned long size_t;
typedef unsigned long phy_addr_t;

#define MAX(a, b)	a > b ? a : b
#define MIN(a, b)	a < b ? a : b
#define max(a, b)	a > b ? a : b
#define min(a, b)	a < b ? a : b

#define u8_to_u16(low, high)	((high << 8) | low)
#define u8_to_u32(u1, u2, u3, u4)	\
	((u4 << 24) | (u3 << 16) | (u2 << 8) | (u1))
#define u16_to_u32(low, high)	((high << 16) | low)

#define NULL ((void *)0)
typedef void (*void_func_t)(void);

#define container_of(ptr, name, member) \
	(name *)((unsigned char *)ptr - ((unsigned char *)&(((name *)0)->member)))

#define BIT(nr) (1UL << (nr))

#define ALIGN(num, size)	((num) & ~(size - 1))
#define BALIGN(num, size)	(((num) + (size - 1)) & ~(size - 1))

#define stringify_no_expansion(x) #x
#define stringify(x) stringify_no_expansion(x)

#define SIZE_1G		(0x40000000)
#define SIZE_4K		(0x1000)
#define SIZE_1M		(0x100000)
#define SIZE_1K		(0x400)

#define SIZE_16K	(16 * SIZE_1K)
#define SIZE_32M	(32 * SIZE_1M)
#define SIZE_64K	(64 * SIZE_1K)
#define SIZE_512M	(512 * SIZE_1M)
#define SIZE_2M		(2 * 1024 * 1024)
#define SIZE_8M		(8 * 1024 * 1024)

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define BITS_TO_LONGS(nr) DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

#define DECLARE_BITMAP(name,bits) \
	const unsigned long name[BITS_TO_LONGS(bits)]

#define BITS_PER_LONG		64
#define BIT_ULL(nr)		(1ULL << (nr))
#define BIT_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)	(1ULL << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)	((nr) / BITS_PER_LONG_LONG)
#define BITS_PER_BYTE		8

#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))

#endif
