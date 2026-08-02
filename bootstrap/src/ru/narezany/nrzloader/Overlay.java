package ru.narezany.nrzloader;

import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.Map;

/**
 * Windows a mod can put on top of the game.
 *
 * A mod written in JavaScript has no way to draw anything: the engine it runs
 * in belongs to the game's interface, and adding to that means editing the
 * game's own files. A window owned by the system sits above all of it, is
 * described in html, and outlives whatever screen the game happens to be on.
 *
 * Every method here is called from native code and returns a plain value, so
 * the bridge on the other side needs no knowledge of Android.
 */
public final class Overlay {
    private static final String TAG = "NRZLoader";

    /** Windows currently on screen, by the name the mod gave them. */
    private static final Map<String, Window> windows = new HashMap<>();

    /** Messages the pages sent back, waiting to be collected. */
    private static final Map<String, ArrayDeque<String>> inbox = new HashMap<>();

    /** Messages meant for the loader rather than for the mod. */
    private static final String COMMAND_PREFIX = "nrz:";

    private static Context context;
    private static Handler main;

    /** Implemented in the loader library, which is already loaded by then. */
    private static native void nativeCommand(String id, String command);

    private Overlay() {
    }

    /** Handed the game's context by the loader, once, while the app starts. */
    public static void attach(Context newContext) {
        context = newContext.getApplicationContext();
        main = new Handler(Looper.getMainLooper());
    }

    /**
     * Whether the user has allowed windows on top of other apps.
     *
     * It is off by default and cannot be turned on from inside the app, so the
     * launcher sends the user to the system screen for it.
     */
    public static boolean allowed() {
        if (context == null) return false;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return true;
        return Settings.canDrawOverlays(context);
    }

    /**
     * Opens a window, or replaces the contents of one already open.
     *
     * @param html what to show; a whole document, styled however the mod likes
     * @return an empty string on success, otherwise why not
     */
    public static String open(final String id, final String html, final int x, final int y,
            final int width, final int height, final boolean touchable) {
        if (context == null || main == null) return "the loader has no context";
        if (!allowed()) return "not allowed to draw over other apps";
        if (id == null || id.isEmpty()) return "the window needs a name";

        onMain(new Runnable() {
            @Override
            public void run() {
                try {
                    Window window = windows.get(id);
                    if (window == null) {
                        window = new Window(id, touchable);
                        windows.put(id, window);
                    }
                    window.show(x, y, width, height, touchable);
                    window.load(html == null ? "" : html);
                } catch (Throwable error) {
                    Log.e(TAG, "could not open window " + id + ": " + error);
                }
            }
        });
        return "";
    }

    public static String close(final String id) {
        if (main == null) return "the loader has no context";

        onMain(new Runnable() {
            @Override
            public void run() {
                Window window = windows.remove(id);
                if (window != null) window.hide();
                inbox.remove(id);
            }
        });
        return "";
    }

    public static String closeAll() {
        if (main == null) return "";

        onMain(new Runnable() {
            @Override
            public void run() {
                for (Window window : windows.values()) window.hide();
                windows.clear();
                inbox.clear();
            }
        });
        return "";
    }

    /**
     * Replaces what an open window shows, leaving where it sits alone.
     *
     * Going back through open() would also reapply the position and size, so
     * a mod redrawing its readout every second would keep shoving the window
     * back to where it first put it.
     */
    public static String setHtml(final String id, final String html) {
        if (main == null) return "the loader has no context";

        onMain(new Runnable() {
            @Override
            public void run() {
                Window window = windows.get(id);
                if (window != null) window.load(html == null ? "" : html);
            }
        });
        return "";
    }

    /** Runs javascript inside an open window, for updating it in place. */
    public static String eval(final String id, final String script) {
        if (main == null) return "the loader has no context";

        onMain(new Runnable() {
            @Override
            public void run() {
                Window window = windows.get(id);
                if (window != null) window.eval(script);
            }
        });
        return "";
    }

    public static String move(final String id, final int x, final int y, final int width,
            final int height) {
        if (main == null) return "the loader has no context";

        onMain(new Runnable() {
            @Override
            public void run() {
                Window window = windows.get(id);
                if (window != null) window.move(x, y, width, height);
            }
        });
        return "";
    }

    /**
     * Everything the page sent since the last call, one message per line.
     *
     * Polling rather than calling back into the mod keeps this on whatever
     * thread the mod already runs on; a callback would arrive on the main
     * thread, in the middle of whatever the engine was doing.
     */
    public static String poll(String id) {
        synchronized (inbox) {
            ArrayDeque<String> queue = inbox.get(id);
            if (queue == null || queue.isEmpty()) return "";

            StringBuilder joined = new StringBuilder();
            while (!queue.isEmpty()) {
                if (joined.length() > 0) joined.append('\n');
                joined.append(queue.poll());
            }
            return joined.toString();
        }
    }

    /** The names of the windows currently open, one per line. */
    public static String list() {
        synchronized (windows) {
            StringBuilder joined = new StringBuilder();
            for (String id : windows.keySet()) {
                if (joined.length() > 0) joined.append('\n');
                joined.append(id);
            }
            return joined.toString();
        }
    }

    private static void onMain(Runnable work) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            work.run();
        } else {
            main.post(work);
        }
    }

    /**
     * A message a page inside a window sent.
     *
     * Anything starting with the loader's own prefix is handled by the loader
     * itself. That matters because a page has no other way to reach it: a mod
     * would have to be running a timer to notice the message, and the engine
     * a mod runs in does not always offer one. This way a window full of
     * buttons works on its own.
     */
    private static void deliver(String id, String message) {
        if (message.startsWith(COMMAND_PREFIX)) {
            try {
                nativeCommand(id, message.substring(COMMAND_PREFIX.length()));
                return;
            } catch (Throwable error) {
                Log.w(TAG, "command from " + id + " went nowhere: " + error);
                return;
            }
        }

        synchronized (inbox) {
            ArrayDeque<String> queue = inbox.get(id);
            if (queue == null) {
                queue = new ArrayDeque<String>();
                inbox.put(id, queue);
            }
            // A mod that never polls must not grow this without bound.
            while (queue.size() >= 256) queue.poll();
            queue.add(message);
        }
    }

    /** One window: a web view held up by the window manager. */
    private static final class Window {
        private final String id;
        private final WebView view;
        private final WindowManager manager;
        private WindowManager.LayoutParams layout;
        private boolean shown;

        Window(String id, boolean touchable) {
            this.id = id;
            this.manager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);

            view = new WebView(context);
            view.getSettings().setJavaScriptEnabled(true);
            view.setBackgroundColor(Color.TRANSPARENT);
            view.setWebViewClient(new WebViewClient());
            view.addJavascriptInterface(new Bridge(id), "nrzhost");
            if (!touchable) view.setEnabled(false);
        }

        void show(int x, int y, int width, int height, boolean touchable) {
            layout = new WindowManager.LayoutParams(
                    width > 0 ? width : WindowManager.LayoutParams.WRAP_CONTENT,
                    height > 0 ? height : WindowManager.LayoutParams.WRAP_CONTENT,
                    overlayType(),
                    flags(touchable),
                    PixelFormat.TRANSLUCENT);
            layout.gravity = Gravity.TOP | Gravity.START;
            layout.x = x;
            layout.y = y;

            if (shown) {
                manager.updateViewLayout(view, layout);
            } else {
                manager.addView(view, layout);
                shown = true;
            }
        }

        void move(int x, int y, int width, int height) {
            if (!shown || layout == null) return;
            layout.x = x;
            layout.y = y;
            if (width > 0) layout.width = width;
            if (height > 0) layout.height = height;
            manager.updateViewLayout(view, layout);
        }

        void load(String html) {
            // A data url would make the page opaque to javascript running in
            // it; a base url of null with html keeps it a normal document.
            view.loadDataWithBaseURL(null, html, "text/html", "utf-8", null);
        }

        void eval(String script) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
                view.evaluateJavascript(script, null);
            } else {
                view.loadUrl("javascript:" + script);
            }
        }

        void hide() {
            if (!shown) return;
            try {
                manager.removeView(view);
            } catch (Throwable error) {
                Log.w(TAG, "window " + id + " was already gone");
            }
            view.destroy();
            shown = false;
        }

        private static int overlayType() {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                return WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY;
            }
            return WindowManager.LayoutParams.TYPE_PHONE;
        }

        /**
         * A window that does not take touches lets every tap through to the
         * game, which is what a readout wants; one that does is a control
         * panel and has to receive them.
         */
        private static int flags(boolean touchable) {
            int base = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS;
            if (!touchable) base |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
            return base;
        }
    }

    /** What the page inside a window can call. */
    private static final class Bridge {
        private final String id;

        Bridge(String id) {
            this.id = id;
        }

        @JavascriptInterface
        public void send(String message) {
            deliver(id, message == null ? "" : message.replace('\n', ' '));
        }
    }
}
