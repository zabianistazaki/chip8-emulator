/*
 * chip8.h - Platform-independent CHIP-8 interpreter core.
 *
 * DESIGN NOTE
 * -----------
 * The core knows nothing about SDL, files, windows or clocks. It is a pure
 * state machine: you feed it a ROM, key states and a "run N instructions"
 * request, and it mutates its own state (memory, registers, framebuffer).
 * This separation is what makes the emulator unit-testable without a display
 * and portable to any front end (SDL, terminal, WebAssembly, ...).
 */
#ifndef CHIP8_H
#define CHIP8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Hardware constants of the original CHIP-8 virtual machine ---------- */
#define CHIP8_MEM_SIZE    4096u   /* 4 KiB of RAM, addresses 0x000-0xFFF      */
#define CHIP8_ADDR_MASK   0x0FFFu /* 12-bit address bus: used to wrap safely  */
#define CHIP8_ROM_START   0x200u  /* programs are loaded at 0x200             */
#define CHIP8_FONT_START  0x050u  /* built-in hex font lives at 0x050-0x09F   */
#define CHIP8_WIDTH       64      /* monochrome display, 64 x 32 pixels       */
#define CHIP8_HEIGHT      32
#define CHIP8_STACK_DEPTH 16      /* 16 nested subroutine calls               */
#define CHIP8_NUM_REGS    16      /* V0..VF (VF doubles as the flag register) */
#define CHIP8_NUM_KEYS    16      /* hex keypad 0x0-0xF                       */
#define CHIP8_MAX_ROM     (CHIP8_MEM_SIZE - CHIP8_ROM_START)

/*
 * "Quirks": CHIP-8 was re-implemented many times and a few instructions
 * behave differently between interpreters. Games were written against one
 * variant, so a good emulator makes these switchable.
 */
typedef struct {
    bool vf_reset;  /* 8xy1/8xy2/8xy3 reset VF to 0            (COSMAC VIP)  */
    bool shift_vy;  /* 8xy6/8xyE shift Vy into Vx, not Vx      (COSMAC VIP)  */
    bool inc_i;     /* Fx55/Fx65 leave I = I + x + 1           (COSMAC VIP)  */
    bool jump_vx;   /* Bnnn becomes Bxnn: jump to xnn + Vx     (SUPER-CHIP)  */
    bool clip;      /* sprites clip at screen edge, not wrap                 */
    bool disp_wait; /* Dxyn waits for vertical blank (max 1 sprite / frame)  */
} Chip8Quirks;

/* Preset: behaviour most modern ROMs/interpreters expect. */
Chip8Quirks chip8_quirks_modern(void);
/* Preset: original COSMAC VIP behaviour (needed by some 1970s-era ROMs). */
Chip8Quirks chip8_quirks_vip(void);

typedef struct {
    /* --- architectural state ------------------------------------------- */
    uint8_t  mem[CHIP8_MEM_SIZE];
    uint8_t  V[CHIP8_NUM_REGS];
    uint16_t I;                          /* index register (12 bits used)    */
    uint16_t pc;                         /* program counter                  */
    uint16_t stack[CHIP8_STACK_DEPTH];
    uint8_t  sp;                         /* stack pointer: next free slot    */
    uint8_t  delay;                      /* delay timer, counts down at 60Hz */
    uint8_t  sound;                      /* sound timer, beeps while > 0     */

    /* --- I/O ------------------------------------------------------------ */
    uint8_t  gfx[CHIP8_WIDTH * CHIP8_HEIGHT]; /* one byte per pixel, 0 or 1  */
    bool     keys[CHIP8_NUM_KEYS];       /* true while key is held down      */
    bool     draw;                       /* framebuffer changed since cleared */

    /* --- emulator bookkeeping ------------------------------------------ */
    Chip8Quirks q;
    bool     vblank_wait;   /* set by Dxyn under disp_wait: end this frame   */
    bool     key_latched;   /* Fx0A state machine: a key was pressed...      */
    uint8_t  key_latch;     /* ...and this is which one (waiting for release)*/
    uint32_t rng;           /* xorshift32 state for Cxnn                     */

    /* --- fault reporting (illegal opcode, stack over/underflow) --------- */
    const char *fault;      /* NULL while healthy                            */
    uint16_t fault_pc;
    uint16_t fault_op;
} Chip8;

/* Reset the machine: clears RAM/regs/screen, installs font, sets pc=0x200. */
void chip8_init(Chip8 *c, Chip8Quirks quirks, uint32_t rng_seed);

/* Copy a ROM image to 0x200. Returns false if it does not fit. */
bool chip8_load(Chip8 *c, const uint8_t *rom, size_t len);

/* Execute exactly one fetch-decode-execute cycle. False on fault. */
bool chip8_step(Chip8 *c);

/* Decrement delay/sound timers (call at 60 Hz). */
void chip8_tick_timers(Chip8 *c);

/*
 * Run one 60 Hz frame: up to `cycles` instructions, then tick the timers.
 * Returns false if the CPU faulted (see c->fault).
 */
bool chip8_run_frame(Chip8 *c, int cycles);

#endif /* CHIP8_H */
