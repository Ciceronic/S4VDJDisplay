#define AppName "S4 VDJ Display"
#define AppVersion "1.0.12"

[Setup]
AppId={{8A7397D2-B0B0-497B-9A6B-E67E7F6AC6AA}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=S4 VDJ Display project
DefaultDirName={localappdata}\VirtualDJ\Plugins64\AutoStart
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
OutputDir=.
OutputBaseFilename=S4VDJDisplay_Setup_v12
Compression=lzma2
SolidCompression=yes
Uninstallable=yes
CreateUninstallRegKey=yes
WizardStyle=modern
SetupLogging=yes
CloseApplications=no

[Files]
Source: "wdi-simple.exe"; Flags: dontcopy
Source: "S4VDJDisplay_v38b.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "S4VDJDisplay_RestoreNI_v7.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "COPYING.txt"; DestDir: "{app}\Licenses"; DestName: "COPYING-libusb.txt"; Flags: ignoreversion
Source: "COPYING-LIBWDI"; DestDir: "{app}\Licenses"; DestName: "COPYING-libwdi.txt"; Flags: ignoreversion

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
  Wdi: String;
  DriverDir: String;
  Dest: String;
  Plugin: String;
  Usb: String;
  Params: String;
begin
  Result := '';
  Dest := ExpandConstant('{app}');

  if not DirExists(ExpandConstant('{localappdata}\VirtualDJ')) then
  begin
    Result := 'VirtualDJ user data directory was not found. Install and run VirtualDJ first.';
    Exit;
  end;

  if not DirExists(Dest) then
  begin
    Result := 'VirtualDJ AutoStart folder was not found: ' + Dest;
    Exit;
  end;

  try
    ExtractTemporaryFile('wdi-simple.exe');
  except
    Result := 'Could not unpack wdi-simple.exe: ' + GetExceptionMessage;
    Exit;
  end;

  Wdi := ExpandConstant('{tmp}\wdi-simple.exe');
  DriverDir := ExpandConstant('{tmp}\S4WinUSB');

  Params :=
    '-n "Traktor Kontrol S4 MK3 BD" ' +
    '-f s4mk3_display.inf ' +
    '-m "libwdi" ' +
    '-v 0x17CC -p 0x1720 -i 4 -t 0 ' +
    '-d ' + AddQuotes(DriverDir) + ' -s';

  Log('Installing WinUSB for USB\VID_17CC&PID_1720&MI_04');

  if not Exec(Wdi, Params, ExpandConstant('{tmp}'), SW_HIDE,
              ewWaitUntilTerminated, ExitCode) then
  begin
    Result := 'Could not start the S4 display driver installer.';
    Exit;
  end;

  if ExitCode <> 0 then
  begin
    Result := 'S4 display driver installation failed (code ' +
      IntToStr(ExitCode) + ').';
    Exit;
  end;

  Plugin := Dest + '\S4VDJDisplay_v38b.dll';
  Usb := Dest + '\libusb-1.0.dll';

  if FileExists(Plugin) and
     (not FileExists(Plugin + '.before-setup-v8.bak')) then
  begin
    if not CopyFile(Plugin, Plugin + '.before-setup-v8.bak', False) then
    begin
      Result := 'Could not back up the existing display plugin. Close VirtualDJ and retry.';
      Exit;
    end;
  end;

  if FileExists(Usb) and
     (not FileExists(Usb + '.before-setup-v8.bak')) then
  begin
    if not CopyFile(Usb, Usb + '.before-setup-v8.bak', False) then
    begin
      Result := 'Could not back up the existing libusb DLL. Close VirtualDJ and retry.';
      Exit;
    end;
  end;

  Log('WinUSB installation and backups completed.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then
    Log('S4 VDJ Display v8 installation finished.');
end;

[UninstallRun]
Filename: "{app}\S4VDJDisplay_RestoreNI_v7.exe"; Flags: runhidden waituntilterminated; RunOnceId: "RestoreNIDriver"