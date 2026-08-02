package ru.narezany.nrzloader;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

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
        if (!loadLoader()) return true;

        try {
            nativeAttach(getContext());
        } catch (Throwable error) {
            Log.e(TAG, "could not hand over the context: " + error);
        }
        return true;
    }

    /**
     * Загружает загрузчик — по возможности тот, что лежит рядом с модами.
     *
     * Внутри игры лежит своя копия, вшитая при сборке. Но пересобирать
     * полугигабайтный пакет ради правки в мегабайтной библиотеке — занятие
     * тяжёлое, а на телефоне ещё и долгое. Поэтому сначала проверяется копия
     * в папке загрузчика: лаунчер кладёт её туда при обновлении, и игре
     * достаточно перезапуска.
     *
     * Прямо из общей памяти запускать код нельзя, поэтому файл сперва
     * переносится в собственный каталог приложения. Это же значит, что игра
     * исполняет код, лежащий там, куда может писать любое приложение с
     * доступом к файлам, — ровно как и нативные моды, которые она и так
     * оттуда берёт.
     */
    private boolean loadLoader() {
        File external = new File(Environment.getExternalStorageDirectory(),
                "NRZLoader/libnrzloader.so");

        if (external.isFile() && external.length() > 0) {
            try {
                File own = copyIntoOwnDirectory(external);
                System.load(own.getAbsolutePath());
                Log.i(TAG, "loader taken from " + external + ", " + external.length() + " bytes");
                return true;
            } catch (Throwable error) {
                Log.w(TAG, "could not use the loader from storage: " + error);
            }
        }

        try {
            System.loadLibrary("nrzloader");
            Log.i(TAG, "loader library loaded from the package");
            return true;
        } catch (Throwable error) {
            // A failure here must not stop the game from starting.
            Log.e(TAG, "could not load the loader: " + error);
            return false;
        }
    }

    /** Переносит библиотеку туда, откуда её разрешено запускать. */
    private File copyIntoOwnDirectory(File source) throws IOException {
        File target = new File(getContext().getFilesDir(), "nrzloader-" + source.length() + ".so");

        // Имя содержит размер, поэтому другая сборка попадёт в другой файл, а
        // ту же самую копировать заново незачем.
        if (target.isFile() && target.length() == source.length()) return target;

        // Старые копии больше не нужны: они остаются занятыми, только пока
        // игра не перезапущена.
        File[] old = getContext().getFilesDir().listFiles();
        if (old != null) {
            for (File file : old) {
                if (file.getName().startsWith("nrzloader-")) file.delete();
            }
        }

        InputStream in = new FileInputStream(source);
        try {
            OutputStream out = new FileOutputStream(target);
            try {
                byte[] buffer = new byte[1 << 16];
                int read;
                while ((read = in.read(buffer)) > 0) out.write(buffer, 0, read);
            } finally {
                out.close();
            }
        } finally {
            in.close();
        }
        return target;
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
