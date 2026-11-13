#include "multilingual_ocr.h"
#include "transfer.h"

#include <chrono>
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
        std::cout << "OCR checks passed: light/dark/mixed text, small 4K screen text, cancellation, recovery, superseded jobs, shutdown\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
