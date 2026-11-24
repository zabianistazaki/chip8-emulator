/*
 * main.c - SDL2 front end for the CHIP-8 core.
 *
 * Responsibilities (everything the core deliberately does NOT know about):
 *   1. Parse the command line and load the ROM file.
 *   2. Open a window, a texture and an audio device.
 *   3. Run the 60 Hz main loop: input -> CPU frame -> render -> sleep.
 *   4. Translate the 64x32 framebuffer into pixels, and sound timer into beeps.
 */
#include "chip8.h"

#include <SDL.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Tunables ----------------------------------------------------------- */
#define DEFAULT_SCALE   16        /* 64x32 -> 1024x512 window                */
#define DEFAULT_CPU_HZ  700       /* instructions per second                 */
#define FRAME_HZ        60        /* CHIP-8 timers & display run at 60 Hz    */
#define COLOR_ON        0xFFE8E8E8u   /* ARGB: lit pixel                     */
#define COLOR_OFF       0xFF101820u   /* ARGB: dark pixel                    */

#define BEEP_AMPLITUDE  2500      /* 16-bit sample amplitude (quiet)         */
#define BEEP_PERIOD     100       /* samples per cycle @44.1kHz => ~441 Hz   */

/*
 * Original COSMAC VIP keypad (left) mapped to a QWERTY block (right):
 *      1 2 3 C        1 2 3 4
 *      4 5 6 D   -->  Q W E R
 *      7 8 9 E        A S D F
 *      A 0 B F        Z X C V
 * Scancodes are physical positions, so this also works on AZERTY/Dvorak.
 * Index = CHIP-8 key value.
 */
static const SDL_Scancode KEYMAP[CHIP8_NUM_KEYS] = {
    SDL_SCANCODE_X, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, /* 0 1 2 3 */
    SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_A, /* 4 5 6 7 */
    SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_Z, SDL_SCANCODE_C, /* 8 9 A B */
    SDL_SCANCODE_4, SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_V  /* C D E F */
};

/* ---- Command line ------------------------------------------------------- */
typedef struct {
    const char *rom_path;
    int  scale;
    int  cpu_hz;
    bool vip;
} Options;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <rom.ch8>\n"
        "  -s, --scale N    pixel scale factor (default %d)\n"
        "  -c, --cpu-hz N   instructions per second (default %d)\n"
        "      --vip        original COSMAC VIP quirks (try if a ROM misbehaves)\n"
        "  -h, --help       show this help\n"
        "Keys: CHIP-8 keypad = 1234/QWER/ASDF/ZXCV   P pause   F5 reset   Esc quit\n",
        prog, DEFAULT_SCALE, DEFAULT_CPU_HZ);
}

static bool parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static bool parse_args(int argc, char **argv, Options *o)
{
    o->rom_path = NULL;
    o->scale = DEFAULT_SCALE;
    o->cpu_hz = DEFAULT_CPU_HZ;
    o->vip = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const bool has_val = i + 1 < argc;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            return false;
        } else if (!strcmp(a, "--vip")) {
            o->vip = true;
        } else if (!strcmp(a, "-s") || !strcmp(a, "--scale")) {
            if (!has_val || !parse_int(argv[++i], 1, 64, &o->scale)) {
                fprintf(stderr, "error: --scale expects 1..64\n");
                return false;
            }
        } else if (!strcmp(a, "-c") || !strcmp(a, "--cpu-hz")) {
            if (!has_val || !parse_int(argv[++i], FRAME_HZ, 100000, &o->cpu_hz)) {
                fprintf(stderr, "error: --cpu-hz expects %d..100000\n", FRAME_HZ);
                return false;
            }
        } else if (a[0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            return false;
        } else if (!o->rom_path) {
            o->rom_path = a;
        } else {
            fprintf(stderr, "error: more than one ROM given\n");
            return false;
        }
    }
    if (!o->rom_path) { fprintf(stderr, "error: no ROM given\n"); return false; }
    return true;
}

/* ---- ROM loading -------------------------------------------------------- */
static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno)); return NULL; }

    /* Read at most MAX_ROM+1 bytes: enough to detect "too big" without
     * trusting ftell() or loading a huge file into memory. */
    uint8_t *buf = malloc(CHIP8_MAX_ROM + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, CHIP8_MAX_ROM + 1, f);
    const bool io_err = ferror(f);
    fclose(f);

    if (io_err || n == 0 || n > CHIP8_MAX_ROM) {
        fprintf(stderr, "error: '%s' is empty, unreadable or larger than %u bytes\n",
                path, (unsigned)CHIP8_MAX_ROM);
        free(buf);
        return NULL;
    }
    *len_out = n;
    return buf;
}

/* ---- Audio: a square-wave beep driven by the sound timer ---------------- */
typedef struct {
    SDL_atomic_t on;        /* written by main thread, read by audio thread  */
    unsigned     phase;     /* audio thread only                             */
} Beeper;

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    Beeper *b = userdata;
    Sint16 *out = (Sint16 *)stream;
    const int count = len / (int)sizeof(Sint16);
    const int on = SDL_AtomicGet(&b->on);

    for (int i = 0; i < count; i++) {
        if (on) {
            out[i] = (b->phase < BEEP_PERIOD / 2) ? BEEP_AMPLITUDE : -BEEP_AMPLITUDE;
            b->phase = (b->phase + 1) % BEEP_PERIOD;
        } else {
            out[i] = 0;
            b->phase = 0;
        }
    }
}

/* ---- Rendering ---------------------------------------------------------- */
static void render(SDL_Renderer *ren, SDL_Texture *tex, const Chip8 *c, uint32_t *pixels)
{
    /* Expand the 1-byte-per-pixel framebuffer to 32-bit ARGB, then upload the
     * whole 64x32 image in one call; the GPU scales it to the window. */
    for (int i = 0; i < CHIP8_WIDTH * CHIP8_HEIGHT; i++)
        pixels[i] = c->gfx[i] ? COLOR_ON : COLOR_OFF;

    SDL_UpdateTexture(tex, NULL, pixels, CHIP8_WIDTH * (int)sizeof(uint32_t));
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    if (b && (!s || b > s)) s = b;
    return s ? s + 1 : path;
}

/* ---- Main --------------------------------------------------------------- */
int main(int argc, char **argv)
{
    Options opt;
    if (!parse_args(argc, argv, &opt)) { usage(argv[0]); return 2; }

    size_t rom_len = 0;
    uint8_t *rom = read_file(opt.rom_path, &rom_len);
    if (!rom) return 1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        free(rom);
        return 1;
    }

    int exit_code = 1;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    SDL_AudioDeviceID audio = 0;
    Beeper beeper;
    SDL_AtomicSet(&beeper.on, 0);
    beeper.phase = 0;

    char title[256];
    snprintf(title, sizeof title, "CHIP-8 - %s", base_name(opt.rom_path));

    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           CHIP8_WIDTH * opt.scale, CHIP8_HEIGHT * opt.scale,
                           SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); goto cleanup; }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");   /* crisp pixels */
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); goto cleanup; }

    /* Logical size = 64x32: SDL letterboxes and scales for any window size. */
    SDL_RenderSetLogicalSize(ren, CHIP8_WIDTH, CHIP8_HEIGHT);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);

    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                            CHIP8_WIDTH, CHIP8_HEIGHT);
    if (!tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); goto cleanup; }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audio_callback;
    want.userdata = &beeper;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);   /* 0: SDL converts */
    if (audio) SDL_PauseAudioDevice(audio, 0);
    else fprintf(stderr, "warning: no audio (%s); continuing silently\n", SDL_GetError());

    /* ---- Emulator setup ---- */
    static uint32_t pixels[CHIP8_WIDTH * CHIP8_HEIGHT];
    static Chip8 chip;                                    /* ~4.2 KiB, static */
    const Chip8Quirks quirks = opt.vip ? chip8_quirks_vip() : chip8_quirks_modern();
    const uint32_t seed = (uint32_t)SDL_GetPerformanceCounter() ^ 0x9E3779B9u;
    const int cycles_per_frame = opt.cpu_hz / FRAME_HZ;

    chip8_init(&chip, quirks, seed);
    if (!chip8_load(&chip, rom, rom_len)) { fprintf(stderr, "ROM does not fit\n"); goto cleanup; }

    /* ---- Main loop: fixed 60 Hz timestep -------------------------------- */
    const uint64_t freq  = SDL_GetPerformanceFrequency();
    const uint64_t frame = freq / FRAME_HZ;
    uint64_t next = SDL_GetPerformanceCounter() + frame;

    bool running = true, paused = false, halted = false, dirty = true;

    while (running) {
        /* 1. INPUT -------------------------------------------------------- */
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE: running = false; break;
                case SDL_SCANCODE_P:      if (!halted) paused = !paused; break;
                case SDL_SCANCODE_F5:     /* hard reset, keep same ROM */
                    chip8_init(&chip, quirks, seed);
                    chip8_load(&chip, rom, rom_len);
                    paused = halted = false;
                    dirty = true;
                    break;
                default: break;
                }
            } else if (e.type == SDL_WINDOWEVENT &&
                       (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                        e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                dirty = true;                              /* repaint        */
            }
        }
        const Uint8 *ks = SDL_GetKeyboardState(NULL);      /* current state  */
        for (int k = 0; k < CHIP8_NUM_KEYS; k++) chip.keys[k] = ks[KEYMAP[k]] != 0;

        /* 2. UPDATE: one frame of CPU work + one 60 Hz timer tick --------- */
        if (!paused && !halted) {
            if (!chip8_run_frame(&chip, cycles_per_frame)) {
                fprintf(stderr, "CPU fault: %s at 0x%03X (opcode 0x%04X)\n",
                        chip.fault, chip.fault_pc, chip.fault_op);
                halted = true;
            }
            SDL_AtomicSet(&beeper.on, chip.sound > 0 && !halted);
        } else {
            SDL_AtomicSet(&beeper.on, 0);
        }

        /* 3. RENDER only when something changed --------------------------- */
        if (chip.draw || dirty) {
            render(ren, tex, &chip, pixels);
            chip.draw = false;
            dirty = false;
        }

        /* 4. PACE: sleep until the next 60 Hz boundary -------------------- */
        next += frame;
        const uint64_t now = SDL_GetPerformanceCounter();
        if (now < next) SDL_Delay((Uint32)((next - now) * 1000 / freq));
        else if (now - next > frame * 4) next = now;       /* fell behind: resync */
    }
    exit_code = 0;

cleanup:
    if (audio) SDL_CloseAudioDevice(audio);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    free(rom);
    return exit_code;
}
