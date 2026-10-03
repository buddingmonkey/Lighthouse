package com.harbormasters.lighthouse;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.ImageView;

import java.lang.ref.WeakReference;

public class ScreenArtActivity extends Activity {
    private static final String TAG = "Lighthouse";
    private static final int[] IMAGES = {
        R.drawable.screen_art_cover,
        R.drawable.screen_art_hero,
        R.drawable.screen_art_jiggies,
        R.drawable.screen_art_poster,
        R.drawable.screen_art_back,
        R.drawable.screen_art_box,
    };

    private static WeakReference<ScreenArtActivity> shown = new WeakReference<>(null);
    private static final String EXTRA_LAUNCH = "launch";
    private static int wantedImage = -1;
    private static int launch = 0;
    private static int launchDisplay = Display.INVALID_DISPLAY;
    private static int gameDisplay = Display.DEFAULT_DISPLAY;

    private ImageView view;
    private int image = -1;

    public static boolean hasImage(int index) {
        return index >= 0 && index <= IMAGES.length;
    }

    public static void show(Context context, int gameDisplayId, Display display, int index) {
        wantedImage = index;
        gameDisplay = gameDisplayId;
        ScreenArtActivity activity = shown.get();
        if (activity != null && !activity.isFinishing()
            && activity.getWindowManager().getDefaultDisplay().getDisplayId() == display.getDisplayId()) {
            activity.showImage(index);
            return;
        }
        if (activity == null && launchDisplay == display.getDisplayId()) {
            return;
        }
        hide();
        launchDisplay = display.getDisplayId();
        Log.i(TAG, "Showing image " + index + " on screen " + display.getDisplayId() + " " + display.getName());
        ActivityOptions options = ActivityOptions.makeBasic().setLaunchDisplayId(display.getDisplayId());
        Intent intent = new Intent(context, ScreenArtActivity.class)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK
                    | Intent.FLAG_ACTIVITY_NO_ANIMATION)
            .putExtra(EXTRA_LAUNCH, launch);
        context.startActivity(intent, options.toBundle());
    }

    public static void followGame(int gameDisplayId) {
        gameDisplay = gameDisplayId;
        if (shown.get() == null && launchDisplay == gameDisplayId) {
            hide();
        }
    }

    public static void hide() {
        ScreenArtActivity activity = shown.get();
        shown.clear();
        launch++;
        launchDisplay = Display.INVALID_DISPLAY;
        if (activity != null && !activity.isFinishing()) {
            activity.finish();
            activity.overridePendingTransition(0, 0);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (getIntent().getIntExtra(EXTRA_LAUNCH, -1) != launch) {
            finish();
            return;
        }
        launchDisplay = Display.INVALID_DISPLAY;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        view = new ImageView(this);
        view.setBackgroundColor(Color.BLACK);
        view.setScaleType(ImageView.ScaleType.CENTER_CROP);
        setContentView(view);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            view.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                       | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                       | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
        shown = new WeakReference<>(this);
        showImage(wantedImage);
    }

    @Override
    public void onTopResumedActivityChanged(boolean top) {
        super.onTopResumedActivityChanged(top);
        if (!top) {
            return;
        }
        Log.i(TAG, "Giving the focus back to the game on screen " + gameDisplay);
        startActivity(new Intent(this, LighthouseActivity.class)
                          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION),
                      ActivityOptions.makeBasic().setLaunchDisplayId(gameDisplay).toBundle());
    }

    @Override
    protected void onDestroy() {
        if (shown.get() == this) {
            shown.clear();
        }
        super.onDestroy();
    }

    private void showImage(int index) {
        if (!hasImage(index)) {
            finish();
            return;
        }
        if (index == image) {
            return;
        }
        image = index;
        if (index == IMAGES.length) {
            view.setImageDrawable(null);
            return;
        }
        Rect bounds;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            bounds = getWindowManager().getCurrentWindowMetrics().getBounds();
        } else {
            DisplayMetrics metrics = getResources().getDisplayMetrics();
            bounds = new Rect(0, 0, Math.max(metrics.widthPixels, 1), Math.max(metrics.heightPixels, 1));
        }
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inJustDecodeBounds = true;
        BitmapFactory.decodeResource(getResources(), IMAGES[index], options);
        int scale = 1;
        while (options.outWidth / (scale * 2) >= bounds.width() && options.outHeight / (scale * 2) >= bounds.height()) {
            scale *= 2;
        }
        options.inJustDecodeBounds = false;
        options.inSampleSize = scale;
        options.inScaled = false;
        Bitmap bitmap = BitmapFactory.decodeResource(getResources(), IMAGES[index], options);
        view.setImageBitmap(bitmap);
    }
}
