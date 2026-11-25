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
#ifndef DeployDir
  #define DeployDir "deploy_dist"
#endif
#if Tools3000Architecture != "x64" && Tools3000Architecture != "arm64"
  #error Tools3000Architecture must be x64 or arm64
#endif

[Setup]
SourceDir=..\..
OutputDir=Output
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
#ifndef LZMABlockThreads
  #define LZMABlockThreads "8"
#endif
#ifndef LZMABlockSizeKB
  #define LZMABlockSizeKB "16384"
#endif
Compression={#CompressionLevel}
SolidCompression=yes
; 开启 LZMA2 多线程分块并行压缩（16MB 分块 x 8 线程），在保持 ultra64 高压缩率的同时将安装包压缩提速 3.4x
LZMANumBlockThreads={#LZMABlockThreads}
LZMABlockSize={#LZMABlockSizeKB}
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
; 默认严格跟随系统 UI 语言（支持 8 大语种，未知系统语种一律纯英文兜底），零弹窗干扰
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
; 通知 Explorer 刷新文件关联与图标缓存
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "resources\installer\ChineseSimplified.isl"
Name: "chinesetraditional"; MessagesFile: "resources\installer\ChineseTraditional.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[CustomMessages]
chinesesimplified.AppRunningPrompt=安装程序检测到 Tools3000 正在运行。%n%n是否自动关闭正在运行的 Tools3000 并继续安装？
chinesetraditional.AppRunningPrompt=安裝程式偵測到 Tools3000 正在執行。%n%n是否自動關閉正在執行的 Tools3000 並繼續安裝？
english.AppRunningPrompt=Setup detected that Tools3000 is currently running.%n%nWould you like to automatically close running instances of Tools3000 and continue with the installation?
japanese.AppRunningPrompt=Tools3000 が現在実行中であることを検出しました。%n%n実行中の Tools3000 を自動的に終了してインストールを続行しますか？
korean.AppRunningPrompt=Tools3000이 현재 실행 중입니다.%n%n실행 중인 Tools3000을 자동으로 종료하고 설치를 계속하시겠습니까?
russian.AppRunningPrompt=Установщик обнаружил, что Tools3000 сейчас запущен.%n%nАвтоматически закрыть запущенные копии Tools3000 и продолжить установку?
brazilianportuguese.AppRunningPrompt=O instalador detectou que o Tools3000 está em execução.%n%nDeseja fechar automaticamente o Tools3000 e continuar a instalação?
spanish.AppRunningPrompt=El instalador detectó que Tools3000 está en ejecución.%n%n¿Desea cerrar automáticamente Tools3000 y continuar con la instalación?

chinesesimplified.InstallationAbortedByUser=安装已由用户取消。请关闭 Tools3000 后重新运行安装程序。
chinesetraditional.InstallationAbortedByUser=安裝已由使用者取消。請關閉 Tools3000 後重新執行安裝程式。
english.InstallationAbortedByUser=Installation was cancelled by the user. Please close Tools3000 and rerun setup.
japanese.InstallationAbortedByUser=インストールはユーザーによってキャンセルされました。Tools3000 を終了してから再度セットアップを実行してください。
korean.InstallationAbortedByUser=사용자가 설치를 취소했습니다. Tools3000을 종료한 후 설치 프로그램을 다시 실행해 주세요.
russian.InstallationAbortedByUser=Установка отменена пользователем. Закройте Tools3000 и запустите установщик снова.
brazilianportuguese.InstallationAbortedByUser=A instalação foi cancelada pelo usuário. Feche o Tools3000 e execute o instalador novamente.
spanish.InstallationAbortedByUser=La instalación fue cancelada por el usuario. Cierre Tools3000 y vuelva a ejecutar el instalador.

chinesesimplified.AppRunningUninstallPrompt=卸载程序检测到 Tools3000 正在运行。%n%n是否自动关闭 Tools3000 并继续卸载？
chinesetraditional.AppRunningUninstallPrompt=解除安裝程式偵測到 Tools3000 正在執行。%n%n是否自動關閉 Tools3000 並繼續解除安裝？
english.AppRunningUninstallPrompt=Uninstall detected that Tools3000 is currently running.%n%nWould you like to automatically close running instances of Tools3000 and continue?
japanese.AppRunningUninstallPrompt=Tools3000 が現在実行中であることを検出しました。%n%n実行中の Tools3000 を自動的に終了してアンインストールを続行しますか？
korean.AppRunningUninstallPrompt=Tools3000이 현재 실행 중입니다.%n%n실행 중인 Tools3000을 자동으로 종료하고 제거를 계속하시겠습니까?
russian.AppRunningUninstallPrompt=Программа удаления обнаружила, что Tools3000 запущен.%n%nАвтоматически закрыть Tools3000 и продолжить удаление?
brazilianportuguese.AppRunningUninstallPrompt=O desinstalador detectou que o Tools3000 está em execução.%n%nDeseja fechar o Tools3000 automaticamente e continuar a desinstalação?
spanish.AppRunningUninstallPrompt=El desinstalador detectó que Tools3000 está en ejecución.%n%n¿Desea cerrar automáticamente Tools3000 y continuar con la desinstalación?

chinesesimplified.UninstallAbortedByUser=卸载已由用户取消。请关闭 Tools3000 后重新运行卸载程序。
chinesetraditional.UninstallAbortedByUser=解除安裝已由使用者取消。請關閉 Tools3000 後重新執行解除安裝程式。
english.UninstallAbortedByUser=Uninstall was cancelled by the user. Please close Tools3000 and rerun uninstall.
japanese.UninstallAbortedByUser=アンインストールはユーザーによってキャンセルされました。Tools3000 を終了してから再度実行してください。
korean.UninstallAbortedByUser=사용자가 제거를 취소했습니다. Tools3000을 종료한 후 다시 실행해 주세요.
russian.UninstallAbortedByUser=Удаление отменено пользователем. Закройте Tools3000 и повторите удаление.
brazilianportuguese.UninstallAbortedByUser=A desinstalação foi cancelada pelo usuário. Feche o Tools3000 e tente novamente.
spanish.UninstallAbortedByUser=La desinstalación fue cancelada por el usuario. Cierre Tools3000 y vuelva a intentarlo.

chinesesimplified.InstallingService=正在安装快速文件索引服务...
chinesetraditional.InstallingService=正在安裝極速檔案索引服務...
english.InstallingService=Installing fast file search index service...
japanese.InstallingService=高速ファイル検索インデックスサービスをインストールしています...
korean.InstallingService=초고속 파일 검색 인덱스 서비스를 설치하는 중...
russian.InstallingService=Установка службы быстрого индексирования файлов...
brazilianportuguese.InstallingService=Instalando serviço de indexação rápida de arquivos...
spanish.InstallingService=Instalando el servicio de indexación rápida de archivos...

chinesesimplified.RegisteringPicasaViewer=正在关联 Picasa 图片查看器支持的图片格式...
chinesetraditional.RegisteringPicasaViewer=正在關聯 Picasa 圖片檢視器支援的圖片格式...
english.RegisteringPicasaViewer=Associating supported image formats with Picasa Photo Viewer...
japanese.RegisteringPicasaViewer=Picasa フォトビューアーの対応画像形式を関連付けています...
korean.RegisteringPicasaViewer=Picasa 이미지 뷰어 지원 이미지 형식을 연결하는 중...
russian.RegisteringPicasaViewer=Настройка ассоциаций изображений для Picasa Photo Viewer...
brazilianportuguese.RegisteringPicasaViewer=Associando formatos de imagem ao Visualizador Picasa...
spanish.RegisteringPicasaViewer=Asociando formatos de imagen al Visor Picasa...

chinesesimplified.ShowDetails=详细信息(&D)
chinesetraditional.ShowDetails=詳細資訊(&D)
english.ShowDetails=Show &Details
japanese.ShowDetails=詳細を表示(&D)
korean.ShowDetails=자세히 보기(&D)
russian.ShowDetails=&Подробности
brazilianportuguese.ShowDetails=Mostrar &detalhes
spanish.ShowDetails=Mostrar &detalles

chinesesimplified.HideDetails=隐藏信息(&D)
chinesetraditional.HideDetails=隱藏資訊(&D)
english.HideDetails=Hide &Details
japanese.HideDetails=詳細を隠す(&D)
korean.HideDetails=자세히 숨기기(&D)
russian.HideDetails=&Скрыть
brazilianportuguese.HideDetails=Ocultar &detalhes
spanish.HideDetails=Ocultar &detalles

chinesesimplified.PersonalDataTitle=个人数据与配置处理
chinesetraditional.PersonalDataTitle=個人資料與設定處理
english.PersonalDataTitle=Personal Data & Preferences
japanese.PersonalDataTitle=個人データと設定の扱い
korean.PersonalDataTitle=개인 데이터 및 설정 처리
russian.PersonalDataTitle=Личные данные и настройки
brazilianportuguese.PersonalDataTitle=Dados pessoais e preferências
spanish.PersonalDataTitle=Datos personales y preferencias

chinesesimplified.PersonalDataDescription=请选择卸载 Tools3000 时的个人数据处理方式：
chinesetraditional.PersonalDataDescription=請選擇解除安裝 Tools3000 時的個人資料處理方式：
english.PersonalDataDescription=Choose how your personal data and configuration should be handled:
japanese.PersonalDataDescription=Tools3000 をアンインストールする際の個人データの扱いを選択してください：
korean.PersonalDataDescription=Tools3000 제거 시 개인 데이터 및 설정 처리 방법을 선택하세요:
russian.PersonalDataDescription=Выберите, как поступить с личными данными и настройками при удалении Tools3000:
brazilianportuguese.PersonalDataDescription=Escolha como seus dados pessoais e configurações devem ser tratados ao desinstalar:
spanish.PersonalDataDescription=Elija cómo gestionar sus datos personales y configuración al desinstalar Tools3000:

chinesesimplified.UninstallModeStandard=保留个人配置与媒体 (推荐)
chinesetraditional.UninstallModeStandard=保留個人設定與媒體（建議）
english.UninstallModeStandard=Keep personal configuration and media (Recommended)
japanese.UninstallModeStandard=個人設定とメディアを保持する（推奨）
korean.UninstallModeStandard=개인 설정 및 미디어 유지 (권장)
russian.UninstallModeStandard=Сохранить настройки и медиафайлы (рекомендуется)
brazilianportuguese.UninstallModeStandard=Manter configurações pessoais e mídia (Recomendado)
spanish.UninstallModeStandard=Conservar configuración personal y multimedia (Recomendado)

chinesesimplified.UninstallModeStandardDesc=仅移除应用程序与后台服务。保留您的偏好设置、手势方案以及截图/录屏作品，便于以后重新安装。
chinesetraditional.UninstallModeStandardDesc=僅移除應用程式與背景服務。保留您的偏好設定、手勢方案以及擷圖／錄影檔案，方便日後重新安裝。
english.UninstallModeStandardDesc=Removes only the application and services. Retains your preferences, gestures, and screenshots/recordings for future installations.
japanese.UninstallModeStandardDesc=アプリ本体とバックグラウンドサービスのみを削除します。設定・ジェスチャー・キャプチャ／録画データは保持されます。
korean.UninstallModeStandardDesc=애플리케이션과 백그라운드 서비스만 제거합니다. 환경 설정, 제스처, 스크린샷/녹화 파일은 보존됩니다.
russian.UninstallModeStandardDesc=Удаляет только программу и службы. Сохраняет настройки, жесты, скриншоты и записи для будущей установки.
brazilianportuguese.UninstallModeStandardDesc=Remove apenas o aplicativo e os serviços. Mantém suas preferências, gestos e capturas/gravações para reinstalações futuras.
spanish.UninstallModeStandardDesc=Elimina solo la aplicación y los servicios. Conserva sus preferencias, gestos y capturas/grabaciones para futuras instalaciones.

chinesesimplified.UninstallModeClean=清理全部配置与运行缓存
chinesetraditional.UninstallModeClean=清理全部設定與執行快取
english.UninstallModeClean=Clear all configuration and runtime caches
japanese.UninstallModeClean=すべての設定とキャッシュを削除する
korean.UninstallModeClean=모든 설정 및 런타임 캐시 삭제
russian.UninstallModeClean=Удалить все настройки и кэши
brazilianportuguese.UninstallModeClean=Limpar todas as configurações e caches
spanish.UninstallModeClean=Borrar toda la configuración y cachés

chinesesimplified.UninstallModeCleanDesc=删除偏好设置、窗口记忆、运行历史、诊断日志与搜索索引缓存。
chinesetraditional.UninstallModeCleanDesc=刪除偏好設定、視窗記憶、執行歷程、診斷日誌與搜尋索引快取。
english.UninstallModeCleanDesc=Removes user preferences, window state, run history, diagnostic logs, and search index caches.
japanese.UninstallModeCleanDesc=設定・ウィンドウ状態・実行履歴・診断ログ・検索インデックスキャッシュをすべて削除します。
korean.UninstallModeCleanDesc=사용자 설정, 창 상태, 실행 기록, 진단 로그 및 검색 인덱스 캐시를 삭제합니다.
russian.UninstallModeCleanDesc=Удаляет настройки, состояние окон, историю, диагностические логи и кэш поискового индекса.
brazilianportuguese.UninstallModeCleanDesc=Remove preferências, estado das janelas, histórico, logs de diagnóstico e caches do índice de busca.
spanish.UninstallModeCleanDesc=Elimina preferencias, estado de ventanas, historial, registros de diagnóstico y cachés del índice de búsqueda.

chinesesimplified.DeleteMediaCaptures=同时永久删除“截图和录屏”媒体文件
chinesetraditional.DeleteMediaCaptures=同時永久刪除「擷圖與錄影」媒體檔案
english.DeleteMediaCaptures=Permanently delete screenshot and recording files as well
japanese.DeleteMediaCaptures=スクリーンショットと録画ファイルも完全に削除する
korean.DeleteMediaCaptures=스크린샷 및 녹화 미디어 파일도 영구 삭제
russian.DeleteMediaCaptures=Также навсегда удалить файлы скриншотов и записей
brazilianportuguese.DeleteMediaCaptures=Excluir permanentemente também arquivos de captura e gravação
spanish.DeleteMediaCaptures=Eliminar también de forma permanente las capturas y grabaciones

chinesesimplified.DeleteMediaCapturesNote=注意：此操作将清空默认媒体保存目录，删除后无法恢复。
chinesetraditional.DeleteMediaCapturesNote=注意：此操作將清空預設媒體儲存資料夾，刪除後無法復原。
english.DeleteMediaCapturesNote=Notice: This permanently removes files in the default capture folders and cannot be undone.
japanese.DeleteMediaCapturesNote=注意：既定の保存フォルダー内のファイルが完全に削除され、元に戻せません。
korean.DeleteMediaCapturesNote=주의: 기본 저장 폴더의 파일이 영구 삭제되며 복구할 수 없습니다.
russian.DeleteMediaCapturesNote=Внимание: файлы в папках сохранения по умолчанию будут удалены без возможности восстановления.
brazilianportuguese.DeleteMediaCapturesNote=Atenção: isso remove permanentemente os arquivos das pastas padrão e não pode ser desfeito.
spanish.DeleteMediaCapturesNote=Aviso: esto elimina permanentemente los archivos de las carpetas predeterminadas y no se puede deshacer.

chinesesimplified.ContinueUninstall=继续卸载
chinesetraditional.ContinueUninstall=繼續解除安裝
english.ContinueUninstall=Continue Uninstall
japanese.ContinueUninstall=アンインストールを続行
korean.ContinueUninstall=제거 계속
russian.ContinueUninstall=Продолжить удаление
brazilianportuguese.ContinueUninstall=Continuar desinstalação
spanish.ContinueUninstall=Continuar desinstalación

chinesesimplified.AutoStartProgram=开机自动启动 Tools3000
chinesetraditional.AutoStartProgram=開機自動啟動 Tools3000
english.AutoStartProgram=Start Tools3000 automatically on Windows startup
japanese.AutoStartProgram=Windows 起動時に Tools3000 を自動起動する
korean.AutoStartProgram=Windows 시작 시 Tools3000 자동 실행
russian.AutoStartProgram=Запускать Tools3000 автоматически при старте Windows
brazilianportuguese.AutoStartProgram=Iniciar o Tools3000 automaticamente com o Windows
spanish.AutoStartProgram=Iniciar Tools3000 automáticamente con Windows

chinesesimplified.TypeFull=完整体验安装 (推荐 · 默认启用全部 7 大核心模块)
chinesetraditional.TypeFull=完整體驗安裝（建議 · 預設啟用全部核心模組）
english.TypeFull=Full Installation (Recommended - All Core Modules Enabled)
japanese.TypeFull=フルインストール（推奨・すべてのコアモジュールを有効化）
korean.TypeFull=전체 설치 (권장 · 모든 핵심 모듈 활성화)
russian.TypeFull=Полная установка (рекомендуется · все модули)
brazilianportuguese.TypeFull=Instalação completa (Recomendado - Todos os módulos)
spanish.TypeFull=Instalación completa (Recomendado - Todos los módulos)

chinesesimplified.TypeCompact=极简轻量安装
chinesetraditional.TypeCompact=極簡輕量安裝
english.TypeCompact=Compact Installation
japanese.TypeCompact=コンパクトインストール
korean.TypeCompact=간편 설치
russian.TypeCompact=Компактная установка
brazilianportuguese.TypeCompact=Instalação compacta
spanish.TypeCompact=Instalación compacta

chinesesimplified.TypeCustom=自定义模块选择
chinesetraditional.TypeCustom=自訂模組選擇
english.TypeCustom=Custom Module Selection
japanese.TypeCustom=カスタムモジュール選択
korean.TypeCustom=사용자 지정 모듈 선택
russian.TypeCustom=Выборочная установка модулей
brazilianportuguese.TypeCustom=Seleção personalizada de módulos
spanish.TypeCustom=Selección personalizada de módulos

chinesesimplified.CompSearch=超级文件检索 (Search) — 全盘秒级索引与极速文件启动
chinesetraditional.CompSearch=極速檔案檢索 (Search) — 全磁碟毫秒索引與快速預覽
english.CompSearch=Fast File Search (Search) — Instant disk indexing & launcher
japanese.CompSearch=高速ファイル検索 (Search) — ミリ秒インデックス＆クイックプレビュー
korean.CompSearch=초고속 파일 검색 (Search) — 전체 디스크 즉시 인덱싱 및 빠른 미리보기
russian.CompSearch=Быстрый поиск файлов (Search) — мгновенный индекс и QuickLook
brazilianportuguese.CompSearch=Busca Rápida de Arquivos (Search) — indexação instantânea e QuickLook
spanish.CompSearch=Búsqueda Rápida de Archivos (Search) — indexación instantánea y QuickLook

chinesesimplified.CompCapture=截图贴图与录屏 (Capture) — 智能贴图、长截图、拾色器与高清录屏
chinesetraditional.CompCapture=擷圖貼圖與錄影 (Capture) — 智慧貼圖、長擷圖、取色器與高畫質錄影
english.CompCapture=Screenshot, Pin & Recording (Capture) — Smart pin, OCR & HD recording
japanese.CompCapture=キャプチャ・ピン留め・録画 (Capture) — 注釈・OCR・60 FPS 録画
korean.CompCapture=캡처·화면 고정·녹화 (Capture) — 스마트 핀, OCR 및 고화질 녹화
russian.CompCapture=Скриншоты, закрепление и запись (Capture) — разметка, OCR и 60 FPS
brazilianportuguese.CompCapture=Captura, Pins e Gravação (Capture) — pins inteligentes, OCR e 60 FPS
spanish.CompCapture=Captura, Pines y Grabación (Capture) — pines inteligentes, OCR y 60 FPS

chinesesimplified.CompOcrOfflinePack=离线高精度文字识别引擎包 (PP-OCRv4 · 约 16MB) — 勾选后首次启动后台自动部署，大幅增强小字、中英代码混排、表格与倾斜文本识别精度
chinesetraditional.CompOcrOfflinePack=離線高精度文字辨識引擎包 (PP-OCRv4 · 約 16MB) — 勾選後首次啟動背景自動部署，大幅提升小字、程式碼與表格辨識精度
english.CompOcrOfflinePack=High-Precision Offline OCR Engine Pack (PP-OCRv4, ~16 MB) — Auto-downloads in background on first launch for superior table, code & small-text accuracy
japanese.CompOcrOfflinePack=高精度オフライン OCR パック (PP-OCRv4・約 16MB) — 初回起動時にバックグラウンド自動取得し、コード・表・小文字の認識精度を向上
korean.CompOcrOfflinePack=고정밀 오프라인 OCR 엔진 팩 (PP-OCRv4 · 약 16MB) — 첫 실행 시 백그라운드 자동 다운로드로 코드·표·작은 글자 인식 향상
russian.CompOcrOfflinePack=Офлайн-пакет OCR высокой точности (PP-OCRv4, ~16 МБ) — автозагрузка при первом запуске для кода, таблиц и мелкого текста
brazilianportuguese.CompOcrOfflinePack=Pacote OCR Offline de Alta Precisão (PP-OCRv4, ~16 MB) — download automático em segundo plano para tabelas, código e textos pequenos
spanish.CompOcrOfflinePack=Paquete OCR Offline de Alta Precisión (PP-OCRv4, ~16 MB) — descarga automática en segundo plano para tablas, código y texto pequeño

chinesesimplified.CompGesture=鼠标手势与触发角 (Gesture) — 右键手势轨迹、屏幕四角触发与边缘滚轮
chinesetraditional.CompGesture=滑鼠手勢與觸發角 (Gesture) — 右鍵手勢軌跡、螢幕四角觸發與邊緣滾輪
english.CompGesture=Mouse Gestures & Hot Corners (Gesture) — Trailing gestures, hot corners & edge wheel
japanese.CompGesture=マウスジェスチャー＆ホットコーナー (Gesture) — 右ドラッグ軌跡・画面端ホイール
korean.CompGesture=마우스 제스처 및 핫 코너 (Gesture) — 우클릭 궤적, 화면 모서리 및 가장자리 휠
russian.CompGesture=Жесты мышью и углы экрана (Gesture) — траектории ПКМ, углы и колесо у края
brazilianportuguese.CompGesture=Gestos do Mouse e Cantos Ativos (Gesture) — gestos com botão direito e roda na borda
spanish.CompGesture=Gestos de Ratón y Esquinas Activas (Gesture) — gestos con clic derecho y rueda en borde

chinesesimplified.CompKeycast=按键回显 (Keycast) — 屏幕实时按键显示、机械键帽动效
chinesetraditional.CompKeycast=按鍵回顯 (Keycast) — 螢幕即時按鍵顯示與鍵帽動效
english.CompKeycast=Keycast Overlay (Keycast) — Real-time keystroke visualization
japanese.CompKeycast=キー入力表示 (Keycast) — リアルタイムキーキャップオーバーレイ
korean.CompKeycast=키 입력 표시 (Keycast) — 실시간 키캡 오버레이 시각화
russian.CompKeycast=Отображение клавиш (Keycast) — показ нажатий в реальном времени
brazilianportuguese.CompKeycast=Exibição de Teclas (Keycast) — visualização de teclas em tempo real
spanish.CompKeycast=Visualizador de Teclas (Keycast) — pulsaciones en pantalla en tiempo real

chinesesimplified.CompDialog=文件对话框增强 (Dialog Enhancer) — 常用目录快速跳转、历史路径记忆
chinesetraditional.CompDialog=檔案對話方塊增強 (Dialog Enhancer) — 常用資料夾快速跳轉與路徑記憶
english.CompDialog=File Dialog Enhancer (Dialog) — Quick folders & path memory
japanese.CompDialog=ファイルダイアログ拡張 (Dialog) — よく使うフォルダーへ瞬時移動
korean.CompDialog=파일 대화 상자 향상 (Dialog) — 자주 쓰는 폴더 빠른 이동 및 경로 기억
russian.CompDialog=Улучшение диалогов файлов (Dialog) — быстрые папки и история путей
brazilianportuguese.CompDialog=Aprimorador de Diálogos (Dialog) — pastas rápidas e memória de caminhos
spanish.CompDialog=Mejora de Cuadros de Archivo (Dialog) — carpetas rápidas y memoria de rutas

chinesesimplified.CompSpotlight=演示专用特效 (Spotlight) — 屏幕聚光灯聚焦、点击水波纹与流光轨迹
chinesetraditional.CompSpotlight=簡報專用特效 (Spotlight) — 螢幕聚光燈聚焦、點擊波紋與流光軌跡
english.CompSpotlight=Presentation FX (Spotlight) — Screen spotlight focus, click ripple & mouse trails
japanese.CompSpotlight=プレゼン演出エフェクト (Spotlight) — スポットライト・クリック波紋・軌跡
korean.CompSpotlight=프레젠테이션 효과 (Spotlight) — 화면 스포트라이트, 클릭 파문 및 커서 궤적
russian.CompSpotlight=Эффекты презентации (Spotlight) — прожектор курсора, круги кликов и след
brazilianportuguese.CompSpotlight=Efeitos de Apresentação (Spotlight) — holofote, ondas de clique e rastros
spanish.CompSpotlight=Efectos de Presentación (Spotlight) — foco de cursor, ondas de clic y estelas

chinesesimplified.CompRemote=远程协助增强 (Remote Boost) — 主控端热键直通、修饰键急救冲刷与输入法脱敏
chinesetraditional.CompRemote=遠端協助增強 (Remote Boost) — 主控端熱鍵直通、修飾鍵釋放與輸入法保護
english.CompRemote=Remote Boost (Remote) — Immersive hotkey tunnel, emergency flush & smart IME sanitizing
japanese.CompRemote=リモート操作支援 (Remote) — ホットキー透過・修飾キー固着解除
korean.CompRemote=원격 제어 도우미 (Remote) — 단축키 패스스루 및 조합 키 걸림 해제
russian.CompRemote=Помощник удалённого доступа (Remote) — проброс горячих клавиш и сброс залипаний
brazilianportuguese.CompRemote=Acelerador Remoto (Remote) — passagem de atalhos e desbloqueio de teclas presas
spanish.CompRemote=Asistente Remoto (Remote) — paso de atajos y liberación de teclas bloqueadas

chinesesimplified.CompPicasaViewer=Picasa 图片查看器 (Rust 复刻 · 可选) — 仅复刻原版经典极速看图与动效（非原版 Picasa 全功能套件），勾选启用后自动关联全部支持的图片格式
chinesetraditional.CompPicasaViewer=Picasa 圖片檢視器 (Rust 復刻 · 選用) — 僅復刻原版經典極速看圖與動效（非原版 Picasa 全功能套件），勾選啟用後自動關聯全部支援的圖片格式
english.CompPicasaViewer=Picasa Photo Viewer (Rust Recreation · Optional) — Lightweight image viewer only (not the full Google Picasa suite); automatically associates all supported image formats when enabled
japanese.CompPicasaViewer=Picasa フォトビューアー (Rust 再現・オプション) — 軽量画像ビューアー専用（オリジナル Picasa 全機能版ではありません）。有効化すると対応画像形式を自動関連付け
korean.CompPicasaViewer=Picasa 이미지 뷰어 (Rust 재구현 · 선택) — 경량 이미지 뷰어 전용(원본 Picasa 전체 기능 제품군 아님), 활성화 시 지원하는 모든 이미지 형식 자동 연결
russian.CompPicasaViewer=Просмотрщик фото Picasa (реализация на Rust · опция) — только лёгкий просмотрщик (не полный пакет Google Picasa); при включении автоматически связывает все поддерживаемые форматы
brazilianportuguese.CompPicasaViewer=Visualizador de Fotos Picasa (Recriação em Rust · Opcional) — Apenas visualizador leve (não é o pacote completo Google Picasa); associa automaticamente todos os formatos de imagem suportados
spanish.CompPicasaViewer=Visor de Fotos Picasa (Recreación en Rust · Opcional) — Solo visor ligero de imágenes (no la suite completa de Google Picasa); asocia automáticamente todos los formatos compatibles al habilitarlo

[Types]
Name: "full"; Description: "{cm:TypeFull}"
Name: "compact"; Description: "{cm:TypeCompact}"
Name: "custom"; Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
Name: "search"; Description: "{cm:CompSearch}"; Types: full compact
; 注意：capture 拥有可选子组件 capture\ocr_models (dontinheritcheck)，必须显式声明 Flags: checkablealone，
; 否则 Inno Setup 在所有子组件默认未勾选时会自动取消勾选父组件 capture，导致全新安装默认关闭截图功能。
Name: "capture"; Description: "{cm:CompCapture}"; Types: full compact; Flags: checkablealone
Name: "capture\ocr_models"; Description: "{cm:CompOcrOfflinePack}"; Flags: dontinheritcheck
Name: "gesture"; Description: "{cm:CompGesture}"; Types: full compact
Name: "keycast"; Description: "{cm:CompKeycast}"; Types: full
Name: "dialogenhancer"; Description: "{cm:CompDialog}"; Types: full
Name: "spotlight"; Description: "{cm:CompSpotlight}"; Types: full
Name: "remote"; Description: "{cm:CompRemote}"; Types: full
; 可选独立组件：Rust 复刻轻量 Picasa 图片查看器（严禁挂在 full/compact 下，默认安装不勾选；勾选启用时自动关联全部支持图片格式）
Name: "picasaviewer"; Description: "{cm:CompPicasaViewer}"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "{cm:AutoStartProgram}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 核心可执行文件与主程序根目录共享运行库（显式排除测试库 *gtest*/*gmock* 与插件本体 Plugin_*.dll；禁用 restartreplace，改由全目录清场 + NTFS Rename-Aside 实现 100% 物理零重启覆盖）
Source: "{#DeployDir}\*.dll"; DestDir: "{app}"; Excludes: "*gtest*.dll,*gmock*.dll,Plugin_*.dll"; Flags: ignoreversion uninsrestartdelete
; 插件目录严格仅打包 Plugin_*.dll 与 *.plugin.json（共享运行库统一复用 {app} 根目录唯一副本）
Source: "{#DeployDir}\plugins\Plugin_*.dll"; DestDir: "{app}\plugins"; Flags: ignoreversion uninsrestartdelete
Source: "{#DeployDir}\plugins\*.plugin.json"; DestDir: "{app}\plugins"; Flags: ignoreversion uninsrestartdelete
Source: "{#DeployDir}\Tools3000.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#DeployDir}\Tools3000_Service.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#DeployDir}\Tools3000_Picasa_Viewer.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#DeployDir}\tools3000-cli.exe"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
; 其余全量资源与依赖文件（排除已明确声明的 DLL/EXE/JSON 避免二次拷贝冲突，且绝不配置 ProgramData users-modify）
Source: "{#DeployDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs uninsrestartdelete; Excludes: "*.pdb,Tools3000Tests.*,*Preview.*,*Integration.*,*.flag,debug.flag,*.dll,*.plugin.json,Tools3000.exe,Tools3000_Service.exe,Tools3000_Picasa_Viewer.exe,tools3000-cli.exe"

[InstallDelete]
Type: files; Name: "{app}\*gtest*.dll"
Type: files; Name: "{app}\*gmock*.dll"
Type: files; Name: "{app}\Plugin_*.dll"
Type: files; Name: "{app}\Tools3000Tests.*"
; 覆盖升级时顺带清理历史版本遗留的 6 个 0 引用冗余 DLL
Type: files; Name: "{app}\WebView2Loader.dll"
Type: files; Name: "{app}\vccorlib140.dll"
Type: files; Name: "{app}\msvcp140_1.dll"
Type: files; Name: "{app}\msvcp140_2.dll"
Type: files; Name: "{app}\msvcp140_codecvt_ids.dll"
Type: files; Name: "{app}\vcruntime140_threads.dll"

[UninstallDelete]
Type: files; Name: "{autodesktop}\Tools3000.lnk"
Type: files; Name: "{userdesktop}\Tools3000.lnk"
Type: files; Name: "{commondesktop}\Tools3000.lnk"
Type: filesandordirs; Name: "{autoprograms}\Tools3000"
Type: filesandordirs; Name: "{userprograms}\Tools3000"
Type: filesandordirs; Name: "{commonprograms}\Tools3000"
Type: files; Name: "{app}\initial_modules.json"
Type: files; Name: "{app}\*.log"
Type: files; Name: "{app}\*.old_del_*"
Type: files; Name: "{app}\plugins\*.old_del_*"
Type: files; Name: "{app}\ui\assets\*.old_del_*"
Type: dirifempty; Name: "{app}\resources\locales\native"
Type: dirifempty; Name: "{app}\resources\locales"
Type: dirifempty; Name: "{app}\resources\wallpapers"
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
; 授予本机交互式登录用户 (IU) 与已认证用户 (AU) 按需启动 (RP / SERVICE_START) 与查询状态 (LC) 权限，不开放配置篡改或删除权限
Filename: "{sys}\sc.exe"; Parameters: "sdset Tools3000_SearchService ""D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPLOCRRC;;;IU)(A;;CCLCSWRPLOCRRC;;;AU)(A;;CCLCSWLOCRRC;;;SU)"""; Flags: runhidden waituntilterminated; Check: ServiceExists
; 若用户在安装时勾选了可选的 Picasa 图片查看器，则在交互桌面用户上下文静默关联全部支持的图片格式（避免主程序启动后再二次重复注册）
Filename: "{app}\Tools3000_Picasa_Viewer.exe"; Parameters: "--register-silent"; Flags: runhidden waituntilterminated runasoriginaluser; StatusMsg: "{cm:RegisteringPicasaViewer}"; Check: IsPicasaViewerRegisterNeeded
; 使用 runasoriginaluser 兼容多账户 UAC 提权场景，确保客户端运行在发起安装的交互桌面用户上下文中
Filename: "{app}\Tools3000.exe"; Description: "{cm:LaunchProgram,Tools3000}"; Flags: nowait postinstall skipifsilent runasoriginaluser
; 应用内一键静默更新 (/SILENT /RESTARTAPPLICATIONS 或 /AUTOUPDATE) 完成后自动无缝拉起新版 Tools3000；
; 普通运维静默部署 (/SILENT /VERYSILENT 不带上述参数) 仍保持静默不自动启动，确保严格 MECE 互斥。
Filename: "{app}\Tools3000.exe"; Flags: nowait postinstall runasoriginaluser; Check: ShouldAutoLaunchAfterSilentUpgrade


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
  HasExistingUserConfig: Boolean;
  InitialCompSearch: Boolean;
  InitialCompCapture: Boolean;
  InitialCompOcrPack: Boolean;
  InitialCompGesture: Boolean;
  InitialCompKeycast: Boolean;
  InitialCompDialog: Boolean;
  InitialCompSpotlight: Boolean;
  InitialCompRemote: Boolean;
  InitialCompPicasaViewer: Boolean;
  UserVisitedComponentsPage: Boolean;

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
  PICASA_PROG_ID = 'Tools3000.PicasaViewer';
  // 与 src/viewer/src/constants.rs SUPPORTED_EXTENSIONS 保持 1:1 对齐的全部 41 个支持的图片扩展名
  PICASA_SUPPORTED_EXTS =
    'jpg,jpeg,jpe,jfif,pjpeg,' +
    'png,apng,webp,gif,bmp,dib,rle,ico,cur,tif,tiff,svg,svgz,' +
    'avif,heic,heif,hif,wdp,hdp,jxr,tga,qoi,ppm,pgm,pbm,dds,' +
    'dng,cr2,cr3,nef,arw,orf,rw2,pef,raf,srw';

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
  WIN32_HKEY_LOCAL_MACHINE = $80000002;
  KEY_QUERY_VALUE_FLAG = $0001;
  KEY_SET_VALUE_FLAG = $0002;
  KEY_WOW64_64KEY_FLAG = $0100;
  REG_MULTI_SZ_TYPE = 7;
  MOVEFILE_REPLACE_EXISTING_FLAG = $0001;
  SESSION_MANAGER_SUBKEY = 'SYSTEM\CurrentControlSet\Control\Session Manager';

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
function RegOpenKeyExW(hKey: Cardinal; lpSubKey: String; ulOptions: DWORD; samDesired: DWORD; var phkResult: THandle): LongInt;
  external 'RegOpenKeyExW@advapi32.dll stdcall';
function RegQueryValueExWLen(hKey: THandle; lpValueName: String; lpReserved: Cardinal; var lpType: DWORD; lpData: Cardinal; var lpcbData: DWORD): LongInt;
  external 'RegQueryValueExW@advapi32.dll stdcall';
function RegQueryValueExWStr(hKey: THandle; lpValueName: String; lpReserved: Cardinal; var lpType: DWORD; lpData: String; var lpcbData: DWORD): LongInt;
  external 'RegQueryValueExW@advapi32.dll stdcall';
function RegSetValueExWStr(hKey: THandle; lpValueName: String; Reserved: DWORD; dwType: DWORD; lpData: String; cbData: DWORD): LongInt;
  external 'RegSetValueExW@advapi32.dll stdcall';
function RegDeleteValueW(hKey: THandle; lpValueName: String): LongInt;
  external 'RegDeleteValueW@advapi32.dll stdcall';
function RegCloseKey(hKey: THandle): LongInt;
  external 'RegCloseKey@advapi32.dll stdcall';
function MoveFileExW(lpExistingFileName, lpNewFileName: String; dwFlags: DWORD): BOOL;
  external 'MoveFileExW@kernel32.dll stdcall';
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
  FullPath, FileNameLower, UninstallerName: String;
begin
  Result := False;
  if (DirPath = '') or (not DirExists(DirPath)) then Exit;

  if IsUninstaller then
    UninstallerName := Lowercase(ExtractFileName(ExpandConstant('{uninstallexe}')))
  else
    UninstallerName := '';

  if FindFirst(DirPath + '\*.*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name = '.') or (FindRec.Name = '..') then
          Continue;

        FullPath := DirPath + '\' + FindRec.Name;
        FileNameLower := Lowercase(FindRec.Name);

        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          if CheckDirFilesLocked(FullPath) then
          begin
            Result := True;
            Exit;
          end;
        end
        else
        begin
          // 绝对排除卸载程序自身 (unins*) 与已移开的旧版本占位壳 (.old_del_*)
          if (Pos('unins', FileNameLower) = 1) or
             ((UninstallerName <> '') and (FileNameLower = UninstallerName)) or
             (Pos('.old_del_', FileNameLower) > 0) then
            Continue;

          // 覆盖全文件类型（含 .dll/.exe/.woff2/.js/.html），确保孤儿 WebView2 锁住字体/前端资源时也能 100% 捕获
          if IsFileLocked(FullPath) then
          begin
            Log('CheckDirFilesLocked: File is locked -> ' + FullPath);
            Result := True;
            Exit;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function AreAppFilesLockedInDir(const TargetAppDir: String): Boolean;
var
  NormalizedDir: String;
begin
  NormalizedDir := RemoveBackslashUnlessRoot(Trim(TargetAppDir));
  if NormalizedDir = '' then
  begin
    Result := False;
    Exit;
  end;
  Result := CheckDirFilesLocked(NormalizedDir);
end;

function AreAppFilesLocked(): Boolean;
begin
  Result := AreAppFilesLockedInDir(ExpandConstant('{app}'));
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
     IsFileLocked(AppDir + '\Tools3000_Service.exe') or
     IsFileLocked(AppDir + '\Tools3000_Picasa_Viewer.exe') then
  begin
    Result := True;
    Exit;
  end;
  // 遍历 app 及 plugins 目录全量动态库
  Result := AreAppFilesLocked();
end;

function IsTools3000Running(): Boolean;
begin
  // 1. 会话级 (Local\) 与旧版全局 (Global\) 单实例互斥体秒级探测 (0.001ms)
  if CheckForMutexes('Local\Tools3000_SingleInstance_Mutex') or
     CheckForMutexes('Global\Tools3000_SingleInstance_Mutex') or
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

procedure StopSearchServiceNativeForDir(const TargetAppDir: String);
var
  hSCM, hSvc: THandle;
  Status: TServiceStatus;
  Dummy: Integer;
  ServiceExePath: String;
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
          Log('StopSearchServiceNativeForDir: Sent SERVICE_CONTROL_STOP via Win32 SCM API.');
        finally
          CloseServiceHandle(hSvc);
        end;
      end;
    finally
      CloseServiceHandle(hSCM);
    end;
  end;
  ServiceExePath := '';
  if Trim(TargetAppDir) <> '' then
    ServiceExePath := RemoveBackslashUnlessRoot(Trim(TargetAppDir)) + '\Tools3000_Service.exe';
  // 仅当服务仍在运行或服务可执行文件仍锁定时才执行兜底 taskkill，绝不在已停止状态下空转创建外部进程
  if IsSearchServiceRunning() or ((ServiceExePath <> '') and IsFileLocked(ServiceExePath)) then
  begin
    Log('StopSearchServiceNativeForDir: Service process still active, issuing taskkill...');
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Service.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
  end;
end;

procedure StopSearchServiceNative();
begin
  StopSearchServiceNativeForDir(ExpandConstant('{app}'));
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

function GetInteractiveUserHint(): String;
var
  LogonUser: String;
  SlashPos: Integer;
begin
  Result := '';
  if RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\LogonUI', 'LastLoggedOnSAMUser', LogonUser) or
     RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\LogonUI', 'LastLoggedOnUser', LogonUser) then
  begin
    SlashPos := Pos('\', LogonUser);
    if SlashPos > 0 then
      Delete(LogonUser, 1, SlashPos);
    Result := Trim(LogonUser);
  end;
end;

procedure CleanPicasaAssocInHive(RootKey: Integer; const HivePrefix: String);
var
  BasePrefix, Remaining, Ext, DotExt: String;
  ExtClassKey, OwpKey, BackupRootKey, BackupExtKey: String;
  CurDefault, BackupVal: String;
  CommaPos: Integer;
begin
  BasePrefix := HivePrefix;
  if (BasePrefix <> '') and (BasePrefix[Length(BasePrefix)] <> '\') then
    BasePrefix := BasePrefix + '\';

  BackupRootKey := BasePrefix + 'Software\Tools3000\PicasaViewer\BackupAssoc';

  // 遍历全部 41 个支持的图片扩展名：清理 OpenWithProgids 并还原或删除默认值（绝不留空串遮蔽 HKLM）
  Remaining := PICASA_SUPPORTED_EXTS;
  while Remaining <> '' do
  begin
    CommaPos := Pos(',', Remaining);
    if CommaPos > 0 then
    begin
      Ext := Copy(Remaining, 1, CommaPos - 1);
      Delete(Remaining, 1, CommaPos);
    end
    else
    begin
      Ext := Remaining;
      Remaining := '';
    end;
    Ext := Trim(Ext);
    if Ext = '' then Continue;

    DotExt := '.' + Ext;
    ExtClassKey := BasePrefix + 'Software\Classes\' + DotExt;
    OwpKey := ExtClassKey + '\OpenWithProgids';
    BackupExtKey := BackupRootKey + '\' + DotExt;

    RegDeleteValue(RootKey, OwpKey, PICASA_PROG_ID);

    CurDefault := '';
    if RegQueryStringValue(RootKey, ExtClassKey, '', CurDefault) and
       (CompareText(Trim(CurDefault), PICASA_PROG_ID) = 0) then
    begin
      BackupVal := '';
      // 优先从 Software\Tools3000\PicasaViewer\BackupAssoc\.ext 还原原关联值，同时兼容父键同名字符串值
      if (not RegQueryStringValue(RootKey, BackupExtKey, '', BackupVal)) or (Trim(BackupVal) = '') then
        RegQueryStringValue(RootKey, BackupRootKey, DotExt, BackupVal);

      BackupVal := Trim(BackupVal);
      if (BackupVal <> '') and (CompareText(BackupVal, PICASA_PROG_ID) <> 0) then
        RegWriteStringValue(RootKey, ExtClassKey, '', BackupVal)
      else
        RegDeleteValue(RootKey, ExtClassKey, '');
    end;
  end;

  // 全部 41 个扩展名清理还原完毕后，再删除 Picasa 看图器配置与 ProgID 主键
  RegDeleteValue(RootKey, BasePrefix + 'Software\RegisteredApplications', 'Tools3000 Picasa Viewer');
  RegDeleteKeyIncludingSubkeys(RootKey, BasePrefix + 'Software\Tools3000\PicasaViewer');
  RegDeleteKeyIncludingSubkeys(RootKey, BasePrefix + 'Software\Classes\' + PICASA_PROG_ID);
  RegDeleteKeyIncludingSubkeys(RootKey, BasePrefix + 'Software\Classes\Applications\Tools3000_Picasa_Viewer.exe');
end;

procedure CleanScheduledTasksAndRegistries();
var
  TaskService, RootFolder, ToolsFolder, Tasks, TaskItem: Variant;
  I, Dummy: Integer;
  ComSuccess: Boolean;
  InteractiveUser: String;
  SubKeys: TArrayOfString;
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
    InteractiveUser := GetInteractiveUserHint();
    if (InteractiveUser <> '') and (CompareText(InteractiveUser, GetUserNameString()) <> 0) then
      Exec(ExpandConstant('{sys}\schtasks.exe'), '/delete /tn "Tools3000\Autorun for ' + InteractiveUser + '" /f', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
  end;

  // 清除当前用户及多账户 UAC 场景下所有已加载用户配置单元的自启动项 (HKCU / HKU Run)、File Locksmith 右键菜单与 Picasa 看图器全部 41 格式关联
  RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Tools3000');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\*\shell\Tools3000.FileLocksmith');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Directory\shell\Tools3000.FileLocksmith');
  CleanPicasaAssocInHive(HKCU, '');
  if RegGetSubkeyNames(HKU, '', SubKeys) then
  begin
    for I := 0 to GetArrayLength(SubKeys) - 1 do
    begin
      if (Length(SubKeys[I]) > 8) and
         (CompareText(Copy(SubKeys[I], Length(SubKeys[I]) - 7, 8), '_Classes') = 0) then
        Continue;
      RegDeleteValue(HKU, SubKeys[I] + '\Software\Microsoft\Windows\CurrentVersion\Run', 'Tools3000');
      RegDeleteKeyIncludingSubkeys(HKU, SubKeys[I] + '\Software\Classes\*\shell\Tools3000.FileLocksmith');
      RegDeleteKeyIncludingSubkeys(HKU, SubKeys[I] + '\Software\Classes\Directory\shell\Tools3000.FileLocksmith');
      CleanPicasaAssocInHive(HKU, SubKeys[I]);
    end;
  end;
end;

procedure StopAndKillAllTools3000ProcessesForDir(const TargetAppDir: String);
var
  ResultCode: Integer;
  MsgHwnd: LongWord;
  NeedKillApp: Boolean;
  NeedKillService: Boolean;
  FilesLocked: Boolean;
  ExistingInstallPresent: Boolean;
  NormDir: String;
  GraceStart: DWORD;
begin
  NormDir := RemoveBackslashUnlessRoot(Trim(TargetAppDir));
  ExistingInstallPresent := (NormDir <> '') and
                            (FileExists(NormDir + '\Tools3000.exe') or FileExists(NormDir + '\Tools3000_Service.exe'));
  NeedKillApp := IsTools3000Running();
  NeedKillService := IsSearchServiceRunning();
  FilesLocked := AreAppFilesLockedInDir(NormDir);

  // 全新安装（目标目录尚不存在旧版可执行文件）且全局无活跃实例时快速跳过；
  // 只要是覆盖升级或卸载（ExistingInstallPresent = True），无条件执行清场终止，彻底消除跨会话/跨桌面互斥体盲区漏杀！
  if (not ExistingInstallPresent) and (not NeedKillApp) and (not NeedKillService) and (not FilesLocked) then
  begin
    Log('StopAndKillAllTools3000ProcessesForDir: Fresh target dir with no active Tools3000 instances, skipping process kills.');
    Exit;
  end;

  Log(Format('StopAndKillAllTools3000ProcessesForDir: Unconditionally clearing processes (existing=%d, app=%d, service=%d, locked=%d)...', [Ord(ExistingInstallPresent), Ord(NeedKillApp), Ord(NeedKillService), Ord(FilesLocked)]));

  MsgHwnd := FindWindowByClassName('Tools3000_MessageWindow');
  if MsgHwnd <> 0 then
  begin
    PostMessage(MsgHwnd, WM_CLOSE_MSG, 0, 0);
    // 先给予主程序最多 2500ms 优雅退场窗口（实测仅 ~250ms），干净卸载钩子、保存配置并释放 WebView2 与 DLL 锁
    GraceStart := GetTickCount();
    while IsTools3000Running() and ((GetTickCount() - GraceStart) < 2500) do
    begin
      Sleep(40);
    end;
    Log(Format('StopAndKillAllTools3000ProcessesForDir: Graceful exit wait finished in %d ms (stillRunning=%d).', [GetTickCount() - GraceStart, Ord(IsTools3000Running())]));
  end;

  // 关键防自杀铁律：针对 Tools3000.exe 绝严禁加 /t（终止进程树）！
  // 应用内一键更新时，Setup.exe / Setup.tmp 可能是旧版 Tools3000.exe 的子进程；加 /t 会沿 PPID 进程树把安装器自己当场强杀自爆！
  // WebView2 等受管子进程已绑定至 Tools3000.exe 的 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE Job，主进程退出后由内核自动清零。
  if IsTools3000Running() or AreAppFilesLockedInDir(NormDir) then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im Tools3000.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Picasa_Viewer.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im tools3000-cli.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  StopSearchServiceNativeForDir(NormDir);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Service.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure StopAndKillAllTools3000Processes();
begin
  StopAndKillAllTools3000ProcessesForDir(ExpandConstant('{app}'));
end;

function WaitForProcessesAndFilesReleasedInDir(const TargetAppDir: String; MaxWaitMs: Integer): Boolean;
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
       (not AreAppFilesLockedInDir(TargetAppDir)) then
    begin
      Elapsed := GetTickCount() - StartTick;
      Log(Format('WaitForProcessesAndFilesReleasedInDir: All processes exited and file locks released in %d ms', [Elapsed]));
      Result := True;
      Exit;
    end;

    Elapsed := GetTickCount() - StartTick;
    if Elapsed >= DWORD(MaxWaitMs) then
      Break;

    Sleep(30);

    // 超过 500ms 仍未释放时周期性补发 taskkill 确保彻底干掉残留进程（针对 Tools3000.exe 严禁带 /t 防自杀）
    if (Elapsed > 500) and ((Elapsed mod 500) < 40) then
    begin
      Log('WaitForProcessesAndFilesReleasedInDir: Locks or processes still active, re-issuing taskkill...');
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im Tools3000.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Picasa_Viewer.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im tools3000-cli.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Service.exe', '', SW_HIDE, ewWaitUntilTerminated, Dummy);
    end;
  end;

  Result := (not IsTools3000Running()) and
            (not IsSearchServiceRunning()) and
            (not AreAppFilesLockedInDir(TargetAppDir));
  Elapsed := GetTickCount() - StartTick;
  if Result then
    Log(Format('WaitForProcessesAndFilesReleasedInDir: Released at final check (%d ms)', [Elapsed]))
  else
    Log(Format('WaitForProcessesAndFilesReleasedInDir: Timed out after %d ms. Files or processes may still be locked.', [Elapsed]));
end;

function WaitForProcessesAndFilesReleased(MaxWaitMs: Integer): Boolean;
begin
  Result := WaitForProcessesAndFilesReleasedInDir(ExpandConstant('{app}'), MaxWaitMs);
end;

// ============================================================================
// 安装前 PendingFileRenameOperations (PFRO) 精确成对自愈引擎
// 说明：Inno Setup 在进入 wpPreparing 瞬间（早于 PrepareToInstall()）会扫描
//       HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations(2)。
//       若上次卸载/升级有任意旧文件短暂被占用写入了 PFRO，即使此刻早已解锁，
//       Inno Setup 也会直接阻断并强制要求重启电脑。
//       本引擎严格在 wpReady 点击“安装”瞬间（进入 wpPreparing 之前）仅针对目标
//       Tools3000 目录进行现场解锁与精准成对摘除，100% 字节无损保留系统及其他软件条目。
// ============================================================================

function ReadExactMultiStringValue(Win32RootKey: Cardinal; const SubKey, ValueName: String; var RawPayload: String): Boolean;
var
  hKey: THandle;
  ValType, CbData: DWORD;
  Res: LongInt;
begin
  Result := False;
  RawPayload := '';
  Res := RegOpenKeyExW(Win32RootKey, SubKey, 0, KEY_QUERY_VALUE_FLAG or KEY_WOW64_64KEY_FLAG, hKey);
  if Res <> 0 then Exit;
  try
    ValType := 0;
    CbData := 0;
    Res := RegQueryValueExWLen(hKey, ValueName, 0, ValType, 0, CbData);
    if (Res <> 0) or (ValType <> REG_MULTI_SZ_TYPE) or (CbData < 2) then Exit;
    SetLength(RawPayload, (CbData + 1) div 2);
    Res := RegQueryValueExWStr(hKey, ValueName, 0, ValType, RawPayload, CbData);
    if (Res = 0) and (ValType = REG_MULTI_SZ_TYPE) then
    begin
      SetLength(RawPayload, CbData div 2);
      Result := True;
    end;
  finally
    RegCloseKey(hKey);
  end;
end;

function WriteExactMultiStringValue(Win32RootKey: Cardinal; const SubKey, ValueName, MultiStrPayload: String): Boolean;
var
  hKey: THandle;
  Payload: String;
  Res: LongInt;
begin
  Result := False;
  // REG_MULTI_SZ UTF-16LE 精确格式：每条字符串（含空目标串）以 #0 终止，列表末尾追加 1 个 #0 终止符
  Payload := MultiStrPayload + #0;
  Res := RegOpenKeyExW(Win32RootKey, SubKey, 0, KEY_SET_VALUE_FLAG or KEY_WOW64_64KEY_FLAG, hKey);
  if Res <> 0 then
  begin
    Log(Format('WriteExactMultiStringValue: RegOpenKeyExW failed (%d) for %s', [Res, ValueName]));
    Exit;
  end;
  try
    Res := RegSetValueExWStr(hKey, ValueName, 0, REG_MULTI_SZ_TYPE, Payload, DWORD(Length(Payload) * 2));
    Result := (Res = 0);
    if not Result then
      Log(Format('WriteExactMultiStringValue: RegSetValueExW failed (%d) for %s', [Res, ValueName]));
  finally
    RegCloseKey(hKey);
  end;
end;

function DeleteExactRegValue(Win32RootKey: Cardinal; const SubKey, ValueName: String): Boolean;
var
  hKey: THandle;
  Res: LongInt;
begin
  Result := False;
  Res := RegOpenKeyExW(Win32RootKey, SubKey, 0, KEY_SET_VALUE_FLAG or KEY_WOW64_64KEY_FLAG, hKey);
  if Res <> 0 then Exit;
  try
    Res := RegDeleteValueW(hKey, ValueName);
    Result := (Res = 0);
  finally
    RegCloseKey(hKey);
  end;
end;

function ParseExactMultiStringItems(const RawPayload: String; var Items: TArrayOfString): Integer;
var
  EffLen, I, N: Integer;
  Token: String;
begin
  N := 0;
  SetArrayLength(Items, 0);
  EffLen := Length(RawPayload);
  if EffLen = 0 then
  begin
    Result := 0;
    Exit;
  end;

  // 去掉 REG_MULTI_SZ 末尾最后一个代表整个列表结束的 #0，完整保留末尾空串项
  if RawPayload[EffLen] = #0 then
    EffLen := EffLen - 1;

  Token := '';
  for I := 1 to EffLen do
  begin
    if RawPayload[I] = #0 then
    begin
      SetArrayLength(Items, N + 1);
      Items[N] := Token;
      N := N + 1;
      Token := '';
    end
    else
      Token := Token + RawPayload[I];
  end;

  if Token <> '' then
  begin
    SetArrayLength(Items, N + 1);
    Items[N] := Token;
    N := N + 1;
  end;

  // 若遭遇奇数字节尾残留单个空串，对齐为合法 (Src, Dst) 成对结构
  if ((N mod 2) = 1) and (Items[N - 1] = '') then
  begin
    N := N - 1;
    SetArrayLength(Items, N);
  end;

  Result := N;
end;

function CleanPfroPath(const Raw: String): String;
var
  S: String;
  P: Integer;
begin
  S := Trim(Raw);
  P := Pos('\??\', S);
  if P > 0 then
    Delete(S, 1, P + 3)
  else
  begin
    P := Pos('\\?\', S);
    if P > 0 then
      Delete(S, 1, P + 3);
  end;
  StringChangeEx(S, '/', '\', True);
  Result := RemoveBackslashUnlessRoot(S);
end;

function IsSafeTools3000TargetDir(const DirPath: String): Boolean;
var
  NormLower: String;
begin
  NormLower := Lowercase(RemoveBackslashUnlessRoot(Trim(DirPath)));
  // 安全白名单守卫：必须是绝对盘符目录、深度合理且包含 tools3000，严防误伤系统/根目录
  Result := (Length(NormLower) >= 10) and
            (Pos(':\', NormLower) = 2) and
            (Pos('tools3000', NormLower) > 0);
end;

function IsPathUnderOrEqualTargetDir(const RawPfroPath, TargetLower, ShortLower: String): Boolean;
var
  CleanLower: String;
begin
  Result := False;
  if RawPfroPath = '' then Exit;
  CleanLower := Lowercase(CleanPfroPath(RawPfroPath));
  if (CleanLower = TargetLower) or (Pos(TargetLower + '\', CleanLower) = 1) then
  begin
    Result := True;
    Exit;
  end;
  if (ShortLower <> '') and (ShortLower <> TargetLower) and (Length(ShortLower) >= 10) then
  begin
    if (CleanLower = ShortLower) or (Pos(ShortLower + '\', CleanLower) = 1) then
      Result := True;
  end;
end;

function IsPfroPairOwnedByTargetDir(const Src, Dst, TargetLower, ShortLower: String): Boolean;
begin
  if Dst = '' then
    Result := IsPathUnderOrEqualTargetDir(Src, TargetLower, ShortLower)
  else
    // 重命名/替换对严格要求目标文件必须位于 Tools3000 目录内，绝不触碰跨向外部目录的条目
    Result := IsPathUnderOrEqualTargetDir(Dst, TargetLower, ShortLower) and
              (IsPathUnderOrEqualTargetDir(Src, TargetLower, ShortLower) or
               (Lowercase(ExtractFileExt(CleanPfroPath(Src))) = '.tmp'));
end;

function TryResolveStalePfroPair(const Src, Dst: String): Boolean;
var
  CleanSrc, CleanDst: String;
begin
  Result := False;
  CleanSrc := CleanPfroPath(Src);
  CleanDst := CleanPfroPath(Dst);

  if Dst = '' then
  begin
    // 待重启删除条目：目录清理或尝试现场删除旧文件；已不存在视为僵尸条目直接摘除
    if DirExists(CleanSrc) then
    begin
      RemoveDir(CleanSrc);
      Result := True;
      Exit;
    end;
    if FileExists(CleanSrc) then
    begin
      DeleteFile(CleanSrc);
      Result := not FileExists(CleanSrc);
      Exit;
    end;
    Result := True;
  end
  else
  begin
    // 待重启替换条目（如上次残留 is-XXXXX.tmp -> Tools3000Core.dll）：
    // 先清理过时临时文件，仅当临时文件已移除且目标文件当前未被进程锁定时才摘除
    if FileExists(CleanSrc) then
      DeleteFile(CleanSrc);
    if (not FileExists(CleanSrc)) and (not IsFileLocked(CleanDst)) then
      Result := True;
  end;
end;

procedure ClearStaleAppPendingFileRenamesInValue(
  const TargetAppDir, TargetLower, ShortLower, ValueName: String;
  var KilledOnce: Boolean);
var
  RawVal, Src, Dst, NewPayload: String;
  Items: TArrayOfString;
  N, I, KeptPairs, RemovedPairs, RetainedLocked: Integer;
  HasTargetMatch: Boolean;
begin
  if not ReadExactMultiStringValue(WIN32_HKEY_LOCAL_MACHINE, SESSION_MANAGER_SUBKEY, ValueName, RawVal) then
    Exit;

  N := ParseExactMultiStringItems(RawVal, Items);
  if N < 2 then Exit;

  // 第一遍只读快速扫描：若无 Tools3000 相关残留条目，零开销直接放行
  HasTargetMatch := False;
  I := 0;
  while I + 1 < N do
  begin
    if IsPfroPairOwnedByTargetDir(Items[I], Items[I + 1], TargetLower, ShortLower) then
    begin
      HasTargetMatch := True;
      Break;
    end;
    I := I + 2;
  end;

  if not HasTargetMatch then Exit;

  Log(Format('ClearStaleAppPendingFileRenames: Detected stale Tools3000 entries in %s (total items=%d). Attempting self-heal...', [ValueName, N]));

  // 仅在首次命中 PFRO 残留时终止残留进程并等待句柄释放，确保旧文件可被现场安全清理
  if not KilledOnce then
  begin
    KilledOnce := True;
    StopAndKillAllTools3000ProcessesForDir(TargetAppDir);
    WaitForProcessesAndFilesReleasedInDir(TargetAppDir, 3000);
  end;

  NewPayload := '';
  KeptPairs := 0;
  RemovedPairs := 0;
  RetainedLocked := 0;
  I := 0;
  while I + 1 < N do
  begin
    Src := Items[I];
    Dst := Items[I + 1];
    if IsPfroPairOwnedByTargetDir(Src, Dst, TargetLower, ShortLower) then
    begin
      if TryResolveStalePfroPair(Src, Dst) then
      begin
        RemovedPairs := RemovedPairs + 1;
        Log(Format('ClearStaleAppPendingFileRenames: Cleared stale pair [%s] -> [%s]', [Src, Dst]));
      end
      else
      begin
        NewPayload := NewPayload + Src + #0 + Dst + #0;
        KeptPairs := KeptPairs + 1;
        RetainedLocked := RetainedLocked + 1;
        Log(Format('ClearStaleAppPendingFileRenames: Retained actively locked pair [%s] -> [%s]', [Src, Dst]));
      end;
    end
    else
    begin
      NewPayload := NewPayload + Src + #0 + Dst + #0;
      KeptPairs := KeptPairs + 1;
    end;
    I := I + 2;
  end;

  if RemovedPairs > 0 then
  begin
    if KeptPairs = 0 then
    begin
      if DeleteExactRegValue(WIN32_HKEY_LOCAL_MACHINE, SESSION_MANAGER_SUBKEY, ValueName) then
        Log(Format('ClearStaleAppPendingFileRenames: Removed all %d stale Tools3000 pairs and deleted empty %s.', [RemovedPairs, ValueName]));
    end
    else
    begin
      if WriteExactMultiStringValue(WIN32_HKEY_LOCAL_MACHINE, SESSION_MANAGER_SUBKEY, ValueName, NewPayload) then
        Log(Format('ClearStaleAppPendingFileRenames: Updated %s (removed=%d, kept=%d, retainedLocked=%d).', [ValueName, RemovedPairs, KeptPairs, RetainedLocked]));
    end;
  end;
end;

procedure ClearStaleAppPendingFileRenames(const TargetAppDir: String);
var
  NormalizedDir, TargetLower, ShortLower: String;
  KilledOnce: Boolean;
begin
  NormalizedDir := RemoveBackslashUnlessRoot(Trim(TargetAppDir));
  if not IsSafeTools3000TargetDir(NormalizedDir) then
  begin
    Log('ClearStaleAppPendingFileRenames: Skipped due to non-Tools3000 target dir: ' + NormalizedDir);
    Exit;
  end;

  TargetLower := Lowercase(NormalizedDir);
  ShortLower := '';
  if DirExists(NormalizedDir) then
    ShortLower := Lowercase(RemoveBackslashUnlessRoot(GetShortName(NormalizedDir)));

  KilledOnce := False;
  ClearStaleAppPendingFileRenamesInValue(NormalizedDir, TargetLower, ShortLower, 'PendingFileRenameOperations', KilledOnce);
  ClearStaleAppPendingFileRenamesInValue(NormalizedDir, TargetLower, ShortLower, 'PendingFileRenameOperations2', KilledOnce);
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
function HasCommandLinePrefix(const Prefix: String): Boolean; forward;
function FindExistingUserConfigPath(): String; forward;

// ── 轻量 JSON 布尔值定位解析器（按对象层级精确扫描，严防嵌套同名键误匹配） ──
function IsJsonWhitespace(Ch: Char): Boolean;
begin
  Result := (Ch = ' ') or (Ch = #9) or (Ch = #10) or (Ch = #13);
end;

procedure JsonSkipWhitespace(const S: String; var P: Integer);
var
  Len: Integer;
begin
  Len := Length(S);
  while (P <= Len) and IsJsonWhitespace(S[P]) do
    P := P + 1;
end;

function JsonReadString(const S: String; var P: Integer; var OutStr: String): Boolean;
var
  Len: Integer;
  Ch: Char;
begin
  Result := False;
  OutStr := '';
  Len := Length(S);
  JsonSkipWhitespace(S, P);
  if (P > Len) or (S[P] <> '"') then Exit;
  P := P + 1;
  while P <= Len do
  begin
    Ch := S[P];
    if Ch = '\' then
    begin
      if P + 1 > Len then Exit;
      OutStr := OutStr + S[P + 1];
      P := P + 2;
    end
    else if Ch = '"' then
    begin
      P := P + 1;
      Result := True;
      Exit;
    end
    else
    begin
      OutStr := OutStr + Ch;
      P := P + 1;
    end;
  end;
end;

function JsonSkipValue(const S: String; var P: Integer): Boolean;
var
  Len, Depth: Integer;
  DummyStr: String;
  InStr, Escaped: Boolean;
  OpenCh, CloseCh, Ch: Char;
begin
  Result := False;
  Len := Length(S);
  JsonSkipWhitespace(S, P);
  if P > Len then Exit;

  if S[P] = '"' then
  begin
    Result := JsonReadString(S, P, DummyStr);
    Exit;
  end;

  if (S[P] = '{') or (S[P] = '[') then
  begin
    OpenCh := S[P];
    if OpenCh = '{' then CloseCh := '}' else CloseCh := ']';
    Depth := 0;
    InStr := False;
    Escaped := False;
    while P <= Len do
    begin
      Ch := S[P];
      P := P + 1;
      if InStr then
      begin
        if Escaped then
          Escaped := False
        else if Ch = '\' then
          Escaped := True
        else if Ch = '"' then
          InStr := False;
      end
      else
      begin
        if Ch = '"' then
          InStr := True
        else if Ch = OpenCh then
          Depth := Depth + 1
        else if Ch = CloseCh then
        begin
          Depth := Depth - 1;
          if Depth = 0 then
          begin
            Result := True;
            Exit;
          end;
        end;
      end;
    end;
    Exit;
  end;

  // 原子字面量（true / false / null / number）
  while (P <= Len) and (not IsJsonWhitespace(S[P])) and
        (S[P] <> ',') and (S[P] <> '}') and (S[P] <> ']') do
    P := P + 1;
  Result := True;
end;

function JsonFindObjectMember(const S: String; ObjStart: Integer; const TargetKey: String; var ValStart: Integer): Boolean;
var
  P, Len: Integer;
  KeyName: String;
begin
  Result := False;
  ValStart := 0;
  Len := Length(S);
  P := ObjStart;
  JsonSkipWhitespace(S, P);
  if (P > Len) or (S[P] <> '{') then Exit;
  P := P + 1;

  while P <= Len do
  begin
    JsonSkipWhitespace(S, P);
    if (P > Len) or (S[P] = '}') then Exit;
    if not JsonReadString(S, P, KeyName) then Exit;
    JsonSkipWhitespace(S, P);
    if (P > Len) or (S[P] <> ':') then Exit;
    P := P + 1;
    JsonSkipWhitespace(S, P);
    if P > Len then Exit;

    if KeyName = TargetKey then
    begin
      ValStart := P;
      Result := True;
      Exit;
    end;

    if not JsonSkipValue(S, P) then Exit;
    JsonSkipWhitespace(S, P);
    if (P <= Len) and (S[P] = ',') then
      P := P + 1
    else if (P <= Len) and (S[P] = '}') then
      Exit
    else
      Exit;
  end;
end;

function IsJsonTokenBoundary(const S: String; NextPos: Integer): Boolean;
var
  Ch: Char;
begin
  if NextPos > Length(S) then
  begin
    Result := True;
    Exit;
  end;
  Ch := S[NextPos];
  Result := IsJsonWhitespace(Ch) or (Ch = ',') or (Ch = '}') or (Ch = ']');
end;

function JsonReadBoolAt(const S: String; ValStart: Integer; var OutVal: Boolean): Boolean;
var
  P: Integer;
begin
  Result := False;
  P := ValStart;
  JsonSkipWhitespace(S, P);
  if (Copy(S, P, 4) = 'true') and IsJsonTokenBoundary(S, P + 4) then
  begin
    OutVal := True;
    Result := True;
  end
  else if (Copy(S, P, 5) = 'false') and IsJsonTokenBoundary(S, P + 5) then
  begin
    OutVal := False;
    Result := True;
  end;
end;

function FindJsonRootObjectStart(const S: String): Integer;
var
  P, Len: Integer;
begin
  Result := 0;
  Len := Length(S);
  P := 1;
  // 跳过可能存在的 UTF-8 BOM
  if (Len >= 3) and (Ord(S[1]) = $EF) and (Ord(S[2]) = $BB) and (Ord(S[3]) = $BF) then
    P := 4;
  JsonSkipWhitespace(S, P);
  if (P <= Len) and (S[P] = '{') then
    Result := P;
end;

function TryReadJsonBool2(const JsonText, Key1, Key2: String; var OutVal: Boolean): Boolean;
var
  RootPos, SecPos, ValPos: Integer;
begin
  Result := False;
  RootPos := FindJsonRootObjectStart(JsonText);
  if RootPos = 0 then Exit;
  if not JsonFindObjectMember(JsonText, RootPos, Key1, SecPos) then Exit;
  if not JsonFindObjectMember(JsonText, SecPos, Key2, ValPos) then Exit;
  Result := JsonReadBoolAt(JsonText, ValPos, OutVal);
end;

function TryReadJsonBool3(const JsonText, Key1, Key2, Key3: String; var OutVal: Boolean): Boolean;
var
  RootPos, Sec1Pos, Sec2Pos, ValPos: Integer;
begin
  Result := False;
  RootPos := FindJsonRootObjectStart(JsonText);
  if RootPos = 0 then Exit;
  if not JsonFindObjectMember(JsonText, RootPos, Key1, Sec1Pos) then Exit;
  if not JsonFindObjectMember(JsonText, Sec1Pos, Key2, Sec2Pos) then Exit;
  if not JsonFindObjectMember(JsonText, Sec2Pos, Key3, ValPos) then Exit;
  Result := JsonReadBoolAt(JsonText, ValPos, OutVal);
end;

procedure ApplyComponentSelectionFlag(const CompName: String; Enabled: Boolean);
begin
  if Enabled then
    WizardSelectComponents(CompName)
  else
    WizardSelectComponents('!' + CompName);
end;

procedure SnapshotInitialComponentSelections();
begin
  InitialCompSearch := WizardIsComponentSelected('search');
  InitialCompCapture := WizardIsComponentSelected('capture');
  InitialCompOcrPack := WizardIsComponentSelected('capture\ocr_models');
  InitialCompGesture := WizardIsComponentSelected('gesture');
  InitialCompKeycast := WizardIsComponentSelected('keycast');
  InitialCompDialog := WizardIsComponentSelected('dialogenhancer');
  InitialCompSpotlight := WizardIsComponentSelected('spotlight');
  InitialCompRemote := WizardIsComponentSelected('remote');
  InitialCompPicasaViewer := WizardIsComponentSelected('picasaviewer');
end;

function HaveSelectedComponentsChanged(): Boolean;
begin
  Result :=
    (WizardIsComponentSelected('search') <> InitialCompSearch) or
    (WizardIsComponentSelected('capture') <> InitialCompCapture) or
    (WizardIsComponentSelected('capture\ocr_models') <> InitialCompOcrPack) or
    (WizardIsComponentSelected('gesture') <> InitialCompGesture) or
    (WizardIsComponentSelected('keycast') <> InitialCompKeycast) or
    (WizardIsComponentSelected('dialogenhancer') <> InitialCompDialog) or
    (WizardIsComponentSelected('spotlight') <> InitialCompSpotlight) or
    (WizardIsComponentSelected('remote') <> InitialCompRemote) or
    (WizardIsComponentSelected('picasaviewer') <> InitialCompPicasaViewer);
end;

procedure InheritExistingModuleSelectionsFromConfig();
var
  ConfigPath, JsonText: String;
  RawAnsi: AnsiString;
  FlagVal: Boolean;
begin
  HasExistingUserConfig := False;
  ConfigPath := FindExistingUserConfigPath();
  if ConfigPath = '' then Exit;

  HasExistingUserConfig := True;
  Log('InitializeWizard: Existing user config detected -> ' + ConfigPath);

  // 若命令行显式指定了 /COMPONENTS= 或 /TYPE=，严格尊重命令行参数
  if HasCommandLinePrefix('/components=') or HasCommandLinePrefix('/type=') then
    Exit;

  if not LoadStringFromFile(ConfigPath, RawAnsi) then
  begin
    Log('InitializeWizard: Failed to read existing config.json, keeping default component selections.');
    Exit;
  end;
  JsonText := String(RawAnsi);

  if TryReadJsonBool3(JsonText, 'plugins', 'search', 'enabled', FlagVal) or
     TryReadJsonBool2(JsonText, 'search', 'enabled', FlagVal) then
    ApplyComponentSelectionFlag('search', FlagVal);

  if TryReadJsonBool3(JsonText, 'plugins', 'capture', 'enabled', FlagVal) then
    ApplyComponentSelectionFlag('capture', FlagVal);

  if WizardIsComponentSelected('capture') and
     TryReadJsonBool2(JsonText, 'ocr', 'autoDownloadPackOnStartup', FlagVal) then
    ApplyComponentSelectionFlag('capture\ocr_models', FlagVal);

  if TryReadJsonBool3(JsonText, 'plugins', 'gesture', 'enabled', FlagVal) or
     TryReadJsonBool2(JsonText, 'gesture', 'enabled', FlagVal) then
    ApplyComponentSelectionFlag('gesture', FlagVal);

  if TryReadJsonBool3(JsonText, 'plugins', 'keycast', 'enabled', FlagVal) or
     TryReadJsonBool2(JsonText, 'general', 'keycastEnabled', FlagVal) then
    ApplyComponentSelectionFlag('keycast', FlagVal);

  if TryReadJsonBool3(JsonText, 'plugins', 'dialogenhancer', 'enabled', FlagVal) or
     TryReadJsonBool2(JsonText, 'dialog', 'enabled', FlagVal) then
    ApplyComponentSelectionFlag('dialogenhancer', FlagVal);

  if TryReadJsonBool3(JsonText, 'plugins', 'spotlight', 'enabled', FlagVal) or
     TryReadJsonBool2(JsonText, 'spotlight', 'enabled', FlagVal) then
    ApplyComponentSelectionFlag('spotlight', FlagVal);

  if TryReadJsonBool3(JsonText, 'plugins', 'remote_boost', 'enabled', FlagVal) or
     TryReadJsonBool2(JsonText, 'remote_boost', 'enabled', FlagVal) then
    ApplyComponentSelectionFlag('remote', FlagVal);

  // 仅当已有配置明确启用了 picasa_viewer 时预勾（新装或旧配置未开启时保持默认不勾选）
  if TryReadJsonBool3(JsonText, 'plugins', 'picasa_viewer', 'enabled', FlagVal) or
     TryReadJsonBool2(JsonText, 'picasa_viewer', 'enabled', FlagVal) then
  begin
    ApplyComponentSelectionFlag('picasaviewer', FlagVal);
    if FlagVal then
      Log('InitializeWizard: Preserved existing enabled state for optional component picasaviewer.');
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

// 抢在 Inno Setup 内置 wpPreparing (Windows Restart Manager) 扫描之前主动完成旧进程优雅退出与僵死清场：
// 1. 彻底向下兼容 v1.4.10 及更早版本应用内更新传入的 /CLOSEAPPLICATIONS 参数；
// 2. 避免旧版在 CoUninitialize() 阶段僵死后无窗口响应 Restart Manager，弹出“安装程序无法自动关闭所有应用程序”错误框。
procedure PreClearProcessesBeforeRestartManager(const TargetAppDir: String);
var
  NormDir: String;
begin
  NormDir := RemoveBackslashUnlessRoot(Trim(TargetAppDir));
  if NormDir = '' then Exit;
  Log(Format('PreClearProcessesBeforeRestartManager: Clearing running/stale Tools3000 processes before wpPreparing for %s...', [NormDir]));
  StopAndKillAllTools3000ProcessesForDir(NormDir);
  WaitForProcessesAndFilesReleasedInDir(NormDir, 3000);
  ClearStaleAppPendingFileRenames(NormDir);
end;

procedure InitializeWizard();
begin
  ApplyComponentsListStyles();
  WizardForm.TasksList.ShowLines := False;
  UserVisitedComponentsPage := False;

  // 覆盖升级继承已有用户模块开关（新装无旧配置时可选组件 picasaviewer 严格保持默认不勾选）
  InheritExistingModuleSelectionsFromConfig();
  SnapshotInitialComponentSelections();

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

  // 静默安装或应用内一键自动更新时，在进入 wpPreparing / Restart Manager 之前先完成清场与 PFRO 自愈
  // （非静默交互向导延后至用户于 wpReady 明确点击“安装”时执行，避免取消安装误杀正在运行的实例）
  if WizardSilent or HasCommandLineParameter('/AUTOUPDATE') or HasCommandLineParameter('/CLOSEAPPLICATIONS') then
  begin
    PreClearProcessesBeforeRestartManager(WizardDirValue);
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  // 在用户点击“安装”(wpReady) 进入 wpPreparing 之前瞬间执行进程清场与 PFRO 自愈清理，
  // 抢在 Inno Setup 内置 wpPreparing Restart Manager / PendingFileRenameOperations 门禁之前消除锁定与重启拦截。
  if CurPageID = wpReady then
  begin
    PreClearProcessesBeforeRestartManager(WizardDirValue);
    ClearStaleAppPendingFileRenames(WizardDirValue);
  end;
  Result := True;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectComponents then
  begin
    UserVisitedComponentsPage := True;
  end;
  if (CurPageID = wpSelectComponents) or (CurPageID = wpSelectTasks) then
  begin
    ApplyComponentsListStyles();
  end;
  // 终极防呆安全锁：即便极端外部环境令完成页展示重启单选框，也强制默认选中「否，稍后重启」，杜绝点击完成误触发重启
  if CurPageID = wpFinished then
  begin
    if WizardForm.NoRadio.Visible then
    begin
      WizardForm.YesRadio.Checked := False;
      WizardForm.NoRadio.Checked := True;
    end;
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

function IsPicasaViewerComponentSelected(): Boolean;
begin
  Result := WizardIsComponentSelected('picasaviewer');
end;

function IsPicasaViewerRegisteredInAnyUserHive(): Boolean;
var
  SubKeys: TArrayOfString;
  I: Integer;
begin
  if RegKeyExists(HKCU, 'Software\Classes\' + PICASA_PROG_ID) then
  begin
    Result := True;
    Exit;
  end;
  if RegGetSubkeyNames(HKU, '', SubKeys) then
  begin
    for I := 0 to GetArrayLength(SubKeys) - 1 do
    begin
      if (Length(SubKeys[I]) > 8) and
         (CompareText(Copy(SubKeys[I], Length(SubKeys[I]) - 7, 8), '_Classes') = 0) then
        Continue;
      if RegKeyExists(HKU, SubKeys[I] + '\Software\Classes\' + PICASA_PROG_ID) then
      begin
        Result := True;
        Exit;
      end;
    end;
  end;
  Result := False;
end;

function IsPicasaViewerRegisterNeeded(): Boolean;
begin
  if not WizardIsComponentSelected('picasaviewer') then
  begin
    Result := False;
    Exit;
  end;
  // 全新安装、显式 CLI 组件指定、本次覆盖升级新勾选、或尚未注册关联时执行 --register-silent
  // 若为覆盖升级且用户之前已启用并完成注册，保留用户自定义格式子集，避免无意义重复注册
  if (not HasExistingUserConfig) or
     HasCommandLinePrefix('/components=') or
     HasCommandLinePrefix('/type=') or
     (not InitialCompPicasaViewer) or
     (not IsPicasaViewerRegisteredInAnyUserHive()) then
    Result := True
  else
    Result := False;
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
  InteractiveUser: String;
begin
  // 检测当前用户专属规范任务 (Tools3000\Autorun for <User>)
  Result := Exec(ExpandConstant('{sys}\schtasks.exe'),
    '/query /tn "Tools3000\Autorun for ' + GetUserNameString() + '"', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  if Result then Exit;

  // 兼容多账户 UAC 场景：检测物理登录用户的自启任务
  InteractiveUser := GetInteractiveUserHint();
  if (InteractiveUser <> '') and (CompareText(InteractiveUser, GetUserNameString()) <> 0) then
  begin
    Result := Exec(ExpandConstant('{sys}\schtasks.exe'),
      '/query /tn "Tools3000\Autorun for ' + InteractiveUser + '"', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
    if Result then Exit;
  end;

  // 检测注册表 Run 键 (HKCU Run 键)
  Result := RegValueExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Tools3000');
end;

procedure CreateAutoStartTask();
var
  ResultCode: Integer;
  TargetExe, InteractiveUser, TaskName: String;
begin
  TargetExe := ExpandConstant('{app}\Tools3000.exe');
  InteractiveUser := GetInteractiveUserHint();

  // 若处于跨账户 UAC 提权上下文（管理员账户与桌面登录账户不同），为交互桌面用户精准注册登录触发任务并写入 Run 兜底
  if (InteractiveUser <> '') and (CompareText(InteractiveUser, GetUserNameString()) <> 0) then
  begin
    TaskName := 'Tools3000\Autorun for ' + InteractiveUser;
    if Exec(ExpandConstant('{sys}\schtasks.exe'),
      '/create /tn "' + TaskName + '" /tr "\"' + TargetExe + '\" --silent" /sc onlogon /ru "' + InteractiveUser + '" /rl highest /f',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    begin
      Log('Registered AutoStart task for original interactive user: ' + InteractiveUser);
      Exit;
    end;
  end;

  // 直接委托原生注册，100% 保证安装包与软件设置页同源、同逻辑、同配置
  if Exec(TargetExe, '--register-autostart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    Log('Registered AutoStart task via native COM API')
  else
    Log(Format('Failed to register AutoStart task, exit code %d', [ResultCode]));
end;

procedure RemoveAutoStartTask();
var
  ResultCode: Integer;
  InteractiveUser: String;
begin
  if FileExists(ExpandConstant('{app}\Tools3000.exe')) then
    Exec(ExpandConstant('{app}\Tools3000.exe'), '--unregister-autostart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\schtasks.exe'), '/delete /tn "Tools3000\Autorun for ' + GetUserNameString() + '" /f', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  InteractiveUser := GetInteractiveUserHint();
  if (InteractiveUser <> '') and (CompareText(InteractiveUser, GetUserNameString()) <> 0) then
    Exec(ExpandConstant('{sys}\schtasks.exe'), '/delete /tn "Tools3000\Autorun for ' + InteractiveUser + '" /f', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure PurgeLegacyDuplicatePluginDlls();
var
  PluginsDir, NameLower: String;
  FindRec: TFindRec;
begin
  PluginsDir := ExpandConstant('{app}\plugins');
  if not DirExists(PluginsDir) then Exit;
  if FindFirst(PluginsDir + '\*.dll', FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
        begin
          NameLower := Lowercase(FindRec.Name);
          if Pos('plugin_', NameLower) <> 1 then
          begin
            if DeleteFile(PluginsDir + '\' + FindRec.Name) then
              Log('PurgeLegacyDuplicatePluginDlls: Removed legacy duplicate DLL -> ' + FindRec.Name);
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure ConfigureSearchServiceStartDacl();
var
  ResultCode: Integer;
begin
  if not ServiceExists() then Exit;
  if Exec(ExpandConstant('{sys}\sc.exe'),
    'sdset Tools3000_SearchService "D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPLOCRRC;;;IU)(A;;CCLCSWRPLOCRRC;;;AU)(A;;CCLCSWLOCRRC;;;SU)"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    Log('ConfigureSearchServiceStartDacl: Granted SERVICE_START (RP) and QUERY_STATUS (LC) to IU/AU.')
  else
    Log(Format('ConfigureSearchServiceStartDacl: sc.exe sdset exited with code %d', [ResultCode]));
end;

function HasCommandLinePrefix(const Prefix: String): Boolean;
var
  I: Integer;
  LowerPrefix, ArgLower: String;
begin
  Result := False;
  LowerPrefix := Lowercase(Prefix);
  for I := 1 to ParamCount do
  begin
    ArgLower := Lowercase(ParamStr(I));
    if Pos(LowerPrefix, ArgLower) = 1 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function FindExistingUserConfigPath(): String;
var
  InteractiveUser, UsersRoot, Candidate: String;
begin
  Result := '';

  // 跨账户 UAC 提权时优先定位原交互桌面用户的 config.json（同时兼容 Local\config\config.json 与 Roaming\config.json）
  InteractiveUser := GetInteractiveUserHint();
  if (InteractiveUser <> '') and (CompareText(InteractiveUser, GetUserNameString()) <> 0) then
  begin
    UsersRoot := ExpandConstant('{sd}\Users\') + InteractiveUser + '\AppData\';
    Candidate := UsersRoot + 'Local\Tools3000\config\config.json';
    if FileExists(Candidate) then begin Result := Candidate; Exit; end;
    Candidate := UsersRoot + 'Roaming\Tools3000\config.json';
    if FileExists(Candidate) then begin Result := Candidate; Exit; end;
    Candidate := UsersRoot + 'Roaming\Tools3000\config\config.json';
    if FileExists(Candidate) then begin Result := Candidate; Exit; end;
    Candidate := UsersRoot + 'Local\Tools3000\config.json';
    if FileExists(Candidate) then begin Result := Candidate; Exit; end;
  end;

  Candidate := ExpandConstant('{localappdata}\Tools3000\config\config.json');
  if FileExists(Candidate) then begin Result := Candidate; Exit; end;
  Candidate := ExpandConstant('{userappdata}\Tools3000\config.json');
  if FileExists(Candidate) then begin Result := Candidate; Exit; end;
  Candidate := ExpandConstant('{userappdata}\Tools3000\config\config.json');
  if FileExists(Candidate) then begin Result := Candidate; Exit; end;
  Candidate := ExpandConstant('{localappdata}\Tools3000\config.json');
  if FileExists(Candidate) then begin Result := Candidate; Exit; end;
end;

function UserConfigAlreadyExists(): Boolean;
begin
  Result := FindExistingUserConfigPath() <> '';
end;

procedure SyncInitialModuleConfig();
var
  AppDir, InitialModulesPath: String;
  ExplicitCliSelection, ConfigExists, ComponentsChanged: Boolean;
  SearchSel, CaptureSel, OcrPackSel, GestureSel, KeycastSel, DialogSel, SpotlightSel, RemoteSel, PicasaViewerSel: Boolean;
  SearchStr, CaptureStr, OcrPackStr, GestureStr, KeycastStr, DialogStr, SpotlightStr, RemoteStr, PicasaViewerStr: String;
  JsonContent: String;
begin
  AppDir := ExpandConstant('{app}');
  InitialModulesPath := AppDir + '\initial_modules.json';

  ExplicitCliSelection := HasCommandLinePrefix('/components=') or HasCommandLinePrefix('/type=');
  ConfigExists := HasExistingUserConfig or UserConfigAlreadyExists();
  ComponentsChanged := HaveSelectedComponentsChanged();

  // 仅在首次安装（config.json 不存在）、命令行显式指定组件、或用户在组件选择流程中实际变更勾选时写入初始配置；
  // 静默升级与未变更组件的图形覆盖升级均跳过写入，彻底避免重置用户已手动开启的 Picasa 等模块开关！
  if ConfigExists and (not ExplicitCliSelection) and (not ComponentsChanged) then
  begin
    DeleteFile(InitialModulesPath);
    Log('SyncInitialModuleConfig: Existing user config detected with no component selection changes; skipped initial_modules.json write to preserve user toggles.');
    Exit;
  end;

  SearchSel := WizardIsComponentSelected('search');
  CaptureSel := WizardIsComponentSelected('capture');
  OcrPackSel := WizardIsComponentSelected('capture\ocr_models');
  GestureSel := WizardIsComponentSelected('gesture');
  KeycastSel := WizardIsComponentSelected('keycast');
  DialogSel := WizardIsComponentSelected('dialogenhancer');
  SpotlightSel := WizardIsComponentSelected('spotlight');
  RemoteSel := WizardIsComponentSelected('remote');
  PicasaViewerSel := WizardIsComponentSelected('picasaviewer');

  if SearchSel then SearchStr := 'true' else SearchStr := 'false';
  if CaptureSel then CaptureStr := 'true' else CaptureStr := 'false';
  if OcrPackSel then OcrPackStr := 'true' else OcrPackStr := 'false';
  if GestureSel then GestureStr := 'true' else GestureStr := 'false';
  if KeycastSel then KeycastStr := 'true' else KeycastStr := 'false';
  if DialogSel then DialogStr := 'true' else DialogStr := 'false';
  if SpotlightSel then SpotlightStr := 'true' else SpotlightStr := 'false';
  if RemoteSel then RemoteStr := 'true' else RemoteStr := 'false';
  if PicasaViewerSel then PicasaViewerStr := 'true' else PicasaViewerStr := 'false';

  JsonContent :=
    '{' + #13#10 +
    '  "plugins": {' + #13#10 +
    '    "search": { "enabled": ' + SearchStr + ' },' + #13#10 +
    '    "capture": { "enabled": ' + CaptureStr + ' },' + #13#10 +
    '    "gesture": { "enabled": ' + GestureStr + ' },' + #13#10 +
    '    "keycast": { "enabled": ' + KeycastStr + ' },' + #13#10 +
    '    "dialogenhancer": { "enabled": ' + DialogStr + ' },' + #13#10 +
    '    "spotlight": { "enabled": ' + SpotlightStr + ' },' + #13#10 +
    '    "remote_boost": { "enabled": ' + RemoteStr + ' },' + #13#10 +
    '    "picasa_viewer": { "enabled": ' + PicasaViewerStr + ' }' + #13#10 +
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
    '  "picasa_viewer": {' + #13#10 +
    '    "enabled": ' + PicasaViewerStr + '' + #13#10 +
    '  },' + #13#10 +
    '  "ocr": {' + #13#10 +
    '    "autoDownloadPackOnStartup": ' + OcrPackStr + '' + #13#10 +
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

// NTFS 原地改名腾位引擎：清理上次遗留的 .old_del_*，并将任何仍被外部进程映射占用的目标文件
// 原地重命名为 .old_del_<tick>，使新版本文件无需 restartreplace、100% 物理零重启当场写入生效
procedure EvacuateLockedFilesInTree(const DirPath: String);
var
  FindRec: TFindRec;
  FullPath, LowerName, AsidePath: String;
begin
  if (DirPath = '') or (not DirExists(DirPath)) then Exit;
  if FindFirst(DirPath + '\*.*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name = '.') or (FindRec.Name = '..') then Continue;
        FullPath := DirPath + '\' + FindRec.Name;
        LowerName := Lowercase(FindRec.Name);
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          EvacuateLockedFilesInTree(FullPath);
        end
        else
        begin
          if Pos('unins', LowerName) = 1 then Continue;
          if Pos('.old_del_', LowerName) > 0 then
          begin
            if DeleteFile(FullPath) then
              Log('EvacuateLockedFilesInTree: Purged previous old_del file -> ' + FullPath);
            Continue;
          end;
          if IsFileLocked(FullPath) then
          begin
            AsidePath := Format('%s.old_del_%u', [FullPath, GetTickCount()]);
            if MoveFileExW(FullPath, AsidePath, MOVEFILE_REPLACE_EXISTING_FLAG) then
              Log('EvacuateLockedFilesInTree: Renamed locked file aside -> ' + AsidePath)
            else
              Log(Format('EvacuateLockedFilesInTree: MoveFileExW failed (%d) for %s', [GetLastError(), FullPath]));
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    EvacuateLockedFilesInTree(ExpandConstant('{app}'));
  end;
  if CurStep = ssPostInstall then
  begin
    PurgeLegacyDuplicatePluginDlls();
    ConfigureSearchServiceStartDacl();
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

function ShouldAutoLaunchAfterSilentUpgrade(): Boolean;
begin
  Result := WizardSilent and
    (HasCommandLineParameter('/RESTARTAPPLICATIONS') or HasCommandLineParameter('/AUTOUPDATE'));
end;

procedure DeleteSelectedPersonalData();
var
  LocalRoot, RoamingRoot, CommonRoot, InteractiveUser, UsersRoot, OrigLocalRoot, OrigRoamingRoot: String;
begin
  LocalRoot := ExpandConstant('{localappdata}\Tools3000');
  RoamingRoot := ExpandConstant('{userappdata}\Tools3000');
  CommonRoot := ExpandConstant('{commonappdata}\Tools3000');
  OrigLocalRoot := '';
  OrigRoamingRoot := '';
  InteractiveUser := GetInteractiveUserHint();
  if (InteractiveUser <> '') and (CompareText(InteractiveUser, GetUserNameString()) <> 0) then
  begin
    UsersRoot := ExtractFileDrive(ExpandConstant('{win}')) + '\Users\' + InteractiveUser + '\AppData';
    if DirExists(UsersRoot) then
    begin
      OrigLocalRoot := UsersRoot + '\Local\Tools3000';
      OrigRoamingRoot := UsersRoot + '\Roaming\Tools3000';
    end;
  end;

  if DeleteSettingsHistory and DeleteDiagnostics and DeleteCachesIndexes and DeleteCaptures then
  begin
    DelTree(LocalRoot, True, True, True);
    DelTree(RoamingRoot, True, True, True);
    DelTree(CommonRoot, True, True, True);
    if OrigLocalRoot <> '' then DelTree(OrigLocalRoot, True, True, True);
    if OrigRoamingRoot <> '' then DelTree(OrigRoamingRoot, True, True, True);
    Log('Deleted all selected Tools3000 personal data');
    Exit;
  end;

  if DeleteSettingsHistory then
  begin
    DelTree(LocalRoot + '\config', True, True, True);
    DelTree(LocalRoot + '\stats', True, True, True);
    DeleteFile(RoamingRoot + '\Run History.csv');
    DeleteFile(RoamingRoot + '\Search History.csv');
    if OrigLocalRoot <> '' then
    begin
      DelTree(OrigLocalRoot + '\config', True, True, True);
      DelTree(OrigLocalRoot + '\stats', True, True, True);
    end;
    if OrigRoamingRoot <> '' then
    begin
      DeleteFile(OrigRoamingRoot + '\Run History.csv');
      DeleteFile(OrigRoamingRoot + '\Search History.csv');
    end;
  end;
  if DeleteDiagnostics then
  begin
    DelTree(LocalRoot + '\logs', True, True, True);
    DelTree(LocalRoot + '\crashdumps', True, True, True);
    DelTree(CommonRoot + '\logs', True, True, True);
    if OrigLocalRoot <> '' then
    begin
      DelTree(OrigLocalRoot + '\logs', True, True, True);
      DelTree(OrigLocalRoot + '\crashdumps', True, True, True);
    end;
  end;
  if DeleteCachesIndexes then
  begin
    DelTree(LocalRoot + '\webview2_data', True, True, True);
    DelTree(LocalRoot + '\temp', True, True, True);
    DeleteFile(LocalRoot + '\Tools3000.db');
    DeleteFile(RoamingRoot + '\Tools3000.db');
    if OrigLocalRoot <> '' then
    begin
      DelTree(OrigLocalRoot + '\webview2_data', True, True, True);
      DelTree(OrigLocalRoot + '\temp', True, True, True);
      DeleteFile(OrigLocalRoot + '\Tools3000.db');
    end;
    if OrigRoamingRoot <> '' then
      DeleteFile(OrigRoamingRoot + '\Tools3000.db');
  end;
  if DeleteCaptures then
  begin
    DelTree(LocalRoot + '\Screenshots', True, True, True);
    DelTree(LocalRoot + '\Recordings', True, True, True);
    if OrigLocalRoot <> '' then
    begin
      DelTree(OrigLocalRoot + '\Screenshots', True, True, True);
      DelTree(OrigLocalRoot + '\Recordings', True, True, True);
    end;
  end;
  RemoveDir(LocalRoot);
  RemoveDir(RoamingRoot);
  RemoveDir(CommonRoot);
  if OrigLocalRoot <> '' then RemoveDir(OrigLocalRoot);
  if OrigRoamingRoot <> '' then RemoveDir(OrigRoamingRoot);
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

  // 2. 【句柄释放重试轮询】：在强杀后循环探测（最多重试 15 次，每次 Sleep 200ms，无锁时第 1 次 0ms 立即跳出）确保关键 DLL 文件不再处于锁定状态
  for RetryCount := 1 to 15 do
  begin
    DllsLocked := AreCriticalDllsLocked();
    if not DllsLocked then
    begin
      Log(Format('PrepareToInstall: Step 2 - All critical DLL file locks released at attempt %d (elapsed %d ms)', [RetryCount, (RetryCount - 1) * 200]));
      Break;
    end;

    Log(Format('PrepareToInstall: Critical DLL files still locked, waiting 200ms (attempt %d/15)...', [RetryCount]));
    Sleep(200);

    // 第 3 次与第 8 次探测若仍有锁定，补发一次 taskkill 确保彻底清场（针对 Tools3000.exe 严禁带 /t 防自杀）
    if ((RetryCount = 3) or (RetryCount = 8)) and DllsLocked then
    begin
      Log(Format('PrepareToInstall: Locks still detected at attempt %d, re-issuing taskkill...', [RetryCount]));
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im Tools3000.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Picasa_Viewer.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im tools3000-cli.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /t /im Tools3000_Service.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;
  end;

  if AreCriticalDllsLocked() then
  begin
    Log('PrepareToInstall: Warning - File locks still present after 15 retries. EvacuateLockedFilesInTree will rename locked files aside during install.');
  end
  else
  begin
    Log('PrepareToInstall: Verification passed - all file locks completely released.');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  UnregCode: Integer;
  ViewerExe: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    // 1. 毫秒级停止并清理后台搜索服务，注销 Picasa 看图器文件关联，终止进程树并留足句柄释放窗口
    StopAndKillAllTools3000Processes();
    ViewerExe := ExpandConstant('{app}\Tools3000_Picasa_Viewer.exe');
    if FileExists(ViewerExe) then
      Exec(ViewerExe, '--unregister-silent', '', SW_HIDE, ewWaitUntilTerminated, UnregCode);
    DeleteSearchServiceNative();
    DeleteFile(ExpandConstant('{app}\initial_modules.json'));
    WaitForProcessesAndFilesReleased(3500);
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
      WaitForProcessesAndFilesReleased(3500);
    end
    else
    begin
      SuppressibleMsgBox(CustomMessage('UninstallAbortedByUser'), mbInformation, MB_OK, IDOK);
      Result := False;
      Exit;
    end;
  end;
end;


