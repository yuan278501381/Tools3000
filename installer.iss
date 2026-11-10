[Defines]
#ifndef Tools3000Version
  #error Tools3000Version must be supplied by deploy.ps1 from the root VERSION file
#endif
#ifndef Tools3000Architecture
  #define Tools3000Architecture "x64"
#endif
#ifndef Tools3000SetupBaseFilename
  #define Tools3000SetupBaseFilename "Tools3000-Setup"
#endif
#if Tools3000Architecture != "x64" && Tools3000Architecture != "arm64"
  #error Tools3000Architecture must be x64 or arm64
#endif

[Setup]
AppName=Tools3000
AppVersion={#Tools3000Version}
UninstallDisplayName=Tools3000
AppPublisher=Yy1 (yuan278501381)
AppPublisherURL=https://github.com/yuan278501381/Tools3000
AppCopyright=Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors
AppSupportURL=https://github.com/yuan278501381/Tools3000/issues
AppUpdatesURL=https://github.com/yuan278501381/Tools3000/releases
DefaultDirName={autopf}\Tools3000
DefaultGroupName=Tools3000
DisableProgramGroupPage=yes
OutputBaseFilename={#Tools3000SetupBaseFilename}
#ifndef CompressionLevel
  #define CompressionLevel "lzma2/ultra64"
#endif
Compression={#CompressionLevel}
SolidCompression=yes
#ifdef Tools3000SignedBuild
SignTool=tools3000
SignedUninstaller=yes
#endif
#if Tools3000Architecture == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
SetupIconFile=resources\app.ico
UninstallDisplayIcon={app}\Tools3000.exe
; 全盘 NTFS 索引服务需要管理员权限注册并读取 USN Journal。
PrivilegesRequired=admin
; 进程管理由 Tools3000 原生互斥体与消息循环毫秒级接管；禁用重启管理器以消除 1000ms 开销
CloseApplications=no
RestartApplications=no
; 默认严格跟随系统 UI 语言，非中文环境一律纯英文兜底，零弹窗干扰
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
; 通知 Explorer 刷新文件关联与图标缓存
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "resources\installer\ChineseSimplified.isl"

[CustomMessages]
chinesesimplified.AppRunningPrompt=安装程序检测到 Tools3000 正在运行。%n%n是否自动关闭正在运行的 Tools3000 并继续安装？
english.AppRunningPrompt=Setup detected that Tools3000 is currently running.%n%nWould you like to automatically close running instances of Tools3000 and continue with the installation?
chinesesimplified.InstallationAbortedByUser=安装已由用户取消。请关闭 Tools3000 后重新运行安装程序。
english.InstallationAbortedByUser=Installation was cancelled by the user. Please close Tools3000 and rerun setup.
chinesesimplified.AppRunningUninstallPrompt=卸载程序检测到 Tools3000 正在运行。%n%n是否自动关闭 Tools3000 并继续卸载？
english.AppRunningUninstallPrompt=Uninstall detected that Tools3000 is currently running.%n%nWould you like to automatically close running instances of Tools3000 and continue?
chinesesimplified.UninstallAbortedByUser=卸载已由用户取消。请关闭 Tools3000 后重新运行卸载程序。
english.UninstallAbortedByUser=Uninstall was cancelled by the user. Please close Tools3000 and rerun uninstall.
chinesesimplified.InstallingService=正在安装快速文件索引服务...
english.InstallingService=Installing fast file search index service...
chinesesimplified.ShowDetails=详细信息(&D)
english.ShowDetails=Show &Details
chinesesimplified.HideDetails=隐藏信息(&D)
english.HideDetails=Hide &Details
chinesesimplified.PersonalDataTitle=个人数据与配置处理
english.PersonalDataTitle=Personal Data & Preferences
chinesesimplified.PersonalDataDescription=请选择卸载 Tools3000 时的个人数据处理方式：
english.PersonalDataDescription=Choose how your personal data and configuration should be handled:
chinesesimplified.UninstallModeStandard=保留个人配置与媒体 (推荐)
english.UninstallModeStandard=Keep personal configuration and media (Recommended)
chinesesimplified.UninstallModeStandardDesc=仅移除应用程序与后台服务。保留您的偏好设置、手势方案以及截图/录屏作品，便于以后重新安装。
english.UninstallModeStandardDesc=Removes only the application and services. Retains your preferences, gestures, and screenshots/recordings for future installations.
chinesesimplified.UninstallModeClean=清理全部配置与运行缓存
english.UninstallModeClean=Clear all configuration and runtime caches
chinesesimplified.UninstallModeCleanDesc=删除偏好设置、窗口记忆、运行历史、诊断日志与搜索索引缓存。
english.UninstallModeCleanDesc=Removes user preferences, window state, run history, diagnostic logs, and search index caches.
chinesesimplified.DeleteMediaCaptures=同时永久删除“截图和录屏”媒体文件
english.DeleteMediaCaptures=Permanently delete screenshot and recording files as well
chinesesimplified.DeleteMediaCapturesNote=注意：此操作将清空默认媒体保存目录，删除后无法恢复。
english.DeleteMediaCapturesNote=Notice: This permanently removes files in the default capture folders and cannot be undone.
chinesesimplified.ContinueUninstall=继续卸载
english.ContinueUninstall=Continue Uninstall
chinesesimplified.AutoStartProgram=开机自动启动 Tools3000
english.AutoStartProgram=Start Tools3000 automatically on Windows startup
chinesesimplified.TypeFull=完整体验安装 (推荐 · 默认启用全部 7 大核心模块)
english.TypeFull=Full Installation (Recommended - All 7 Core Modules Enabled)
chinesesimplified.TypeCompact=极简轻量安装
english.TypeCompact=Compact Installation
chinesesimplified.TypeCustom=自定义模块选择
english.TypeCustom=Custom Module Selection
chinesesimplified.CompSearch=超级文件检索 (Search) — 全盘秒级索引与极速文件启动
english.CompSearch=Fast File Search (Search) — Instant disk indexing & launcher
chinesesimplified.CompCapture=截图贴图与录屏 (Capture) — 智能贴图、长截图、拾色器与高清录屏
english.CompCapture=Screenshot, Pin & Recording (Capture) — Smart pin, OCR & HD recording
chinesesimplified.CompGesture=鼠标手势与触发角 (Gesture) — 右键手势轨迹、屏幕四角触发与轮盘菜单
english.CompGesture=Mouse Gestures & Hot Corners (Gesture) — Trailing gestures, hot corners & radial menu
chinesesimplified.CompKeycast=按键回显 (Keycast) — 屏幕实时按键显示、机械键帽动效
english.CompKeycast=Keycast Overlay (Keycast) — Real-time keystroke visualization
chinesesimplified.CompDialog=文件对话框增强 (Dialog Enhancer) — 常用目录快速跳转、历史路径记忆
english.CompDialog=File Dialog Enhancer (Dialog) — Quick folders & path memory
chinesesimplified.CompSpotlight=演示专用特效 (Spotlight) — 屏幕聚光灯聚焦、点击水波纹与流光轨迹
english.CompSpotlight=Presentation FX (Spotlight) — Screen spotlight focus, click ripple & mouse trails
chinesesimplified.CompRemote=远程协助增强 (Remote Boost) — 主控端热键直通、修饰键急救冲刷与输入法脱敏
english.CompRemote=Remote Boost (Remote) — Immersive hotkey tunnel, emergency flush & smart IME sanitizing

[Types]
Name: "full"; Description: "{cm:TypeFull}"
Name: "compact"; Description: "{cm:TypeCompact}"
Name: "custom"; Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
Name: "search"; Description: "{cm:CompSearch}"; Types: full
Name: "capture"; Description: "{cm:CompCapture}"; Types: full
Name: "gesture"; Description: "{cm:CompGesture}"; Types: full
Name: "keycast"; Description: "{cm:CompKeycast}"; Types: full
Name: "dialogenhancer"; Description: "{cm:CompDialog}"; Types: full
Name: "spotlight"; Description: "{cm:CompSpotlight}"; Types: full
Name: "remote"; Description: "{cm:CompRemote}"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "{cm:AutoStartProgram}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 核心可执行文件与全量动态链接库（显式配置 restartreplace 容错标记，即使极端占用下亦由系统在重启时替换，绝不弹出“拒绝访问”弹窗打断安装）
Source: "deploy_dist\*.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "deploy_dist\plugins\*.dll"; DestDir: "{app}\plugins"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "deploy_dist\Tools3000.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "deploy_dist\Tools3000_Service.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
; 其余全量资源与依赖文件（保持递归与 restartreplace 双保险，排除已明确声明的 DLL/EXE 避免二次拷贝冲突）
Source: "deploy_dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace uninsrestartdelete; Excludes: "*.pdb,Tools3000Tests.*,*Preview.*,*Integration.*,*.flag,debug.flag,*.dll,Tools3000.exe,Tools3000_Service.exe"

[InstallDelete]

[UninstallDelete]
Type: files; Name: "{autodesktop}\Tools3000.lnk"
Type: files; Name: "{userdesktop}\Tools3000.lnk"
Type: files; Name: "{commondesktop}\Tools3000.lnk"
Type: filesandordirs; Name: "{autoprograms}\Tools3000"
Type: filesandordirs; Name: "{userprograms}\Tools3000"
Type: filesandordirs; Name: "{commonprograms}\Tools3000"
Type: files; Name: "{app}\initial_modules.json"
Type: files; Name: "{app}\*.log"
Type: dirifempty; Name: "{app}\plugins"
Type: dirifempty; Name: "{app}\resources\scripts"
Type: dirifempty; Name: "{app}\resources"
Type: dirifempty; Name: "{app}\ui\assets"
Type: dirifempty; Name: "{app}\ui"
Type: dirifempty; Name: "{app}"

[Icons]
Name: "{group}\Tools3000"; Filename: "{app}\Tools3000.exe"; AppUserModelID: "Yy1.Tools3000"
Name: "{group}\{cm:UninstallProgram,Tools3000}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Tools3000"; Filename: "{app}\Tools3000.exe"; Tasks: desktopicon; AppUserModelID: "Yy1.Tools3000"

[Run]
; 服务只负责承载按需索引。安装、系统启动和 WebView 预加载都不得启动它；
; SearchWindow::show() 在用户通过快捷键、托盘或设置页按钮主动唤起时发送 search.warmup。
Filename: "{sys}\sc.exe"; Parameters: "create Tools3000_SearchService binPath= ""{app}\Tools3000_Service.exe"" start= demand DisplayName= ""Tools3000 Search Service"""; Flags: runhidden waituntilterminated; StatusMsg: "{cm:InstallingService}"; Check: IsSearchServiceInstallNeeded
Filename: "{sys}\sc.exe"; Parameters: "config Tools3000_SearchService binPath= ""{app}\Tools3000_Service.exe"" start= demand DisplayName= ""Tools3000 Search Service"""; Flags: runhidden waituntilterminated; Check: IsSearchServiceConfigNeeded
Filename: "{sys}\sc.exe"; Parameters: "description Tools3000_SearchService ""Tools3000 本地文件快速搜索索引"""; Flags: runhidden waituntilterminated; Check: IsSearchComponentSelected
Filename: "{app}\Tools3000.exe"; Description: "{cm:LaunchProgram,Tools3000}"; Flags: nowait postinstall skipifsilent


[Code]
var
  DetailsButton: TNewButton;
  DetailsMemo: TNewMemo;
  ExtractTimerId: LongWord;
  TimerCallbackAddr: LongWord;
  LastExtractedFile: String;
  DeleteSettingsHistory: Boolean;
  DeleteDiagnostics: Boolean;
  DeleteCachesIndexes: Boolean;
  DeleteCaptures: Boolean;

function SetTimer(hWnd: LongWord; nIDEvent, uElapse: LongWord; lpTimerFunc: LongWord): LongWord;
  external 'SetTimer@user32.dll stdcall';
function KillTimer(hWnd: LongWord; uIDEvent: LongWord): Boolean;
  external 'KillTimer@user32.dll stdcall';
procedure SHChangeNotify(wEventId: LongInt; uFlags: Cardinal; dwItem1, dwItem2: Cardinal);
  external 'SHChangeNotify@shell32.dll stdcall';

const
  SHCNE_ASSOCCHANGED = $08000000;
  SHCNF_IDLIST = $0000;
  GENERIC_READ_FLAG = $80000000;
  GENERIC_WRITE_FLAG = $40000000;
  OPEN_EXISTING_FLAG = 3;
  WM_CLOSE_MSG = $0010;

function CreateFile(
  lpFileName: String;
  dwDesiredAccess: Cardinal;
  dwShareMode: Cardinal;
  lpSecurityAttributes: Cardinal;
  dwCreationDisposition: Cardinal;
  dwFlagsAndAttributes: Cardinal;
  hTemplateFile: THandle
): THandle;
  external 'CreateFileW@kernel32.dll stdcall';

function CloseHandle(hObject: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function PostMessage(hWnd: LongWord; Msg, wParam: LongWord; lParam: LongInt): Boolean;
  external 'PostMessageW@user32.dll stdcall';

function GetTickCount(): DWORD;
  external 'GetTickCount@kernel32.dll stdcall';

const
  SC_MANAGER_CONNECT = $0001;
  SC_MANAGER_ALL_ACCESS = $F003F;
  SERVICE_QUERY_STATUS = $0004;
  SERVICE_STOP = $0020;
  DELETE_ACCESS = $00010000;
  SERVICE_CONTROL_STOP = 1;
  ERROR_SHARING_VIOLATION = 32;
  ERROR_LOCK_VIOLATION = 33;

type
  TServiceStatus = record
    dwServiceType: DWORD;
    dwCurrentState: DWORD;
    dwControlsAccepted: DWORD;
    dwWin32ExitCode: DWORD;
    dwServiceSpecificExitCode: DWORD;
    dwCheckPoint: DWORD;
    dwWaitHint: DWORD;
  end;

function OpenSCManager(lpMachineName, lpDatabaseName: String; dwDesiredAccess: DWORD): THandle;
  external 'OpenSCManagerW@advapi32.dll stdcall';
function OpenService(hSCManager: THandle; lpServiceName: String; dwDesiredAccess: DWORD): THandle;
  external 'OpenServiceW@advapi32.dll stdcall';
function QueryServiceStatus(hService: THandle; var lpServiceStatus: TServiceStatus): BOOL;
  external 'QueryServiceStatus@advapi32.dll stdcall';
function ControlService(hService: THandle; dwControl: DWORD; var lpServiceStatus: TServiceStatus): BOOL;
  external 'ControlService@advapi32.dll stdcall';
function DeleteService(hService: THandle): BOOL;
  external 'DeleteService@advapi32.dll stdcall';
function CloseServiceHandle(hSCObject: THandle): BOOL;
  external 'CloseServiceHandle@advapi32.dll stdcall';
function GetLastError(): DWORD;
  external 'GetLastError@kernel32.dll stdcall';

function ServiceExists(): Boolean; forward;

function IsFileLocked(const Filename: String): Boolean;
var
  H: THandle;
  Err: DWORD;
begin
  Result := False;
  if not FileExists(Filename) then Exit;
  H := CreateFile(Filename, GENERIC_READ_FLAG or GENERIC_WRITE_FLAG, 0, 0, OPEN_EXISTING_FLAG, FILE_ATTRIBUTE_NORMAL, 0);
  if (H = -1) or (H = 0) or (H = 4294967295) then
  begin
    Err := GetLastError();
    // 仅当底层返回共享冲突或文件锁冲突时确认为活跃进程独占占用，杜绝只读属性或权限误报
    Result := (Err = ERROR_SHARING_VIOLATION) or (Err = ERROR_LOCK_VIOLATION);
  end
  else
    CloseHandle(H);
end;

function CheckDirFilesLocked(const DirPath: String): Boolean;
var
  FindRec: TFindRec;
  Ext, FileNameLower, UninstallerName: String;
begin
  Result := False;
  if not DirExists(DirPath) then Exit;

  UninstallerName := Lowercase(ExtractFileName(ExpandConstant('{uninstallexe}')));

  if FindFirst(DirPath + '\*.*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
        begin
          FileNameLower := Lowercase(FindRec.Name);
          // 绝对排除卸载程序自身与其数据文件 (unins*)，彻底杜绝自锁误报与假死超时
          if (Pos('unins', FileNameLower) = 1) or ((UninstallerName <> '') and (FileNameLower = UninstallerName)) then
            Continue;

          Ext := Lowercase(ExtractFileExt(FindRec.Name));
          if (Ext = '.dll') or (Ext = '.exe') then
          begin
            if IsFileLocked(DirPath + '\' + FindRec.Name) then
            begin
              Log(Format('CheckDirFilesLocked: File is locked -> %s\%s', [DirPath, FindRec.Name]));
              Result := True;
              Exit;
            end;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function AreAppFilesLocked(): Boolean;
var
  AppDir: String;
begin
  AppDir := ExpandConstant('{app}');
  Result := CheckDirFilesLocked(AppDir) or CheckDirFilesLocked(AppDir + '\plugins');
end;

function AreCriticalDllsLocked(): Boolean;
var
  AppDir: String;
begin
  AppDir := ExpandConstant('{app}');
  // 关键音视频处理库、核心框架库与主可执行程序优先精准探测
  if IsFileLocked(AppDir + '\avcodec-62.dll') or
     IsFileLocked(AppDir + '\avformat-62.dll') or
     IsFileLocked(AppDir + '\avutil-60.dll') or
     IsFileLocked(AppDir + '\swresample-6.dll') or
     IsFileLocked(AppDir + '\swscale-9.dll') or
     IsFileLocked(AppDir + '\Tools3000Core.dll') or
     IsFileLocked(AppDir + '\Tools3000.exe') or
     IsFileLocked(AppDir + '\Tools3000_Service.exe') then
  begin
    Result := True;
    Exit;
  end;
  // 遍历 app 及 plugins 目录全量动态库
  Result := AreAppFilesLocked();
end;

function IsTools3000Running(): Boolean;
begin
  // 1. 全局单实例互斥体秒级探测 (0.001ms)：Tools3000 进程存活之唯一底层真相
  if CheckForMutexes('Global\Tools3000_SingleInstance_Mutex') or
     CheckForMutexes('Tools3000_SingleInstance_Mutex') then
  begin
    Result := True;
    Exit;
  end;

  // 2. 隐藏消息窗口句柄快速探测 (0.001ms)
  if FindWindowByClassName('Tools3000_MessageWindow') <> 0 then
  begin
    Result := True;
    Exit;
  end;

  Result := False;
end;

function IsSearchServiceRunning(): Boolean;
var
  hSCM, hSvc: THandle;
  Status: TServiceStatus;
begin
  Result := False;

  // SCM 内核状态秒级直查 (0.01ms)：免创建任何进程，直连服务控制管理器断言
  hSCM := OpenSCManager('', '', SC_MANAGER_CONNECT);
  if hSCM <> 0 then
  begin
    try
      hSvc := OpenService(hSCM, 'Tools3000_SearchService', SERVICE_QUERY_STATUS);
      if hSvc <> 0 then
      begin
        try
          if QueryServiceStatus(hSvc, Status) then
          begin
            if (Status.dwCurrentState = 4 { SERVICE_RUNNING }) or
               (Status.dwCurrentState = 2 { SERVICE_START_PENDING }) or
               (Status.dwCurrentState = 3 { SERVICE_STOP_PENDING }) then
            begin
              Result := True;
              Exit;
            end;
          end;
        finally
          CloseServiceHandle(hSvc);
        end;
      end;
    finally
      CloseServiceHandle(hSCM);
    end;
  end;
end;

procedure StopSearchServiceNative();
var
  hSCM, hSvc: THandle;
  Status: TServiceStatus;
  Dummy: Integer;
begin
  hSCM := OpenSCManager('', '', SC_MANAGER_CONNECT);
  if hSCM <> 0 then
  begin
    try
      hSvc := OpenService(hSCM, 'Tools3000_SearchService', SERVICE_STOP or SERVICE_QUERY_STATUS);
      if hSvc <> 0 then
      begin
        try
          ControlService(hSvc, SERVICE_CONTROL_STOP, Status);
          Log('StopSearchServiceNative: Sent SERVICE_CONTROL_STOP via Win32 SCM API.');
        finally
          CloseServiceHandle(hSvc);
        end;
      end;
    finally
      CloseServiceHandle(hSCM);
    end;
  end;
  // 仅当服务仍在运行或文件锁定时才执行兜底 taskkill，绝不在已停止状态下空转创建外部进程
  if IsSearchServiceRunning() or IsFileLocked(ExpandConstant('{app}\Tools3000_Service.exe')) then
  begin
    Log('StopSearchServiceNative: Service process still active, issuing taskkill...');
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Service.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
  end;
end;

procedure DeleteSearchServiceNative();
var
  hSCM, hSvc: THandle;
  Status: TServiceStatus;
  Dummy: Integer;
begin
  hSCM := OpenSCManager('', '', SC_MANAGER_CONNECT);
  if hSCM <> 0 then
  begin
    try
      hSvc := OpenService(hSCM, 'Tools3000_SearchService', SERVICE_STOP or DELETE_ACCESS or SERVICE_QUERY_STATUS);
      if hSvc <> 0 then
      begin
        try
          if QueryServiceStatus(hSvc, Status) then
          begin
            if Status.dwCurrentState <> 1 { SERVICE_STOPPED } then
              ControlService(hSvc, SERVICE_CONTROL_STOP, Status);
          end;
          if DeleteService(hSvc) then
            Log('DeleteSearchServiceNative: Search service stopped and marked for deletion via Win32 SCM API.')
          else
            Log(Format('DeleteSearchServiceNative: DeleteService returned false, code=%d', [GetLastError()]));
        finally
          CloseServiceHandle(hSvc);
        end;
      end;
    finally
      CloseServiceHandle(hSCM);
    end;
  end;
  // 仅当服务仍在运行或文件锁定时才执行兜底 taskkill，绝不在已停止状态下空转创建外部进程
  if IsSearchServiceRunning() or IsFileLocked(ExpandConstant('{app}\Tools3000_Service.exe')) then
  begin
    Log('DeleteSearchServiceNative: Service process still active, issuing taskkill fallback...');
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Service.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
  end;
end;

procedure CleanScheduledTasksAndRegistries();
var
  TaskService, RootFolder, ToolsFolder, Tasks, TaskItem: Variant;
  I, Dummy: Integer;
  ComSuccess: Boolean;
begin
  ComSuccess := False;
  try
    TaskService := CreateOleObject('Schedule.Service');
    TaskService.Connect();
    try
      ToolsFolder := TaskService.GetFolder('\Tools3000');
      Tasks := ToolsFolder.GetTasks(0);
      for I := Tasks.Count downto 1 do
      begin
        TaskItem := Tasks.Item(I);
        ToolsFolder.DeleteTask(TaskItem.Name, 0);
      end;
      RootFolder := TaskService.GetFolder('\');
      RootFolder.DeleteFolder('Tools3000', 0);
      ComSuccess := True;
      Log('CleanScheduledTasksAndRegistries: Cleaned all tasks in Tools3000 folder via COM.');
    except
      ComSuccess := True;
      Log('CleanScheduledTasksAndRegistries: No \Tools3000 folder found in Task Scheduler.');
    end;
  except
    Log('CleanScheduledTasksAndRegistries: Schedule.Service COM initialization failed, falling back to schtasks.');
  end;

  if not ComSuccess then
  begin
    Exec(ExpandConstant('{sys}\schtasks.exe'), '/delete /tn "Tools3000\Autorun for ' + GetUserNameString() + '" /f', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
  end;

  // 清除注册表当前用户自启动项 (HKCU Run)
  RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Tools3000');
end;

procedure StopAndKillAllTools3000Processes();
var
  ResultCode: Integer;
  MsgHwnd: LongWord;
  NeedKillApp: Boolean;
  NeedKillService: Boolean;
begin
  NeedKillApp := IsTools3000Running();
  NeedKillService := IsSearchServiceRunning();

  // 若均未运行且关键文件未锁定，毫秒级直接跳过，杜绝空转执行 taskkill/sc 造成的 500ms~1000ms 进程创建开销
  if (not NeedKillApp) and (not NeedKillService) and (not AreAppFilesLocked()) then
  begin
    Log('StopAndKillAllTools3000Processes: No running Tools3000 instances or file locks detected, skipping process kills.');
    Exit;
  end;

  Log('StopAndKillAllTools3000Processes: Initiating graceful and forceful termination of active processes and services...');

  if NeedKillApp then
  begin
    MsgHwnd := FindWindowByClassName('Tools3000_MessageWindow');
    if MsgHwnd <> 0 then
    begin
      PostMessage(MsgHwnd, WM_CLOSE_MSG, 0, 0);
      Sleep(20);
    end;
    // 强杀主程序进程树（包含子 WebView2 进程）
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;

  if NeedKillService then
  begin
    StopSearchServiceNative();
  end;
end;

function WaitForProcessesAndFilesReleased(MaxWaitMs: Integer): Boolean;
var
  StartTick: DWORD;
  Elapsed: DWORD;
  Dummy: Integer;
begin
  Result := False;
  StartTick := GetTickCount();

  while True do
  begin
    if (not IsTools3000Running()) and
       (not IsSearchServiceRunning()) and
       (not AreAppFilesLocked()) then
    begin
      Elapsed := GetTickCount() - StartTick;
      Log(Format('WaitForProcessesAndFilesReleased: All processes exited and file locks released in %d ms', [Elapsed]));
      Result := True;
      Exit;
    end;

    Elapsed := GetTickCount() - StartTick;
    if Elapsed >= DWORD(MaxWaitMs) then
      Break;

    Sleep(30);

    // 超过 500ms 仍未释放时重试一次 taskkill 确保彻底干掉残留进程
    if (Elapsed > 500) and ((Elapsed mod 500) < 40) then
    begin
      Log('WaitForProcessesAndFilesReleased: Locks or processes still active, re-issuing taskkill /f /t...');
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Service.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
    end;
  end;

  Result := (not IsTools3000Running()) and
            (not IsSearchServiceRunning()) and
            (not AreAppFilesLocked());
  Elapsed := GetTickCount() - StartTick;
  if Result then
    Log(Format('WaitForProcessesAndFilesReleased: Released at final check (%d ms)', [Elapsed]))
  else
    Log(Format('WaitForProcessesAndFilesReleased: Timed out after %d ms. Files or processes may still be locked.', [Elapsed]));
end;


procedure OnExtractTimer(hWnd: LongWord; uMsg: LongWord; idEvent: LongWord; dwTime: LongWord);
var
  CurFile: String;
begin
  CurFile := WizardForm.FilenameLabel.Caption;
  if (CurFile <> '') and (CurFile <> LastExtractedFile) then
  begin
    LastExtractedFile := CurFile;
    DetailsMemo.Lines.Add(CurFile);
  end;
end;

procedure DetailsButtonClick(Sender: TObject);
begin
  DetailsMemo.Visible := not DetailsMemo.Visible;
  if DetailsMemo.Visible then
    DetailsButton.Caption := CustomMessage('HideDetails')
  else
    DetailsButton.Caption := CustomMessage('ShowDetails');
end;

procedure ApplyComponentsListStyles();
begin
  { 世界级组件列表高分屏与呼吸感重构：行高38px + 左内边距16px，彻底杜绝复选框裁剪 }
  WizardForm.ComponentsList.MinItemHeight := ScaleY(38);
  WizardForm.ComponentsList.Offset := ScaleX(16);
  WizardForm.ComponentsList.ShowLines := False;
  WizardForm.ComponentsList.Font.Name := 'Microsoft YaHei UI';
  WizardForm.ComponentsList.Font.Size := 9;

  WizardForm.TasksList.MinItemHeight := ScaleY(34);
  WizardForm.TasksList.Offset := ScaleX(16);
  WizardForm.TasksList.ShowLines := False;
  WizardForm.TasksList.Font.Name := 'Microsoft YaHei UI';
  WizardForm.TasksList.Font.Size := 9;
end;

function AutoStartTaskExists(): Boolean; forward;

procedure InitializeWizard();
begin
  ApplyComponentsListStyles();
  WizardForm.TasksList.ShowLines := False;

  // 恢复自启动任务勾选状态：若系统中已存在自启动任务或注册表项，自动保持勾选！
  if AutoStartTaskExists() then
  begin
    WizardSelectTasks('autostart');
    Log('InitializeWizard: Existing AutoStart task/registry detected, preserved autostart selection');
  end;

  // 创建详细信息展开/收起按钮
  DetailsButton := TNewButton.Create(WizardForm);
  DetailsButton.Parent := WizardForm.InstallingPage;
  DetailsButton.Left := WizardForm.ProgressGauge.Left;
  DetailsButton.Top := WizardForm.ProgressGauge.Top + WizardForm.ProgressGauge.Height + ScaleY(10);
  DetailsButton.Width := ScaleX(95);
  DetailsButton.Height := ScaleY(24);
  DetailsButton.Caption := CustomMessage('ShowDetails');
  DetailsButton.OnClick := @DetailsButtonClick;

  // 创建详细信息文本日志框
  DetailsMemo := TNewMemo.Create(WizardForm);
  DetailsMemo.Parent := WizardForm.InstallingPage;
  DetailsMemo.Left := WizardForm.ProgressGauge.Left;
  DetailsMemo.Top := DetailsButton.Top + DetailsButton.Height + ScaleY(8);
  DetailsMemo.Width := WizardForm.ProgressGauge.Width;
  DetailsMemo.Height := WizardForm.InstallingPage.Height - DetailsMemo.Top - ScaleY(4);
  DetailsMemo.ReadOnly := True;
  DetailsMemo.ScrollBars := ssVertical;
  DetailsMemo.Font.Name := 'Consolas';
  DetailsMemo.Font.Size := 8;
  DetailsMemo.Visible := False;

  TimerCallbackAddr := CreateCallback(@OnExtractTimer);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpSelectComponents) or (CurPageID = wpSelectTasks) then
  begin
    ApplyComponentsListStyles();
  end;
  if CurPageID = wpInstalling then
  begin
    if (ExtractTimerId = 0) and (TimerCallbackAddr <> 0) then
      ExtractTimerId := SetTimer(0, 0, 30, TimerCallbackAddr);
  end
  else
  begin
    if ExtractTimerId <> 0 then
    begin
      KillTimer(0, ExtractTimerId);
      ExtractTimerId := 0;
    end;
  end;
end;

procedure DeinitializeSetup();
begin
  if ExtractTimerId <> 0 then
  begin
    KillTimer(0, ExtractTimerId);
    ExtractTimerId := 0;
  end;
end;

function ServiceExists(): Boolean;
begin
  Result := RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\Tools3000_SearchService');
end;

function IsSearchComponentSelected(): Boolean;
begin
  Result := WizardIsComponentSelected('search');
end;

function IsSearchServiceInstallNeeded(): Boolean;
begin
  Result := IsSearchComponentSelected() and (not ServiceExists());
end;

function IsSearchServiceConfigNeeded(): Boolean;
begin
  { 升级时也要把旧版 auto 服务迁移为 demand，即使用户未勾选搜索组件。 }
  { 否则遗留服务仍会在每次 Windows 启动时常驻。 }
  Result := ServiceExists();
end;

function AutoStartTaskExists(): Boolean;
var
  ResultCode: Integer;
begin
  // 检测当前用户专属规范任务 (Tools3000\Autorun for <User>)
  Result := Exec(ExpandConstant('{sys}\schtasks.exe'),
    '/query /tn "Tools3000\Autorun for ' + GetUserNameString() + '"', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  if Result then Exit;

  // 检测注册表 Run 键 (HKCU Run 键)
  Result := RegValueExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Tools3000');
end;

procedure CreateAutoStartTask();
var
  ResultCode: Integer;
  TargetExe: String;
begin
  TargetExe := ExpandConstant('{app}\Tools3000.exe');

  // 直接委托原生注册，100% 保证安装包与软件设置页同源、同逻辑、同配置
  if Exec(TargetExe, '--register-autostart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    Log('Registered AutoStart task via native COM API')
  else
    Log(Format('Failed to register AutoStart task, exit code %d', [ResultCode]));
end;

procedure RemoveAutoStartTask();
var
  ResultCode: Integer;
begin
  if FileExists(ExpandConstant('{app}\Tools3000.exe')) then
    Exec(ExpandConstant('{app}\Tools3000.exe'), '--unregister-autostart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\schtasks.exe'), '/delete /tn "Tools3000\Autorun for ' + GetUserNameString() + '" /f', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure SyncInitialModuleConfig();
var
  AppDir, InitialModulesPath: String;
  SearchSel, CaptureSel, GestureSel, KeycastSel, DialogSel, SpotlightSel, RemoteSel: Boolean;
  SearchStr, CaptureStr, GestureStr, KeycastStr, DialogStr, SpotlightStr, RemoteStr: String;
  JsonContent: String;
begin
  AppDir := ExpandConstant('{app}');
  InitialModulesPath := AppDir + '\initial_modules.json';

  SearchSel := WizardIsComponentSelected('search');
  CaptureSel := WizardIsComponentSelected('capture');
  GestureSel := WizardIsComponentSelected('gesture');
  KeycastSel := WizardIsComponentSelected('keycast');
  DialogSel := WizardIsComponentSelected('dialogenhancer');
  SpotlightSel := WizardIsComponentSelected('spotlight');
  RemoteSel := WizardIsComponentSelected('remote');

  if SearchSel then SearchStr := 'true' else SearchStr := 'false';
  if CaptureSel then CaptureStr := 'true' else CaptureStr := 'false';
  if GestureSel then GestureStr := 'true' else GestureStr := 'false';
  if KeycastSel then KeycastStr := 'true' else KeycastStr := 'false';
  if DialogSel then DialogStr := 'true' else DialogStr := 'false';
  if SpotlightSel then SpotlightStr := 'true' else SpotlightStr := 'false';
  if RemoteSel then RemoteStr := 'true' else RemoteStr := 'false';

  JsonContent :=
    '{' + #13#10 +
    '  "plugins": {' + #13#10 +
    '    "search": { "enabled": ' + SearchStr + ' },' + #13#10 +
    '    "capture": { "enabled": ' + CaptureStr + ' },' + #13#10 +
    '    "gesture": { "enabled": ' + GestureStr + ' },' + #13#10 +
    '    "keycast": { "enabled": ' + KeycastStr + ' },' + #13#10 +
    '    "dialogenhancer": { "enabled": ' + DialogStr + ' },' + #13#10 +
    '    "remote_boost": { "enabled": ' + RemoteStr + ' }' + #13#10 +
    '  },' + #13#10 +
    '  "search": {' + #13#10 +
    '    "enabled": ' + SearchStr + ',' + #13#10 +
    '    "residentInBackground": true' + #13#10 +
    '  },' + #13#10 +
    '  "gesture": { "enabled": ' + GestureStr + ' },' + #13#10 +
    '  "dialog": { "enabled": ' + DialogStr + ' },' + #13#10 +
    '  "spotlight": { "enabled": ' + SpotlightStr + ' },' + #13#10 +
    '  "remote_boost": {' + #13#10 +
    '    "enabled": ' + RemoteStr + '' + #13#10 +
    '  },' + #13#10 +
    '  "general": {' + #13#10 +
    '    "keycastEnabled": ' + KeycastStr + '' + #13#10 +
    '  }' + #13#10 +
    '}';

  SaveStringToFile(InitialModulesPath, JsonContent, False);
  Log('SyncInitialModuleConfig: Wrote initial_modules.json -> ' + InitialModulesPath);
end;

procedure UpdateShortcuts();
var
  ResultCode: Integer;
  TargetExe: String;
begin
  TargetExe := ExpandConstant('{app}\Tools3000.exe');

  if Exec(TargetExe, '--update-shortcuts', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Log('Updated shortcuts and AUMID via native COM API')
  else
    Log(Format('Failed to update shortcuts, exit code %d', [ResultCode]));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    SyncInitialModuleConfig();
    UpdateShortcuts();
    if WizardIsTaskSelected('autostart') then
      CreateAutoStartTask()
    else if not WizardSilent then
      RemoveAutoStartTask()
    else if not AutoStartTaskExists() then
      RemoveAutoStartTask();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
  end;
end;

var
  StandardRadio, CleanRadio: TRadioButton;
  DeleteCapturesCheck: TCheckBox;
  DeleteCapturesNote: TNewStaticText;

procedure OnStandardModeClick(Sender: TObject);
begin
  StandardRadio.Checked := True;
  CleanRadio.Checked := False;
  DeleteCapturesCheck.Enabled := False;
  DeleteCapturesCheck.Checked := False;
  DeleteCapturesNote.Enabled := False;
end;

procedure OnCleanModeClick(Sender: TObject);
begin
  CleanRadio.Checked := True;
  StandardRadio.Checked := False;
  DeleteCapturesCheck.Enabled := True;
  DeleteCapturesNote.Enabled := True;
end;

function ShowPersonalDataOptions(): Boolean;
var
  Form: TSetupForm;
  Heading, Intro: TNewStaticText;
  StandardDesc, CleanDesc: TNewStaticText;
  FooterLine: TBevel;
  OKButton, CancelButton: TNewButton;
  ButtonWidth, ContentLeft, ContentWidth, IndentLeft, SubIndentLeft, BottomContentY: Integer;
begin
  Result := True;
  if UninstallSilent then
    Exit;

  // 创建现代精致紧凑对话框 (宽度 520px)
  Form := CreateCustomForm(ScaleX(520), ScaleY(380), True, True);
  try
    Form.Caption := ExpandConstant('{cm:UninstallProgram,Tools3000}');
    Form.Position := poScreenCenter;

    ContentLeft := ScaleX(28);
    ContentWidth := Form.ClientWidth - ScaleX(56);
    IndentLeft := ContentLeft + ScaleX(22);
    SubIndentLeft := ContentLeft + ScaleX(42);

    // 1. 头部主标题 (11pt Bold 黑色)
    Heading := TNewStaticText.Create(Form);
    Heading.Parent := Form;
    Heading.Left := ContentLeft;
    Heading.Top := ScaleY(22);
    Heading.Width := ContentWidth;
    Heading.Height := ScaleY(24);
    Heading.AutoSize := False;
    Heading.Caption := CustomMessage('PersonalDataTitle');
    Heading.Font.Style := [fsBold];
    Heading.Font.Size := 11;

    // 2. 引言说明 (次级灰色文本)
    Intro := TNewStaticText.Create(Form);
    Intro.Parent := Form;
    Intro.Left := ContentLeft;
    Intro.Top := Heading.Top + Heading.Height + ScaleY(2);
    Intro.Width := ContentWidth;
    Intro.Height := ScaleY(20);
    Intro.AutoSize := False;
    Intro.Caption := CustomMessage('PersonalDataDescription');
    Intro.Font.Color := clGrayText;

    // 3. 选项一：保留个人配置与媒体 (推荐)
    StandardRadio := TRadioButton.Create(Form);
    StandardRadio.Parent := Form;
    StandardRadio.Left := ContentLeft;
    StandardRadio.Top := Intro.Top + Intro.Height + ScaleY(16);
    StandardRadio.Width := ContentWidth;
    StandardRadio.Height := ScaleY(22);
    StandardRadio.Caption := CustomMessage('UninstallModeStandard');
    StandardRadio.Font.Style := [fsBold];
    StandardRadio.Checked := True;
    StandardRadio.OnClick := @OnStandardModeClick;

    StandardDesc := TNewStaticText.Create(Form);
    StandardDesc.Parent := Form;
    StandardDesc.Left := IndentLeft;
    StandardDesc.Top := StandardRadio.Top + StandardRadio.Height + ScaleY(3);
    StandardDesc.Width := ContentWidth - ScaleX(22);
    StandardDesc.Height := ScaleY(34);
    StandardDesc.AutoSize := False;
    StandardDesc.WordWrap := True;
    StandardDesc.Caption := CustomMessage('UninstallModeStandardDesc');
    StandardDesc.Font.Color := clGrayText;
    StandardDesc.OnClick := @OnStandardModeClick;

    // 4. 选项二：清理全部配置与运行缓存
    CleanRadio := TRadioButton.Create(Form);
    CleanRadio.Parent := Form;
    CleanRadio.Left := ContentLeft;
    CleanRadio.Top := StandardDesc.Top + StandardDesc.Height + ScaleY(14);
    CleanRadio.Width := ContentWidth;
    CleanRadio.Height := ScaleY(22);
    CleanRadio.Caption := CustomMessage('UninstallModeClean');
    CleanRadio.Font.Style := [fsBold];
    CleanRadio.Checked := False;
    CleanRadio.OnClick := @OnCleanModeClick;

    CleanDesc := TNewStaticText.Create(Form);
    CleanDesc.Parent := Form;
    CleanDesc.Left := IndentLeft;
    CleanDesc.Top := CleanRadio.Top + CleanRadio.Height + ScaleY(3);
    CleanDesc.Width := ContentWidth - ScaleX(22);
    CleanDesc.Height := ScaleY(32);
    CleanDesc.AutoSize := False;
    CleanDesc.WordWrap := True;
    CleanDesc.Caption := CustomMessage('UninstallModeCleanDesc');
    CleanDesc.Font.Color := clGrayText;
    CleanDesc.OnClick := @OnCleanModeClick;

    // 进阶复选框与警示说明
    DeleteCapturesCheck := TCheckBox.Create(Form);
    DeleteCapturesCheck.Parent := Form;
    DeleteCapturesCheck.Left := IndentLeft;
    DeleteCapturesCheck.Top := CleanDesc.Top + CleanDesc.Height + ScaleY(8);
    DeleteCapturesCheck.Width := ContentWidth - ScaleX(22);
    DeleteCapturesCheck.Height := ScaleY(22);
    DeleteCapturesCheck.Caption := CustomMessage('DeleteMediaCaptures');
    DeleteCapturesCheck.Checked := False;
    DeleteCapturesCheck.Enabled := False;

    DeleteCapturesNote := TNewStaticText.Create(Form);
    DeleteCapturesNote.Parent := Form;
    DeleteCapturesNote.Left := SubIndentLeft;
    DeleteCapturesNote.Top := DeleteCapturesCheck.Top + DeleteCapturesCheck.Height + ScaleY(2);
    DeleteCapturesNote.Width := ContentWidth - ScaleX(42);
    DeleteCapturesNote.Height := ScaleY(24);
    DeleteCapturesNote.AutoSize := False;
    DeleteCapturesNote.WordWrap := True;
    DeleteCapturesNote.Caption := CustomMessage('DeleteMediaCapturesNote');
    DeleteCapturesNote.Font.Color := $002020B0; // 柔和深红/暗红警示
    DeleteCapturesNote.Enabled := False;

    // 5. 动态精确自适应窗口高度，杜绝多余空旷空白
    BottomContentY := DeleteCapturesNote.Top + DeleteCapturesNote.Height;
    Form.ClientHeight := BottomContentY + ScaleY(62);

    // 6. 底部柔和分割线与操作按钮
    FooterLine := TBevel.Create(Form);
    FooterLine.Parent := Form;
    FooterLine.Left := 0;
    FooterLine.Top := Form.ClientHeight - ScaleY(50);
    FooterLine.Width := Form.ClientWidth;
    FooterLine.Height := ScaleY(1);
    FooterLine.Shape := bsTopLine;

    OKButton := TNewButton.Create(Form);
    OKButton.Parent := Form;
    OKButton.Caption := CustomMessage('ContinueUninstall');
    OKButton.Top := Form.ClientHeight - ScaleY(38);
    OKButton.Height := ScaleY(28);
    OKButton.ModalResult := mrOk;
    OKButton.Default := True;

    CancelButton := TNewButton.Create(Form);
    CancelButton.Parent := Form;
    CancelButton.Caption := SetupMessage(msgButtonCancel);
    CancelButton.Top := OKButton.Top;
    CancelButton.Height := OKButton.Height;
    CancelButton.ModalResult := mrCancel;
    CancelButton.Cancel := True;

    ButtonWidth := Form.CalculateButtonWidth([OKButton.Caption, CancelButton.Caption]);
    OKButton.Width := ButtonWidth;
    CancelButton.Width := ButtonWidth;
    CancelButton.Left := Form.ClientWidth - ButtonWidth - ScaleX(20);
    OKButton.Left := CancelButton.Left - ButtonWidth - ScaleX(10);

    Result := Form.ShowModal() = mrOk;
    if Result then
    begin
      if StandardRadio.Checked then
      begin
        DeleteSettingsHistory := False;
        DeleteDiagnostics := True;
        DeleteCachesIndexes := True;
        DeleteCaptures := False;
      end
      else
      begin
        DeleteSettingsHistory := True;
        DeleteDiagnostics := True;
        DeleteCachesIndexes := True;
        DeleteCaptures := DeleteCapturesCheck.Checked;
      end;
    end;
  finally
    Form.Free();
  end;
end;

function HasCommandLineParameter(Parameter: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
  begin
    if Uppercase(ParamStr(I)) = Uppercase(Parameter) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

procedure DeleteSelectedPersonalData();
var
  LocalRoot, RoamingRoot, CommonRoot: String;
begin
  LocalRoot := ExpandConstant('{localappdata}\Tools3000');
  RoamingRoot := ExpandConstant('{userappdata}\Tools3000');
  CommonRoot := ExpandConstant('{commonappdata}\Tools3000');

  if DeleteSettingsHistory and DeleteDiagnostics and DeleteCachesIndexes and DeleteCaptures then
  begin
    DelTree(LocalRoot, True, True, True);
    DelTree(RoamingRoot, True, True, True);
    DelTree(CommonRoot, True, True, True);
    Log('Deleted all selected Tools3000 personal data');
    Exit;
  end;

  if DeleteSettingsHistory then
  begin
    DelTree(LocalRoot + '\config', True, True, True);
    DelTree(LocalRoot + '\stats', True, True, True);
    DeleteFile(RoamingRoot + '\Run History.csv');
    DeleteFile(RoamingRoot + '\Search History.csv');
  end;
  if DeleteDiagnostics then
  begin
    DelTree(LocalRoot + '\logs', True, True, True);
    DelTree(LocalRoot + '\crashdumps', True, True, True);
    DelTree(CommonRoot + '\logs', True, True, True);
  end;
  if DeleteCachesIndexes then
  begin
    DelTree(LocalRoot + '\webview2_data', True, True, True);
    DelTree(LocalRoot + '\temp', True, True, True);
    DeleteFile(LocalRoot + '\Tools3000.db');
    DeleteFile(RoamingRoot + '\Tools3000.db');
  end;
  if DeleteCaptures then
  begin
    DelTree(LocalRoot + '\Screenshots', True, True, True);
    DelTree(LocalRoot + '\Recordings', True, True, True);
  end;
  RemoveDir(LocalRoot);
  RemoveDir(RoamingRoot);
  RemoveDir(CommonRoot);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  RetryCount: Integer;
  DllsLocked: Boolean;
begin
  Result := '';
  Log('PrepareToInstall: Step 1 - Unconditionally terminating process tree and stopping services...');

  // 1. 【强制终止进程树】：在 PrepareToInstall 阶段无条件执行 taskkill 和停止服务
  StopAndKillAllTools3000Processes();

  // 2. 【句柄释放重试轮询】：在强杀后循环探测（最多重试 5 次，每次 Sleep 200ms）确保关键 DLL 文件（如 avcodec-62.dll）不再处于锁定状态
  for RetryCount := 1 to 5 do
  begin
    DllsLocked := AreCriticalDllsLocked();
    if not DllsLocked then
    begin
      Log(Format('PrepareToInstall: Step 2 - All critical DLL file locks released at attempt %d (elapsed %d ms)', [RetryCount, (RetryCount - 1) * 200]));
      Break;
    end;

    Log(Format('PrepareToInstall: Critical DLL files still locked, waiting 200ms (attempt %d/5)...', [RetryCount]));
    Sleep(200);

    // 第3次探测若仍有锁定，补发一次 taskkill /f /t 确保彻底杀绝
    if (RetryCount = 3) and DllsLocked then
    begin
      Log('PrepareToInstall: Locks still detected at attempt 3, re-issuing taskkill...');
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Service.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;
  end;

  if AreCriticalDllsLocked() then
  begin
    Log('PrepareToInstall: Warning - File locks still present after 5 retries. Inno Setup restartreplace will handle update on reboot.');
  end
  else
  begin
    Log('PrepareToInstall: Verification passed - all file locks completely released.');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    // 1. 毫秒级停止并清理后台搜索服务，终止进程树
    StopAndKillAllTools3000Processes();
    DeleteSearchServiceNative();
    DeleteFile(ExpandConstant('{app}\initial_modules.json'));
    WaitForProcessesAndFilesReleased(1000);
  end;
  if CurUninstallStep = usPostUninstall then
  begin
    // 2. 原生注销自启动计划任务（单次通配符覆盖全账户）与注册表 Run 键
    CleanScheduledTasksAndRegistries();
    DeleteSelectedPersonalData();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  DeleteSettingsHistory := True;
  DeleteDiagnostics := True;
  DeleteCachesIndexes := True;
  DeleteCaptures := True;

  if HasCommandLineParameter('/KEEPPERSONALDATA') then
  begin
    DeleteSettingsHistory := False;
    DeleteDiagnostics := False;
    DeleteCachesIndexes := False;
    DeleteCaptures := False;
  end
  else if not ShowPersonalDataOptions() then
  begin
    Result := False;
    Exit;
  end;

  // 仅在 Tools3000 正在运行时提示用户自动关闭
  if IsTools3000Running() then
  begin
    // 弹出多语言确认提示框，用户确认后自动杀掉进程并继续卸载
    if SuppressibleMsgBox(CustomMessage('AppRunningUninstallPrompt'), mbConfirmation, MB_YESNO, IDYES) = IDYES then
    begin
      StopAndKillAllTools3000Processes();
      WaitForProcessesAndFilesReleased(1000);
    end
    else
    begin
      SuppressibleMsgBox(CustomMessage('UninstallAbortedByUser'), mbInformation, MB_OK, IDOK);
      Result := False;
      Exit;
    end;
  end;
end;


