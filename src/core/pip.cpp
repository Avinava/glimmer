#include "pip.h"
#include "display.h"
#include "theme.h"

// Body template: 20×20 chars.
//   . = transparent (don't draw)
//   k = outline (near-black)
//   B = body light
//   b = body shade
//   w = cream face area (default fill; mood face overrides per pixel)
//   _ = body dark shade (line color)
static const char* const kBody[20] = {
    "....................",   // 0
    "........kk..........",   // 1
    ".......k..k.........",   // 2
    ".....kk....kk.......",   // 3
    "....k........k......",   // 4
    "...kBBBBBBBBBBk.....",   // 5
    "..kBwwwwwwwwwwBk....",   // 6
    "..kBwwwwwwwwwwBk....",   // 7   face row 0
    "..kBwwwwwwwwwwBk....",   // 8   face row 1
    "..kBwwwwwwwwwwBk....",   // 9   face row 2
    "..kBwwwwwwwwwwBk....",   // 10  face row 3
    "..kBwwwwwwwwwwBk....",   // 11  face row 4
    "..kBwwwwwwwwwwBk....",   // 12  face row 5
    "..kBwwwwwwwwwwBk....",   // 13
    "..kBBBBBBBBBBBBk....",   // 14
    "...kk........kk.....",   // 15
    "....kk......kk......",   // 16
    "...kBBk....kBBk.....",   // 17
    "...k..k....k..k.....",   // 18
    "....................",   // 19
};

// Mood faces — 6 rows × 8 cols, overlaid at body[7..12][4..11].
// Same char palette as the body. Space or '.' = "don't override".
static const char* const kFaceThinking[6] = {
    "        ",
    " kk  kk ",
    "        ",
    "        ",
    "  kkk   ",
    "        ",
};

static const char* const kFaceSignal[6] = {
    "        ",
    " kk   k ",
    " kk  kk ",
    "    kkk ",
    "   kkkk ",
    "        ",
};

static const char* const kFaceHappy[6] = {
    "        ",
    " kk  kk ",
    " kk  kk ",
    "        ",
    " kkkkkk ",
    "        ",
};

static const char* const kFaceHi[6] = {
    "        ",
    "  kk kk ",
    "  kk kk ",
    "        ",
    " kkkkkk ",
    " kk  kk ",
};

static const char* const kFaceLoading[6] = {
    "        ",
    " kk kk  ",
    "        ",
    "  kkkk  ",
    "        ",
    "        ",
};

// Sleep — eyes drawn as horizontal dashes ("zzz")
static const char* const kFaceSleep[6] = {
    "        ",
    " kkkkkk ",
    "        ",
    "        ",
    "        ",
    "        ",
};

// Excited — wide eyes + open mouth + pink cheeks
static const char* const kFaceExcited[6] = {
    "        ",
    " kk  kk ",
    " pp  pp ",
    "        ",
    " kkkkkk ",
    " kk  kk ",
};

// Focus — narrowed eyes (single coral line per eye)
static const char* const kFaceFocus[6] = {
    " cccccc ",
    " c    c ",
    " c kk c ",
    " c    c ",
    " cccccc ",
    "        ",
};

// Weather — soft happy + amber accent (sun-like)
static const char* const kFaceWeather[6] = {
    "        ",
    " yy  yy ",
    "        ",
    "        ",
    " kkkkkk ",
    "        ",
};

// Offline — X eyes (sad)
static const char* const kFaceOffline[6] = {
    "        ",
    " k k k k",
    "  k k k ",
    " k k k k",
    "        ",
    "  k  k  ",
};

static const char* const* faceFor(MoodId m) {
    switch (m) {
        case MoodId::THINKING: return kFaceThinking;
        case MoodId::SIGNAL:   return kFaceSignal;
        case MoodId::HAPPY:    return kFaceHappy;
        case MoodId::HI:       return kFaceHi;
        case MoodId::LOADING:  return kFaceLoading;
        case MoodId::SLEEP:    return kFaceSleep;
        case MoodId::EXCITED:  return kFaceExcited;
        case MoodId::FOCUS:    return kFaceFocus;
        case MoodId::WEATHER:  return kFaceWeather;
        case MoodId::OFFLINE:  return kFaceOffline;
        default:               return nullptr;
    }
}

static uint16_t paletteFor(char ch) {
    switch (ch) {
        case 'k': return 0x0841;          // outline near-black, slightly above BG
        case 'B': return 0x39C7;          // body light  #3A3346
        case 'b': return 0x2104;          // body shade  #23202C
        case 'w': return Theme::INK;      // cream face
        case '_': return Theme::LINE;
        case 'c': return Theme::CORAL;
        case 'y': return Theme::AMBER;
        case 'm': return Theme::MINT;
        case 's': return Theme::SKY;
        case 'L': return Theme::LILAC;
        case 'p': return Theme::PINK;
        default:  return 0;               // transparent / skip
    }
}

void Pip::draw(int x, int y, uint8_t scale, MoodId mood) {
    if (mood == MoodId::NONE) return;
    const char* const* face = faceFor(mood);

    for (int cy = 0; cy < 20; cy++) {
        for (int cx = 0; cx < 20; cx++) {
            char ch = kBody[cy][cx];

            // Face overlay slot? rows 7..12 cols 4..11
            if (face && cy >= 7 && cy <= 12 && cx >= 4 && cx <= 11) {
                char fch = face[cy - 7][cx - 4];
                if (fch != ' ' && fch != '.') ch = fch;
            }
            if (ch == '.' || ch == ' ') continue;   // transparent
            uint16_t col = paletteFor(ch);
            if (scale == 1) tft.drawPixel(x + cx, y + cy, col);
            else            tft.fillRect(x + cx * scale, y + cy * scale,
                                         scale, scale, col);
        }
    }
}
