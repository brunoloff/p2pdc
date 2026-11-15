#include "runtime_consumer.hpp"

#include <emscripten.h>

EM_JS(void, show_result, (int valid), {
    document.body.dataset.result = valid ? 'pass' : 'fail';
    document.body.textContent = valid ? 'pass' : 'runtime behavior mismatch';
});

int main() { p2pdc_test::run_runtime_consumer([](bool valid) { show_result(valid); }); }
