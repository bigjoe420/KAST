; KAST-Setup — one self-contained Windows installer for the whole KAST PC side.
; Approved direction 2026-10-31: per-user install; end users need only OBS,
; the box, and a USB cable. Built by installer\build_installer.ps1, which
; stages installer\payload\ and passes /DAppVersion.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#define AppName "KAST"
#define AppPublisher "KAST"
#define PayloadDir "payload"

[Setup]
AppId={{35440a2a-9389-41e8-9bd4-879a209df531}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\KAST
DefaultGroupName=KAST
PrivilegesRequired=lowest
OutputDir=dist
OutputBaseFilename=KAST-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\companion\kast.ico
UninstallDisplayIcon={app}\companion\kast.ico
; Never touch anything outside our own install folder.
DirExistsWarning=no
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &Desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "autostart"; Description: "&Start KAST Companion when I sign in"; GroupDescription: "Startup:"

[Files]
Source: "stop_kast.ps1"; Flags: dontcopy
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\KAST Companion"; Filename: "{app}\companion\KAST Companion.cmd"; IconFilename: "{app}\companion\kast.ico"; Comment: "Run the KAST relay in the system tray"
Name: "{group}\KAST first-time setup"; Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -STA -File ""{app}\companion\first_run.ps1"""; IconFilename: "{app}\companion\kast.ico"; Comment: "Flash the box and set up OBS"
Name: "{autodesktop}\KAST Companion"; Filename: "{app}\companion\KAST Companion.cmd"; IconFilename: "{app}\companion\kast.ico"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "KAST Companion"; ValueType: string; ValueData: """{app}\companion\KAST Companion.cmd"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -STA -File ""{app}\companion\first_run.ps1"""; Description: "Run KAST first-time setup (flash the box, set up OBS)"; Flags: postinstall nowait unchecked

[Code]
{ Before overwriting files, stop any running KAST tray/relay from THIS
  install folder, otherwise node.exe and the relay files are locked. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  PS: String;
begin
  ExtractTemporaryFile('stop_kast.ps1');
  PS := ExpandConstant('{tmp}\stop_kast.ps1');
  Log('running stop helper: ' + PS);
  if Exec('powershell.exe',
       '-NoProfile -ExecutionPolicy Bypass -File "' + PS + '" "' + ExpandConstant('{app}') + '"',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Log('stop helper exit code: ' + IntToStr(ResultCode))
  else
    Log('stop helper failed to start');
  Result := '';
end;
