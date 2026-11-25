#include "settings.h"
#include "text_layout.h"
#include "capture_region.h"
#include "screen.h"
#include "ocr_color.h"

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
        Check(selected.highlights.size() == 2, "Normal word spaces must form continuous line bands");
        auto shortWord = Word(L"lower", 80, 5);
        shortWord.line.bounds.bottom = shortWord.line.words[0].bounds.bottom = 18;
        const auto mixedHeight = TextLayout::FromFragments({Word(L"Tall", 0, 0), shortWord});
        const auto band = mixedHeight.Select({-1,-1}, {200,30});
        Check(band.highlights.size() == 1 && band.highlights[0].top == -2 && band.highlights[0].bottom == 22,
            "Different word heights must share one stable line band");
        const auto partial = mixedHeight.Select({81,6}, {110,17});
        Check(partial.highlights.size() == 1 && partial.highlights[0].top == -2 && partial.highlights[0].bottom == 22,
            "Partial selection must retain the full line height");
        Check(layout.Select({-1,-1}, {360,30}).highlights.size() == 2, "Large column gaps must stay separate");
        auto distorted = Word(L"First", 0, 0);
        auto tall = Word(L"second", 80, 0);
        tall.line.words[0].bounds.bottom = 80;
        for (auto& stop : tall.line.words[0].stops) stop.offset += distorted.text.size() + 1;
        distorted.text += L" " + tall.text;
        distorted.line.words.push_back(tall.line.words[0]);
        distorted.line.bounds = {0,0,140,80};
        const auto mixedFonts = TextLayout::FromFragments({distorted}).Select({-1,-1}, {200,100});
        Check(mixedFonts.highlights[0].bottom == 88, "Legitimate mixed font sizes must not be clipped");
        const auto repaired = TextLayout::FromFragments({distorted, Word(L"Next", 0, 50)});
        const auto repairedSelection = repaired.Select({-1,-1}, {200,100});
        Check(repairedSelection.text == L"First second\r\nNext" && repairedSelection.highlights.size() == 2,
            "A tall OCR word box must not merge two physical rows");
        Check(repairedSelection.highlights[0].bottom == 22 && repairedSelection.highlights[1].top == 48,
            "One tall word must not inflate its row's highlight");
        const auto touching = TextLayout::FromFragments({Word(L"Upper",0,0), Word(L"Lower",0,20)});
        const auto touchingSelection = touching.Select({-1,-1}, {200,60});
        Check(touchingSelection.highlights.size() == 2 &&
            touchingSelection.highlights[0].bottom < touchingSelection.highlights[1].top,
            "Adjacent bands must not overlap or darken each other");
        const Image gray{20,20,std::vector<uint32_t>(400,0xeeeeee)};
        Check(ColorTextMasks(gray, {}).empty(), "Monochrome screens must not trigger extra OCR passes");
        const std::vector<std::vector<RecognizedWord>> colored{{{L"Colored title",{0,0,150,20},90}}};
        auto merged = MergeColorText({{{L"garbage",{0,0,50,20},20}},{{L"Footer",{0,100,70,120},95}}}, colored);
        Check(merged.size()==2 && merged[0][0].text==L"Footer" && merged[1][0].text==L"Colored title",
            "Color repair must replace unreliable text and preserve unrelated text");
        merged = MergeColorText({{{L"Reliable original",{0,0,150,20},95}}}, colored);
        Check(merged.size()==1 && merged[0][0].text==L"Reliable original", "Color retry must not overwrite confident original text");
        merged = MergeColorText({{{L"title",{90,0,150,20},95}}}, colored);
        Check(merged.size()==1 && merged[0][0].text==L"Colored title", "Confident fragments must not block complete colored text");
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
