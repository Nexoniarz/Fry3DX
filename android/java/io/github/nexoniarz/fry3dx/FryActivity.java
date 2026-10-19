package io.github.nexoniarz.fry3dx;

import org.libsdl.app.SDLActivity;

// Fry3DX's activity: SDL's, loading SDL and the app (libmain.so, whose SDL_main it runs).
public class FryActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
