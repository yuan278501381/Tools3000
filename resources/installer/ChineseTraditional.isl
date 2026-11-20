; *** Inno Setup 6.x 標準繁體中文語言包 (台灣慣用術語，100% 完整在地化) ***

[LangOptions]
LanguageName=繁體中文
LanguageID=$0404
LanguageCodePage=0
DialogFontName=Microsoft JhengHei UI
DialogFontSize=9
WelcomeFontName=Microsoft JhengHei UI
WelcomeFontSize=12

[Messages]

; *** Application titles
SetupAppTitle=安裝
SetupWindowTitle=安裝 - %1
UninstallAppTitle=解除安裝
UninstallAppFullTitle=解除安裝 %1

; *** Misc. common
InformationTitle=資訊
ConfirmTitle=確認
ErrorTitle=錯誤

; *** SetupLdr messages
SetupLdrStartupMessage=這將安裝 %1。您想要繼續嗎？
LdrCannotCreateTemp=無法建立暫存檔案。安裝程式已中止
LdrCannotExecTemp=無法在暫存資料夾中執行檔案。安裝程式已中止
HelpTextNote=

; *** Startup error messages
LastErrorMessage=%1。%n%n錯誤 %2: %3
SetupFileMissing=安裝資料夾中缺少檔案 %1。請修正此問題或重新下載安裝程式。
SetupFileCorrupt=安裝檔案已損毀。請重新下載安裝程式。
SetupFileCorruptOrWrongVer=安裝檔案已損毀，或與此版本的安裝程式不相容。請重新下載安裝程式。
InvalidParameter=命令列中傳遞了無效的參數：%n%n%1
SetupAlreadyRunning=安裝程式已在執行中。
WindowsVersionNotSupported=此程式不支援您電腦上執行的 Windows 版本。
WindowsServicePackRequired=此程式需要 %1 Service Pack %2 或更新版本。
NotOnThisPlatform=此程式無法在 %1 上執行。
OnlyOnThisPlatform=此程式只能在 %1 上執行。
OnlyOnTheseArchitectures=此程式僅能安裝於支援以下處理器架構的 Windows 版本：%n%n%1
WinVersionTooLowError=此程式需要 %1 版本 %2 或更新版本。
WinVersionTooHighError=此程式無法安裝於 %1 版本 %2 或更新版本。
AdminPrivilegesRequired=安裝此程式時您必須以系統管理員身分登入。
PowerUserPrivilegesRequired=安裝此程式時您必須以系統管理員或 Power Users 群組成員身分登入。
SetupAppRunningError=安裝程式偵測到 %1 正在執行。%n%n請關閉所有執行中的視窗後點擊「確定」繼續，或點擊「取消」離開。
UninstallAppRunningError=解除安裝程式偵測到 %1 正在執行。%n%n請關閉所有執行中的視窗後點擊「確定」繼續，或點擊「取消」離開。

; *** Startup questions
PrivilegesRequiredOverrideTitle=選擇安裝模式
PrivilegesRequiredOverrideInstruction=選擇安裝模式
PrivilegesRequiredOverrideText1=%1 可為所有使用者安裝（需要系統管理員權限），或僅為目前使用者安裝。
PrivilegesRequiredOverrideText2=%1 可僅為目前使用者安裝，或為所有使用者安裝（需要系統管理員權限）。
PrivilegesRequiredOverrideAllUsers=為所有使用者安裝(&A)
PrivilegesRequiredOverrideAllUsersRecommended=為所有使用者安裝（建議）(&A)
PrivilegesRequiredOverrideCurrentUser=僅為我安裝(&M)
PrivilegesRequiredOverrideCurrentUserRecommended=僅為我安裝（建議）(&M)

; *** Misc. errors
ErrorCreatingDir=安裝程式無法建立資料夾「%1」
ErrorTooManyFilesInDir=無法在資料夾「%1」中建立檔案，因為該資料夾包含過多檔案

; *** Setup common messages
ExitSetupTitle=離開安裝程式
ExitSetupMessage=安裝尚未完成。如果您現在離開，程式將不會被安裝。%n%n您可以稍後再次執行安裝程式以完成安裝。%n%n確定要離開安裝程式嗎？
AboutSetupMenuItem=關於安裝程式(&A)...
AboutSetupTitle=關於安裝程式
AboutSetupMessage=%1 版本 %2%n%3%n%n%1 官方網站:%n%4
AboutSetupNote=
TranslatorNote=

; *** Buttons
ButtonBack=< 上一步(&B)
ButtonNext=下一步(&N) >
ButtonInstall=安裝(&I)
ButtonOK=確定
ButtonCancel=取消
ButtonYes=是(&Y)
ButtonYesToAll=全部皆是(&A)
ButtonNo=否(&N)
ButtonNoToAll=全部皆否(&O)
ButtonFinish=完成(&F)
ButtonBrowse=瀏覽(&B)...
ButtonWizardBrowse=瀏覽(&R)...
ButtonNewFolder=新增資料夾(&M)

; *** "Select Language" dialog messages
SelectLanguageTitle=選擇安裝語言
SelectLanguageLabel=選擇安裝過程要使用的語言。

; *** Common wizard text
ClickNext=點擊「下一步」繼續，或點擊「取消」離開安裝程式。
BeveledLabel=
BrowseDialogTitle=瀏覽資料夾
BrowseDialogLabel=請從下列清單中選擇資料夾，然後點擊「確定」。
NewFolderName=新資料夾

; *** "Welcome" wizard page
WelcomeLabel1=歡迎使用 [name] 安裝精靈
WelcomeLabel2=本精靈將在您的電腦上安裝 [name/ver]。%n%n建議您在繼續之前先關閉其他所有應用程式。

; *** "Password" wizard page
WizardPassword=密碼
PasswordLabel1=此安裝程式受密碼保護。
PasswordLabel3=請輸入密碼，然後點擊「下一步」繼續。密碼區分大小寫。
PasswordEditLabel=密碼(&P):
IncorrectPassword=您輸入的密碼不正確，請重試。

; *** "License Agreement" wizard page
WizardLicense=授權合約
LicenseLabel=在繼續之前請閱讀以下重要資訊。
LicenseLabel3=請閱讀下列授權合約。您必須同意合約條款才能繼續安裝。
LicenseAccepted=我同意此授權合約(&A)
LicenseNotAccepted=我不同意此授權合約(&D)

; *** "Information" wizard pages
WizardInfoBefore=資訊
InfoBeforeLabel=在繼續之前請閱讀以下重要資訊。
InfoBeforeClickLabel=準備好繼續安裝時，請點擊「下一步」。
WizardInfoAfter=資訊
InfoAfterLabel=在繼續之前請閱讀以下重要資訊。
InfoAfterClickLabel=準備好完成安裝時，請點擊「下一步」。

; *** "User Information" wizard page
WizardUserInfo=使用者資訊
UserInfoDesc=請輸入您的資訊。
UserInfoName=使用者名稱(&U):
UserInfoOrg=組織(&O):
UserInfoSerial=序號(&S):
UserInfoNameRequired=必須輸入使用者名稱。

; *** "Select Destination Location" wizard page
WizardSelectDir=選擇安裝位置
SelectDirDesc=您希望將 [name] 安裝在何處？
SelectDirLabel3=安裝程式將把 [name] 安裝到以下資料夾中。
SelectDirBrowseLabel=若要繼續，請點擊「下一步」。若要選擇其他資料夾，請點擊「瀏覽」。
DiskSpaceGBLabel=至少需要 [gb] GB 的可用磁碟空間。
DiskSpaceMBLabel=至少需要 [mb] MB 的可用磁碟空間。
CannotInstallToNetworkDrive=安裝程式無法安裝到網路磁碟機。
CannotInstallToUNCPath=安裝程式無法安裝到 UNC 路徑。
InvalidPath=您必須輸入包含磁碟機代號的完整路徑；例如：%n%nC:\APP%n%n或 UNC 路徑：%n%n\\server\share
InvalidDrive=您選擇的磁碟機或 UNC 共用不存在或無法存取，請選擇其他位置。
DiskSpaceWarningTitle=磁碟空間不足
DiskSpaceWarning=安裝程式至少需要 %1 KB 的可用磁碟空間，但選定的磁碟機僅剩 %2 KB 可用。%n%n您仍要繼續安裝嗎？
DirNameTooLong=資料夾名稱或路徑過長。
InvalidDirName=資料夾名稱無效。
BadDirName32=資料夾名稱不得包含以下任何字元：%n%n%1
DirExistsTitle=資料夾已存在
DirExists=資料夾：%n%n%1%n%n已經存在。您要繼續安裝到此資料夾嗎？
DirDoesntExistTitle=資料夾不存在
DirDoesntExist=資料夾：%n%n%1%n%n不存在。您要建立此資料夾嗎？

; *** "Select Components" wizard page
WizardSelectComponents=選擇安裝元件
SelectComponentsDesc=要安裝哪些元件？
SelectComponentsLabel2=勾選您要安裝的元件，取消勾選不需要的元件，完成後請點擊「下一步」。
FullInstallation=完整安裝
CompactInstallation=精簡安裝
CustomInstallation=自訂安裝
NoUninstallWarningTitle=元件已存在
NoUninstallWarning=安裝程式偵測到下列元件已安裝於您的電腦：%n%n%1%n%n取消勾選這些元件不會將其解除安裝。%n%n確定要繼續嗎？
ComponentSize1=%1 KB
ComponentSize2=%1 MB
ComponentsDiskSpaceGBLabel=目前選取的元件至少需要 [gb] GB 磁碟空間。
ComponentsDiskSpaceMBLabel=目前選取的元件至少需要 [mb] MB 磁碟空間。

; *** "Select Additional Tasks" wizard page
WizardSelectTasks=選擇附加工作
SelectTasksDesc=您想要執行哪些附加工作？
SelectTasksLabel2=請選擇安裝 [name] 時要執行的附加工作，然後點擊「下一步」。

; *** "Select Start Menu Folder" wizard page
WizardSelectProgramGroup=選擇「開始」功能表資料夾
SelectStartMenuFolderDesc=安裝程式應該在何處建立程式捷徑？
SelectStartMenuFolderLabel3=安裝程式將在下列「開始」功能表資料夾中建立捷徑。
SelectStartMenuFolderBrowseLabel=若要繼續，請點擊「下一步」。若要選擇其他資料夾，請點擊「瀏覽」。
MustEnterGroupName=您必須輸入資料夾名稱。
GroupNameTooLong=資料夾名稱或路徑過長。
InvalidGroupName=資料夾名稱無效。
BadGroupName=資料夾名稱不得包含以下任何字元：%n%n%1
NoProgramGroupCheck2=不建立「開始」功能表資料夾(&D)

; *** "Ready to Install" wizard page
WizardReady=準備安裝
ReadyLabel1=安裝程式已準備好在您的電腦上安裝 [name]。
ReadyLabel2a=點擊「安裝」開始安裝，或點擊「上一步」檢視或修改設定。
ReadyLabel2b=點擊「安裝」開始安裝。
ReadyMemoUserInfo=使用者資訊:
ReadyMemoDir=安裝位置:
ReadyMemoType=安裝類型:
ReadyMemoComponents=選取的元件:
ReadyMemoGroup=「開始」功能表資料夾:
ReadyMemoTasks=附加工作:

; *** TDownloadWizardPage wizard page and DownloadTemporaryFile
DownloadingLabel2=正在下載檔案...
ButtonStopDownload=停止下載(&S)
StopDownload=正在停止下載...
ErrorDownloadAborted=下載已中止
ErrorDownloadFailed=下載失敗: %1 %2
ErrorDownloadSizeFailed=取得檔案大小失敗：%n%n%1
ErrorProgress=無效的進度: %1 / %2
ErrorFileSize=無效的檔案大小: 預期 %1，實際 %2

; *** TExtractionWizardPage wizard page and ExtractArchive
ExtractingLabel=正在解壓縮檔案...
ButtonStopExtraction=停止解壓縮(&S)
StopExtraction=正在停止解壓縮...
ErrorExtractionAborted=解壓縮已中止
ErrorExtractionFailed=解壓縮失敗: %1

; *** Archive extraction failure details
ArchiveIncorrectPassword=密碼不正確
ArchiveIsCorrupted=壓縮檔已損毀
ArchiveUnsupportedFormat=不支援的壓縮格式

; *** "Preparing to Install" wizard page
WizardPreparing=正在準備安裝
PreparingDesc=安裝程式正準備在您的電腦上安裝 [name]。
PreviousInstallNotCompleted=先前的安裝或解除安裝尚未完成，您需要重新啟動電腦。%n%n重新啟動後請再次執行安裝程式以完成 [name] 的安裝。
CannotContinue=安裝程式無法繼續，請點擊「取消」離開。
ApplicationsFound=下列應用程式正在使用需要更新的檔案。建議允許安裝程式自動關閉這些應用程式。
ApplicationsFound2=下列應用程式正在使用需要更新的檔案。建議允許安裝程式自動關閉這些應用程式，安裝完成後將嘗試重新啟動它們。
CloseApplications=自動關閉應用程式(&A)
DontCloseApplications=不關閉應用程式(&D)
ErrorCloseApplications=安裝程式無法自動關閉所有應用程式，建議先手動關閉佔用檔案的程式後再繼續。
PrepareToInstallNeedsRestart=安裝程式必須重新啟動電腦。重新啟動後請再次執行安裝程式以完成 [name] 安裝。%n%n您現在要重新啟動嗎？

; *** "Installing" wizard page
WizardInstalling=正在安裝
InstallingLabel=正在將 [name] 安裝到您的電腦，請稍候。

; *** "Setup Completed" wizard page
FinishedHeadingLabel=[name] 安裝精靈完成
FinishedLabelNoIcons=安裝程式已完成 [name] 的安裝。
FinishedLabel=安裝程式已完成 [name] 的安裝。您可以點擊捷徑圖示啟動程式。
ClickFinish=點擊「完成」離開安裝程式。
FinishedRestartLabel=若要完成 [name] 的安裝，必須重新啟動電腦。您現在要重新啟動嗎？
FinishedRestartMessage=若要完成 [name] 的安裝，必須重新啟動電腦。%n%n您現在要重新啟動嗎？
ShowReadmeCheck=檢視 README 說明檔案
YesRadio=是，立即重新啟動電腦(&Y)
NoRadio=否，稍後自行重新啟動電腦(&N)
RunEntryExec=執行 %1
RunEntryShellExec=開啟 %1

; *** "Setup Needs the Next Disk" stuff
ChangeDiskTitle=需要下一張磁片
SelectDiskLabel2=請插入磁片 %1 並點擊「確定」。%n%n若檔案位於其他資料夾，請輸入正確路徑或點擊「瀏覽」。
PathLabel=路徑(&P):
FileNotInDir2=在「%2」中找不到檔案「%1」。請插入正確磁片或選擇其他資料夾。
SelectDirectoryLabel=目標資料夾(&D)

; *** Installation phase messages
SetupAborted=安裝程式未能完成安裝。%n%n請排除問題後重新執行安裝程式。
AbortRetryIgnoreSelectAction=請選擇操作
AbortRetryIgnoreRetry=重試(&T)
AbortRetryIgnoreIgnore=略過錯誤並繼續(&I)
AbortRetryIgnoreCancel=取消安裝
RetryCancelSelectAction=請選擇操作。
RetryCancelRetry=重試(&R)
RetryCancelCancel=取消安裝(&C)

; *** Installation status messages
StatusClosingApplications=正在關閉應用程式...
StatusCreateDirs=正在建立資料夾...
StatusExtractFiles=正在解壓縮檔案...
StatusDownloadFiles=正在下載檔案...
StatusCreateIcons=正在建立捷徑...
StatusCreateIniEntries=正在建立 INI 項目...
StatusCreateRegistryEntries=正在寫入登錄檔項目...
StatusRegisterFiles=正在註冊檔案...
StatusSavingUninstall=正在儲存解除安裝資訊...
StatusRunProgram=正在完成安裝...
StatusRestartingApplications=正在重新啟動應用程式...
StatusRollback=正在還原變更...

; *** Misc. errors
ErrorInternal2=內部錯誤: %1
ErrorFunctionFailedNoCode=%1 失敗
ErrorFunctionFailed=%1 失敗；錯誤代碼 %2
ErrorFunctionFailedWithMessage=%1 失敗；錯誤代碼 %2。%n%3
ErrorExecutingProgram=無法執行檔案：%n%1

; *** Registry errors
ErrorRegOpenKey=開啟登錄檔機碼時發生錯誤：%n%1\%2
ErrorRegCreateKey=建立登錄檔機碼時發生錯誤：%n%1\%2
ErrorRegWriteKey=寫入登錄檔機碼時發生錯誤：%n%1\%2

; *** INI errors
ErrorIniEntry=在檔案「%1」建立 INI 項目時發生錯誤。

; *** File copying errors
FileAbortRetryIgnoreSkipNotRecommended=略過此檔案（不建議）(&S)
FileAbortRetryIgnoreIgnoreNotRecommended=略過錯誤並繼續（不建議）(&I)
SourceIsCorrupted=來源檔案已損毀
SourceDoesntExist=來源檔案「%1」不存在
SourceVerificationFailed=來源檔案驗證失敗
VerificationSignatureDoesntExist=驗證簽章不存在: %1
VerificationSignatureInvalid=驗證簽章無效: %1
VerificationKeyNotFound=找不到驗證金鑰: %1
VerificationFileNameIncorrect=檔案名稱不正確: %1
VerificationFileTagIncorrect=檔案標記不正確: %1
VerificationFileSizeIncorrect=檔案大小不正確: %1
VerificationFileHashIncorrect=檔案雜湊值不正確: %1
ExistingFileReadOnly2=現有檔案已設為唯讀。
ExistingFileReadOnlyRetry=移除唯讀屬性並重試(&R)
ExistingFileReadOnlyKeepExisting=保留現有檔案(&K)
ErrorReadingExistingDest=讀取現有檔案時發生錯誤：
FileExistsSelectAction=請選擇操作
FileExists2=檔案已存在。
FileExistsOverwriteExisting=覆寫現有檔案(&O)
FileExistsKeepExisting=保留現有檔案(&K)
FileExistsOverwriteOrKeepAll=後續衝突皆套用相同操作(&D)
ExistingFileNewerSelectAction=現有檔案比準備安裝的版本更新，建議保留現有檔案。%n%n您要保留現有檔案嗎？
ExistingFileNewer2=現有檔案比準備安裝的版本更新。
ExistingFileNewerOverwriteExisting=覆寫現有檔案(&O)
ExistingFileNewerKeepExisting=保留現有檔案（建議）(&K)
ExistingFileNewerOverwriteOrKeepAll=後續衝突皆套用相同操作(&D)
ErrorChangingAttr=變更現有檔案屬性時發生錯誤：
ErrorCreatingTemp=在目標資料夾建立檔案時發生錯誤：
ErrorReadingSource=讀取來源檔案時發生錯誤：
ErrorCopying=複製檔案時發生錯誤：%n%n%1
ErrorDownloading=下載檔案時發生錯誤：%n%n%1
ErrorExtracting=解壓縮檔案時發生錯誤：
ErrorReplacingExistingFile=取代現有檔案時發生錯誤：
ErrorRestartReplace=RestartReplace 失敗：%n%n%1
ErrorRenamingTemp=重新命名目標資料夾檔案時發生錯誤：
ErrorRegisterServer=無法註冊 DLL/OCX：%1
ErrorRegSvr32Failed=RegSvr32 失敗，結束代碼 %1
ErrorRegisterTypeLib=無法註冊類型程式庫：%1

; *** Uninstall display name markings
UninstallDisplayNameMark=%1 (%2)
UninstallDisplayNameMarks=%1 (%2, %3)
UninstallDisplayNameMark32Bit=32 位元
UninstallDisplayNameMark64Bit=64 位元
UninstallDisplayNameMarkAllUsers=所有使用者
UninstallDisplayNameMarkCurrentUser=目前使用者

; *** Post-installation errors
ErrorOpeningReadme=開啟 README 檔案時發生錯誤。
ErrorRestartingComputer=安裝程式無法重新啟動電腦，請手動重新啟動。

; *** Uninstaller messages
UninstallNotFound=檔案「%1」不存在，無法解除安裝。
UninstallOpenError=無法開啟檔案「%1」，無法解除安裝
UninstallUnsupportedVer=解除安裝紀錄檔「%1」格式不受此版本支援，無法解除安裝
UninstallUnknownEntry=解除安裝紀錄檔中出現未知項目 (%1)
ConfirmUninstall=您確定要完整移除 %1 及其所有元件嗎？
UninstallOnlyOnWin64=此安裝僅能在 64 位元 Windows 上解除安裝。
OnlyAdminCanUninstall=此安裝僅能由具備系統管理員權限的使用者解除安裝。
UninstallStatusLabel=請稍候，正在從您的電腦移除 %1。
UninstalledAll=已成功從您的電腦移除 %1。
UninstalledMost=%1 解除安裝完成。%n%n部分項目未能自動移除，可稍後手動刪除。
UninstalledAndNeedsRestart=若要完成 %1 的解除安裝，必須重新啟動電腦。%n%n您現在要重新啟動嗎？
UninstallDataCorrupted=解除安裝資料已損毀，無法解除安裝

; *** Uninstallation phase messages
ConfirmDeleteSharedFileTitle=刪除共用檔案？
ConfirmDeleteSharedFile2=系統顯示下列共用檔案已無其他程式使用，是否由解除安裝程式刪除？%n%n若仍有程式依賴此檔案，刪除可能導致異常。若不確定請選擇「否」，保留檔案不會造成危害。
SharedFileNameLabel=檔案名稱:
SharedFileLocationLabel=位置:
WizardUninstalling=正在解除安裝
StatusUninstalling=正在解除安裝...

; *** Shutdown block reasons
ShutdownBlockReasonInstallingApp=正在安裝 %1。
ShutdownBlockReasonUninstallingApp=正在解除安裝 %1。

[CustomMessages]
NameAndVersion=%1 版本 %2
AdditionalIcons=附加圖示:
CreateDesktopIcon=建立桌面捷徑(&D)
CreateQuickLaunchIcon=建立快速啟動捷徑(&Q)
ProgramOnTheWeb=%1 官方網站
UninstallProgram=解除安裝 %1
LaunchProgram=啟動 %1(&L)
AssocFileExtension=將 %1 與 %2 副檔名建立關聯(&A)
AssocingFileExtension=正在將 %1 與 %2 副檔名建立關聯...
AutoStartProgramGroupDescription=啟動:
AutoStartProgram=開機自動啟動 %1
AddonHostProgramNotFound=在選定的資料夾中找不到 %1。%n%n是否仍要繼續？
