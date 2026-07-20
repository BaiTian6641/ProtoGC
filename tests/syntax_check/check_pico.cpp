// ProtoGC syntax/AST compile check — Pico SDK platform backend TU.
//
// Compiles src/pgc_pico.cpp as its own translation unit with the real Pico
// SDK headers and the same arm-none-eabi-g++ flags the RP2350-ProtoGPU
// firmware build uses (see run_check_pico.sh). No code is linked or executed.

#include "../../src/pgc_pico.cpp"
