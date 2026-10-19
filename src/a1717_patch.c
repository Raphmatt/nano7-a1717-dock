/*
 * a1717_patch.c - iPod nano 7G, osos 1.1.2 (build 39A10023) only. RAM-only, nothing is flashed.
 *
 * Lets the Apple Lightning Dock A1717 work: no "accessory not supported" popup, audio on the
 * dock's 3.5 mm line-out. All addresses are runtime addresses (osos at 0x08000000, Thumb).
 *
 * One inline hook on 0x084df4c4, the function that copies the port chip's accessory-ID packet
 * into the ID struct at 0x08ae77a0 (r0 = packet + 5; the 26 ID bytes start at r0 + 2):
 *
 *   1. The dock reports vendor 0x05ac, type 0x0005. The allowlist at 0x08025358 only accepts
 *      type 1 or 2, otherwise it posts the popup. We rewrite the type to 0x0002 before the
 *      packet is stored.
 *   2. Being accepted is not enough: the nano never turns the dock's port lines on (port-chip
 *      IDs 1/2). In stock firmware only the port-power callback 0x080d0408(4, 1) does that,
 *      reached when USB host mode starts, which an accessory requests over iAP1 - the dock
 *      never does. We post a call to the UI task that waits for the port state byte
 *      (0x08918558) to reach 3 ("ready") and then calls 0x080d0408(4, 1).
 *
 * Undock is left to the stock detach path (0x084e0d34 turns IDs 1/2 off again).
 * Load once per boot (see tools/load.py); the hook checks the original bytes first.
 */
#include <stdint.h>

#define ID_WRITER    0x084df4c4u   /* push {r4,lr}; ldrb r1,[r0,#1] = 10 b5 41 78 */
#define UI_POST      0x0802fbd9u   /* post(obj, pmf.fn, pmf.adj, arg): run a call on the UI task */
#define UI_POST_OBJ  0x0842a995u   /* returns the object the stock popup delegate is posted with */
#define PORT_STATE   0x08918558u   /* 0 detached, 1 attached, 3 ready, 4 powered */
#define PWR_HANDLER  0x080d0409u   /* port-power callback (event, on) */
#define OS_SLEEP     0x0842b1ddu   /* (ticks, 0) */

typedef uint32_t (*fn0_t)(void);
typedef uint32_t (*fn2_t)(uint32_t, uint32_t);
typedef uint32_t (*fn5_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* __tramp: 8 bytes reserved by link.ld at the end of the image for the relocated prologue
 * (4 bytes) + a B.W back to ID_WRITER + 4. */
extern uint8_t __text_start__[], __text_end__[], __tramp[];
static const uint8_t k_orig[4] = { 0x10, 0xb5, 0x41, 0x78 };

static void icache_sync(uint32_t addr, uint32_t size)
{
    register uint32_t r12 __asm__("r12") = 7;   /* firmware cache-maintenance SVC, selector 7 */
    register uint32_t r0 __asm__("r0") = addr;
    register uint32_t r1 __asm__("r1") = size;
    __asm__ volatile("svc #70" : "+r"(r12), "+r"(r0), "+r"(r1) : : "memory");
}

/* Thumb-2 B.W (unconditional, +-16 MB) at dst, jumping to target. */
static void emit_bw(volatile uint8_t *dst, uint32_t target)
{
    uint32_t off = (target & ~1u) - ((uint32_t)(uintptr_t)dst + 4u);
    uint32_t s = (off >> 24) & 1u, i1 = (off >> 23) & 1u, i2 = (off >> 22) & 1u;
    uint32_t j1 = (~(i1 ^ s)) & 1u, j2 = (~(i2 ^ s)) & 1u;
    uint32_t hw1 = 0xF000u | (s << 10) | ((off >> 12) & 0x3FFu);
    uint32_t hw2 = 0x9000u | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FFu);

    dst[0] = (uint8_t)hw1; dst[1] = (uint8_t)(hw1 >> 8);
    dst[2] = (uint8_t)hw2; dst[3] = (uint8_t)(hw2 >> 8);
}

/* Runs on the UI task (posted as a member call; its arguments are ignored). */
static void power_on(void)
{
    uint32_t i;

    for (i = 0; i < 40u && *(volatile uint8_t *)PORT_STATE < 3u; i++)
        ((fn2_t)OS_SLEEP)(25u, 0);
    ((fn2_t)PWR_HANDLER)(4u, 1u);   /* state 3 -> 4, port-chip IDs 1/2 on; no-op if already 4 */
}

static uint32_t id_writer_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    volatile uint8_t *p = (volatile uint8_t *)a0;   /* [?, 2, 26-byte ID ...] */

    if (p[1] == 2u && p[2 + 0x0d] == 0x05u && p[2 + 0x0e] == 0xacu &&
        p[2 + 0x10] == 0x00u && p[2 + 0x11] == 0x05u) {
        p[2 + 0x11] = 0x02u;                                   /* type 5 -> 2 */
        ((fn5_t)UI_POST)(((fn0_t)UI_POST_OBJ)(), (uint32_t)(uintptr_t)power_on | 1u, 0, 0, 0);
    }
    return ((fn5_t)((uint32_t)(uintptr_t)__tramp | 1u))(a0, a1, a2, a3, a4);
}

/* Entry point, called once via the SCSI exec (address | 1). Must stay the first function. */
__attribute__((section(".text.entry"), used, noinline))
void payload_entry(void)
{
    volatile uint8_t *f = (volatile uint8_t *)ID_WRITER;
    int i;

    icache_sync((uint32_t)(uintptr_t)__text_start__, (uint32_t)(__text_end__ - __text_start__));
    for (i = 0; i < 4; i++)
        if (f[i] != k_orig[i])
            return;                         /* not 1.1.2, or already patched this boot */

    for (i = 0; i < 4; i++)
        ((volatile uint8_t *)__tramp)[i] = k_orig[i];
    emit_bw((volatile uint8_t *)__tramp + 4, ID_WRITER + 4u);
    icache_sync((uint32_t)(uintptr_t)__tramp, 8u);
    emit_bw(f, (uint32_t)(uintptr_t)id_writer_hook);
    icache_sync(ID_WRITER, 8u);
}
