#include "capture_region.h"
#include "screen.h"

#include <algorithm>

RECT CaptureRegion(POINT anchor, POINT cursor, int width, int height) {
    if (width <= 0 || height <= 0) return {};
    return {std::clamp(std::min(anchor.x, cursor.x), 0L, static_cast<LONG>(width)),
        std::clamp(std::min(anchor.y, cursor.y), 0L, static_cast<LONG>(height)),
        std::clamp(std::max(anchor.x, cursor.x), 0L, static_cast<LONG>(width)),
        std::clamp(std::max(anchor.y, cursor.y), 0L, static_cast<LONG>(height))};
}

Image CropForOcr(const Image& image, RECT region) {
    const RECT bounds{0, 0, image.width, image.height};
    RECT crop{};
    if (image.width <= 0 || image.height <= 0 ||
        image.pixels.size() != static_cast<size_t>(image.width) * image.height ||
        !IntersectRect(&crop, &bounds, &region)) return {};
    Image result{crop.right - crop.left, crop.bottom - crop.top};
    result.origin = {image.origin.x + crop.left, image.origin.y + crop.top};
    result.pixels.resize(static_cast<size_t>(result.width) * result.height);
    for (int row = 0; row < result.height; ++row) {
        std::copy_n(image.pixels.data() + static_cast<size_t>(crop.top + row) * image.width + crop.left,
            result.width, result.pixels.data() + static_cast<size_t>(row) * result.width);
    }
    return result;
}
