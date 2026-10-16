# NOTES — init-ssh-fixes (2026-10-17)

Branch `init-ssh-fixes`, work log per the owner's bug list. Every "фикс"
below is either a repo (build.sh / portal / board) change or a BSP-series
patch, with the check that proves it.

## 1. sids не ложились в defconfig (ГЛАВНЫЙ БАГ) — НАЙДЕНА ТОЧНАЯ ПРИЧИНА

**Баг:** build5.log печатает `>> slimming knobs:` (шаг выполнялся), но
`BR2_ROOTFS_USERS_TABLES` и вся ssh-политика в defconfig не менялись → в
rootfs нет юзера admin → `passwd admin` в портале падает → SSH включить
невозможно.

**Причина (не cwd!):** в `rnsbox_slim_bsp` глоб

```sh
for dc in buildroot/configs/cvitek_*.defconfig; do
```

ищет файлы с ТОЧКОЙ перед `defconfig`, а реальные файлы называются
`cvitek_SG200X_64bit_defconfig` (подчёркивание). Глоб не матчил НИ ОДНОГО
файла ни в одной сборке никогда, и `[ -f "$dc" ] || continue` молча съедал
промах. Седы не выполнялись в принципе. (Проверка: `tests/slim-pristine-test.sh`,
секция "historical-bug regression" — подтверждает, что старый глоб не матчит
реальные файлы.) Дополнительная дыра того же шага: инкрементальная сборка не
реимпортирует defconfig, а «живой» конфиг у cvitek BSP — `buildroot/.config`
В ДЕРЕВЕ, а не `buildroot/output/.config` (его sed'ы в
`rnsbox_build_bsp_generic`/duo/univ потоках таргетили несуществующий файл).

**Фикс (build.sh):**
- глоб `cvitek_*_defconfig` + якорь всех путей на абсолютный `$WORKDIR`
  (cwd-независимость);
- новый `rnsbox_assert_bsp_policy`: после седов каждая ручка политики
  (users table, root locked `*`, root login off, dropbear off, openssh on,
  py-only, resblks=1, post-build hook) проверяется `grep -xF`; отсутствующий
  символ дозаписывается в EOF (kconfig last-wins), а «символ есть, значение
  не то» — ГРОМКИЙ FATAL и падение сборки;
- новый `rnsbox_stamp_live_config` штампует ту же политику в живые
  `buildroot/.config` (и легаси `buildroot/output/.config`, если есть);
- все три потока (bsp_generic, milkv-duo-01, universal) теперь штампуют
  EXT2_SIZE в реальный `buildroot/.config`.

**Проверено:** `tests/slim-fns-test.sh` (12 сценариев: позитив, идемпотент,
live-config stamp, 3 негатива с громким FATAL, self-heal) и
`tests/slim-pristine-test.sh` — прогон на ПРАВИЛЬНЫХ апстримных defconfig'ах
(git HEAD трех cvitek_*) → всё легло, assert прошёл.

## 2. sshd стартовал при enabled=no (arm64)

**Причина:** гейт-патч `0031-ssh-gated-S50sshd` есть только в серии
`milkv-duo-01` (rv64 duo01). arm64-флоу применяет `licheerv-nano` +
`milkv-duo-01-arm`, где S50sshd — стоковый buildroot-скрипт: стартует
всегда, при первом старте делает `ssh-keygen -A` (в логе «Starting sshd:
OK» + генерация хост-ключей).

**Фикс:** `patches/licheerv-nano/0030-ssh-gated-S50sshd...patch` — та же
- правка (gated S50sshd + `etc/rnsbox/ssh.conf enabled=no` +
  sshd_config `PermitRootLogin no`), у серий общий апстрим-базис, патч
  байт-в-байт переиспользован. Правильное поведение теперь: enabled=no →
  sshd не стартует никогда, хост-ключи генерятся лениво при первом
  включении (S50sshd делает `ssh-keygen -A` только при enabled).

**Проверено:** `git apply --check` в тёплом arm64-воркспейсе — OK; `git am`
в чистом клоне — OK. На rv64-duo01 воркспейсе патч корректно ОТКАЗЫВАЕТСЯ
применяться (там уже есть свой 0031) — дубля нет.

NB: серия `milkv-duo-cv1800b` гейта не имеет тоже (там openssh=y в
defconfig от того же slim-шага) — в этот заход не трогал, помечено как
последующий бэкпорт (один и тот же файл патча).

## 3. Цепочка admin-юзера

users.txt (`admin -1 admin -1 ! /home/admin /bin/sh`) теперь гарантированно
попадает в defconfig (см. п.1) и валидируется assert'ом. Портальный
`passwd -a sha512 admin` получит существующего юзера. Проверка собранного
rootfs — `board/common/assert-image.sh` (см. п.9).

## 4. rnsd «DOWN при живом pid» + перезапуски

**Разбор:** статус-источник банера — `/run/rnsd-down-since` (владелец —
S46wanwatch, пишет uptime-секунду начала даун-стриика; читатель — портал,
routes_reticulum.cpp). Файл один, семантика однозначная, НО флаг снимал
только 10-секундный тик wanwatch: после рестарта rnsd из ПОРТАЛА (или если
сам wanwatch мёртв) флаг оставался → портал показывает «rnsd DOWN 3 min»
рядом с живым pid. Это и есть ложняк. Реальные перезапуски rnsd на 512MB —
отдельная тема (см. п.8), мониторингу они не противоречат.

**Фикс:** `patches/licheerv-nano/0032-...patch` (копия
`patches/milkv-duo-01/0037-...`): S82rnsd при КАЖДОМ успешном старте
(бут, кик wanwatch, Restart из портала) снимает `/run/rnsd-down-since`;
S46wanwatch при снятии флага пишет в wanwatch.log понятную строку
«rnsd back up — outage streak since N cleared».

## 5. WiFi на arm64 («No WiFi radio detected»)

**Причина:** в arm64 board dts (sg2000_duo01evb_arm64_sd,
sg2002_duo_arm64_sd) узел `wifisd` присутствует только как заглушка из
`soph_base_arm.dtsi` (только interrupt, без compatible/reg/clocks) →
cvitek-sdio драйвер никогда не зондирует AIC8800 → wlan0 не появляется →
S35wifi мягко выходит, портал пишет «No WiFi radio detected». У RV-борды
sg2000_duomodule01_sd полный узел есть — отсюда разница.

**Фикс:** `patches/milkv-duo-01-arm/0015-wifi-aic8800-sdio-...patch`,
по-пакетно:
- sg2000_duo01evb (BGA): полный `&wifisd` (свойства как у вендорских arm
  борд) + `wifi_pin` porta15/porte7 (референс Milk-V Duo S, как у RV
  duomodule01);
- sg2002_duo (QFN): ВАЖНО — soph_asic_qfn.dtsi ДЕЛИТИТ узел
  (`/delete-node/ wifi-sd@4320000`) и несёт свой wifi_pin (porte2/porte6),
  поэтому узел пересоздаётся ЦЕЛИКОМ в board dts (ровно то же делает
  вендорский sg2002_licheea53nano arm-борд), wifi_pin не трогаем;
- `CONFIG_CVI_WIFI_PIN=y` в оба linux defconfig (драйвер `cvitek,wifi-pin`
  — sequencing WL_REG_ON/HOST_WAKE).
Первая версия патча с `&wifisd` на sg2002 падала на dtc («Label or path
wifisd not found») — поймано сборкой, переделано.

**Проверено:** apply-check/am-check; dts пересобирается при сборке (эта
сборка). На Module 01 без радио (не -W) отсутствие остаётся мягким:
S35wifi детектит «no wlanN» и выходит 0.

## 6. BLE на модуле — документирую (не включаю)

- Прошивки для BT есть: пакет aic8800-sdio-firmware возит `fmacfwbt*.bin`.
- Пин-драйвер в ядре есть: `cvitek,bt-pin` (drivers/soc/cvitek/bt_pin/),
  узел `bt_pin` уже в базовом dtsi (poweron porte9) и попадает в собранный dtb.
- НО BT-транспорт в собранных ko ОТСУТСТВУЕТ: в стоковом aic8800_fdrv.ko
  нет ни символов/строк btsdio/hci, ни запроса fmacfwbt (он грузит только
  fmacfw.bin — WiFi-only прошивку). В вендорских исходниках транспорт ЕСТЬ
  (osdrv/extdrv/wireless/aic8800/aic8800_fdrv/aic_btsdio.c + отдельный
  aic8800_btlpm), просто не собран в поставляемые ko.
- Ядро собрано под USB-донгл (CONFIG_BT_HCIBTUSB, патч 0014 arm-серии) —
  донгл-путь работает, BlueZ + ble_reticulum в rootfs есть и без адаптера
  деградируют в no-op.

Итог: BT-железо на -W кремнии есть, программного пути в текущих ko нет.
Включение = пересборка вендорского aic8800-драйвера с BT-конфигом
(aic_btsdio + fmacfwbt + btlpm) и проверка на железе — отдельная задача.

## 7. SLIP-подсказка в портале

`store.cpp` и `pages/reticulum.html` говорили «2 Mbaud только на arm64».
Теперь: точные 2 Mbaud на ВСЕХ rnsbox-платах (reclock 96 MHz), единственное
исключение — классическая LicheeRV Nano-e (25 MHz, использовать 1500000).
(slip.conf-оверлей и store.h уже были актуальны.)

## 8. OOM-наблюдение (без кода, к сведению)

512MB плата: rnsd (python) + nomadnet + echobot + portal. zram-патч есть
только в сериях duo01-rv (0030) и cv1800b (0036), в arm64 серии zram НЕТ —
на arm64-плате свопа нет вовсе. Если падения rnsd — OOM, стоит портировать
S20zram в licheerv/arm серии отдельным шагом (помечено, не делал: не просили
и нужно измерение).

## 9. assert-image.sh

`board/common/assert-image.sh <target-dir> [cvitek|rpi|pc]` — ассерты по
собранному rootfs: admin в passwd + '!' в shadow, root locked, dropbear
отсутствует, sshd есть, S50sshd — гейт-вариант, ssh.conf enabled=no,
PermitRootLogin no, хост-ключей нет в образе, S82rnsd/S60routing/S35wifi/
S31slip на месте, py-only (нет .pyc), slimmer-эффект (ensurepip удалён).
Важно: admin/passwd-проверки читают СОБРАННЫЙ rootfs.ext2/4 через debugfs —
users-таблица применяется на fakeroot-шаге при создании ОБРАЗА, а не в
output/target (стейджинг-дерево admin не содержит, и это норма).
На ТЕКУЩЕМ (сломанном) таргете воркспейса скрипт поймал все 5 реальных дыр —
негативный тест пройден. На свежесобранном — 0 failures. Гнать после каждой
сборки.

## 10. Прочее из списка

- **soph_*.ko insmod-шум** — `patches/milkv-duo-01-arm/0016-...patch`:
  loadsystemko.sh инсмодит каждый модуль только если файл есть в образе
  (порядок и параметры сохранены).
- **dnsmasq двойной старт** — S60routing в `apply()` делал restart
  безусловно: при буте это ПЕРВЫЙ старт (rcS ещё не дошёл до S80dnsmasq),
  dnsmasq поднимался раньше времени, и собственный старт S80 падал с
  «already running FAIL». Фикс `patches/licheerv-nano/0031-...patch`
  (копия `patches/milkv-duo-01/0036-...`): restart только если dnsmasq уже
  работает; бутовый старт — за S80dnsmasq, который идёт следом со свежим
  скоупом.
- **Хостнейм** — проверил цепочку: S10uuid ставит `<prefix>-<hash4>` из
  /device_key_legacy, но `/boot/hostname` ПЕРЕКРЫВАЕТ; портал
  (routes_settings.cpp hostname_set) пишет /etc/hostname + /bin/hostname +
  строку 127.0.1.1 в /etc/hosts + /boot/hostname (иначе ребут вернул бы
  старое). Механизм меняется чисто; дефолт «hostname = имя echobot-ноды» —
  за портал-агентом (S87echobot стартует S10-позже, имя в
  /etc/rnsbox/echobot.conf `name=`, к моменту смены через портал bootstrap
  уже прошёл).

## Сборка/проверка

- Тёплая сборка: `flock ~/rnsbox/.build.lock -c 'cd ~/init-fix &&
  WORKDIR=~/rnsbox/milkv-duo-01-arm RNSBOX_NPROC=6 ./build.sh sg2000-arm64
  lite'` → BUILD-EXIT=0 → `board/common/assert-image.sh
  ~/rnsbox/milkv-duo-01-arm/buildroot/output/target cvitek` → 0 failures.
- Патчи применены в тёплый воркспейс через `git apply` (без коммитов,
  история воркспейса не тронута); git-am-валидация всей пятёрки — в
  throwaway-клоне (/tmp/ws-am-check), все 5 AM-OK.
- Юнит-тесты build.sh: `tests/slim-fns-test.sh`, `tests/slim-pristine-test.sh`.

## 11. Найдено попутно и починено: stale local-site пакеты

Пакеты с SITE_METHOD=local (rnsbox-portal, python-rns, lxmf, nomadnet,
ble-reticulum, bluezero, rnsbox-update) buildroot rsync'ит в
output/build/<pkg>-<ver> ОДИН раз при extract; инкрементальная сборка
радостно пересобирает УСТАРЕВШУЮ копию — свежесобранный образ нёс бинарник
портала недельной давности при уже обновлённых исходниках. build.sh теперь
после синка пакетов сносит их output/build копии (пакеты мелкие: C++ портал
и pure-python колёса — секунды на тёплом хосте). Проверено: после фикса
портал в образе несёт новый SLIP-текст.
