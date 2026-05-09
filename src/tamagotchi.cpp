// tamagotchi.cpp – Tamagotchi mini-game
//
// Controls (same single-button scheme as the main menu):
//   Short press  →  move cursor to next menu item
//   Long  press  →  activate selected item
//
// Screen layout (80 × 160, portrait):
//   y=  0.. 13   title bar  ("TAMAGOTCHI" or "TAMAGOTCHI RIP" when dead)
//   y= 16.. 47   pet face  (two rows of textSize=2 ASCII art)
//   y= 49.. 56   age line  / action feedback flash
//   y= 59.. 91   stat bars: Hunger, Joy, Energy, HP
//   y= 96..159   menu items  (up to 4 rows, ITEM_H=13, GAP=4)
//
// NVS namespace "tamagotchi" – keys: hunger, happiness, energy, health,
//   age_mins, alive.
//
// ─────────────────────────────────────────────────────────────────────────────
#include "tamagotchi.h"
#include "display.h"
#include "button.h"
#include <Preferences.h>

// ── Colors ───────────────────────────────────────────────────────────────────
#define T_BG               RGB565(  0,   0,   0)
#define T_TITLE_BG         RGB565(  0,  70,   0)
#define T_TITLE_TEXT       RGB565(  0, 255,   0)
#define T_DEAD_TITLE_BG    RGB565( 80,   0,   0)
#define T_DEAD_TITLE_TEXT  RGB565(255,   0,   0)
#define T_TEXT             RGB565(180, 180, 180)
#define T_FACE_HAPPY       RGB565(  0, 220,   0)
#define T_FACE_NORMAL      RGB565(180, 180, 180)
#define T_FACE_SAD         RGB565(255, 180,   0)
#define T_FACE_BAD         RGB565(255,  80,   0)
#define T_FACE_DEAD        RGB565(140,   0,   0)
#define T_CURSOR_BG        RGB565(  0,   0, 200)
#define T_CURSOR_TEXT      RGB565(255, 255, 255)
#define T_ITEM_BG          RGB565(  0,   0,   0)
#define T_ITEM_TEXT        RGB565( 64,  64,  64)
#define T_CONFIRM_BG       RGB565( 80,  60, 250)
#define T_CONFIRM_TEXT     RGB565(255, 255, 255)
#define T_BAR_BG           RGB565( 30,  30,  30)
#define T_BAR_GOOD         RGB565(  0, 200,   0)
#define T_BAR_MID          RGB565(200, 180,   0)
#define T_BAR_BAD          RGB565(220,   0,   0)

// ── Timing ───────────────────────────────────────────────────────────────────
// Stats decay (real milliseconds per 1-point change)
static const uint32_t HUNGER_RATE_MS    = 20000UL; // hunger   +1 per 20 s
static const uint32_t HAPPINESS_RATE_MS = 30000UL; // happiness-1 per 30 s
static const uint32_t ENERGY_RATE_MS    = 25000UL; // energy   -1 per 25 s
static const uint32_t HEALTH_RATE_MS    = 10000UL; // health   -1 per 10 s (when in danger)
static const uint32_t AGE_RATE_MS       = 60000UL; // age+1 per real minute

// ── Layout ───────────────────────────────────────────────────────────────────
static const int16_t TITLE_H      = 14;
static const int16_t FACE1_Y      = 16;   // eyes row (textSize=2, 16 px tall)
static const int16_t FACE2_Y      = 32;   // mouth row
static const int16_t AGE_Y        = 49;   // age / feedback text
static const int16_t BAR_Y[4]     = { 59, 68, 77, 86 }; // H, J, E, HP
static const int16_t ITEM_H       = 13;
static const int16_t ITEM_GAP     = 4;
static const int16_t FIRST_ITEM_Y = 96;
static const int16_t BAR_LBL_W    = 14;   // label column width (px)
static const int16_t BAR_X        = BAR_LBL_W;
static const int16_t BAR_W        = 44;
static const int16_t BAR_H        = 6;

// ── Pet data ─────────────────────────────────────────────────────────────────
struct Pet {
    uint8_t  hunger;     // 0 = full (good), 100 = starving (bad)
    uint8_t  happiness;  // 0 = miserable (bad), 100 = joyful (good)
    uint8_t  energy;     // 0 = exhausted (bad), 100 = rested (good)
    uint8_t  health;     // 0 = dead (bad), 100 = perfect (good)
    uint32_t age_mins;   // total game-minutes the pet has been alive
    bool     alive;
};

static Pet         s_pet;
static Preferences s_prefs;
static int         s_sel        = 0;

// Per-stat millisecond accumulators
static uint32_t s_hunger_acc  = 0;
static uint32_t s_happy_acc   = 0;
static uint32_t s_energy_acc  = 0;
static uint32_t s_health_acc  = 0;
static uint32_t s_age_acc     = 0;

// ── Item tables ───────────────────────────────────────────────────────────────
static const char *ALIVE_ITEMS[] = { "Feed", "Play", "Sleep", "Back to Menu" };
static const char *DEAD_ITEMS[]  = { "New Game",    "Back to Menu" };
static const int   NUM_ALIVE     = 4;
static const int   NUM_DEAD      = 2;

static int         num_items()     { return s_pet.alive ? NUM_ALIVE : NUM_DEAD; }
static const char *item_label(int i) {
    if (s_pet.alive) return (i < NUM_ALIVE) ? ALIVE_ITEMS[i] : "";
    return (i < NUM_DEAD) ? DEAD_ITEMS[i] : "";
}

// ── Persistence ───────────────────────────────────────────────────────────────
static void pet_load() {
    s_prefs.begin("tamagotchi", true);
    s_pet.hunger    = s_prefs.getUChar("hunger",     20);
    s_pet.happiness = s_prefs.getUChar("happiness",  80);
    s_pet.energy    = s_prefs.getUChar("energy",     80);
    s_pet.health    = s_prefs.getUChar("health",    100);
    s_pet.age_mins  = s_prefs.getUInt ("age_mins",    0);
    s_pet.alive     = s_prefs.getBool ("alive",    true);
    s_prefs.end();
}

static void pet_save() {
    s_prefs.begin("tamagotchi", false);
    s_prefs.putUChar("hunger",    s_pet.hunger);
    s_prefs.putUChar("happiness", s_pet.happiness);
    s_prefs.putUChar("energy",    s_pet.energy);
    s_prefs.putUChar("health",    s_pet.health);
    s_prefs.putUInt ("age_mins",  s_pet.age_mins);
    s_prefs.putBool ("alive",     s_pet.alive);
    s_prefs.end();
}

static void pet_reset() {
    s_pet         = { 20, 80, 80, 100, 0, true };
    s_hunger_acc  = 0;
    s_happy_acc   = 0;
    s_energy_acc  = 0;
    s_health_acc  = 0;
    s_age_acc     = 0;
}

// ── Stat update ───────────────────────────────────────────────────────────────
// Returns true when at least one stat changed (used to trigger a redraw).
// Also sets s_pet.alive = false and resets s_sel when health reaches 0.
static bool tick_stats(uint32_t delta_ms) {
    if (!s_pet.alive || delta_ms == 0) return false;

    bool changed = false;

    s_hunger_acc += delta_ms;
    while (s_hunger_acc >= HUNGER_RATE_MS) {
        s_hunger_acc -= HUNGER_RATE_MS;
        if (s_pet.hunger < 100) { s_pet.hunger++; changed = true; }
    }

    s_happy_acc += delta_ms;
    while (s_happy_acc >= HAPPINESS_RATE_MS) {
        s_happy_acc -= HAPPINESS_RATE_MS;
        if (s_pet.happiness > 0) { s_pet.happiness--; changed = true; }
    }

    s_energy_acc += delta_ms;
    while (s_energy_acc >= ENERGY_RATE_MS) {
        s_energy_acc -= ENERGY_RATE_MS;
        if (s_pet.energy > 0) { s_pet.energy--; changed = true; }
    }

    // Health degrades only when the pet is critically hungry or sad
    bool in_danger = (s_pet.hunger >= 90) || (s_pet.happiness <= 10);
    if (in_danger) {
        s_health_acc += delta_ms;
        while (s_health_acc >= HEALTH_RATE_MS) {
            s_health_acc -= HEALTH_RATE_MS;
            if (s_pet.health > 0) { s_pet.health--; changed = true; }
        }
    } else {
        s_health_acc = 0;
    }

    s_age_acc += delta_ms;
    while (s_age_acc >= AGE_RATE_MS) {
        s_age_acc -= AGE_RATE_MS;
        s_pet.age_mins++;
        changed = true;
    }

    if (s_pet.health == 0 && s_pet.alive) {
        s_pet.alive = false;
        s_sel       = 0;
        changed     = true;
    }

    return changed;
}

// ── Drawing helpers ───────────────────────────────────────────────────────────
static uint16_t bar_color(uint8_t pct) {
    if (pct > 60) return T_BAR_GOOD;
    if (pct > 30) return T_BAR_MID;
    return T_BAR_BAD;
}

// Draw one labelled progress bar.
// good_is_high=true  → full bar means 100 (health, joy, energy)
// good_is_high=false → full bar means 0 (hunger: lower is better)
static void draw_bar(int16_t y, const char *label, uint8_t value, bool good_is_high) {
    uint8_t pct = good_is_high ? value : (100 - value);

    gfx->fillRect(0, y, 80, 8, T_BG);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    // Label
    gfx->setTextColor(T_TEXT);
    gfx->setCursor(1, y);
    gfx->print(label);

    // Bar background + fill
    gfx->fillRect(BAR_X, y, BAR_W, BAR_H, T_BAR_BG);
    int16_t filled = (int16_t)(BAR_W * pct / 100);
    if (filled > 0)
        gfx->fillRect(BAR_X, y, filled, BAR_H, bar_color(pct));

    // Numeric value
    char buf[4];
    snprintf(buf, sizeof(buf), "%3u", (unsigned)value);
    gfx->setTextColor(T_TEXT);
    gfx->setCursor(BAR_X + BAR_W + 2, y);
    gfx->print(buf);
}

static void get_face(const char *&eyes, const char *&mouth, uint16_t &color) {
    if (!s_pet.alive)         { eyes = "(X.X)"; mouth = "(RIP)"; color = T_FACE_DEAD;   return; }
    if (s_pet.health   <  25) { eyes = "(x.x)"; mouth = "(+_+)"; color = T_FACE_BAD;    return; }
    if (s_pet.energy   <  20) { eyes = "(-.-)" ; mouth = "(z_z)"; color = T_FACE_SAD;    return; }
    if (s_pet.hunger   >  75) { eyes = "(O.O)"; mouth = "(>_<)"; color = T_FACE_SAD;    return; }
    if (s_pet.happiness >= 70 && s_pet.hunger < 40 && s_pet.health > 70)
                               { eyes = "(^.^)"; mouth = "(-w-)"; color = T_FACE_HAPPY;  return; }
    eyes  = "(o.o)"; mouth = "(-_-)"; color = T_FACE_NORMAL;
}

static void draw_face() {
    const char *eyes, *mouth;
    uint16_t    color;
    get_face(eyes, mouth, color);

    // Each 5-char string at textSize=2 → 5×12 = 60 px wide; centre on 80 px → x=10
    gfx->setTextSize(2);
    gfx->setTextWrap(false);

    gfx->fillRect(0, FACE1_Y, 80, 16, T_BG);
    gfx->setTextColor(color);
    gfx->setCursor(10, FACE1_Y);
    gfx->print(eyes);

    gfx->fillRect(0, FACE2_Y, 80, 16, T_BG);
    gfx->setCursor(10, FACE2_Y);
    gfx->print(mouth);

    gfx->setTextSize(1);
}

static void draw_age() {
    gfx->fillRect(0, AGE_Y, 80, 8, T_BG);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);
    gfx->setTextColor(T_TEXT);
    gfx->setCursor(1, AGE_Y);
    char buf[22];
    uint32_t days  = s_pet.age_mins / (60u * 24u);
    uint32_t hours = (s_pet.age_mins / 60u) % 24u;
    snprintf(buf, sizeof(buf), "Age: %ud %uh", (unsigned)days, (unsigned)hours);
    gfx->print(buf);
}

static void draw_stats() {
    draw_age();
    draw_bar(BAR_Y[0], "H",  s_pet.hunger,    false); // low hunger = good
    draw_bar(BAR_Y[1], "J",  s_pet.happiness, true);
    draw_bar(BAR_Y[2], "E",  s_pet.energy,    true);
    draw_bar(BAR_Y[3], "HP", s_pet.health,    true);
}

static void draw_item_row(int i, bool confirmed) {
    int16_t fy = FIRST_ITEM_Y + (int16_t)i * (ITEM_H + ITEM_GAP);
    int16_t ty = fy + 3;
    bool    cursor = (i == s_sel);

    uint16_t bg = (confirmed && cursor) ? T_CONFIRM_BG
                : cursor                ? T_CURSOR_BG
                :                        T_ITEM_BG;

    uint16_t tc = (confirmed && cursor) ? T_CONFIRM_TEXT
                : cursor                ? T_CURSOR_TEXT
                :                        T_ITEM_TEXT;

    gfx->fillRect(0, fy, 80, ITEM_H, bg);
    gfx->setTextColor(tc);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);
    gfx->setCursor(2, ty);
    gfx->print(confirmed ? "v" : (cursor ? ">" : " "));
    gfx->setCursor(12, ty);
    gfx->print(item_label(i));
}

static void draw_items() {
    for (int i = 0; i < num_items(); i++)
        draw_item_row(i, false);
}

static void draw_screen() {
    gfx->fillScreen(T_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);

    // Title bar
    uint16_t tbg = s_pet.alive ? T_TITLE_BG    : T_DEAD_TITLE_BG;
    uint16_t ttx = s_pet.alive ? T_TITLE_TEXT   : T_DEAD_TITLE_TEXT;
    gfx->fillRect(0, 0, 80, TITLE_H, tbg);
    gfx->setTextColor(ttx);
    gfx->setCursor(2, 3);
    gfx->print(s_pet.alive ? "TAMAGOTCHI" : "TAMAGOTCHI RIP");

    draw_face();
    draw_stats();
    draw_items();
}

// ── Action feedback ───────────────────────────────────────────────────────────
// Briefly shows a message in the age line, then leaves it for draw_age()
// to overwrite on the next stat redraw.
static void feedback(const char *msg, uint16_t color) {
    gfx->fillRect(0, AGE_Y, 80, 8, T_BG);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);
    gfx->setTextColor(color);
    gfx->setCursor(1, AGE_Y);
    gfx->print(msg);
    delay(700);
}

// ── Actions ───────────────────────────────────────────────────────────────────
static void do_feed() {
    if (s_pet.hunger < 10) { feedback("Not hungry!", T_BAR_MID); return; }
    s_pet.hunger  = (s_pet.hunger  > 30) ? s_pet.hunger  - 30 : 0;
    s_pet.health  = (s_pet.health  < 95) ? s_pet.health  +  5 : 100;
    s_hunger_acc  = 0;
    feedback("Yum! (+HP)", T_BAR_GOOD);
}

static void do_play() {
    if (s_pet.energy < 15) { feedback("Too tired!", T_BAR_MID); return; }
    s_pet.happiness = (s_pet.happiness < 75) ? s_pet.happiness + 25 : 100;
    s_pet.energy    = (s_pet.energy    > 15) ? s_pet.energy    - 15 : 0;
    s_happy_acc     = 0;
    feedback("Wheee! (+J)", T_BAR_GOOD);
}

static void do_sleep() {
    s_pet.energy = (s_pet.energy < 60) ? s_pet.energy + 40 : 100;
    s_pet.hunger = (s_pet.hunger < 90) ? s_pet.hunger + 10 : 100;
    s_energy_acc = 0;
    feedback("Zzz... (+E)", T_FACE_SAD);
}

// ── Public entry point ────────────────────────────────────────────────────────
void tamagotchi_run() {
    pet_load();
    s_hunger_acc = 0;
    s_happy_acc  = 0;
    s_energy_acc = 0;
    s_health_acc = 0;
    s_age_acc    = 0;
    s_sel        = 0;
    draw_screen();

    for (;;) {
        uint32_t t0 = millis();

        ButtonEvent ev = button_read([]() {
            // Long-press threshold crossed – highlight selected item
            draw_item_row(s_sel, true);
        });

        // Cap delta so a held button doesn't cause a large time jump
        uint32_t delta = millis() - t0;
        if (delta > 5000) delta = 500;

        bool was_alive   = s_pet.alive;
        bool stats_dirty = tick_stats(delta);
        bool just_died   = was_alive && !s_pet.alive;

        if (ev == BTN_NONE) {
            if (stats_dirty) {
                if (just_died) {
                    draw_screen(); // full redraw: title, face, items all change
                } else {
                    draw_face();
                    draw_stats();
                }
            }
            delay(20); // ~50 Hz polling – avoids burning 100 % CPU
            continue;
        }

        // ── Button event ─────────────────────────────────────────────────────
        // Apply any pending stat changes to the display before handling input
        if (stats_dirty) {
            if (just_died) {
                draw_screen();
                continue; // skip input handling this tick – screen was just rebuilt
            }
            draw_face();
            draw_stats();
        }

        if (ev == BTN_SHORT) {
            s_sel = (s_sel + 1) % num_items();
            draw_items();

        } else { // BTN_LONG
            if (s_pet.alive) {
                switch (s_sel) {
                    case 0:
                        do_feed();
                        draw_face();
                        draw_stats();
                        draw_items();
                        break;
                    case 1:
                        do_play();
                        draw_face();
                        draw_stats();
                        draw_items();
                        break;
                    case 2:
                        do_sleep();
                        draw_face();
                        draw_stats();
                        draw_items();
                        break;
                    case 3:        // Back to Menu
                        pet_save();
                        return;
                }
            } else {
                switch (s_sel) {
                    case 0:        // New Game
                        pet_reset();
                        pet_save();
                        s_sel = 0;
                        draw_screen();
                        break;
                    case 1:        // Back to Menu
                        pet_save();
                        return;
                }
            }
        }
    }
}
