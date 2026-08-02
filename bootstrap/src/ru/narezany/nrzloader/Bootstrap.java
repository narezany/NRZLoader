package ru.narezany.nrzloader;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

/**
 * Loads the mod loader into the game process.
 *
 * A content provider is used rather than an Application subclass because the
 * game already has one of its own, and replacing it would break the game.
 * Providers are created before any activity runs and alongside whatever
 * Application the app declares, so this neither conflicts with the game nor
 * arrives too late to be useful.
 */
public final class Bootstrap extends ContentProvider {
    private static final String TAG = "NRZLoader";

    /**
     * Handed the provider's context, which is the app itself.
     *
     * Without it the loader has to catch an activity through whatever java
     * entry point the game happens to call, and on a retail build those are
     * mostly input handlers: the first one to arrive can be a back press
     * minutes into a session. This arrives before the game draws anything.
     */
    private static native void nativeAttach(Object context);

    @Override
    public boolean onCreate() {
        try {
            System.loadLibrary("nrzloader");
            Log.i(TAG, "loader library loaded");
        } catch (Throwable error) {
            // A failure here must not stop the game from starting.
            Log.e(TAG, "could not load the loader: " + error);
            return true;
        }

        try {
            nativeAttach(getContext());
        } catch (Throwable error) {
            Log.e(TAG, "could not hand over the context: " + error);
        }
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        return 0;
    }
}
