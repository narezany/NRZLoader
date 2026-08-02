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
        "  .close { background: none; padding: 0 4px; opacity: .55; }",
        "</style>",
        "<div class=card>",
        "  <p class=title><span>Эффекты</span><button class=close onclick=\"send('close')\">✕</button></p>",
        "  <div class=grid>", rows, "</div>",
        "</div>",
        "<script>",
        "  function send(command) { nrzhost.send('nrz:' + command); }",
        "<\/script>",
    ].join("\n");

    var result = nrz.window.open("nrz.panel", {
        x: 24,
        y: 120,
        width: 320,
        height: 0,        // as tall as the buttons need
        touchable: true,  // it is a control panel, so it takes taps
        html: page,
    });

    if (result) {
        nrz.log("panel: could not open the window: " + result);
    } else {
        nrz.log("panel: window is up, effects: " + (nrz.fx.info().names || ""));
    }
})();
