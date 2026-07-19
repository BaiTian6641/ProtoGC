// ProtoGC syntax/AST compile check — ESP-IDF platform backend TU.
//
// Compiles src/pgc_espidf.cpp as its own translation unit, exactly as a
// firmware build would. This validates the pgc_* backend implementations
// against the real ESP-IDF v6.0.2 headers.

#include "../../src/pgc_espidf.cpp"
