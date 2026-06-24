# Лицензии

Документ описывает лицензии, применимые к **ntchff's Overlay** (`ntchffs-overlay`), и сторонним компонентам, которые используются при сборке и работе плагина.

## Лицензия проекта

**ntchff's Overlay**  
Copyright (C) 2026 ntchff

Программный код плагина распространяется на условиях **GNU General Public License v2.0 или более поздней версии** (GPL-2.0-or-later).

Полный текст лицензии: [LICENSE](LICENSE)  
Официальный текст GPL-2.0: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

---

## Зависимости времени выполнения

Плагин загружается в OBS Studio и использует библиотеки, уже присутствующие в окружении OBS или в операционной системе.

| Компонент | Назначение | Лицензия | Ссылка |
| --- | --- | --- | --- |
| [OBS Studio](https://obsproject.com/) (`libobs`, `obs-frontend-api`) | API плагинов, интеграция с интерфейсом OBS | GPL-2.0-or-later | <https://github.com/obsproject/obs-studio/blob/master/COPYING> |
| [Qt 6](https://www.qt.io/) (`Qt6::Core`, `Qt6::Widgets`) | Диалог настроек плагина | LGPL-3.0 / GPL-3.0 (dual license) | <https://www.qt.io/licensing/> |
| [FFmpeg](https://ffmpeg.org/) (`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`) | Декодирование видео и аудио в галерее (Windows) | LGPL-2.1-or-later / GPL-2.0-or-later (в зависимости от сборки) | <https://www.ffmpeg.org/legal.html> |

> **Примечание.** Конкретная лицензия FFmpeg и Qt зависит от того, как они собраны и поставляются вместе с OBS Studio. При распространении бинарных сборок следуйте требованиям лицензий OBS и его зависимостей.

## Сторонние ресурсы

### Иконки интерфейса

В оверлее используются PNG-иконки с именами, соответствующими набору [**Phosphor Icons**](https://phosphoricons.com/) (например, `caret-line-left`, `download-simple`, `trash-simple`, `speaker-high`, `chart-line`).

| Ресурс | Лицензия | Ссылка |
| --- | --- | --- |
| Phosphor Icons | MIT | <https://github.com/phosphor-icons/core/blob/main/LICENSE> |

Иконки встраиваются в DLL при сборке (каталог `data/icons/`) или подгружаются из каталога данных плагина.

---

## Инфраструктура сборки

Структура CMake, CI и вспомогательные скрипты основаны на шаблоне [**obs-plugintemplate**](https://github.com/obsproject/obs-plugintemplate) от OBS Project.

| Компонент | Лицензия | Ссылка |
| --- | --- | --- |
| obs-plugintemplate | GPL-2.0-or-later | <https://github.com/obsproject/obs-plugintemplate/blob/master/LICENSE> |

Исходники OBS Studio и предсобранные зависимости (`obs-deps`, Qt6) загружаются при сборке согласно [buildspec.json](buildspec.json) и лицензируются их правообладателями (OBS Project и соответствующие upstream-проекты).

---

## Контакты

- Автор: **ntchff**
- Сайт: <https://github.com/notch4ff4>
- E-mail: notch4ff4@gmail.com

При распространении производных работ или бинарных сборок сохраняйте уведомления об авторских правах и условия лицензий, перечисленных в этом документе.
