# План переноса ClearRss на C/GTK3

## Оригинал

Источник: `/usr/share/screenlets/ClearRss/ClearRssScreenlet.py` v0.1 и `menu.xml`.
Оригинальные ассеты оставляем только эталоном: `themes/default/background.svg` и
`themes/Simple/background.svg` копируются в проект, чтобы runtime не зависел от
python2-пакета.

## Поведение

- базовое окно 200×200, themes `default` и `Simple`;
- один выбранный RSS/Atom feed, `feed_name` + `feed_url`;
- периодическое обновление (1..60 минут), ручное Refresh;
- список entries: title + summary, переход Previous/Next item;
- прокрутка длинного текста кнопками в нижней части окна и колесом мыши;
- View this News открывает ссылку выбранной записи через `xdg-open`;
- Properties: RSS, Text и Theme; шрифт, цвета текста/фона, интервал;
- guest frame: сначала рисует snapshot фона родителя, затем свой theme/content;
- один timer ядра для UI, сетевые запросы — libsoup async, разбор XML — libxml2
  в worker-потоке; в GTK main loop возвращаемся только через `g_idle_add`.

## Интеграция

- модуль `build/clearrss.so`, descriptor type = `clearrss`;
- менеджер Applet management подхватит модуль и иконку автоматически;
- конфиг инстанса — обычная секция в `.plugins/<type>-UUID-label.conf`;
- themes ищутся сначала в `~/.config/xscreenlets/themes/clearrss`, затем в
  `icons`-независимом project-local fallback `themes/clearrss`;
- plugin никогда не трогает `plugins_on`;
- сборка без изменения ABI `xs_api.h`: только новый source/object/target/install.

## Стартовый feed

Дефолтный feed — `https://lwn.net/headlines/newrss`. При недоступной сети
UI сохраняет последнюю успешную модель и показывает состояние ошибки;
пустой ответ отличается от HTTP/XML/network error.
