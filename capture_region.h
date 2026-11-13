#pragma once

#include <windows.h>

struct Image;

RECT CaptureRegion(POINT anchor, POINT cursor, int width, int height);
Image CropForOcr(const Image& image, RECT region);
