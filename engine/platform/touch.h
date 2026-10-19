#pragma once
// Touch controls (phones, tablets, touch screens): while you play, a stick under your left thumb
// moves, dragging on the right looks, and buttons on the right press the same "+jump",
// "+attack"... a key would. In menus, touches are taps (SDL turns them into mouse clicks).
// They show on Android, and anywhere once a finger touches the screen (touch_controls 0: never).
union SDL_Event;

namespace keel::touch {
bool active();
// inGame: playing (the mouse is captured), when touches are controls, not taps.
void handleEvent(const SDL_Event& ev, bool inGame);
// The stick: forward and sideways, -1..1.
void move(float& forward, float& side);
// Looking since the last call, in mouse pixels (so mouse sensitivity applies).
void takeLook(float& dx, float& dy);
// The pause button was pressed (once).
bool takeEscape();
// Draws the controls over the game (playing only).
void draw(bool inGame);
// Lets go of everything held (a menu opened).
void releaseAll();
} // namespace keel::touch
