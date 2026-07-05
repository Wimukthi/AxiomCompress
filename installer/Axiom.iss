; Inno Setup script for Axiom.
; The build wrapper passes AppVersion from the executable resource so installer
; names and Windows uninstall metadata stay aligned with the binary.

#ifndef AppVersion
#define AppVersion "0.1.0.0"
#endif

#ifndef Configuration
#define Configuration "Release"
#endif

#ifndef Platform
#define Platform "x64"
#endif

#define AppName "Axiom"
#define AppPublisher "Wimukthi Bandara"
#define SourceRoot ".."

[Setup]
AppId={{2D355952-C061-4E0D-9874-A693F3959959}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\Axiom
DefaultGroupName=Axiom
DisableProgramGroupPage=yes
LicenseFile={#SourceRoot}\LICENSE
OutputDir=output
OutputBaseFilename=AxiomSetup-{#AppVersion}-win-x64
SetupIconFile={#SourceRoot}\Icons\axiom.ico
UninstallDisplayIcon={app}\Axiom.exe
UninstallDisplayName=Axiom
VersionInfoVersion={#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern dynamic
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UsePreviousAppDir=yes
CloseApplications=yes
RestartApplications=no
ChangesAssociations=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#SourceRoot}\out\{#Configuration}\Axiom.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\out\{#Configuration}\axiomc.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\out\{#Configuration}\backends\7zip\*"; DestDir: "{app}\backends\7zip"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceRoot}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\CLI_GUIDE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\ARCHITECTURE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\FORMAT.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Axiom"; Filename: "{app}\Axiom.exe"; WorkingDir: "{app}"
Name: "{group}\Axiom CLI"; Filename: "{app}\axiomc.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Axiom"; Filename: "{app}\Axiom.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\Axiom.exe"; ValueType: string; ValueName: ""; ValueData: "{app}\Axiom.exe"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\Axiom.exe"; ValueType: string; ValueName: "Path"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\axiomc.exe"; ValueType: string; ValueName: ""; ValueData: "{app}\axiomc.exe"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\axiomc.exe"; ValueType: string; ValueName: "Path"; ValueData: "{app}"; Flags: uninsdeletekey

[Run]
Filename: "{app}\Axiom.exe"; Description: "Launch Axiom"; Flags: nowait postinstall skipifsilent

[Code]
var
  MaintenancePage: TInputOptionWizardPage;
  InstalledVersion: String;
  InstalledUninstallString: String;
  InstalledVersionIsNewer: Boolean;

function QueryInstalledValue(Name: String; var Value: String): Boolean;
begin
  Result := RegQueryStringValue(HKLM64, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{2D355952-C061-4E0D-9874-A693F3959959}_is1', Name, Value);
  if not Result then
    Result := RegQueryStringValue(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{2D355952-C061-4E0D-9874-A693F3959959}_is1', Name, Value);
end;

function VersionPart(Value: String; WantedPart: Integer): Integer;
var
  I: Integer;
  PartIndex: Integer;
  PartText: String;
  Ch: String;
begin
  Result := 0;
  PartIndex := 0;
  PartText := '';
  for I := 1 to Length(Value) + 1 do
  begin
    if I <= Length(Value) then
      Ch := Copy(Value, I, 1)
    else
      Ch := '.';

    if Ch = '.' then
    begin
      if PartIndex = WantedPart then
      begin
        Result := StrToIntDef(PartText, 0);
        Exit;
      end;
      PartText := '';
      PartIndex := PartIndex + 1;
    end
    else
      PartText := PartText + Ch;
  end;
end;

function CompareVersions(Left: String; Right: String): Integer;
var
  I: Integer;
  LeftPart: Integer;
  RightPart: Integer;
begin
  Result := 0;
  for I := 0 to 3 do
  begin
    LeftPart := VersionPart(Left, I);
    RightPart := VersionPart(Right, I);
    if LeftPart < RightPart then
    begin
      Result := -1;
      Exit;
    end;
    if LeftPart > RightPart then
    begin
      Result := 1;
      Exit;
    end;
  end;
end;

function SplitCommandLine(CommandLine: String; var FileName: String; var Parameters: String): Boolean;
var
  I: Integer;
  EndQuote: Integer;
begin
  Result := False;
  FileName := '';
  Parameters := '';
  CommandLine := Trim(CommandLine);
  if CommandLine = '' then
    Exit;

  if Copy(CommandLine, 1, 1) = '"' then
  begin
    EndQuote := 0;
    for I := 2 to Length(CommandLine) do
    begin
      if Copy(CommandLine, I, 1) = '"' then
      begin
        EndQuote := I;
        Break;
      end;
    end;
    if EndQuote = 0 then
      Exit;

    FileName := Copy(CommandLine, 2, EndQuote - 2);
    Parameters := Trim(Copy(CommandLine, EndQuote + 1, Length(CommandLine) - EndQuote));
  end
  else
  begin
    I := Pos(' ', CommandLine);
    if I = 0 then
      FileName := CommandLine
    else
    begin
      FileName := Copy(CommandLine, 1, I - 1);
      Parameters := Trim(Copy(CommandLine, I + 1, Length(CommandLine) - I));
    end;
  end;

  Result := FileName <> '';
end;

procedure InitializeWizard;
var
  Comparison: Integer;
begin
  if not QueryInstalledValue('DisplayVersion', InstalledVersion) then
    Exit;

  QueryInstalledValue('UninstallString', InstalledUninstallString);
  Comparison := CompareVersions(InstalledVersion, '{#AppVersion}');
  InstalledVersionIsNewer := Comparison > 0;

  MaintenancePage := CreateInputOptionPage(
    wpWelcome,
    'Existing Axiom installation',
    'Axiom is already installed.',
    'Choose how Setup should handle the existing Axiom installation.',
    True,
    False);

  if InstalledVersionIsNewer then
    MaintenancePage.Add('Keep installed Axiom ' + InstalledVersion + ' (newer than this package)')
  else if Comparison < 0 then
    MaintenancePage.Add('Update Axiom from ' + InstalledVersion + ' to {#AppVersion}')
  else
    MaintenancePage.Add('Repair/reinstall Axiom {#AppVersion}');

  MaintenancePage.Add('Remove Axiom');
  MaintenancePage.Values[0] := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Uninstaller: String;
  Parameters: String;
  ResultCode: Integer;
begin
  Result := True;
  if (MaintenancePage = nil) or (CurPageID <> MaintenancePage.ID) then
    Exit;

  if MaintenancePage.Values[0] then
  begin
    if InstalledVersionIsNewer then
    begin
      MsgBox('A newer Axiom version is already installed. Cancel Setup or choose Remove Axiom.', mbInformation, MB_OK);
      Result := False;
    end;
    Exit;
  end;

  if not SplitCommandLine(InstalledUninstallString, Uninstaller, Parameters) then
  begin
    MsgBox('Axiom is installed, but Setup could not find its uninstaller.', mbError, MB_OK);
    Result := False;
    Exit;
  end;

  if MsgBox('Remove the existing Axiom installation now?', mbConfirmation, MB_YESNO) = IDYES then
  begin
    Exec(Uninstaller, Parameters, '', SW_SHOW, ewNoWait, ResultCode);
    WizardForm.Close;
  end;
  Result := False;
end;
