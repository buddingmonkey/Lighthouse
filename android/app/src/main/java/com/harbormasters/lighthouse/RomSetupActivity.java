package com.harbormasters.lighthouse;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

/**
 * Takes a ROM from the player before the game starts.
 *
 * <p>Android 11 hid {@code Android/data} from the Files app and from the Storage Access Framework
 * alike, so a player has no way to put a ROM into the app's own directory by hand. The system
 * picker still reads from everywhere else, so the file is chosen there and copied in. This runs
 * as its own launcher activity to keep SDL's lifecycle in {@link LighthouseActivity} untouched.
 */
public class RomSetupActivity extends Activity {
    private static final String TAG = "Lighthouse";
    private static final int REQUEST_ROM = 1;
    private static final String[] ROM_EXTENSIONS = { ".z64", ".n64", ".v64" };
    /** The engine loads this one without asking again. */
    private static final String IMPORTED_ROM_NAME = "baserom.us.z64";

    /** First word of an N64 image, which says nothing about the game but everything about byte order. */
    private static final byte[] MAGIC_Z64 = { (byte) 0x80, 0x37, 0x12, 0x40 };
    private static final byte[] MAGIC_V64 = { 0x37, (byte) 0x80, 0x40, 0x12 };
    private static final byte[] MAGIC_N64 = { 0x40, 0x12, 0x37, (byte) 0x80 };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (hasGameData()) {
            startGame();
            return;
        }
        explain(null);
    }

    /** Says what is wanted and why before the picker appears over the top of everything. */
    private void explain(String problem) {
        String message = getString(R.string.rom_needed_message);
        if (problem != null) {
            message = problem + "\n\n" + message;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.rom_needed_title)
                .setMessage(message)
                .setCancelable(false)
                .setPositiveButton(R.string.rom_needed_choose, (dialog, which) -> openPicker())
                .setNegativeButton(android.R.string.cancel, (dialog, which) -> finish())
                .show();
    }

    private void openPicker() {
        // No MIME type is registered for .z64, and filtering on a guess would leave the ROM
        // unselectable wherever a provider reports something else. Take anything and check the
        // header instead, which also allows a precise complaint.
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        try {
            startActivityForResult(pick, REQUEST_ROM);
        } catch (Exception e) {
            Log.e(TAG, "No document picker available", e);
            startGame();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_ROM) {
            return;
        }

        Uri source = (resultCode == RESULT_OK && data != null) ? data.getData() : null;
        if (source == null) {
            explain(null); // The picker was dismissed; there is still nothing to play.
            return;
        }

        File target = new File(getExternalFilesDir(null), IMPORTED_ROM_NAME);
        File partial = new File(target.getPath() + ".part");
        try {
            copy(source, partial);
            String problem = romProblem(partial);
            if (problem != null) {
                partial.delete();
                explain(problem);
                return;
            }
            // Rename last, so a cancelled or rejected copy never looks like a usable ROM.
            if (!partial.renameTo(target)) {
                throw new IOException("Could not move the ROM into place");
            }
            Log.i(TAG, "Imported a ROM to " + target);
            startGame();
        } catch (IOException e) {
            Log.e(TAG, "Could not import the ROM", e);
            partial.delete();
            explain(getString(R.string.rom_import_failed));
        }
    }

    /** Null when the file looks like a big-endian N64 image, otherwise what to tell the player. */
    private String romProblem(File file) throws IOException {
        byte[] header = new byte[4];
        try (InputStream in = new java.io.FileInputStream(file)) {
            if (in.read(header) != header.length) {
                return getString(R.string.rom_not_a_rom);
            }
        }
        if (matches(header, MAGIC_Z64)) {
            return null;
        }
        if (matches(header, MAGIC_V64) || matches(header, MAGIC_N64)) {
            return getString(R.string.rom_wrong_format);
        }
        return getString(R.string.rom_not_a_rom);
    }

    private static boolean matches(byte[] header, byte[] magic) {
        for (int i = 0; i < magic.length; i++) {
            if (header[i] != magic[i]) {
                return false;
            }
        }
        return true;
    }

    /** True once there is either an extracted archive or a ROM to extract from. */
    private boolean hasGameData() {
        File dir = getExternalFilesDir(null);
        if (dir == null) {
            return false;
        }
        if (new File(dir, "bk.o2r").isFile()) {
            return true;
        }
        File[] files = dir.listFiles();
        if (files == null) {
            return false;
        }
        for (File file : files) {
            String name = file.getName().toLowerCase(Locale.ROOT);
            for (String extension : ROM_EXTENSIONS) {
                if (name.endsWith(extension)) {
                    return true;
                }
            }
        }
        return false;
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

    private void startGame() {
        startActivity(new Intent(this, LighthouseActivity.class));
        finish();
    }
}
