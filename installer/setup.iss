; AudioPlaybackConnector2 bootstrapper installer (English-only, minimal clicks).
; Flow: Start page (welcome + version state + repair/uninstall choice)
;       -> Download page (web variant only, native Inno download progress)
;       -> Progress page (built-in installing page + live step output memo)
;       -> Finished page (context-dependent text)
; Per-user (no admin): imports the self-signed cert into
; CurrentUser\TrustedPeople, then registers the MSIX via Add-AppxPackage.
; Modes: Bundle (default, payloads embedded) or Web (/DWEBBOOT=1).
; Build with: installer\build-installer.ps1

#define AppName "AudioPlaybackConnector2"
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "stage"
#endif
#ifdef WEBBOOT
  #define BaseName AppName + "-WebSetup"
#else
  #define BaseName AppName + "-Setup-" + AppVersion
  #ifndef PackageFileName
    #error PackageFileName is required; use installer/build-installer.ps1 to stage and validate the payload.
  #endif
#endif

[Setup]
AppId={{8F3B2E71-5C4A-4E9D-9B1E-1A2B3C4D5E6F}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=N0ahTM
AppPublisherURL=https://github.com/N0ahTM/AudioPlaybackConnector2
AppSupportURL=https://github.com/N0ahTM/AudioPlaybackConnector2/issues
DefaultDirName={localappdata}\{#AppName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible or arm64
ArchitecturesInstallIn64BitMode=x64compatible or arm64
MinVersion=10.0.19041
OutputDir=..\dist\installer
OutputBaseFilename={#BaseName}
SetupIconFile=..\AudioPlaybackConnector2\res\app.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
; The bootstrapper only registers the MSIX and does not install files into {app}.
; Uninstall is offered by this bootstrapper itself and via Windows Settings.
Uninstallable=no
CreateAppDir=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#StageDir}\install-app.ps1"; DestDir: "{tmp}\pkg"; Flags: ignoreversion
Source: "{#StageDir}\AudioPlaybackConnector2.cer"; DestDir: "{tmp}\pkg"; Flags: ignoreversion
#ifndef WEBBOOT
Source: "{#StageDir}\{#PackageFileName}"; DestDir: "{tmp}\pkg"; Flags: ignoreversion
Source: "{#StageDir}\Dependencies\*"; DestDir: "{tmp}\pkg\Dependencies"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
#endif

[Code]
var
  InstallFailed, UninstallMode: Boolean;
  InstalledVersion, LatestVersion: String;
  StartPage: TWizardPage;
  StartTitle, StartText: TNewStaticText;
  ActionPrimary, ActionUninstall, ActionKeep: TRadioButton;
  ProgressMemo: TNewMemo;
#ifdef WEBBOOT
  DownloadsDone: Boolean;
  DownloadPage: TDownloadWizardPage;
#endif

function NextPart(var S: String): Integer;
var
  P: Integer;
  T: String;
begin
  P := Pos('.', S);
  if P = 0 then
  begin
    T := S;
    S := '';
  end
  else
  begin
    T := Copy(S, 1, P - 1);
    Delete(S, 1, P);
  end;
  Result := StrToIntDef(T, 0);
end;

{ 1 when A > B, -1 when A < B, 0 when equal (four numeric components). }
function CompareVersions(A, B: String): Integer;
var
  I, X, Y: Integer;
begin
  for I := 1 to 4 do
  begin
    X := NextPart(A);
    Y := NextPart(B);
    if X > Y then begin Result := 1;  exit; end;
    if X < Y then begin Result := -1; exit; end;
  end;
  Result := 0;
end;

function PsExe: String;
{ Full path: bare "powershell.exe" depends on the caller's PATH, which is
  unreliable when setup is launched from non-standard shells. }
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function NativePackageArchitecture: String;
begin
  if IsArm64 then
    Result := 'arm64'
  else
    Result := 'x64';
end;

procedure RunVersionCheck;
var
  ResultCode: Integer;
  OutFile, Cmd: String;
  Lines: TArrayOfString;
  I: Integer;
begin
  InstalledVersion := '';
  LatestVersion := '';
  OutFile := ExpandConstant('{tmp}\apc2-version.txt');
  { Kept as one -Command so no script file is needed before the install step.
    Emits installed= / latest= (four components, or empty when unknown). }
  Cmd := '-NoProfile -ExecutionPolicy Bypass -Command "' +
    '$p = Get-AppxPackage -Name ''N0ahTM.AudioPlaybackConnector2'' -ErrorAction SilentlyContinue; ' +
    '$installed = ''''; if ($p) { $installed = [string]$p.Version }; ' +
#ifdef WEBBOOT
    '$latest = ''''; ' +
    'try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; ' +
    '$r = Invoke-RestMethod ''https://api.github.com/repos/N0ahTM/AudioPlaybackConnector2/releases/latest'' ' +
    '-Headers @{ ''User-Agent'' = ''apc2-setup'' } -TimeoutSec 15; ' +
    '$latest = ($r.tag_name.TrimStart(''v'')) + ''.0'' } catch {}; ' +
#else
    '$latest = ''{#AppVersion}.0''; ' +
#endif
    'Set-Content -Path ''' + OutFile + ''' -Value "installed=$installed`nlatest=$latest" -Encoding ASCII"';
  if not Exec(PsExe, Cmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    exit;
  if not FileExists(OutFile) then
    exit;
  if not LoadStringsFromFile(OutFile, Lines) then
    exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    if Pos('installed=', Lines[I]) = 1 then
      InstalledVersion := Copy(Lines[I], 11, Length(Lines[I]));
    if Pos('latest=', Lines[I]) = 1 then
      LatestVersion := Copy(Lines[I], 8, Length(Lines[I]));
  end;
end;

procedure UpdateStartButtons;
begin
  if Assigned(ActionKeep) and ActionKeep.Checked then
    WizardForm.NextButton.Caption := '&Close'
  else if Assigned(ActionUninstall) and ActionUninstall.Checked then
    WizardForm.NextButton.Caption := '&Uninstall'
  else
    WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
end;

procedure ActionRadioClick(Sender: TObject);
begin
  UpdateStartButtons;
end;

procedure MakeRadio(var Btn: TRadioButton; const Caption: String; Top: Integer; Checked: Boolean);
begin
  Btn := TRadioButton.Create(StartPage);
  Btn.Parent := StartPage.Surface;
  Btn.Caption := Caption;
  Btn.Left := 0;
  Btn.Top := Top;
  Btn.Width := StartPage.SurfaceWidth;
  Btn.Checked := Checked;
  Btn.OnClick := @ActionRadioClick;
end;

procedure InitializeWizard;
var
  Cmp: Integer;
  Y: Integer;
  Body: String;
begin
  RunVersionCheck;

  StartPage := CreateCustomPage(wpWelcome, 'AudioPlaybackConnector2 Setup',
    'Bluetooth audio playback, one click away');
  StartTitle := TNewStaticText.Create(StartPage);
  StartTitle.Parent := StartPage.Surface;
  StartTitle.Left := 0;
  StartTitle.Top := 0;
  StartTitle.Width := StartPage.SurfaceWidth;
  StartTitle.Font.Size := 12;
  StartTitle.Font.Style := [fsBold];
  StartTitle.AutoSize := True;

  StartText := TNewStaticText.Create(StartPage);
  StartText.Parent := StartPage.Surface;
  StartText.Left := 0;
  StartText.Top := 30;
  StartText.Width := StartPage.SurfaceWidth;
  StartText.Height := 120;
  StartText.WordWrap := True;
  StartText.AutoSize := False;

  { Compose per state. Cmp: <0 update available, 0 same version, >0 downgrade. }
  Cmp := 0;
  if (InstalledVersion <> '') and (LatestVersion <> '') then
    Cmp := CompareVersions(InstalledVersion, LatestVersion);
  Y := 150;

  if InstalledVersion = '' then
  begin
    StartTitle.Caption := 'Install AudioPlaybackConnector2';
    if LatestVersion <> '' then
      Body := 'Version ' + LatestVersion + ' will be installed.'
    else
      Body := 'The latest release will be installed.';
    Body := Body + #13#10#13#10 +
      'No existing installation was found. The app runs in the system tray ' +
      'and connects your Bluetooth audio devices as playback sinks.';
  end
  else if LatestVersion = '' then
  begin
    StartTitle.Caption := 'AudioPlaybackConnector2 ' + InstalledVersion + ' is installed';
    Body := 'The latest version could not be determined (offline?).' + #13#10#13#10 +
      'Choose whether to repair the installation or remove the app.';
    MakeRadio(ActionPrimary, 'Repair / reinstall', Y, True);
    MakeRadio(ActionUninstall, 'Uninstall AudioPlaybackConnector2', Y + 24, False);
  end
  else if Cmp = 0 then
  begin
    StartTitle.Caption := 'AudioPlaybackConnector2 ' + InstalledVersion + ' is already installed';
    Body := 'This is the latest version.' + #13#10#13#10 +
      'Choose whether to repair the installation or remove the app. ' +
      'Your settings are kept in both cases.';
    MakeRadio(ActionPrimary, 'Repair / reinstall version ' + LatestVersion, Y, True);
    MakeRadio(ActionUninstall, 'Uninstall AudioPlaybackConnector2', Y + 24, False);
  end
  else if Cmp < 0 then
  begin
    StartTitle.Caption := 'Update available';
    Body := 'Installed: ' + InstalledVersion + #13#10 +
      'New: ' + LatestVersion + #13#10#13#10 +
      'Updating keeps your settings. You can also remove the app instead.';
    MakeRadio(ActionPrimary, 'Update to version ' + LatestVersion, Y, True);
    MakeRadio(ActionUninstall, 'Uninstall AudioPlaybackConnector2', Y + 24, False);
  end
  else
  begin
    StartTitle.Caption := 'A newer version is already installed';
    Body := 'Installed: ' + InstalledVersion + '  (newer)' + #13#10 +
      'This installer: ' + LatestVersion + #13#10#13#10 +
      'Installing the older version is not recommended. ' +
      'Close setup, or remove the installed version.';
    StartText.Font.Style := [fsBold];
    MakeRadio(ActionKeep, 'Keep version ' + InstalledVersion + ' (close setup)', Y, True);
    MakeRadio(ActionUninstall, 'Uninstall version ' + InstalledVersion, Y + 24, False);
  end;
  StartText.Caption := Body;

#ifdef WEBBOOT
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing),
    SetupMessage(msgPreparingDesc), nil);
#endif

  { Live step output on the built-in installing page. }
  ProgressMemo := TNewMemo.Create(WizardForm);
  ProgressMemo.Parent := WizardForm.InstallingPage;
  ProgressMemo.Left := 0;
  ProgressMemo.Top := WizardForm.ProgressGauge.Top + WizardForm.ProgressGauge.Height + 12;
  ProgressMemo.Width := WizardForm.InstallingPage.ClientWidth;
  ProgressMemo.Height := WizardForm.InstallingPage.ClientHeight - ProgressMemo.Top - 8;
  ProgressMemo.ScrollBars := ssVertical;
  ProgressMemo.ReadOnly := True;
  ProgressMemo.Font.Name := 'Consolas';
  ProgressMemo.Font.Size := 8;
end;

function ResolveDownloadList(const ListFile: String): Boolean;
var
  ResultCode: Integer;
  Arch, Cmd: String;
begin
  Result := False;
  Arch := NativePackageArchitecture;
  Cmd := '-NoProfile -ExecutionPolicy Bypass -Command "' +
    '[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; ' +
    '$r = Invoke-RestMethod ''https://api.github.com/repos/N0ahTM/AudioPlaybackConnector2/releases/latest'' ' +
    '-Headers @{ ''User-Agent'' = ''apc2-setup'' } -TimeoutSec 30; ' +
    '$resolvedVersion = ($r.tag_name.TrimStart(''v'')) + ''.0''; ' +
    'if (''' + LatestVersion + ''' -ne '''' -and $resolvedVersion -ne ''' + LatestVersion + ''') { exit 3 }; ' +
    '$packages = @($r.assets | Where-Object { $_.Name -match ''^AudioPlaybackConnector2_[\d.]+_x64_ARM64\.msixbundle$'' }); ' +
    'if ($packages.Count -ne 1) { exit 2 }; $out = @(); ' +
    '$out += @($packages | ' +
    'ForEach-Object { $_.browser_download_url + ''|'' + $_.Name }); ' +
    '$out += @($r.assets | Where-Object { ($_.Name -match ''\.(msix|appx)$'') -and ' +
    '($_.Name -notmatch ''^AudioPlaybackConnector2_'') -and ' +
    '(($_.Name -match ''\.' + Arch + '\.'') -or ($_.Name -notmatch ''\.(x64|arm64)\.'')) } | ' +
    'ForEach-Object { $_.browser_download_url + ''|'' + $_.Name }); ' +
    '[IO.File]::WriteAllLines(''' + ListFile + ''', $out)"';
  if Exec(PsExe, Cmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and
     (ResultCode = 0) and FileExists(ListFile) then
    Result := True;
end;

#ifdef WEBBOOT
function PerformDownloads: Boolean;
var
  ListFile: String;
  Lines: TArrayOfString;
  I, P: Integer;
begin
  Result := False;
  ListFile := ExpandConstant('{tmp}\apc2-dl.txt');
  if not ResolveDownloadList(ListFile) then
  begin
    SuppressibleMsgBox('Could not resolve the latest release assets. Check your internet connection.',
      mbError, MB_OK, IDOK);
    exit;
  end;
  if not LoadStringsFromFile(ListFile, Lines) then
    exit;
  ForceDirectories(ExpandConstant('{tmp}\pkg'));
  DownloadPage.Clear;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    P := Pos('|', Lines[I]);
    if P > 1 then
      DownloadPage.Add(Copy(Lines[I], 1, P - 1), 'pkg\' + Copy(Lines[I], P + 1, Length(Lines[I])), '');
  end;
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      DownloadsDone := True;
      Result := True;
    except
      if DownloadPage.AbortedByUser then
        Log('Download aborted by user')
      else
        SuppressibleMsgBox('Download failed: ' + GetExceptionMessage, mbError, MB_OK, IDOK);
    end;
  finally
    DownloadPage.Hide;
  end;
end;
#endif

procedure AppendLogToMemo(const FileName: String);
var
  Lines: TArrayOfString;
  I: Integer;
begin
  if not LoadStringsFromFile(FileName, Lines) then
    exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
    ProgressMemo.Lines.Add(Lines[I]);
  ProgressMemo.Lines.Add('');
end;

procedure RunStep(const StepName, Desc: String);
var
  ResultCode: Integer;
  OutFile, Params: String;
begin
  if InstallFailed then
    exit;
  WizardForm.StatusLabel.Caption := Desc;
  OutFile := ExpandConstant('{tmp}\apc2-step-' + StepName + '.log');
  { cmd /c so console output lands in a file the memo can display. }
  Params := '/c ""' + PsExe + '" -NoProfile -ExecutionPolicy Bypass -File "' +
            ExpandConstant('{tmp}\pkg\install-app.ps1') + '" -Step ' + StepName +
            ' -PackageArchitecture ' + NativePackageArchitecture;
  Params := Params + ' -PackageDir "' + ExpandConstant('{tmp}\pkg') + '"';
  if (StepName = 'validate') and (LatestVersion <> '') then
    Params := Params + ' -ExpectedPackageVersion "' + LatestVersion + '"';
  if (StepName = 'verify') and (not WizardSilent) then
    Params := Params + ' -Launch';
  Params := Params + ' > "' + OutFile + '" 2>&1"';
  if not Exec(ExpandConstant('{cmd}'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    ResultCode := -1;
  AppendLogToMemo(OutFile);
  WizardForm.ProgressGauge.Position := WizardForm.ProgressGauge.Position + 25;
  if ResultCode <> 0 then
  begin
    InstallFailed := True;
    Log('Step ' + StepName + ' failed with exit code ' + IntToStr(ResultCode));
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  if InstallFailed then
    Result := 1
  else
    Result := 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    WizardForm.ProgressGauge.Position := 0;
    if UninstallMode then
    begin
      RunStep('uninstall', 'Removing AudioPlaybackConnector2 ...');
      WizardForm.ProgressGauge.Position := 100;
    end
    else
    begin
      RunStep('validate', 'Validating the downloaded package ...');
      RunStep('cert', 'Trusting the signing certificate ...');
      RunStep('install', 'Installing the app package ...');
      RunStep('verify', 'Verifying and launching ...');
      if not InstallFailed then
        WizardForm.ProgressGauge.Position := 100;
    end;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (StartPage <> nil) and (CurPageID = StartPage.ID) then
  begin
    { Silent mode must never auto-close on downgrade; the PowerShell-side
      0x80073D06 guard already leaves a newer installed version untouched. }
    if not WizardSilent then
    begin
      if Assigned(ActionKeep) and ActionKeep.Checked then
      begin
        WizardForm.Close;
        Result := False;
        exit;
      end;
      if Assigned(ActionUninstall) and ActionUninstall.Checked then
      begin
        if MsgBox('Remove AudioPlaybackConnector2 from this PC?' + #13#10 +
                  'Your settings under %LOCALAPPDATA% are kept.',
                  mbConfirmation, MB_YESNO) <> IDYES then
        begin
          Result := False;
          exit;
        end;
        UninstallMode := True;
        exit;
      end;
    end;
#ifdef WEBBOOT
    if not DownloadsDone then
      Result := PerformDownloads;
#endif
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (StartPage <> nil) and (CurPageID = StartPage.ID) then
    UpdateStartButtons;
  if CurPageID = wpFinished then
  begin
    if InstallFailed then
    begin
      WizardForm.FinishedHeadingLabel.Caption := 'Installation failed';
      WizardForm.FinishedLabel.Caption :=
        'AudioPlaybackConnector2 could not be installed automatically.' + #13#10#13#10 +
        'Details: ' + ExpandConstant('{localappdata}\AudioPlaybackConnector2\install.log') + #13#10 +
        'Manual fallback: import AudioPlaybackConnector2.cer into "Trusted People" ' +
        'and open the .appinstaller file from the release page.';
    end
    else if UninstallMode then
    begin
      WizardForm.FinishedHeadingLabel.Caption := 'AudioPlaybackConnector2 was uninstalled';
      WizardForm.FinishedLabel.Caption :=
        'The app and its certificate were removed.' + #13#10 +
        'Your settings under %LOCALAPPDATA% were kept.' + #13#10#13#10 +
        'Thanks for trying AudioPlaybackConnector2.';
    end
    else
    begin
      WizardForm.FinishedLabel.Caption :=
        'AudioPlaybackConnector2 was installed and launched.' + #13#10#13#10 +
        'Look for the speaker icon in the system tray (near the clock). ' +
        'Left-click it to pick a Bluetooth audio device, right-click for options.';
    end;
  end;
end;
