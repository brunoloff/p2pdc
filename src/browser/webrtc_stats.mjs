const routeDeadlineMs = 2000;

// Retain the original request after a timeout so callers can avoid starting
// another getStats operation while the browser still has one pending.
export function observeWithDeadline(request, onExpire) {
    let expired = false, cancel;
    const deadline = new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            expired = true;
            onExpire();
            reject(new Error("route observation timed out"));
        }, routeDeadlineMs);
        cancel = () => { clearTimeout(timer); reject(new Error("route observation canceled")); };
    });
    return { result: Promise.race([request, deadline]), cancel, expired: () => expired };
}

export function recoverExpiredRequest(request, result, expired, recover) {
    const settled = () => { if (expired()) void result.then(recover); };
    request.then(settled, settled);
}
