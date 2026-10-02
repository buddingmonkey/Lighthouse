package com.harbormasters.lighthouse;

import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Rect;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.hardware.display.DisplayManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ResultReceiver;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;

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

public class LighthouseActivity extends SDLActivity {
    private static final String TAG = "Lighthouse";
    private static final String STAMP = ".unpacked";
    private static final String MOVED = ".moved";
    private static final int REQUEST_PICK_FILE = 1;
    private static final String IMPORT_DIR = "import";
    private static final String FALLBACK_IMPORT_NAME = "import.tmp";
    private static final String FEATURE_HINGE_ANGLE = "android.hardware.sensor.hinge_angle";
    private static final int LARGE_SCREEN_DP = 600;
    private static final long PICKER_FOCUS_CHECK_MS = 500;

    private static final String[] SHIPPED = {
        "lighthouse.o2r",
        "config.yml",
        "gamecontrollerdb.txt",
        "assets/yaml",
    };

    // A preloaded build carries these too; a normal build has neither.
    private static final String[] BUNDLED = {
        "bk.o2r",
        "mods",
    };

    private volatile File dataDir;
    private volatile int softKeyboardResult = -1;
    private int wantedScreen = -1;
    private int screenArt = -1;
    private boolean started;
    private int loggedOrientation = -1;
    private DisplayManager.DisplayListener screenListener;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        dataDir = dataDir();
        moveLegacyData(dataDir);
        try {
            unpackAssets(dataDir);
        } catch (IOException e) {
            Log.e(TAG, "Could not unpack the shipped assets", e);
        }
        super.onCreate(savedInstanceState);
        mLayout.post(this::goImmersive);
        mLayout.setOnApplyWindowInsetsListener((view, insets) -> {
            reportInsets(view, insets);
            return view.onApplyWindowInsets(insets);
        });
        mLayout.requestApplyInsets();
        applyOrientation(getResources().getConfiguration());
        watchScreens();
    }

    @Override
    public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        applyOrientation(config);
        reportScreens();
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        runOnUiThread(() -> applyOrientation(getResources().getConfiguration()));
    }

    private boolean hasHinge() {
        if (getPackageManager().hasSystemFeature(FEATURE_HINGE_ANGLE)) {
            return true;
        }
        SensorManager sensors = getSystemService(SensorManager.class);
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && sensors != null
            && sensors.getDefaultSensor(Sensor.TYPE_HINGE_ANGLE) != null;
    }

    private int screenSmallestDp(Configuration config) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return config.smallestScreenWidthDp;
        }
        Rect bounds = getWindowManager().getMaximumWindowMetrics().getBounds();
        float density = getResources().getDisplayMetrics().density;
        return Math.round(Math.min(bounds.width(), bounds.height()) / density);
    }

    private void applyOrientation(Configuration config) {
        int smallest = screenSmallestDp(config);
        boolean free = smallest >= LARGE_SCREEN_DP && hasHinge();
        int wanted = free ? ActivityInfo.SCREEN_ORIENTATION_FULL_USER
                          : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        if (loggedOrientation != wanted) {
            Log.i(TAG, "Screen smallest " + smallest + " dp, window smallest " + config.smallestScreenWidthDp
                  + " dp: " + (free ? "all orientations" : "landscape only"));
            loggedOrientation = wanted;
        }
        if (getRequestedOrientation() != wanted) {
            setRequestedOrientation(wanted);
        }
    }

    private void watchScreens() {
        DisplayManager displays = getSystemService(DisplayManager.class);
        screenListener = new DisplayManager.DisplayListener() {
            @Override
            public void onDisplayAdded(int displayId) {
                reportScreens();
            }

            @Override
            public void onDisplayRemoved(int displayId) {
                reportScreens();
            }

            @Override
            public void onDisplayChanged(int displayId) {
            }
        };
        displays.registerDisplayListener(screenListener, null);
        reportScreens();
    }

    private List<Display> gameScreens() {
        List<Display> screens = new ArrayList<>();
        Intent game = new Intent(this, LighthouseActivity.class);
        for (Display display : getSystemService(DisplayManager.class).getDisplays()) {
            int id = display.getDisplayId();
            if (id == Display.DEFAULT_DISPLAY) {
                screens.add(0, display);
            } else if ((display.getFlags() & Display.FLAG_PRIVATE) == 0
                       && getSystemService(ActivityManager.class).isActivityStartAllowedOnDisplay(this, id, game)) {
                screens.add(display);
            }
        }
        return screens;
    }

    private int currentDisplayId() {
        return getWindowManager().getDefaultDisplay().getDisplayId();
    }

    private void reportScreens() {
        List<Display> screens = gameScreens();
        int current = -1;
        StringBuilder names = new StringBuilder();
        for (int i = 0; i < screens.size(); i++) {
            Display display = screens.get(i);
            if (display.getDisplayId() == currentDisplayId()) {
                current = i;
            }
            names.append(i == 0 ? "" : ", ").append(display.getDisplayId()).append(' ').append(display.getName());
        }
        Log.i(TAG, "Game screens: " + names + "; game on screen " + current);
        int wanted = wantedScreen;
        wantedScreen = -1;
        if (wanted >= 0 && wanted < screens.size() && wanted != current) {
            showOnScreen(screens.get(wanted));
        } else if (!mBrokenLibraries) {
            nativeGameScreens(screens.size(), Math.max(current, 0));
            showScreenArt(screens);
        }
    }

    public void setScreenArt(int index) {
        runOnUiThread(() -> {
            screenArt = index;
            showScreenArt(gameScreens());
        });
    }

    private void showScreenArt(List<Display> screens) {
        Display free = null;
        for (Display display : screens) {
            if (display.getDisplayId() != currentDisplayId()) {
                free = display;
                break;
            }
        }
        if (started && free != null && ScreenArtActivity.hasImage(screenArt)) {
            ScreenArtActivity.show(this, currentDisplayId(), free, screenArt);
        } else {
            ScreenArtActivity.hide();
        }
    }

    public void setGameScreen(int index) {
        runOnUiThread(() -> {
            wantedScreen = index;
            reportScreens();
        });
    }

    private void showOnScreen(Display display) {
        Log.i(TAG, "Moving the game to screen " + display.getDisplayId() + " " + display.getName());
        ActivityOptions options = ActivityOptions.makeBasic().setLaunchDisplayId(display.getDisplayId());
        startActivity(new Intent(this, LighthouseActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                      options.toBundle());
    }

    private static native void nativeGameScreens(int count, int current);

    public void probeSoftKeyboard() {
        softKeyboardResult = -1;
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm == null || mTextEdit == null) {
                softKeyboardResult = InputMethodManager.RESULT_UNCHANGED_HIDDEN;
                return;
            }
            imm.showSoftInput(mTextEdit, 0, new ResultReceiver(null) {
                @Override
                protected void onReceiveResult(int resultCode, Bundle resultData) {
                    softKeyboardResult = resultCode;
                }
            });
        });
    }

    public int softKeyboardResult() {
        return softKeyboardResult;
    }

    public int systemKeyboardTakesFocus() {
        return getPackageManager().hasSystemFeature("oculus.software.overlay_keyboard") ? 1 : 0;
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            goImmersive();
            showScreenArt(gameScreens());
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        started = true;
        showScreenArt(gameScreens());
    }

    @Override
    protected void onStop() {
        started = false;
        ScreenArtActivity.hide();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        boolean relaunch = isChangingConfigurations();
        int displayId = currentDisplayId();
        if (screenListener != null) {
            getSystemService(DisplayManager.class).unregisterDisplayListener(screenListener);
        }
        ScreenArtActivity.hide();
        super.onDestroy();
        if (relaunch) {
            Log.i(TAG, "Configuration change needs a new activity; starting a new process on screen " + displayId);
            ActivityOptions options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId);
            startActivity(new Intent(this, LighthouseActivity.class), options.toBundle());
        }
        System.exit(0);
    }

    private void goImmersive() {
        Window window = getWindow();
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

    private void reportInsets(View view, WindowInsets insets) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            nativeSafeAreaInsets(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                                 insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return;
        }
        android.graphics.Insets reserved =
            insets.getInsets(WindowInsets.Type.displayCutout() | WindowInsets.Type.systemBars());
        nativeSafeAreaInsets(reserved.left, reserved.top, reserved.right, reserved.bottom);

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

    // Android 11 closed Android/data to the Files app, to USB and to the document picker.
    // Android/media stayed open to all three and needs no permission. Ship::Context picks the
    // same folder by the same rule.
    private File dataDir() {
        File[] media = getExternalMediaDirs();
        if (media != null && media.length > 0 && media[0] != null
            && (media[0].isDirectory() || media[0].mkdirs())) {
            return media[0];
        }
        return getExternalFilesDir(null);
    }

    // A file the app cannot read must not strand the rest, so each entry is moved on its own and
    // the stamp waits until every one of them arrived.
    private void moveLegacyData(File target) {
        File legacy = getExternalFilesDir(null);
        if (target == null || legacy == null || legacy.equals(target)) {
            return;
        }
        File stamp = new File(target, MOVED);
        if (stamp.isFile()) {
            return;
        }
        boolean complete = true;
        File[] entries = legacy.listFiles();
        if (entries != null) {
            for (File entry : entries) {
                try {
                    move(entry, new File(target, entry.getName()));
                } catch (IOException e) {
                    Log.e(TAG, "Could not move " + entry, e);
                    complete = false;
                }
            }
        }
        if (!complete) {
            return;
        }
        try {
            writeText(stamp, legacy.getPath());
        } catch (IOException e) {
            Log.e(TAG, "Could not write " + stamp, e);
            return;
        }
        Log.i(TAG, "Moved the app folder out of " + legacy);
    }

    private static void move(File source, File target) throws IOException {
        if (target.exists() || source.renameTo(target)) {
            return;
        }
        if (source.isDirectory()) {
            if (!target.mkdirs()) {
                throw new IOException("Could not create " + target);
            }
            File[] children = source.listFiles();
            if (children != null) {
                for (File child : children) {
                    move(child, new File(target, child.getName()));
                }
            }
            source.delete();
            return;
        }
        copy(source, target);
        source.delete();
    }

    public void openFilePicker() {
        runOnUiThread(() -> {
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
        mLayout.postDelayed(this::takeBackFocus, PICKER_FOCUS_CHECK_MS);
        Uri source = (resultCode == RESULT_OK && data != null) ? data.getData() : null;
        if (source == null) {
            nativeFilePicked(null);
            return;
        }
        new Thread(() -> nativeFilePicked(importDocument(source)), "FileImport").start();
    }

    private void takeBackFocus() {
        if (hasWindowFocus() || isFinishing()) {
            return;
        }
        Log.i(TAG, "No window focus after the file picker; bringing the game to the front");
        ActivityOptions options = ActivityOptions.makeBasic().setLaunchDisplayId(currentDisplayId());
        startActivity(new Intent(this, LighthouseActivity.class), options.toBundle());
    }

    private String importDocument(Uri source) {
        File files = dataDir;
        if (files == null) {
            Log.e(TAG, "No app folder to import into");
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

    private static void copy(File source, File target) throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Could not create " + parent);
        }
        try (InputStream in = new java.io.FileInputStream(source); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[256 * 1024];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
        }
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

    private void unpackAssets(File target) throws IOException {
        if (target == null) {
            throw new IOException("No app folder");
        }
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("Could not create " + target);
        }

        String fingerprint = shippedFingerprint();
        File stamp = new File(target, STAMP);
        if (stamp.isFile() && fingerprint.equals(readText(stamp)) && shippedAssetsExist(target)) {
            return;
        }
        stamp.delete();

        AssetManager assets = getAssets();
        for (String path : SHIPPED) {
            copyAsset(assets, path, new File(target, path));
        }
        for (String path : BUNDLED) {
            copyAssetIfPresent(assets, path, new File(target, path));
        }
        writeText(stamp, fingerprint);
        Log.i(TAG, "Unpacked shipped assets " + fingerprint);
    }

    private boolean shippedAssetsExist(File target) {
        for (String path : SHIPPED) {
            File file = new File(target, path);
            if (!file.exists() || (file.isDirectory() && file.list() == null)) {
                return false;
            }
        }
        return true;
    }

    private String shippedFingerprint() throws IOException {
        List<String> roots = new ArrayList<>();
        Collections.addAll(roots, SHIPPED);
        Collections.addAll(roots, BUNDLED);
        List<String> entries = new ArrayList<>();
        try (ZipFile apk = new ZipFile(getApplicationInfo().sourceDir)) {
            for (Enumeration<? extends ZipEntry> e = apk.entries(); e.hasMoreElements();) {
                ZipEntry entry = e.nextElement();
                if (entry.isDirectory()) {
                    continue;
                }
                for (String root : roots) {
                    String prefix = "assets/" + root;
                    if (entry.getName().equals(prefix) || entry.getName().startsWith(prefix + "/")) {
                        entries.add(entry.getName() + ":" + entry.getSize() + ":" + entry.getCrc());
                        break;
                    }
                }
            }
        }
        Collections.sort(entries);
        CRC32 digest = new CRC32();
        for (String entry : entries) {
            digest.update(entry.getBytes(StandardCharsets.UTF_8));
        }
        return String.format("%08x.%d", digest.getValue(), entries.size());
    }

    private void copyAssetIfPresent(AssetManager assets, String path, File target) throws IOException {
        try {
            copyAsset(assets, path, target);
        } catch (java.io.FileNotFoundException e) {
            // Not in this build.
        }
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
