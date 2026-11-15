return createRecordBridge((store, operation, kind, bytes, detail) => {
    let pointer = 0;
    try {
        if (bytes !== null) {
            pointer = Module._p2pdc_proxy_alloc(bytes.length);
            if (!pointer) {
                const failure = kind === 1 ? 2 : 5;
                Module._p2pdc_proxy_event(store, operation, failure, 0, 0, 5);
                return false;
            }
            HEAPU8.set(bytes, pointer);
        }
        return Module._p2pdc_proxy_event(store, operation, kind, pointer, bytes?.length ?? 0, detail) === 1;
    } finally { if (pointer) Module._p2pdc_proxy_free(pointer); }
});
