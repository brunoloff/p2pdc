return createWebRtcBridge((id, kind, value, first, second) => {
    let a = 0, b = 0;
    try {
        if (first !== null) {
            a = Module._p2pdc_webrtc_alloc(first.length);
            if (!a) {
                Module._p2pdc_webrtc_event(id, 5, 0, 0, 0, 0, 0);
                return false;
            }
            HEAPU8.set(first, a);
        }
        if (second !== null) {
            b = Module._p2pdc_webrtc_alloc(second.length);
            if (!b) {
                Module._p2pdc_webrtc_event(id, 5, 0, 0, 0, 0, 0);
                return false;
            }
            HEAPU8.set(second, b);
        }
        return Module._p2pdc_webrtc_event(id, kind, value, a, first?.length ?? 0,
                                           b, second?.length ?? 0) === 1;
    } finally {
        if (a) Module._p2pdc_webrtc_free(a);
        if (b) Module._p2pdc_webrtc_free(b);
    }
});
