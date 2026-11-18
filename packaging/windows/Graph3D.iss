#define AppName "Graph3D"
#define RepositoryUrl "https://github.com/ketchup2031/Graph3D"

[Setup]
AppId={{C12F9DBD-5A15-4307-8CAC-A96F9AF49186}
AppName={#AppName}
AppVersion={#Version}
AppPublisher=Graph3D
AppPublisherURL={#RepositoryUrl}
AppUpdatesURL={#RepositoryUrl}/releases
DefaultDirName={localappdata}\Programs\Graph3D
DefaultGroupName=Graph3D
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
SetupIconFile=..\..\resources\icons\graph3d.ico
UninstallDisplayIcon={app}\Graph3D.exe
OutputDir={#OutputFolder}
OutputBaseFilename=Graph3D-{#Version}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
VersionInfoVersion={#Version}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "{#PayloadFolder}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\Graph3D\Graph3D"; Filename: "{app}\Graph3D.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\Graph3D"; Filename: "{app}\Graph3D.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Graph3D.exe"; Description: "{cm:LaunchProgram,Graph3D}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
