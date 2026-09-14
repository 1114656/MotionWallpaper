#ifndef MyAppVersion
  #error MyAppVersion must be supplied by scripts\build-installer.ps1
#endif
#ifndef MyAppFileVersion
  #error MyAppFileVersion must be supplied by scripts\build-installer.ps1
#endif

#ifndef MyAppName
  #define MyAppName "MotionWallpaper"
#endif
#ifndef MyAppPublisher
  #define MyAppPublisher "MotionWallpaper"
#endif
#ifndef MyAppExeName
  #define MyAppExeName "MotionWallpaper.exe"
#endif
#ifndef MyAppId
  #define MyAppId "{{F2984836-8AAC-4A5E-B137-69472F784A32}"
#endif
#ifndef MyInstallerBaseName
  #define MyInstallerBaseName "MotionWallpaper-v" + MyAppVersion + "-setup-windows-x64"
#endif
#ifndef LegacyDataRoot
  #define LegacyDataRoot "{localappdata}\MotionWallpaper"
#endif

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=no
UsePreviousAppDir=yes
PrivilegesRequired=lowest
MinVersion=10.0.19041
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\artifacts
OutputBaseFilename={#MyInstallerBaseName}
SetupIconFile=..\native\MotionWallpaper.App\Assets\motion-logo.ico
UninstallDisplayIcon={app}\App\{#MyAppExeName}
#ifdef InstallerSmokeTest
Compression=none
SolidCompression=no
#else
Compression=lzma2/max
SolidCompression=yes
#endif
WizardStyle=modern
CloseApplications=yes
CloseApplicationsFilter=MotionWallpaper.exe,motionwallpaper-agent.exe,motionwallpaper-renderer.exe
RestartApplications=no
SetupLogging=yes
LicenseFile=..\LICENSE
VersionInfoVersion={#MyAppFileVersion}
VersionInfoProductVersion={#MyAppFileVersion}
VersionInfoProductTextVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoDescription=MotionWallpaper 安装程序
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright=Copyright (c) 2026 MotionWallpaper contributors

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\*"; DestDir: "{app}\App"; Excludes: "Config\*,Wallpapers\*"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\App\{#MyAppExeName}"; WorkingDir: "{app}\App"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\App\{#MyAppExeName}"; WorkingDir: "{app}\App"; Tasks: desktopicon

[Run]
Filename: "{app}\App\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; WorkingDir: "{app}\App"; Flags: nowait postinstall skipifsilent; Check: LegacyMigrationAllowsAppLaunch

#ifndef InstallerSmokeTest
[UninstallRun]
Filename: "{cmd}"; Parameters: "/C taskkill /F /T /IM MotionWallpaper.exe >nul 2>&1 & taskkill /F /T /IM motionwallpaper-agent.exe >nul 2>&1 & taskkill /F /T /IM motionwallpaper-renderer.exe >nul 2>&1 & exit /B 0"; Flags: runhidden waituntilterminated; RunOnceId: "StopMotionWallpaper"
Filename: "{cmd}"; Parameters: "/C reg delete HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v MotionWallpaper /f >nul 2>&1 & exit /B 0"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveStartupEntry"
#endif

[UninstallDelete]
Type: filesandordirs; Name: "{app}\App\Config"
Type: filesandordirs; Name: "{app}\App\Wallpapers"
Type: files; Name: "{app}\App\legacy-data-fallback.mode"
Type: files; Name: "{app}\App\legacy-data-conflict.mode"
Type: files; Name: "{app}\App\legacy-migration-complete.mode"
; A media library moved outside {app} is deliberately user-owned data and is
; never traversed or deleted by the uninstaller.

[Code]
const
  MW_FILE_ATTRIBUTE_DIRECTORY = $00000010;
  MW_FILE_ATTRIBUTE_NORMAL = $00000080;
  MW_FILE_ATTRIBUTE_REPARSE_POINT = $00000400;
  MW_GENERIC_WRITE = $40000000;
  MW_FILE_SHARE_READ = $00000001;
  MW_FILE_SHARE_WRITE = $00000002;
  MW_FILE_SHARE_DELETE = $00000004;
  MW_OPEN_EXISTING = 3;
  MW_MOVEFILE_REPLACE_EXISTING = $00000001;
  MW_MOVEFILE_WRITE_THROUGH = $00000008;
  MW_INVALID_FILE_ATTRIBUTES = $FFFFFFFF;
  MW_INVALID_HANDLE_VALUE = $FFFFFFFF;
  MW_MIGRATION_OWNER_MARKER = '.legacy-migration-owner.mode';

var
  MigrationAllowsAppLaunch: Boolean;
  MigrationConflictDetected: Boolean;

function MWGetFileAttributes(const FileName: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function MWGetCurrentProcessId(): LongWord;
  external 'GetCurrentProcessId@kernel32.dll stdcall';

function MWCreateFile(const FileName: String; DesiredAccess, ShareMode,
  SecurityAttributes, CreationDisposition, FlagsAndAttributes,
  TemplateFile: LongWord): LongWord;
  external 'CreateFileW@kernel32.dll stdcall';

function MWFlushFileBuffers(Handle: LongWord): Boolean;
  external 'FlushFileBuffers@kernel32.dll stdcall';

function MWCloseHandle(Handle: LongWord): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function MWMoveFileEx(const ExistingFileName, NewFileName: String;
  Flags: LongWord): Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

procedure MWExitProcess(ExitCode: LongWord);
  external 'ExitProcess@kernel32.dll stdcall';

function InitializeSetup(): Boolean;
begin
  MigrationAllowsAppLaunch := True;
  MigrationConflictDetected := False;
  Result := True;
end;

#ifdef InstallerSmokeTest
function InstallerSmokeSwitchPresent(const SwitchName: String): Boolean;
var
  Index: Integer;
begin
  Result := False;
  for Index := 1 to ParamCount do
  begin
    if CompareText(ParamStr(Index), SwitchName) = 0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;
#endif

function WriteDurableMarkerAtomic(const MarkerPath, Value: String): Boolean;
var
  TemporaryPath: String;
  Handle: LongWord;
  Flushed: Boolean;
begin
  Result := False;
  TemporaryPath := MarkerPath + '.tmp-' +
    IntToStr(Integer(MWGetCurrentProcessId()));
  if DirExists(TemporaryPath) then
  begin
    Log('Durable marker staging path is unexpectedly a directory: ' + TemporaryPath);
    Exit;
  end;
  if FileExists(TemporaryPath) and not DeleteFile(TemporaryPath) then
  begin
    Log('Cannot remove stale durable marker staging file: ' + TemporaryPath);
    Exit;
  end;
  if not SaveStringToFile(TemporaryPath, Value, False) then
  begin
    Log('Cannot write durable marker staging file: ' + TemporaryPath);
    Exit;
  end;

  Handle := MWCreateFile(TemporaryPath, MW_GENERIC_WRITE,
    MW_FILE_SHARE_READ or MW_FILE_SHARE_WRITE or MW_FILE_SHARE_DELETE,
    0, MW_OPEN_EXISTING, MW_FILE_ATTRIBUTE_NORMAL, 0);
  if Handle = MW_INVALID_HANDLE_VALUE then
  begin
    Log('Cannot open durable marker staging file for flush: ' + TemporaryPath +
      '; Win32 error ' + IntToStr(DLLGetLastError()));
    DeleteFile(TemporaryPath);
    Exit;
  end;
  Flushed := MWFlushFileBuffers(Handle);
  MWCloseHandle(Handle);
  if not Flushed then
  begin
    Log('Cannot flush durable marker staging file: ' + TemporaryPath +
      '; Win32 error ' + IntToStr(DLLGetLastError()));
    DeleteFile(TemporaryPath);
    Exit;
  end;

  { The staging file lives beside the marker, so this is a same-volume atomic
    replacement. WRITE_THROUGH makes the namespace update durable before a
    migration directory can acquire an authoritative Config/Wallpapers name. }
  if not MWMoveFileEx(TemporaryPath, MarkerPath,
    MW_MOVEFILE_REPLACE_EXISTING or MW_MOVEFILE_WRITE_THROUGH) then
  begin
    Log('Cannot atomically publish durable marker: ' + MarkerPath +
      '; Win32 error ' + IntToStr(DLLGetLastError()));
    DeleteFile(TemporaryPath);
    Exit;
  end;
  Result := True;
end;

function MigrationTokenIsValid(const Token: String): Boolean;
var
  Index: Integer;
  Character: Char;
begin
  Result := Length(Token) = 64;
  if not Result then
    Exit;
  for Index := 1 to Length(Token) do
  begin
    Character := Token[Index];
    if not (((Character >= '0') and (Character <= '9')) or
            ((Character >= 'a') and (Character <= 'f')) or
            ((Character >= 'A') and (Character <= 'F'))) then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function NewMigrationToken(): String;
begin
  Result := GetSHA256OfUnicodeString(
    ExpandConstant('{app}') + '|' +
    GetDateTimeString('yyyy/mm/dd hh:nn:ss.zzz', '/', ':') + '|' +
    IntToStr(Integer(MWGetCurrentProcessId())) + '|' +
    IntToStr(Random($7FFFFFFF)) + '|' + IntToStr(Random($7FFFFFFF)));
end;

function MigrationFallbackValue(const Token: String): String;
begin
  Result := 'MotionWallpaper.LegacyFallback/v2'#13#10 + Token + #13#10;
end;

function LoadMigrationFallbackToken(const MarkerPath: String;
  var Token: String): Boolean;
var
  Lines: TArrayOfString;
begin
  Result := False;
  Token := '';
  if not LoadStringsFromFile(MarkerPath, Lines) or
     (GetArrayLength(Lines) <> 2) or
     (Lines[0] <> 'MotionWallpaper.LegacyFallback/v2') then
    Exit;
  Token := Trim(Lines[1]);
  Result := MigrationTokenIsValid(Token);
end;

function MigrationOwnershipValue(const DirectoryName,
  Token: String): String;
begin
  Result := 'MotionWallpaper.LegacyMigrationOwner/v1'#13#10 +
    DirectoryName + #13#10 + Token + #13#10;
end;

function MigrationOwnershipMarkerPath(const TargetDir: String): String;
begin
  Result := AddBackslash(TargetDir) + MW_MIGRATION_OWNER_MARKER;
end;

function MigrationOwnershipMatches(const TargetDir, DirectoryName,
  Token: String): Boolean;
var
  MarkerPath: String;
  MarkerAttributes: LongWord;
  Lines: TArrayOfString;
begin
  Result := False;
  MarkerPath := MigrationOwnershipMarkerPath(TargetDir);
  MarkerAttributes := MWGetFileAttributes(MarkerPath);
  if (MarkerAttributes = MW_INVALID_FILE_ATTRIBUTES) or
     ((MarkerAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) or
     ((MarkerAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
     not LoadStringsFromFile(MarkerPath, Lines) then
    Exit;
  Result := (GetArrayLength(Lines) = 3) and
    (Lines[0] = 'MotionWallpaper.LegacyMigrationOwner/v1') and
    (Lines[1] = DirectoryName) and (Lines[2] = Token);
end;

function DirectoryTreeIsReparseFree(const RootDir: String): Boolean;
var
  FindRec: TFindRec;
  ChildPath: String;
  Attributes: LongWord;
begin
  Result := False;
  Attributes := MWGetFileAttributes(RootDir);
  if (Attributes = MW_INVALID_FILE_ATTRIBUTES) or
     ((Attributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((Attributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
    Exit;

  if not FindFirst(AddBackslash(RootDir) + '*', FindRec) then
  begin
    Log('Refusing to trust a directory that cannot be enumerated: ' + RootDir);
    Exit;
  end;

  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        ChildPath := AddBackslash(RootDir) + FindRec.Name;
        Attributes := MWGetFileAttributes(ChildPath);
        if (Attributes = MW_INVALID_FILE_ATTRIBUTES) or
           ((Attributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
        begin
          Log('Refusing to traverse a reparse point or unreadable path: ' + ChildPath);
          Exit;
        end;
        if ((Attributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) and
           (not DirectoryTreeIsReparseFree(ChildPath)) then
          Exit;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
  Result := True;
end;

function FilesMatchBySizeAndSHA256(const SourcePath, TargetPath: String): Boolean;
var
  SourceRec: TFindRec;
  TargetRec: TFindRec;
begin
  Result := False;
  if not FindFirst(SourcePath, SourceRec) then
    Exit;
  try
    if ((SourceRec.Attributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) or
       ((SourceRec.Attributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
      Exit;
    if not FindFirst(TargetPath, TargetRec) then
      Exit;
    try
      if ((TargetRec.Attributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) or
         ((TargetRec.Attributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
         (SourceRec.SizeHigh <> TargetRec.SizeHigh) or
         (SourceRec.SizeLow <> TargetRec.SizeLow) then
        Exit;
      try
        Result := GetSHA256OfFile(SourcePath) = GetSHA256OfFile(TargetPath);
      except
        Log('Unable to hash a legacy migration file pair: ' + SourcePath + ' -> ' + TargetPath);
        Result := False;
      end;
    finally
      FindClose(TargetRec);
    end;
  finally
    FindClose(SourceRec);
  end;
end;

function DirectoryTreesMatchInternal(const SourceDir, TargetDir,
  AllowedExtraTargetPath: String): Boolean;
var
  FindRec: TFindRec;
  SourcePath: String;
  TargetPath: String;
  SourceAttributes: LongWord;
  TargetAttributes: LongWord;
begin
  Result := False;
  SourceAttributes := MWGetFileAttributes(SourceDir);
  TargetAttributes := MWGetFileAttributes(TargetDir);
  if (SourceAttributes = MW_INVALID_FILE_ATTRIBUTES) or
     (TargetAttributes = MW_INVALID_FILE_ATTRIBUTES) or
     ((SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((SourceAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
     ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
    Exit;

  { Source-to-target pass: every source entry must exist with the same type and
    every file must have the same 64-bit size and SHA-256 digest. }
  if not FindFirst(AddBackslash(SourceDir) + '*', FindRec) then
    Exit;
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        SourcePath := AddBackslash(SourceDir) + FindRec.Name;
        TargetPath := AddBackslash(TargetDir) + FindRec.Name;
        SourceAttributes := MWGetFileAttributes(SourcePath);
        TargetAttributes := MWGetFileAttributes(TargetPath);
        if (SourceAttributes = MW_INVALID_FILE_ATTRIBUTES) or
           (TargetAttributes = MW_INVALID_FILE_ATTRIBUTES) or
           ((SourceAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
           ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
           (((SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) <>
            ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0)) then
          Exit;
        if (SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          if not DirectoryTreesMatchInternal(SourcePath, TargetPath,
            AllowedExtraTargetPath) then
            Exit;
        end
        else if not FilesMatchBySizeAndSHA256(SourcePath, TargetPath) then
          Exit;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;

  { Target-to-source pass: reject injected or otherwise unexpected target
    entries even if every source file copied successfully. }
  if not FindFirst(AddBackslash(TargetDir) + '*', FindRec) then
    Exit;
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        SourcePath := AddBackslash(SourceDir) + FindRec.Name;
        TargetPath := AddBackslash(TargetDir) + FindRec.Name;
        SourceAttributes := MWGetFileAttributes(SourcePath);
        TargetAttributes := MWGetFileAttributes(TargetPath);
        if (AllowedExtraTargetPath <> '') and
           PathSame(TargetPath, AllowedExtraTargetPath) then
        begin
          if (TargetAttributes = MW_INVALID_FILE_ATTRIBUTES) or
             ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) or
             ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
            Exit;
        end
        else
        begin
        if (SourceAttributes = MW_INVALID_FILE_ATTRIBUTES) or
           (TargetAttributes = MW_INVALID_FILE_ATTRIBUTES) or
           ((SourceAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
           ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
           (((SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) <>
            ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0)) then
          Exit;
        end;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
  Result := True;
end;

function DirectoryTreesMatch(const SourceDir, TargetDir: String): Boolean;
begin
  Result := DirectoryTreesMatchInternal(SourceDir, TargetDir, '');
end;

function PublishedMigrationTreeMatches(const SourceDir, TargetDir,
  DirectoryName, Token: String): Boolean;
begin
  Result := MigrationOwnershipMatches(TargetDir, DirectoryName, Token) and
    DirectoryTreesMatchInternal(SourceDir, TargetDir,
      MigrationOwnershipMarkerPath(TargetDir));
end;

function TargetTreeIsVerifiedSourceSubset(const SourceDir, TargetDir: String): Boolean;
var
  FindRec: TFindRec;
  SourcePath: String;
  TargetPath: String;
  SourceAttributes: LongWord;
  TargetAttributes: LongWord;
begin
  Result := False;
  SourceAttributes := MWGetFileAttributes(SourceDir);
  TargetAttributes := MWGetFileAttributes(TargetDir);
  if (SourceAttributes = MW_INVALID_FILE_ATTRIBUTES) or
     (TargetAttributes = MW_INVALID_FILE_ATTRIBUTES) or
     ((SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((SourceAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
     ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
    Exit;

  if not FindFirst(AddBackslash(TargetDir) + '*', FindRec) then
    Exit;
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        SourcePath := AddBackslash(SourceDir) + FindRec.Name;
        TargetPath := AddBackslash(TargetDir) + FindRec.Name;
        SourceAttributes := MWGetFileAttributes(SourcePath);
        TargetAttributes := MWGetFileAttributes(TargetPath);
        if (SourceAttributes = MW_INVALID_FILE_ATTRIBUTES) or
           (TargetAttributes = MW_INVALID_FILE_ATTRIBUTES) or
           ((SourceAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
           ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
           (((SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) <>
            ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0)) then
          Exit;
        if (TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          if not TargetTreeIsVerifiedSourceSubset(SourcePath, TargetPath) then
            Exit;
        end
        else if not FilesMatchBySizeAndSHA256(SourcePath, TargetPath) then
          Exit;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
  Result := True;
end;

function CopyDirectoryContents(const SourceDir, TargetDir: String): Boolean;
var
  FindRec: TFindRec;
  SourcePath: String;
  TargetPath: String;
  SourceAttributes: LongWord;
  TargetAttributes: LongWord;
begin
  Result := False;
  SourceAttributes := MWGetFileAttributes(SourceDir);
  if (SourceAttributes = MW_INVALID_FILE_ATTRIBUTES) or
     ((SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((SourceAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
    Exit;

  TargetAttributes := MWGetFileAttributes(TargetDir);
  if (TargetAttributes = MW_INVALID_FILE_ATTRIBUTES) or
     ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
    Exit;

  if FindFirst(AddBackslash(SourceDir) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          SourcePath := AddBackslash(SourceDir) + FindRec.Name;
          TargetPath := AddBackslash(TargetDir) + FindRec.Name;
          SourceAttributes := MWGetFileAttributes(SourcePath);
          if (SourceAttributes = MW_INVALID_FILE_ATTRIBUTES) or
             ((SourceAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
          begin
            Log('Legacy migration stopped at a reparse point or unreadable path: ' + SourcePath);
            Result := False;
            Exit;
          end;
          if (SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          begin
            if MWGetFileAttributes(TargetPath) <> MW_INVALID_FILE_ATTRIBUTES then
              Result := False
            else
            begin
              Result := CreateDir(TargetPath);
              if Result then
                Result := CopyDirectoryContents(SourcePath, TargetPath);
            end;
          end
          else if MWGetFileAttributes(TargetPath) <> MW_INVALID_FILE_ATTRIBUTES then
            Result := False
          else
            Result := CopyFile(SourcePath, TargetPath, True);
          if not Result then
            Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  Result := True;
end;

function ClaimMigrationStaging(const TargetDir: String; var StagingDir: String): Boolean;
var
  Attempt: Integer;
  BaseName: String;
begin
  Result := False;
  BaseName := TargetDir + '.legacy-migration-' + IntToStr(Integer(MWGetCurrentProcessId()));
  for Attempt := 0 to 63 do
  begin
    StagingDir := BaseName + '-' + IntToStr(Attempt);
    if (MWGetFileAttributes(StagingDir) = MW_INVALID_FILE_ATTRIBUTES) and
       CreateDir(StagingDir) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function ArchiveFallbackTarget(const TargetDir: String): Boolean;
var
  Attempt: Integer;
  RecoveryDir: String;
  BaseName: String;
  Attributes: LongWord;
begin
  Result := False;
  Attributes := MWGetFileAttributes(TargetDir);
  if Attributes = MW_INVALID_FILE_ATTRIBUTES then
  begin
    Result := True;
    Exit;
  end;
  if ((Attributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((Attributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
    Exit;

  BaseName := TargetDir + '.legacy-recovery-' + IntToStr(Integer(MWGetCurrentProcessId()));
  for Attempt := 0 to 63 do
  begin
    RecoveryDir := BaseName + '-' + IntToStr(Attempt);
    if (MWGetFileAttributes(RecoveryDir) = MW_INVALID_FILE_ATTRIBUTES) and
       RenameFile(TargetDir, RecoveryDir) then
    begin
      Log('Archived an earlier incomplete migration target at ' + RecoveryDir);
      Result := True;
      Exit;
    end;
  end;
end;

function PreflightLegacyDataDirectory(const DirectoryName: String;
  const RetryIncompleteMigration, TrustExistingTarget: Boolean;
  const MigrationToken: String;
  var WouldPublish: Boolean): Boolean;
var
  LegacyRoot: String;
  SourceDir: String;
  TargetDir: String;
  SourceAttributes: LongWord;
  TargetAttributes: LongWord;
begin
  Result := False;
  WouldPublish := False;
  LegacyRoot := ExpandConstant('{#LegacyDataRoot}');
  SourceDir := AddBackslash(LegacyRoot) + DirectoryName;
  TargetDir := ExpandConstant('{app}\App\') + DirectoryName;
  SourceAttributes := MWGetFileAttributes(SourceDir);
  TargetAttributes := MWGetFileAttributes(TargetDir);

  if SourceAttributes = MW_INVALID_FILE_ATTRIBUTES then
  begin
    if TargetAttributes = MW_INVALID_FILE_ATTRIBUTES then
      Result := True
    else
      Result := ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) and
        ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) = 0) and
        DirectoryTreeIsReparseFree(TargetDir);
    Exit;
  end;

  if ((SourceAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((SourceAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
     (MWGetFileAttributes(MigrationOwnershipMarkerPath(SourceDir)) <>
       MW_INVALID_FILE_ATTRIBUTES) or
     not DirectoryTreeIsReparseFree(LegacyRoot) or
     not DirectoryTreeIsReparseFree(SourceDir) then
    Exit;

  if TargetAttributes = MW_INVALID_FILE_ATTRIBUTES then
  begin
    WouldPublish := True;
    Result := True;
    Exit;
  end;
  if ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
     ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) or
     not DirectoryTreeIsReparseFree(TargetDir) then
    Exit;

  if RetryIncompleteMigration then
  begin
    if not MigrationOwnershipMatches(TargetDir, DirectoryName,
      MigrationToken) then
      Exit;
    WouldPublish := True;
    Result := True;
  end
  else if TrustExistingTarget then
    Result := True
  else
    Result := DirectoryTreesMatch(SourceDir, TargetDir);
end;

function MigrateLegacyDataDirectory(const DirectoryName: String;
  const RetryIncompleteMigration, TrustExistingTarget: Boolean;
  const MigrationToken: String): Boolean;
var
  LegacyRoot: String;
  SourceDir: String;
  TargetDir: String;
  StagingDir: String;
  TargetAttributes: LongWord;
begin
  Result := False;
  LegacyRoot := ExpandConstant('{#LegacyDataRoot}');
  SourceDir := AddBackslash(LegacyRoot) + DirectoryName;
  TargetDir := ExpandConstant('{app}\App\') + DirectoryName;
  TargetAttributes := MWGetFileAttributes(TargetDir);
  if not DirExists(SourceDir) then
  begin
    { With no legacy side, an existing install-tree directory is the only
      possible user-data copy. It is usable only when the complete tree can be
      inspected without crossing a reparse point. }
    if TargetAttributes = MW_INVALID_FILE_ATTRIBUTES then
      Result := True
    else if ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) and
            ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) = 0) and
            DirectoryTreeIsReparseFree(TargetDir) then
      Result := True
    else
    begin
      Log('Existing install-tree data is unsafe or unreadable: ' + TargetDir);
      MigrationConflictDetected := True;
    end;
    Exit;
  end;

  if (TargetAttributes <> MW_INVALID_FILE_ATTRIBUTES) and TrustExistingTarget and
     ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) <> 0) and
     ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) = 0) and
     DirectoryTreeIsReparseFree(TargetDir) then
  begin
    Log('Keeping the previously verified authoritative data directory: ' + TargetDir);
    Result := True;
    Exit;
  end;

  if not DirectoryTreeIsReparseFree(LegacyRoot) or
     not DirectoryTreeIsReparseFree(SourceDir) or
     (MWGetFileAttributes(MigrationOwnershipMarkerPath(SourceDir)) <>
       MW_INVALID_FILE_ATTRIBUTES) then
  begin
    Log('Skipping legacy data migration because its source contains a reparse point or cannot be verified: ' + SourceDir);
    MigrationConflictDetected := True;
    Exit;
  end;

  if TargetAttributes <> MW_INVALID_FILE_ATTRIBUTES then
  begin
    if RetryIncompleteMigration then
    begin
      if ((TargetAttributes and MW_FILE_ATTRIBUTE_DIRECTORY) = 0) or
         ((TargetAttributes and MW_FILE_ATTRIBUTE_REPARSE_POINT) <> 0) then
      begin
        Log('Incomplete migration target has an unsafe type: ' + TargetDir);
        MigrationConflictDetected := True;
        Exit;
      end;
      if not MigrationOwnershipMatches(TargetDir, DirectoryName,
        MigrationToken) then
      begin
        Log('Incomplete migration target has no matching transaction ownership marker: ' + TargetDir);
        MigrationConflictDetected := True;
        Exit;
      end;
      if not ArchiveFallbackTarget(TargetDir) then
      begin
        Log('Cannot archive an earlier incomplete migration target: ' + TargetDir);
        Exit;
      end;
    end;
    if not RetryIncompleteMigration then
    begin
      { An alpha.4 installation may have left either a partial target or a
        partially deleted source. Without a marker neither side is provably
        authoritative, so accept only an exact match and never overwrite one
        with the other. }
      if DirectoryTreesMatch(SourceDir, TargetDir) then
      begin
        Log('Existing legacy and installed data trees match exactly: ' + TargetDir);
        Result := True;
      end
      else
      begin
        Log('Legacy migration conflict: preserving both non-matching trees: ' + SourceDir + ' and ' + TargetDir);
        MigrationConflictDetected := True;
      end;
      Exit;
    end;
  end;

  { Copy only into a non-authoritative sibling. A failed attempt must never
    create Config or Wallpapers, because either name selects the install tree
    as the live data root on the next application start. }
  if not ClaimMigrationStaging(TargetDir, StagingDir) then
  begin
    Log('Cannot exclusively claim a legacy migration staging directory for ' + TargetDir);
    Exit;
  end;

  Log('Staging MotionWallpaper data from ' + SourceDir + ' to ' + StagingDir);
  if not CopyDirectoryContents(SourceDir, StagingDir) then
  begin
    Log('Migration copy failed; preserving the original and non-authoritative staging tree: ' + SourceDir);
    Exit;
  end
  else if not DirectoryTreeIsReparseFree(LegacyRoot) or
          not DirectoryTreesMatch(SourceDir, StagingDir) then
  begin
    Log('Migration verification failed; preserving the original and non-authoritative staging tree: ' + SourceDir);
    Exit;
  end;

  if not WriteDurableMarkerAtomic(MigrationOwnershipMarkerPath(StagingDir),
    MigrationOwnershipValue(DirectoryName, MigrationToken)) then
  begin
    Log('Cannot persist migration ownership inside staging: ' + StagingDir);
    Exit;
  end;

  if (MWGetFileAttributes(TargetDir) <> MW_INVALID_FILE_ATTRIBUTES) or
     not RenameFile(StagingDir, TargetDir) then
  begin
    Log('Migration target appeared or could not be published atomically: ' + TargetDir);
    Exit;
  end;

  { Revalidate after publication. If the legacy source changed at the handoff,
    move the copy back out of the authoritative name without deleting either. }
  if not DirectoryTreeIsReparseFree(LegacyRoot) or
     not PublishedMigrationTreeMatches(SourceDir, TargetDir, DirectoryName,
       MigrationToken) then
  begin
    Log('Published migration failed its final verification; withdrawing it: ' + TargetDir);
    if not RenameFile(TargetDir, StagingDir) then
      Log('Unable to withdraw the unverified target; legacy fallback remains mandatory: ' + TargetDir);
    Exit;
  end;

  Log('Migration copied and verified; preserving the legacy source as a rollback copy: ' + SourceDir);
  Result := True;
end;

function LegacyMigrationAllowsAppLaunch(): Boolean;
begin
  Result := MigrationAllowsAppLaunch;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  LegacyRoot: String;
  FallbackMarker: String;
  ConflictMarker: String;
  CompleteMarker: String;
  RetryIncompleteMigration: Boolean;
  TrustExistingTarget: Boolean;
  MigrationNeedsFallback: Boolean;
  FallbackCleared: Boolean;
  ConfigWouldPublish: Boolean;
  WallpapersWouldPublish: Boolean;
  ConfigPreflightPassed: Boolean;
  WallpapersPreflightPassed: Boolean;
  ConfigMigrated: Boolean;
  WallpapersMigrated: Boolean;
  MigrationToken: String;
  OwnershipMarker: String;
#ifdef InstallerSmokeTest
  InjectedTarget: String;
#endif
begin
  if CurStep = ssPostInstall then
  begin
    MigrationAllowsAppLaunch := True;
    MigrationConflictDetected := False;
    LegacyRoot := ExpandConstant('{#LegacyDataRoot}');
    FallbackMarker := ExpandConstant('{app}\App\legacy-data-fallback.mode');
    ConflictMarker := ExpandConstant('{app}\App\legacy-data-conflict.mode');
    CompleteMarker := ExpandConstant('{app}\App\legacy-migration-complete.mode');
    RetryIncompleteMigration := FileExists(FallbackMarker);
    if RetryIncompleteMigration then
    begin
      if not LoadMigrationFallbackToken(FallbackMarker, MigrationToken) then
      begin
        MigrationAllowsAppLaunch := False;
        if not WriteDurableMarkerAtomic(ConflictMarker,
          'MotionWallpaper.LegacyConflict/v1'#13#10) then
          RaiseException('Legacy migration fallback is invalid and its conflict marker could not be recorded safely.');
        Log('Legacy migration fallback has no valid transaction token; refusing to archive or publish either data directory.');
        Exit;
      end;
    end
    else
      MigrationToken := NewMigrationToken();
    { A fallback marker always wins. The previous pass may have written its
      completion marker but then failed to clear fallback; a manually launched
      app would still have used the legacy root and may have changed it since.
      Re-stage from that authoritative legacy copy instead of trusting a stale
      install-tree target merely because both markers exist. }
    TrustExistingTarget := FileExists(CompleteMarker) and
      not RetryIncompleteMigration;

    { Inspect both sides before changing any migration state. A fresh ambiguous
      target must never be converted into a retry target merely because the
      other directory would otherwise need publication. }
    ConfigPreflightPassed := PreflightLegacyDataDirectory('Config',
      RetryIncompleteMigration, TrustExistingTarget, MigrationToken,
      ConfigWouldPublish);
    WallpapersPreflightPassed := PreflightLegacyDataDirectory('Wallpapers',
      RetryIncompleteMigration, TrustExistingTarget, MigrationToken,
      WallpapersWouldPublish);
    if not ConfigPreflightPassed or not WallpapersPreflightPassed then
    begin
      MigrationConflictDetected := True;
      MigrationAllowsAppLaunch := False;
      if not WriteDurableMarkerAtomic(ConflictMarker,
        'MotionWallpaper.LegacyConflict/v1'#13#10) then
        RaiseException('Legacy migration conflict could not be recorded safely.');
      Log('Legacy migration preflight found an ambiguous or unsafe tree; preserving both roots without publishing either directory.');
      Exit;
    end;

    { Establish the durable selector before either directory can acquire its
      authoritative Config/Wallpapers name. Keep the entry-state retry bit
      separate: a newly created guard must not turn an unrelated existing
      target into a fallback target during this same pass. }
    MigrationNeedsFallback := RetryIncompleteMigration or
      ConfigWouldPublish or WallpapersWouldPublish;
    if MigrationNeedsFallback and
       not WriteDurableMarkerAtomic(FallbackMarker,
         MigrationFallbackValue(MigrationToken)) then
    begin
      MigrationAllowsAppLaunch := False;
      RaiseException('Legacy migration cannot start because its durable fallback marker could not be written.');
    end;

#ifdef InstallerSmokeTest
    if InstallerSmokeSwitchPresent('/MIGRATIONRACETEST') then
    begin
      InjectedTarget := ExpandConstant('{app}\App\Config');
      if (MWGetFileAttributes(InjectedTarget) <> MW_INVALID_FILE_ATTRIBUTES) or
         not ForceDirectories(InjectedTarget) or
         not SaveStringToFile(AddBackslash(InjectedTarget) + 'settings.json',
           '{"injectedAfterPreflight":true}', False) then
        RaiseException('Could not inject the post-preflight migration target race.');
      Log('Injected an unowned Config target after migration preflight and durable fallback publication.');
    end;
#endif

    ConfigMigrated := MigrateLegacyDataDirectory(
      'Config', RetryIncompleteMigration, TrustExistingTarget, MigrationToken);
#ifdef InstallerMigrationFaultAfterConfig
    if ConfigMigrated and ConfigWouldPublish and WallpapersWouldPublish then
    begin
      Log('Injecting a hard process exit between Config and Wallpapers publication.');
      MWExitProcess(197);
    end;
#endif
    if ConfigMigrated then
    begin
      WallpapersMigrated := MigrateLegacyDataDirectory(
        'Wallpapers', RetryIncompleteMigration, TrustExistingTarget,
        MigrationToken);
    end
    else
      WallpapersMigrated := False;

    if ConfigMigrated and WallpapersMigrated then
    begin
      if not WriteDurableMarkerAtomic(CompleteMarker,
        'MotionWallpaper.LegacyMigration/v1'#13#10) then
      begin
        Log('Cannot persist the completed legacy migration marker; suppressing automatic launch.');
        MigrationAllowsAppLaunch := False;
        RaiseException('Legacy migration completed, but its durable completion marker could not be written.');
      end;
      FallbackCleared := not FileExists(FallbackMarker);
      if not FallbackCleared then
      begin
        FallbackCleared := DeleteFile(FallbackMarker);
        if not FallbackCleared then
        begin
          Log('Cannot clear the legacy fallback marker; suppressing automatic launch.');
          MigrationAllowsAppLaunch := False;
        end;
      end;
      if FallbackCleared then
      begin
        OwnershipMarker := MigrationOwnershipMarkerPath(
          ExpandConstant('{app}\App\Config'));
        if FileExists(OwnershipMarker) and not DeleteFile(OwnershipMarker) then
          Log('Cannot clear the completed Config migration ownership marker: ' + OwnershipMarker);
        OwnershipMarker := MigrationOwnershipMarkerPath(
          ExpandConstant('{app}\App\Wallpapers'));
        if FileExists(OwnershipMarker) and not DeleteFile(OwnershipMarker) then
          Log('Cannot clear the completed Wallpapers migration ownership marker: ' + OwnershipMarker);
      end;
      if FileExists(ConflictMarker) and not DeleteFile(ConflictMarker) then
      begin
        Log('Cannot clear the legacy conflict marker; suppressing automatic launch.');
        MigrationAllowsAppLaunch := False;
      end;
    end
    else
    begin
      MigrationAllowsAppLaunch := False;
      if MigrationConflictDetected then
      begin
        if not WriteDurableMarkerAtomic(ConflictMarker,
          'MotionWallpaper.LegacyConflict/v1'#13#10) then
          RaiseException('Legacy migration conflict could not be recorded safely.');
        Log('Legacy migration is ambiguous or unsafe; preserving both roots and suppressing automatic launch.');
      end
      else
      begin
        { Common checks this marker before portable.mode and before existing
          Config/Wallpapers, so a known failed attempt cannot hide the complete
          legacy source on the next start. }
        if not WriteDurableMarkerAtomic(FallbackMarker,
          MigrationFallbackValue(MigrationToken)) then
          RaiseException('Legacy migration failed and its safe fallback marker could not be written.');
        Log('Legacy migration is incomplete; the application will continue using ' + LegacyRoot);
      end;
    end;
  end;
end;
