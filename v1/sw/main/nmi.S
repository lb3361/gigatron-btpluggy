/** NMI handler */

#include <xtensa/coreasm.h>
#include "soc/gpio_reg.h"

/**
 *  Initialization of the irq data structure.
 *  See gigatron.h
 */
        .global irq
        .data
        .align  4
        .type   irq, @object
        .size   irq, 36
irq:
        .word   irq+5
        .byte   0
        .byte   -1
        .zero   14
        .word   0
        .byte   -1
        .zero   3
        .word   0
        .word   0

/**
 *  Private area to save the register during nmi
 *  This works because the nmi handler will return
 *  long before the next nmi (one scanline later).
 */

        .global irqsav
        .data
        .align  4
        .type   irqsav, @object
        .size   irqsav, 32
irqsav:
        .space  32

/**
 * This current ISR was called via `call0` instruction, so `a0` (return address)
 * was altered. Fortunately, `a0` was saved in XT_REG_EXCSAVE registers, restore it before
 * returning
 */
        .text
        .section .iram1.0,"ax",@progbits
        .literal_position
	.literal .LC0, GPIO
	.literal .LC1, irq
	.literal .LC2, irq+24  // inject
	.literal .LC3, irq+5   // hc595state
        .literal .LC4, irqsav
        .align  4
        .global xt_nmi
        .type   xt_nmi, @function
xt_nmi:
        /* Save registers */
        l32r    a0, .LC4
        s32i    a8,  a0, 0
        s32i    a9,  a0, 4
        s32i    a10, a0, 8
        s32i    a11, a0, 12
        s32i    a12, a0, 16
        s32i    a13, a0, 20
        s32i    a14, a0, 24

        /* Code lifted from a gcc compile (see pseudo code in gigatron.c) */
	l32r	a14, .LC0       // hw (a14) = &GPIO
	memw
	l32i	a12, a14, 64    // in (a12) = hw->in1.val
	l32r	a8, .LC1        // d (a8) = &irq
	l32i.n	a13, a8, 0
	l8ui	a9, a13, 0      // ix = *(d->hc595ptr)
	l32i.n	a10, a8, 20
	addx4	a11, a9, a10
	l32i.n	a11, a11, 0
	memw
	s32i.n	a11, a14, 8     // hw->out_w1ts = d->bytemap[ix^0xff]
	movi.n	a11, -1
	xor	a9, a9, a11
	extui	a9, a9, 0, 8
	addx4	a9, a9, a10
	l32i.n	a9, a9, 0
	memw
	s32i.n	a9, a14, 12     // hw->out_w1tc = d->bytemap[ix^0xff]
	l8ui	a9, a8, 5
	add.n	a9, a9, a9
	extui	a10, a12, 6, 1
	or	a9, a9, a10
	s8i	a9, a8, 5       // d->hc595state = ...
	movi.n	a11, 0x10
	memw
	s32i	a11, a14, 88    // hw->status1_w1tc.val = (1<<(GIGATRON_SERCLK...
	l32i.n	a10, a8, 12
	addi.n	a10, a10, 1
	s32i.n	a10, a8, 12     // d->videolines_since_ie += 1
	l32i.n	a9, a8, 8
	addi.n	a9, a9, 1
	s32i.n	a9, a8, 8       // d->videoline += 1
	bnone	a12, a11, .L2   // if (in>>(GIGATRON_SERCLK_GPIO-32)) & 1)
	mov.n	a11, a8
	l16ui	a8, a8, 6
	addi.n	a8, a8, 1
	s16i	a8, a11, 6      //| d->missed++
.L2:
	movi.n	a8, -0x1c
	bne	a9, a8, .L3     // if (d->videoline == -28)
	l32r	a8, .LC1
	l32i.n	a8, a8, 28
	beqz.n	a8, .L4         //| if (d->loaderframe
	l32r	a9, .LC1
	l32i.n	a10, a9, 32
	blti	a10, 1, .L4     //|     && d->loaderframelen > 0)
	s32i.n	a8, a9, 0       //|| d->hc595ptr = d->loaderframe
	s8i	a10, a9, 4      //|| d->hc595ptrlen = d->loaderframelen
	movi.n	a8, 0
	s32i.n	a8, a9, 32      //|| d->loaderframelen = 0
	s32i.n	a8, a9, 28      //|| d->loaderframe = 0
	j	.L1             //| else
.L4:
	l32r	a8, .LC1
	l8ui	a9, a8, 24
	movi	a8, 0xff
	beq	a9, a8, .L6     //| if (d->inject != 0xff)
	l32r	a8, .LC1
	l32r	a9, .LC2
	s32i.n	a9, a8, 0       //|| d->hc595ptr = &inject
	movi.n	a9, 0
	s8i	a9, a8, 4       //|| d->hc595ptrlen = 0
	j	.L1             //| else
.L6:
	l32r	a8, .LC1
	l32r	a9, .LC3
	s32i.n	a9, a8, 0       //|| d->hc595ptr = &hc595state
	movi.n	a9, 0
	s8i	a9, a8, 4       //|| d->hc595ptrlen = 0
	j	.L1             //else
.L3:
	l32r	a8, .LC1
	l8ui	a8, a8, 4
	bnei	a10, 1, .L7     //| if (d->videolines_since_ie == 1
	beqz.n	a8, .L7         //|     && d->hc595ptrlen > 0)
	l32r	a10, .LC1
	addi.n	a13, a13, 1
	s32i.n	a13, a10, 0     //|| hc595ptr++
	addi.n	a8, a8, -1
	s8i	a8, a10, 4      //|| hc595ptrlen--
.L7:
	l32r	a8, .LC1
	l8ui	a8, a8, 4
	bnez.n	a8, .L8         //| if (hc595ptrlen == 0)
	l32r	a8, .LC1
	l32r	a10, .LC3
	s32i.n	a10, a8, 0      //|| hc595ptr = &hc595state
	movi.n	a10, 0
	s8i	a10, a8, 4      //|| hc595ptrlen = 0
.L8:
	bnez.n	a9, .L9         //| if (d->videoline == 0)
	l32r	a8, .LC0
	movi.n	a9, 1
	memw
	s32i.n	a9, a8, 20      //|| hw->out1_w1ts.val = (1<<(PSEUDO_VBL_GPIO-32))
	j	.L1
.L9:
	movi	a8, 0x1df
	bge	a8, a9, .L1     //| if (videoline >= 0480)
	l32r	a8, .LC1
	l32i.n	a9, a8, 16
	addi.n	a9, a9, 1
	s32i.n	a9, a8, 16      //|| d->framecount++
	movi	a9, -0x29
	s32i.n	a9, a8, 8       //|| d->videoline = -41
	l32r	a8, .LC0
	movi.n	a9, 1
	memw
	s32i.n	a9, a8, 24      //| hw->out1_w1tc.val = (1<<(PSEUDO_VBL_GPIO-32))
.L1:

        /* Restore registers */
        l32r    a0, .LC4
        l32i    a8,  a0, 0
        l32i    a9,  a0, 4
        l32i    a10, a0, 8
        l32i    a11, a0, 12
        l32i    a12, a0, 16
        l32i    a13, a0, 20
        l32i    a14, a0, 24
        rsr     a0, XT_REG_EXCSAVE + XCHAL_NMILEVEL

        /* Return from NMI, we need to specify the level */
        rfi     XCHAL_NMILEVEL
        .size   xt_nmi, .-xt_nmi

