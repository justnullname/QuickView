; QuickView Inno Setup Script
; Version: 1.1.0
; Author: justnullname

#define MyAppName "QuickView"
#ifndef MyAppVersion
  ; Try to extract version from the compiled executable
  #if FileExists("..\Release\" + AppArch + "\QuickView.exe")
    #define MyAppVersion GetFileVersion("..\Release\" + AppArch + "\QuickView.exe")
  #else
    ; Fallback version if the executable is missing (GitHub Actions will override this via CLI)
    #define MyAppVersion "2.1.0"
  #endif
#endif
#define MyAppPublisher "justnullname"
#define MyAppURL "https://github.com/justnullname/QuickView"
#define MyAppExeName "QuickView.exe"

; Architectures: x64, arm64
#ifndef AppArch
  #define AppArch "x64"
#endif

[Setup]
AppId={{D175135F-5C7A-4E7B-B41E-C638E75C421A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=..\LICENSE
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\Release\installer
#ifdef OutputName
  OutputBaseFilename={#OutputName}
#else
  OutputBaseFilename=QuickView_Installer_{#AppArch}
#endif
SetupIconFile=..\QuickView.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern

#if AppArch == "x64"
  ArchitecturesAllowed=x64
  ArchitecturesInstallIn64BitMode=x64
#elif AppArch == "arm64"
  ArchitecturesAllowed=arm64
  ArchitecturesInstallIn64BitMode=arm64
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Assume build artifacts are organized in Release\<Arch>\ directory
Source: "..\Release\{#AppArch}\QuickView.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README_zh-CN.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\config.json"
