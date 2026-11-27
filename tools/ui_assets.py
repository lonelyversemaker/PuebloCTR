#!/usr/bin/env python3
"""The bottom screen's look ("the dark"): its pictures and strings, made once on a computer into
overlay/port/whole/3ds/ui_assets.h (used by 3ds/ui_bottom_3ds.cpp; the file is committed - run this only to change it).

- the background 320x240: a smooth dark gradient with a soft vignette (dithered into RGB565), blood-red hairlines, the
  buttons' slabs, the map's frame - RGB565 in the framebuffer's order (column x from the bottom row up:
  x * 240 + 239 - y): drawn by one memcpy;
- the text: every string of the screen in each language (en fr de es it ja chs; Noto Serif JP, OFL - one face for Latin
  with accents and Japanese; Noto Serif SC for the simplified Chinese texts), anti-aliased, as 8-bit coverage cut to its box
  (origin: the left end of the baseline; the advance for the alignment); the numbers by glyphs of four styles; two strings
  with numbers as templates (pieces and the arguments' places: the order differs by language);
- the heartbeat's curves: coverage, coloured by the life at run time.
The layout's numbers go into the header too (UI_*): the background and the code agree.

Run from the repository's root (Python 3 with tools/requirements-assets.txt; the fonts: tools/fetch_fonts.sh):
  python3 tools/ui_assets.py [--preview PNG]
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # (the repository: tools/..)
FONTS = os.path.join(ROOT, 'build/fonts')  # (tools/fetch_fonts.sh)
OUT = os.path.join(ROOT, 'overlay/port/whole/3ds/ui_assets.h')
PREVIEW = sys.argv[sys.argv.index('--preview') + 1] if '--preview' in sys.argv else None  # (the background as shown)
W, H, SS = 320, 240, 4

SERIF = 'NotoSerifJP-SemiBold.otf'
SERIF_B = 'NotoSerifJP-Black.otf'
SERIF_R = 'NotoSerifJP-Regular.otf'
# (the simplified Chinese texts draw with the Noto Serif SC faces: the JP subsets lack 26 of their characters - 弹 药 设 置
# ...; the three faces as above, for the same weights)
SC = {SERIF: 'NotoSerifSC-SemiBold.otf', SERIF_B: 'NotoSerifSC-Black.otf', SERIF_R: 'NotoSerifSC-Regular.otf'}


def face(lang, fname):
    return SC[fname] if lang == 'chs' else fname

# --- the layout (the code reads these) ----------------------------------------------------------------------------------
L = dict(
    LABEL_X=12, NUM_R=308,             # the left labels, the right numbers' end
    LEON_BASE=19, ASHLEY_BASE=35,      # the life rows' baselines
    PULSE_X=64, PULSE_W=186,           # the heartbeats (from x, wide)
    PULSE_LEON_Y=15, PULSE_ASHLEY_Y=31,  # their middle rows
    LABEL2_BASE=46, NUM_BASE=63,       # AMMO / PESETAS, the numbers under them
    LINE1_Y=68, LINE2_Y=204,           # the hairlines
    MAP_X=8, MAP_Y=73, MAP_W=304, MAP_H=128,  # the minimap's box (its zoom buttons inside, right)
    BTN_Y=208, BTN_H=27, BTN0_X=8, BTN1_X=144, BTN_W=128, BTN_BASE=226,
    GEAR_X=280, GEAR_W=32,             # the settings' gear (the option screen and the settings)
    # the D-pad weapons: four cells in the buttons' strip (the big CASE / MAP buttons gone), a small case button
    WEP_X=8, WEP_W=56, WEP_GAP=2, WEP_NAME_W=50, CASE_X=242, CASE_W=32,
    # the settings before the gameplay: the title, a hairline, three tabs (CHEATS,
    # CONTROLS, SCREEN: a player's wish brought more rows than one page holds), the open tab's rows, a hairline, the
    # footnote
    S_TITLE_BASE=21, S_LINE1_Y=28, S_TAB_Y=31, S_TAB_H=22, S_TAB_BASE=46, S_ROW1_Y=58, S_ROW_H=24,
    S_LINE2_Y=215, S_FOOT_BASE=229, S_BOX=11, S_BOX_X=289, S_ARROW_L=228, S_ARROW_R=300, S_GAIN_X=264,
)

# (the strings below may keep one text more than the languages, Russian: left out of the screen - zip drops it)
LANGS = ['en', 'fr', 'de', 'es', 'it', 'ja', 'chs']  # (chs: simplified Chinese, contributed by DeanCharpen - PuebloCTR pull request 3)

# the strings: (name, font, size, letter spacing, {lang: text}); one text for all when a str
STRINGS = [
    ('LEON', SERIF, 10, 2, ['LEON'] * 5 + ['レオン', '里昂', 'ЛЕОН']),
    ('ASHLEY', SERIF, 10, 2, ['ASHLEY'] * 5 + ['アシュリー', '阿什莉', 'ЭШЛИ']),
    ('AMMO', SERIF_R, 8, 2, ['AMMO', 'MUNITIONS', 'MUNITION', 'MUNICIÓN', 'MUNIZIONI', '弾薬', '弹药', 'ПАТРОНЫ']),
    ('PTAS', SERIF_R, 8, 2, ['PESETAS', 'PESETAS', 'PESETEN', 'PESETAS', 'PESETAS', 'ペセタ', '比塞塔', 'ПЕСЕТЫ']),
    ('CASE', SERIF, 13, 3, ['CASE', 'MALLETTE', 'KOFFER', 'MALETÍN', 'VALIGETTA', 'アタッシュケース', '手提箱', 'КЕЙС']),
    ('MAP', SERIF, 13, 3, ['MAP', 'CARTE', 'KARTE', 'MAPA', 'MAPPA', 'マップ', '地图', 'КАРТА']),
    ('AREA', SERIF_R, 8, 2, ['AREA', 'ZONE', 'GEBIET', 'ZONA', 'ZONA', 'エリア', '区域', 'ОБЛАСТЬ']),
    ('ROOM', SERIF_R, 8, 2, ['ROOM', 'SALLE', 'RAUM', 'SALA', 'STANZA', '部屋', '房间', 'КОМНАТА']),
    ('X2', SERIF_R, 9, 1, '×2'),
    ('X4', SERIF_R, 9, 1, '×4'),
    ('OFFMAP', SERIF_R, 10, 0, ['outside the area map', 'hors de la carte', 'außerhalb der Karte', 'fuera del mapa',
                                'fuori dalla mappa', 'マップの範囲外', '区域地图之外', 'вне карты области']),
    ('NOMAP', SERIF_R, 11, 1, ['no map', 'pas de carte', 'keine Karte', 'sin mapa', 'nessuna mappa', 'マップなし',
                               '无地图', 'карта недоступна']),
    ('BADGE_S', SERIF_B, 9, 0, 'S'),       # (the minimap's typewriters)
    ('BADGE_D', SERIF_B, 8, 0, 'M'),       # (its merchant: M as on the game's map; 8 px: 9 fills the box)
    # the settings before the
    # gameplay (3ds/setup_3ds.cpp, drawn by re4_ui_setup)
    ('S_TITLE', SERIF, 13, 3, ['SETTINGS', 'RÉGLAGES', 'EINSTELLUNGEN', 'AJUSTES', 'IMPOSTAZIONI', '設定', '设置']),
    ('S_VERSION', SERIF_R, 8, 1, 'PuebloCTR β1'),
    ('S_CHEATS', SERIF_R, 8, 2, ['CHEATS', 'TRICHES', 'CHEATS', 'TRUCOS', 'TRUCCHI', 'チート', '作弊']),
    ('S_GAME', SERIF_R, 8, 2, ['GAME', 'JEU', 'SPIEL', 'JUEGO', 'GIOCO', 'ゲーム', '游戏']),
    ('S_HEALTH', SERIF_R, 11, 0, ['Infinite health', 'Santé infinie', 'Unendliche Gesundheit', 'Salud infinita',
                                  'Salute infinita', '体力無限', '无限生命值']),
    ('S_AMMO', SERIF_R, 11, 0, ['Infinite ammo', 'Munitions infinies', 'Unendliche Munition', 'Munición infinita',
                                'Munizioni infinite', '弾薬無限', '无限弹药']),
    ('S_ONEHIT', SERIF_R, 11, 0, ['One-hit kills', 'Ennemis en un coup', 'Gegner mit einem Treffer',
                                  'Enemigos de un golpe', 'Nemici con un colpo', '一撃で倒す', '一击必杀']),
    ('S_QTE', SERIF_R, 11, 0, ['Automatic QTEs', 'QTE automatiques', 'QTEs automatisch', 'QTE automáticos',
                               'QTE automatici', 'QTE自動', '自动QTE']),
    ('S_MONEY', SERIF_R, 11, 0, ['+1,000,000 pesetas', '+1 000 000 pesetas', '+1.000.000 Peseten',
                                 '+1.000.000 pesetas', '+1.000.000 pesetas', '+1,000,000ペセタ', '+1,000,000比塞塔']),
    # own cheats: all weapons at the merchant (a visit only)
    ('S_SHOPWEP', SERIF_R, 11, 0, ['All weapons at the merchant', 'Toutes les armes chez le marchand',
                                   'Alle Waffen beim Händler', 'Todas las armas en el buhonero',
                                   'Tutte le armi dal mercante', '商人が全武器を販売', '商人出售全部武器']),
    # the inventory: its pictures being made (the first start)
    ('I_PREP', SERIF_R, 10, 0, ['Preparing item pictures', 'Préparation des images', 'Bilder werden vorbereitet',
                                'Preparando imágenes', 'Preparazione immagini', 'アイテム画像を準備中', '正在准备物品图片']),
    # the inventory's edits: the band's button, the strip's hint and the discard's question
    ('I_EDIT', SERIF, 10, 1, ['EDIT', 'MODIFIER', 'ÄNDERN', 'EDITAR', 'MODIFICA', '編集', '编辑']),
    ('I_DONE', SERIF, 10, 1, ['DONE', 'OK', 'FERTIG', 'LISTO', 'FATTO', '完了', '完成']),
    ('I_HINT', SERIF_R, 9, 0, ['Tap to select · drag to move or combine', 'Touchez : choisir · glissez : déplacer',
                               'Tippen: wählen · ziehen: bewegen', 'Toca: elegir · arrastra: mover',
                               'Tocca: scegli · trascina: sposta', 'タッチで選択・ドラッグで移動', '点击选择 · 拖动移动']),
    ('I_DISCARD', SERIF_R, 11, 0, ['Discard?', 'Jeter ?', 'Wegwerfen?', '¿Tirar?', 'Gettare?', '捨てる？', '丢弃？']),
    ('I_YES', SERIF, 11, 1, ['YES', 'OUI', 'JA', 'SÍ', 'SÌ', 'はい', '是']),
    ('I_NO', SERIF, 11, 1, ['NO', 'NON', 'NEIN', 'NO', 'NO', 'いいえ', '否']),
    # the giving cheats' buttons: before and after
    ('S_GET', SERIF, 12, 1, ['GET', 'OBTENIR', 'HOLEN', 'OBTENER', 'OTTIENI', '入手', '获取']),
    ('S_DONE', SERIF_R, 11, 0, ['Done', 'Fait', 'Erledigt', 'Hecho', 'Fatto', '済み', '已获得']),
    # (the aim may be on any button: no "(hold R)")
    ('S_GYRO', SERIF_R, 11, 0, ['Gyro aim', 'Visée gyroscopique', 'Zielen mit Gyroskop', 'Apuntar con giroscopio',
                                'Mira con giroscopio', 'ジャイロ照準', '陀螺仪瞄准']),
    ('S_FPS', SERIF_R, 11, 0, ['Frame counter', 'Images par seconde', 'Bilder pro Sekunde', 'Contador de FPS',
                               'Contatore FPS', 'フレームレート表示', '帧率显示']),
    ('S_OFF', SERIF_R, 9, 1, ['OFF', 'NON', 'AUS', 'NO', 'NO', 'オフ', '关']),
    ('S_FOOT', SERIF_R, 9, 0, ['Applied when a game starts or loads', "Appliqué au début ou au chargement d'une partie",
                               'Gilt ab Spielbeginn oder Laden eines Spielstands',
                               'Se aplica al empezar o cargar una partida',
                               "Si applica all'avvio o al caricamento di una partita", 'ゲーム開始・ロード時に反映',
                               '开始游戏或加载存档时生效']),
    # the gyro's gains (the decimal mark of the language)
    ('S_G10', SERIF, 11, 0, '1×'),
    ('S_G15', SERIF, 11, 0, ['1.5×', '1,5×', '1,5×', '1,5×', '1,5×', '1.5×', '1.5×']),
    ('S_G20', SERIF, 11, 0, '2×'),
    ('S_G25', SERIF, 11, 0, ['2.5×', '2,5×', '2,5×', '2,5×', '2,5×', '2.5×', '2.5×']),
    ('S_G30', SERIF, 11, 0, '3×'),
    ('S_G40', SERIF, 11, 0, '4×'),
    # the circle pad's sensitivity (3ds/input_3ds.cpp re4_3ds_stick_set): its row and the gain between 1× and 1.5×
    ('S_STICK', SERIF_R, 11, 0, ['Circle Pad sensitivity', 'Sensibilité du pad circulaire', 'Schiebepad-Empfindlichkeit',
                                 'Sensibilidad del botón deslizante', 'Sensibilità del pad scorrevole', 'スライドパッド感度',
                                 '滑杆灵敏度']),
    ('S_G12', SERIF, 11, 0, ['1.25×', '1,25×', '1,25×', '1,25×', '1,25×', '1.25×', '1.25×']),
    # the picture on the top screen (3ds/gx_3ds.cpp re4_3ds_gx_fill; a player's wish: no bars): the game's
    # 16:9 band fitted (as before), stretched to the screen's height, or zoomed to it (the sides cut)
    ('S_PICTURE', SERIF_R, 11, 0, ['Picture', 'Image', 'Bild', 'Imagen', 'Immagine', '画面', '画面']),
    ('S_FIT', SERIF, 10, 0, ['Fit', 'Ajustée', 'Passend', 'Ajustada', 'Adattata', '標準', '适应']),
    ('S_STRETCH', SERIF, 10, 0, ['Stretch', 'Étirée', 'Gestreckt', 'Estirada', 'Allungata', '引き伸ばし', '拉伸']),
    ('S_ZOOM', SERIF, 10, 0, ['Zoom', 'Zoom', 'Zoom', 'Zoom', 'Zoom', 'ズーム', '放大']),
    # the settings screen's tabs
    ('S_TAB_CHEATS', SERIF_R, 10, 2, ['CHEATS', 'TRICHES', 'CHEATS', 'TRUCOS', 'TRUCCHI', 'チート', '作弊']),
    ('S_TAB_CONTROLS', SERIF_R, 10, 2, ['CONTROLS', 'COMMANDES', 'STEUERUNG', 'CONTROLES', 'COMANDI', '操作', '操作']),
    ('S_TAB_SCREEN', SERIF_R, 10, 2, ['SCREEN', 'ÉCRAN', 'BILDSCHIRM', 'PANTALLA', 'SCHERMO', '画面', '画面']),
    # the button mapping (3ds/controls_3ds.cpp): its row on the CONTROLS tab, its page
    # (the game's actions, their buttons), the buttons' names (the same in every language)
    ('S_BUTTONS', SERIF_R, 11, 0, ['Button mapping', 'Attribution des boutons', 'Tastenbelegung',
                                   'Asignación de botones', 'Mappatura dei pulsanti', 'ボタン配置', '按键分配']),
    # the Circle Pad's dead zone (a worn pad's drift ignored): its row; the value a percent (the digits) or OFF
    ('S_DEADZONE', SERIF_R, 11, 0, ['Circle Pad dead zone', 'Zone morte du pad circulaire', 'Schiebepad-Totzone',
                                    'Zona muerta del botón deslizante', 'Zona morta del pad scorrevole',
                                    'スライドパッドの遊び', '滑杆死区']),
    # the SCREEN tab: the game's life / ammo HUD off the top screen (the bottom screen
    # shows them); the bottom screen's own language: Auto (the game's) or one of seven, each named in itself
    # the settings opened from the game's option screen: the changes apply at once
    ('S_FOOT_LIVE', SERIF_R, 9, 0, ['Applied at once', 'Appliqué immédiatement', 'Gilt sofort',
                                    'Se aplica al instante', 'Si applica subito', 'すぐに反映', '立即生效']),
    ('S_TOPHUD', SERIF_R, 11, 0, ['Hide HUD on top screen', "Masquer l'ATH en haut", 'HUD oben ausblenden',
                                  'Ocultar HUD superior', "Nascondi l'HUD in alto", '上画面のHUDを隠す', '隐藏上屏HUD']),
    ('S_UILANG', SERIF_R, 11, 0, ['Bottom screen language', "Langue de l'écran du bas", 'Sprache unten',
                                  'Idioma de abajo', 'Lingua schermo inferiore', '下画面の言語', '下屏语言']),
    ('S_L_AUTO', SERIF, 10, 0, ['Auto', 'Auto', 'Auto', 'Auto', 'Auto', '自動', '自动']),
    ('S_L_EN', SERIF, 10, 0, 'English'),
    ('S_L_FR', SERIF, 10, 0, 'Français'),
    ('S_L_DE', SERIF, 10, 0, 'Deutsch'),
    ('S_L_ES', SERIF, 10, 0, 'Español'),
    ('S_L_IT', SERIF, 10, 0, 'Italiano'),
    ('S_L_JA', SERIF, 10, 0, '日本語'),
    ('S_L_CHS', SERIF, 10, 0, '中文'),
    ('S_BTN_TITLE', SERIF, 13, 3, ['BUTTONS', 'BOUTONS', 'TASTEN', 'BOTONES', 'PULSANTI', 'ボタン配置', '按键']),
    ('A_UP', SERIF_R, 11, 0, ['Forward / aim up', 'Avancer / viser en haut', 'Vorwärts / nach oben zielen',
                              'Avanzar / apuntar arriba', 'Avanti / mira in alto', '前進・上を狙う', '前进 / 向上瞄准']),
    ('A_DOWN', SERIF_R, 11, 0, ['Back / aim down', 'Reculer / viser en bas', 'Rückwärts / nach unten zielen',
                                'Retroceder / apuntar abajo', 'Indietro / mira in basso', '後退・下を狙う', '后退 / 向下瞄准']),
    ('A_LEFT', SERIF_R, 11, 0, ['Turn / aim left', 'Tourner / viser à gauche', 'Links drehen / zielen',
                                'Girar / apuntar a la izq.', 'Gira / mira a sinistra', '左を向く・狙う', '左转 / 向左瞄准']),
    ('A_RIGHT', SERIF_R, 11, 0, ['Turn / aim right', 'Tourner / viser à droite', 'Rechts drehen / zielen',
                                 'Girar / apuntar a la der.', 'Gira / mira a destra', '右を向く・狙う', '右转 / 向右瞄准']),
    ('A_LUP', SERIF_R, 11, 0, ['Look up / zoom in', 'Regarder en haut / zoom +', 'Nach oben sehen / Zoom +',
                               'Mirar arriba / zoom +', 'Guarda su / zoom +', '上を見る・ズーム＋', '向上看 / 放大']),
    ('A_LDOWN', SERIF_R, 11, 0, ['Look down / zoom out', 'Regarder en bas / zoom −', 'Nach unten sehen / Zoom −',
                                 'Mirar abajo / zoom −', 'Guarda giù / zoom −', '下を見る・ズーム－', '向下看 / 缩小']),
    ('A_LLEFT', SERIF_R, 11, 0, ['Look left', 'Regarder à gauche', 'Nach links sehen', 'Mirar a la izquierda',
                                 'Guarda a sinistra', '左を見る', '向左看']),
    ('A_LRIGHT', SERIF_R, 11, 0, ['Look right', 'Regarder à droite', 'Nach rechts sehen', 'Mirar a la derecha',
                                  'Guarda a destra', '右を見る', '向右看']),
    ('A_ACTION', SERIF_R, 11, 0, ['Action', 'Action', 'Aktion', 'Acción', 'Azione', 'アクション', '互动']),
    ('A_RUN', SERIF_R, 11, 0, ['Run', 'Courir', 'Rennen', 'Correr', 'Correre', '走る', '奔跑']),
    ('A_TURN', SERIF_R, 11, 0, ['Quick turn', 'Demi-tour rapide', 'Schnelle Drehung', 'Giro rápido', 'Giro rapido',
                                'クイックターン', '快速转向']),
    ('A_AIM', SERIF_R, 11, 0, ['Aim', 'Viser', 'Zielen', 'Apuntar', 'Mirare', '構える', '瞄准']),
    ('A_FIRE', SERIF_R, 11, 0, ['Fire', 'Tirer', 'Schießen', 'Disparar', 'Sparare', '撃つ', '射击']),
    ('A_RELOAD', SERIF_R, 11, 0, ['Reload', 'Recharger', 'Nachladen', 'Recargar', 'Ricaricare', 'リロード', '装填']),
    ('A_KNIFE', SERIF_R, 11, 0, ['Knife', 'Couteau', 'Messer', 'Cuchillo', 'Coltello', 'ナイフ', '小刀']),
    ('A_SLASH', SERIF_R, 11, 0, ['Knife attack', 'Coup de couteau', 'Messerangriff', 'Cuchillada',
                                 'Colpo di coltello', 'ナイフ攻撃', '小刀攻击']),
    ('A_ASHLEY', SERIF_R, 11, 0, ['Ashley', 'Ashley', 'Ashley', 'Ashley', 'Ashley', 'アシュリー', '阿什莉']),
    ('A_CASE', SERIF_R, 11, 0, ['Attaché case', 'Mallette', 'Koffer', 'Maletín', 'Valigetta', 'アタッシュケース', '手提箱']),
    ('A_MAP', SERIF_R, 11, 0, ['Map', 'Carte', 'Karte', 'Mapa', 'Mappa', 'マップ', '地图']),
    ('A_OPTIONS', SERIF_R, 11, 0, ['Options', 'Options', 'Optionen', 'Opciones', 'Opzioni', 'オプション', '选项']),
    # the D-pad weapons: the button mapping's actions, the list's title and its empty choice, the names
    ('A_WUP', SERIF_R, 11, 0, ['Weapon ↑', 'Arme ↑', 'Waffe ↑', 'Arma ↑', 'Arma ↑', '武器 ↑', '武器 ↑']),
    ('A_WDOWN', SERIF_R, 11, 0, ['Weapon ↓', 'Arme ↓', 'Waffe ↓', 'Arma ↓', 'Arma ↓', '武器 ↓', '武器 ↓']),
    ('A_WLEFT', SERIF_R, 11, 0, ['Weapon ←', 'Arme ←', 'Waffe ←', 'Arma ←', 'Arma ←', '武器 ←', '武器 ←']),
    ('A_WRIGHT', SERIF_R, 11, 0, ['Weapon →', 'Arme →', 'Waffe →', 'Arma →', 'Arma →', '武器 →', '武器 →']),
    ('W_PICK', SERIF, 13, 3, ['WEAPON', 'ARME', 'WAFFE', 'ARMA', 'ARMA', '武器', '武器']),
    ('W_HINT', SERIF_R, 9, 0, ['Tap a weapon for this arrow of the D-pad', 'Touchez une arme pour cette flèche',
                               'Waffe für diese Richtung antippen', 'Toca un arma para esta flecha',
                               'Tocca un’arma per questa freccia', 'この方向キーに割り当てる武器をタッチ',
                               '点选分配给此方向键的武器']),
    ('W_NONE', SERIF_R, 11, 0, ['None', 'Aucune', 'Keine', 'Ninguna', 'Nessuna', 'なし', '无']),
    ('W_EMPTY', SERIF_R, 9, 0, '—'),
    ('S_RESUME', SERIF, 12, 1, ['RESUME', 'REPRENDRE', 'WEITER', 'CONTINUAR', 'RIPRENDI', 'ゲームに戻る', '返回游戏']),
    ('WN_HANDGUN', SERIF_R, 9, 0, ['Handgun', 'Pistolet', 'Pistole', 'Pistola', 'Pistola', 'ハンドガン', '手枪']),
    ('WN_RED9', SERIF_R, 9, 0, ['Red9', 'Red9', 'Red9', 'Red9', 'Red9', 'レッド9', '红9']),
    ('WN_PUNISHER', SERIF_R, 9, 0, ['Punisher', 'Punisher', 'Punisher', 'Punisher', 'Punisher', 'パニッシャー', '惩罚者']),
    ('WN_BLACKTAIL', SERIF_R, 9, 0, ['Blacktail', 'Blacktail', 'Blacktail', 'Blacktail', 'Blacktail', 'ブラックテイル', '黑尾']),
    ('WN_MATILDA', SERIF_R, 9, 0, ['Matilda', 'Matilda', 'Matilda', 'Matilda', 'Matilda', 'マチルダ', '玛蒂尔达']),
    ('WN_BUTTERFLY', SERIF_R, 9, 0, ['Butterfly', 'Butterfly', 'Butterfly', 'Butterfly', 'Butterfly', 'バタフライ', '断蝶']),
    ('WN_KILLER7', SERIF_R, 9, 0, ['Killer7', 'Killer7', 'Killer7', 'Killer7', 'Killer7', 'キラー7', '杀手7']),
    ('WN_SHOTGUN', SERIF_R, 9, 0, ['Shotgun', 'Fusil à pompe', 'Schrotflinte', 'Escopeta', 'Fucile a pompa', 'ショットガン', '霰弹枪']),
    ('WN_STRIKER', SERIF_R, 9, 0, ['Striker', 'Striker', 'Striker', 'Striker', 'Striker', 'ストライカー', '打击者']),
    ('WN_RIOT', SERIF_R, 9, 0, ['Riot gun', 'Riot gun', 'Riot Gun', 'Riot gun', 'Riot gun', 'ライオットガン', '防暴枪']),
    ('WN_RIFLE', SERIF_R, 9, 0, ['Rifle', 'Fusil', 'Gewehr', 'Rifle', 'Fucile', 'ライフル', '步枪']),
    ('WN_SEMI', SERIF_R, 9, 0, ['Semi-auto', 'Semi-auto', 'Halbautom.', 'Semiautom.', 'Semiautom.', 'セミオート', '半自动']),
    ('WN_TMP', SERIF_R, 9, 0, ['TMP', 'TMP', 'TMP', 'TMP', 'TMP', 'TMP', 'TMP']),
    ('WN_CHICAGO', SERIF_R, 9, 0, ['Chicago', 'Chicago', 'Chicago', 'Chicago', 'Chicago', 'シカゴ', '芝加哥']),
    ('WN_ROCKET', SERIF_R, 9, 0, ['Launcher', 'Lance-roquettes', 'Raketenwerfer', 'Lanzacohetes', 'Lanciarazzi', 'ロケラン', '火箭筒']),
    ('WN_ROCKETINF', SERIF_R, 9, 0, ['Launcher ∞', 'Lance-r. ∞', 'Raketenw. ∞', 'Lanzac. ∞', 'Lanciar. ∞', 'ロケラン∞', '火箭筒∞']),
    ('WN_MINE', SERIF_R, 9, 0, ['Mine thrower', 'Lance-mines', 'Minenwerfer', 'Lanzaminas', 'Lanciamine', 'マインスロアー', '地雷枪']),
    ('WN_HANDCANNON', SERIF_R, 9, 0, ['Handcannon', 'Handcannon', 'Handcannon', 'Handcannon', 'Handcannon', 'ハンドキャノン', '手炮']),
    ('WN_GRENADE', SERIF_R, 9, 0, ['Grenade', 'Grenade', 'Granate', 'Granada', 'Granata', '手榴弾', '手榴弹']),
    ('WN_INCEND', SERIF_R, 9, 0, ['Incendiary', 'Incendiaire', 'Brandgranate', 'Incendiaria', 'Incendiaria', '焼夷手榴弾', '燃烧弹']),
    ('WN_FLASH', SERIF_R, 9, 0, ['Flash', 'Flash', 'Blendgranate', 'Cegadora', 'Accecante', '閃光手榴弾', '闪光弹']),
    ('WN_OTHER', SERIF_R, 9, 0, ['Weapon', 'Arme', 'Waffe', 'Arma', 'Arma', '武器', '武器']),
    ('S_RESET', SERIF_R, 11, 0, ['Reset to defaults', 'Rétablir par défaut', 'Standard wiederherstellen',
                                 'Restablecer', 'Ripristina predefiniti', '初期設定に戻す', '恢复默认设置']),
    ('S_PRESS', SERIF, 10, 0, ['Press a button…', 'Appuyez sur un bouton…', 'Taste drücken…', 'Pulsa un botón…',
                               'Premi un pulsante…', 'ボタンを押す…', '按下按键…']),
    ('S_BTN_FOOT', SERIF_R, 9, 0, ['Tap, then press one button or two · SELECT: none',
                                   'Touchez, puis 1 ou 2 boutons · SELECT : aucun',
                                   'Antippen, dann 1 oder 2 Tasten · SELECT: keine',
                                   'Toca y pulsa 1 o 2 botones · SELECT: ninguno',
                                   'Tocca, poi 1 o 2 pulsanti · SELECT: nessuno',
                                   'タッチ→ボタンを1つか2つ・SELECTで解除',
                                   '点按后按1个或2个键 · SELECT: 无']),
    ('K_A', SERIF, 11, 0, 'A'), ('K_B', SERIF, 11, 0, 'B'), ('K_X', SERIF, 11, 0, 'X'), ('K_Y', SERIF, 11, 0, 'Y'),
    ('K_L', SERIF, 11, 0, 'L'), ('K_R', SERIF, 11, 0, 'R'), ('K_ZL', SERIF, 11, 0, 'ZL'), ('K_ZR', SERIF, 11, 0, 'ZR'),
    ('K_UP', SERIF, 11, 0, '↑'), ('K_DOWN', SERIF, 11, 0, '↓'), ('K_LEFT', SERIF, 11, 0, '←'),
    ('K_RIGHT', SERIF, 11, 0, '→'), ('K_START', SERIF, 10, 1, 'START'), ('K_PLUS', SERIF_R, 11, 0, ' + '),
    ('K_NONE', SERIF_R, 11, 0, '—'),
    # the sticks' directions by the names the 3DS's manuals give them (a player tells the two apart)
    ('K_PUP', SERIF, 10, 0, ['Circle Pad ↑', 'Pad circ. ↑', 'Schiebepad ↑', 'Desliz. ↑', 'Pad scorr. ↑', 'スライドパッド↑', '滑杆↑']),
    ('K_PDOWN', SERIF, 10, 0, ['Circle Pad ↓', 'Pad circ. ↓', 'Schiebepad ↓', 'Desliz. ↓', 'Pad scorr. ↓', 'スライドパッド↓', '滑杆↓']),
    ('K_PLEFT', SERIF, 10, 0, ['Circle Pad ←', 'Pad circ. ←', 'Schiebepad ←', 'Desliz. ←', 'Pad scorr. ←', 'スライドパッド←', '滑杆←']),
    ('K_PRIGHT', SERIF, 10, 0, ['Circle Pad →', 'Pad circ. →', 'Schiebepad →', 'Desliz. →', 'Pad scorr. →', 'スライドパッド→', '滑杆→']),
    ('K_CUP', SERIF, 10, 0, ['C-Stick ↑', 'Stick C ↑', 'C-Stick ↑', 'Palanca C ↑', 'Stick C ↑', 'Cスティック↑', 'C摇杆↑']),
    ('K_CDOWN', SERIF, 10, 0, ['C-Stick ↓', 'Stick C ↓', 'C-Stick ↓', 'Palanca C ↓', 'Stick C ↓', 'Cスティック↓', 'C摇杆↓']),
    ('K_CLEFT', SERIF, 10, 0, ['C-Stick ←', 'Stick C ←', 'C-Stick ←', 'Palanca C ←', 'Stick C ←', 'Cスティック←', 'C摇杆←']),
    ('K_CRIGHT', SERIF, 10, 0, ['C-Stick →', 'Stick C →', 'C-Stick →', 'Palanca C →', 'Stick C →', 'Cスティック→', 'C摇杆→']),
    # (English only - the game's language is not known yet) the checks
    # at the start (3ds/startcheck_3ds.cpp, drawn by re4_ui_check). C_T_* the titles, C_F_* the footers, C_H_* the
    # headings of a problem, the rest its lines
    ('C_T_STOP', SERIF, 13, 3, 'CANNOT START'),
    ('C_T_WARN', SERIF, 13, 3, 'BEFORE YOU PLAY'),
    ('C_F_STOP', SERIF_R, 10, 0, 'START  exit'),
    ('C_F_WARN', SERIF_R, 10, 0, 'A  continue          START  exit'),
    ('C_H_OLD', SERIF, 12, 0, 'A New Nintendo 3DS is needed'),
    ('C_OLD1', SERIF_R, 10, 0, 'New Nintendo 3DS, New 3DS XL or New 2DS XL.'),
    ('C_OLD2', SERIF_R, 10, 0, 'The older 3DS and 2DS are too slow for this game.'),
    ('C_H_MEM', SERIF, 12, 0, 'Not enough memory'),
    ('C_MEM1', SERIF_R, 10, 0, 'The system gave PuebloCTR too little memory.'),
    ('C_MEM2', SERIF_R, 10, 0, 'Start the Homebrew Launcher through a game'),
    ('C_MEM3', SERIF_R, 10, 0, '(title takeover), not as an applet.'),
    ('C_H_NODATA', SERIF, 12, 0, 'Game data not found'),
    ('C_NODATA1', SERIF_R, 10, 0, 'The files of disc 1 are expected in'),
    ('C_NODATA2', SERIF_R, 10, 0, 'sdmc:/3ds/pueblo/data/disc0'),
    ('C_PREP', SERIF_R, 10, 0, 'Prepare them from your own discs with PuebloPrep.'),
    ('C_H_BAD', SERIF, 12, 0, 'Game data incomplete'),
    ('C_BAD1', SERIF_R, 10, 0, 'data/disc0 has no sys/boot.bin or sys/fst.bin.'),
    ('C_H_ED', SERIF, 12, 0, 'This disc is not supported'),
    ('C_ED_JP', SERIF_R, 10, 0, 'Found: the Japanese edition (G4BJ08).'),
    ('C_ED_DBG', SERIF_R, 10, 0, 'Found: a debug build of the game.'),
    ('C_ED_UNK', SERIF_R, 10, 0, 'Found: an unknown disc.'),
    ('C_ED_OK', SERIF_R, 10, 0, 'Supported: Europe, USA and Japan.'),
    ('C_H_SWAP', SERIF, 12, 0, 'The discs are swapped'),
    ('C_SWAP1', SERIF_R, 10, 0, 'data/disc0 holds disc 2.'),
    ('C_DISCS', SERIF_R, 10, 0, 'Disc 1 goes in data/disc0, disc 2 in data/disc1.'),
    ('C_H_MIX', SERIF, 12, 0, 'The discs do not match'),
    ('C_MIX1', SERIF_R, 10, 0, 'data/disc1 is not disc 2 of the same edition.'),
    ('C_H_NO2', SERIF, 12, 0, 'Disc 2 not found (data/disc1)'),
    ('C_NO2_1', SERIF_R, 10, 0, 'You can play until the game asks for disc 2.'),
    ('C_H_NOMOV', SERIF, 12, 0, 'Movies not found (movie/)'),
    ('C_H_SOMEMOV', SERIF, 12, 0, 'Some movies are missing (movie/)'),
    ('C_MOV1', SERIF_R, 10, 0, 'The full-motion movies will not show properly.'),
    ('C_H_NOPAK', SERIF, 12, 0, 'Texture pack not found (textures.pak)'),
    ('C_H_OLDPAK', SERIF, 12, 0, 'The texture pack does not fit this version'),
    ('C_PAK1', SERIF_R, 10, 0, 'The game stutters when new textures appear.'),
]
# templates: {0} {1} the arguments (in the style DS_SMALL); the pieces in the same face
TEMPLATES = [
    ('FLOOR', ['floor {0} of {1}', 'étage {0} sur {1}', 'Etage {0} von {1}', 'planta {0} de {1}', 'piano {0} di {1}',
               '{0}階 / {1}', '{0}层 / {1}', 'этаж {0} из {1}']),
    ('MEDALS', ['medallions {0}/{1}', 'médaillons {0}/{1}', 'Medaillons {0}/{1}', 'medallones {0}/{1}',
                'medaglioni {0}/{1}', '青いメダル {0}/{1}', '蓝色徽章 {0}/{1}', 'медальоны {0}/{1}']),
]
TPL_FONT, TPL_SIZE = SERIF_R, 10
DIGITS = '0123456789/ -—×%'
DSTYLES = [('BIG', SERIF_B, 16),  # (the ammo; 20 nearly touched the AMMO label)
           ('MID', SERIF_R, 12), ('PTAS', SERIF, 16), ('SMALL', SERIF_R, 10)]


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


# --- the pieces of coverage ---------------------------------------------------------------------------------------------
pieces = []  # (w, h, ox, oy, adv, bytes)


def add_piece(text, fname, size, spacing):
    f = font(fname, size)
    pad = size * 2
    widths = [f.getlength(c) for c in text]
    adv = sum(widths) + spacing * max(0, len(text) - 1)
    img = Image.new('L', (int(adv) + 2 * pad + 4, size * 4), 0)
    d = ImageDraw.Draw(img)
    x = pad
    base = size * 2
    if spacing:
        for c, wdt in zip(text, widths):
            d.text((x, base), c, font=f, fill=255, anchor='ls')
            x += wdt + spacing
    else:
        d.text((x, base), text, font=f, fill=255, anchor='ls')
    bb = img.getbbox()
    if not bb:  # (a space)
        pieces.append((0, 0, 0, 0, int(round(adv)), b''))
        return len(pieces) - 1
    crop = img.crop(bb)
    pieces.append((crop.width, crop.height, bb[0] - pad, bb[1] - base, int(round(adv)), crop.tobytes()))
    return len(pieces) - 1


def lang_size(lang, size):
    return max(size, 10) if lang in ('ja', 'chs') else size  # (kana and kanji below 10 px do not read)


def lang_spacing(lang, spacing):
    return min(spacing, 1) if lang in ('ja', 'chs') else spacing


str_idx = {}  # name -> [piece per language]
for name, fname, size, spacing, texts in STRINGS:
    if isinstance(texts, str):
        p = add_piece(texts, fname, size, spacing)
        str_idx[name] = [p] * len(LANGS)
    else:
        str_idx[name] = [add_piece(t, face(l, fname), lang_size(l, size), lang_spacing(l, spacing))
                         for t, l in zip(texts, LANGS)]
    if name.startswith('C_'):  # (R3) the start's lines from x 16 to 304 at most
        assert pieces[str_idx[name][0]][4] <= 288, (name, pieces[str_idx[name][0]][4])

# the weapon cells' names: narrowed to the cell's room when longer (WN_*)
for name in [n for n in str_idx if n.startswith('WN_')]:
    for p in sorted(set(str_idx[name])):
        w, h, ox, oy, adv, data = pieces[p]
        if adv > L['WEP_NAME_W'] and w:
            q = L['WEP_NAME_W'] / adv
            img = Image.frombytes('L', (w, h), data).resize((max(1, round(w * q)), h), Image.LANCZOS)
            pieces[p] = (img.width, h, round(ox * q), oy, L['WEP_NAME_W'], img.tobytes())

tpl_tokens = {}  # name -> [tokens per language]; a token >= 0 a piece, -1 / -2 the argument 0 / 1
for name, texts in TEMPLATES:
    per = []
    for t, l in zip(texts, LANGS):
        toks = []
        rest = t
        while rest:
            i0, i1 = rest.find('{0}'), rest.find('{1}')
            cand = [i for i in (i0, i1) if i >= 0]
            i = min(cand) if cand else -1
            if i < 0:
                toks.append(add_piece(rest, face(l, TPL_FONT), lang_size(l, TPL_SIZE), 0))
                break
            if i > 0:
                toks.append(add_piece(rest[:i], face(l, TPL_FONT), lang_size(l, TPL_SIZE), 0))
            toks.append(-1 if rest[i + 1] == '0' else -2)
            rest = rest[i + 3:]
        assert len(toks) <= 8, t
        per.append(toks)
    tpl_tokens[name] = per

digit_idx = []
for dname, fname, size in DSTYLES:
    digit_idx.append([add_piece(c, fname, size, 0) for c in DIGITS])


# --- the heartbeats -----------------------------------------------------------------------------------------------------
def pulse(w, h, amp, period):
    img = Image.new('L', (w * SS, h * SS), 0)
    d = ImageDraw.Draw(img)
    mid = h / 2
    pts = []
    for xx in range(0, w * 4 + 1):
        x = xx / 4
        ph = (x + period * 0.3) % period
        y = mid
        # the P wave, the QRS, the T wave
        if 8 <= ph < 13:
            y = mid - amp * 0.18 * np.sin((ph - 8) / 5 * np.pi)
        elif 16 <= ph < 17.5:
            y = mid + amp * 0.25 * (ph - 16) / 1.5
        elif 17.5 <= ph < 19.5:
            y = mid + amp * 0.25 - amp * 1.25 * (ph - 17.5) / 2
        elif 19.5 <= ph < 22:
            y = mid - amp + amp * 1.6 * (ph - 19.5) / 2.5
        elif 22 <= ph < 23.5:
            y = mid + amp * 0.6 - amp * 0.6 * (ph - 22) / 1.5
        elif 27 <= ph < 35:
            y = mid - amp * 0.3 * np.sin((ph - 27) / 8 * np.pi)
        pts.append((x * SS, y * SS))
    d.line(pts, fill=255, width=int(1.25 * SS), joint='curve')
    img = img.resize((w, h), Image.LANCZOS)
    pieces.append((w, h, 0, -h // 2, w, img.tobytes()))
    return len(pieces) - 1


pulse_leon = pulse(L['PULSE_W'], 21, 9.5, 46)
pulse_ashley = pulse(L['PULSE_W'], 15, 6.0, 40)


# --- R2: the settings' shapes (coverage; the box's top left / the arrow's middle at (x, y) of the blit) ----------------
def shape(w, h, ox, oy, draw):
    img = Image.new('L', (w * SS, h * SS), 0)
    draw(ImageDraw.Draw(img), SS)
    img = img.resize((w, h), Image.LANCZOS)
    pieces.append((w, h, ox, oy, w, img.tobytes()))
    return len(pieces) - 1


# the mark inside the check box (S_BOX square, its top left at the piece's origin)
shape_check = shape(L['S_BOX'], L['S_BOX'], 0, 0, lambda d, k: d.line(
    (2.5 * k, 5.5 * k, 4.7 * k, 8 * k, 8.6 * k, 2.8 * k), fill=255, width=int(1.6 * k), joint='curve'))
# the gyro's arrows: 6 x 8 around the middle (x, y)
shape_arrow_l = shape(8, 10, -4, -5, lambda d, k: d.polygon([(7 * k, 1 * k), (7 * k, 9 * k), (1 * k, 5 * k)], fill=255))
shape_arrow_r = shape(8, 10, -4, -5, lambda d, k: d.polygon([(1 * k, 1 * k), (1 * k, 9 * k), (7 * k, 5 * k)], fill=255))


# the settings' gear: 16 x 16 around the middle (x, y) - eight teeth, a ring, a hole
def draw_gear(d, k):
    import math
    c, r_out, r_in, r_hole = 8 * k, 7.6 * k, 5.6 * k, 2.4 * k
    pts = []
    for i in range(8):
        a = i * math.pi / 4
        for da, r in ((-0.24, r_in), (-0.17, r_out), (0.17, r_out), (0.24, r_in)):
            pts.append((c + r * math.cos(a + da), c + r * math.sin(a + da)))
    d.polygon(pts, fill=255)
    d.ellipse((c - r_in, c - r_in, c + r_in, c + r_in), fill=255)
    d.ellipse((c - r_hole, c - r_hole, c + r_hole, c + r_hole), fill=0)


shape_gear = shape(16, 16, -8, -8, draw_gear)


# the case button's icon: a briefcase 18 x 14 around the middle (x, y)
def draw_case(d, k):
    d.rounded_rectangle((1 * k, 3.5 * k, 17 * k, 13 * k), radius=1.5 * k, outline=255, width=int(1.6 * k))
    d.rectangle((6.5 * k, 1 * k, 11.5 * k, 3.5 * k), outline=255, width=int(1.3 * k))
    d.line((1 * k, 7.5 * k, 17 * k, 7.5 * k), fill=255, width=int(1.2 * k))
    d.rectangle((8 * k, 6.3 * k, 10 * k, 8.8 * k), fill=255)


shape_case = shape(18, 14, -9, -7, draw_case)
# the inventory's edits: ROTATE - an arrow round clockwise, 16 x 16 around the middle (x, y)
def draw_rotate(d, k):
    import math
    c, r, a0, a1 = 8 * k, 5.6 * k, -70, 200
    d.arc((c - r, c - r, c + r, c + r), a0, a1, fill=255, width=int(1.8 * k))
    t = math.radians(a1)
    ex, ey = c + r * math.cos(t), c + r * math.sin(t)
    tx, ty = -math.sin(t), math.cos(t)  # the clockwise tangent (y down)
    nx, ny = math.cos(t), math.sin(t)
    s = 3.2 * k
    d.polygon([(ex + tx * s * 1.2, ey + ty * s * 1.2), (ex + nx * s, ey + ny * s), (ex - nx * s, ey - ny * s)],
              fill=255)


shape_rotate = shape(16, 16, -8, -8, draw_rotate)


# DISCARD - a bin 14 x 16 around the middle (x, y)
def draw_trash(d, k):
    d.line((1 * k, 3.5 * k, 13 * k, 3.5 * k), fill=255, width=int(1.6 * k))
    d.rectangle((5 * k, 1 * k, 9 * k, 3.5 * k), outline=255, width=int(1.3 * k))
    d.polygon([(2.5 * k, 5 * k), (11.5 * k, 5 * k), (10.5 * k, 15 * k), (3.5 * k, 15 * k)], outline=255,
              width=int(1.5 * k))
    for x in (5.5, 8.5):
        d.line((x * k, 7 * k, x * k, 13 * k), fill=255, width=int(1.2 * k))


shape_trash = shape(14, 16, -7, -8, draw_trash)
# the weapon cells' arrows: 7 x 7 triangles, their middle at (x, y)
shape_tri_u = shape(7, 7, -3, -3, lambda d, k: d.polygon([(3.5 * k, 0.5 * k), (6.5 * k, 6 * k), (0.5 * k, 6 * k)], fill=255))
shape_tri_d = shape(7, 7, -3, -3, lambda d, k: d.polygon([(0.5 * k, 1 * k), (6.5 * k, 1 * k), (3.5 * k, 6.5 * k)], fill=255))
shape_tri_l = shape(7, 7, -3, -3, lambda d, k: d.polygon([(0.5 * k, 3.5 * k), (6 * k, 0.5 * k), (6 * k, 6.5 * k)], fill=255))
shape_tri_r = shape(7, 7, -3, -3, lambda d, k: d.polygon([(1 * k, 0.5 * k), (6.5 * k, 3.5 * k), (1 * k, 6.5 * k)], fill=255))


# --- the background -----------------------------------------------------------------------------------------------------
def background():
    """A smooth vertical gradient, dark blood-brown at the top to near black at
    the bottom, a soft vignette; no grain, no stains. Returns the picture and where its gradient shows (1 - the overlay's
    alpha): only there the RGB565 conversion dithers, the lines and slabs stay flat."""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    t = yy / (H - 1)
    top, bottom = np.array((34, 17, 16), np.float32), np.array((7, 6, 6), np.float32)
    a = top + (bottom - top) * (t ** 0.8)[..., None]
    dd = np.sqrt(((xx - W / 2) / (W / 2)) ** 2 + ((yy - H * 0.4) / (H * 0.75)) ** 2)
    a *= (1 - 0.45 * np.clip(dd - 0.45, 0, 1) ** 1.3)[..., None]
    img = Image.fromarray(np.round(a).clip(0, 255).astype(np.uint8))
    base = a
    layer = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    k = SS
    blood = (90, 10, 10, 255)
    d.line((8 * k, L['LINE1_Y'] * k, 312 * k, L['LINE1_Y'] * k), fill=blood, width=k)
    d.line((8 * k, L['LINE2_Y'] * k, 312 * k, L['LINE2_Y'] * k), fill=blood, width=k)
    # the map's frame
    mx, my, mw, mh = L['MAP_X'], L['MAP_Y'], L['MAP_W'], L['MAP_H']
    d.rectangle(((mx - 1) * k, (my - 1) * k, (mx + mw) * k, (my + mh) * k), outline=(40, 30, 28, 255), width=k)
    # the buttons' slabs (lit at run time: the inside and the line beneath)
    cells = [(L['WEP_X'] + i * (L['WEP_W'] + L['WEP_GAP']), L['WEP_W']) for i in range(4)]
    for x0, bw in cells + [(L['CASE_X'], L['CASE_W']), (L['GEAR_X'], L['GEAR_W'])]:
        x1, y0, y1 = x0 + bw, L['BTN_Y'], L['BTN_Y'] + L['BTN_H']
        d.rectangle((x0 * k, y0 * k, x1 * k - 1, y1 * k - 1), fill=(22, 20, 20, 255), outline=(48, 44, 42, 255), width=k)
        d.rectangle(((x0 + 1) * k, (y1 - 3) * k, (x1 - 1) * k - 1, (y1 - 1) * k - 1), fill=blood)
    layer = layer.resize((W, H), Image.LANCZOS)
    alpha = np.asarray(layer)[..., 3].astype(np.float32) / 255
    over = np.asarray(layer)[..., :3].astype(np.float32)
    exact = base * (1 - alpha[..., None]) + over * alpha[..., None]  # (unrounded: the dither works from it)
    img.paste(layer, (0, 0), layer)
    return img, exact, 1 - alpha


bg, exact, open_ = background()
# RGB565 with an ordered 8x8 Bayer dither within one step where the gradient shows (32 steps of red and blue would
# band on a dark gradient); the lines and slabs as they are
b2 = np.array([[0, 2], [3, 1]])
bayer = b2
for _ in range(2):
    bayer = np.block([[4 * bayer + b2[0, 0], 4 * bayer + b2[0, 1]], [4 * bayer + b2[1, 0], 4 * bayer + b2[1, 1]]])
thr = ((bayer + 0.5) / 64.0)[np.arange(H)[:, None] % 8, np.arange(W)[None, :] % 8]
def quant(v, step, keep):
    d = np.floor((v + (thr - 0.5) * step * keep) / step + 0.5)
    return d.clip(0, 255 // step).astype(np.uint32)
flat = np.asarray(bg).astype(np.float32)
src = exact * (open_[..., None] > 0.99) + flat * (open_[..., None] <= 0.99)
keep = (open_ > 0.99).astype(np.float32)
r5, g6, b5 = quant(src[..., 0], 255 / 31, keep), quant(src[..., 1], 255 / 63, keep), quant(src[..., 2], 255 / 31, keep)
rgb565 = (r5 << 11) | (g6 << 5) | b5
if PREVIEW:
    Image.fromarray(np.stack([(r5 * 255 + 15) // 31, (g6 * 255 + 31) // 63, (b5 * 255 + 15) // 31], -1)
                    .astype(np.uint8)).save(PREVIEW)  # (as the screen shows it)
fbo = np.zeros(W * H, np.uint16)
for x in range(W):
    fbo[x * 240:(x + 1) * 240] = rgb565[::-1, x]  # (the column from the bottom row up)

# --- the header ---------------------------------------------------------------------------------------------------------
blob = bytearray()
table = []
for (w, h, ox, oy, adv, data) in pieces:
    table.append((len(blob), w, h, ox, oy, adv))
    blob += data


def hexrows(vals, fmt, per):
    rows = []
    for i in range(0, len(vals), per):
        rows.append('    ' + ','.join(fmt % v for v in vals[i:i + per]) + ',')
    return '\n'.join(rows)


o = ['// Generated by tools/ui_assets.py (stage 9: the bottom screen\'s look "B - the dark"): do not edit.',
     '// Noto Serif JP and Noto Serif SC (SIL Open Font License 1.1: licenses/OFL-1.1.txt): their glyphs drawn into',
     '// coverage here.',
     '#pragma once', '', '#include <stdint.h>', '']
for k_, v in L.items():
    o.append('#define UI_%s %d' % (k_, v))
o.append('')
o.append('enum { ' + ', '.join('UI_LANG_' + l.upper() for l in LANGS) + ', UI_LANGS };')
o.append('enum { ' + ', '.join('UI_' + s[0] for s in STRINGS) + ', UI_STRINGS };')
o.append('enum { ' + ', '.join('UI_T_' + t[0] for t in TEMPLATES) + ', UI_TEMPLATES };')
o.append('enum { ' + ', '.join('UI_DS_' + d[0] for d in DSTYLES) + ', UI_DSTYLES };')
o.append('#define UI_DIGITS "%s"  // (UTF-8: the glyphs of a style, in this order; a char not found - none)' % DIGITS)
o.append('#define UI_PULSE_LEON %d' % pulse_leon)
o.append('#define UI_PULSE_ASHLEY %d' % pulse_ashley)
o.append('#define UI_SHAPE_CHECK %d' % shape_check)
o.append('#define UI_SHAPE_ARROW_L %d' % shape_arrow_l)
o.append('#define UI_SHAPE_ARROW_R %d' % shape_arrow_r)
o.append('#define UI_SHAPE_GEAR %d' % shape_gear)
o.append('#define UI_SHAPE_CASE %d' % shape_case)
o.append('#define UI_SHAPE_TRI_U %d' % shape_tri_u)
o.append('#define UI_SHAPE_TRI_D %d' % shape_tri_d)
o.append('#define UI_SHAPE_TRI_L %d' % shape_tri_l)
o.append('#define UI_SHAPE_TRI_R %d' % shape_tri_r)
o.append('#define UI_SHAPE_ROTATE %d' % shape_rotate)
o.append('#define UI_SHAPE_TRASH %d' % shape_trash)
o.append('')
o.append('struct UiPiece {\n    uint32_t off;  // into kUiCov\n    uint16_t w, h;\n    int16_t ox, oy;  // the box from the baseline\'s left end\n    int16_t adv;\n};')
o.append('static const UiPiece kUiPiece[%d] = {' % len(table))
o.append('\n'.join('    {%d, %d, %d, %d, %d, %d},' % t for t in table))
o.append('};')
o.append('static const int16_t kUiStr[UI_LANGS][UI_STRINGS] = {')
for li in range(len(LANGS)):
    o.append('    {' + ', '.join(str(str_idx[s[0]][li]) for s in STRINGS) + '},')
o.append('};')
o.append('// the templates: up to 8 tokens (a piece, -1 the argument 0, -2 the argument 1), the count first')
o.append('static const int16_t kUiTpl[UI_LANGS][UI_TEMPLATES][9] = {')
for li in range(len(LANGS)):
    row = []
    for t in TEMPLATES:
        toks = tpl_tokens[t[0]][li]
        row.append('{' + ', '.join(str(v) for v in [len(toks)] + toks + [0] * (8 - len(toks))) + '}')
    o.append('    {' + ', '.join(row) + '},')
o.append('};')
o.append('static const int16_t kUiDigit[UI_DSTYLES][%d] = {' % len(DIGITS))
for d in digit_idx:
    o.append('    {' + ', '.join(str(v) for v in d) + '},')
o.append('};')
o.append('static const uint8_t kUiCov[%d] = {' % len(blob))
o.append(hexrows(list(blob), '0x%02x', 32))
o.append('};')
o.append('// the background: RGB565, the framebuffer\'s order (x * 240 + 239 - y)')
o.append('static const uint16_t kUiBg[320 * 240] = {')
o.append(hexrows([int(v) for v in fbo], '0x%04x', 24))
o.append('};')
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'w', encoding='utf-8') as fh:
    fh.write('\n'.join(o) + '\n')
print('ok: %s (%d pieces, %d KB of coverage, background %d KB)' % (OUT, len(table), len(blob) >> 10, W * H * 2 >> 10))
