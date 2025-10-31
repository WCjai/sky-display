#include "display_primitives.h"

#include <algorithm>
#include <cmath>

namespace {

inline void DrawVLine(Paint& p, int x, int y1, int y2, int color) {
  if (y2 < y1) std::swap(y1, y2);
  for (int yy = y1; yy <= y2; ++yy) {
    p.DrawFilledRectangle(x, yy, x, yy, color);
  }
}

inline void DrawHLine(Paint& p, int x1, int x2, int y, int color) {
  if (x2 < x1) std::swap(x1, x2);
  for (int xx = x1; xx <= x2; ++xx) {
    p.DrawFilledRectangle(xx, y, xx, y, color);
  }
}

inline void plotTL(Paint& p, int cx, int cy, int x, int y, int color) {
  p.DrawAbsolutePixel(cx - x, cy - y, color);
  p.DrawAbsolutePixel(cx - y, cy - x, color);
}

inline void plotTR(Paint& p, int cx, int cy, int x, int y, int color) {
  p.DrawAbsolutePixel(cx + x, cy - y, color);
  p.DrawAbsolutePixel(cx + y, cy - x, color);
}

inline void plotBL(Paint& p, int cx, int cy, int x, int y, int color) {
  p.DrawAbsolutePixel(cx - x, cy + y, color);
  p.DrawAbsolutePixel(cx - y, cy + x, color);
}

inline void plotBR(Paint& p, int cx, int cy, int x, int y, int color) {
  p.DrawAbsolutePixel(cx + x, cy + y, color);
  p.DrawAbsolutePixel(cx + y, cy + x, color);
}

void drawQuarterCircleOutline(Paint& p, int cx, int cy, int r, int corner, int color) {
  if (r <= 0) return;
  int x = 0;
  int y = r;
  int d = 3 - 2 * r;
  while (x <= y) {
    switch (corner) {
      case 0: plotTL(p, cx, cy, x, y, color); break;
      case 1: plotTR(p, cx, cy, x, y, color); break;
      case 2: plotBL(p, cx, cy, x, y, color); break;
      case 3: plotBR(p, cx, cy, x, y, color); break;
    }
    if (d < 0) {
      d += 4 * x + 6;
    } else {
      d += 4 * (x - y) + 10;
      --y;
    }
    ++x;
  }
}

}  // namespace

void DrawRoundedRectFilled(Paint& p, int x, int y, int w, int h, int r, int color) {
  if (w <= 0 || h <= 0) return;
  if (r < 0) r = 0;
  int rmax = std::min(w, h) / 2;
  if (r > rmax) r = rmax;

  if (w - 2 * r > 0) {
    p.DrawFilledRectangle(x + r, y, x + w - r - 1, y + h - 1, color);
  }

  int cy = y + h / 2;
  for (int dy = -r; dy <= r; ++dy) {
    int dx = static_cast<int>(std::floor(std::sqrt(std::max(0.0f, static_cast<float>(r * r - dy * dy)))));
    int yy = cy + dy;
    p.DrawFilledRectangle(x + r - dx, yy, x + r - 1, yy, color);
    p.DrawFilledRectangle(x + w - r, yy, x + w - r + dx - 1, yy, color);
  }
}

void DrawRoundedRectOutline(Paint& p, int x, int y, int w, int h, int r, int thickness, int color) {
  if (w <= 0 || h <= 0 || thickness <= 0) return;
  if (r < 0) r = 0;
  int rmax = std::min(w, h) / 2;
  if (r > rmax) r = rmax;

  for (int t = 0; t < thickness; ++t) {
    int xi = x + t;
    int yi = y + t;
    int wi = w - 2 * t;
    int hi = h - 2 * t;
    if (wi <= 0 || hi <= 0) break;

    int rr = r - t;
    if (rr < 0) rr = 0;
    int left = xi;
    int right = xi + wi - 1;
    int top = yi;
    int bottom = yi + hi - 1;

    if (wi > 0) {
      DrawHLine(p, left + rr, right - rr, top, color);
      DrawHLine(p, left + rr, right - rr, bottom, color);
    }
    if (hi > 0) {
      DrawVLine(p, left, top + rr, bottom - rr, color);
      DrawVLine(p, right, top + rr, bottom - rr, color);
    }

    if (rr > 0) {
      int cxL = left + rr;
      int cxR = right - rr;
      int cyT = top + rr;
      int cyB = bottom - rr;
      drawQuarterCircleOutline(p, cxL, cyT, rr, 0, color);
      drawQuarterCircleOutline(p, cxR, cyT, rr, 1, color);
      drawQuarterCircleOutline(p, cxL, cyB, rr, 2, color);
      drawQuarterCircleOutline(p, cxR, cyB, rr, 3, color);
    }
  }
}

void DrawFilledPill(Paint& p, int x, int y, int w, int h, int color) {
  if (w <= 0 || h <= 1) return;
  int r = h / 2;
  if (2 * r > h) r = h / 2;
  if (2 * r > w) r = w / 2;

  int left = x;
  int right = x + w - 1;
  int top = y;
  int bottom = y + h - 1;

  if (right - left + 1 > 0 && r > 0) {
    p.DrawFilledRectangle(left + r, top, right - r, bottom, color);
  } else {
    p.DrawFilledRectangle(left, top, right, bottom, color);
    return;
  }

  for (int dy = -r; dy <= r; ++dy) {
    int yy = y + r + dy;
    int rr_minus_dy2 = r * r - dy * dy;
    int dx = rr_minus_dy2 > 0 ? static_cast<int>(std::floor(std::sqrt(static_cast<float>(rr_minus_dy2)))) : 0;
    int xl = left + r - dx;
    int xr = right - r + dx;
    if (xl > xr) std::swap(xl, xr);
    p.DrawFilledRectangle(xl, yy, xr, yy, color);
  }
}

void DrawSolidHLine(Paint& p, int y, int width, int thickness, int color) {
  if (thickness <= 0 || width <= 0) return;
  for (int t = 0; t < thickness; ++t) {
    p.DrawFilledRectangle(0, y + t, width - 1, y + t, color);
  }
}

void DrawDottedVLine(Paint& p, int x, int height, int thickness, int onLen, int offLen, int color) {
  if (thickness <= 0 || height <= 0 || onLen <= 0) return;
  if (offLen < 0) offLen = 0;
  for (int t = 0; t < thickness; ++t) {
    for (int y0 = 0; y0 < height; y0 += (onLen + offLen)) {
      int y1 = y0;
      int y2 = std::min(y0 + onLen - 1, height - 1);
      p.DrawFilledRectangle(x + t, y1, x + t, y2, color);
    }
  }
}

void DrawDottedHLine(Paint& p, int y, int width, int thickness, int onLen, int offLen, int color) {
  if (thickness <= 0 || width <= 0 || onLen <= 0) return;
  if (offLen < 0) offLen = 0;
  for (int t = 0; t < thickness; ++t) {
    for (int x0 = 0; x0 < width; x0 += (onLen + offLen)) {
      int x1 = x0;
      int x2 = std::min(x0 + onLen - 1, width - 1);
      p.DrawFilledRectangle(x1, y + t, x2, y + t, color);
    }
  }
}
