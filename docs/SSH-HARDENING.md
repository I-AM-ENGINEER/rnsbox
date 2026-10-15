# SSH hardening — SSH выключен по умолчанию, пользователь `admin`

Модель безопасности, принятая во всех образах RNSBox (начиная с этой версии).

## Кратко

- **SSH выключен по умолчанию.** `sshd` не запускается при загрузке: скрипт
  `/etc/init.d/S50sshd` (наш gate-скрипт из overlay, заменяет пакетный) читает
  `/etc/rnsbox/ssh.conf` и стартует сервер только при `enabled=yes`.
- **Включается только через настройки** — портал: **Settings → SSH**.
  Включение требует задать пароль; выключение останавливает `sshd` и
  возвращает `enabled=no` (переживает перезагрузку).
- **Пароль задаёт пользователь.** При включении SSH форма требует пароль
  (дважды, минимум 6 символов) для unix-пользователя `admin`. Портал задаёт его
  через `passwd -a sha512 admin` (CGI работает от root). Повторное включение
  после выключения пароль не сбрасывает; поля можно оставить пустыми и просто
  сменить пароль в любой момент на той же карточке.
- **root закрыт.** В `sshd_config` — `PermitRootLogin no`; кроме того, пароль
  root заблокирован на этапе сборки (`BR2_TARGET_ENABLE_ROOT_LOGIN` выключен —
  buildroot пишет `root:*` в `/etc/shadow`). Войти как root нельзя ни по SSH,
  ни с консоли.
- **По умолчанию пользователь `admin`.** В образе через users-таблицу
  (`board/<board>/users.txt`) создан непривилегированный пользователь
  `admin` (uid/gid 1000, shell `/bin/sh`, home `/home/admin`). Его пароль
  заблокирован (`!`) до тех пор, пока пользователь не задаст его при включении
  SSH. Консольный вход (HDMI/UART) — тоже `admin`.
- **WAN закрыт.** `tcp 22` больше не входит в поставляемый
  `/etc/rnsbox/openports.conf`: WAN-сторона SSH открывается только вручную
  через **Network → Open ports**. LAN-стороне (USB-C gadget / WiFi-точка /
  Ethernet) отдельного правила не нужно — LAN принимается фаерволом целиком.

## Порядок включения SSH (портал)

1. Откройте портал `http://10.42.0.1/`, войдите (пароль веб-интерфейса — не
   путать с SSH-паролем — меняется в **Settings → Change admin password**).
2. **Settings → SSH**: отметьте *Enable SSH server*, введите пароль для
   `admin` дважды, **Save SSH setting**.
3. Статус на карточке покажет `running`; подключение:
   `ssh admin@10.42.0.1` (LAN) или `ssh -p 2222 root@…` — не сработает: root
   отклоняется всегда.

Выключение: снять галочку, **Save SSH setting** — `sshd` будет остановлен
(`enabled=no` сохраняется в `ssh.conf`).

## Красный баннер на дашборде

Если пароль веб-портала всё ещё заводской `admin/admin`, вверху дашборда
выводится красное предупреждение со ссылкой на смену пароля. Кнопка «×»
скрывает баннер только до следующего рендера/входа — отклонение нигде не
сохраняется, баннер возвращается при каждом входе, пока пароль не сменён.
Проверка делается на сервере: сохранённый pbkdf2-хеш из `/etc/rnsbox/auth.json`
проверяется на соответствие «admin» (`session::default_password_active()`).

## Как это устроено (для сопровождающих)

| Файл | Роль |
| --- | --- |
| `/etc/rnsbox/ssh.conf` | `enabled=no|yes` — единственный источник истины для gate |
| `/etc/init.d/S50sshd` (overlay) | gate-скрипт: `start` молча выходит при `enabled=no`; `restart` = stop + start-if-enabled. Заменяет пакетный скрипт (overlay копируется после пакетов, обновления пакета не ломают gate) |
| `/etc/ssh/sshd_config` (overlay) | `PermitRootLogin no`, `PasswordAuthentication yes`, `PermitEmptyPasswords no` |
| `users.txt` (users-таблица) | пользователь `admin`, пароль заблокирован, uid 1000 |
| defconfig | `# BR2_TARGET_ENABLE_ROOT_LOGIN is not set` → root заблокирован сборкой |
| `openports.conf` | без `tcp 22` — WAN закрыт |
| `package/rnsbox-portal` | карточка SSH (Settings), route `POST /settings/ssh`, статус `running/disabled/stopped`, красный баннер дашборда |

Пароль `admin` для SSH портал задаёт вызовом `passwd -a sha512 admin`
(двойной ввод подаётся на stdin; shell не используется — `util::run` делает
execve напрямую). В образе нет applet'а `chpasswd`, `passwd` — тот же бэкенд.
Перед записью `ssh.conf` портал убеждается, что хеш появился в `/etc/shadow`
(строка `admin` не `!`/`*`/пустая).

LAN vs WAN: `S29fw`/`S60routing` (nftgen) принимают весь трафик с LAN-интерфейсов;
`openports.conf` открывает порты только на WAN. Поэтому SSH после включения
доступен из LAN сразу, а из WAN — только после явного `tcp 22` в Open ports.

## Зеркалирование на платы duo (необходимая дельта)

Эта ветка реализует модель на примере rpi0-2w (эталонный overlay) и общих
частях портала. Параллельная линия плат milkv-duo должна повторить у себя:

1. `board/<duo-board>/users.txt` — та же users-таблица (пользователь `admin`,
   пароль `!`), и в defconfig каждой duo-платы:
   `BR2_ROOTFS_USERS_TABLES="$(BR2_EXTERNAL_RNSBOX_PATH)/board/<duo-board>/users.txt"`.
2. Overlay-файлы (копируются из `board/raspberrypi/rnsbox-0-2w/rootfs-overlay/`):
   `etc/init.d/S50sshd` (gate), `etc/rnsbox/ssh.conf` (`enabled=no`),
   `etc/ssh/sshd_config` (`PermitRootLogin no`).
3. `etc/rnsbox/openports.conf` overlay-а платы — убрать строку `tcp 22`.
4. В defconfig каждой duo-платы — убрать
   `BR2_TARGET_GENERIC_ROOT_PASSWD="…"` (root остаётся заблокированным,
   `# BR2_TARGET_ENABLE_ROOT_LOGIN is not set`).
5. Портал (карточка SSH, route `POST /settings/ssh`, красный баннер) — общий
   код `package/rnsbox-portal`, отдельных работ по платам не требует.
6. Правки документации (`docs/BOARD-*.md`, `scripts/clients-stick`): убрать
   упоминания доступа `root/…` по SSH, описать включение через портал.

### Осторожно: /etc/shadow и glibc (актуально для glibc-плат duo)

Строки `/etc/shadow` должны содержать **ровно 9 полей (8 двоеточий)**.
glibc-шный nss_files **молча выбрасывает** строку с лишним полем: для такого
пользователя `getspnam()` возвращает `NULL`, и openssh запрещает парольный
вход (сравнивает с `x` из /etc/passwd), при этом busybox-утилоты (`passwd`,
`su`) работают нормально — проблема выглядит как необъяснимый отказ sshd.
buildroot (`support/scripts/mkusers`) пишет корректные строки
(`admin:!:::::::` — 7 двоеточий после `!`), поэтому собранный образ всегда
корректен; правило касается только ручной правки rootfs (post-build скрипты,
довнастройка образов): после таких правок проверяйте формат строк shadow.

## Миграция существующих установок (обновление на месте)

- После обновления **SSH окажется выключен** (`/etc/rnsbox/ssh.conf`
  отсутствовал → gate считает SSH выключенным). Это осознанно: прежний доступ
  `root/admin` по SSH закрыт.
  Включите SSH через портал (**Settings → SSH**), задав пароль для `admin`.
- **Учётная запись root**: у существующих установок root-пароль остаётся как
  был (обновление образа per-partition применит новую модель; в живой системе
  `root:*` не переписывается) — но вход root по SSH всё равно закрыт
  `PermitRootLogin no` из обновлённого `sshd_config`.
- **Пользователь admin** появляется с образом; на обновлённой «на живую»
  системе его можно создать вручную:
  `adduser -D -h /home/admin -s /bin/sh admin && passwd -l admin`.
- Файрвол: если в `openports.conf` вручную добавляли `tcp 22`, запись
  сохранится при обновлении — уберите её, если SSH из WAN не нужен.
- Консольный вход: теперь `admin` (не root). Экстренный доступ остаётся:
  однопользовательский режим загрузчика.

Пароль веб-портала (`auth.json`) и SSH-пароль `admin` (unix) — **разные**
пароли: первый охраняет веб-интерфейс, второй — shell/SSH.
