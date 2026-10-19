package io.github.nexoniarz.fry3dx;

import android.os.Build;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import org.libsdl.app.SDLActivity;

// Fry3DX's activity: SDL's, loading SDL and the app (libmain.so, whose SDL_main it runs).
public class FryActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    // The whole screen: the status and navigation bars hidden (a swipe from the edge shows them
    // for a moment). SDL's own way (the old system-UI flags) doesn't do it on new Androids.
    @Override
    protected void onResume() {
        super.onResume();
        hideBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideBars();
    }

    // SDL sets the bars again (its old way) whenever the window changes: hidden again after it.
    @Override
    protected boolean sendCommand(int command, Object data) {
        final boolean sent = super.sendCommand(command, data);
        if (command == COMMAND_CHANGE_WINDOW_STYLE) getWindow().getDecorView().post(new Runnable() { // (no lambdas: built against android.jar alone)
            @Override
            public void run() {
                hideBars();
            }
        });
        return sent;
    }

    private void hideBars() {
        if (Build.VERSION.SDK_INT < 30) return;
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }
}
