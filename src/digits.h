// digits.h
#pragma once
#include <stdint.h>

struct Glyph1bpp {
  const uint8_t* data;     // MSB-first, 1 = black
  uint16_t width_bits;     // visible width (70)
  uint16_t height;         // e.g., 120
  uint16_t stride_bits;    // padded row width in bits (72 -> 9 bytes/row)
};

// Use the *visible* width/height for layout
#define DIG_W  70
#define DIG_H  120

// Optional colon bitmap sizes if you have them:
#define COLON_W_BMP 24
#define COLON_H_BMP 120

extern const uint8_t gDigit_0[];
extern const uint8_t gDigit_1[];
extern const uint8_t gDigit_2[];
extern const uint8_t gDigit_3[];
extern const uint8_t gDigit_4[];
extern const uint8_t gDigit_5[];
extern const uint8_t gDigit_6[];
extern const uint8_t gDigit_7[];
extern const uint8_t gDigit_8[];
extern const uint8_t gDigit_9[];

// One entry per digit (fill this in your digits.cpp)
extern const Glyph1bpp kDigits[10];

// Convenience
static constexpr uint16_t kDigitVisibleWidthBits = 70;
static constexpr uint16_t kDigitStrideBits       = 72;   // 9 bytes
static constexpr uint16_t kDigitStrideBytes      = kDigitStrideBits / 8;
