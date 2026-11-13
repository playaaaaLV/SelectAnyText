param(
    [Parameter(Mandatory)][string]$ReleaseDir,
    [Parameter(Mandatory)][string]$VcpkgTripletDir,
    [Parameter(Mandatory)][string]$CrtDir,
    [Parameter(Mandatory)][string]$Iscc,
    [string]$Version = '1.0',
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\dist')
)

$ErrorActionPreference = 'Stop'
$ReleaseDir = (Resolve-Path -LiteralPath $ReleaseDir).Path
$VcpkgTripletDir = (Resolve-Path -LiteralPath $VcpkgTripletDir).Path
$CrtDir = (Resolve-Path -LiteralPath $CrtDir).Path
$OutputDir = (New-Item -ItemType Directory -Path $OutputDir -Force).FullName
$stage = Join-Path $OutputDir ('payload-' + [guid]::NewGuid().ToString('N'))
$ocr = Join-Path $stage 'ocr'
$licenses = Join-Path $ocr 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
$project = Split-Path $PSScriptRoot -Parent
Copy-Item -LiteralPath (Join-Path $project 'LICENSE'), (Join-Path $project 'THIRD_PARTY_NOTICES.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $project 'licenses') -Destination $stage -Recurse
$installationLicense = "SelectAnyText application license (MIT)`r`n`r`n" +
    (Get-Content -LiteralPath (Join-Path $project 'LICENSE') -Raw) +
    "`r`nMicrosoft Visual C++ runtime terms (Microsoft components only)`r`n`r`n" +
    (Get-Content -LiteralPath (Join-Path $project 'licenses\microsoft-visual-cpp-runtime.txt') -Raw -Encoding UTF8)
[IO.File]::WriteAllText((Join-Path $stage 'INSTALLATION_LICENSE.txt'), $installationLicense, [Text.UTF8Encoding]::new($true))

Copy-Item -LiteralPath (Join-Path $ReleaseDir 'SelectAnyText.exe') -Destination $stage
$models = Join-Path $ReleaseDir 'tessdata'
$stagedModels = Join-Path $stage 'tessdata'
New-Item -ItemType Directory -Path $stagedModels | Out-Null
Copy-Item -LiteralPath (Join-Path $models 'LICENSE') -Destination $stagedModels
$modelManifest = @(Get-Content -LiteralPath (Join-Path $models 'MODELS.json') -Raw | ConvertFrom-Json)
Copy-Item -LiteralPath (Join-Path $models 'MODELS.json') -Destination $stagedModels

$languages = @(Import-Csv -LiteralPath (Join-Path $PSScriptRoot 'OcrLanguages.csv'))
if (!$languages.Count -or (@($languages.Code | Select-Object -Unique).Count -ne $languages.Count)) {
    throw 'OCR language codes must be nonempty and unique.'
}
$components = foreach ($language in $languages) {
    $code = $language.Code
    if ($code -notmatch '^[a-z][a-z0-9_]*$' -or $language.Name -notmatch '^[A-Za-z][A-Za-z ()-]*$') {
        throw 'Invalid OCR language code or display name.'
    }
    $file = "$code.traineddata"
    $model = @($modelManifest | Where-Object file -EQ $file)
    if ($model.Count -ne 1 -or $model[0].sha256 -ne (Get-FileHash -LiteralPath (Join-Path $models $file) -Algorithm SHA256).Hash) {
        throw "Model provenance or checksum mismatch: $file"
    }
    Copy-Item -LiteralPath (Join-Path $models $file) -Destination $stagedModels
    $types = if ($language.Default -eq 'yes') { 'standard full' } else { 'full' }
    [pscustomobject]@{
        Component = 'Name: "ocr_{0}"; Description: "{1}"; Types: {2}; Flags: disablenouninstallwarning' -f $code, $language.Name, $types
        File = 'Source: "{{#Payload}}\tessdata\{0}"; DestDir: "{{app}}\tessdata"; Flags: ignoreversion; Components: ocr_{1}' -f $file, $code
        Delete = 'Type: files; Name: "{{app}}\tessdata\{0}"; Components: not ocr_{1}' -f $file, $code
    }
}
$include = @('[Components]') + $components.Component + @('', '[Files]') +
    $components.File + @('', '[InstallDelete]') + $components.Delete
Set-Content -LiteralPath (Join-Path $stage 'OcrLanguages.iss') -Value $include -Encoding Ascii

$bin = Join-Path $VcpkgTripletDir 'bin'
$tesseract = @(Get-ChildItem -LiteralPath $bin -File | Where-Object Name -Match '^(lib)?tesseract.*\.dll$')
if ($tesseract.Count -ne 1) {
    throw 'Expected one Tesseract DLL in the vcpkg release bin directory.'
}
Copy-Item -LiteralPath $tesseract[0].FullName -Destination (Join-Path $ocr 'libtesseract-5.dll')
$runtimeFiles = @('gif.dll', 'jpeg62.dll', 'leptonica-*.dll', 'liblzma.dll',
    'libpng16.dll', 'libsharpyuv.dll', 'libwebp.dll', 'libwebpmux.dll', 'tiff.dll', 'z.dll')
foreach ($pattern in $runtimeFiles) {
    $matches = @(Get-ChildItem -LiteralPath $bin -File -Filter $pattern)
    if ($matches.Count -ne 1) { throw "Expected one runtime dependency matching $pattern." }
    Copy-Item -LiteralPath $matches[0].FullName -Destination $ocr
}
foreach ($name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    Copy-Item -LiteralPath (Join-Path $CrtDir $name) -Destination $ocr
}
foreach ($package in @('tesseract', 'leptonica', 'giflib', 'libjpeg-turbo', 'liblzma',
        'libpng', 'libwebp', 'tiff', 'zlib')) {
    $copyright = Join-Path $VcpkgTripletDir "share\$package\copyright"
    if (!(Test-Path -LiteralPath $copyright -PathType Leaf)) {
        throw "Missing license for $package."
    }
    Copy-Item -LiteralPath $copyright -Destination (Join-Path $licenses "$package.txt")
}

& $Iscc "/DAppVersion=$Version" "/DPayload=$stage" "/O$OutputDir" (Join-Path $PSScriptRoot 'SelectAnyText.iss')
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup exited with code $LASTEXITCODE."
}
