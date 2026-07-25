/* SDL2 keyboard-layout probe.
 * Static part: dump what SDL2 maps physical keys to under the CURRENT layout.
 *   On a pt (Portuguese) layout, physical US-';' (SCANCODE_SEMICOLON) is 'ç',
 *   and several keys carry accents. On US it stays ';'.
 * Live part: 15s event loop echoing TEXTINPUT text + KEYDOWN keysym so we can
 *   see exactly what SDL2 hands the app when real keys are pressed.
 * Build: cc sdl2_kbprobe.c $(pkg-config --cflags --libs sdl2) -o sdl2_kbprobe
 * Run:   DISPLAY=:0 ./sdl2_kbprobe
 */
#include <SDL.h>
#include <stdio.h>
#include <time.h>

static void dump_key(SDL_Scancode sc, const char *label)
{
    SDL_Keycode kc = SDL_GetKeyFromScancode(sc);
    printf("  %-14s scancode=%-3d -> keycode=0x%08x  name=\"%s\"\n",
           label, sc, kc, SDL_GetKeyName(kc));
}

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    printf("video driver: %s\n", SDL_GetCurrentVideoDriver());

    SDL_Window *w = SDL_CreateWindow("kbprobe", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 320, 120, SDL_WINDOW_SHOWN);
    if (!w) { printf("CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_StartTextInput();

    printf("\n=== SDL2's view of the current layout (physical key -> what SDL2 thinks it is) ===\n");
    dump_key(SDL_SCANCODE_SEMICOLON,  "US ';' key");   /* pt: 'ç'        */
    dump_key(SDL_SCANCODE_APOSTROPHE, "US ''' key");   /* pt: dead 'º/ª' */
    dump_key(SDL_SCANCODE_LEFTBRACKET,"US '[' key");   /* pt: '+/*'      */
    dump_key(SDL_SCANCODE_MINUS,      "US '-' key");   /* pt: ''/?'      */
    dump_key(SDL_SCANCODE_0,          "digit 0");
    dump_key(SDL_SCANCODE_Q,          "Q");
    printf("If the ';' key shows keycode 0xe7 / name \"\\u00e7\" (or similar accented\n"
           "names appear), SDL2 sees pt. If they all read as plain US symbols,\n"
           "SDL2 is NOT honoring the X11 pt layout.\n");

    printf("\n=== live: press keys for 15s (try the c-cedilla key, AltGr+combos) ===\n");
    Uint32 start = SDL_GetTicks();
    SDL_Event e;
    while (SDL_GetTicks() - start < 15000) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto done;
            if (e.type == SDL_TEXTINPUT)
                printf("  TEXTINPUT  text=\"%s\"\n", e.text.text);
            else if (e.type == SDL_KEYDOWN)
                printf("  KEYDOWN    sym=0x%x name=\"%s\" scancode=%d mod=0x%x\n",
                       e.key.keysym.sym, SDL_GetKeyName(e.key.keysym.sym),
                       e.key.keysym.scancode, e.key.keysym.mod);
        }
        SDL_Delay(10);
    }
done:
    SDL_StopTextInput();
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
