#define AppName "%%APP_NAME%%"
#define AppNameOneWord "%%APP_NAME_ONEWORD%%"
#define FriendlyAppName "%%FRIENDLY_APP_NAME%%"
#define AppVersion "%%VERSION%%"
#define ViewerExe "%%FINAL_EXE%%"
#define IsAVX2 %%IS_AVX2%%
#define IsOpenSim %%IS_OPENSIM%%
#define DownloadURL "%%DOWNLOAD_URL%%"

[Setup]
AppId=VulkanStorm.{#AppNameOneWord}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=The Phoenix Firestorm Project, Inc.
AppPublisherURL=https://www.firestormviewer.org
AppSupportURL=https://www.firestormviewer.org/support
AppUpdatesURL=https://www.firestormviewer.org/downloads
DefaultDirName={autopf64}\{#AppNameOneWord}
DefaultGroupName={#AppName}
DisableProgramGroupPage=auto
LicenseFile=%%OUTPUT_DIR%%\VivoxAUP.txt
OutputDir=%%OUTPUT_DIR%%
OutputBaseFilename=%%INSTALLER_BASENAME%%
SetupIconFile=%%SOURCE%%\installers\windows\firestorm_icon%%ICON_SUFFIX%%.ico
UninstallDisplayIcon={app}\{#ViewerExe}
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=6.1sp1
CloseApplications=yes
CloseApplicationsFilter={#ViewerExe}
RestartApplications=no
ChangesAssociations=yes
VersionInfoVersion={#AppVersion}
VersionInfoCompany=The Phoenix Firestorm Project, Inc.
VersionInfoDescription={#AppName} installer
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
WizardStyle=modern

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "pl"; MessagesFile: "compiler:Languages\Polish.isl"
Name: "pt"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "protocols"; Description: "Register Second Life and grid URL protocols"; Flags: checkedonce

[Files]
%%INSTALL_FILES%%

[InstallDelete]
Type: filesandordirs; Name: "{app}\*"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#ViewerExe}"; Parameters: "--set InstallLanguage {language}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#ViewerExe}"; Parameters: "--set InstallLanguage {language}"; Tasks: desktopicon
Name: "{app}\{#AppName}"; Filename: "{app}\{#ViewerExe}"; Parameters: "--set InstallLanguage {language}"

[Registry]
Root: HKLM; Subkey: "SOFTWARE\The Phoenix Firestorm Project\{#AppNameOneWord}"; ValueType: string; ValueName: ""; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\The Phoenix Firestorm Project\{#AppNameOneWord}"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"
Root: HKLM; Subkey: "SOFTWARE\The Phoenix Firestorm Project\{#AppNameOneWord}"; ValueType: string; ValueName: "Shortcut"; ValueData: "{#AppName}"
Root: HKLM; Subkey: "SOFTWARE\The Phoenix Firestorm Project\{#AppNameOneWord}"; ValueType: string; ValueName: "Exe"; ValueData: "{#ViewerExe}"
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\{#ViewerExe}"; ValueType: dword; ValueName: "DisableExceptionChainValidation"; ValueData: "1"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Applications\{#ViewerExe}"; ValueType: string; ValueName: "IsHostApp"; ValueData: ""; Flags: uninsdeletekey
Root: HKCR; Subkey: "secondlife"; ValueType: string; ValueName: ""; ValueData: "URL:Second Life"; Tasks: protocols; Flags: uninsdeletekey
Root: HKCR; Subkey: "secondlife"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Tasks: protocols
Root: HKCR; Subkey: "secondlife\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#ViewerExe}"; Tasks: protocols
Root: HKCR; Subkey: "secondlife\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#FriendlyAppName}"; Tasks: protocols
Root: HKCR; Subkey: "secondlife\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#ViewerExe}"" -url ""%1"""; Tasks: protocols
Root: HKCR; Subkey: "x-grid-location-info"; ValueType: string; ValueName: ""; ValueData: "URL:Hypergrid"; Tasks: protocols; Flags: uninsdeletekey
Root: HKCR; Subkey: "x-grid-location-info"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Tasks: protocols
Root: HKCR; Subkey: "x-grid-location-info\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#ViewerExe}"" -url ""%1"""; Tasks: protocols
Root: HKCR; Subkey: "x-grid-info"; ValueType: string; ValueName: ""; ValueData: "URL:Hypergrid"; Tasks: protocols; Flags: uninsdeletekey
Root: HKCR; Subkey: "x-grid-info"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Tasks: protocols
Root: HKCR; Subkey: "x-grid-info\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#ViewerExe}"" -url ""%1"""; Tasks: protocols
Root: HKCR; Subkey: "hop"; ValueType: string; ValueName: ""; ValueData: "URL:Hypergrid"; Tasks: protocols; Check: IsOpenSimBuild; Flags: uninsdeletekey
Root: HKCR; Subkey: "hop"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Tasks: protocols; Check: IsOpenSimBuild
Root: HKCR; Subkey: "hop\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#ViewerExe}"" -url ""%1"""; Tasks: protocols; Check: IsOpenSimBuild

[Code]
function IsProcessorFeaturePresent(ProcessorFeature: Cardinal): Boolean;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';

function IsOpenSimBuild: Boolean;
begin
  Result := ({#IsOpenSim} <> 0);
end;

function InitializeSetup: Boolean;
var
  Choice: Integer;
  ExecResult: Integer;
begin
  Result := False;
  if not IsProcessorFeaturePresent(10) then
  begin
    MsgBox('This viewer requires a processor with SSE2 support.', mbCriticalError, MB_OK);
    Exit;
  end;

  if ({#IsAVX2} <> 0) and not IsProcessorFeaturePresent(40) then
  begin
    Choice := MsgBox('This AVX2 build is not supported by this processor. Open the legacy CPU download page?', mbCriticalError, MB_YESNO);
    if Choice = IDYES then
      ShellExec('', '{#DownloadURL}-legacy-cpus', '', '', SW_SHOWNORMAL, ewNoWait, ExecResult);
    Exit;
  end;

  if ({#IsAVX2} = 0) and IsProcessorFeaturePresent(40) and not WizardSilent then
  begin
    Choice := MsgBox('This processor supports the faster AVX2 build. Open the AVX2 download page instead?', mbConfirmation, MB_YESNO);
    if Choice = IDYES then
    begin
      ShellExec('', '{#DownloadURL}', '', '', SW_SHOWNORMAL, ewNoWait, ExecResult);
      Exit;
    end;
  end;

  Result := True;
end;