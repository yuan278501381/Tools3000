; *** Inno Setup 6.x 官方全量标准简体中文语言包 (100% 词条全汉化，0 英文残留) ***

[LangOptions]
LanguageName=简体中文
LanguageID=$0804
LanguageCodePage=0
DialogFontName=Microsoft YaHei UI
DialogFontSize=9
WelcomeFontName=Microsoft YaHei UI
WelcomeFontSize=12

[Messages]

; *** Application titles
SetupAppTitle=安装
SetupWindowTitle=安装 - %1
UninstallAppTitle=卸载
UninstallAppFullTitle=%1 卸载

; *** Misc. common
InformationTitle=信息
ConfirmTitle=确认
ErrorTitle=错误

; *** SetupLdr messages
SetupLdrStartupMessage=这将安装 %1。您想要继续吗？
LdrCannotCreateTemp=无法创建临时文件。安装程序中止
LdrCannotExecTemp=无法在临时目录中执行文件。安装程序中止
HelpTextNote=

; *** Startup error messages
LastErrorMessage=%1。%n%n错误 %2: %3
SetupFileMissing=安装目录中缺少文件 %1。请解决此问题或获取该程序的新副本。
SetupFileCorrupt=安装文件已损坏。请获取该程序的新副本。
SetupFileCorruptOrWrongVer=安装文件已损坏，或与此版本的安装程序不兼容。请解决此问题或获取该程序的新副本。
InvalidParameter=在命令行上传递了一个无效的参数：%n%n%1
SetupAlreadyRunning=安装程序已在运行。
WindowsVersionNotSupported=此程序不支持您计算机上运行的 Windows 版本。
WindowsServicePackRequired=此程序需要 %1 Service Pack %2 或更高版本。
NotOnThisPlatform=此程序无法在 %1 上运行。
OnlyOnThisPlatform=此程序只能在 %1 上运行。
OnlyOnTheseArchitectures=此程序只能在专为以下处理器架构设计的 Windows 版本上安装：%n%n%1
WinVersionTooLowError=此程序需要 %1 版本 %2 或更高版本。
WinVersionTooHighError=此程序无法在 %1 版本 %2 或更高版本上安装。
AdminPrivilegesRequired=安装此程序时您必须以管理员身份登录。
PowerUserPrivilegesRequired=安装此程序时您必须以管理员身份或以 Power Users 组的成员身份登录。
SetupAppRunningError=安装程序检测到 %1 正在运行。%n%n请关闭它的所有实例，然后单击“确定”继续，或单击“取消”退出。
UninstallAppRunningError=卸载程序检测到 %1 正在运行。%n%n请关闭它的所有实例，然后单击“确定”继续，或单击“取消”退出。

; *** Startup questions
PrivilegesRequiredOverrideTitle=选择安装模式
PrivilegesRequiredOverrideInstruction=选择安装模式
PrivilegesRequiredOverrideText1=%1 可以为所有用户安装 (需要管理员特权)，或仅为您自己安装。
PrivilegesRequiredOverrideText2=%1 可以仅为您自己安装，或为所有用户安装 (需要管理员特权)。
PrivilegesRequiredOverrideAllUsers=为所有用户安装(&A)
PrivilegesRequiredOverrideAllUsersRecommended=为所有用户安装 (推荐)(&A)
PrivilegesRequiredOverrideCurrentUser=仅为我安装(&M)
PrivilegesRequiredOverrideCurrentUserRecommended=仅为我安装 (推荐)(&M)

; *** Misc. errors
ErrorCreatingDir=安装程序无法创建目录“%1”
ErrorTooManyFilesInDir=无法在目录“%1”中创建文件，因为该目录包含太多文件

; *** Setup common messages
ExitSetupTitle=退出安装程序
ExitSetupMessage=安装未完成。如果您现在退出，程序将不会被安装。%n%n您可以稍后再次运行安装程序以完成安装。%n%n退出安装程序吗？
AboutSetupMenuItem=关于安装程序(&A)...
AboutSetupTitle=关于安装程序
AboutSetupMessage=%1 版本 %2%n%3%n%n%1 官方主页:%n%4
AboutSetupNote=
TranslatorNote=

; *** Buttons
ButtonBack=< 上一步(&B)
ButtonNext=下一步(&N) >
ButtonInstall=安装(&I)
ButtonOK=确定
ButtonCancel=取消
ButtonYes=是(&Y)
ButtonYesToAll=全是(&A)
ButtonNo=否(&N)
ButtonNoToAll=全否(&O)
ButtonFinish=完成(&F)
ButtonBrowse=浏览(&B)...
ButtonWizardBrowse=浏览(&R)...
ButtonNewFolder=新建文件夹(&M)

; *** "Select Language" dialog messages
SelectLanguageTitle=选择安装语言
SelectLanguageLabel=选择安装时要使用的语言。

; *** Common wizard text
ClickNext=单击“下一步”继续，或单击“取消”退出安装程序。
BeveledLabel=
BrowseDialogTitle=浏览文件夹
BrowseDialogLabel=在下列列表中选择一个文件夹，然后单击“确定”。
NewFolderName=新建文件夹

; *** "Welcome" wizard page
WelcomeLabel1=欢迎使用 [name] 安装向导
WelcomeLabel2=此向导将在您的计算机上安装 [name/ver]。%n%n建议您在继续之前关闭所有其它应用程序。

; *** "Password" wizard page
WizardPassword=密码
PasswordLabel1=此安装程序受密码保护。
PasswordLabel3=请输入密码，然后单击“下一步”继续。密码区分大小写。
PasswordEditLabel=密码(&P):
IncorrectPassword=您输入的密码不正确。请重试。

; *** "License Agreement" wizard page
WizardLicense=许可协议
LicenseLabel=在继续之前请阅读下列重要信息。
LicenseLabel3=请阅读下列许可协议。您必须接受该协议的条款才能继续安装。
LicenseAccepted=我同意此协议(&A)
LicenseNotAccepted=我不同意此协议(&D)

; *** "Information" wizard pages
WizardInfoBefore=信息
InfoBeforeLabel=在继续之前请阅读下列重要信息。
InfoBeforeClickLabel=当您准备好继续安装时，请单击“下一步”。
WizardInfoAfter=信息
InfoAfterLabel=在继续之前请阅读下列重要信息。
InfoAfterClickLabel=当您准备好退出安装时，请单击“下一步”。

; *** "User Information" wizard page
WizardUserInfo=用户信息
UserInfoDesc=请输入您的信息。
UserInfoName=用户名(&U):
UserInfoOrg=组织(&O):
UserInfoSerial=序列号(&S):
UserInfoNameRequired=必须输入用户名。

; *** "Select Destination Location" wizard page
WizardSelectDir=选择目标位置
SelectDirDesc=您想要将 [name] 安装在何处？
SelectDirLabel3=安装程序将把 [name] 安装到下列文件夹中。
SelectDirBrowseLabel=若要继续，请单击“下一步”。若要选择其他文件夹，请单击“浏览”。
DiskSpaceGBLabel=至少需要 [gb] GB 的可用磁盘空间。
DiskSpaceMBLabel=至少需要 [mb] MB 的可用磁盘空间。
CannotInstallToNetworkDrive=安装程序无法安装到网络驱动器。
CannotInstallToUNCPath=安装程序无法安装到 UNC 路径。
InvalidPath=您必须输入带有盘符的完整路径；例如：%n%nC:\APP%n%n或 UNC 格式的路径：%n%n\\server\share
InvalidDrive=您选定的驱动器或 UNC 共享不存在或无法访问。请选择其他驱动器。
DiskSpaceWarningTitle=磁盘空间不足
DiskSpaceWarning=安装程序至少需要 %1 KB 的可用磁盘空间，但选定的驱动器仅有 %2 KB 可用。%n%n您想要继续安装吗？
DirNameTooLong=文件夹名称或路径太长。
InvalidDirName=文件夹名称无效。
BadDirName32=文件夹名称中不能包含下列任何字符：%n%n%1
DirExistsTitle=文件夹已存在
DirExists=文件夹：%n%n%1%n%n已存在。您想要继续安装到此文件夹吗？
DirDoesntExistTitle=文件夹不存在
DirDoesntExist=文件夹：%n%n%1%n%n不存在。您想要创建此文件夹吗？

; *** "Select Components" wizard page
WizardSelectComponents=选择组件
SelectComponentsDesc=应安装哪些组件？
SelectComponentsLabel2=选择您想要安装的组件；清除您不想安装的组件。准备好后请单击“下一步”。
FullInstallation=完全安装
; if possible don't translate 'Compact' as 'Minimal' (I mean 'Minimal' in your language)
CompactInstallation=精简安装
CustomInstallation=自定义安装
NoUninstallWarningTitle=组件已存在
NoUninstallWarning=安装程序检测到下列组件已在您的计算机上安装：%n%n%1%n%n取消选择这些组件将不会卸载它们。%n%n您想要继续吗？
ComponentSize1=%1 KB
ComponentSize2=%1 MB
ComponentsDiskSpaceGBLabel=当前选定的组件至少需要 [gb] GB 的磁盘空间。
ComponentsDiskSpaceMBLabel=当前选定的组件至少需要 [mb] MB 的磁盘空间。

; *** "Select Additional Tasks" wizard page
WizardSelectTasks=选择附加任务
SelectTasksDesc=您想要执行哪些附加任务？
SelectTasksLabel2=选择您想要安装程序在安装 [name] 时执行的附加任务，然后单击“下一步”。

; *** "Select Start Menu Folder" wizard page
WizardSelectProgramGroup=选择开始菜单文件夹
SelectStartMenuFolderDesc=安装程序应该在哪里放置程序的快捷方式？
SelectStartMenuFolderLabel3=安装程序将在下列开始菜单文件夹中创建程序的快捷方式。
SelectStartMenuFolderBrowseLabel=若要继续，请单击“下一步”。若要选择其他文件夹，请单击“浏览”。
MustEnterGroupName=您必须输入一个文件夹名称。
GroupNameTooLong=文件夹名称或路径太长。
InvalidGroupName=文件夹名称无效。
BadGroupName=文件夹名称中不能包含下列任何字符：%n%n%1
NoProgramGroupCheck2=不创建开始菜单文件夹(&D)

; *** "Ready to Install" wizard page
WizardReady=准备安装
ReadyLabel1=安装程序已准备好开始在您的计算机上安装 [name]。
ReadyLabel2a=单击“安装”继续安装，或单击“上一步”查看或更改任何设置。
ReadyLabel2b=单击“安装”继续安装。
ReadyMemoUserInfo=用户信息:
ReadyMemoDir=目标位置:
ReadyMemoType=安装类型:
ReadyMemoComponents=选定组件:
ReadyMemoGroup=开始菜单文件夹:
ReadyMemoTasks=附加任务:

; *** TDownloadWizardPage wizard page and DownloadTemporaryFile
DownloadingLabel2=正在下载文件...
ButtonStopDownload=停止下载(&S)
StopDownload=正在停止下载...
ErrorDownloadAborted=下载已中止
ErrorDownloadFailed=下载失败: %1 %2
ErrorDownloadSizeFailed=获取文件大小时失败：%n%n%1
ErrorProgress=无效的进度: %1 / %2
ErrorFileSize=无效的文件大小: 期望 %1，实际 %2

; *** TExtractionWizardPage wizard page and ExtractArchive
ExtractingLabel=正在解压缩文件...
ButtonStopExtraction=停止解压(&S)
StopExtraction=正在停止解压...
ErrorExtractionAborted=解压已中止
ErrorExtractionFailed=解压失败: %1

; *** Archive extraction failure details
ArchiveIncorrectPassword=密码不正确
ArchiveIsCorrupted=压缩文件已损坏
ArchiveUnsupportedFormat=不支持的压缩文件格式

; *** "Preparing to Install" wizard page
WizardPreparing=准备安装
PreparingDesc=安装程序正准备在您的计算机上安装 [name]。
PreviousInstallNotCompleted=先前程序的安装/删除未完成。您需要重启计算机以完成该安装。%n%n重启计算机后，请再次运行安装程序以完成 [name] 的安装。
CannotContinue=安装程序无法继续。请单击“取消”退出。
ApplicationsFound=下列应用程序正在使用需要由安装程序更新的文件。建议您允许安装程序自动关闭这些应用程序。
ApplicationsFound2=下列应用程序正在使用需要由安装程序更新的文件。建议您允许安装程序自动关闭这些应用程序。安装完成后，安装程序将尝试重新启动这些应用程序。
CloseApplications=自动关闭应用程序(&A)
DontCloseApplications=不关闭应用程序(&D)
ErrorCloseApplications=安装程序无法自动关闭所有应用程序。建议您在继续之前关闭所有使用需要由安装程序更新的文件的应用程序。
PrepareToInstallNeedsRestart=安装程序必须重启您的计算机。重启计算机后，请再次运行安装程序以完成 [name] 的安装。%n%n您现在想要重启吗？

; *** "Installing" wizard page
WizardInstalling=正在安装
InstallingLabel=安装程序正在您的计算机上安装 [name]，请稍候。

; *** "Setup Completed" wizard page
FinishedHeadingLabel=[name] 安装向导完成
FinishedLabelNoIcons=安装程序已在您的计算机上安装了 [name]。
FinishedLabel=安装程序已在您的计算机上安装了 [name]。可以通过选择已安装的图标启动该应用程序。
ClickFinish=单击“完成”退出安装程序。
FinishedRestartLabel=要完成 [name] 的安装，安装程序必须重启您的计算机。您现在想要重启吗？
FinishedRestartMessage=要完成 [name] 的安装，安装程序必须重启您的计算机。%n%n您现在想要重启吗？
ShowReadmeCheck=是的，我想查看 README 文件
YesRadio=我同意此协议(&A)
NoRadio=我不同意此协议(&D)
; used for example as 'Run MyProg.exe'
RunEntryExec=正在运行: %1
; used for example as 'View Readme.txt'
RunEntryShellExec=正在打开: %1

; *** "Setup Needs the Next Disk" stuff
ChangeDiskTitle=安装程序需要下一个磁盘
SelectDiskLabel2=请插入磁盘 %1 并单击“确定”。%n%n如果此磁盘上的文件可以在不同于下列文件夹中找到，请输入正确路径或单击“浏览”。
PathLabel=路径(&P):
FileNotInDir2=在“%2”中找不到文件“%1”。请插入正确的磁盘或选择其他文件夹。
SelectDirectoryLabel=目标文件夹(&D)

; *** Installation phase messages
SetupAborted=安装程序未能完成安装。%n%n请纠正问题并重新运行安装程序。
AbortRetryIgnoreSelectAction=请选择操作
AbortRetryIgnoreRetry=重试(&T)
AbortRetryIgnoreIgnore=忽略错误并继续(&I)
AbortRetryIgnoreCancel=取消安装
RetryCancelSelectAction=请选择操作。
RetryCancelRetry=重试(&R)
RetryCancelCancel=取消安装(&C)

; *** Installation status messages
StatusClosingApplications=正在关闭应用程序...
StatusCreateDirs=正在创建目录...
StatusExtractFiles=正在解压文件...
StatusDownloadFiles=正在下载文件...
StatusCreateIcons=正在创建快捷方式...
StatusCreateIniEntries=正在创建 INI 注册项...
StatusCreateRegistryEntries=正在创建注册表项...
StatusRegisterFiles=正在注册文件...
StatusSavingUninstall=正在保存卸载信息...
StatusRunProgram=正在完成安装...
StatusRestartingApplications=正在重启应用程序...
StatusRollback=正在回滚更改...

; *** Misc. errors
ErrorInternal2=内部错误: %1
ErrorFunctionFailedNoCode=%1 失败
ErrorFunctionFailed=%1 失败；代码 %2
ErrorFunctionFailedWithMessage=%1 失败；代码 %2。%n%3
ErrorExecutingProgram=无法执行文件：%n%1

; *** Registry errors
ErrorRegOpenKey=打开注册表项时出错：%n%1\%2
ErrorRegCreateKey=创建注册表项时出错：%n%1\%2
ErrorRegWriteKey=写入注册表项时出错：%n%1\%2

; *** INI errors
ErrorIniEntry=在文件“%1”中创建 INI 项时出错。

; *** File copying errors
FileAbortRetryIgnoreSkipNotRecommended=跳过此文件 (不推荐)(&S)
FileAbortRetryIgnoreIgnoreNotRecommended=忽略错误并继续 (不推荐)(&I)
SourceIsCorrupted=源文件已损坏
SourceDoesntExist=源文件“%1”不存在
SourceVerificationFailed=源文件验证失败
VerificationSignatureDoesntExist=验证签名不存在: %1
VerificationSignatureInvalid=验证签名无效: %1
VerificationKeyNotFound=未找到验证密钥: %1
VerificationFileNameIncorrect=文件名不正确: %1
VerificationFileTagIncorrect=文件标记不正确: %1
VerificationFileSizeIncorrect=文件大小不正确: %1
VerificationFileHashIncorrect=文件哈希不正确: %1
ExistingFileReadOnly2=现有文件被标记为只读。
ExistingFileReadOnlyRetry=移除只读属性并重试(&R)
ExistingFileReadOnlyKeepExisting=保留现有文件(&K)
ErrorReadingExistingDest=尝试读取现有文件时出错：
FileExistsSelectAction=请选择操作
FileExists2=文件已存在。
FileExistsOverwriteExisting=覆盖现有文件(&O)
FileExistsKeepExisting=保留现有文件(&K)
FileExistsOverwriteOrKeepAll=对后续冲突执行相同操作(&D)
ExistingFileNewerSelectAction=现有文件比安装程序尝试安装的文件更新。建议您保留现有文件。%n%n您想要保留现有文件吗？
ExistingFileNewer2=现有文件比安装程序尝试安装的文件更新。
ExistingFileNewerOverwriteExisting=覆盖现有文件(&O)
ExistingFileNewerKeepExisting=保留现有文件 (推荐)(&K)
ExistingFileNewerOverwriteOrKeepAll=对后续冲突执行相同操作(&D)
ErrorChangingAttr=尝试更改现有文件的属性时出错：
ErrorCreatingTemp=尝试在目标目录中创建文件时出错：
ErrorReadingSource=尝试读取源文件时出错：
ErrorCopying=尝试复制文件时出错：%n%n%1
ErrorDownloading=下载文件时出错：%n%n%1
ErrorExtracting=尝试解压压缩包时出错：
ErrorReplacingExistingFile=尝试替换现有文件时出错：
ErrorRestartReplace=RestartReplace 失败：%n%n%1
ErrorRenamingTemp=尝试重命名目标目录中的文件时出错：
ErrorRegisterServer=无法注册 DLL/OCX：%1
ErrorRegSvr32Failed=RegSvr32 失败，退出码 %1
ErrorRegisterTypeLib=无法注册类型库：%1

; *** Uninstall display name markings
; used for example as 'My Program (32-bit)'
UninstallDisplayNameMark=%1 (%2)
; used for example as 'My Program (32-bit, All users)'
UninstallDisplayNameMarks=%1 (%2, %3)
UninstallDisplayNameMark32Bit=32 位
UninstallDisplayNameMark64Bit=64 位
UninstallDisplayNameMarkAllUsers=所有用户
UninstallDisplayNameMarkCurrentUser=当前用户

; *** Post-installation errors
ErrorOpeningReadme=尝试打开 README 文件时出错。
ErrorRestartingComputer=安装程序无法重启计算机。请手动执行此操作。

; *** Uninstaller messages
UninstallNotFound=文件“%1”不存在。无法卸载。
UninstallOpenError=文件“%1”无法打开。无法卸载
UninstallUnsupportedVer=卸载日志文件“%1”的格式不受此版本的卸载程序支持。无法卸载
UninstallUnknownEntry=在卸载日志中遇到未知项 (%1)
ConfirmUninstall=您确定要完全删除 %1 及其所有组件吗？
UninstallOnlyOnWin64=此安装程序只能在 64 位 Windows 上卸载。
OnlyAdminCanUninstall=此安装只能由具有管理员权限的用户卸载。
UninstallStatusLabel=请稍候，正在从您的计算机中删除 %1。
UninstalledAll=%1 已成功从您的计算机中删除。
UninstalledMost=%1 卸载完成。%n%n部分元素未能删除，这些可以通过手动删除。
UninstalledAndNeedsRestart=要完成 %1 的卸载，必须重启计算机。%n%n您现在想要重启吗？
UninstallDataCorrupted=卸载文件已损坏。无法卸载

; *** Uninstallation phase messages
ConfirmDeleteSharedFileTitle=删除共享文件？
ConfirmDeleteSharedFile2=系统指示下列共享文件已不再被任何程序使用。您想要卸载程序删除此共享文件吗？%n%n如果任何程序仍在使用此文件且将其删除，这些程序可能无法正常运行。如果您不确定，请选择“否”。将文件保留在系统上不会造成任何损害。
SharedFileNameLabel=文件名:
SharedFileLocationLabel=位置:
WizardUninstalling=正在卸载
StatusUninstalling=正在卸载...

; *** Shutdown block reasons
ShutdownBlockReasonInstallingApp=正在安装 %1。
ShutdownBlockReasonUninstallingApp=正在卸载 %1。

; The custom messages below aren't used by Setup itself, but if you make
; use of them in your scripts, you'll want to translate them.


[CustomMessages]

NameAndVersion=%1 version %2
AdditionalIcons=Additional shortcuts:
CreateDesktopIcon=Create a &desktop shortcut
CreateQuickLaunchIcon=Create a &Quick Launch shortcut
ProgramOnTheWeb=%1 on the Web
UninstallProgram=Uninstall %1
LaunchProgram=Launch %1
AssocFileExtension=&Associate %1 with the %2 file extension
AssocingFileExtension=Associating %1 with the %2 file extension...
AutoStartProgramGroupDescription=Startup:
AutoStartProgram=Automatically start %1
AddonHostProgramNotFound=%1 could not be located in the folder you selected.%n%nDo you want to continue anyway?

; *** Tools3000 专属扩展消息 ***
CreateDesktopIcon=创建桌面快捷方式(&D)
AdditionalIcons=附加图标:
LaunchProgram=运行 %1(&L)
UninstallProgram=卸载 %1
NameAndVersion=%1 版本 %2
CreateQuickLaunchIcon=创建快速启动图标(&Q)
ProgramOnTheWeb=%1 官方网站
AssocFileExtension=将 %1 与 %2 文件关联
AssocingFileExtension=正在将 %1 与 %2 文件关联...
AutoStartProgramGroupDescription=启动:
AutoStartProgram=在 Windows 启动时自动运行 %1
AddonHostProgramNotFound=未能找到宿主程序。
AppRunningPrompt=安装程序检测到 Tools3000 正在运行。%n%n是否自动关闭正在运行的 Tools3000 并继续安装？
InstallationAbortedByUser=安装已由用户取消。请关闭 Tools3000 后重新运行安装程序。
InstallingService=正在安装快速文件索引服务...
ShowDetails=详细信息(&D)
HideDetails=隐藏信息(&D)
