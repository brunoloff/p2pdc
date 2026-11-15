// Separate wall-clock validation from timers: time regression is terminal for
// replay protection even when the platform timer itself is monotonic.
export class DiscoveryClock {
    constructor(now) {
        if (typeof now !== "function") throw new TypeError("clock function required");
        this.now = now; this.last = null;
    }
    read() {
        const value = this.now();
        if (!Number.isSafeInteger(value) || value < 0 || value > Number.MAX_SAFE_INTEGER - 300 ||
            (this.last !== null && value < this.last)) throw new RangeError("invalid or regressing clock");
        this.last = value;
        return value;
    }
}
export function timing(options) {
    const timers = options.timers ?? { set: (callback, delay) => setTimeout(callback, delay), clear: (id) => clearTimeout(id) };
    if (typeof timers.set !== "function" || typeof timers.clear !== "function") throw new TypeError("invalid timers");
    const now = options.now ?? (() => Math.floor(Date.now() / 1000));
    return { timers, clock: new DiscoveryClock(now) };
}
