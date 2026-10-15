# Echo bot — локальный LXMF echo-сервис RNSBox (дизайн)

Ветка: `echo-bot` (база `raspberry-pi-zero-2w`). Статус: реализовано, см. сводку в конце.

## 1. Цель

Локальный LXMF echo bot, работающий «из коробки» на каждой RNSBox:

- анонсирует свой LXMF-адрес раз в день по умолчанию (интервал настраивается);
- принудительный анонс кнопкой из веб-морды;
- эхо-ответ на любое входящее LXMF-сообщение (проверка достижимости с любого
  LXMF-клиента — NomadNet, Sideband и т.п.);
- имя ноды: база задаётся вручную, суффикс «6 hex-символов» генерится
  автоматически (не редактируется), по умолчанию `rnsbox echo bot <suffix>`.

Референс: https://github.com/liamcottle/lxmf-echobot (крошечный: анонс + эхо).
У референса **нет файла LICENSE** (только README/код) — код НЕ вендорится;
написан собственный минимальный эквивалент на чистом RNS+LXMF API в стиле
проекта (стратегия согласована с постановкой задачи).

## 2. Разведка (факты, на которых стоит дизайн)

- **LXMF 0.9.8 API** (package/python-lxmf, патч 0001 redates stored messages):
  `LXMRouter(identity=, storagepath=)`, `register_delivery_identity(identity,
  display_name=, stamp_cost=)` (ровно одна delivery-identity на роутер),
  `register_delivery_callback(cb)` (cb получает `LXMessage`), `announce(hash)`,
  `handle_outbound(lxmsg)`, `delivery_link_available(hash)`,
  `get_outbound_ticket/get_outbound_stamp_cost`. `LXMessage(destination, source,
  content, title, fields, desired_method)`. Методы доставки `DIRECT` /
  `OPPORTUNISTIC` (оппортунистический быстрее — один пакет — но требует
  известного ratchet у получателя).
- **RNS 1.5.7 shared instance**: клиент подключается к rnsd через
  LocalClientInterface (AF_UNIX `@rns/<path>` или TCP 127.0.0.1:37428) и
  **сам переподключается**, когда rnsd перезапускается. `RNS.Reticulum(...,
  require_shared_instance=True)` падает с SystemError, если shared instance
  нет — вместо молчаливого поднятия второго инстанса. Подводный камень
  (гонка на загрузке): `Reticulum.__instance` присваивается ДО ошибки, поэтому
  повторный `RNS.Reticulum()` после неудачи невозможен (OSError reinitialise);
  поэтому демон сам ждёт появления shared instance, опрашивая `/proc/net/unix`
  (`@rns/*` в состоянии listen) и `/proc/net/tcp` (127.0.0.1:37428 LISTEN) —
  тот же тест, что `rns_shared_up()` в S85lxmd/S46wanwatch — и строит
  `RNS.Reticulum()` один раз.
- **Сосуществование с lxmd/nomadnet**: у каждого процесса свой LXMRouter и
  своя identity ⇒ свои destination-хэши; транспорт один (shared instance rnsd).
  LXMRouter без propagation-режима хранит только кэши (local_deliveries,
  outbound_stamp_costs, ratchets) — объём мал, пишет редко (SD не изнашивает).
  lxmd (S85lxmd) и echobot (S87echobot) независимы: любой может быть выключен.
- **Identity**: rnsd держит `/etc/reticulum/storage` на tmpfs и персистит
  только transport identity в `/etc/rnsbox/identity.pem`. У lxmd identity —
  `/etc/lxmd/identity` на персистентном rootfs. Echo bot: identity в
  `/var/lib/rnsbox-echobot/identity.pem` (rootfs персистентен; создаётся один
  раз, umask 077) ⇒ LXMF-адрес и суффикс имени стабильны между ребутами и
  обновлениями.
- **Суффикс**: первые 3 байта `RNS.Identity.hash` (truncated hash публичного
  ключа) в hex = 6 символов, например `a1b2c3`. Функция identity детерминирована
  ⇒ суффикс постоянен для данной ноды.
- **Портал**: CGI на C++ (`package/rnsbox-portal/src/cgi/`), страницы —
  `.html` с `__TOKEN__`, POST-redirect-GET с flash, мутации POST-only
  (`POST_ONLY` в main.cpp), `web::run_init("S8x", arg)` дёргает init-скрипты.
  Источники пакета на patch-flow платах (SG200X) НЕ вендорятся: build.sh
  rsync-ит repo `package/` в BSP `buildroot/package/` (см. патч 0024 в сериях)
  ⇒ изменения портала автоматически попадают на все платы.

## 3. Архитектура

```
            /etc/reticulum/config (shared instance, share_instance=Yes)
rnsd (S82rnsd)  <----  локальный сокет  ---->  rnsbox-echobot (S87echobot)
   транспорт, интерфейсы                        свой LXMRouter + identity
                                                LXMF destination "lxmf.delivery"
        ^                                                   ^
        |  SIGUSR1 ("анонсировать сейчас")                   | state.json
        +---- /etc/init.d/S87echobot announce <--- портал (Echo bot tab)
```

### Файлы (всё — rootfs overlay, без нового buildroot-пакета)

| Файл | Назначение |
|---|---|
| `/usr/bin/rnsbox-echobot` | демон (python3, только RNS+LXMF) |
| `/etc/init.d/S87echobot` | start/stop/restart/status/`announce`/`enabled` |
| `/etc/rnsbox/echobot.conf` | `enabled=`, `interval=`, `name=`, `announce_at_start=`, `min_free_mb=` |
| `/var/lib/rnsbox-echobot/identity.pem` | identity бота (персистентная, 0600) |
| `/var/lib/rnsbox-echobot/lxmf/` | хранилище LXMRouter (ratchets, кэши) |
| `/var/lib/rnsbox-echobot/state.json` | статус для портала (адрес, суффикс, счётчики) |
| `/var/run/echobot.pid` | pidfile (портал/скрипт) |

Overlay-only доставка: rpi — `board/raspberrypi/rnsbox-0-2w/rootfs-overlay/…`;
SG200X (licheerv-nano → патч 0025, milkv-duo-01 → патч 0027) —
`buildroot/board/cvitek/SG200X/overlay/…`. Новый buildroot-пакет не нужен:
это три статических файла, одинаковых для всех плат, и не требуется трогать
defconfig/Kconfig (меньше конфликтов с параллельными ветками).

### Демон (`rnsbox-echobot`)

1. Читает conf. Если `enabled=no` — выходит (S-скрипт и так не стартует;
   защита от ручного запуска).
2. Ждёт shared instance rnsd (опрос `/proc`, без `RNS.Reticulum()` до
   появления; статус «waiting» в state.json — rnsd может стартовать после нас).
3. Грузит/создаёт identity; считает суффикс; effective display name =
   `<name> <suffix>`.
4. `LXMRouter(identity, storagepath=/var/lib/rnsbox-echobot)`,
   `register_delivery_identity(...)` (stamp_cost не задаём — входящие без
   proof-of-work; иначе CPU дохлой доской), `register_delivery_callback`.
5. Анонс: при старте (`announce_at_start=yes`) и каждые `interval` секунд
   (планирование по monotonic — не зависит от скачков часов). Каждый анонс
   пишется в syslog (`LXMF announced <addr> …`) и в state.json.
6. `SIGUSR1` ⇒ внеплановый анонс (кнопка в морде; S-скрипт `announce`).
7. Входящее сообщение ⇒ очередь ⇒ worker-поток (не блокируя delivery-поток
   LXMF): путь к отправителю (`has_path`/`request_path`, ожидание ≤15 с),
   ответ `LXMessage` с тем же title/content, `DIRECT` по умолчанию,
   `OPPORTUNISTIC` если линка нет и известен ratchet (как в референсе).
   **fields не эхо-даются** (в отличие от референса): там могут быть вложения
   мегабайтного размера — на mesh-канале это усиление трафика, echo-бот
   доказывает достижимость, а не гоняет файлы. Счётчики echo ok/fail — в
   state.json.
8. SIGTERM ⇒ аккуратный выход (LXMRouter персистит кэши по atexit).

Периодический таймер — простой поток с тиком 1 с (CPU ~0). RSS демона
~15–25 МБ (python + RNS/LXMF без message store).

### S87echobot

Образец — S85lxmd (rpi-версия): read_conf flat key=value; `enabled`
( exit 0 ⇔ enabled=yes); start: проверка pidfile/comm, отказ если rnsd не
запущен, RAM-пол `min_free_mb` (защита rnsd от OOM), запуск через FIFO→logger
в syslog, setsid (своя процесс-группа — стоп kill-ит группу целиком, у LXMF
бывают fork-воркеры генерации штампов); stop как в S85lxmd (STOP/TERM/CONT +
KILL группы); `announce` — `kill -USR1` пиктограмме из pidfile (после проверки
comm); flock на /run/echobot.lock. Общий инстанс демон ждёт сам, поэтому
в S46wanwatch менять ничего не нужно (на обеих платформах).

### Конфиг по умолчанию (`/etc/rnsbox/echobot.conf`)

```
enabled=yes          # см. обоснование ниже
interval=86400       # 1/день
name=rnsbox echo bot
announce_at_start=yes
min_free_mb=40
```

**Почему enabled=yes (в отличие от lxmd/nomadnet):**
- Пользователь описал бота как дефолтную фичу коробки («анонсы раз в 1 день
  по умолчанию»): это «hello world» RNSBox — коробка сразу видна в NomadNet,
  достижимость проверяется с любого LXMF-клиента без настройки.
- Стоимость ограничена: 1 анонс/день (эфир ≈ 0), демон без message store
  (~15–25 МБ RSS) на 512 МБ (Pi Zero 2 W) / 245 МБ (SG2002) рядом с rnsd
  и CGI-порталом (0 резидентных) — помещается; RAM-пол `min_free_mb` в
  S-скрипте гарантирует, что на тесной коробке не стартует (fail = «бот
  выключен», никогда «rnsd убит OOM»).
- Выключается одним чекбоксом в морде; в отличие от opt-in никто не должен
  знать про скрытый переключатель, чтобы фича вообще существовала.

### Морда (новая вкладка «Echo bot», карточки)

- **Status**: бейдж running/stopped/waiting, LXMF-адрес, полное имя
  (`<база> <суффикс>`; суффикс показан отдельно, редактировать нельзя),
  анонсов отправлено, последний анонс, эхо-ответов (ok/fail), подсказка
  «проверьте с NomadNet → Conversation → <адрес>».
- **Settings**: чекбокс enabled, имя (база; суффикс отображается рядом и
  генерится автоматом), интервал селектом (1 ч / 6 ч / 12 ч / 1 день
  [дефолт] / 2 дня / 7 дней), Save (POST `/echobot/save` → write conf →
  `S87echobot restart`).
- Кнопки: **Announce now** (POST `/echobot/announce` → `S87echobot announce`),
  **Restart** (POST `/echobot/restart`).
- Данные: `/var/lib/rnsbox-echobot/state.json` (мини-JSON-ридер как в
  routes_reticulum.cpp) + `/var/run/echobot.pid` (comm-проверка). Никаких
  python-спавнов на отрисовке страницы.

Общие файлы портала (минимальные диффы): `main.cpp` (роутинг + POST_ONLY),
`routes.h`, `pages/base.html` (nav). Новые: `routes_echobot.cpp`,
`pages/echobot.html` (подхватываются glob-ами сборки/инсталла).

## 4. QEMU-приёмка (riscv64 `virt`, методика duo01-rv-validate)

Мини-rootfs из готового `qemu-rootfs-rv.ext4` (RNS 1.5.7 + LXMF 0.9.8 уже
внутри) + инъекция моих файлов (debugfs). Проверки:
1. `S87echobot start` при работающем rnsd → pidfile, статус running в
   state.json;
2. в syslog строка `LXMF announced <addr>`;
3. второй процесс (python, тот же shared instance, своя identity + роутер)
   шлёт «ping» на адрес бота из state.json ⇒ получает эхо «ping»;
4. `S87echobot announce` ⇒ `last_announce` в state.json обновился,
   в syslog новый `LXMF announced`;
5. `S87echobot stop` ⇒ группа завершена, pidfile снят;
6. портал: `rnsbox-portal` CGI-вызовом (env REQUEST_METHOD/PATH_INFO) —
   GET `/echobot` рендерит карточку, POST `/echobot/announce` дёргает скрипт.

Железная проверка (пользователь, NomadNet):
1. Открыть портал → вкладка **Echo bot** → статус running, адрес вида
   `<20 hex>`, имя `rnsbox echo bot <6 hex>`.
2. В NomadNet: Conversations → ввести LXMF-адрес → отправить любой текст →
   дождаться эха (обычно секунды; вуали: обоим нужен путь — бот анонсится на
   общем инстансе, путь появится после анонса).
3. Кнопка **Announce now** → в syslog (`logread | grep echobot`) свежий
   `LXMF announced`; в NomadNet обновить список нод — имя/дата анонса
   обновились.
4. Смена имени/интервала в морде → Save (рестарт бота) → следующий анонс уже
   с новым именем; интервал виден в state.json (`interval`).
5. Ребут → адрес и суффикс те же (identity персистентна).

## 5. Границы

Владение: демон, echobot.conf, S87echobot, вкладка Echo bot портала,
патчи 0025 (licheerv-nano) / 0027 (milkv-duo-01). Не тронуто: S46wanwatch,
lxmd-конфигурация, чужие вкладки портала, defconfig'ы, arm/cv1800b-серии.
