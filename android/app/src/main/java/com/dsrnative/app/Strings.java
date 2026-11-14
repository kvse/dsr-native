package com.dsrnative.app;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Text of the settings menu, the touch layout editor and the first-launch tip, in the game's languages. The menu
 * follows the game language chosen in it (settings.txt `language`).
 */
final class Strings {
    private Strings() {}

    /** The game's languages: settings.txt code and name in the language itself. Same order as the columns below. */
    static final String[] CODES = {"en-US", "fr", "de", "it", "es", "pt", "ru", "ja", "ko", "zh-CN", "zh-TW"};
    static final String[] NAMES = {"English", "Français", "Deutsch", "Italiano", "Español", "Português", "Русский",
            "日本語", "한국어", "简体中文", "繁體中文"};

    private static int sLang = 0;

    /** Index into CODES for a settings.txt language code (regional variants map to their language). */
    static int indexOf(String code) {
        if (code == null) return 0;
        for (int i = 0; i < CODES.length; i++)
            if (CODES[i].equalsIgnoreCase(code)) return i;
        if (code.startsWith("en")) return 0;
        if (code.startsWith("fr")) return 1;
        if (code.startsWith("es")) return 4;
        return 0;
    }

    static void setLanguage(String code) { sLang = indexOf(code); }

    static String t(String key, Object... args) {
        String[] row = TABLE.get(key);
        String s = row == null ? key : (sLang < row.length && row[sLang] != null ? row[sLang] : row[0]);
        return args.length == 0 ? s : String.format(Locale.ROOT, s, args);
    }

    private static final Map<String, String[]> TABLE = new HashMap<>();

    // en, fr, de, it, es, pt, ru, ja, ko, zh-CN, zh-TW
    private static void put(String key, String... text) { TABLE.put(key, text); }

    static {
        put("tab_driver", "Driver", "Pilote", "Treiber", "Driver", "Controlador", "Driver", "Драйвер", "ドライバー",
                "드라이버", "驱动", "驅動");
        put("tab_settings", "Settings", "Paramètres", "Einstellungen", "Impostazioni", "Ajustes", "Configurações",
                "Настройки", "設定", "설정", "设置", "設定");
        put("tab_touch", "Touch", "Tactile", "Touch", "Touch", "Táctil", "Toque", "Сенсор", "タッチ", "터치", "触控",
                "觸控");
        put("tab_language", "Language", "Langue", "Sprache", "Lingua", "Idioma", "Idioma", "Язык", "言語", "언어",
                "语言", "語言");
        put("close", "Close", "Fermer", "Schließen", "Chiudi", "Cerrar", "Fechar", "Закрыть", "閉じる", "닫기", "关闭",
                "關閉");
        put("ok", "OK", "OK", "OK", "OK", "Aceptar", "OK", "ОК", "OK", "확인", "确定", "確定");
        put("hint",
                "Hold − and + for a second to open this menu. Controller: D-pad to move, A to choose, B to close, L / R to switch tabs.",
                "Maintenez − et + une seconde pour ouvrir ce menu. Manette : croix pour se déplacer, A pour choisir, B pour fermer, L / R pour changer d'onglet.",
                "Halte − und + eine Sekunde lang gedrückt, um dieses Menü zu öffnen. Controller: Steuerkreuz zum Bewegen, A zum Auswählen, B zum Schließen, L / R für die Tabs.",
                "Tieni premuti − e + per un secondo per aprire questo menu. Controller: croce direzionale per spostarti, A per scegliere, B per chiudere, L / R per cambiare scheda.",
                "Mantén − y + un segundo para abrir este menú. Mando: cruceta para moverte, A para elegir, B para cerrar, L / R para cambiar de pestaña.",
                "Segure − e + por um segundo para abrir este menu. Controle: direcional para mover, A para escolher, B para fechar, L / R para trocar de aba.",
                "Удерживайте − и + одну секунду, чтобы открыть это меню. Геймпад: крестовина — перемещение, A — выбор, B — закрыть, L / R — вкладки.",
                "− と + を1秒間長押しするとこのメニューが開きます。コントローラー：十字キーで移動、A で決定、B で閉じる、L / R でタブ切り替え。",
                "− 와 + 를 1초 동안 누르면 이 메뉴가 열립니다. 컨트롤러: 방향 버튼으로 이동, A로 선택, B로 닫기, L / R로 탭 전환.",
                "同时按住 − 和 + 一秒即可打开此菜单。手柄：方向键移动，A 选择，B 关闭，L / R 切换标签页。",
                "同時按住 − 和 + 一秒即可開啟此選單。手把：方向鍵移動，A 選擇，B 關閉，L / R 切換分頁。");
        put("applied", "%s: applied", "%s : appliqué", "%s: übernommen", "%s: applicato", "%s: aplicado",
                "%s: aplicado", "%s: применено", "%s：適用しました", "%s: 적용됨", "%s：已应用", "%s：已套用");
        put("next_start", "%s will change the next time you start the game.",
                "%s changera au prochain lancement du jeu.", "%s ändert sich beim nächsten Start des Spiels.",
                "%s cambierà al prossimo avvio del gioco.", "%s cambiará la próxima vez que inicies el juego.",
                "%s mudará na próxima vez que você iniciar o jogo.", "%s изменится при следующем запуске игры.",
                "%sは次回ゲーム起動時に変更されます。", "%s은(는) 다음에 게임을 시작할 때 바뀝니다.", "%s将在下次启动游戏时更改。",
                "%s將在下次啟動遊戲時變更。");
        put("cant_save_t", "Could not save", "Impossible d'enregistrer", "Speichern nicht möglich",
                "Impossibile salvare", "No se pudo guardar", "Não foi possível salvar", "Не удалось сохранить",
                "保存できませんでした", "저장할 수 없습니다", "无法保存", "無法儲存");
        put("cant_save", "settings.txt could not be written:\n%s", "settings.txt n'a pas pu être écrit :\n%s",
                "settings.txt konnte nicht geschrieben werden:\n%s", "Impossibile scrivere settings.txt:\n%s",
                "No se pudo escribir settings.txt:\n%s", "Não foi possível gravar settings.txt:\n%s",
                "Не удалось записать settings.txt:\n%s", "settings.txt に書き込めませんでした：\n%s",
                "settings.txt에 쓸 수 없습니다:\n%s", "无法写入 settings.txt：\n%s", "無法寫入 settings.txt：\n%s");
        put("on", "On", "Activé", "An", "Sì", "Sí", "Ligado", "Вкл", "オン", "켜기", "开", "開");
        put("off", "Off", "Désactivé", "Aus", "No", "No", "Desligado", "Выкл", "オフ", "끄기", "关", "關");
        put("changes_now", "Changes at once.", "S'applique immédiatement.", "Wirkt sofort.", "Si applica subito.",
                "Se aplica al instante.", "Aplica na hora.", "Применяется сразу.", "すぐに反映されます。", "즉시 적용됩니다.",
                "立即生效。", "立即生效。");
        put("changes_next", "Changes at the next start.", "S'applique au prochain lancement.",
                "Wirkt beim nächsten Start.", "Si applica al prossimo avvio.", "Se aplica en el próximo inicio.",
                "Aplica no próximo início.", "Применяется при следующем запуске.", "次回起動時に反映されます。",
                "다음 시작 시 적용됩니다.", "下次启动时生效。", "下次啟動時生效。");
        put("default_mark", "(default)", "(par défaut)", "(Standard)", "(predefinito)", "(predeterminado)",
                "(padrão)", "(по умолчанию)", "（既定）", "(기본값)", "（默认）", "（預設）");

        // ---- Driver tab
        put("gpu_driver", "GPU driver", "Pilote graphique", "Grafiktreiber", "Driver GPU", "Controlador de GPU",
                "Driver da GPU", "Драйвер GPU", "GPU ドライバー", "GPU 드라이버", "GPU 驱动", "GPU 驅動程式");
        put("driver_note",
                "The Vulkan driver the game runs on. The phone's own (system) driver is the default. Others are driver packages (.zip, as made for AdrenoTools) or .so files in %s. A new choice is used from the next start.",
                "Le pilote Vulkan utilisé par le jeu. Par défaut, celui du téléphone (système). Les autres sont des paquets de pilotes (.zip, pour AdrenoTools) ou des fichiers .so dans %s. Un nouveau choix s'applique au prochain lancement.",
                "Der Vulkan-Treiber, mit dem das Spiel läuft. Standard ist der eigene Treiber des Telefons (System). Andere sind Treiberpakete (.zip, für AdrenoTools) oder .so-Dateien in %s. Eine neue Wahl gilt ab dem nächsten Start.",
                "Il driver Vulkan usato dal gioco. Il predefinito è quello del telefono (sistema). Gli altri sono pacchetti di driver (.zip, per AdrenoTools) o file .so in %s. Una nuova scelta vale dal prossimo avvio.",
                "El controlador Vulkan con el que funciona el juego. Por defecto, el del propio teléfono (sistema). Los demás son paquetes de controladores (.zip, para AdrenoTools) o archivos .so en %s. Una nueva elección se usa desde el próximo inicio.",
                "O driver Vulkan usado pelo jogo. O padrão é o do próprio celular (sistema). Os outros são pacotes de driver (.zip, para AdrenoTools) ou arquivos .so em %s. Uma nova escolha vale a partir do próximo início.",
                "Драйвер Vulkan, на котором работает игра. По умолчанию — системный драйвер телефона. Другие — пакеты драйверов (.zip для AdrenoTools) или файлы .so в %s. Новый выбор применяется со следующего запуска.",
                "ゲームが使用する Vulkan ドライバーです。既定はスマートフォン本体（システム）のドライバーです。その他は %s にあるドライバーパッケージ（AdrenoTools 用の .zip）または .so ファイルです。変更は次回起動時から有効です。",
                "게임이 사용하는 Vulkan 드라이버입니다. 기본값은 휴대폰 자체(시스템) 드라이버입니다. 그 밖의 드라이버는 %s에 있는 드라이버 패키지(AdrenoTools용 .zip) 또는 .so 파일입니다. 새로운 선택은 다음 시작부터 사용됩니다.",
                "游戏使用的 Vulkan 驱动。默认使用手机自带（系统）驱动。其他为 %s 中的驱动包（适用于 AdrenoTools 的 .zip）或 .so 文件。新的选择将在下次启动时生效。",
                "遊戲使用的 Vulkan 驅動程式。預設為手機內建（系統）驅動程式。其他為 %s 中的驅動程式套件（適用於 AdrenoTools 的 .zip）或 .so 檔案。新的選擇將在下次啟動時生效。");
        put("in_use_now", "In use now: %s", "Utilisé actuellement : %s", "Derzeit verwendet: %s", "In uso ora: %s",
                "En uso ahora: %s", "Em uso agora: %s", "Сейчас используется: %s", "現在使用中：%s", "현재 사용 중: %s",
                "当前使用：%s", "目前使用：%s");
        put("system_driver", "System driver", "Pilote système", "Systemtreiber", "Driver di sistema",
                "Controlador del sistema", "Driver do sistema", "Системный драйвер", "システムドライバー", "시스템 드라이버",
                "系统驱动", "系統驅動程式");
        put("mark_in_use", "in use now", "utilisé actuellement", "derzeit verwendet", "in uso ora", "en uso ahora",
                "em uso agora", "используется сейчас", "使用中", "사용 중", "正在使用", "使用中");
        put("mark_missing", "file missing", "fichier manquant", "Datei fehlt", "file mancante", "falta el archivo",
                "arquivo ausente", "файл отсутствует", "ファイルがありません", "파일 없음", "文件缺失", "檔案遺失");
        put("add_driver", "Add a driver file…", "Ajouter un fichier de pilote…", "Treiberdatei hinzufügen…",
                "Aggiungi un file driver…", "Añadir un archivo de controlador…", "Adicionar um arquivo de driver…",
                "Добавить файл драйвера…", "ドライバーファイルを追加…", "드라이버 파일 추가…", "添加驱动文件…", "新增驅動程式檔案…");
        put("driver_same", "%s is the driver in use now; it stays in use.",
                "%s est le pilote utilisé actuellement ; il reste utilisé.",
                "%s ist der aktuell verwendete Treiber und bleibt aktiv.", "%s è il driver in uso ora; resta in uso.",
                "%s es el controlador en uso; se seguirá usando.", "%s é o driver em uso agora; ele continua em uso.",
                "%s уже используется и останется.", "%s は現在使用中のドライバーです。そのまま使用されます。",
                "%s은(는) 현재 사용 중인 드라이버이며 계속 사용됩니다.", "%s 是当前正在使用的驱动，将继续使用。",
                "%s 是目前使用中的驅動程式，將繼續使用。");
        put("driver_changed_t", "Driver changed", "Pilote modifié", "Treiber geändert", "Driver cambiato",
                "Controlador cambiado", "Driver alterado", "Драйвер изменён", "ドライバーを変更しました", "드라이버 변경됨",
                "驱动已更改", "驅動程式已變更");
        put("driver_changed",
                "%s will be used the next time you start the game.\n\nClose the app completely and open it again to switch now. If it cannot be loaded, the system driver is used.",
                "%s sera utilisé au prochain lancement du jeu.\n\nFermez complètement l'application puis rouvrez-la pour changer maintenant. S'il ne peut pas être chargé, le pilote système est utilisé.",
                "%s wird beim nächsten Start des Spiels verwendet.\n\nSchließe die App vollständig und öffne sie erneut, um jetzt zu wechseln. Kann er nicht geladen werden, wird der Systemtreiber verwendet.",
                "%s verrà usato al prossimo avvio del gioco.\n\nChiudi completamente l'app e riaprila per cambiare subito. Se non può essere caricato, verrà usato il driver di sistema.",
                "%s se usará la próxima vez que inicies el juego.\n\nCierra la aplicación por completo y ábrela de nuevo para cambiar ahora. Si no se puede cargar, se usará el controlador del sistema.",
                "%s será usado na próxima vez que você iniciar o jogo.\n\nFeche o app completamente e abra de novo para trocar agora. Se ele não puder ser carregado, o driver do sistema será usado.",
                "%s будет использоваться при следующем запуске игры.\n\nЧтобы переключиться сейчас, полностью закройте приложение и откройте снова. Если его не удастся загрузить, будет использован системный драйвер.",
                "%s は次回ゲーム起動時に使用されます。\n\n今すぐ切り替えるには、アプリを完全に終了してから再度開いてください。読み込めない場合はシステムドライバーが使用されます。",
                "%s은(는) 다음에 게임을 시작할 때 사용됩니다.\n\n지금 바꾸려면 앱을 완전히 종료한 뒤 다시 여세요. 불러올 수 없으면 시스템 드라이버가 사용됩니다.",
                "%s 将在下次启动游戏时使用。\n\n如需立即切换，请完全关闭应用后重新打开。如果无法加载，将使用系统驱动。",
                "%s 將在下次啟動遊戲時使用。\n\n如需立即切換，請完全關閉應用程式後重新開啟。若無法載入，將使用系統驅動程式。");
        put("no_picker_t", "No file picker", "Aucun sélecteur de fichiers", "Keine Dateiauswahl",
                "Nessun selettore di file", "No hay selector de archivos", "Nenhum seletor de arquivos", "Нет выбора файлов",
                "ファイル選択がありません", "파일 선택기가 없습니다", "没有文件选择器", "沒有檔案選擇器");
        put("no_picker", "Copy the driver file into %s instead.", "Copiez plutôt le fichier de pilote dans %s.",
                "Kopiere die Treiberdatei stattdessen nach %s.", "Copia invece il file del driver in %s.",
                "Copia el archivo del controlador en %s.", "Copie o arquivo do driver para %s.",
                "Скопируйте файл драйвера в %s.", "代わりにドライバーファイルを %s にコピーしてください。",
                "대신 드라이버 파일을 %s에 복사하세요.", "请改为将驱动文件复制到 %s。", "請改為將驅動程式檔案複製到 %s。");
        put("not_driver_t", "Not a driver", "Ce n'est pas un pilote", "Kein Treiber", "Non è un driver",
                "No es un controlador", "Não é um driver", "Это не драйвер", "ドライバーではありません", "드라이버가 아닙니다",
                "不是驱动", "不是驅動程式");
        put("not_driver", "Choose a driver package (.zip, as made for AdrenoTools) or a .so file.",
                "Choisissez un paquet de pilote (.zip, pour AdrenoTools) ou un fichier .so.",
                "Wähle ein Treiberpaket (.zip, für AdrenoTools) oder eine .so-Datei.",
                "Scegli un pacchetto driver (.zip, per AdrenoTools) o un file .so.",
                "Elige un paquete de controlador (.zip, para AdrenoTools) o un archivo .so.",
                "Escolha um pacote de driver (.zip, para AdrenoTools) ou um arquivo .so.",
                "Выберите пакет драйвера (.zip для AdrenoTools) или файл .so.",
                "ドライバーパッケージ（AdrenoTools 用の .zip）または .so ファイルを選んでください。",
                "드라이버 패키지(AdrenoTools용 .zip) 또는 .so 파일을 선택하세요.",
                "请选择驱动包（适用于 AdrenoTools 的 .zip）或 .so 文件。", "請選擇驅動程式套件（適用於 AdrenoTools 的 .zip）或 .so 檔案。");
        put("driver_added_t", "Driver added", "Pilote ajouté", "Treiber hinzugefügt", "Driver aggiunto",
                "Controlador añadido", "Driver adicionado", "Драйвер добавлен", "ドライバーを追加しました", "드라이버 추가됨",
                "驱动已添加", "驅動程式已新增");
        put("driver_added", "%s was copied to the drivers folder. Choose it in the list to use it.",
                "%s a été copié dans le dossier des pilotes. Choisissez-le dans la liste pour l'utiliser.",
                "%s wurde in den Treiberordner kopiert. Wähle ihn in der Liste, um ihn zu verwenden.",
                "%s è stato copiato nella cartella dei driver. Sceglilo nell'elenco per usarlo.",
                "%s se copió a la carpeta de controladores. Elígelo en la lista para usarlo.",
                "%s foi copiado para a pasta de drivers. Escolha-o na lista para usá-lo.",
                "%s скопирован в папку драйверов. Выберите его в списке, чтобы использовать.",
                "%s をドライバーフォルダーにコピーしました。使用するには一覧から選んでください。",
                "%s을(를) 드라이버 폴더에 복사했습니다. 사용하려면 목록에서 선택하세요.", "%s 已复制到驱动文件夹。在列表中选择它即可使用。",
                "%s 已複製到驅動程式資料夾。在清單中選擇即可使用。");
        put("driver_failed_t", "Could not add the driver", "Impossible d'ajouter le pilote",
                "Treiber konnte nicht hinzugefügt werden", "Impossibile aggiungere il driver",
                "No se pudo añadir el controlador", "Não foi possível adicionar o driver", "Не удалось добавить драйвер",
                "ドライバーを追加できませんでした", "드라이버를 추가할 수 없습니다", "无法添加驱动", "無法新增驅動程式");
        put("driver_failed", "Copying %s failed: %s", "La copie de %s a échoué : %s",
                "Kopieren von %s fehlgeschlagen: %s", "Copia di %s non riuscita: %s", "Error al copiar %s: %s",
                "Falha ao copiar %s: %s", "Не удалось скопировать %s: %s", "%s のコピーに失敗しました：%s", "%s 복사 실패: %s",
                "复制 %s 失败：%s", "複製 %s 失敗：%s");

        // ---- Settings tab
        put("sec_display", "Display", "Affichage", "Anzeige", "Schermo", "Pantalla", "Tela", "Экран", "画面", "화면",
                "显示", "顯示");
        put("fps_t", "Frame rate cap", "Limite d'images par seconde", "Bildratenbegrenzung", "Limite di frame rate",
                "Límite de fotogramas", "Limite de quadros", "Ограничение частоты кадров", "フレームレート上限",
                "프레임 속도 제한", "帧率上限", "影格率上限");
        put("fps_auto", "Auto", "Auto", "Auto", "Auto", "Auto", "Auto", "Авто", "自動", "자동", "自动", "自動");
        put("fps_auto_n", "Auto picks 60, 30 or 20 fps by what your device can hold, and keeps the game at normal speed.",
                "Auto choisit 60, 30 ou 20 IPS selon ce que l'appareil peut tenir, et garde le jeu à vitesse normale.",
                "Auto wählt 60, 30 oder 20 FPS, je nachdem, was das Gerät schafft, und hält das Spiel auf normaler Geschwindigkeit.",
                "Auto sceglie 60, 30 o 20 FPS in base a ciò che il dispositivo regge e mantiene il gioco a velocità normale.",
                "Auto elige 60, 30 o 20 FPS según lo que aguante tu dispositivo y mantiene el juego a velocidad normal.",
                "Auto escolhe 60, 30 ou 20 FPS conforme o que o aparelho aguenta e mantém o jogo na velocidade normal.",
                "«Авто» выбирает 60, 30 или 20 FPS по возможностям устройства и сохраняет нормальную скорость игры.",
                "自動では端末が維持できる 60・30・20 FPS を選び、ゲームの速度は通常のまま保ちます。",
                "자동은 기기가 유지할 수 있는 60, 30, 20 FPS 중에서 고르며 게임 속도는 정상으로 유지합니다.",
                "自动会根据设备的承受能力选择 60、30 或 20 FPS，并保持游戏正常速度。",
                "自動會依裝置可維持的程度選擇 60、30 或 20 FPS，並保持遊戲正常速度。");
        put("minfps_t", "Lowest auto frame rate", "Plus bas en mode Auto", "Niedrigste Auto-Bildrate",
                "Frame rate minimo in Auto", "Mínimo en modo Auto", "Mínimo no modo Auto",
                "Минимум в режиме Авто", "自動の最低フレームレート", "자동 최저 프레임 속도", "自动模式最低帧率", "自動模式最低幀率");
        put("minfps_n", "Only with Auto: the slowest rate Auto may drop to. Lower looks smoother on weak devices, but the game runs slower.",
                "Seulement avec Auto : le taux le plus bas qu'Auto peut atteindre. Plus bas = plus fluide sur les appareils faibles, mais jeu plus lent.",
                "Nur bei Auto: die niedrigste Rate, auf die Auto fallen darf. Niedriger wirkt flüssiger auf schwachen Geräten, das Spiel läuft dafür langsamer.",
                "Solo con Auto: la velocità più bassa a cui Auto può scendere. Più bassa = più fluida sui dispositivi deboli, ma gioco più lento.",
                "Solo con Auto: la velocidad más baja a la que puede bajar Auto. Más baja = más fluida en equipos débiles, pero el juego va más lento.",
                "Só com Auto: a menor taxa a que o Auto pode cair. Mais baixa = mais fluida em aparelhos fracos, mas o jogo fica mais lento.",
                "Только для Авто: самая низкая частота, до которой может опускаться Авто. Ниже — плавнее на слабых устройствах, но игра замедляется.",
                "自動のときのみ：自動が下げる最低のレートです。低いほど弱い機種で滑らかですが、ゲームは遅くなります。",
                "자동일 때만: 자동이 내려갈 수 있는 최저 속도입니다. 낮을수록 약한 기기에서 부드럽지만 게임이 느려집니다.",
                "仅自动模式：自动可降到的最低帧率。越低在弱机上越流畅，但游戏会变慢。",
                "僅自動模式：自動可降到的最低幀率。越低在弱機上越流暢，但遊戲會變慢。");
        put("scalef_t", "Upscale filter", "Filtre d'agrandissement", "Vergrößerungsfilter", "Filtro di ingrandimento",
                "Filtro de ampliación", "Filtro de ampliação", "Фильтр увеличения", "拡大フィルター", "확대 필터", "放大滤镜", "放大濾鏡");
        put("scalef_n", "Smooths the enlarged picture (bilinear) or keeps pixels sharp (nearest). Bicubic and FXAA need shaders: not yet.",
                "Lisse l'image agrandie (bilinéaire) ou garde les pixels nets (proche). Bicubique et FXAA demandent des shaders : pas encore.",
                "Glättet das vergrößerte Bild (bilinear) oder hält die Pixel scharf (nächster Nachbar). Bikubisch und FXAA brauchen Shader: noch nicht.",
                "Leviga l'immagine ingrandita (bilineare) o mantiene i pixel nitidi (vicino). Bicubico e FXAA richiedono shader: non ancora.",
                "Suaviza la imagen ampliada (bilineal) o mantiene los píxeles nítidos (vecino). Bicúbico y FXAA necesitan shaders: aún no.",
                "Suaviza a imagem ampliada (bilinear) ou mantém os pixels nítidos (vizinho). Bicúbico e FXAA precisam de shaders: ainda não.",
                "Сглаживает увеличенное изображение (билинейный) или сохраняет пиксели четкими (ближайший). Бикубический и FXAA требуют шейдеров: пока нет.",
                "拡大画像を滑らかにします（バイリニア）。ピクセルを鮮明に保つ場合はニアレスト。バイキュービックとFXAAはシェーダーが必要で、まだ未対応です。",
                "확대된 화면을 부드럽게(바이리니어) 또는 픽셀을 선명하게(니어리스트) 유지합니다. 바이큐빅과 FXAA는 셰이더가 필요해 아직 없습니다.",
                "平滑放大后的画面（双线性），或保持像素锐利（最近邻）。双三次和FXAA需要着色器，暂不可用。",
                "平滑放大後的畫面（雙線性），或保持像素銳利（最近鄰）。雙三次和FXAA需要著色器，暫不可用。");
        put("scalef_0", "Nearest", "Pixels nets", "Nächster Nachbar", "Vicino", "Vecino más cercano", "Vizinho mais próximo",
                "Ближайший", "ニアレスト", "니어리스트", "最近邻", "最近鄰");
        put("scalef_1", "Bilinear", "Bilinéaire", "Bilinear", "Bilineare", "Bilineal", "Bilinear", "Билинейный",
                "バイリニア", "바이리니어", "双线性", "雙線性");
        put("tab_graphics", "Graphics", "Graphismes", "Grafik", "Grafica", "Gráficos", "Gráficos", "Графика", "グラフィック",
                "그래픽", "图形", "圖形");
        put("sec_game", "Game", "Jeu", "Spiel", "Gioco", "Juego", "Jogo", "Игра", "ゲーム", "게임", "游戏", "遊戲");
        put("sec_quality", "Quality", "Qualité", "Qualität", "Qualità", "Calidad", "Qualidade", "Качество", "画質", "품질",
                "画质", "畫質");
        put("perf_t", "Performance mode", "Mode performance", "Leistungsmodus", "Modalità prestazioni",
                "Modo rendimiento", "Modo desempenho", "Режим производительности", "パフォーマンスモード", "성능 모드",
                "性能模式", "效能模式");
        put("perf_n",
                "Lighter effects, lower-resolution shadows and a 75% render scale (the slider below can still be moved).",
                "Effets plus légers, ombres en basse résolution et échelle de rendu à 75 % (le curseur ci-dessous reste réglable).",
                "Leichtere Effekte, Schatten in niedrigerer Auflösung und 75 % Renderskalierung (der Regler unten bleibt einstellbar).",
                "Effetti più leggeri, ombre a risoluzione inferiore e scala di rendering al 75% (il cursore qui sotto resta regolabile).",
                "Efectos más ligeros, sombras de menor resolución y escala de renderizado al 75 % (el control de abajo se puede seguir moviendo).",
                "Efeitos mais leves, sombras em resolução menor e escala de renderização em 75% (o controle abaixo ainda pode ser ajustado).",
                "Облегчённые эффекты, тени в низком разрешении и масштаб рендеринга 75% (ползунок ниже можно менять).",
                "エフェクトを軽くし、影の解像度を下げ、レンダリング倍率を 75% にします（下のスライダーは自由に調整できます）。",
                "가벼운 효과, 낮은 해상도의 그림자, 75% 렌더링 배율을 사용합니다 (아래 슬라이더는 계속 조절할 수 있습니다).",
                "更轻的特效、更低分辨率的阴影和 75% 的渲染比例（下方滑块仍可调节）。",
                "更輕的特效、更低解析度的陰影和 75% 的繪製比例（下方滑桿仍可調整）。");
        put("sec_gpu", "GPU options", "Options GPU", "GPU-Optionen", "Opzioni GPU", "Opciones de GPU", "Opções de GPU",
                "Параметры GPU", "GPU オプション", "GPU 옵션", "GPU 选项", "GPU 選項");
        put("gpu_n", "For GPUs such as Mali that lack the game's texture formats or draw in tiles. Off unless you are testing.",
                "Pour les GPU comme Mali, sans les formats de texture du jeu ou qui dessinent par tuiles. Désactivé sauf pour des tests.",
                "Für GPUs wie Mali, denen die Texturformate des Spiels fehlen oder die in Kacheln zeichnen. Aus, außer zum Testen.",
                "Per GPU come Mali, prive dei formati di texture del gioco o che disegnano a tile. Disattivato salvo test.",
                "Para GPU como Mali, sin los formatos de textura del juego o que dibujan por mosaicos. Desactivado salvo para pruebas.",
                "Para GPUs como Mali, sem os formatos de textura do jogo ou que desenham em blocos. Desligado, exceto para testes.",
                "Для GPU вроде Mali, без форматов текстур игры или с плиточной отрисовкой. Выключено, если вы не тестируете.",
                "Mali など、ゲームのテクスチャ形式がない、またはタイル方式で描画する GPU 向けです。テスト時以外はオフ。",
                "Mali처럼 게임의 텍스처 형식이 없거나 타일 방식으로 그리는 GPU용입니다. 테스트할 때가 아니면 끄세요.",
                "适用于 Mali 等缺少游戏纹理格式或以分块方式绘制的 GPU。除测试外请保持关闭。",
                "適用於 Mali 等缺少遊戲紋理格式或以分塊方式繪製的 GPU。除測試外請保持關閉。");
        put("bc3_t", "Compress BC3 textures (ETC2)", "Compresser les textures BC3 (ETC2)", "BC3-Texturen komprimieren (ETC2)",
                "Comprimi texture BC3 (ETC2)", "Comprimir texturas BC3 (ETC2)", "Comprimir texturas BC3 (ETC2)",
                "Сжимать текстуры BC3 (ETC2)", "BC3 テクスチャを圧縮 (ETC2)", "BC3 텍스처 압축 (ETC2)", "压缩 BC3 纹理 (ETC2)",
                "壓縮 BC3 紋理 (ETC2)");
        put("bc3_n", "4x less texture memory on GPUs without BC support; slightly lossy.",
                "4 fois moins de mémoire de textures sur les GPU sans BC ; légère perte de qualité.",
                "4x weniger Texturspeicher auf GPUs ohne BC; leicht verlustbehaftet.",
                "Memoria texture ridotta di 4 volte sulle GPU senza BC; lievemente lossy.",
                "4 veces menos memoria de texturas en GPU sin BC; con una ligera pérdida.",
                "4x menos memória de texturas em GPUs sem BC; com leve perda.",
                "В 4 раза меньше памяти под текстуры на GPU без BC; с небольшой потерей качества.",
                "BC 非対応の GPU でテクスチャメモリが 4 分の 1 に。わずかに劣化します。",
                "BC를 지원하지 않는 GPU에서 텍스처 메모리가 4분의 1로 줄어듭니다. 약간 손실이 있습니다.",
                "在不支持 BC 的 GPU 上纹理内存减少至 1/4；略有损失。",
                "在不支援 BC 的 GPU 上紋理記憶體減少至 1/4；略有損失。");
        put("mip_t", "Texture mip skip", "Saut de mip des textures", "Textur-Mip überspringen", "Salto mip delle texture",
                "Omitir mip de texturas", "Pular mip de texturas", "Пропуск MIP текстур", "テクスチャ MIP スキップ",
                "텍스처 밉 건너뛰기", "跳过纹理 Mip", "略過紋理 Mip");
        put("mip_n",
                "Big textures load without their most detailed level: about 75% less texture data, softer up close. Performance mode turns it on on GPUs without BC support.",
                "Les grandes textures se chargent sans leur niveau le plus détaillé : environ 75 % de données en moins, plus flou de près. Le mode performance l'active sur les GPU sans BC.",
                "Große Texturen werden ohne ihre detaillierteste Stufe geladen: etwa 75 % weniger Texturdaten, aus der Nähe weicher. Der Leistungsmodus schaltet es auf GPUs ohne BC ein.",
                "Le texture grandi si caricano senza il livello più dettagliato: circa il 75% di dati in meno, più morbide da vicino. La modalità prestazioni lo attiva sulle GPU senza BC.",
                "Las texturas grandes se cargan sin su nivel más detallado: un 75 % menos de datos, más suaves de cerca. El modo rendimiento lo activa en GPU sin BC.",
                "Texturas grandes carregam sem o nível mais detalhado: cerca de 75% menos dados, mais suaves de perto. O modo desempenho liga isso em GPUs sem BC.",
                "Большие текстуры загружаются без самого детального уровня: примерно на 75% меньше данных, вблизи мягче. Режим производительности включает это на GPU без BC.",
                "大きなテクスチャを最も詳細なレベルなしで読み込みます：データ量は約 75% 減、近くでは少しぼやけます。BC 非対応 GPU ではパフォーマンスモードでオンになります。",
                "큰 텍스처를 가장 세밀한 단계 없이 불러옵니다: 텍스처 데이터 약 75% 감소, 가까이서는 조금 흐립니다. BC를 지원하지 않는 GPU에서는 성능 모드가 이를 켭니다.",
                "大纹理不加载最精细的一级：纹理数据减少约 75%，近看略模糊。在不支持 BC 的 GPU 上，性能模式会将其开启。",
                "大型紋理不載入最精細的一級：紋理資料減少約 75%，近看略模糊。在不支援 BC 的 GPU 上，效能模式會將其開啟。");
        put("malib_t", "Tile-GPU barriers (experimental)", "Barrières GPU à tuiles (expérimental)",
                "Kachel-GPU-Barrieren (experimentell)", "Barriere GPU a tile (sperimentale)",
                "Barreras para GPU por mosaicos (experimental)", "Barreiras para GPU em blocos (experimental)",
                "Барьеры для плиточных GPU (эксперимент)", "タイル GPU 用バリア（実験的）", "타일 GPU 배리어 (실험적)",
                "分块 GPU 屏障（实验性）", "分塊 GPU 屏障（實驗性）");
        put("malib_n", "Lets GPUs like Mali overlap rendering passes. Turn off if effects flicker or go missing.",
                "Permet aux GPU comme Mali de chevaucher les passes de rendu. Désactivez si des effets clignotent ou disparaissent.",
                "Lässt GPUs wie Mali Renderdurchläufe überlappen. Ausschalten, wenn Effekte flackern oder fehlen.",
                "Permette alle GPU come Mali di sovrapporre i passaggi di rendering. Disattiva se gli effetti sfarfallano o spariscono.",
                "Permite a GPU como Mali solapar pasadas de renderizado. Desactívalo si los efectos parpadean o desaparecen.",
                "Permite que GPUs como Mali sobreponham passagens de renderização. Desligue se efeitos piscarem ou sumirem.",
                "Позволяет GPU вроде Mali совмещать проходы отрисовки. Выключите, если эффекты мерцают или пропадают.",
                "Mali などの GPU で描画パスを重ねて実行します。エフェクトがちらつく・消える場合はオフにしてください。",
                "Mali 같은 GPU가 렌더링 패스를 겹쳐 실행합니다. 효과가 깜박이거나 사라지면 끄세요.",
                "让 Mali 等 GPU 重叠执行渲染通道。如果特效闪烁或消失，请关闭。",
                "讓 Mali 等 GPU 重疊執行繪製通道。如果特效閃爍或消失，請關閉。");
        put("aspect_t", "Aspect ratio (experimental)", "Format d'image (expérimental)",
                "Seitenverhältnis (experimentell)", "Proporzioni (sperimentale)", "Relación de aspecto (experimental)",
                "Proporção de tela (experimental)", "Соотношение сторон (экспериментально)", "アスペクト比（試験的）",
                "화면 비율(실험적)", "宽高比（实验性）", "長寬比（實驗性）");
        put("aspect_n",
                "16:9 is the game's own. Fill screen widens (or narrows) the view to the screen's shape. Changes at the next start.",
                "16:9 est le format du jeu. Plein écran élargit (ou rétrécit) la vue à la forme de l'écran. S'applique au prochain lancement.",
                "16:9 ist das Format des Spiels. Bildschirm füllen verbreitert (oder verengt) die Ansicht auf die Bildschirmform. Wirkt beim nächsten Start.",
                "16:9 è quello del gioco. Riempi schermo allarga (o restringe) la vista alla forma dello schermo. Si applica al prossimo avvio.",
                "16:9 es el del juego. Llenar pantalla ensancha (o estrecha) la vista a la forma de la pantalla. Se aplica en el próximo inicio.",
                "16:9 é o do jogo. Preencher tela alarga (ou estreita) a visão para o formato da tela. Aplica no próximo início.",
                "16:9 — родной формат игры. «Во весь экран» расширяет (или сужает) обзор под форму экрана. Применяется при следующем запуске.",
                "16:9 はゲーム本来の比率です。「全画面」は画面の形に合わせて視野を広げ（または狭め）ます。次回起動時に反映されます。",
                "16:9는 게임 본래 비율입니다. 전체 화면은 화면 모양에 맞게 시야를 넓히거나 좁힙니다. 다음 시작 시 적용됩니다.",
                "16:9 为游戏原始比例。「填满屏幕」会按屏幕形状加宽（或收窄）视野。下次启动时生效。",
                "16:9 為遊戲原始比例。「填滿螢幕」會依螢幕形狀加寬（或縮窄）視野。下次啟動時生效。");
        put("aspect_fill", "Fill screen", "Plein écran", "Bildschirm füllen", "Riempi schermo", "Llenar pantalla",
                "Preencher tela", "Во весь экран", "全画面", "전체 화면", "填满屏幕", "填滿螢幕");
        put("scale_t", "Render scale (experimental)", "Échelle de rendu (expérimental)",
                "Renderskalierung (experimentell)", "Scala di rendering (sperimentale)",
                "Escala de renderizado (experimental)", "Escala de renderização (experimental)",
                "Масштаб рендеринга (экспериментально)", "レンダリングスケール（試験的）", "렌더링 배율(실험적)", "渲染比例（实验性）",
                "渲染比例（實驗性）");
        put("scale_n",
                "Draws the game at a lower resolution for more speed; the picture is enlarged to fit the screen. Changes at the next start.",
                "Dessine le jeu à une résolution plus basse pour gagner en vitesse ; l'image est agrandie pour remplir l'écran. S'applique au prochain lancement.",
                "Berechnet das Spiel in niedrigerer Auflösung für mehr Tempo; das Bild wird auf den Bildschirm vergrößert. Wirkt beim nächsten Start.",
                "Disegna il gioco a una risoluzione più bassa per più velocità; l'immagine viene ingrandita per riempire lo schermo. Si applica al prossimo avvio.",
                "Dibuja el juego a menor resolución para ganar velocidad; la imagen se amplía para llenar la pantalla. Se aplica en el próximo inicio.",
                "Desenha o jogo em resolução menor para ganhar velocidade; a imagem é ampliada para caber na tela. Aplica no próximo início.",
                "Рисует игру в меньшем разрешении для скорости; картинка растягивается на экран. Применяется при следующем запуске.",
                "より低い解像度で描画して速度を上げます。映像は画面に合わせて拡大されます。次回起動時に反映されます。",
                "더 낮은 해상도로 그려 속도를 높이며, 화면은 크기에 맞게 확대됩니다. 다음 시작 시 적용됩니다.",
                "以较低分辨率绘制游戏以提高速度，画面会放大以适应屏幕。下次启动时生效。", "以較低解析度繪製遊戲以提升速度，畫面會放大以符合螢幕。下次啟動時生效。");
        put("scale_res", "Render resolution: %d × %d", "Résolution de rendu : %d × %d", "Renderauflösung: %d × %d",
                "Risoluzione di rendering: %d × %d", "Resolución de renderizado: %d × %d",
                "Resolução de renderização: %d × %d", "Разрешение рендеринга: %d × %d", "レンダリング解像度：%d × %d",
                "렌더링 해상도: %d × %d", "渲染分辨率：%d × %d", "渲染解析度：%d × %d");
        put("skip_t", "Skip intro logos", "Passer les logos d'introduction", "Intro-Logos überspringen",
                "Salta i loghi iniziali", "Omitir logotipos de inicio", "Pular logotipos de abertura",
                "Пропускать заставки с логотипами", "起動時のロゴをスキップ", "시작 로고 건너뛰기", "跳过开场标志",
                "略過開場標誌");
        put("skip_n", "Goes straight to the title screen without the company logos. Changes at the next start.",
                "Passe directement à l'écran titre, sans les logos des studios. S'applique au prochain lancement.",
                "Direkt zum Titelbildschirm, ohne die Firmenlogos. Gilt ab dem nächsten Start.",
                "Va direttamente alla schermata del titolo senza i loghi delle aziende. Si applica al prossimo avvio.",
                "Va directamente a la pantalla de título sin los logotipos de las compañías. Se aplica en el próximo inicio.",
                "Vai direto para a tela de título sem os logotipos das empresas. Vale a partir da próxima abertura.",
                "Сразу переходит к титульному экрану без логотипов компаний. Применяется при следующем запуске.",
                "会社ロゴを表示せずにタイトル画面へ進みます。次回の起動時に反映されます。",
                "회사 로고 없이 바로 타이틀 화면으로 넘어갑니다. 다음 실행 시 적용됩니다.",
                "不显示公司标志，直接进入标题画面。下次启动时生效。",
                "不顯示公司標誌，直接進入標題畫面。下次啟動時生效。");
        put("fpscounter_t", "FPS counter", "Compteur d'IPS", "FPS-Anzeige", "Contatore FPS", "Contador de FPS",
                "Contador de FPS", "Счётчик FPS", "FPS カウンター", "FPS 표시", "FPS 计数器", "FPS 計數器");
        put("fpscounter_n",
                "Shows the frame rate at the top of the screen. Drawn by Android over the game, so it costs no frames.",
                "Affiche les images par seconde en haut de l'écran. Dessiné par Android par-dessus le jeu, il ne coûte aucune image.",
                "Zeigt die Bildrate oben am Bildschirm. Android zeichnet sie über das Spiel, das kostet keine Bilder.",
                "Mostra il frame rate in alto sullo schermo. È disegnato da Android sopra il gioco, quindi non costa frame.",
                "Muestra los FPS en la parte superior. Android lo dibuja sobre el juego, así que no cuesta fotogramas.",
                "Mostra o FPS no topo da tela. É desenhado pelo Android sobre o jogo, então não custa quadros.",
                "Показывает частоту кадров вверху экрана. Рисуется Android поверх игры, поэтому не отнимает кадров.",
                "画面上部にフレームレートを表示します。Android がゲームの上に描くため、フレームを消費しません。",
                "화면 위쪽에 프레임 속도를 표시합니다. Android가 게임 위에 그리므로 프레임을 소모하지 않습니다.",
                "在屏幕顶部显示帧率。由 Android 绘制在游戏之上，不会占用帧数。", "在螢幕頂端顯示影格率。由 Android 繪製於遊戲上層，不會占用影格。");
        put("bright_t", "Brightness", "Luminosité", "Helligkeit", "Luminosità", "Brillo", "Brilho", "Яркость", "明るさ",
                "밝기", "亮度", "亮度");
        put("bright_orig", "Original", "Originale", "Original", "Originale", "Original", "Original", "Исходная",
                "オリジナル", "원본", "原始", "原始");
        put("bright_def", "Default", "Par défaut", "Standard", "Predefinita", "Predeterminado", "Padrão",
                "По умолчанию", "既定", "기본값", "默认", "預設");
        put("bright_more", "Brighter", "Plus clair", "Heller", "Più luminosa", "Más brillo", "Mais claro", "Ярче",
                "明るめ", "더 밝게", "更亮", "更亮");
        put("present_t", "Frame presentation", "Présentation des images", "Bildausgabe", "Presentazione dei frame",
                "Presentación de fotogramas", "Apresentação de quadros", "Вывод кадров", "フレーム表示方式", "프레임 표시 방식",
                "画面呈现方式", "畫面呈現方式");
        put("present_mailbox", "Low latency", "Faible latence", "Geringe Latenz", "Bassa latenza", "Baja latencia",
                "Baixa latência", "Низкая задержка", "低遅延", "낮은 지연", "低延迟", "低延遲");
        put("present_vsync", "V-sync", "V-sync", "V-Sync", "V-sync", "V-sync", "V-sync", "Верт. синхронизация",
                "垂直同期", "수직 동기화", "垂直同步", "垂直同步");
        put("sec_controls", "Controller", "Manette", "Controller", "Controller", "Mando", "Controle", "Геймпад",
                "コントローラー", "컨트롤러", "手柄", "手把");
        put("swap_t", "Swap A / B and X / Y inputs", "Inverser A / B et X / Y", "A / B und X / Y tauschen",
                "Scambia A / B e X / Y", "Intercambiar A / B y X / Y", "Trocar A / B e X / Y", "Поменять A / B и X / Y",
                "A / B と X / Y を入れ替える", "A / B와 X / Y 바꾸기", "交换 A / B 与 X / Y", "交換 A / B 與 X / Y");
        put("swap_n", "On: the face buttons act by position, as on a Switch (the right button confirms).",
                "Activé : les boutons agissent selon leur position, comme sur Switch (le bouton de droite valide).",
                "An: Die Tasten wirken nach ihrer Position wie auf der Switch (die rechte Taste bestätigt).",
                "Sì: i pulsanti agiscono in base alla posizione, come su Switch (il pulsante a destra conferma).",
                "Sí: los botones actúan según su posición, como en Switch (el botón derecho confirma).",
                "Ligado: os botões agem pela posição, como no Switch (o botão da direita confirma).",
                "Вкл: кнопки работают по расположению, как на Switch (правая кнопка — подтверждение).",
                "オン：ボタンは Switch と同じく位置で動作します（右のボタンで決定）。",
                "켜기: 버튼이 Switch처럼 위치에 따라 동작합니다(오른쪽 버튼이 확인).", "开：按键按位置生效，与 Switch 相同（右侧按键为确认）。",
                "開：按鍵依位置作用，與 Switch 相同（右側按鍵為確認）。");
        put("sec_folder", "Game folder", "Dossier du jeu", "Spielordner", "Cartella del gioco", "Carpeta del juego",
                "Pasta do jogo", "Папка игры", "ゲームフォルダー", "게임 폴더", "游戏文件夹", "遊戲資料夾");
        put("folder_inuse", "Game files in use: %s", "Fichiers du jeu utilisés : %s", "Verwendete Spieldateien: %s",
                "File di gioco in uso: %s", "Archivos del juego en uso: %s", "Arquivos do jogo em uso: %s",
                "Используемые файлы игры: %s", "使用中のゲームファイル：%s", "사용 중인 게임 파일: %s", "正在使用的游戏文件：%s",
                "使用中的遊戲檔案：%s");
        put("folder_note",
                "The app looks in %s first. If the game is not there, it uses the folder chosen here (or asks for one).",
                "L'application cherche d'abord dans %s. Si le jeu n'y est pas, elle utilise le dossier choisi ici (ou en demande un).",
                "Die App sucht zuerst in %s. Ist das Spiel dort nicht, nutzt sie den hier gewählten Ordner (oder fragt nach einem).",
                "L'app cerca prima in %s. Se il gioco non c'è, usa la cartella scelta qui (o ne chiede una).",
                "La aplicación busca primero en %s. Si el juego no está ahí, usa la carpeta elegida aquí (o pide una).",
                "O app procura primeiro em %s. Se o jogo não estiver lá, usa a pasta escolhida aqui (ou pede uma).",
                "Приложение сначала ищет в %s. Если игры там нет, используется выбранная здесь папка (или приложение попросит её выбрать).",
                "アプリはまず %s を探します。そこにゲームがない場合は、ここで選んだフォルダーを使用します（またはフォルダーを尋ねます）。",
                "앱은 먼저 %s에서 찾습니다. 게임이 없으면 여기서 선택한 폴더를 사용합니다(또는 폴더를 묻습니다).",
                "应用会先在 %s 中查找。如果游戏不在那里，则使用此处选择的文件夹（或请你选择一个）。",
                "應用程式會先在 %s 中尋找。若遊戲不在那裡，則使用此處選擇的資料夾（或請你選擇一個）。");
        put("folder_chosen", "Chosen folder: %s", "Dossier choisi : %s", "Gewählter Ordner: %s", "Cartella scelta: %s",
                "Carpeta elegida: %s", "Pasta escolhida: %s", "Выбранная папка: %s", "選択したフォルダー：%s",
                "선택한 폴더: %s", "已选择的文件夹：%s", "已選擇的資料夾：%s");
        put("folder_pick", "Choose another game folder…", "Choisir un autre dossier de jeu…",
                "Anderen Spielordner wählen…", "Scegli un'altra cartella di gioco…", "Elegir otra carpeta del juego…",
                "Escolher outra pasta do jogo…", "Выбрать другую папку игры…", "別のゲームフォルダーを選ぶ…",
                "다른 게임 폴더 선택…", "选择其他游戏文件夹…", "選擇其他遊戲資料夾…");
        put("install_pkg", "Install game or update from .nsp / .xci…", "Installer le jeu ou une mise à jour (.nsp / .xci)…",
                "Spiel oder Update aus .nsp / .xci installieren…", "Installa gioco o aggiornamento da .nsp / .xci…",
                "Instalar juego o actualización desde .nsp / .xci…", "Instalar jogo ou atualização de .nsp / .xci…",
                "Установить игру или обновление из .nsp / .xci…", ".nsp / .xci からゲームまたは更新をインストール…",
                ".nsp / .xci에서 게임 또는 업데이트 설치…", "从 .nsp / .xci 安装游戏或更新…", "從 .nsp / .xci 安裝遊戲或更新…");
        put("install_n",
                "Select the game's .nsp / .xci (and its update), or only the update (1.0.3) to update the game in this folder. Needs your Switch's prod.keys. Used from the next start.",
                "Sélectionnez le .nsp / .xci du jeu (et sa mise à jour), ou seulement la mise à jour (1.0.3) pour mettre à jour le jeu de ce dossier. Nécessite le prod.keys de votre Switch. Utilisé au prochain lancement.",
                "Wähle die .nsp / .xci des Spiels (und das Update) oder nur das Update (1.0.3), um das Spiel in diesem Ordner zu aktualisieren. Benötigt die prod.keys deiner Switch. Gilt ab dem nächsten Start.",
                "Seleziona il .nsp / .xci del gioco (e l'aggiornamento), o solo l'aggiornamento (1.0.3) per aggiornare il gioco in questa cartella. Servono le prod.keys della tua Switch. Usato dal prossimo avvio.",
                "Selecciona el .nsp / .xci del juego (y su actualización), o solo la actualización (1.0.3) para actualizar el juego de esta carpeta. Necesita el prod.keys de tu Switch. Se usa desde el próximo inicio.",
                "Selecione o .nsp / .xci do jogo (e a atualização), ou só a atualização (1.0.3) para atualizar o jogo desta pasta. Precisa do prod.keys do seu Switch. Vale a partir do próximo início.",
                "Выберите .nsp / .xci игры (и обновление) или только обновление (1.0.3), чтобы обновить игру в этой папке. Нужен prod.keys вашей Switch. Применяется со следующего запуска.",
                "ゲームの .nsp / .xci（と更新データ）、またはこのフォルダーのゲームを更新する場合は更新データ（1.0.3）のみを選択します。Switch の prod.keys が必要です。次回起動時から使用されます。",
                "게임의 .nsp / .xci(및 업데이트)를 선택하거나, 이 폴더의 게임을 업데이트하려면 업데이트(1.0.3)만 선택하세요. Switch의 prod.keys가 필요합니다. 다음 시작부터 사용됩니다.",
                "选择游戏的 .nsp / .xci（及其更新），或仅选择更新（1.0.3）来更新此文件夹中的游戏。需要你的 Switch 的 prod.keys。下次启动时生效。",
                "選擇遊戲的 .nsp / .xci（及其更新），或僅選擇更新（1.0.3）來更新此資料夾中的遊戲。需要你的 Switch 的 prod.keys。下次啟動時生效。");
        put("saved_in", "Settings are saved in %s", "Les paramètres sont enregistrés dans %s",
                "Einstellungen werden in %s gespeichert", "Le impostazioni sono salvate in %s",
                "Los ajustes se guardan en %s", "As configurações são salvas em %s", "Настройки сохраняются в %s",
                "設定は %s に保存されます", "설정은 %s에 저장됩니다", "设置保存在 %s", "設定儲存在 %s");
        put("no_fm_t", "No file manager", "Aucun gestionnaire de fichiers", "Kein Dateimanager", "Nessun file manager",
                "No hay gestor de archivos", "Nenhum gerenciador de arquivos", "Нет файлового менеджера",
                "ファイルマネージャーがありません", "파일 관리자가 없습니다", "没有文件管理器", "沒有檔案管理員");
        put("no_fm", "No app is available to choose a folder.", "Aucune application ne permet de choisir un dossier.",
                "Keine App zum Auswählen eines Ordners verfügbar.", "Nessuna app disponibile per scegliere una cartella.",
                "No hay ninguna aplicación para elegir una carpeta.", "Nenhum app disponível para escolher uma pasta.",
                "Нет приложения для выбора папки.", "フォルダーを選択できるアプリがありません。", "폴더를 선택할 수 있는 앱이 없습니다.",
                "没有可用于选择文件夹的应用。", "沒有可用來選擇資料夾的應用程式。");
        put("folder_set", "Game folder set to %s.\n\nIt is used from the next start whenever the game is not in %s.",
                "Dossier du jeu défini sur %s.\n\nIl sera utilisé dès le prochain lancement si le jeu n'est pas dans %s.",
                "Spielordner auf %s gesetzt.\n\nEr wird ab dem nächsten Start verwendet, wenn das Spiel nicht in %s liegt.",
                "Cartella del gioco impostata su %s.\n\nVerrà usata dal prossimo avvio se il gioco non è in %s.",
                "Carpeta del juego establecida en %s.\n\nSe usará desde el próximo inicio cuando el juego no esté en %s.",
                "Pasta do jogo definida como %s.\n\nEla será usada a partir do próximo início quando o jogo não estiver em %s.",
                "Папка игры: %s.\n\nОна будет использоваться со следующего запуска, если игры нет в %s.",
                "ゲームフォルダーを %s に設定しました。\n\n次回起動時から、%s にゲームがない場合に使用されます。",
                "게임 폴더를 %s(으)로 설정했습니다.\n\n다음 시작부터 %s에 게임이 없을 때 사용됩니다.",
                "游戏文件夹已设为 %s。\n\n从下次启动起，当游戏不在 %s 中时将使用它。", "遊戲資料夾已設為 %s。\n\n從下次啟動起，當遊戲不在 %s 中時將使用它。");
        put("incomplete_t", "Game files incomplete", "Fichiers du jeu incomplets", "Spieldateien unvollständig",
                "File di gioco incompleti", "Archivos del juego incompletos", "Arquivos do jogo incompletos",
                "Файлы игры неполные", "ゲームファイルが不完全です", "게임 파일이 불완전합니다", "游戏文件不完整", "遊戲檔案不完整");
        put("incomplete",
                "%s is missing:\n%s\n\nThese files belong next to the exefs folder. Choose a folder that holds the complete game.",
                "Il manque dans %s :\n%s\n\nCes fichiers vont à côté du dossier exefs. Choisissez un dossier qui contient le jeu complet.",
                "In %s fehlt:\n%s\n\nDiese Dateien gehören neben den Ordner exefs. Wähle einen Ordner mit dem vollständigen Spiel.",
                "In %s manca:\n%s\n\nQuesti file vanno accanto alla cartella exefs. Scegli una cartella che contenga il gioco completo.",
                "A %s le falta:\n%s\n\nEstos archivos van junto a la carpeta exefs. Elige una carpeta que contenga el juego completo.",
                "Em %s falta:\n%s\n\nEsses arquivos ficam ao lado da pasta exefs. Escolha uma pasta que contenha o jogo completo.",
                "В %s не хватает:\n%s\n\nЭти файлы должны лежать рядом с папкой exefs. Выберите папку с полной игрой.",
                "%s に次のファイルがありません：\n%s\n\nこれらは exefs フォルダーと同じ場所に必要です。ゲーム一式が入ったフォルダーを選んでください。",
                "%s에 다음 파일이 없습니다:\n%s\n\n이 파일들은 exefs 폴더 옆에 있어야 합니다. 게임 전체가 들어 있는 폴더를 선택하세요.",
                "%s 缺少：\n%s\n\n这些文件应与 exefs 文件夹放在一起。请选择包含完整游戏的文件夹。",
                "%s 缺少：\n%s\n\n這些檔案應與 exefs 資料夾放在一起。請選擇包含完整遊戲的資料夾。");
        put("nofiles_t", "No game files there", "Aucun fichier du jeu ici", "Dort sind keine Spieldateien",
                "Nessun file di gioco qui", "No hay archivos del juego ahí", "Não há arquivos do jogo ali",
                "Здесь нет файлов игры", "ゲームファイルがありません", "게임 파일이 없습니다", "此处没有游戏文件", "此處沒有遊戲檔案");
        put("nofiles",
                "%s does not hold the game files. Choose the folder that contains the exefs folder, dvdbnd0.bdt and dvdbnd1.bdt.",
                "%s ne contient pas les fichiers du jeu. Choisissez le dossier qui contient le dossier exefs, dvdbnd0.bdt et dvdbnd1.bdt.",
                "%s enthält keine Spieldateien. Wähle den Ordner mit dem Ordner exefs, dvdbnd0.bdt und dvdbnd1.bdt.",
                "%s non contiene i file di gioco. Scegli la cartella che contiene la cartella exefs, dvdbnd0.bdt e dvdbnd1.bdt.",
                "%s no contiene los archivos del juego. Elige la carpeta que contiene la carpeta exefs, dvdbnd0.bdt y dvdbnd1.bdt.",
                "%s não contém os arquivos do jogo. Escolha a pasta que contém a pasta exefs, dvdbnd0.bdt e dvdbnd1.bdt.",
                "В %s нет файлов игры. Выберите папку, где есть папка exefs, dvdbnd0.bdt и dvdbnd1.bdt.",
                "%s にはゲームファイルがありません。exefs フォルダー、dvdbnd0.bdt、dvdbnd1.bdt が入ったフォルダーを選んでください。",
                "%s에는 게임 파일이 없습니다. exefs 폴더, dvdbnd0.bdt, dvdbnd1.bdt가 있는 폴더를 선택하세요.",
                "%s 中没有游戏文件。请选择包含 exefs 文件夹、dvdbnd0.bdt 和 dvdbnd1.bdt 的文件夹。",
                "%s 中沒有遊戲檔案。請選擇包含 exefs 資料夾、dvdbnd0.bdt 和 dvdbnd1.bdt 的資料夾。");
        put("that_folder", "That folder", "Ce dossier", "Dieser Ordner", "Quella cartella", "Esa carpeta",
                "Essa pasta", "Эта папка", "そのフォルダー", "그 폴더", "该文件夹", "該資料夾");

        // ---- Touch tab
        put("sec_touch", "Touch controls", "Commandes tactiles", "Touch-Steuerung", "Controlli touch",
                "Controles táctiles", "Controles de toque", "Сенсорное управление", "タッチ操作", "터치 조작", "触控操作",
                "觸控操作");
        put("touch_t", "Show touch controls", "Afficher les commandes tactiles", "Touch-Steuerung anzeigen",
                "Mostra controlli touch", "Mostrar controles táctiles", "Mostrar controles de toque",
                "Показывать сенсорное управление", "タッチ操作を表示", "터치 조작 표시", "显示触控操作", "顯示觸控操作");
        put("opacity_t", "Opacity", "Opacité", "Deckkraft", "Opacità", "Opacidad", "Opacidade", "Непрозрачность",
                "不透明度", "불투명도", "不透明度", "不透明度");
        put("camspeed_t", "Camera stick speed", "Vitesse du stick caméra", "Tempo des Kamerasticks",
                "Velocità dello stick della telecamera", "Velocidad del stick de cámara",
                "Velocidade do analógico da câmera", "Скорость стика камеры", "カメラスティックの速さ", "카메라 스틱 속도",
                "视角摇杆速度", "視角搖桿速度");
        put("speed_slow", "Slow", "Lente", "Langsam", "Lenta", "Lenta", "Lenta", "Медленно", "遅い", "느리게", "慢", "慢");
        put("speed_normal", "Normal", "Normale", "Normal", "Normale", "Normal", "Normal", "Обычно", "普通", "보통", "普通",
                "普通");
        put("speed_fast", "Fast", "Rapide", "Schnell", "Veloce", "Rápida", "Rápida", "Быстро", "速い", "빠르게", "快", "快");
        put("speed_faster", "Faster", "Très rapide", "Sehr schnell", "Molto veloce", "Muy rápida", "Muito rápida",
                "Очень быстро", "とても速い", "매우 빠르게", "更快", "更快");
        put("invert_t", "Invert camera", "Inverser la caméra", "Kamera invertieren", "Inverti telecamera",
                "Invertir cámara", "Inverter câmera", "Инвертировать камеру", "カメラを反転", "카메라 반전", "反转视角", "反轉視角");
        put("drift_t", "Prevent right stick centre drift", "Empêcher le recentrage du stick droit",
                "Zurückdriften des rechten Sticks verhindern", "Impedisci il ritorno al centro dello stick destro",
                "Evitar que el stick derecho vuelva al centro", "Impedir que o analógico direito volte ao centro",
                "Не возвращать правый стик в центр", "右スティックの中心への戻りを防ぐ", "오른쪽 스틱 중앙 복귀 방지", "防止右摇杆回中",
                "防止右搖桿回中");
        put("drift_n",
                "On: the camera stick stays where it is while your thumb rests on it, so the camera keeps turning. Off: it drifts back to the centre under a resting thumb and the camera eases to a stop.",
                "Activé : le stick caméra reste en place sous le pouce immobile, la caméra continue de tourner. Désactivé : il revient vers le centre sous le pouce et la caméra ralentit jusqu'à l'arrêt.",
                "An: Der Kamerastick bleibt unter dem ruhenden Daumen stehen, die Kamera dreht weiter. Aus: Er driftet unter dem Daumen zur Mitte zurück und die Kamera kommt sanft zum Stehen.",
                "Sì: lo stick resta dov'è sotto il pollice fermo, quindi la telecamera continua a girare. No: torna verso il centro sotto il pollice e la telecamera si ferma dolcemente.",
                "Sí: el stick de cámara se queda donde está bajo el pulgar quieto, así la cámara sigue girando. No: vuelve al centro bajo el pulgar y la cámara se detiene suavemente.",
                "Ligado: o analógico da câmera fica onde está sob o polegar parado, então a câmera continua girando. Desligado: ele volta ao centro sob o polegar e a câmera para suavemente.",
                "Вкл: стик камеры остаётся на месте под неподвижным пальцем, и камера продолжает поворачиваться. Выкл: он плавно возвращается в центр, и камера останавливается.",
                "オン：親指を止めてもカメラスティックはその位置に留まり、カメラは回り続けます。オフ：止めた親指の下で中心へ戻り、カメラは滑らかに止まります。",
                "켜기: 엄지를 멈춰도 카메라 스틱이 그 자리에 있어 카메라가 계속 회전합니다. 끄기: 멈춘 엄지 아래에서 중앙으로 돌아가 카메라가 부드럽게 멈춥니다.",
                "开：拇指停住时视角摇杆保持原位，视角持续转动。关：拇指停住时摇杆回到中心，视角平滑停止。",
                "開：拇指停住時視角搖桿保持原位，視角持續轉動。關：拇指停住時搖桿回到中心，視角平順停止。");
        put("sec_layout", "Layout", "Disposition", "Anordnung", "Disposizione", "Disposición", "Layout",
                "Расположение", "配置", "배치", "布局", "配置");
        put("layout_n", "Move the buttons and sticks where you like them. The layout is saved.",
                "Placez les boutons et les sticks où vous voulez. La disposition est enregistrée.",
                "Verschiebe Tasten und Sticks dorthin, wo du sie haben willst. Die Anordnung wird gespeichert.",
                "Sposta pulsanti e stick dove preferisci. La disposizione viene salvata.",
                "Mueve los botones y sticks donde prefieras. La disposición se guarda.",
                "Mova os botões e analógicos para onde preferir. O layout é salvo.",
                "Переместите кнопки и стики, куда вам удобно. Расположение сохраняется.",
                "ボタンとスティックを好きな位置に動かせます。配置は保存されます。", "버튼과 스틱을 원하는 위치로 옮길 수 있습니다. 배치는 저장됩니다.",
                "可将按键和摇杆移到你喜欢的位置。布局会被保存。", "可將按鍵和搖桿移到你喜歡的位置。配置會被儲存。");
        put("edit_layout", "Edit layout", "Modifier la disposition", "Anordnung bearbeiten", "Modifica disposizione",
                "Editar disposición", "Editar layout", "Изменить расположение", "配置を編集", "배치 편집", "编辑布局", "編輯配置");
        put("reset_layout", "Reset layout", "Réinitialiser la disposition", "Anordnung zurücksetzen",
                "Ripristina disposizione", "Restablecer disposición", "Redefinir layout", "Сбросить расположение",
                "配置をリセット", "배치 초기화", "重置布局", "重設配置");
        put("layout_reset_done", "Layout reset", "Disposition réinitialisée", "Anordnung zurückgesetzt",
                "Disposizione ripristinata", "Disposición restablecida", "Layout redefinido", "Расположение сброшено",
                "配置をリセットしました", "배치를 초기화했습니다", "布局已重置", "配置已重設");
        put("edit_help", "Drag to move \u00b7 pinch or \u2212 / + to resize", "Glissez pour déplacer \u00b7 pincez ou \u2212 / + pour redimensionner",
                "Ziehen zum Verschieben \u00b7 Zwei Finger oder \u2212 / + für die Größe", "Trascina per spostare \u00b7 pizzica o \u2212 / + per ridimensionare",
                "Arrastra para mover \u00b7 pellizca o \u2212 / + para cambiar el tamaño", "Arraste para mover \u00b7 pinça ou \u2212 / + para redimensionar",
                "Перетаскивайте для перемещения \u00b7 щипок или \u2212 / + для размера", "ドラッグで移動 \u00b7 ピンチか \u2212 / + でサイズ変更",
                "끌어서 이동 \u00b7 핀치 또는 \u2212 / + 로 크기 조절", "拖动以移动 \u00b7 双指缩放或 \u2212 / + 调整大小",
                "拖曳以移動 \u00b7 雙指縮放或 \u2212 / + 調整大小");
        put("size_pick", "Tap a control to resize it", "Touchez une commande pour la redimensionner",
                "Tippe ein Steuerelement an, um seine Größe zu ändern", "Tocca un controllo per ridimensionarlo",
                "Toca un control para cambiar su tamaño", "Toque um controle para redimensioná-lo",
                "Коснитесь элемента, чтобы изменить размер", "サイズを変える操作ボタンをタップ", "크기를 바꿀 버튼을 누르세요",
                "点击控件以调整大小", "點選控制項以調整大小");
        put("size_of", "Size: %d%%", "Taille : %d %%", "Größe: %d %%", "Dimensione: %d%%", "Tamaño: %d %%", "Tamanho: %d%%",
                "Размер: %d%%", "サイズ：%d%%", "크기: %d%%", "大小：%d%%", "大小：%d%%");
        put("sec_presets", "Layout presets", "Dispositions enregistrées", "Layout-Vorlagen", "Preset di layout",
                "Diseños guardados", "Predefinições de layout", "Пресеты раскладки", "レイアウトプリセット", "레이아웃 프리셋",
                "布局预设", "版面預設");
        put("presets_n", "Save the current layout under a name and switch between saved ones.",
                "Enregistrez la disposition actuelle sous un nom et passez d'une disposition enregistrée à l'autre.",
                "Speichere die aktuelle Anordnung unter einem Namen und wechsle zwischen gespeicherten.",
                "Salva il layout attuale con un nome e passa da uno salvato all'altro.",
                "Guarda el diseño actual con un nombre y cambia entre los guardados.",
                "Salve o layout atual com um nome e alterne entre os salvos.",
                "Сохраните текущую раскладку под именем и переключайтесь между сохранёнными.",
                "現在のレイアウトに名前を付けて保存し、保存したものに切り替えられます。",
                "현재 레이아웃을 이름으로 저장하고 저장된 레이아웃으로 전환할 수 있습니다.",
                "以名称保存当前布局，并在已保存的布局之间切换。", "以名稱儲存目前版面，並在已儲存的版面之間切換。");
        put("preset_save", "Save current layout as preset\u2026", "Enregistrer la disposition actuelle\u2026",
                "Aktuelle Anordnung als Vorlage speichern\u2026", "Salva il layout attuale come preset\u2026",
                "Guardar el diseño actual\u2026", "Salvar o layout atual como predefinição\u2026",
                "Сохранить текущую раскладку\u2026", "現在のレイアウトを保存\u2026", "현재 레이아웃을 프리셋으로 저장\u2026",
                "将当前布局保存为预设\u2026", "將目前版面儲存為預設\u2026");
        put("preset_name", "Preset name", "Nom", "Name der Vorlage", "Nome del preset", "Nombre", "Nome da predefinição",
                "Имя пресета", "プリセット名", "프리셋 이름", "预设名称", "預設名稱");
        put("preset_saved", "Saved \"%s\"", "\u00ab %s \u00bb enregistrée", "\u201e%s\u201c gespeichert", "\"%s\" salvato",
                "\"%s\" guardado", "\"%s\" salvo", "\u00ab%s\u00bb сохранён", "\u300c%s\u300dを保存しました", "\"%s\" 저장됨",
                "已保存\u201c%s\u201d", "已儲存\u300c%s\u300d");
        put("preset_loaded", "Layout \"%s\" in use", "Disposition \u00ab %s \u00bb utilisée", "Anordnung \u201e%s\u201c aktiv",
                "Layout \"%s\" in uso", "Diseño \"%s\" en uso", "Layout \"%s\" em uso", "Раскладка \u00ab%s\u00bb применена",
                "\u300c%s\u300dを使用中", "\"%s\" 레이아웃 사용 중", "正在使用\u201c%s\u201d", "正在使用\u300c%s\u300d");
        put("preset_deleted", "Deleted \"%s\"", "\u00ab %s \u00bb supprimée", "\u201e%s\u201c gelöscht", "\"%s\" eliminato",
                "\"%s\" eliminado", "\"%s\" excluído", "\u00ab%s\u00bb удалён", "\u300c%s\u300dを削除しました", "\"%s\" 삭제됨",
                "已删除\u201c%s\u201d", "已刪除\u300c%s\u300d");
        put("preset_use", "Use", "Utiliser", "Verwenden", "Usa", "Usar", "Usar", "Применить", "使用", "사용", "使用", "使用");
        put("preset_delete", "Delete", "Supprimer", "Löschen", "Elimina", "Eliminar", "Excluir", "Удалить", "削除", "삭제",
                "删除", "刪除");
        put("save", "Save", "Enregistrer", "Speichern", "Salva", "Guardar", "Salvar", "Сохранить", "保存", "저장", "保存",
                "儲存");
        put("reset", "Reset", "Réinitialiser", "Zurücksetzen", "Ripristina", "Restablecer", "Redefinir", "Сбросить",
                "リセット", "초기화", "重置", "重設");
        put("cancel", "Cancel", "Annuler", "Abbrechen", "Annulla", "Cancelar", "Cancelar", "Отмена", "キャンセル", "취소",
                "取消", "取消");

        // ---- Language tab
        put("lang_n", "The game's text and this menu. The menu changes at once; the game at its next start.",
                "Les textes du jeu et ce menu. Le menu change immédiatement ; le jeu au prochain lancement.",
                "Die Texte des Spiels und dieses Menüs. Das Menü wechselt sofort, das Spiel beim nächsten Start.",
                "I testi del gioco e di questo menu. Il menu cambia subito, il gioco al prossimo avvio.",
                "Los textos del juego y de este menú. El menú cambia al instante; el juego, en el próximo inicio.",
                "Os textos do jogo e deste menu. O menu muda na hora; o jogo, no próximo início.",
                "Тексты игры и этого меню. Меню меняется сразу, игра — при следующем запуске.",
                "ゲームとこのメニューの言語です。メニューはすぐに、ゲームは次回起動時に切り替わります。",
                "게임과 이 메뉴의 언어입니다. 메뉴는 즉시, 게임은 다음 시작 시 바뀝니다.", "游戏和本菜单的语言。菜单立即切换，游戏在下次启动时切换。",
                "遊戲和本選單的語言。選單立即切換，遊戲在下次啟動時切換。");
        put("lang_changed", "The game's language will change the next time you start the game.",
                "La langue du jeu changera au prochain lancement du jeu.",
                "Die Sprache des Spiels ändert sich beim nächsten Start.",
                "La lingua del gioco cambierà al prossimo avvio.",
                "El idioma del juego cambiará la próxima vez que inicies el juego.",
                "O idioma do jogo mudará na próxima vez que você iniciar o jogo.",
                "Язык игры изменится при следующем запуске.", "ゲームの言語は次回起動時に切り替わります。",
                "게임 언어는 다음에 게임을 시작할 때 바뀝니다.", "游戏语言将在下次启动游戏时切换。", "遊戲語言將在下次啟動遊戲時切換。");

        // ---- First launch
        put("tip_t", "Settings menu", "Menu des paramètres", "Einstellungsmenü", "Menu impostazioni", "Menú de ajustes",
                "Menu de configurações", "Меню настроек", "設定メニュー", "설정 메뉴", "设置菜单", "設定選單");
        put("tip",
                "Hold − and + together for one second to open the settings menu, on a controller or the touch controls.",
                "Maintenez − et + ensemble une seconde pour ouvrir le menu des paramètres, sur une manette ou les commandes tactiles.",
                "Halte − und + zusammen eine Sekunde lang gedrückt, um das Einstellungsmenü zu öffnen – am Controller oder an der Touch-Steuerung.",
                "Tieni premuti − e + insieme per un secondo per aprire il menu impostazioni, sul controller o sui controlli touch.",
                "Mantén − y + a la vez durante un segundo para abrir el menú de ajustes, en un mando o en los controles táctiles.",
                "Segure − e + juntos por um segundo para abrir o menu de configurações, no controle ou nos controles de toque.",
                "Удерживайте − и + одновременно одну секунду, чтобы открыть меню настроек — на геймпаде или сенсорном управлении.",
                "コントローラーまたはタッチ操作で − と + を同時に1秒間長押しすると、設定メニューが開きます。",
                "컨트롤러나 터치 조작에서 − 와 + 를 동시에 1초 동안 누르면 설정 메뉴가 열립니다.",
                "在手柄或触控操作上同时按住 − 和 + 一秒，即可打开设置菜单。", "在手把或觸控操作上同時按住 − 和 + 一秒，即可開啟設定選單。");
    }
}
