#include "multilingual_ocr.h"
#include "transfer.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Image Render(const std::vector<std::wstring>& phrases, bool dark, bool mixed = false,
             int repetitions = 1, int fontSize = 32, int width = 1000, int height = 0) {
    const auto lineCount = static_cast<int>(phrases.size()) * repetitions;
    const auto lineHeight = fontSize + 20;
    Image image{width, height ? height : 24 + lineHeight * lineCount};
    image.pixels.resize(static_cast<size_t>(image.width) * image.height);
    const auto dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), image.width, -image.height, 1, 32, BI_RGB};
    void* pixels = nullptr;
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    Check(dc && bitmap && font, "Cannot render OCR fixture");
    const auto oldBitmap = SelectObject(dc, bitmap);
    const auto oldFont = SelectObject(dc, font);
    SetDCBrushColor(dc, dark ? RGB(24,24,24) : RGB(248,248,248));
    const RECT area{0,0,image.width,image.height};
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    for (int line = 0; line < lineCount; ++line) {
        const bool lightPanel = mixed && line == lineCount - 1;
        if (lightPanel) {
            const RECT panel{0, 12 + line * lineHeight, image.width, image.height};
            SetDCBrushColor(dc, RGB(248,248,248));
            FillRect(dc, &panel, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        SetTextColor(dc, dark && !lightPanel ? RGB(248,248,248) : RGB(24,24,24));
        const auto& phrase = phrases[line % phrases.size()];
        TextOutW(dc, 24, 12 + line * lineHeight, phrase.data(), static_cast<int>(phrase.size()));
    }
    GdiFlush();
    std::memcpy(image.pixels.data(), pixels, image.pixels.size() * sizeof(uint32_t));
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return image;
}

Image Sample(bool dark, bool mixed = false, int repetitions = 1) {
    return Render({L"Copy this text", L"Привет, мир!", L"Labdien, kā klājas?"}, dark, mixed, repetitions);
}

Image ColoredSample(uint32_t color) {
    auto image = Sample(false);
    const auto glyphs = image.pixels;
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
        const auto i = static_cast<size_t>(y) * image.width + x;
        const unsigned background = 70 + (x / 13 + y / 7) % 160;
        const unsigned ink = (248 - std::min(248u, glyphs[i] & 255)) * 255 / 224;
        uint32_t pixel = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            pixel |= (((color >> shift & 255) * ink + background * (255 - ink)) / 255) << shift;
        }
        image.pixels[i] = pixel;
    }
    return image;
}

Image SmallScreenText(bool dark) {
    return Render({
        L"Остаётся одно существенное ограничение: нынешний EXE не подписан и может вызывать SmartScreen.",
        L"Исходники приведены в порядок, история очищена, описание и лицензии включены.",
        L"Store Web Installer: Microsoft, MSIX, v1.0.",
        L"Labdien, kā klājas? Āboli, ķirši, ģimene, ļaudis, šodien, ūdens, žurnāls."
    }, dark, false, 1, 16, 3840, 2160);
}

std::wstring Text(const std::vector<std::vector<RecognizedWord>>& lines) {
    std::wstring text;
    for (const auto& line : lines) {
        for (const auto& word : line) {
            if (!text.empty()) text += L' ';
            text += word.text;
        }
    }
    return text;
}

void CheckContains(const std::wstring& text, std::wstring_view expected, const char* message) {
    if (text.find(expected) == text.npos) {
        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            nullptr, 0, nullptr, nullptr);
        std::string utf8(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            utf8.data(), bytes, nullptr, nullptr);
        std::cerr << "Recognized: " << utf8 << '\n';
        throw std::runtime_error(message);
    }
}

void CheckLanguages(const std::wstring& text) {
    CheckContains(text, L"Copy this text", "English recognition changed");
    CheckContains(text, L"Привет, мир!", "Russian recognition changed");
    CheckContains(text, L"Labdien, kā klājas?", "Latvian recognition changed");
}

OcrResult Wait(OcrWorker& worker) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until) {
        if (auto result = worker.TakeResult()) return std::move(*result);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("OCR worker timed out");
}

void CheckOblique(OcrWorker& worker, const Image& label, double degrees) {
    Image crop{340, 340, {}, {431, 122}};
    crop.pixels.assign(size_t(crop.width) * crop.height, label.pixels[0]);
    const double theta = degrees * std::acos(-1.) / 180, c = std::cos(theta), s = std::sin(theta);
    for (int y = 0; y < crop.height; ++y) for (int x = 0; x < crop.width; ++x) {
        const double dx = x - 170., dy = y - 170.;
        const long u = std::lround(c * dx + s * dy + label.width * .5);
        const long v = std::lround(-s * dx + c * dy + label.height * .5);
        if (u >= 0 && u < label.width && v >= 0 && v < label.height)
            crop.pixels[y * crop.width + x] = label.pixels[v * label.width + u];
    }
    worker.Submit(9, std::move(crop));
    auto result = Wait(worker);
    auto selection = result.layout.Select({431,122}, {771,462});
    CheckContains(selection.text, L"Vertical label", "Deskewed text changed");
    if (degrees == 180.) {
        Check(result.layout.frame.cosine < -.999, "Upside-down label lost its reading direction");
        return;
    }
    Check(selection.polygons.size() == 1, "Expected a single inclined band");
    const auto& p = selection.polygons.front();
    const double dx = p[1].x - p[0].x, dy = p[1].y - p[0].y;
    Check((dx * c + dy * s) / std::hypot(dx, dy) > .999, "Band has the wrong reading angle");
    // Verify the actual painted geometry covers the source glyphs at the crop offset.
    int misses = 0, ink = 0;
    for (int y = 0; y < label.height; ++y) for (int x = 0; x < label.width; ++x) {
        if ((label.pixels[y * label.width + x] & 255) > 80) continue;
        const double sx = 601 + c * (x - label.width * .5) - s * (y - label.height * .5);
        const double sy = 292 + s * (x - label.width * .5) + c * (y - label.height * .5);
        ++ink;
        for (size_t i = 0; i < p.size(); ++i) {
            const auto q = p[(i+1) % 4];
            if ((q.x-p[i].x)*(sy-p[i].y) < (q.y-p[i].y)*(sx-p[i].x)) { ++misses; break; }
        }
    }
    Check(ink && misses * 100 < ink, "Band misses source glyphs");
}
}

int main() {
    try {
        const MultilingualOcr engine;
        if (!engine) {
            std::cout << "OCR checks require the bundled runtime and eng/lav/rus models beside this executable.\n";
            return 77;
        }
        CheckLanguages(Text(engine.Recognize(Sample(false))));
        CheckLanguages(Text(engine.Recognize(Sample(true))));
        CheckLanguages(Text(engine.Recognize(Sample(true, true, 3))));
        for (const auto color : {0xffd200u, 0x00dcffu, 0xe628c8u}) {
            CheckLanguages(Text(engine.Recognize(ColoredSample(color))));
        }

        std::stop_source cancelled;
        cancelled.request_stop();
        Check(engine.Recognize(Sample(true), cancelled.get_token()).empty(), "Pre-cancelled scan ran");
        std::stop_source active;
        const auto dense = Sample(true, false, 10);
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
            active.request_stop();
        });
        Check(engine.Recognize(dense, active.get_token()).empty(), "Cancelled scan returned partial text");
        CheckLanguages(Text(engine.Recognize(Sample(false))));

        const auto owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(owner != nullptr, "Cannot create OCR test owner");
        {
            OcrWorker worker(owner, WM_APP + 1);
            worker.Submit(1, Sample(false));
            Check(Wait(worker).error == nullptr, "Worker warmup failed");
            for (const bool dark : {false, true}) {
                worker.Submit(10, SmallScreenText(dark));
                const auto small = Wait(worker);
                Check(!small.error, "Small screen text recognition failed");
                const auto text = small.layout.Select({0,0}, {3840,2160}).text;
                CheckContains(text, L"Остаётся одно существенное ограничение:",
                    "Small Russian text lost letters or punctuation on a large screen");
                CheckContains(text, L"порядок, история очищена,", "Small-text commas were lost");
                CheckContains(text, L"Store Web Installer: Microsoft", "English product names were changed");
                CheckContains(text, L"v1.0.", "Small-text version was changed");
                CheckContains(text, L"Labdien, kā klājas?", "Small Latvian text lost diacritics");
            }
            // Exercise the actual clipboard layout at a nonzero crop origin.
            const auto label = Render({L"Vertical label"}, false, false, 1, 36, 260, 68);
            for (double degrees : {-147.5, -32., 4.5, 48., 112., 179., 180.}) CheckOblique(worker, label, degrees);
            for (const int direction : {-1, 1}) {
                Image strip{label.height, label.width, {}, {83, 121}};
                strip.pixels.resize(label.pixels.size());
                for (int y = 0; y < label.height; ++y) for (int x = 0; x < label.width; ++x) {
                    const int rx = direction > 0 ? label.height - 1 - y : y;
                    const int ry = direction > 0 ? x : label.width - 1 - x;
                    strip.pixels[ry * strip.width + rx] = label.pixels[y * label.width + x];
                }
                worker.Submit(8, std::move(strip));
                const auto rotated = Wait(worker);
                Check(!rotated.error, "Vertical OCR failed");
                const auto selection = rotated.layout.Select({83,121}, {151,381});
                Check(selection.text == L"Vertical label" && selection.highlights.size() == 1,
                    "Rotated selection lost text, ordering or line geometry");
                const auto box = selection.highlights.front();
                Check(box.left >= 83 && box.right <= 151 && box.top >= 121 && box.bottom <= 381 &&
                    box.bottom-box.top > 3*(box.right-box.left), "Vertical highlight is misplaced");
                Check(rotated.layout.Select({0,0}, {80,380}).text.empty(),
                    "Selection outside the rotated caption must be empty");
            }
            worker.Submit(2, dense);
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
            worker.Submit(3, Sample(true));
            auto result = Wait(worker);
            Check(result.captureId == 3 && !result.error, "Superseded scan produced a result");
            CheckLanguages(result.layout.Select({0,0}, {1000,180}).text);
            worker.Submit(5, dense);
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
            worker.Cancel();
            Check(!worker.TakeResult(), "Cancelled region left a result queued");
            worker.Submit(6, Sample(false));
            const auto recovered = Wait(worker);
            Check(recovered.captureId == 6 && !recovered.error, "Explicit cancellation did not recover");
            CheckLanguages(recovered.layout.Select({0,0}, {1000,180}).text);
            worker.Submit(4, dense);
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
            // Destruction must cancel active OCR before joining its worker thread.
        }
        DestroyWindow(owner);
        std::cout << "OCR checks passed: light/dark/mixed and colored text, small 4K screen text, cancellation, recovery, superseded jobs, shutdown\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
