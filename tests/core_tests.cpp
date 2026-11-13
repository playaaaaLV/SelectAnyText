#include "settings.h"
#include "text_layout.h"
#include "capture_region.h"
#include "screen.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

TextFragment Word(std::wstring text, LONG x, LONG y) {
    TextWord word;
    word.bounds = {x, y, x + static_cast<LONG>(text.size()) * 10, y + 20};
    for (size_t i = 0; i <= text.size(); ++i) {
        word.stops.push_back({static_cast<float>(x + i * 10), i});
    }
    return {{word.bounds, {word}}, std::move(text)};
}
}

int main() {
    try {
        const Image pixels{4, 3, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, {-200, 40}};
        const auto frame = CaptureRegion({3, 3}, {1, 1}, 4, 3);
        const auto crop = CropForOcr(pixels, frame);
        Check(crop.width == 2 && crop.height == 2 && crop.pixels == std::vector<uint32_t>{5,6,9,10},
            "Region OCR must receive only pixels inside the rectangle");
        Check(crop.origin.x == -199 && crop.origin.y == 41, "Crop origin must preserve screen coordinates");
        const auto edge = CropForOcr(pixels, {-3, -2, 2, 2});
        Check(edge.width == 2 && edge.height == 2 && edge.pixels == std::vector<uint32_t>{0,1,4,5},
            "Edge crop was not clipped to the image");
        Check(CropForOcr(pixels, {1,1,1,2}).pixels.empty(), "Zero-width region must not scan");
        Check(CropForOcr(pixels, {10,10,12,12}).pixels.empty(), "Outside region must not scan");
        Check(CropForOcr(Image{4,3,{1}}, {0,0,4,3}).pixels.empty(), "Invalid source pixels must be rejected");
        const auto layout = TextLayout::FromFragments({
            Word(L"Menu", 0, 0), Word(L"Hello", 200, 0),
            Word(L"world", 300, 0), Word(L"Next", 200, 40)});
        const auto selected = layout.Select({190, -5}, {360, 65});
        Check(selected.text == L"Hello world\r\nNext", "Selection must exclude the sidebar and span empty space");
        Check(layout.Select({360, 65}, {190, -5}).text == selected.text, "Reverse drag must select the same text");
        Check(layout.Select({210, 5}, {230, 15}).text == L"el", "Partial words must follow the rectangle");
        Check(layout.Select({150, 25}, {180, 35}).text.empty(), "Empty area must not select text");
        Check(layout.Select({210, 5}, {210, 5}).text.empty(), "A click must not select text");
        Check(layout.Select({190, 0}, {360, 0}).text == L"Hello world", "A horizontal drag must select its row");
        for (const auto& span : selected.spans) {
            Check(span.length && span.offset + span.length <= selected.text.size(), "Invalid source-text span");
        }
        const TextWord unicode{{{0, 0}, {10, 1}, {30, 3}, {40, 5}}, {0, 0, 40, 20}};
        const auto unicodeLayout = TextLayout::FromFragments({{{unicode.bounds, {unicode}}, L"A\U0001F600e\u0301"}});
        Check(unicodeLayout.Select({11, 1}, {29, 19}).text == L"\U0001F600", "Surrogate pairs must stay intact");
        Check(unicodeLayout.Select({31, 1}, {39, 19}).text == L"e\u0301", "Combining marks must stay attached");
        Check(IsValidShortcut({MOD_WIN | MOD_SHIFT, 'D'}), "Default shortcut must be valid");
        Check(!IsValidShortcut({0, 'D'}), "Single keys must not be shortcuts");
        Check(!IsValidShortcut({MOD_CONTROL, VK_SHIFT}), "Modifier-only shortcuts are unsupported");
        Check(!IsValidShortcut({0x8000, 'D'}), "Unknown modifiers must be rejected");
        Check(FormatShortcut({MOD_WIN | MOD_SHIFT, 'D'}) == L"Win + Shift + D", "Shortcut label changed");
        std::cout << "Core regression checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
