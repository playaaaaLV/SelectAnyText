# SelectAnyText

**Copy any text you can see on your screen.** From a screenshot, a PDF, a video frame, or an app that won’t let you select it.

[Download for Windows](https://github.com/playaaaaLV/SelectAnyText/releases/tag/v1.0)

- **Frame it. Copy it.** Hit **Win+Shift+D**, draw a rectangle, and release to scan. Press **Ctrl+C** to copy and close, or **Esc** to cancel.
- **See what you’re copying.** All recognized text is highlighted. Draw another rectangle to try a different area.
- **Works offline.** OCR runs on your PC.
- **Ready in the tray.** Starts when you sign in. Click the tray icon to change the shortcut.

The x64 installer puts SelectAnyText in `%LOCALAPPDATA%\Programs\SelectAnyText` and needs no administrator rights. If a copy is already running, choose **Exit** from its tray menu before installing.

Requires Windows 10 version 1809 or newer, or Windows 11. Screen capture and OCR
run locally. Captured screens are held in memory and selected text is placed on
the Windows clipboard. OCR accuracy depends on the source image.

## Build

Use Visual Studio 2022 or newer with Desktop development with C++, a Windows SDK, and CMake 3.21 or newer.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

For regression checks, configure with `-DSELECTANYTEXT_BUILD_TESTS=ON`,
build, and run `ctest --test-dir build -C Release --output-on-failure`.
OCR checks are skipped when the OCR engine is unavailable. To run the multilingual
fixtures with their reference configuration, copy `SelectAnyTextOcrTests.exe` to
a separate directory with the bundled runtime in `ocr/` and only the English,
Latvian, and Russian models in `tessdata/`, then run that executable.

The Release directory contains the executable and Tesseract models with their [Apache 2.0 license](tessdata/LICENSE). English and Russian use [tessdata_best 4.1.0](https://github.com/tesseract-ocr/tessdata_best/tree/4.1.0); the other models use [tessdata_fast 4.1.0](https://github.com/tesseract-ocr/tessdata_fast/tree/4.1.0). [MODELS.json](tessdata/MODELS.json) records their sources and SHA-256 hashes. The installer ships the OCR runtime. For a development build, copy the installed app's `ocr/` directory beside the executable, or install Tesseract 5 in Program Files.

## Add an OCR language

1. Find the [Tesseract language code](https://tesseract-ocr.github.io/tessdoc/Data-Files-in-different-versions.html) and download the matching `.traineddata` file from [tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast/tree/4.1.0).
2. For a development build, save the model in `tessdata/` and rebuild. For an existing installation, copy it into the `tessdata` folder next to `SelectAnyText.exe` and restart the app.
3. To include the language in the installer, add its code and English display name to [installer/OcrLanguages.csv](installer/OcrLanguages.csv), add its source URL, license, and SHA-256 hash to [tessdata/MODELS.json](tessdata/MODELS.json), then rebuild the installer. Test the model on an image with text in that language.

All installed models are loaded together. Keep only the languages you need: extra models increase startup time, memory use, and recognition ambiguity. Models from `tessdata_best` can also be used, but compare accuracy and speed before replacing a model.

## Package

Use PowerShell 5.1 or newer. Clone vcpkg and check out the baseline commit from
`installer/vcpkg.json`, then run its bootstrap script. From `installer`, run
`vcpkg install --triplet x64-windows --overlay-triplets=triplets` to build the
pinned OCR dependencies. Run [installer/Build-Installer.ps1](installer/Build-Installer.ps1)
with `-ReleaseDir`, `-VcpkgTripletDir`, `-CrtDir`, and `-Iscc`.
These point to the app's Release directory, `installer/vcpkg_installed/x64-windows`,
the x64 release CRT directory under Visual Studio's `VC/Redist/MSVC`, and
Inno Setup 6.4.3's `ISCC.exe`. Output goes to `dist/`.

## License

The application source and icon use the [MIT License](LICENSE).
Bundled components retain their [third-party licenses](THIRD_PARTY_NOTICES.md).
