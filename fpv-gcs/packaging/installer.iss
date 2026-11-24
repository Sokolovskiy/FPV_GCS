#define AppVersion "0.1.10"
#ifndef PackageDir
  #error PackageDir must point to the prepared runtime folder
#endif
#ifndef ReleaseDir
  #error ReleaseDir must point to the release output folder
#endif

[Setup]
AppId={{8A5DDDC8-667A-4BD4-8380-0A42C99BB25D}
AppName=FPV GCS
AppVersion={#AppVersion}
AppVerName=FPV GCS {#AppVersion}
AppPublisher=FPV GCS
AppPublisherURL=https://github.com/Sokolovskiy/FPV_GCS
AppSupportURL=https://github.com/Sokolovskiy/FPV_GCS/issues
AppUpdatesURL=https://github.com/Sokolovskiy/FPV_GCS/releases/latest
DefaultDirName={localappdata}\Programs\FPV GCS
DefaultGroupName=FPV GCS
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#ReleaseDir}
OutputBaseFilename=FPV-GCS-Setup-{#AppVersion}-win64
SetupIconFile=..\assets\fatum.ico
UninstallDisplayIcon={app}\FPV GCS.exe
WizardStyle=modern
WizardSizePercent=110
Compression=lzma2/fast
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=FPV GCS Windows Installer

[Languages]
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\FPV GCS"; Filename: "{app}\FPV GCS.exe"
Name: "{group}\{cm:UninstallProgram,FPV GCS}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\FPV GCS"; Filename: "{app}\FPV GCS.exe"; Tasks: desktopicon
