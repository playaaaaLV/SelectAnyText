#include "multilingual_ocr.h"

#include "screen.h"

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
        if (!valid || box[2] <= 0 || box[3] <= 0 || text.empty()) {
            continue;
        }
        const std::array line{fields[2], fields[3], fields[4]};
        if (lines.empty() || previousLine != line) {
            lines.emplace_back();
            previousLine = line;
        }
        lines.back().push_back({std::move(text),
            {box[0], box[1], box[0] + box[2], box[1] + box[3]}});
    }
    return lines;
}
}

struct MultilingualOcr::Impl {
    HMODULE library = nullptr;
    void* engine = nullptr;
    void (*destroy)(void*) = nullptr;
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
    const auto segmentation = api->Symbol<void (*)(void*, int)>("TessBaseAPISetPageSegMode");
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
    if (!create || !init || !segmentation || !api->destroy || !api->setImage || !api->getTsv ||
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
    segmentation(api->engine, automaticLayout);
    impl_ = std::move(api);
}

MultilingualOcr::~MultilingualOcr() = default;

MultilingualOcr::operator bool() const {
    return impl_ != nullptr;
}

std::vector<std::vector<RecognizedWord>> MultilingualOcr::Recognize(const Image& image, std::stop_token cancel) const {
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
    if (dark > samples * 3 / 4) {
        std::transform(grayscale.begin(), grayscale.end(), grayscale.begin(),
            [](unsigned char pixel) { return static_cast<unsigned char>(255 - pixel); });
    }
    const auto& api = *impl_;
    const std::unique_ptr<void, decltype(api.deleteMonitor)> monitor(api.createMonitor(), api.deleteMonitor);
    if (!monitor) {
        throw std::runtime_error("Could not create OCR monitor");
    }
    api.setCancelContext(monitor.get(), &cancel);
    api.setCancel(monitor.get(), [](void* context, int) {
        return static_cast<std::stop_token*>(context)->stop_requested();
    });
    api.setImage(api.engine, grayscale.data(), image.width, image.height, 1, image.width);
    const auto status = api.recognize(api.engine, monitor.get());
    if (cancel.stop_requested() || status != 0) {
        api.clear(api.engine);
        if (cancel.stop_requested()) {
            return {};
        }
        throw std::runtime_error("Tesseract OCR failed");
    }
    const std::unique_ptr<char, decltype(api.deleteText)> text(api.getTsv(api.engine, 0), api.deleteText);
    api.clear(api.engine);
    if (!text) {
        throw std::runtime_error("Tesseract OCR failed");
    }
    return ParseWords(text.get());
}
