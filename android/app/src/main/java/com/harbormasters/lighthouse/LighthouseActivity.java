package com.harbormasters.lighthouse;

import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

import org.libsdl.app.SDLActivity;

/**
 * Unpacks the shipped read-only data before SDL starts the game.
 *
 * <p>APK assets are not files, and libultraship resolves both {@code GetAppBundlePath()} and
 * {@code GetAppDirectoryPath()} to the external files directory on Android, so there is no
 * read-only location for the engine to fall back to. Everything therefore has to be copied out
 * once, into the same directory that holds saves, mods and the user's own bk.o2r.
 */
public class LighthouseActivity extends SDLActivity {
    private static final String TAG = "Lighthouse";
    private static final String STAMP = ".unpacked";

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
    }

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
