// Scout — reports what each JavaScript context inside the game offers.
//
// Doubles as a working example of the whole native bridge: it reads and writes
// its own files, keeps a counter between launches, and leaves a report that
// says what a mod could reach from here.

(function () {
    "use strict";

    if (typeof nrz === "undefined") {
        // Nothing to do without the bridge; the loader logs why separately.
        return;
    }

    var REPORT = "reports/scout.json";
    var STATE = "reports/scout-state.json";

    function safe(fn, fallback) {
        try {
            return fn();
        } catch (error) {
            return fallback;
        }
    }

    // What kind of context is this? The interface and the game's scripting
    // engine are the same engine, but they carry different globals.
    function describeContext() {
        var names = safe(function () {
            return Object.getOwnPropertyNames(globalThis);
        }, []);

        var marks = {
            document: typeof globalThis.document !== "undefined",
            window: typeof globalThis.window !== "undefined",
            engine: typeof globalThis.engine !== "undefined",
            require: typeof globalThis.require === "function",
            console: typeof globalThis.console !== "undefined",
            setTimeout: typeof globalThis.setTimeout === "function",
        };

        var modules = {};
        ["@minecraft/server", "@minecraft/server-ui", "@minecraft/vanilla-data"]
            .forEach(function (name) {
                modules[name] = safe(function () {
                    return typeof require === "function" && !!require(name);
                }, false);
            });

        return {
            kind: marks.document ? "interface" : (modules["@minecraft/server"] ? "game" : "unknown"),
            globals: names.slice(0, 80),
            globalCount: names.length,
            marks: marks,
            modules: modules,
            location: safe(function () { return String(globalThis.location); }, ""),
            title: safe(function () { return String(globalThis.document.title); }, ""),
        };
    }

    var state = nrz.readJson(STATE) || { runs: 0 };
    state.runs += 1;
    nrz.writeJson(STATE, state);

    var report = nrz.readJson(REPORT) || { contexts: [] };
    var seen = describeContext();
    seen.run = state.runs;
    report.contexts.push(seen);

    // Keeping every context of every launch would grow without bound.
    if (report.contexts.length > 20) {
        report.contexts = report.contexts.slice(report.contexts.length - 20);
    }
    report.loader = nrz.info();
    report.mods = nrz.listDir("mods");

    nrz.writeJson(REPORT, report);
    nrz.log("scout: context looks like '" + seen.kind + "', " + seen.globalCount
        + " globals, run #" + state.runs);

    // If the game's scripting API is here, say hello properly.
    if (seen.modules["@minecraft/server"]) {
        safe(function () {
            var server = require("@minecraft/server");
            server.world.afterEvents.playerSpawn.subscribe(function (event) {
                nrz.log("scout: player spawned: " + event.player.name);
            });
            nrz.log("scout: subscribed to the game's events");
        });
    }
})();
