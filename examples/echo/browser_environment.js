// Hosting glue only: the C++ application reads the same environment names on
// native and WebAssembly. No discovery or connection API is implemented here.
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
    const values = Module.p2pdcEchoEnvironment || {};
    for (const [key, value] of Object.entries(values)) {
        if (key.startsWith("P2PDC_ECHO_") && typeof value === "string" && value !== "") {
            ENV[key] = value;
        }
    }
});
