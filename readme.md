# ntchffs-overlay

![Release](https://img.shields.io/github/v/release/notch4ff4/ntchffs-overlay)
![Downloads](https://img.shields.io/github/downloads/notch4ff4/ntchffs-overlay/total)

Данный плагин представляет собой ультимативный оверлей для OBS Studio, напоминающий всем известный Nvidia ShadowPlay, только ***на стероидах***.

## Возможности

Полное описание основного функционала и всех уникальных возможностей доступно в [Wiki Features](https://github.com/notch4ff4/ntchffs-overlay/wiki/Features). Рекомендуем ознакомиться с этим разделом: помимо перечня функций, в нём приведены примеры использования, а при необходимости подробно объяснён принцип работы каждой из них.

# Требования

| Компонент  | Версия                 |
| ---------- | ---------------------- |
| OBS Studio | 32.2+                  |
| Windows    | 10/11                  |
| Linux      | Пока не поддерживается |
| macOS      | Пока не поддерживается |

Сборка плагина линкуется к FFmpeg, поставляемому вместе с OBS. Релиз `1.1.0+` рассчитан на OBS Studio 32.2+ (FFmpeg 8) и не загрузится на OBS 32.1.x.
# Установка
## Windows

Скачайте последнюю версию из раздела [Releases](https://github.com/notch4ff4/ntchffs-overlay/releases). Доступны два варианта:

| Файл | Для чего |
| ---- | -------- |
| `ntchffs-overlay-*-windows-x64-Installer.exe` | Обычная установка OBS (через установщик) |
| `ntchffs-overlay-*-windows-x64-portable.zip` | Portable-версия OBS |

Перед установкой закройте OBS Studio.

### Обычная установка (Installer)

1. Запустите `ntchffs-overlay-*-windows-x64-Installer.exe`.
2. На следующем шаге при необходимости измените путь установки.
3. Перезапустите OBS Studio.
4. Настройте горячие клавиши для открытия оверлея и сохранения мгновенного повтора в настройках OBS Studio.

### Portable OBS (ZIP)

1. Распакуйте `ntchffs-overlay-*-windows-x64-portable.zip`.
2. Скопируйте папки `obs-plugins` и `data` из архива в корень вашей portable-установки OBS (туда, где лежат `bin`, `data` и `obs-plugins`).
3. При необходимости подтвердите замену существующих файлов.
4. Перезапустите OBS Studio.
5. Настройте горячие клавиши в настройках OBS Studio.

Структура после распаковки в корень portable OBS:

```
OBS-Studio\
  obs-plugins\64bit\ntchffs-overlay.dll
  data\obs-plugins\ntchffs-overlay\locale\...
```

## Linux
Скоро…
## macOS
Скоро…

# Roadmap

Актуальный план развития проекта опубликован в [Wiki Roadmap](https://github.com/notch4ff4/ntchffs-overlay/wiki/Roadmap). В нём собраны как уже реализованные возможности, так и функции, которые находятся в разработке или запланированы к реализации. По мере развития проекта этот список регулярно обновляется.

# Благодарности
- **OBS Studio Team** – за прекрасное приложение OBS Studio
- Сообществу [**Max.mov | ALLPLAY**](https://t.me/a11p1ay) и отдельно [**Max.mov™**](https://www.youtube.com/@MAXiM0V) – за идейное вдохновение

# Лицензии

Плагин распространяется на условиях [GNU GPL v2](LICENSE). Список сторонних компонентов и их лицензий – в [LICENSES.md](LICENSES.md).

# Поддержать проект
Этот проект абсолютно бесплатный и открытый. Если плагин оказался полезным и Вы хотите поддержать его разработку финансово:

https://www.donationalerts.com/r/notch4ff4

Это поможет уделять больше времени разработке, исправлению ошибок и выпуску новых функций. Спасибо тем, кто помогает развивать проект <3
