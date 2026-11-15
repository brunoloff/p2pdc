export const maximumMessageBytes = 4 * 1024 * 1024;

export function messageLimit(value) {
    const limit = value ?? 262144;
    if (!Number.isSafeInteger(limit) || limit < 1 || limit > maximumMessageBytes)
        throw new RangeError("invalid application message limit");
    return limit;
}
