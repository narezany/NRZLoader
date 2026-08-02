// Панель — a window of buttons that floats over the game.
//
// It is also the shortest demonstration of what the loader can do that does
// not depend on the game at all: a window owned by the system, described in
// html, whose buttons reach straight into the loader. Nothing here touches
// Minecraft's own interface, so nothing here breaks when the game updates.

(function () {
    "use strict";

    if (typeof nrz === "undefined" || !nrz.window) return;

    // Only one context should own the panel; the game runs several, and each
    // of them would otherwise open a window of its own.
    if (globalThis.__nrz_panel_open) return;
    globalThis.__nrz_panel_open = true;

    // Whatever the user set in the launcher. The launcher writes this file
    // from what mod.json declared, so reading it needs nothing special.
    var options = nrz.readJson("config/nrz.panel.json") || {};
    var left = options.x === undefined ? 24 : Math.round(options.x);
    var top = options.y === undefined ? 120 : Math.round(options.y);

    // A shader written here rather than chosen from a list: the loader wraps
    // it, builds it on the drawing thread, and it takes over the picture.
    var THERMAL = [
        "uniform float heat;",
        "vec3 effect(vec2 uv) {",
        "    vec3 c = texture2D(uTex, uv).rgb;",
        "    float v = dot(c, vec3(0.299, 0.587, 0.114));",
        "    v = clamp(v * (0.6 + heat), 0.0, 1.0);",
        // A cheap thermal ramp: black, blue, red, yellow, white.
        "    vec3 cold = mix(vec3(0.0, 0.0, 0.25), vec3(0.7, 0.0, 0.4), smoothstep(0.0, 0.4, v));",
        "    vec3 warm = mix(vec3(1.0, 0.35, 0.0), vec3(1.0, 1.0, 0.85), smoothstep(0.7, 1.0, v));",
        "    return mix(cold, warm, smoothstep(0.35, 0.75, v));",
        "}",
    ].join("\n");

    var presets = {
        "ничего": "fx",
        "crt": "fx crt=0.7",
        "рыбий глаз": "fx fisheye=0.6",
        "плёнка": "fx vignette=0.8,chroma=0.15",
    };

    var start = presets[options.startWith];
    if (start) {
        nrz.fx.set(start === "fx" ? "" : start.slice(3));
    }

    if (options.showPanel === false) {
        nrz.log("panel: the window is switched off in the settings");
        return;
    }

    var state = nrz.window.info();
    if (!state.available) {
        nrz.log("panel: " + (state.reason || "windows are unavailable"));
        // The panel is the whole mod, so there is nothing else to do, but the
        // effects still work from the config file.
        return;
    }

    // Every button is one line: a label, and what the loader should do. The
    // page sends these back with the loader's prefix, which means the loader
    // handles them itself and the mod needs no timer.
    var buttons = [
        ["CRT", "fx crt=0.7"],
        ["Рыбий глаз", "fx fisheye=0.6"],
        ["Помехи", "fx glitch=0.5,chroma=0.4"],
        ["Пиксели", "fx pixelate=0.5"],
        ["Волна", "fx wave=0.6"],
        ["Ч/б", "fx grayscale=1"],
        ["Кино", "fx vignette=0.8,chroma=0.15"],
        ["Выключить", "fx"],
    ];

    function escape(text) {
        return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    var rows = buttons.map(function (button) {
        return '<button onclick="send(\'' + button[1] + '\')">' + escape(button[0]) + "</button>";
    }).join("");

    var page = [
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>",
        "<style>",
        "  * { box-sizing: border-box; }",
        "  body { margin: 0; font: 13px/1.3 sans-serif; color: #fff; }",
        "  .card { background: rgba(20,20,24,.86); border-radius: 14px; padding: 10px;",
        "          backdrop-filter: blur(6px); }",
        "  .title { font-size: 11px; letter-spacing: .08em; text-transform: uppercase;",
        "           opacity: .55; margin: 0 4px 8px; display: flex; justify-content: space-between; }",
        "  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }",
        "  button { appearance: none; border: 0; border-radius: 9px; padding: 9px 6px;",
        "           background: rgba(255,255,255,.1); color: #fff; font: inherit; }",
        "  button:active { background: rgba(160,200,255,.35); }",
        "  button.wide { grid-column: 1 / -1; background: rgba(255,140,60,.22); }",
        "  .close { background: none; padding: 0 4px; opacity: .55; }",
        "</style>",
        "<div class=card>",
        "  <p class=title><span>Эффекты</span><button class=close onclick=\"send('close')\">✕</button></p>",
        "  <div class=grid>",
        rows,
        // The mod's own shader, which no list of built-in effects contains.
        "    <button class=wide onclick=\"thermal()\">Тепловизор (свой шейдер)</button>",
        "  </div>",
        "</div>",
        "<script>",
        "  function send(command) { nrzhost.send('nrz:' + command); }",
        "  function thermal() { nrzhost.send('thermal'); }",
        "<\/script>",
    ].join("\n");

    var result = nrz.window.open("nrz.panel", {
        x: left,
        y: top,
        width: 320,
        height: 0,        // as tall as the buttons need
        touchable: true,  // it is a control panel, so it takes taps
        html: page,
    });

    if (result) {
        nrz.log("panel: could not open the window: " + result);
        return;
    }

    // The thermal button is the one thing the loader cannot handle by itself,
    // so it comes back here as an ordinary message. Contexts that offer a
    // timer poll for it; the rest still get every other button.
    function collect() {
        var messages = nrz.window.poll("nrz.panel");
        for (var index = 0; index < messages.length; index++) {
            if (messages[index] !== "thermal") continue;

            nrz.fx.uniform("heat", 0.4);
            nrz.fx.shader(THERMAL);
            nrz.log("panel: switched to the mod's own shader");
        }
    }

    if (typeof setInterval === "function") {
        setInterval(collect, 500);
        nrz.log("panel: window is up, watching for its own buttons");
    } else {
        nrz.log("panel: window is up; no timer here, so only loader buttons work");
    }
})();
