const translations = {
    "en": {
        "nav.engine": "The Engine",
        "nav.ui": "UI / UX",
        "nav.visuals": "Visuals",
        "nav.features": "Features",
        "nav.download": "Download {version}",
        "hero.tag": "New Release {version}",
        "hero.title": "The Quantum Flow Update.",
        "hero.subtitle": "Built on a <strong>vcpkg + Static Linking</strong> hybrid architecture.<br><span class='highlight'>~6.2MB</span> Single EXE. <span class='highlight'>0 DLL Hell</span>. <span class='highlight'>LTO Enabled</span>.",
        "cta.getting_started": "Get Started",
        "cta.explore": "Explore Architecture",
        "sec1.title": "1. The Engine",
        "sec1.desc": "Mixed Architecture: Small size, instant startup, maximum compatibility.",
        "card.static.title": "Single Static EXE",
        "card.static.desc": "6.2 MB total size. No DLL dependencies. Compiled with <code>LTO</code> and <code>/O2</code> optimization. Run it anywhere.",
        "card.memory.title": "Intelligent Memory",
        "card.memory.desc": "New <strong>TripleArena</strong> system. Zero fragmentation, 30% less RAM usage, and instant resource recycling.",
        "card.render.title": "Cinematic Rendering",
        "card.render.desc": "<strong>DirectComposition</strong> engine delivers tear-free, artifact-free 60FPS+ animations and lossless zooming.",
        "memory.text": "Verified: High Performance, Low Overhead.",
        "sec1.stack": "All-Star Codec Stack",
        "sec2.title": "2. Native & Minimalist",
        "sec2.desc": "Redesigned for 'Borderless Immersion'.",
        "feat.smart_load": "Intelligent Loading",
        "feat.smart_load.desc": "Hybrid 'Smart Skip' prefetcher predicts your cache limits and loads images instantly without freezing.",
        "feat.resize": "Smart Resize",
        "feat.resize.desc": "Window automatically scales with the image content until it hits screen edges.",
        "feat.hud": "Immersive Gallery (HUD)",
        "feat.hud.desc": "Press <strong>'T'</strong> to summon a virtualized filmstrip capable of scrolling thousands of images at 60fps.",
        "sec3.title": "3. Visuals & Data",
        "sec3.desc": "Professional grade introspection.",
        "feat.matrix_hud": "Matrix HUD & Grid",
        "feat.matrix_hud.desc": "Press 'F12' for a real-time topology overlay. Auto-aligning grid displays Exif, GPS, and RGB Histograms.",
        "feat.deep_intel": "Deep Format Intelligence",
        "feat.deep_intel.desc": "Corrects Exif Orientation automatically using decoder truth. Detects Chroma Subsampling, Color Spaces (P3/Rec2020), and Real Bit-Depth.",
        "feat.osd": "Smart OSD",
        "feat.osd.desc": "Modern Toast notifications with color-coded state indicators (Green=Lossless, Red=Lossy).",
        "sec4.title": "4. Features & Config",
        "sec4.desc": "Fully configurable engine behavior.",
        "card.config.title": "🛠️ Configuration Center",
        "card.config.desc": "Bind logic for Window Behavior, Mouse Controls, and Image Decoding directly in the UI.",
        "param.canvas": "<strong>Canvas Color:</strong> Black / White / Grid / Custom",
        "param.assoc": "<strong>File Associations:</strong> Native Windows Shell Registration",
        "param.update": "<strong>Auto-Update:</strong> Silent Background Download",
        "card.edit.title": "🛡️ Non-Destructive Edit",
        "card.edit.desc": "Rotations and flips are performed in temporary buffers. Changes are only committed via a TaskDialog confirmation (Save / Save As).",
        "mini.menu": "<strong>Right Click Menu:</strong> Refactored context menu with 'Magic Bytes' file repair.",
        "footer.slogan": "The Engine for Images.",
        "footer.license": "MIT or GPL License (Check Repo) | <a href='https://github.com/justnullname/QuickView'>GitHub</a>"
    },
    "zh-CN": {
        "nav.engine": "核心引擎",
        "nav.ui": "交互体验",
        "nav.visuals": "视觉数据",
        "nav.features": "功能配置",
        "nav.download": "下载 {version}",
        "hero.tag": "新版本 {version} 发布",
        "hero.title": "量子流更新 (Quantum Flow)",
        "hero.subtitle": "基于 <strong>vcpkg + 静态链接</strong> 混合架构打造。<br><span class='highlight'>~6.2MB</span> 单文件 EXE。 <span class='highlight'>0 DLL 依赖</span>。 <span class='highlight'>启用 LTO 优化</span>。",
        "cta.getting_started": "立即开始",
        "cta.explore": "探索架构",
        "sec1.title": "1. 核心引擎",
        "sec1.desc": "混合架构：体积小、启动快、兼容强。",
        "card.static.title": "单文件静态编译",
        "card.static.desc": "6.2 MB 总提交。无 DLL 地狱。编译器启用 <code>LTO</code> 和 <code>/O2</code> 极速模式优化。随处运行。",
        "card.memory.title": "智能内存管理",
        "card.memory.desc": "全新 <strong>TripleArena</strong> 系统。零碎片化，内存占用降低 30%，资源即时回收。",
        "card.render.title": "电影级渲染",
        "card.render.desc": "<strong>DirectComposition</strong> 引擎带来无撕裂、零伪影的 60FPS+ 动画与无损缩放。",
        "memory.text": "实测验证：高性能，低开销。",
        "sec1.stack": "全明星解码栈",
        "sec2.title": "2. 原生与极简",
        "sec2.desc": "符合“无边框、沉浸式”的设计目标。",
        "feat.smart_load": "智能预读",
        "feat.smart_load.desc": "混合“智能跳过”预读器，预测缓存限制并即时加载图像，杜绝卡顿。",
        "feat.resize": "智能缩放",
        "feat.resize.desc": "窗口随图片尺寸同步缩放，直到屏幕边缘。",
        "feat.hud": "沉浸式画廊 (HUD)",
        "feat.hud.desc": "按 <strong>T</strong> 键呼出，虚拟化列表支持数千张图片极速滚动。",
        "sec3.title": "3. 视觉与数据",
        "sec3.desc": "专业级呈现，不仅看图快，参数也要专业。",
        "feat.matrix_hud": "矩阵 HUD & 网格",
        "feat.matrix_hud.desc": "按 F12 呼出实时拓扑叠加层。自动对齐网格显示 Exif、GPS 和 RGB 直方图。",
        "feat.deep_intel": "深度格式智能",
        "feat.deep_intel.desc": "基于解码器真值自动修正 Exif 方向。检测色度采样、色彩空间 (P3/Rec2020) 和真实位深。",
        "feat.osd": "智能 OSD",
        "feat.osd.desc": "现代化 Toast 风格，圆角半透明黑底。状态指示：绿色(无损)、红色(有损)。",
        "sec4.title": "4. 功能与配置",
        "sec4.desc": "完全可配置的引擎行为。",
        "card.config.title": "🛠️ 配置中心",
        "card.config.desc": "在 UI 中直接绑定窗口行为、鼠标控制和图像解码逻辑。",
        "param.canvas": "<strong>画布颜色：</strong> 黑 / 白 / 网格 / 自定义",
        "param.assoc": "<strong>文件关联：</strong> 原生 Windows Shell 注册",
        "param.update": "<strong>自动更新：</strong> 后台静默下载",
        "card.edit.title": "🛡️ 非破坏性编辑",
        "card.edit.desc": "旋转/翻转操作在临时文件中进行。保存时提供 TaskDialog 确认（保存/另存为）。",
        "mini.menu": "<strong>右键菜单：</strong> 重构菜单，包含根据魔术字节修复后缀名功能。",
        "footer.slogan": "The Engine for Images.",
        "footer.license": "MIT or GPL License (Check Repo) | <a href='https://github.com/justnullname/QuickView'>GitHub</a>"
    },
    "zh-TW": {
        "nav.engine": "核心引擎",
        "nav.ui": "交互體驗",
        "nav.visuals": "視覺數據",
        "nav.features": "功能配置",
        "nav.download": "下載 {version}",
        "hero.tag": "新版本 {version} 發布",
        "hero.title": "效能怪獸",
        "hero.subtitle": "基於 <strong>vcpkg + 靜態連結</strong> 混合架構打造。<br><span class='highlight'>~6.2MB</span> 單一 EXE。<span class='highlight'>0 DLL 依賴</span>。<span class='highlight'>啟動 LTO 優化</span>。",
        "cta.getting_started": "立即開始",
        "cta.explore": "探索架構",
        "sec1.title": "1. 核心引擎",
        "sec1.desc": "混合架構：體積小、啟動快、兼容強。",
        "card.static.title": "單文件靜態編譯",
        "card.static.desc": "6.2 MB 總大小。無 DLL 地獄。編譯器啟用 <code>LTO</code> 和 <code>/O2</code> 極速模式優化。隨處運行。",
        "card.memory.title": "內存管理革命",
        "card.memory.desc": "集成微軟 <strong>Mimalloc</strong>。全局替換 new/delete，內存分配性能提升 10-20%，零碎片。",
        "card.render.title": "渲染管線升級",
        "card.render.desc": "<strong>DXGI Waitable Swap Chain</strong> 物理級壓榨延遲。<strong>C++23 協程</strong> 實現「Fire-and-Forget」異步加載。",
        "memory.text": "實測驗證：高性能，低開銷。",
        "sec1.stack": "全明星解碼棧",
        "sec2.title": "2. 原生與極簡",
        "sec2.desc": "符合「無邊框、沉浸式」的設計目標。",
        "feat.mag": "磁性時間鎖",
        "feat.mag.desc": "縮放經過 100% 時自動吸附並暫時鎖定滾輪（400ms），提供精密儀器的段落感。",
        "feat.resize": "智能縮放",
        "feat.resize.desc": "視窗隨圖片尺寸同步縮放，直到螢幕邊緣。",
        "feat.hud": "沉浸式畫廊 (HUD)",
        "feat.hud.desc": "按 <strong>T</strong> 鍵呼出，虛擬化列表支持數千張圖片極速滾動。",
        "sec3.title": "3. 視覺與數據",
        "sec3.desc": "專業級呈現，不僅看圖快，參數也要專業。",
        "feat.grid": "終極元數據面板",
        "feat.grid.desc": "虛擬網格自動對齊。解析 EXIF、GPS (支持點擊打開地圖)、直方圖 (稀疏採樣秒出)。",
        "feat.info": "深度格式信息",
        "feat.info.desc": "檢測色度採樣 (4:2:0)，估算 JPEG/WebP Q值，識別色彩空間 (sRGB/P3)。",
        "feat.osd": "智能 OSD",
        "feat.osd.desc": "現代化 Toast 風格，圓角半透明黑底。狀態指示：綠色(無損)、紅色(有損)。",
        "sec4.title": "4. 功能與配置",
        "sec4.desc": "完全可配置的引擎行為。",
        "card.config.title": "🛠️ 配置中心",
        "card.config.desc": "在 UI 中直接綁定視窗行為、滑鼠控制和圖像解碼邏輯。",
        "param.canvas": "<strong>畫布顏色：</strong> 黑 / 白 / 網格 / 自定義",
        "param.assoc": "<strong>文件關聯：</strong> 原生 Windows Shell 註冊",
        "param.update": "<strong>自動更新：</strong> 後台靜默下載",
        "card.edit.title": "🛡️ 非破壞性編輯",
        "card.edit.desc": "旋轉/翻轉操作在臨時文件中進行。保存時提供 TaskDialog 確認（保存/另存為）。",
        "mini.menu": "<strong>右鍵選單：</strong> 重構選單，包含根據魔術字節修復後綴名功能。",
        "footer.slogan": "The Engine for Images.",
        "footer.license": "MIT or GPL License (Check Repo) | <a href='https://github.com/justnullname/QuickView'>GitHub</a>"
    },
    "ja": {
        "nav.engine": "エンジン",
        "nav.ui": "UI / UX",
        "nav.visuals": "ビジュアル",
        "nav.features": "機能",
        "nav.download": "ダウンロード {version}",
        "hero.tag": "新リリース {version}",
        "hero.title": "パフォーマンスの怪物。",
        "hero.subtitle": "<strong>vcpkg + 静的リンク</strong> ハイブリッドアーキテクチャ。<br><span class='highlight'>~6.2MB</span> 単一EXE。<span class='highlight'>DLL依存なし</span>。<span class='highlight'>LTO 有効化</span>。",
        "cta.getting_started": "始める",
        "cta.explore": "アーキテクチャを見る",
        "sec1.title": "1. エンジン",
        "sec1.desc": "混合アーキテクチャ：小型、高速起動、最大限の互換性。",
        "card.static.title": "単一静的EXE",
        "card.static.desc": "合計サイズ 6.2 MB。DLL依存関係なし。<code>LTO</code> および <code>/O2</code> 最適化でコンパイル。どこでも実行可能。",
        "card.memory.title": "メモリ革命",
        "card.memory.desc": "Microsoft <strong>Mimalloc</strong> を統合。グローバル new/delete を置換し、割り当てを10-20%高速化、断片化ゼロ。",
        "card.render.title": "レンダリングパイプライン",
        "card.render.desc": "<strong>DXGI Waitable Swap Chain</strong> で超低レイテンシを実現。<strong>C++23 コルーチン</strong> で「ファイア・アンド・フォーゲット」非同期読み込み。",
        "memory.text": "検証済み：高性能、低オーバーヘッド。",
        "sec1.stack": "オールスターコーデックスタック",
        "sec2.title": "2. ネイティブ＆ミニマリスト",
        "sec2.desc": "「ボーダーレスな没入感」のために再設計。",
        "feat.mag": "マグネティック・タイムロック",
        "feat.mag.desc": "100%ズーム通過時に自動的にスナップし、400ms間ビューをロック。精密機器のような感触を提供。",
        "feat.resize": "スマートリサイズ",
        "feat.resize.desc": "ウィンドウは画面の端に達するまで、画像コンテンツに合わせて自動的にスケーリングします。",
        "feat.hud": "没入型ギャラリー (HUD)",
        "feat.hud.desc": "<strong>'T'</strong> キーで仮想化フィルムストリップを呼び出し、数千枚の画像を60fpsでスクロール可能。",
        "sec3.title": "3. ビジュアル＆データ",
        "sec3.desc": "プロフェッショナルグレードのイントロスペクション。",
        "feat.grid": "アルティメット・グリッドパネル",
        "feat.grid.desc": "自動整列仮想グリッド。EXIF、GPS（マップリンク）、リアルタイムRGB輝度ヒストグラムを表示。",
        "feat.info": "詳細フォーマット情報",
        "feat.info.desc": "クロマサブサンプリング (4:2:0) を検出、JPEG/WebP Qファクターを推定、色空間 (sRGB/P3) を識別。",
        "feat.osd": "スマート OSD",
        "feat.osd.desc": "色分けされたステータスインジケーターを備えたモダンなトースト通知（緑＝ロスレス、赤＝ロッシー）。",
        "sec4.title": "4. 機能と設定",
        "sec4.desc": "エンジンの動作を完全に設定可能。",
        "card.config.title": "🛠️ 設定センター",
        "card.config.desc": "ウィンドウの動作、マウスコントロール、画像デコードロジックをUIで直接バインド。",
        "param.canvas": "<strong>キャンバス色：</strong> 黒 / 白 / グリッド / カスタム",
        "param.assoc": "<strong>ファイルの関連付け：</strong> ネイティブ Windows シェル登録",
        "param.update": "<strong>自動更新：</strong> バックグラウンドサイレントダウンロード",
        "card.edit.title": "🛡️ 非破壊編集",
        "card.edit.desc": "回転や反転は一時バッファで実行されます。変更はタスクダイアログ確認（保存/名前を付けて保存）でのみコミットされます。",
        "mini.menu": "<strong>右クリックメニュー：</strong> 「マジックバイト」ファイル修復を含むリファクタリングされたコンテキストメニュー。",
        "footer.slogan": "The Engine for Images.",
        "footer.license": "MIT or GPL License (Check Repo) | <a href='https://github.com/justnullname/QuickView'>GitHub</a>"
    },
    "ru": {
        "nav.engine": "Движок",
        "nav.ui": "Интерфейс",
        "nav.visuals": "Визуализация",
        "nav.features": "Функции",
        "nav.download": "Скачать {version}",
        "hero.tag": "Новый релиз {version}",
        "hero.title": "Монстр производительности.",
        "hero.subtitle": "Построено на гибридной архитектуре <strong>vcpkg + Static Linking</strong>.<br><span class='highlight'>~6.2MB</span> Один EXE. <span class='highlight'>0 зависимостей DLL</span>. <span class='highlight'>LTO включен</span>.",
        "cta.getting_started": "Начать",
        "cta.explore": "Обзор архитектуры",
        "sec1.title": "1. Движок",
        "sec1.desc": "Смешанная архитектура: малый размер, мгновенный запуск, максимальная совместимость.",
        "card.static.title": "Единый статический EXE",
        "card.static.desc": "Общий размер 6.2 МБ. Нет зависимостей DLL. Скомпилировано с оптимизацией <code>LTO</code> и <code>/O2</code>. Запускайте где угодно.",
        "card.memory.title": "Революция памяти",
        "card.memory.desc": "Интегрирован Microsoft <strong>Mimalloc</strong>. Заменяет глобальные new/delete для ускорения выделения на 10-20% и нулевой фрагментации.",
        "card.render.title": "Конвейер рендеринга",
        "card.render.desc": "<strong>DXGI Waitable Swap Chain</strong> для ультра-низкой задержки. <strong>Корутины C++23</strong> для асинхронной загрузки 'Fire-and-Forget'.",
        "memory.text": "Проверено: Высокая производительность, низкие накладные расходы.",
        "sec1.stack": "Звездный стек кодеков",
        "sec2.title": "2. Нативный и минималистичный",
        "sec2.desc": "Переработан для «Безграничного погружения».",
        "feat.mag": "Магнитная временная блокировка",
        "feat.mag.desc": "Масштабирование через 100% автоматически фиксирует вид на 400 мс, обеспечивая тактильное ощущение «засечки».",
        "feat.resize": "Умное изменение размера",
        "feat.resize.desc": "Окно автоматически масштабируется вместе с контентом изображения, пока не достигнет краев экрана.",
        "feat.hud": "Иммерсивная галерея (HUD)",
        "feat.hud.desc": "Нажмите <strong>'T'</strong>, чтобы вызвать виртуализированную ленту, способную прокручивать тысячи изображений при 60fps.",
        "sec3.title": "3. Визуализация и данные",
        "sec3.desc": "Интроспекция профессионального уровня.",
        "feat.grid": "Панель Ultimate Grid",
        "feat.grid.desc": "Автоматически выравниваемая сетка с EXIF, GPS (ссылка на карту) и гистограммами RGB в реальном времени.",
        "feat.info": "Глубокая информация о формате",
        "feat.info.desc": "Обнаруживает цветовую субдискретизацию (4:2:0), оценивает Q-фактор JPEG/WebP и определяет цветовые пространства (sRGB/P3).",
        "feat.osd": "Умный OSD",
        "feat.osd.desc": "Современные уведомления Toast с цветовой индикацией (Зеленый=Lossless, Красный=Lossy).",
        "sec4.title": "4. Функции и конфигурация",
        "sec4.desc": "Полностью настраиваемое поведение движка.",
        "card.config.title": "🛠️ Центр конфигурации",
        "card.config.desc": "Привязка логики поведения окна, управления мышью и декодирования изображений прямо в UI.",
        "param.canvas": "<strong>Цвет холста:</strong> Черный / Белый / Сетка / Свой",
        "param.assoc": "<strong>Ассоциации файлов:</strong> Нативная регистрация в оболочке Windows",
        "param.update": "<strong>Автообновление:</strong> Тихая фоновая загрузка",
        "card.edit.title": "🛡️ Неразрушающее редактирование",
        "card.edit.desc": "Повороты и отражения выполняются во временных буферах. Изменения применяются только через подтверждение TaskDialog (Сохранить / Сохранить как).",
        "mini.menu": "<strong>Контекстное меню:</strong> Рефакторинг меню с функцией восстановления файлов по «Магическим байтам».",
        "footer.slogan": "The Engine for Images.",
        "footer.license": "MIT или GPL (См. репозиторий) | <a href='https://github.com/justnullname/QuickView'>GitHub</a>"
    },
    "es": {
        "nav.engine": "El Motor",
        "nav.ui": "UI / UX",
        "nav.visuals": "Visuales",
        "nav.features": "Funciones",
        "nav.download": "Descargar {version}",
        "hero.tag": "Lanzamiento {version}",
        "hero.title": "El Monstruo del Rendimiento.",
        "hero.subtitle": "Construido sobre una arquitectura híbrida <strong>vcpkg + Enlace Estático</strong>.<br><span class='highlight'>~6.2MB</span> EXE único. <span class='highlight'>0 Dependencias DLL</span>. <span class='highlight'>LTO Habilitado</span>.",
        "cta.getting_started": "Empezar",
        "cta.explore": "Explorar Arquitectura",
        "sec1.title": "1. El Motor",
        "sec1.desc": "Arquitectura mixta: Tamaño pequeño, inicio instantáneo, máxima compatibilidad.",
        "card.static.title": "EXE Estático Único",
        "card.static.desc": "Tamaño total de 6.2 MB. Sin dependencias DLL. Compilado con optimización <code>LTO</code> y <code>/O2</code>.  Ejecútalo donde sea.",
        "card.memory.title": "Revolución de Memoria",
        "card.memory.desc": "Microsoft <strong>Mimalloc</strong> integrado. Reemplaza new/delete global para una asignación 10-20% más rápida y cero fragmentación.",
        "card.render.title": "Tubería de Renderizado",
        "card.render.desc": "<strong>DXGI Waitable Swap Chain</strong> para latencia ultra baja. <strong>Corrutinas C++23</strong> para carga asíncrona 'Fire-and-Forget'.",
        "memory.text": "Verificado: Alto Rendimiento, Baja Sobrecarga.",
        "sec1.stack": "Pila de Códecs All-Star",
        "sec2.title": "2. Nativo y Minimalista",
        "sec2.desc": "Rediseñado para una 'Inmersión sin Fronteras'.",
        "feat.mag": "Bloqueo Magnético de Tiempo",
        "feat.mag.desc": "El zoom al pasar por el 100% bloquea automáticamente la vista durante 400ms, proporcionando una sensación táctil de 'muesca'.",
        "feat.resize": "Redimensionado Inteligente",
        "feat.resize.desc": "La ventana se escala automáticamente con el contenido de la imagen hasta alcanzar los bordes de la pantalla.",
        "feat.hud": "Galería Inmersiva (HUD)",
        "feat.hud.desc": "Presiona <strong>'T'</strong> para invocar una tira de película virtualizada capaz de desplazar miles de imágenes a 60fps.",
        "sec3.title": "3. Visuales y Datos",
        "sec3.desc": "Introspección de grado profesional.",
        "feat.grid": "Panel de Rejilla Ultimate",
        "feat.grid.desc": "Rejilla virtual autoalineable que muestra EXIF, GPS (Enlace al mapa) e histogramas RGB de muestreo disperso en tiempo real.",
        "feat.info": "Información Profunda de Formato",
        "feat.info.desc": "Detecta submuestreo de croma (4:2:0), estima el factor Q de JPEG/WebP e identifica espacios de color (sRGB/P3).",
        "feat.osd": "OSD Inteligente",
        "feat.osd.desc": "Notificaciones Toast modernas con indicadores de estado codificados por colores (Verde=Sin pérdida, Rojo=Con pérdida).",
        "sec4.title": "4. Funciones y Configuración",
        "sec4.desc": "Comportamiento del motor totalmente configurable.",
        "card.config.title": "🛠️ Centro de Configuración",
        "card.config.desc": "Vincula la lógica para el comportamiento de la ventana, controles del ratón y decodificación de imágenes directamente en la UI.",
        "param.canvas": "<strong>Color del Lienzo:</strong> Negro / Blanco / Rejilla / Personalizado",
        "param.assoc": "<strong>Asociaciones de Archivos:</strong> Registro nativo en Windows Shell",
        "param.update": "<strong>Auto-Actualización:</strong> Descarga silenciosa en segundo plano",
        "card.edit.title": "🛡️ Edición No Destructiva",
        "card.edit.desc": "Rotaciones y volteos se realizan en búferes temporales. Los cambios solo se confirman mediante un TaskDialog (Guardar / Guardar como).",
        "mini.menu": "<strong>Menú Contextual:</strong> Menú refactorizado con reparación de archivos por 'Bytes Mágicos'.",
        "footer.slogan": "The Engine for Images.",
        "footer.license": "Licencia MIT o GPL (Ver Repo) | <a href='https://github.com/justnullname/QuickView'>GitHub</a>"
    },
    "de": {
        "nav.engine": "Die Engine",
        "nav.ui": "UI / UX",
        "nav.visuals": "Visuals",
        "nav.features": "Funktionen",
        "nav.download": "Download {version}",
        "hero.tag": "Neues Release {version}",
        "hero.title": "Das Performance-Monster.",
        "hero.subtitle": "Basiert auf einer hybriden <strong>vcpkg + Static Linking</strong> Architektur.<br><span class='highlight'>~6.2MB</span> Einzelne EXE. <span class='highlight'>0 DLL-Abhängigkeiten</span>. <span class='highlight'>LTO Aktiviert</span>.",
        "cta.getting_started": "Loslegen",
        "cta.explore": "Architektur erkunden",
        "sec1.title": "1. Die Engine",
        "sec1.desc": "Gemischte Architektur: Kleine Größe, sofortiger Start, maximale Kompatibilität.",
        "card.static.title": "Einzelne Statische EXE",
        "card.static.desc": "6.2 MB Gesamtgröße. Keine DLL-Abhängigkeiten. Kompiliert mit <code>LTO</code> und <code>/O2</code> Optimierung. Überall ausführbar.",
        "card.memory.title": "Speicher-Revolution",
        "card.memory.desc": "Integriertes Microsoft <strong>Mimalloc</strong>. Ersetzt globales new/delete für 10-20% schnellere Zuweisung und null Fragmentierung.",
        "card.render.title": "Render-Pipeline",
        "card.render.desc": "<strong>DXGI Waitable Swap Chain</strong> für ultra-niedrige Latenz. <strong>C++23 Coroutines</strong> für asynchrones Laden ('Fire-and-Forget').",
        "memory.text": "Verifiziert: Hohe Leistung, geringer Overhead.",
        "sec1.stack": "All-Star Codec Stack",
        "sec2.title": "2. Nativ & Minimalistisch",
        "sec2.desc": "Neu gestaltet für 'Grenzenlose Immersion'.",
        "feat.mag": "Magnetische Zeitsperre",
        "feat.mag.desc": "Das Zoomen durch 100% sperrt die Ansicht automatisch für 400ms und bietet ein taktiles 'Einrast'-Gefühl.",
        "feat.resize": "Smart Resize",
        "feat.resize.desc": "Das Fenster skaliert automatisch mit dem Bildinhalt, bis es die Bildschirmränder erreicht.",
        "feat.hud": "Immersive Galerie (HUD)",
        "feat.hud.desc": "Drücken Sie <strong>'T'</strong>, um einen virtualisierten Filmstreifen aufzurufen, der Tausende von Bildern mit 60fps scrollen kann.",
        "sec3.title": "3. Visuals & Daten",
        "sec3.desc": "Introspektion auf professionellem Niveau.",
        "feat.grid": "Ultimate Grid Panel",
        "feat.grid.desc": "Automatisch ausgerichtetes virtuelles Gitter mit EXIF, GPS (Kartenlink) und Echtzeit-RGB-Luma-Histogrammen.",
        "feat.info": "Tiefe Formatinformationen",
        "feat.info.desc": "Erkennt Chroma-Subsampling (4:2:0), schätzt JPEG/WebP Q-Faktor und identifiziert Farbräume (sRGB/P3).",
        "feat.osd": "Smart OSD",
        "feat.osd.desc": "Moderne Toast-Benachrichtigungen mit farbcodierten Statusindikatoren (Grün=Verlustfrei, Rot=Verlustbehaftet).",
        "sec4.title": "4. Funktionen & Konfig",
        "sec4.desc": "Vollständig konfigurierbares Engine-Verhalten.",
        "card.config.title": "🛠️ Konfigurationszentrum",
        "card.config.desc": "Binden Sie Logik für Fensterverhalten, Maussteuerung und Bilddecodierung direkt in der UI.",
        "param.canvas": "<strong>Leinwandfarbe:</strong> Schwarz / Weiß / Raster / Benutzerdefiniert",
        "param.assoc": "<strong>Dateizuordnungen:</strong> Native Windows Shell Registrierung",
        "param.update": "<strong>Auto-Update:</strong> Stiller Hintergrund-Download",
        "card.edit.title": "🛡️ Zerstörungsfreie Bearbeitung",
        "card.edit.desc": "Rotationen und Spiegelungen werden in temporären Puffern durchgeführt. Änderungen werden nur über eine TaskDialog-Bestätigung (Speichern / Speichern unter) übernommen.",
        "mini.menu": "<strong>Kontextmenü:</strong> Refactored Menü mit 'Magic Bytes' Dateireparatur.",
        "footer.slogan": "The Engine for Images.",
        "footer.license": "MIT oder GPL Lizenz (Siehe Repo) | <a href='https://github.com/justnullname/QuickView'>GitHub</a>"
    }
};

// Fallback logic
Object.keys(translations).forEach(lang => {
    if (lang === 'en') return;
    Object.keys(translations['en']).forEach(key => {
        if (!translations[lang][key]) {
            translations[lang][key] = translations['en'][key];
        }
    });
});

let currentVersion = 'v3.0.4'; // Default fallback

async function fetchVersion() {
    try {
        const response = await fetch('https://api.github.com/repos/justnullname/QuickView/releases/latest');
        if (response.ok) {
            const data = await response.json();
            if (data.tag_name) {
                currentVersion = data.tag_name;
                // Refresh current language to apply new version
                const lang = detectLanguage();
                setLanguage(lang);
            }
        }
    } catch (e) {
        console.warn('Failed to fetch version:', e);
    }
}

function setLanguage(lang) {
    if (!translations[lang]) lang = 'en';
    localStorage.setItem('qv_lang', lang);

    document.documentElement.lang = lang;

    const elements = document.querySelectorAll('[data-i18n]');
    elements.forEach(el => {
        const key = el.getAttribute('data-i18n');
        if (translations[lang][key]) {
            let text = translations[lang][key];
            // Dynamic replacement
            text = text.replace('{version}', currentVersion);
            el.innerHTML = text;
        }
    });

    // Update active state in switcher
    document.querySelectorAll('.lang-option').forEach(opt => {
        opt.classList.remove('active');
        if (opt.getAttribute('data-lang') === lang) opt.classList.add('active');
    });

    // Update button text
    const labels = {
        'en': 'EN', 'zh-CN': '简', 'zh-TW': '繁', 'ru': 'RU', 'ja': 'JP', 'es': 'ES', 'de': 'DE'
    };
    document.getElementById('current-lang').textContent = labels[lang] || 'EN';
}

function detectLanguage() {
    const saved = localStorage.getItem('qv_lang');
    if (saved) return saved;

    const browser = navigator.language;
    if (browser.startsWith('zh-CN')) return 'zh-CN';
    if (browser.startsWith('zh-TW') || browser.startsWith('zh-HK')) return 'zh-TW';
    if (browser.startsWith('ru')) return 'ru';
    if (browser.startsWith('ja')) return 'ja';
    if (browser.startsWith('es')) return 'es';
    if (browser.startsWith('de')) return 'de';
    return 'en';
}

document.addEventListener('DOMContentLoaded', () => {
    // Initial load with default version
    const lang = detectLanguage();
    setLanguage(lang);

    // Fetch real version
    fetchVersion();

    document.querySelectorAll('.lang-option').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const l = e.target.getAttribute('data-lang');
            setLanguage(l);
        });
    });
});
