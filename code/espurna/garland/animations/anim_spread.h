#if GARLAND_SUPPORT

#include "../anim.h"
#include "../color.h"
#include "../palette.h"

//------------------------------------------------------------------------------
class AnimSpread : public Anim {
    const uint8_t minWidth = 10;
    const uint8_t maxWidth = 15;
    const uint8_t numTries = 5;
    uint8_t fade_ind = 10;

    int GeneratePos() {
        int pos = -1;
        for (auto i = 0; i < numTries; ++i) {
            pos = garlandRandom(0, numLeds);
            for (int j = pos - maxWidth; j < pos + maxWidth; ++j) {
                if (j >= 0 && j < numLeds && seq[j] > 0) {
                    pos = -1;
                    break;
                }
            }
            if (pos >= 0) break;
        }
        return pos;
    }

   public:
    AnimSpread() : Anim("Spread") {
        cycleFactor = 2;
    }

    void SetupImpl() override {
        fade_ind = garlandRandom(1, 20);
        inc = garlandRandom(2, 4);
        // DEBUG_MSG_P(PSTR("[GARLAND] AnimSpread inc = %d, fade_inc = %d\n"), inc, fade_ind);
        for (auto i = 0; i < numLeds; ++i)
            seq[i] = 0;
    }

    void Run() override {
        for (auto i = 0; i < numLeds; ++i)
            leds[i] = 0;

        for (auto i = 0; i < numLeds; ++i) {
            if (!ledstmp[i].is_empty()) {
                uint8_t width = maxWidth - seq[i];
                for (int j = i - width; j <= (i + width); j++) {
                    Color c = ledstmp[i];
                    if (j >= 0 && j < numLeds) {
                        if (leds[j].is_empty()) {
                            leds[j] = c;
                        } else {
                            leds[j] = leds[j].interpolate(c, 0.5);
                        }
                    }
                }
                ledstmp[i].fade(fade_ind);
                seq[i]--;
            } else {
                seq[i] = 0;
            }
        }

        if (garlandRandom(inc) == 0) {
            int pos = GeneratePos();
            if (pos == -1)
                return;

            ledstmp[pos] = palette->getRndInterpColor();
            seq[pos] = garlandRandom(minWidth, maxWidth);
        }
    }
};

#endif  // GARLAND_SUPPORT
