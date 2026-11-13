# Third-party notices

SelectAnyText's own source code and icon are licensed under the MIT License in
[LICENSE](LICENSE). Separately licensed components retain the terms below.

| Component | Packaged version | License |
| --- | --- | --- |
| Tesseract | 5.5.4 | Apache-2.0 |
| tessdata_fast models | 4.1.0 | Apache-2.0 |
| tessdata_best models (English and Russian) | 4.1.0 | Apache-2.0 |
| Leptonica | 1.87.0 | BSD-2-Clause |
| giflib | 6.1.3 | MIT |
| libjpeg-turbo | 3.2.1 | BSD-3-Clause and IJG |
| libpng | 1.6.59 | libpng-2.0 |
| libwebp and libsharpyuv | 1.6.0 | BSD-3-Clause |
| libtiff | 4.7.2 | libtiff |
| liblzma | 5.8.4 | 0BSD |
| zlib | 1.3.2 | Zlib |
| Microsoft Visual C++ runtime | 14.51.36231 | Microsoft redistribution terms |
| Inno Setup installer engine | 6.4.3 | Inno Setup License |

This software is based in part on the work of the Independent JPEG Group.

Complete upstream copyright and license notices are included in `licenses/`.
The models also retain their license in `tessdata/LICENSE`. Microsoft runtime
DLLs are distributed unmodified under the Visual Studio redistribution terms,
and are excluded from the project's MIT license.

The OCR runtime is built from the pinned vcpkg baseline in
`installer/vcpkg.json`. That baseline's Tesseract port applies upstream build
integration patches. The project's triplet disables precompiled headers for
Tesseract and disables Leptonica's unused JPEG 2000 decoder. The language model
files are unmodified. Their upstream sources and SHA-256 hashes are recorded in
`tessdata/MODELS.json`.

## Upstream sources

- [Tesseract](https://github.com/tesseract-ocr/tesseract/tree/5.5.4),
  [tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast/tree/4.1.0), and
  [tessdata_best](https://github.com/tesseract-ocr/tessdata_best/tree/4.1.0)
- [Pinned vcpkg ports and build patches](https://github.com/microsoft/vcpkg/tree/d02f7271407a3c23c9284f2feb8560d9545b51cd/ports)
- [Microsoft Visual Studio redistribution terms](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution)
- [Inno Setup 6.4.3 license](https://github.com/jrsoftware/issrc/blob/is-6_4_3/license.txt)
