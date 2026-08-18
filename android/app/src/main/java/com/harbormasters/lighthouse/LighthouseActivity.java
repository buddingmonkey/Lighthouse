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
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import org.libsdl.app.SDLActivity;

/**
 * Unpacks the shipped read-only data before SDL starts the game, and serves the file picker.
 * It also owns the window: it hides the system bars and reports the safe area to the game.
 */
public class LighthouseActivity extends SDLActivity {
    private static final String TAG = "Lighthouse";
    private static final String STAMP = ".unpacked";
    private static final int REQUEST_PICK_FILE = 1;
    /** The picked document is copied here: the game reads paths, not content URIs. */
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

    /** Tells the game which edges it may not draw on, and keeps the back gesture off the pad. */
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

        // The system keeps at most 200 dp of each edge and drops the rest, nearest the bottom first.
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
            // No MIME type exists for .z64, and a guess would make the ROM unselectable.
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

    /** Returns the path of the copy, or null when the document could not be read. */
    private String importDocument(Uri source) {
        // Its own directory, emptied first: an import is never worth keeping after the next one.
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

    /** Copies {@link #SHIPPED} into the external files directory whenever the shipped bytes change. */
    private void unpackAssets() throws IOException {
        File target = getExternalFilesDir(null);
        if (target == null) {
            throw new IOException("No external files directory");
        }
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("Could not create " + target);
        }

        String fingerprint = shippedFingerprint();
        File stamp = new File(target, STAMP);
        if (stamp.isFile() && fingerprint.equals(readText(stamp))) {
            return;
        }
        // Removed first, so a copy that stops part way is done again rather than called complete.
        stamp.delete();

        AssetManager assets = getAssets();
        for (String path : SHIPPED) {
            copyAsset(assets, path, new File(target, path));
        }
        writeText(stamp, fingerprint);
        Log.i(TAG, "Unpacked shipped assets " + fingerprint);
    }

    /**
     * The size and CRC of every shipped APK entry, read from the zip directory. The version code
     * cannot answer this: the archives are built by a host tree and no version number counts them.
     * Nothing is inflated, and the answer comes from the installed file rather than from the build.
     */
    private String shippedFingerprint() throws IOException {
        List<String> entries = new ArrayList<>();
        try (ZipFile apk = new ZipFile(getApplicationInfo().sourceDir)) {
            for (Enumeration<? extends ZipEntry> e = apk.entries(); e.hasMoreElements();) {
                ZipEntry entry = e.nextElement();
                if (entry.isDirectory()) {
                    continue;
                }
                for (String root : SHIPPED) {
                    String prefix = "assets/" + root;
                    if (entry.getName().equals(prefix) || entry.getName().startsWith(prefix + "/")) {
                        entries.add(entry.getName() + ":" + entry.getSize() + ":" + entry.getCrc());
                        break;
                    }
                }
            }
        }
        // Zip order is not promised to hold from one build to the next, and an order the digest
        // can see would ask for a copy at random.
        Collections.sort(entries);
        CRC32 digest = new CRC32();
        for (String entry : entries) {
            digest.update(entry.getBytes(StandardCharsets.UTF_8));
        }
        // readText reads 64 bytes, so the stamp stays short.
        return String.format("%08x.%d", digest.getValue(), entries.size());
    }

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
