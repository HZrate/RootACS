[Setup]
AppId={{8F8B8D78-6F54-4E0E-9D4E-6A676F74414353}
AppName=RootACS
AppVersion=1.0.0
AppVerName=RootACS 1.0.0
AppPublisher=CHANGE_ME
AppPublisherURL=CHANGE_ME
AppSupportURL=CHANGE_ME
AppUpdatesURL=CHANGE_ME
DefaultDirName={tmp}\RootACS
DisableDirPage=yes
DisableProgramGroupPage=yes
Uninstallable=no
CreateAppDir=no
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
Source: "setup.bat"; DestDir: "{tmp}\RootACS"; Flags: deleteafterinstall ignoreversion
Source: "uninstall.bat"; DestDir: "{tmp}\RootACS"; Flags: deleteafterinstall ignoreversion
Source: "release\*"; DestDir: "{tmp}\RootACS\release"; Flags: deleteafterinstall ignoreversion recursesubdirs createallsubdirs

[Run]
Filename: "{tmp}\RootACS\setup.bat"; Flags: waituntilterminated runhidden
