// A JavaScript mod. Drop it next to the native mods, in
//   /sdcard/Android/data/com.mojang.minecraftpe/files/mods/
// and the loader runs it once the game's own scripting engine has started.
//
// Everything here runs inside the game's engine, so the modules Minecraft
// exposes to its own scripts are reachable the usual way. What is available
// depends on the build; the try/catch below keeps a missing module from
// taking the whole file down.

(function () {
    "use strict";

    function report(text) {
        // console goes to logcat, visible with:  adb logcat -s chromium:V
        try {
            console.warn("[hello.js] " + text);
        } catch (error) {
            // Older engines expose print() instead.
            if (typeof print === "function") print("[hello.js] " + text);
        }
    }

    report("loaded, engine is alive");
    report("globals: " + Object.getOwnPropertyNames(globalThis).slice(0, 20).join(", "));

    // Installed by the loader, not by Minecraft. An add-on can do none of
    // this: files, settings of its own, a way back into native code.
    if (typeof nrz !== "undefined") {
        nrz.log("hello.js speaking from the game's javascript");
        report("mods folder: " + nrz.modsDir());

        var seen = (nrz.readJson("hello-state.json") || { launches: 0 });
        seen.launches += 1;
        nrz.writeJson("hello-state.json", seen);
        report("launch number " + seen.launches);
    } else {
        report("no native bridge in this context");
    }

    // The scripting API arrives as a module rather than a global. Whether this
    // resolves depends on the world having script support active.
    try {
        const server = require("@minecraft/server");
        report("@minecraft/server is available");

        server.world.afterEvents.playerSpawn.subscribe(function (event) {
            report("player spawned: " + event.player.name);
            event.player.sendMessage("Загрузчик работает.");
        });
    } catch (error) {
        report("@minecraft/server not reachable here: " + error);
    }
})();
