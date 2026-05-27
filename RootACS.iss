[Setup]
AppId={{8F8B8D78-6F54-4E0E-9D4E-6A676F74414353}
AppName=RootACS
AppVersion=1.0.0
AppVerName=RootACS 1.0.0
AppPublisher=akimaru666(https://github.com/HZrate)
AppPublisherURL=https://github.com/HZrate
AppUpdatesURL=https://github.com/HZrate/RootACS
DefaultDirName={autopf64}\RootACS
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableDirPage=yes
DisableProgramGroupPage=yes
Uninstallable=no
CreateAppDir=yes
OutputDir=.
OutputBaseFilename=RootACS-Setup-1.0.0
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
Source: "setup.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "uninstall.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "RootACS-Service.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "rootacs.exe"; DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{app}\setup.bat"; Flags: waituntilterminated runhidden