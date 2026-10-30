; Per-user testing installer. Signing and installed-app updater delivery are Phase 5.
#ifndef StageDir
  #error StageDir is required
#endif
[Setup]
AppId={{CEF8DB34-17D5-4280-BE44-C543EA69C427}
AppName=Magnite
AppVersion={#ReleaseVersion}
AppVerName=Magnite {#ReleaseVersion}
VersionInfoVersion={#NumericVersion}
VersionInfoTextVersion={#ReleaseVersion}
DefaultDirName={localappdata}\Programs\Magnite
DefaultGroupName=Magnite
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=Magnite-{#ReleaseVersion}-windows-x86_64-setup
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\magnite-app.exe
LicenseFile={#StageDir}\licenses\LICENSE

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Magnite"; Filename: "{app}\magnite-app.exe"

[Run]
Filename: "{app}\magnite-app.exe"; Description: "Launch Magnite"; Flags: nowait postinstall skipifsilent
