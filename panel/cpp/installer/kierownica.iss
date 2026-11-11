; Instalator panelu kierownicy (Inno Setup 6).
;
;   wydanie.bat                       buduje program, testuje i robi instalator
;   ISCC.exe installer\kierownica.iss sam instalator z gotowego bin\Kierownica.exe
;                                     (sterowniki: najpierw pobierz_sterowniki.ps1)
;
; Wersja jest czytana z zasobu VERSIONINFO w Kierownica.exe (src/version.h),
; wiec przy wydaniu zmienia sie ja tylko w jednym miejscu.
;
; Domyslnie instalacja dla biezacego uzytkownika (bez okna administratora),
; w oknie startowym mozna wybrac instalacje dla wszystkich. Sterowniki
; (ViGEmBus, vJoy) sa wbudowane i instaluja sie po jednej zgodzie UAC.
;
; Aktualizacja z panelu: Kierownica-X.Y-instalator.exe /SILENT /UPDATE=1
; (albo /UPDATE=tray - panel byl schowany w zasobniku). Instalator czeka, az
; panel sie zamknie, a na koniec uruchamia go ponownie.

#define ExeSource "..\bin\Kierownica.exe"
#if !FileExists(ExeSource)
  #error "Brak bin\Kierownica.exe - najpierw uruchom build.bat"
#endif
#define VerMajor
#define VerMinor
#define VerRev
#define VerBuild
#expr GetVersionComponents(ExeSource, VerMajor, VerMinor, VerRev, VerBuild)
#define AppVersion Str(VerMajor) + "." + Str(VerMinor) + (VerRev > 0 ? "." + Str(VerRev) : "")

; sterowniki - te same nazwy co w pobierz_sterowniki.ps1
#define VigemSetup "ViGEmBus_1.22.0_x64_x86_arm64.exe"
#define VjoySetup "vJoySetup_v2.2.2.0_Win10_Win11.exe"
#if !FileExists("deps\" + VigemSetup) || !FileExists("deps\" + VjoySetup)
  #error "Brak sterownikow w installer\deps - uruchom installer\pobierz_sterowniki.ps1"
#endif

[Setup]
AppId={{7C1E2F44-3B8D-4E2A-9A61-5D0F7B3C2E19}
AppName=Panel kierownicy
AppVersion={#AppVersion}
AppVerName=Panel kierownicy {#AppVersion}
AppPublisher=Kierownica DIY
AppPublisherURL=https://github.com/rybainformatyk/kierownica
AppUpdatesURL=https://github.com/rybainformatyk/kierownica/releases
VersionInfoVersion={#VerMajor}.{#VerMinor}.{#VerRev}.0
VersionInfoDescription=Instalator panelu kierownicy
DefaultDirName={autopf}\Kierownica
DefaultGroupName=Panel kierownicy
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
SetupIconFile=..\src\res\kierownica.ico
WizardSmallImageFile=..\src\res\kierownica_256.png
UninstallDisplayIcon={app}\Kierownica.exe
UninstallDisplayName=Panel kierownicy
LicenseFile=..\..\..\LICENSE
OutputDir=..\dist
OutputBaseFilename=Kierownica-{#AppVersion}-instalator
Compression=lzma2/ultra
SolidCompression=yes
; Eksplorator od razu pokazuje nowa ikone programu (bez starej z pamieci podrecznej)
ChangesAssociations=yes
; dzialajacy panel trzyma port COM - sprawdzamy go sami w InitializeSetup
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "pl"; MessagesFile: "compiler:Languages\Polish.isl"

[CustomMessages]
pl.TaskAutostart=Uruchamiaj panel razem z Windowsem (w tle, ikona obok zegara)
pl.TaskDesktop=Skrót na pulpicie
pl.GroupOther=Dodatkowo:
pl.GroupDrivers=Sterowniki (wymagają zgody administratora):
pl.TaskVigem=ViGEmBus 1.22 - tryb pada Xbox 360
pl.TaskVjoy=vJoy 2.2.2 - tryby kierownicy vJoy (Force Feedback w symulatorach)
pl.RunPanel=Uruchom panel kierownicy
pl.DriversStatus=Instalowanie sterowników (ViGEmBus, vJoy)...
pl.DriversFailed=Sterowniki nie zostały zainstalowane (brak zgody administratora?).%n%nTryb G29 działa bez nich. Pozostałe możesz doinstalować, uruchamiając instalator ponownie.
pl.DriversPartly=Nie wszystkie sterowniki zainstalowały się poprawnie (%1).%n%nSzczegóły: zakładka Diagnostyka w panelu.
pl.PanelRunning=Panel kierownicy jest uruchomiony.%n%nZamknij go (prawy przycisk na ikonie obok zegara → Zakończ) i kliknij OK.

[Tasks]
Name: "vigem"; Description: "{cm:TaskVigem}"; GroupDescription: "{cm:GroupDrivers}"; Check: not ViGEmInstalled
Name: "vjoy"; Description: "{cm:TaskVjoy}"; GroupDescription: "{cm:GroupDrivers}"; Check: not VJoyInstalled
Name: "autostart"; Description: "{cm:TaskAutostart}"; GroupDescription: "{cm:GroupOther}"
Name: "desktopicon"; Description: "{cm:TaskDesktop}"; GroupDescription: "{cm:GroupOther}"; Flags: unchecked

[Files]
Source: "{#ExeSource}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "CZYTAJ.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
; instalatory sterownikow - tylko na czas instalacji
Source: "deps\{#VigemSetup}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Tasks: vigem
Source: "deps\{#VjoySetup}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Tasks: vjoy

[Icons]
Name: "{group}\Panel kierownicy"; Filename: "{app}\Kierownica.exe"
Name: "{group}\Odinstaluj panel kierownicy"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Panel kierownicy"; Filename: "{app}\Kierownica.exe"; Tasks: desktopicon

[Registry]
; ten sam wpis, ktory ustawia przelacznik w zakladce Diagnostyka
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "KierownicaPanel"; ValueData: """{app}\Kierownica.exe"" --minimized"; Tasks: autostart

[Run]
; aktualizacja z panelu (tryb cichy) - panel wraca sam
Filename: "{app}\Kierownica.exe"; Parameters: "{code:RestartArgs}"; Flags: nowait runasoriginaluser; Check: IsUpdate
Filename: "{app}\Kierownica.exe"; Description: "{cm:RunPanel}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallDelete]
; ViGEmClient.dll i firmware rozpakowane przez program, pobrane aktualizacje
Type: filesandordirs; Name: "{localappdata}\Kierownica"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'KierownicaPanel';
  PanelMutex = 'Local\KierownicaPanel';

var
  DriversNeedRestart: Boolean;

function IsUpdate(): Boolean;
begin
  Result := ExpandConstant('{param:UPDATE|}') <> '';
end;

function RestartArgs(Param: String): String;
begin
  Result := '--po-aktualizacji';
  if ExpandConstant('{param:UPDATE|}') = 'tray' then
    Result := Result + ' --minimized';
end;

function ViGEmInstalled(): Boolean;
begin
  Result := RegKeyExists(HKLM64, 'SYSTEM\CurrentControlSet\Services\ViGEmBus');
end;

function VJoyInstalled(): Boolean;
begin
  Result := RegKeyExists(HKLM64, 'SYSTEM\CurrentControlSet\Services\vjoy');
end;

// Panel trzyma port COM i plik .exe. Przy aktualizacji z panelu zamyka sie
// sam chwile po uruchomieniu instalatora - wtedy czekamy do 15 s.
function InitializeSetup(): Boolean;
var
  I: Integer;
begin
  Result := True;
  if IsUpdate() then begin
    I := 0;
    while CheckForMutexes(PanelMutex) and (I < 60) do begin
      Sleep(250);
      I := I + 1;
    end;
  end;
  while CheckForMutexes(PanelMutex) do begin
    if SuppressibleMsgBox(CustomMessage('PanelRunning'), mbError, MB_OKCANCEL, IDCANCEL) = IDCANCEL then begin
      Result := False;
      Exit;
    end;
  end;
end;

// Oba sterowniki i konfiguracja vJoy w jednym skrypcie uruchamianym jako
// administrator - jedna zgoda UAC zamiast trzech.
procedure InstallDrivers();
var
  Script, Log, Line: String;
  Lines: TArrayOfString;
  Code, I: Integer;
  Failed: String;
begin
  // aktualizacja z panelu nie doklada sterownikow bez pytania
  if IsUpdate() or not (WizardIsTaskSelected('vigem') or WizardIsTaskSelected('vjoy')) then Exit;
  Script := ExpandConstant('{tmp}\sterowniki.cmd');
  Log := ExpandConstant('{tmp}\sterowniki.log');
  // ">>plik echo" zamiast "echo ... 0>>plik" - inaczej cmd czyta "0>>" jako
  // przekierowanie strumienia 0; %errorlevel% poza nawiasami (rozwija sie od razu)
  SaveStringToFile(Script, '@echo off' + #13#10, False);
  if WizardIsTaskSelected('vigem') then
    SaveStringToFile(Script,
      '"' + ExpandConstant('{tmp}\{#VigemSetup}') + '" /exenoui /qn /norestart' + #13#10 +
      '>>"' + Log + '" echo ViGEmBus %errorlevel%' + #13#10, True);
  if WizardIsTaskSelected('vjoy') then
    SaveStringToFile(Script,
      '"' + ExpandConstant('{tmp}\{#VjoySetup}') + '" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-' + #13#10 +
      '>>"' + Log + '" echo vJoy %errorlevel%' + #13#10 +
      // urzadzenie 1 tak, jak go uzywa panel: 6 osi, 32 przyciski, POV, wszystkie efekty FFB
      'set "VJC=%ProgramFiles%\vJoy\x64\vJoyConfig.exe"' + #13#10 +
      'if not exist "%VJC%" goto koniec' + #13#10 +
      '"%VJC%" 1 -f -a x y z rx ry rz -b 32 -p 1 -e All' + #13#10 +
      '>>"' + Log + '" echo vJoyConfig %errorlevel%' + #13#10 +
      ':koniec' + #13#10, True);

  WizardForm.StatusLabel.Caption := CustomMessage('DriversStatus');
  if not ShellExec('runas', ExpandConstant('{cmd}'), '/c ""' + Script + '""', ExpandConstant('{tmp}'),
                   SW_HIDE, ewWaitUntilTerminated, Code) then begin
    SuppressibleMsgBox(CustomMessage('DriversFailed'), mbInformation, MB_OK, IDOK);
    Exit;
  end;

  Failed := '';
  if LoadStringsFromFile(Log, Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do begin
      Line := Trim(Lines[I]);
      // 3010 = zainstalowano, potrzebny restart
      if Pos(' 3010', Line) > 0 then
        DriversNeedRestart := True
      else if (Line <> '') and (Copy(Line, Length(Line) - 1, 2) <> ' 0') then begin
        if Failed <> '' then Failed := Failed + ', ';
        Failed := Failed + Line;
      end;
    end;
  if Failed <> '' then
    SuppressibleMsgBox(FmtMessage(CustomMessage('DriversPartly'), [Failed]), mbInformation, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    InstallDrivers();
end;

function NeedRestart(): Boolean;
begin
  Result := DriversNeedRestart;
end;

// Autostart moze byc wlaczony z panelu, nie z instalatora - usuwamy go, ale
// tylko gdy wskazuje te instalacje (a nie np. wersje Python).
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
begin
  if CurUninstallStep = usUninstall then
    if RegQueryStringValue(HKCU, RunKey, RunValue, Command) then
      if Pos(Lowercase(ExpandConstant('{app}\Kierownica.exe')), Lowercase(Command)) > 0 then
        RegDeleteValue(HKCU, RunKey, RunValue);
end;
