/* Headless unit tests for the CHIP-8 core. No SDL needed: `make test`. */
#include "chip8.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* Build a machine with `prog` loaded and run `steps` instructions. */
static Chip8 run(const uint8_t *prog, size_t len, int steps, Chip8Quirks q)
{
    Chip8 c;
    chip8_init(&c, q, 1);
    CHECK(chip8_load(&c, prog, len));
    for (int i = 0; i < steps; i++) CHECK(chip8_step(&c));
    return c;
}

static void test_add_carry(void)
{
    const uint8_t p[] = { 0x60,0xFF, 0x61,0x02, 0x80,0x14 };      /* V0+=V1 */
    Chip8 c = run(p, sizeof p, 3, chip8_quirks_modern());
    CHECK(c.V[0] == 0x01 && c.V[0xF] == 1);
}

static void test_sub_flags(void)
{
    const uint8_t p[] = { 0x60,0x05, 0x61,0x03, 0x80,0x15 };      /* 5-3    */
    Chip8 c = run(p, sizeof p, 3, chip8_quirks_modern());
    CHECK(c.V[0] == 2 && c.V[0xF] == 1);
    const uint8_t q[] = { 0x60,0x03, 0x61,0x05, 0x80,0x15 };      /* 3-5    */
    c = run(q, sizeof q, 3, chip8_quirks_modern());
    CHECK(c.V[0] == 0xFE && c.V[0xF] == 0);
}

static void test_flag_wins_when_x_is_vf(void)
{
    const uint8_t p[] = { 0x6F,0xFF, 0x61,0x02, 0x8F,0x14 };      /* VF+=V1 */
    Chip8 c = run(p, sizeof p, 3, chip8_quirks_modern());
    CHECK(c.V[0xF] == 1);                  /* carry, not the sum 0x01      */
}

static void test_shift_quirk(void)
{
    const uint8_t p[] = { 0x60,0x00, 0x61,0x05, 0x80,0x16 };      /* V0=V1>>1 or V0>>1 */
    Chip8 c = run(p, sizeof p, 3, chip8_quirks_vip());
    CHECK(c.V[0] == 2 && c.V[0xF] == 1);   /* shifted Vy                   */
    c = run(p, sizeof p, 3, chip8_quirks_modern());
    CHECK(c.V[0] == 0 && c.V[0xF] == 0);   /* shifted Vx in place          */
}

static void test_call_ret(void)
{
    const uint8_t p[] = { 0x22,0x04, 0x60,0x07, 0x61,0x09, 0x00,0xEE }; /* CALL 0x204 */
    Chip8 c;
    chip8_init(&c, chip8_quirks_modern(), 1);
    chip8_load(&c, p, sizeof p);
    CHECK(chip8_step(&c) && c.pc == 0x204 && c.sp == 1);          /* CALL   */
    CHECK(chip8_step(&c));                                        /* V1=9   */
    CHECK(chip8_step(&c) && c.pc == 0x202 && c.sp == 0);          /* RET    */
    CHECK(c.V[1] == 9);
}

static void test_stack_faults(void)
{
    const uint8_t under[] = { 0x00,0xEE };
    Chip8 c;
    chip8_init(&c, chip8_quirks_modern(), 1);
    chip8_load(&c, under, sizeof under);
    CHECK(!chip8_step(&c) && c.fault != NULL);

    const uint8_t over[] = { 0x22,0x00 };                         /* CALL self */
    chip8_init(&c, chip8_quirks_modern(), 1);
    chip8_load(&c, over, sizeof over);
    int ok = 0;
    while (chip8_step(&c)) ok++;
    CHECK(ok == CHIP8_STACK_DEPTH && c.fault != NULL);
}

static void test_bcd_and_store_load(void)
{
    const uint8_t p[] = { 0x60,0x9C, 0xA3,0x00, 0xF0,0x33,        /* 156    */
                          0x62,0xAA, 0x63,0xBB, 0xA4,0x00, 0xF3,0x55,
                          0xA4,0x00, 0x62,0x00, 0xF3,0x65 };
    Chip8 c = run(p, sizeof p, 10, chip8_quirks_modern());
    CHECK(c.mem[0x300] == 1 && c.mem[0x301] == 5 && c.mem[0x302] == 6);
    CHECK(c.mem[0x402] == 0xAA && c.mem[0x403] == 0xBB);
    CHECK(c.V[2] == 0xAA && c.V[3] == 0xBB);
    CHECK(c.I == 0x400);                   /* modern: I untouched          */
    c = run(p, sizeof p, 10, chip8_quirks_vip());
    CHECK(c.I == 0x404);                   /* VIP: I += x + 1              */
}

static void test_draw_collision_and_xor(void)
{
    /* Draw font glyph '0' at (0,0) twice: 2nd draw erases it, VF=1. */
    const uint8_t p[] = { 0x60,0x00, 0xF0,0x29, 0xD1,0x25, 0xD1,0x25 };
    Chip8 c;
    chip8_init(&c, chip8_quirks_modern(), 1);
    chip8_load(&c, p, sizeof p);
    for (int i = 0; i < 3; i++) chip8_step(&c);
    CHECK(c.gfx[0] == 1 && c.gfx[3] == 1 && c.gfx[4] == 0 && c.V[0xF] == 0);
    chip8_step(&c);
    int lit = 0;
    for (size_t i = 0; i < sizeof c.gfx; i++) lit += c.gfx[i];
    CHECK(lit == 0 && c.V[0xF] == 1);
}

static void test_clip_vs_wrap(void)
{
    /* 1-row sprite 0xFF at x=60: clips after 4 px, or wraps to x=0..3. */
    const uint8_t p[] = { 0x60,0x3C, 0x61,0x00, 0xA2,0x0A, 0xD0,0x11, 0x00,0x00, 0xFF };
    Chip8Quirks q = chip8_quirks_modern();
    Chip8 c = run(p, sizeof p, 4, q);
    CHECK(c.gfx[60] == 1 && c.gfx[63] == 1 && c.gfx[0] == 0);
    q.clip = false;
    c = run(p, sizeof p, 4, q);
    CHECK(c.gfx[0] == 1 && c.gfx[3] == 1);
}

static void test_key_wait(void)
{
    const uint8_t p[] = { 0xF5,0x0A };
    Chip8 c;
    chip8_init(&c, chip8_quirks_modern(), 1);
    chip8_load(&c, p, sizeof p);
    for (int i = 0; i < 5; i++) { chip8_step(&c); CHECK(c.pc == 0x200); } /* blocks */
    c.keys[0xB] = true;  chip8_step(&c); CHECK(c.pc == 0x200);            /* held   */
    c.keys[0xB] = false; chip8_step(&c); CHECK(c.pc == 0x202 && c.V[5] == 0xB);
}

static void test_timers_and_frame(void)
{
    const uint8_t p[] = { 0x60,0x03, 0xF0,0x15, 0xF0,0x18, 0x12,0x06 };
    Chip8 c;
    chip8_init(&c, chip8_quirks_modern(), 1);
    chip8_load(&c, p, sizeof p);
    CHECK(chip8_run_frame(&c, 4));         /* 4 instrs then one timer tick */
    CHECK(c.delay == 2 && c.sound == 2);
}

static void test_illegal_opcode(void)
{
    const uint8_t p[] = { 0x80,0x1F };
    Chip8 c;
    chip8_init(&c, chip8_quirks_modern(), 1);
    chip8_load(&c, p, sizeof p);
    CHECK(!chip8_step(&c) && c.fault_pc == 0x200 && c.fault_op == 0x801F);
}

static void test_rom_size_limit(void)
{
    static uint8_t big[CHIP8_MAX_ROM + 1];
    Chip8 c;
    chip8_init(&c, chip8_quirks_modern(), 1);
    CHECK(chip8_load(&c, big, CHIP8_MAX_ROM));
    CHECK(!chip8_load(&c, big, CHIP8_MAX_ROM + 1));
}

int main(void)
{
    test_add_carry();               test_sub_flags();
    test_flag_wins_when_x_is_vf();  test_shift_quirk();
    test_call_ret();                test_stack_faults();
    test_bcd_and_store_load();      test_draw_collision_and_xor();
    test_clip_vs_wrap();            test_key_wait();
    test_timers_and_frame();        test_illegal_opcode();
    test_rom_size_limit();
    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    puts("all core tests passed");
    return 0;
}
