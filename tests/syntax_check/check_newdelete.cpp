// ProtoGC syntax/AST compile check — global operator new/delete TU.
//
// Compiles src/ProtoGCNewDelete.cpp as its own translation unit, exactly as
// a firmware build would. This validates the global operator new/delete
// override definitions against the real ESP-IDF v6.0.2 headers.

#include "../../src/ProtoGCNewDelete.cpp"
