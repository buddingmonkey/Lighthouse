package com.harbormasters.lighthouse;

import android.content.Intent;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDLActivity;

/**
 * Unpacks the shipped read-only data before SDL starts the game, and serves the file picker.
 *
 * <p>APK assets are not files, and libultraship resolves both {@code GetAppBundlePath()} and
 * {@code GetAppDirectoryPath()} to the external files directory on Android, so there is no
 * read-only location for the engine to fall back to. Everything therefore has to be copied out
 * once, into the same directory that holds saves, mods and the user's own bk.o2r.
 *
 * <p>The same directory is closed to the Files app and to the Storage Access Framework since
 * Android 11, so a ROM cannot be put there by hand. The game asks for one through the system
 * picker instead, and the chosen document is copied in. See {@code src/port/FilePicker.cpp}.
 *
 * <p>It also owns the window: {@code SDLActivity.onCreate} calls {@code setWindowStyle(false)},
 * which undoes the fullscreen theme, and the game never makes the SDL window fullscreen, so SDL's
 * own immersive path never runs. What is left of the screen is reported to the touch controls.
 */
public class LighthouseActivity extends SDLActivity {
    private static final String TAG = "Lighthouse";
    private static final String STAMP = ".unpacked";
    private static final int REQUEST_PICK_FILE = 1;
    /** The picked document is copied here, because the game reads paths and not content URIs. */
    private static final String IMPORT_DIR = "import";
    private static final String FALLBACK_IMPORT_NAME = "import.tmp";

    /** Files and directories copied out of the APK, relative to the assets root. */
    private static final String[] SHIPPED = {
        "lighthouse.o2r",
        "config.yml",
        "gamecontrollerdb.txt",
        "assets/yaml",
    };

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        try {
            unpackAssets();
        } catch (IOException e) {
            Log.e(TAG, "Could not unpack the shipped assets", e);
        }
        super.onCreate(savedInstanceState);
        // SDLActivity posts setWindowStyle(false) to this same looper, so queue behind it.
        mLayout.post(this::goImmersive);
        mLayout.setOnApplyWindowInsetsListener((view, insets) -> {
            reportInsets(view, insets);
            return view.onApplyWindowInsets(insets);
        });
        mLayout.requestApplyInsets();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            // A dialog, the picker or a transient swipe all bring the bars back.
            goImmersive();
        }
    }

    /** Hides the status and navigation bars until the user swipes an edge. */
    private void goImmersive() {
        Window window = getWindow();
        // SDLActivity sets this to force the status bar on; it beats every other request.
        window.clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            window.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    /**
     * Tells the game which edges it may not draw controls on, and asks the system not to read a
     * back gesture where the on-screen pad is.
     */
    private void reportInsets(View view, WindowInsets insets) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            nativeSafeAreaInsets(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                                 insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return;
        }
        // A hidden bar reports zero, so this is the cutout plus anything the user keeps on.
        android.graphics.Insets reserved =
            insets.getInsets(WindowInsets.Type.displayCutout() | WindowInsets.Type.systemBars());
        nativeSafeAreaInsets(reserved.left, reserved.top, reserved.right, reserved.bottom);

        // The stick is swept from the bottom corner, which is where a back gesture starts. The
        // system keeps 200 dp of each edge at most and drops the rest, nearest the bottom first.
        if (view.getWidth() <= 0 || view.getHeight() <= 0) {
            return;
        }
        android.graphics.Insets gestures = insets.getInsets(WindowInsets.Type.systemGestures());
        List<Rect> exclusions = new ArrayList<>();
        if (gestures.left > 0) {
            exclusions.add(new Rect(0, 0, gestures.left, view.getHeight()));
        }
        if (gestures.right > 0) {
            exclusions.add(new Rect(view.getWidth() - gestures.right, 0, view.getWidth(), view.getHeight()));
        }
        view.setSystemGestureExclusionRects(exclusions);
    }

    private static native void nativeSafeAreaInsets(int left, int top, int right, int bottom);

    /** Called from the game thread. The answer goes back through {@link #nativeFilePicked}. */
    public void openFilePicker() {
        runOnUiThread(() -> {
            // No MIME type is registered for .z64, and filtering on a guess would leave the ROM
            // unselectable wherever a provider reports something else.
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            try {
                startActivityForResult(pick, REQUEST_PICK_FILE);
            } catch (Exception e) {
                Log.e(TAG, "No document picker available", e);
                nativeFilePicked(null);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK_FILE) {
            return;
        }
        Uri source = (resultCode == RESULT_OK && data != null) ? data.getData() : null;
        if (source == null) {
            nativeFilePicked(null);
            return;
        }
        // A ROM is about 32 MB, so the copy stays off the UI thread.
        new Thread(() -> nativeFilePicked(importDocument(source)), "FileImport").start();
    }

    /** Copies a picked document into the app directory; null when it could not be read. */
    private String importDocument(Uri source) {
        // Its own directory, emptied first: the game names the file it works on, and one import
        // is never worth keeping once the next one is made.
        File files = getExternalFilesDir(null);
        if (files == null) {
            Log.e(TAG, "No external files directory to import into");
            return null;
        }
        File dir = new File(files, IMPORT_DIR);
        File[] previous = dir.listFiles();
        if (previous != null) {
            for (File file : previous) {
                file.delete();
            }
        }
        File target = new File(dir, documentName(source));
        File partial = new File(target.getPath() + ".part");
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) {
                throw new IOException("Could not create " + dir);
            }
            copy(source, partial);
            // Rename last, so a failed copy never looks like a complete file.
            if (!partial.renameTo(target)) {
                throw new IOException("Could not move " + partial + " into place");
            }
            return target.getPath();
        } catch (IOException e) {
            Log.e(TAG, "Could not import " + source, e);
            partial.delete();
            return null;
        }
    }

    /** What the provider calls the document, made safe to use as a file name. */
    private String documentName(Uri source) {
        String name = null;
        try (Cursor cursor = getContentResolver().query(source, new String[] { OpenableColumns.DISPLAY_NAME }, null,
                                                        null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                name = new File(cursor.getString(0)).getName();
            }
        } catch (Exception e) {
            Log.w(TAG, "Could not read the name of " + source, e);
        }
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")) {
            return FALLBACK_IMPORT_NAME;
        }
        return name.replaceAll("[^A-Za-z0-9._-]", "_");
    }

    private void copy(Uri source, File target) throws IOException {
        try (InputStream in = getContentResolver().openInputStream(source)) {
            if (in == null) {
                throw new IOException("Could not open " + source);
            }
            try (OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[256 * 1024];
                int read;
                while ((read = in.read(buffer)) != -1) {
                    out.write(buffer, 0, read);
                }
            }
        }
    }

    private static native void nativeFilePicked(String path);

    /** Copies {@link #SHIPPED} into the external files directory, once per installed version. */
    private void unpackAssets() throws IOException {
        File target = getExternalFilesDir(null);
        if (target == null) {
            throw new IOException("No external files directory");
        }
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("Could not create " + target);
        }

        String version = String.valueOf(getVersionCode());
        File stamp = new File(target, STAMP);
        if (stamp.isFile() && version.equals(readText(stamp))) {
            return;
        }

        AssetManager assets = getAssets();
        for (String path : SHIPPED) {
            copyAsset(assets, path, new File(target, path));
        }
        writeText(stamp, version);
        Log.i(TAG, "Unpacked shipped assets for version " + version);
    }

    private long getVersionCode() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).getLongVersionCode();
        } catch (Exception e) {
            return 0L;
        }
    }

    /** Copies one asset, or every asset below it when the path names a directory. */
    private void copyAsset(AssetManager assets, String path, File target) throws IOException {
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            if (!target.isDirectory() && !target.mkdirs()) {
                throw new IOException("Could not create " + target);
            }
            for (String child : children) {
                copyAsset(assets, path + "/" + child, new File(target, child));
            }
            return;
        }

        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Could not create " + parent);
        }
        try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
        }
    }

    private static String readText(File file) {
        try (InputStream in = new java.io.FileInputStream(file)) {
            byte[] bytes = new byte[(int) Math.min(file.length(), 64L)];
            int read = in.read(bytes);
            return read <= 0 ? "" : new String(bytes, 0, read, StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }
}
