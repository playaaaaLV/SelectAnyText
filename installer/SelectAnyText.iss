[Setup]
AppId={{F3B33EC1-4506-4935-A983-68FCDF9ED1E2}
AppName=SelectAnyText
AppVersion={#AppVersion}
AppMutex=Local\ScreenCopy.Instance
DefaultDirName={localappdata}\Programs\SelectAnyText
DefaultGroupName=SelectAnyText
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
UninstallDisplayIcon={app}\SelectAnyText.exe
SetupIconFile=..\ScreenCopy.ico
OutputBaseFilename=SelectAnyText-{#AppVersion}-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile={#Payload}\INSTALLATION_LICENSE.txt

[Types]
Name: "standard"; Description: "English"
Name: "full"; Description: "All OCR languages"
Name: "custom"; Description: "Custom OCR languages"; Flags: iscustom

[Files]
Source: "{#Payload}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Payload}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Payload}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#Payload}\SelectAnyText.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Payload}\tessdata\LICENSE"; DestDir: "{app}\tessdata"; Flags: ignoreversion
Source: "{#Payload}\tessdata\MODELS.json"; DestDir: "{app}\tessdata"; Flags: ignoreversion
Source: "{#Payload}\ocr\*"; DestDir: "{app}\ocr"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{userprograms}\SelectAnyText"; Filename: "{app}\SelectAnyText.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "SelectAnyText"; ValueData: """{app}\SelectAnyText.exe"""; Flags: uninsdeletevalue

[Run]
Filename: "{app}\SelectAnyText.exe"; Description: "Start SelectAnyText"; Flags: nowait postinstall skipifsilent

#include Payload + "\OcrLanguages.iss"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if WizardSelectedComponents(False) = '' then
    Result := 'No OCR language selected.';
end;
