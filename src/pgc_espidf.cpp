// ProtoGC ESP-IDF platform backend — implements the pgc_platform.h contract.
//
// Thin pass-through to the ESP-IDF heap_caps_* API. One file-static portMUX
// guards all ProtoGC shared state (the same role ProtoGC::sMux played before
// the port); it is statically initialized, so pgc_init() is a no-op here.
// Selected automatically by pgc_platform.h when ESP_PLATFORM /
// ARDUINO_ARCH_ESP32 / ESP32 is defined.

#include "pgc_platform.h"

#if defined(PGC_BACKEND_ESP_IDF)

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace {

// The single ProtoGC critical section (non-recursive by design, cf. M-01).
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

} // namespace

void pgc_init() {
    // No-op: the portMUX above is statically initialized.
}

void* pgc_malloc(size_t bytes, uint32_t caps) {
    return heap_caps_malloc(bytes, caps);
}

void pgc_free(void* ptr) {
    heap_caps_free(ptr);
}

size_t pgc_free_size(uint32_t caps) {
    return heap_caps_get_free_size(caps);
}

size_t pgc_largest_free_block(uint32_t caps) {
    return heap_caps_get_largest_free_block(caps);
}

void pgc_set_external_alloc_threshold(size_t bytes) {
    heap_caps_malloc_extmem_enable(bytes);
}

void pgc_enter_critical() {
    portENTER_CRITICAL(&s_mux);
}

void pgc_exit_critical() {
    portEXIT_CRITICAL(&s_mux);
}

void pgc_enter_critical_isr() {
    portENTER_CRITICAL_ISR(&s_mux);
}

void pgc_exit_critical_isr() {
    portEXIT_CRITICAL_ISR(&s_mux);
}

bool pgc_in_isr() {
    return xPortInIsrContext();
}

#endif // PGC_BACKEND_ESP_IDF
