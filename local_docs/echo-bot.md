# Echo bot — локальный LXMF echo-сервис RNSBox (дизайн + статус)

Ветка: `echo-bot` (база `raspberry-pi-zero-2w`), всё запушено в локальный
Windows-репо. Статус на момент паузы: код готов и закоммичен; WSL-смоук-тест
прошёл полностью; QEMU-приёмка частично (см. §6).

## 1. Цель

Локальный LXMF echo bot, работающий «из коробки» на каждой RNSBox:

- анонсирует свой LXMF-адрес раз в день по умолчанию (интервал настраивается);
- принудительный анонс кнопкой из веб-морды;
- эхо-ответ на любое входящее LXMF-сообщение;
- имя ноды: база задаётся вручную, суффикс «6 hex» генерится автоматически
  (первые 3 байта hash identity бота), по умолчанию `rnsbox echo bot <hex>`.

Референс: https://github.com/liamcottle/lxmf-echobot — **без LICENSE-файла**,
поэтому код НЕ вендорился: написан собственный эквивалент на RNS+LXMF API.

## 2. Ключевые факты разведки

- **LXMF 0.9.8**: `LXMRouter(identity=, storagepath=)`,
  `register_delivery_identity(identity, display_name=)` (одна на роутер),
  `register_delivery_callback`, `announce(hash)`, `handle_outbound`;
  `LXMessage(destination, source, content, title, desired_method)`.
- **RNS 1.5.7 shared instance**: клиент подключается к rnsd через локальный
  интерфейс (AF_UNIX `@rns/<path>` по умолчанию на Linux, либо TCP
  `shared_instance_port`, в shipped-конфиге 37428) и **сам
  переподключается** при рестарте rnsd. `require_shared_instance=True`
  падает с SystemError, если инстанса нет; singleton RNS ломает повторный
  `RNS.Reticulum()`, поэтому демон сам ждёт сокет, опрашивая /proc (тот же
  тест, что `rns_shared_up()` в S85lxmd), и порт берёт из конфига.
- **Сосуществование с lxmd/nomadnet**: у каждого процесса свой LXMRouter и
  своя identity ⇒ свои destination-хэши, один транспорт. LXMRouter без
  propagation-режима хранит только мелкие кэши (ratchets, stamp costs) —
  SD-экономика в порядке.
- **Identity**: `/var/lib/rnsbox-echobot/identity.pem` на персистентном
  rootfs (создаётся один раз, umask 077) ⇒ адрес и суффикс стабильны.
- Важно: `state.json["lxmf_addr"]` — hash IDENTITY; адрес NomadNet —
  hash lxmf.delivery DESTINATION (`dest_hash`), это разные 20-байтные хэши.

## 3. Файлы (всё в overlay, без нового buildroot-пакета)

| Файл | Что |
|---|---|
| `board/raspberrypi/.../rootfs-overlay/usr/bin/rnsbox-echobot` | демон (python3, RNS+LXMF) |
| `board/raspberrypi/.../rootfs-overlay/etc/init.d/S87echobot` | start/stop/restart/status/**announce**/enabled; RAM-пол min_free_mb; FIFO→logger; setsid + групповой стоп (как S85lxmd) |
| `board/raspberrypi/.../rootfs-overlay/etc/rnsbox/echobot.conf` | enabled=yes, interval=86400, name, announce_at_start=yes, min_free_mb=40 |
| `/var/lib/rnsbox-echobot/{identity.pem,state.json,lxmf/}` | персистентное состояние бота (создаётся на старте) |
| `package/rnsbox-portal/src/cgi/routes_echobot.cpp`, `pages/echobot.html` | новая вкладка «Echo bot» |
| `package/rnsbox-portal/src/cgi/{routes.h,main.cpp,pages/base.html}` | минимальная проводка (роутинг, POST_ONLY, nav) |
| `patches/licheerv-nano/0025-...patch`, `patches/milkv-duo-01/0027-...patch` | те же 3 файла в SG200X overlay (проверены `git apply --check` по накатанному BSP-дереву) |

Дефолт `enabled=yes` (обоснование в conf и §1 постановки): это фича «из
коробки», 1 анонс/день, ~15–25 МБ RSS, RAM-пол переводит «тесную коробку» в
«бот выключен», никогда «rnsd убит OOM». Выключается чекбоксом в морде.

Демон: ждёт shared instance (опрос /proc, статус waiting в state.json),
анонс при старте + по интервалу (monotonic-планирование), SIGUSR1 ⇒
внеплановый анонс, входящие ⇒ очередь ⇒ worker (путь ≤15 c, DIRECT,
OPPORTUNISTIC при известном ratchet), эхо title+content (fields НЕ
эхо-даются — там бывают мегабайтные вложения), счётчики echo_ok/echo_fail.

## 4. Морда

Вкладка «Echo bot»: статус (running/stopped/disabled + detail), LXMF-адрес,
полное имя + суффикс (не редактируется), счётчики анонсов/эха, последний
анонс; формы: enabled, имя, интервал (1 ч / 6 ч / 12 ч / 1 день / 2 дня /
7 дней; сторонний интервал в conf показывается как custom); кнопки
«Announce now» (`S87echobot announce`) и «Restart» (`S87echobot restart`).
Save пишет conf (сохраняя операторский min_free_mb) и рестартует сервис.
Данные — из state.json и pidfile+comm; python на отрисовке не спавнится.

## 5. Что проверено и работает

- **WSL-смоук (host, python 3.12, RNS 1.5.7 + LXMF 0.9.8, rnsd + бот +
  клиент, loopback-only конфиг)**: ожидание shared instance; identity
  создаётся/грузится, суффикс=первые 3 байта hash; анонс при старте;
  **эхо «ping» получено** (DIRECT); SIGUSR1 ⇒ announce_count+1 и строка
  «LXMF announced … as "rnsbox smoke bot cc3301"»; счётчики и last_announce
  в state.json; counters персистят между рестартами.
- **QEMU aarch64 `virt` (qemu-rootfs-arm.ext4 — generic-порт с python+RNS+
  LXMF+порталом)**: бот стартует из rcS через S87echobot («Starting echobot:
  OK (pid 680, 438 MB free, shared instance)»), state.json создаётся
  (status running, announce_count 1), S87echobot stop/announce/enabled
  работают, отказ при rnsd-отсутствии — корректный. Инжекция файлов в ext4 —
  debugfs (нюансы: `sif` портит checksum — только chmod staging-файлов;
  `write` не перезаписывает существующие — rm перед write; грязный образ
  после убитой сессии — e2fsck -fy до инжекции).

## 6. Что НЕ доделано (актуальный список)

1. **QEMU-приёмка не зелёная целиком**: в последнем прогоне бот стартовал
   при загрузке (rcS, rnsd ещё не поднят в госте), но повторные
   `S87echobot start` в тесте получали отказ — в госте стартованный
   тестом rnsd не находится `rnsd_running()`/pidof в этом гибридном образе
   (нужно добить: в госте rnsd из rcS стартует, тестовая последовательность
   конфликтует с загрузочным запуском; вероятно, достаточно убрать ручной
   `S82rnsd start`/`stop` из сценария и дождаться загрузочного бота).
2. **Echo-тест в госте** (`/root/echotest.py`) не дошёл до выполнения —
   упирается в п.1.
3. **CGI-тест в госте**: вход по admin/admin не прошёл (auth.json удалён,
   дефолт должен пересоздаться — проверить `session::ensure_default_auth`
   путь/файл в этом образе); 404 на /echobot в первом прогоне был из-за
   старого бинарника — свежий кросс-билд (build_portal_arm64.sh,
   aarch64-musl toolchain из out/rpi0-2w) инжектится, но до него прогон не
   дошёл.
4. **logread в тестовом образе отсутствует** — проверку строки «LXMF
   announced» в syslog в госте заменить на state.json/файловый лог.
5. **Железная проверка** (пользователь, NomadNet): см. §7.
6. Мелочь: S87 ссылается на min_free_mb как на «floor для rnsd» — ок;
   документ README не расширял (не моя секция).

## 7. Железная проверка (когда образ собран)

1. Портал → «Echo bot»: статус running, адрес `<dest_hash>`, имя
   `rnsbox echo bot <6 hex>`.
2. NomadNet → Conversations → адрес бота → любой текст → эхо (секунды).
3. «Announce now» → `logread | grep echobot` покажет «LXMF announced»,
   last_announce в state.json обновится, в списке нод NomadNet имя/дата
   обновятся.
4. Смена имени/интервала → Save → следующий анонс с новым именем.
5. Ребут → адрес и суффикс те же (identity персистентна).

## 8. Границы

Владение: демон, echobot.conf, S87echobot, вкладка Echo bot, патчи
0025/0027. Не тронуто: S46wanwatch, lxmd-конфигурация, чужие вкладки
портала, defconfig'ы, arm/cv1800b-серии.

Артефакты стенда (вне репо): `/home/noname/rnsbox-work/echobot-qemu/`
(mk_image.sh → echobot-test.ext4, echobot-accept.exp, accept.log),
`Temp/echobot/{smoke.sh,pure_rns.sh,cgitest.sh,echotest.py}`.
