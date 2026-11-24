# CHIP-8 Emulator in C (SDL2)

A small, fast, portable CHIP-8 interpreter written in C11. The core is a
platform-independent state machine with unit tests; SDL2 provides video,
keyboard and audio.

<!-- Add a GIF of Pong / Space Invaders / Tetris running here -->

## Features
- All 35 CHIP-8 opcodes, with a strict fault model (illegal opcode, stack over/underflow are reported with PC and opcode instead of crashing)
- Switchable **quirks** (`--vip` for original COSMAC VIP behaviour) so both classic and modern ROMs run correctly
- Fixed 60 Hz timestep, configurable CPU speed, sleeps instead of busy-waiting
- Square-wave beep driven by the sound timer
- Layout-independent keypad mapping (physical key positions)
- Headless unit tests running under AddressSanitizer + UBSan
- Zero heap allocation in the core; ~4 KiB of state in one struct

## Build
```bash
# Linux (Debian/Ubuntu)   sudo apt install build-essential libsdl2-dev
# macOS (Homebrew)        brew install sdl2
# Windows (MSYS2)         pacman -S mingw-w64-x86_64-SDL2 make gcc
make            # builds ./chip8
make test       # core tests, no SDL required
```

## Run
```bash
./chip8 roms/keypad_demo.ch8        # included demo: press a hex key, see its glyph
./chip8 path/to/pong.ch8
./chip8 --scale 20 --cpu-hz 900 game.ch8
./chip8 --vip old_game.ch8          # if a ROM misbehaves, try original quirks
```

| CHIP-8 keypad | Keyboard |
|---|---|
| `1 2 3 C` | `1 2 3 4` |
| `4 5 6 D` | `Q W E R` |
| `7 8 9 E` | `A S D F` |
| `A 0 B F` | `Z X C V` |

`P` pause, `F5` reset, `Esc` quit.

## How it works
```
 ROM file --> [ 4 KiB RAM @0x200 ]
                    |
        +-----------v-----------+      60 Hz frame:
        |  FETCH   2 bytes @ pc |      1. poll keys
        |  DECODE  slice nibbles|      2. run N cycles (cpu_hz / 60)
        |  EXECUTE switch(op>>12)|     3. tick delay/sound timers
        +-----------+-----------+      4. upload 64x32 framebuffer if changed
                    |                  5. sleep to next frame boundary
   V0..VF, I, pc, stack, timers, gfx[2048]
```
- **Fetch**: opcodes are 2 bytes, big-endian: `op = mem[pc] << 8 | mem[pc+1]`, then `pc += 2`.
- **Decode**: `x = (op>>8)&F`, `y = (op>>4)&F`, `n = op&F`, `nn = op&FF`, `nnn = op&FFF`.
- **Execute**: a `switch` on the top nibble (compiled to a jump table); the dense `8xyN` and `Fxnn` groups have their own handlers.
- **Display**: sprites are XOR-ed onto a 64x32 buffer; turning a lit pixel off sets `VF = 1` (collision).
- **Flags**: arithmetic writes the result first and `VF` last, so `8F14`-style instructions leave the flag, not the sum, in `VF`.
- **Blocking input** (`Fx0A`) never blocks the host: it rewinds `pc` so the instruction repeats until a key is pressed *and* released.
- **Timers**: the delay and sound timers tick once per frame (60 Hz), independent of CPU speed.

## Quirks
| Quirk | Modern (default) | `--vip` |
|---|---|---|
| `8xy1/2/3` reset `VF` | no | yes |
| `8xy6/E` shift source | `Vx` | `Vy` |
| `Fx55/Fx65` increment `I` | no | yes |
| `Dxyn` waits for vblank | no | yes |
| Sprites clip at edges | yes | yes |

## Project layout
```
src/chip8.h   public API and machine state
src/chip8.c   interpreter core (no SDL)
src/main.c    SDL2 front end: window, input, audio, timing
tests/        headless unit tests
roms/         tiny demo ROM
```

## Testing with community ROMs
Use a CHIP-8 test-suite ROM set (for example the Timendus `chip8-test-suite`) to validate opcodes and quirks, and check each ROM's licence before redistributing it.

## Ideas to extend
Debugger/disassembler view, SUPER-CHIP (128x64) support, save states, configurable palette, WebAssembly build.
