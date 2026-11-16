// Separate wall-clock validation from timers. Each component owns a clock.
// Backward steps up to the default clock skew are absorbed by holding at the
// last value; a larger step throws, so that component reports a clock error,
// and a recreated component starts from the new time (as natively).
export const clockRegressionTolerance = 30;
export class DiscoveryClock {
    constructor(now) {
        if (typeof now !== "function") throw new TypeError("clock function required");
        this.now = now; this.last = null;
    }
    read() {
        const value = this.now();
        if (!Number.isSafeInteger(value) || value < 0 || value > Number.MAX_SAFE_INTEGER - 300)
            throw new RangeError("invalid clock");
        if (this.last !== null && value < this.last) {
            if (this.last - value <= clockRegressionTolerance) return this.last;
            throw new RangeError("wall clock moved backwards beyond the tolerated skew");
        }
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
