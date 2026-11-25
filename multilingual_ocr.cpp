#include "multilingual_ocr.h"

#include "screen.h"
#include "ocr_color.h"
#include "caption_ocr.h"

#include <shlobj.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace {
std::wstring AppDirectory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        return {};
    }
    path.resize(length);
    path.resize(path.find_last_of(L"\\/") + 1);
    return path;
}

std::wstring SystemTesseractPath() {
    wchar_t folder[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, folder))) {
        return {};
    }
    return std::wstring(folder) + L"\\Tesseract-OCR\\libtesseract-5.dll";
}

std::string InstalledLanguages(const std::wstring& directory) {
    namespace fs = std::filesystem;
    std::vector<std::wstring> models;
    std::error_code error;
    for (fs::directory_iterator item(directory, error), end; !error && item != end; item.increment(error)) {
        std::error_code typeError;
        if (item->path().extension() != L".traineddata" || !item->is_regular_file(typeError)) {
            continue;
        }
        auto code = item->path().stem().wstring();
        if (!code.empty() && code != L"osd" && code != L"equ" &&
            code.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789_") == std::wstring::npos) {
            models.push_back(std::move(code));
        }
    }
    std::sort(models.begin(), models.end());
    std::string languages;
    const auto append = [&languages](std::wstring_view code) {
        if (!languages.empty()) languages += '+';
        for (const auto character : code) languages += static_cast<char>(character);
    };
    // Preserve the recognition order used by earlier installations.
    for (const auto preferred : {L"lav", L"eng", L"rus"}) {
        const auto model = std::find(models.begin(), models.end(), preferred);
        if (model != models.end()) {
            append(*model);
            models.erase(model);
        }
    }
    for (const auto& model : models) append(model);
    return languages;
}

std::string Utf8Path(const std::wstring& directory) {
    const auto size = static_cast<int>(directory.size());
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, directory.data(), size, nullptr, 0, nullptr, nullptr);
    std::string path(bytes, '\0');
    if (bytes) {
        WideCharToMultiByte(CP_UTF8, 0, directory.data(), size, path.data(), bytes, nullptr, nullptr);
    }
    return path;
}

std::wstring Utf8Text(std::string_view text) {
    const auto size = static_cast<int>(text.size());
    const auto characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (!characters) {
        return {};
    }
    std::wstring result(characters, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), characters);
    return result;
}

std::vector<std::vector<RecognizedWord>> ParseWords(std::string_view data) {
    std::vector<std::vector<RecognizedWord>> lines;
    std::array<std::string_view, 3> previousLine{};
    while (!data.empty()) {
        const auto newline = data.find('\n');
        auto row = data.substr(0, newline);
        data.remove_prefix(newline == std::string_view::npos ? data.size() : newline + 1);
        if (!row.starts_with("5\t")) {
            continue;
        }
        std::array<std::string_view, 11> fields{};
        bool valid = true;
        for (auto& field : fields) {
            const auto tab = row.find('\t');
            if (tab == std::string_view::npos) {
                valid = false;
                break;
            }
            field = row.substr(0, tab);
            row.remove_prefix(tab + 1);
        }
        if (!valid) {
            continue;
        }
        std::array<LONG, 4> box{};
        for (size_t index = 0; index < box.size(); ++index) {
            const auto value = fields[index + 6];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), box[index]);
            valid &= parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        }
        if (!row.empty() && row.back() == '\r') {
            row.remove_suffix(1);
        }
        auto text = Utf8Text(row);
        if (!valid || box[2] <= 0 || box[3] <= 0 ||
            text.find_first_not_of(L" \t\r\n\u00a0") == std::wstring::npos) {
            continue;
        }
        const std::array line{fields[2], fields[3], fields[4]};
        if (lines.empty() || previousLine != line) {
            lines.emplace_back();
            previousLine = line;
        }
        float confidence = 0;
        std::from_chars(fields[10].data(), fields[10].data() + fields[10].size(), confidence);
        lines.back().push_back({std::move(text),
            {box[0], box[1], box[0] + box[2], box[1] + box[3]}, confidence});
    }
    return lines;
}

// A thin box around a label can look like a pair of brackets after deskewing.
// Retry inside two visible rules, and only accept an independent reading of
// the same letters. Real brackets inside the box remain part of the raster.
void ReadInsideRules(std::vector<std::vector<RecognizedWord>>& lines,
                     const std::vector<unsigned char>& gray, int w, int h,
                     const auto& scan, std::stop_token cancel) {
    constexpr std::wstring_view border = L"|[]()";
    const auto content = [&](const auto& words) {
        std::wstring joined;
        for (const auto& word : words) joined += word.text;
        const auto start = joined.find_first_not_of(border);
        return start == joined.npos ? std::wstring{} :
            joined.substr(start, joined.find_last_not_of(border) - start + 1);
    };
    for (auto& row : lines) {
        if (cancel.stop_requested()) return;
        if (row.empty() || (border.find(row.front().text.front()) == border.npos &&
                            border.find(row.back().text.back()) == border.npos)) continue;
        RECT b{};
        for (const auto& word : row) UnionRect(&b, &b, &word.bounds);
        const int height = b.bottom - b.top, width = b.right - b.left;
        if (height < 8 || height > 140 || width < 2 * height) continue;
        const int mid = (b.top + b.bottom) / 2;
        const auto rule = [&](int direction) -> RECT {
            for (int y = mid + direction * std::max(3, height / 6);
                 y >= 2 && y + 2 < h && std::abs(y - mid) <= height; y += direction) {
                const auto* p = gray.data() + size_t(y) * w;
                int left = -1, last = -1;
                const int end = std::min<int>(w, b.right + height);
                for (int x = std::max<int>(0, b.left - height); x <= end; ++x) {
                    if (x < end && std::max(std::abs(p[x]-p[x-2*w]), std::abs(p[x]-p[x+2*w])) > 12) {
                        if (left < 0) left = x;
                        last = x;
                    }
                    if (left < 0 || (x < end && x - last <= 2)) continue;
                    if (last - left >= width * .80 && left <= b.left + height / 2 &&
                        last >= b.right - height / 2) return {left,y,last+1,y+1};
                    left = -1;
                }
            }
            return {};
        };
        const RECT top = rule(-1), bottom = rule(1);
        if (IsRectEmpty(&top) || IsRectEmpty(&bottom) ||
            std::abs(top.left-bottom.left) > height/3 || std::abs(top.right-bottom.right) > height/3) continue;
        const int x0 = std::max(top.left,bottom.left), y0 = top.top + 1;
        const int cw = std::min(top.right,bottom.right) - x0, ch = bottom.top - y0;
        if (cw < 1 || ch < 6 || ch * 3 < height) continue;
        constexpr int pad = 8;
        const int scale = std::clamp(36/ch, 1, 4);
        const int sw = cw*scale + 2*pad, sh = ch*scale + 2*pad;
        std::vector<unsigned char> strip(size_t(sw)*sh, gray[size_t(y0)*w+x0]);
        for (int y=0; y<ch*scale; ++y) for (int x=0; x<cw*scale; ++x)
            strip[size_t(y+pad)*sw+x+pad] = gray[size_t(y0+y/scale)*w+x0+x/scale];
        auto retry = scan(strip, sw, sh, 7);
        if (retry.size() != 1 || retry.front().empty() || content(row) != content(retry.front()) ||
            std::any_of(retry.front().begin(), retry.front().end(),
                        [](const auto& word) { return word.confidence < 75; })) continue;
        for (auto& word : retry.front()) {
            const RECT r = word.bounds;
            word.bounds = {x0+(r.left-pad)/scale, y0+(r.top-pad)/scale,
                x0+(r.right-pad+scale-1)/scale, y0+(r.bottom-pad+scale-1)/scale};
        }
        row = std::move(retry.front());
    }
}
}

struct MultilingualOcr::Impl {
    HMODULE library = nullptr;
    void* engine = nullptr;
    void (*destroy)(void*) = nullptr;
    void (*segmentation)(void*, int) = nullptr;
    void (*setImage)(void*, const unsigned char*, int, int, int, int) = nullptr;
    char* (*getTsv)(void*, int) = nullptr;
    void (*deleteText)(const char*) = nullptr;
    void (*clear)(void*) = nullptr;
    int (*recognize)(void*, void*) = nullptr;
    void* (*createMonitor)() = nullptr;
    void (*deleteMonitor)(void*) = nullptr;
    void (*setCancel)(void*, bool (*)(void*, int)) = nullptr;
    void (*setCancelContext)(void*, void*) = nullptr;

    ~Impl() {
        if (engine && destroy) {
            destroy(engine);
        }
        if (library) {
            FreeLibrary(library);
        }
    }

    template<typename Function>
    Function Symbol(const char* name) const {
        return std::bit_cast<Function>(GetProcAddress(library, name));
    }
};

MultilingualOcr::MultilingualOcr() {
    const auto app = AppDirectory();
    if (app.empty()) {
        return;
    }
    const auto models = app + L"tessdata";
    const auto languages = InstalledLanguages(models);
    const auto directory = Utf8Path(models);
    if (languages.empty() || directory.empty()) {
        return;
    }

    auto api = std::make_unique<Impl>();
    constexpr DWORD searchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    api->library = LoadLibraryExW((app + L"ocr\\libtesseract-5.dll").c_str(),
        nullptr, searchFlags);
    if (!api->library) {
        const auto systemPath = SystemTesseractPath();
        if (!systemPath.empty()) {
            api->library = LoadLibraryExW(systemPath.c_str(), nullptr, searchFlags);
        }
    }
    if (!api->library) {
        return;
    }
    using Init = int (*)(void*, const char*, const char*, int);
    const auto create = api->Symbol<void* (*)()>("TessBaseAPICreate");
    const auto init = api->Symbol<Init>("TessBaseAPIInit2");
    api->segmentation = api->Symbol<void (*)(void*, int)>("TessBaseAPISetPageSegMode");
    api->destroy = api->Symbol<decltype(api->destroy)>("TessBaseAPIDelete");
    api->setImage = api->Symbol<decltype(api->setImage)>("TessBaseAPISetImage");
    api->getTsv = api->Symbol<decltype(api->getTsv)>("TessBaseAPIGetTsvText");
    api->deleteText = api->Symbol<decltype(api->deleteText)>("TessDeleteText");
    api->clear = api->Symbol<decltype(api->clear)>("TessBaseAPIClear");
    api->recognize = api->Symbol<decltype(api->recognize)>("TessBaseAPIRecognize");
    api->createMonitor = api->Symbol<decltype(api->createMonitor)>("TessMonitorCreate");
    api->deleteMonitor = api->Symbol<decltype(api->deleteMonitor)>("TessMonitorDelete");
    api->setCancel = api->Symbol<decltype(api->setCancel)>("TessMonitorSetCancelFunc");
    api->setCancelContext = api->Symbol<decltype(api->setCancelContext)>("TessMonitorSetCancelThis");
    if (!create || !init || !api->segmentation || !api->destroy || !api->setImage || !api->getTsv ||
        !api->deleteText || !api->clear || !api->recognize || !api->createMonitor ||
        !api->deleteMonitor || !api->setCancel || !api->setCancelContext) {
        return;
    }
    constexpr int lstmOnly = 1;
    api->engine = create();
    if (!api->engine || init(api->engine, directory.c_str(), languages.c_str(), lstmOnly) != 0) {
        return;
    }
    constexpr int automaticLayout = 3;
    api->segmentation(api->engine, automaticLayout);
    impl_ = std::move(api);
}

MultilingualOcr::~MultilingualOcr() = default;

MultilingualOcr::operator bool() const {
    return impl_ != nullptr;
}

std::vector<std::vector<RecognizedWord>> MultilingualOcr::Recognize(const Image& image, std::stop_token cancel, OcrPass pass) const {
    if (cancel.stop_requested()) {
        return {};
    }
    std::vector<unsigned char> grayscale(image.pixels.size());
    std::transform(image.pixels.begin(), image.pixels.end(), grayscale.begin(), [](uint32_t pixel) {
        const auto blue = pixel & 0xFF;
        const auto green = (pixel >> 8) & 0xFF;
        const auto red = (pixel >> 16) & 0xFF;
        return static_cast<unsigned char>((29 * blue + 150 * green + 77 * red) >> 8);
    });
    // Dark interfaces otherwise often require a second LSTM pass per line.
    // Prefer dark text on a light background; keep Tesseract's automatic
    // per-line inversion enabled for images containing both polarities.
    size_t samples = 0;
    size_t dark = 0;
    const auto step = std::max(size_t{1}, grayscale.size() / 4096);
    for (size_t index = 0; index < grayscale.size(); index += step) {
        dark += grayscale[index] < 128;
        ++samples;
    }
    if (pass != OcrPass::Display && dark > samples * 3 / 4) {
        std::transform(grayscale.begin(), grayscale.end(), grayscale.begin(),
            [](unsigned char pixel) { return static_cast<unsigned char>(255 - pixel); });
    }
    const auto& api = *impl_;
    const std::unique_ptr<void, decltype(api.deleteMonitor)> monitor(api.createMonitor(), api.deleteMonitor);
    if (!monitor) {
        throw std::runtime_error("Could not create OCR monitor");
    }
    api.setCancelContext(monitor.get(), &cancel);
    api.setCancel(monitor.get(), [](void *context, int) {
        return static_cast<std::stop_token *>(context)->stop_requested();
    });
    const auto scan = [&](const std::vector<unsigned char> &pixels, int w, int h, int mode) {
        api.segmentation(api.engine, mode);
        api.setImage(api.engine, pixels.data(), w, h, 1, w);
        const auto status = api.recognize(api.engine, monitor.get());
        if (cancel.stop_requested() || status != 0) {
            api.clear(api.engine);
            if (cancel.stop_requested()) {
                return std::vector<std::vector<RecognizedWord>>{};
            }
            throw std::runtime_error("Tesseract OCR failed");
        }
        const std::unique_ptr<char, decltype(api.deleteText)> text(api.getTsv(api.engine, 0), api.deleteText);
        api.clear(api.engine);
        if (!text) {
            throw std::runtime_error("Tesseract OCR failed");
        }
        return ParseWords(text.get());
    };
    if (pass == OcrPass::AlignedLine) {
        auto rows = scan(grayscale, image.width, image.height, 6);
        if (!cancel.stop_requested() && ColorTextScore(ReliableColorText(rows)) <= 0) {
            auto sparse = ReliableColorText(scan(grayscale, image.width, image.height, 11));
            rows = MergeColorText(std::move(rows), sparse);
        }
        ReadInsideRules(rows, grayscale, image.width, image.height, scan, cancel);
        return rows;
    }
    if (pass == OcrPass::Display) {
        // Logos, titles and small labels can occupy the same narrow strip.
        // Keep whole word boxes and resolve overlapping readings across scales.
        std::vector<RecognizedWord> choices;
        const auto collect = [&](auto rows, int scale, int dx = 0, int dy = 0) {
            for (auto &row : rows)
                for (auto &word : row) {
                    RECT &b = word.bounds;
                    b = {b.left / scale, b.top / scale, (b.right + scale - 1) / scale,
                         (b.bottom + scale - 1) / scale};
                    OffsetRect(&b, dx, dy);
                    if (word.text.size() < 2 || word.confidence < 47 || b.right - b.left < b.bottom - b.top)
                        continue;
                    choices.push_back(std::move(word));
                }
        };
        collect(scan(grayscale, image.width, image.height, 6), 1);
        if (cancel.stop_requested()) return {};
        collect(scan(grayscale, image.width, image.height, 11), 1);
        if (cancel.stop_requested()) return {};
        if (image.pixels.size() <= 450000) {
            constexpr int scale = 3;
            const int w = image.width * scale, h = image.height * scale;
            std::vector<unsigned char> large(size_t(w) * h);
            for (int y = 0; y < h; ++y) {
                if (cancel.stop_requested()) return {};
                const double sy = std::clamp((y + .5) / scale - .5, 0., double(image.height - 1));
                const int iy = int(sy), next = std::min(iy + 1, image.height - 1);
                for (int x = 0; x < w; ++x) {
                    const double sx = std::clamp((x + .5) / scale - .5, 0., double(image.width - 1));
                    const int ix = int(sx), jx = std::min(ix + 1, image.width - 1);
                    const auto at = [&](int row) {
                        return std::lerp(double(grayscale[row * image.width + ix]),
                                         double(grayscale[row * image.width + jx]), sx - ix);
                    };
                    large[y * w + x] =
                        static_cast<unsigned char>(std::lround(std::lerp(at(iy), at(next), sy - iy)));
                }
            }
            collect(scan(large, w, h, 11), scale);
            if (cancel.stop_requested()) return {};
            collect(scan(large, w, h, 6), scale);
        }
        if (cancel.stop_requested()) return {};
        if (image.pixels.size() <= 450000) {
            // Projection of bright strokes separates labels of different sizes.
            // Split wide gaps too: an unrelated logo can share a label's baseline.
            std::array<int, 256> hist{};
            for (auto p : grayscale)
                ++hist[p];
            int count = 0, threshold = 255;
            while (threshold > 0 && (count += hist[threshold]) < int(grayscale.size() / 12))
                --threshold;
            auto masks = ColorTextMasks(image, cancel);
            std::vector<unsigned char> bright(grayscale.size());
            for (size_t i = 0; i < bright.size(); ++i)
                bright[i] = grayscale[i] >= threshold ? 0 : 255;
            masks.push_back(std::move(bright));
            std::vector<unsigned char> soft(grayscale.size());
            for (size_t i = 0; i < soft.size(); ++i)
                soft[i] = grayscale[i] >= threshold * .90 ? 0 : 255;
            masks.push_back(std::move(soft));
            int budget = 24;
            for (const auto &mask : masks) {
                std::vector<int> rows(image.height);
                for (int y = 0; y < image.height; ++y)
                    for (int x = 0; x < image.width; ++x)
                        rows[y] += mask[y * image.width + x] == 0;
                for (int top = 0; top < image.height && budget > 0;) {
                    if (rows[top] < std::max(2, image.width / 120)) {
                        ++top;
                        continue;
                    }
                    int end = top + 1;
                    while (end < image.height && rows[end] >= std::max(2, image.width / 120))
                        ++end;
                    const int height = end - top;
                    if (height >= 4 && height <= 160) {
                        int left = -1, last = -1;
                        for (int x = 0; x <= image.width && budget > 0; ++x) {
                            bool ink = false;
                            if (x < image.width)
                                for (int y = top; y < end && !ink; ++y)
                                    ink = mask[y * image.width + x] == 0;
                            if (ink) {
                                if (left < 0) left = x;
                                last = x;
                            }
                            if (left < 0 || (x < image.width && x - last < std::max(9, height / 2))) continue;
                            const int width = last - left + 1;
                            if (width >= height && width > 8) {
                                if (cancel.stop_requested()) return {};
                                const int scale = std::clamp(48 / height, 1, 6), pad = 4;
                                const int w = (width + 2 * pad) * scale, h = (height + 2 * pad) * scale;
                                std::vector<unsigned char> strip(size_t(w) * h, 255);
                                for (int y = 0; y < height * scale; ++y)
                                    for (int xx = 0; xx < width * scale; ++xx)
                                        strip[(y + pad * scale) * w + xx + pad * scale] =
                                            mask[(top + y / scale) * image.width + left + xx / scale];
                                collect(scan(strip, w, h, 7), scale, left - pad, top - pad);
                                if (height <= 14 && !cancel.stop_requested()) {
                                    // Preserve antialiasing when the detected capitals are only a few pixels
                                    // tall.
                                    for (int y = 0; y < h; ++y)
                                        for (int xx = 0; xx < w; ++xx) {
                                            const double sx = std::clamp(left - pad + (xx + .5) / scale - .5,
                                                                         0., double(image.width - 1));
                                            const double sy = std::clamp(top - pad + (y + .5) / scale - .5,
                                                                         0., double(image.height - 1));
                                            const int ix = int(sx), iy = int(sy);
                                            const auto at = [&](int yy) {
                                                return std::lerp(
                                                    double(grayscale[yy * image.width + ix]),
                                                    double(grayscale[yy * image.width +
                                                                     std::min(ix + 1, image.width - 1)]),
                                                    sx - ix);
                                            };
                                            strip[y * w + xx] = static_cast<unsigned char>(
                                                255 - std::lround(std::lerp(
                                                          at(iy), at(std::min(iy + 1, image.height - 1)),
                                                          sy - iy)));
                                        }
                                    collect(scan(strip, w, h, 7), scale, left - pad, top - pad);
                                }
                                --budget;
                            }
                            left = -1;
                        }
                    }
                    top = end;
                }
            }
        }
        if (cancel.stop_requested()) return {};
        const auto merit = [](const RecognizedWord &word) {
            return (word.confidence / 100) * std::min<size_t>(word.text.size(), 12);
        };
        std::stable_sort(choices.begin(), choices.end(),
                         [&](const auto &a, const auto &b) { return merit(a) > merit(b); });
        std::vector<std::vector<RecognizedWord>> rows;
        for (auto &word : choices) {
            if (word.confidence < 55 || (word.text.size() <= 3 && word.confidence < 85)) continue;
            const auto collides = [&](const auto &row) {
                RECT overlap{};
                if (!IntersectRect(&overlap, &row.front().bounds, &word.bounds)) return false;
                const auto area = [](RECT r) { return double(r.right - r.left) * (r.bottom - r.top); };
                return area(overlap) >= .40 * std::min(area(word.bounds), area(row.front().bounds));
            };
            if (std::none_of(rows.begin(), rows.end(), collides)) rows.push_back({std::move(word)});
        }
        return rows;
    }
    auto result = scan(grayscale, image.width, image.height, 3);
    if (cancel.stop_requested()) return {};
    std::vector<std::vector<RecognizedWord>> best;
    double bestScore = 0;
    auto masks = ColorTextMasks(image, cancel);
    for (const auto &mask : masks) {
        if (cancel.stop_requested()) return {};
        auto candidate = ReliableColorText(scan(mask, image.width, image.height, 3));
        const auto score = ColorTextScore(candidate);
        if (score > bestScore) {
            bestScore = score;
            best = std::move(candidate);
        }
    }
    if (cancel.stop_requested()) return {};
    result = MergeColorText(std::move(result), best);
    // Bound the retry on ambiguous selections. Normal UI keeps its fast path.
    if (image.width < 12 || image.height < 8 || image.pixels.size() > 1000000 ||
        ColorTextScore(ReliableColorText(result)) > 0)
        return result;
    for (auto &wall : caption::OutlineColors(image))
        masks.push_back(std::move(wall));
    for (const auto &mask : masks)
        for (bool tight : {false, true}) {
            auto filled = caption::EnclosedFill(image, mask, cancel, tight);
            if (cancel.stop_requested()) return {};
            if (filled.pixels.empty()) continue;
            auto rows = scan(filled.pixels, filled.w, filled.h, 6);
            auto reliable = rows;
            std::erase_if(reliable,
                          [](const auto &row) { return ColorTextScore(ReliableColorText({row})) <= 0; });
            if (ColorTextScore(reliable) <= 0) continue;
            for (auto &row : reliable)
                for (auto &word : row) {
                    auto &b = word.bounds;
                    b = {(b.left - 8) / 2, (b.top - 8) / 2, (b.right - 8 + 1) / 2, (b.bottom - 8 + 1) / 2};
                }
            result = MergeColorText(std::move(result), reliable);
        }
    if (ColorTextScore(ReliableColorText(result)) > 0) return result;
    for (const auto &line : caption::Locate(image, cancel)) {
        double bestLineScore = 0;
        std::vector<std::vector<RecognizedWord>> bestLine;
        for (double shear : {0.0, 0.18}) {
            if (cancel.stop_requested()) return {};
            const auto raster = caption::Render(image, line, shear, cancel);
            if (cancel.stop_requested()) return {};
            if (raster.pixels.empty()) continue;
            auto words = scan(raster.pixels, raster.w, raster.h, 7);
            if (words.size() != 1) continue;
            auto &row = words.front();
            std::erase_if(row, [](const auto &word) { return word.confidence < 25; });
            double score = 0;
            size_t length = 0;
            for (auto &word : row) {
                score += word.confidence * word.text.size();
                length += word.text.size();
                const RECT b = word.bounds;
                const double offset1 = shear * (b.top - raster.h * .5),
                             offset2 = shear * (b.bottom - raster.h * .5);
                word.bounds = {
                    line.box.left + LONG(std::floor((b.left - raster.padding - std::max(offset1, offset2)) /
                                                    raster.scale)),
                    line.box.top + (b.top - raster.padding) / raster.scale,
                    line.box.left + LONG(std::ceil((b.right - raster.padding - std::min(offset1, offset2)) /
                                                   raster.scale)),
                    line.box.top + (b.bottom - raster.padding + raster.scale - 1) / raster.scale};
                IntersectRect(&word.bounds, &word.bounds, &line.box);
            }
            if (length >= 3 && score / length >= 72 && score > bestLineScore) {
                bestLineScore = score;
                bestLine = std::move(words);
            }
        }
        if (!bestLine.empty()) result = MergeColorText(std::move(result), bestLine);
    }
    return cancel.stop_requested() ? std::vector<std::vector<RecognizedWord>>{} : result;
}
