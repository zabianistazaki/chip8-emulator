/*
 * chip8.c - CHIP-8 interpreter core. See chip8.h for the big picture.
 *
 * OPCODE ANATOMY
 * --------------
 * Every CHIP-8 instruction is 2 bytes, stored big-endian. We slice the
 * 16-bit opcode into reusable fields:
 *
 *      opcode = 0xD123
 *                 |||`-- n   : low nibble          (sprite height, ...)
 *                 ||`--- y   : 3rd nibble          (register index Vy)
 *                 |`---- x   : 2nd nibble          (register index Vx)
 *                 `----- top : 1st nibble          (instruction family)
 *      nn  = low byte   (0x23)  - 8-bit immediate
 *      nnn = low 12 bits(0x123) - 12-bit address
 */
#include "chip8.h"

#include <string.h>

#define W CHIP8_WIDTH
#define H CHIP8_HEIGHT

/* 16 hex digits x 5 bytes. Each byte is one row; the 4 high bits are pixels.
 * e.g. '0' = F0 90 90 90 F0 draws:   ####
 *                                    #  #
 *                                    #  #
 *                                    #  #
 *                                    ####                                  */
static const uint8_t FONT[16 * 5] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, /* 0 */  0x20, 0x60, 0x20, 0x20, 0x70, /* 1 */
    0xF0, 0x10, 0xF0, 0x80, 0xF0, /* 2 */  0xF0, 0x10, 0xF0, 0x10, 0xF0, /* 3 */
    0x90, 0x90, 0xF0, 0x10, 0x10, /* 4 */  0xF0, 0x80, 0xF0, 0x10, 0xF0, /* 5 */
    0xF0, 0x80, 0xF0, 0x90, 0xF0, /* 6 */  0xF0, 0x10, 0x20, 0x40, 0x40, /* 7 */
    0xF0, 0x90, 0xF0, 0x90, 0xF0, /* 8 */  0xF0, 0x90, 0xF0, 0x10, 0xF0, /* 9 */
    0xF0, 0x90, 0xF0, 0x90, 0x90, /* A */  0xE0, 0x90, 0xE0, 0x90, 0xE0, /* B */
    0xF0, 0x80, 0x80, 0x80, 0xF0, /* C */  0xE0, 0x90, 0x90, 0x90, 0xE0, /* D */
    0xF0, 0x80, 0xF0, 0x80, 0xF0, /* E */  0xF0, 0x80, 0xF0, 0x80, 0x80  /* F */
};

/* ------------------------------------------------------------------------ */
/* Quirk presets                                                            */
/* ------------------------------------------------------------------------ */
Chip8Quirks chip8_quirks_modern(void)
{
    Chip8Quirks q = { .vf_reset = false, .shift_vy = false, .inc_i = false,
                      .jump_vx = false,  .clip = true,      .disp_wait = false };
    return q;
}

Chip8Quirks chip8_quirks_vip(void)
{
    Chip8Quirks q = { .vf_reset = true, .shift_vy = true, .inc_i = true,
                      .jump_vx = false, .clip = true,     .disp_wait = true };
    return q;
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                    */
/* ------------------------------------------------------------------------ */
void chip8_init(Chip8 *c, Chip8Quirks quirks, uint32_t rng_seed)
{
    memset(c, 0, sizeof *c);                        /* RAM, regs, screen = 0 */
    memcpy(&c->mem[CHIP8_FONT_START], FONT, sizeof FONT);
    c->pc  = CHIP8_ROM_START;                       /* execution starts here */
    c->q   = quirks;
    c->rng = rng_seed ? rng_seed : 0x1234ABCDu;     /* xorshift must be != 0 */
}

bool chip8_load(Chip8 *c, const uint8_t *rom, size_t len)
{
    if (len == 0 || len > CHIP8_MAX_ROM) return false;
    memcpy(&c->mem[CHIP8_ROM_START], rom, len);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

/* xorshift32: tiny, fast, good enough for Cxnn (no libc rand() state). */
static uint8_t rand_byte(Chip8 *c)
{
    uint32_t x = c->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c->rng = x;
    return (uint8_t)(x >> 8);
}

/* Record a fault and return false so call sites can `return fail(...)`. */
static bool fail(Chip8 *c, const char *why, uint16_t pc, uint16_t op)
{
    c->fault = why;
    c->fault_pc = pc;
    c->fault_op = op;
    return false;
}

/* Skip the next instruction (every instruction is 2 bytes). */
static inline void skip(Chip8 *c)
{
    c->pc = (uint16_t)((c->pc + 2) & CHIP8_ADDR_MASK);
}

/*
 * Dxyn - draw an 8xN sprite from memory[I..I+N-1] at (Vx, Vy).
 *
 * Pixels are XOR-ed onto the screen. If any lit pixel is turned off, VF=1
 * (collision) - games use this for hit detection.
 * The *start* coordinate always wraps (mod 64 / mod 32); pixels that run past
 * the edge are clipped or wrapped depending on the `clip` quirk.
 */
static void draw_sprite(Chip8 *c, unsigned x, unsigned y, unsigned n)
{
    const unsigned px = c->V[x] % W;   /* read Vx/Vy BEFORE touching VF,   */
    const unsigned py = c->V[y] % H;   /* since x or y may be 0xF          */
    uint8_t collision = 0;

    for (unsigned row = 0; row < n; row++) {
        unsigned yy = py + row;
        if (yy >= H) {
            if (c->q.clip) break;      /* rest of the sprite is off-screen */
            yy %= H;
        }
        const uint8_t bits = c->mem[(c->I + row) & CHIP8_ADDR_MASK];
        for (unsigned col = 0; col < 8; col++) {
            if (!(bits & (0x80u >> col))) continue;   /* transparent pixel */
            unsigned xx = px + col;
            if (xx >= W) {
                if (c->q.clip) break;
                xx %= W;
            }
            uint8_t *pix = &c->gfx[yy * W + xx];
            collision |= *pix;         /* was it already lit?              */
            *pix ^= 1;                 /* XOR draw                         */
        }
    }
    c->V[0xF] = collision;
    c->draw = true;
    if (c->q.disp_wait) c->vblank_wait = true;
}

/* ------------------------------------------------------------------------ */
/* ALU: 8xyN                                                                */
/* ------------------------------------------------------------------------ */
static bool exec_alu(Chip8 *c, uint16_t op, unsigned x, unsigned y, unsigned n)
{
    uint8_t *V = c->V;
    const uint8_t vx = V[x], vy = V[y];
    uint8_t flag;

    /*
     * FLAG-ORDER RULE: arithmetic writes the result to Vx FIRST and VF LAST.
     * If x == 0xF the flag must win (this is the documented behaviour and
     * the most common emulator bug).
     */
    switch (n) {
    case 0x0: V[x] = vy;                                   return true;
    case 0x1: V[x] = vx | vy; if (c->q.vf_reset) V[0xF] = 0; return true;
    case 0x2: V[x] = vx & vy; if (c->q.vf_reset) V[0xF] = 0; return true;
    case 0x3: V[x] = vx ^ vy; if (c->q.vf_reset) V[0xF] = 0; return true;
    case 0x4: {                       /* ADD with carry                    */
        const unsigned sum = (unsigned)vx + vy;
        V[x] = (uint8_t)sum;
        V[0xF] = sum > 0xFF;
        return true;
    }
    case 0x5:                         /* Vx = Vx - Vy, VF = NOT borrow     */
        flag = vx >= vy;
        V[x] = (uint8_t)(vx - vy);
        V[0xF] = flag;
        return true;
    case 0x6: {                       /* SHR: bit shifted out goes to VF   */
        const uint8_t src = c->q.shift_vy ? vy : vx;
        flag = src & 1;
        V[x] = src >> 1;
        V[0xF] = flag;
        return true;
    }
    case 0x7:                         /* Vx = Vy - Vx, VF = NOT borrow     */
        flag = vy >= vx;
        V[x] = (uint8_t)(vy - vx);
        V[0xF] = flag;
        return true;
    case 0xE: {                       /* SHL                               */
        const uint8_t src = c->q.shift_vy ? vy : vx;
        flag = src >> 7;
        V[x] = (uint8_t)(src << 1);
        V[0xF] = flag;
        return true;
    }
    default:
        return fail(c, "illegal 8xyN opcode", (uint16_t)(c->pc - 2), op);
    }
}

/* ------------------------------------------------------------------------ */
/* Fxnn group                                                               */
/* ------------------------------------------------------------------------ */
static bool exec_misc(Chip8 *c, uint16_t op, unsigned x)
{
    uint8_t *V = c->V;

    switch (op & 0xFF) {
    case 0x07: V[x] = c->delay;                              return true;
    case 0x15: c->delay = V[x];                              return true;
    case 0x18: c->sound = V[x];                              return true;
    case 0x1E: c->I = (uint16_t)((c->I + V[x]) & CHIP8_ADDR_MASK); return true;
    case 0x29: c->I = (uint16_t)(CHIP8_FONT_START + (V[x] & 0xF) * 5); return true;

    case 0x0A:                        /* wait for a key PRESS then RELEASE */
        /*
         * Blocking is implemented without blocking the host: while the
         * condition isn't met we rewind pc by 2 so this same instruction runs
         * again next cycle. Timers keep ticking, the window stays responsive.
         */
        if (!c->key_latched) {
            for (unsigned k = 0; k < CHIP8_NUM_KEYS; k++) {
                if (c->keys[k]) { c->key_latched = true; c->key_latch = (uint8_t)k; break; }
            }
            c->pc = (uint16_t)((c->pc - 2) & CHIP8_ADDR_MASK);
        } else if (c->keys[c->key_latch]) {
            c->pc = (uint16_t)((c->pc - 2) & CHIP8_ADDR_MASK);   /* still held */
        } else {
            V[x] = c->key_latch;                                 /* released   */
            c->key_latched = false;
        }
        return true;

    case 0x33: {                      /* BCD: store decimal digits of Vx   */
        const uint8_t v = V[x];
        c->mem[ c->I      & CHIP8_ADDR_MASK] = v / 100;
        c->mem[(c->I + 1) & CHIP8_ADDR_MASK] = (v / 10) % 10;
        c->mem[(c->I + 2) & CHIP8_ADDR_MASK] = v % 10;
        return true;
    }
    case 0x55:                        /* store V0..Vx to memory at I       */
        for (unsigned i = 0; i <= x; i++)
            c->mem[(c->I + i) & CHIP8_ADDR_MASK] = V[i];
        if (c->q.inc_i) c->I = (uint16_t)((c->I + x + 1) & CHIP8_ADDR_MASK);
        return true;
    case 0x65:                        /* load V0..Vx from memory at I      */
        for (unsigned i = 0; i <= x; i++)
            V[i] = c->mem[(c->I + i) & CHIP8_ADDR_MASK];
        if (c->q.inc_i) c->I = (uint16_t)((c->I + x + 1) & CHIP8_ADDR_MASK);
        return true;
    default:
        return fail(c, "illegal Fxnn opcode", (uint16_t)(c->pc - 2), op);
    }
}

/* ------------------------------------------------------------------------ */
/* The CPU cycle                                                            */
/* ------------------------------------------------------------------------ */
bool chip8_step(Chip8 *c)
{
    if (c->fault) return false;

    /* 1. FETCH: read 2 bytes at pc (big-endian) and advance pc past them.  */
    const uint16_t at = c->pc;
    const uint16_t op = (uint16_t)((c->mem[at & CHIP8_ADDR_MASK] << 8) |
                                    c->mem[(at + 1) & CHIP8_ADDR_MASK]);
    c->pc = (uint16_t)((at + 2) & CHIP8_ADDR_MASK);

    /* 2. DECODE: slice the opcode into fields.                             */
    const unsigned x   = (op >> 8) & 0xF;
    const unsigned y   = (op >> 4) & 0xF;
    const unsigned n   =  op       & 0xF;
    const uint8_t  nn  = (uint8_t)(op & 0xFF);
    const uint16_t nnn =  op       & 0x0FFF;
    uint8_t *V = c->V;

    /* 3. EXECUTE: a switch on the top nibble compiles to a jump table.     */
    switch (op >> 12) {
    case 0x0:
        if (op == 0x00E0) {                              /* CLS            */
            memset(c->gfx, 0, sizeof c->gfx);
            c->draw = true;
        } else if (op == 0x00EE) {                       /* RET            */
            if (c->sp == 0) return fail(c, "stack underflow", at, op);
            c->pc = c->stack[--c->sp];
        }
        /* else 0nnn (SYS call into native code): ignored, as modern VMs do */
        return true;

    case 0x1: c->pc = nnn; return true;                  /* JP nnn         */

    case 0x2:                                            /* CALL nnn       */
        if (c->sp >= CHIP8_STACK_DEPTH) return fail(c, "stack overflow", at, op);
        c->stack[c->sp++] = c->pc;                       /* return address */
        c->pc = nnn;
        return true;

    case 0x3: if (V[x] == nn)   skip(c); return true;    /* SE  Vx, nn     */
    case 0x4: if (V[x] != nn)   skip(c); return true;    /* SNE Vx, nn     */
    case 0x5:                                            /* SE  Vx, Vy     */
        if (n != 0) return fail(c, "illegal 5xyN opcode", at, op);
        if (V[x] == V[y]) skip(c);
        return true;

    case 0x6: V[x] = nn;                 return true;    /* LD  Vx, nn     */
    case 0x7: V[x] = (uint8_t)(V[x] + nn); return true;  /* ADD Vx, nn (no flag!) */
    case 0x8: return exec_alu(c, op, x, y, n);

    case 0x9:                                            /* SNE Vx, Vy     */
        if (n != 0) return fail(c, "illegal 9xyN opcode", at, op);
        if (V[x] != V[y]) skip(c);
        return true;

    case 0xA: c->I = nnn; return true;                   /* LD  I, nnn     */

    case 0xB:                                            /* JP  V0+nnn     */
        c->pc = (uint16_t)((nnn + (unsigned)(c->q.jump_vx ? V[x] : V[0])) & CHIP8_ADDR_MASK);
        return true;

    case 0xC: V[x] = rand_byte(c) & nn; return true;     /* RND Vx, nn     */

    case 0xD: draw_sprite(c, x, y, n); return true;      /* DRW Vx, Vy, n  */

    case 0xE:
        if (nn == 0x9E) { if ( c->keys[V[x] & 0xF]) skip(c); return true; } /* SKP  */
        if (nn == 0xA1) { if (!c->keys[V[x] & 0xF]) skip(c); return true; } /* SKNP */
        return fail(c, "illegal Exnn opcode", at, op);

    case 0xF: return exec_misc(c, op, x);
    }
    return fail(c, "unreachable", at, op);               /* keeps -Wall quiet */
}

/* ------------------------------------------------------------------------ */
/* Timing                                                                   */
/* ------------------------------------------------------------------------ */
void chip8_tick_timers(Chip8 *c)
{
    if (c->delay > 0) c->delay--;
    if (c->sound > 0) c->sound--;
}

bool chip8_run_frame(Chip8 *c, int cycles)
{
    c->vblank_wait = false;
    for (int i = 0; i < cycles && !c->vblank_wait; i++) {
        if (!chip8_step(c)) return false;
    }
    chip8_tick_timers(c);
    return true;
}
