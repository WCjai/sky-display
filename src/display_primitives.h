#pragma once

#include "epdpaint.h"

void DrawRoundedRectFilled(Paint& p, int x, int y, int w, int h, int r, int color);
void DrawRoundedRectOutline(Paint& p, int x, int y, int w, int h, int r, int thickness, int color);
void DrawFilledPill(Paint& p, int x, int y, int w, int h, int color);
void DrawSolidHLine(Paint& p, int y, int width, int thickness, int color);
void DrawDottedVLine(Paint& p, int x, int height, int thickness, int onLen, int offLen, int color);
void DrawDottedHLine(Paint& p, int y, int width, int thickness, int onLen, int offLen, int color);
