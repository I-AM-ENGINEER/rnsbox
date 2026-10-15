# RNSBox — универсальный образ для всех 64-битных Raspberry Pi

Один образ собирается для всех 64-битных плат Raspberry Pi: **Zero 2 W, 3B/3B+/3A+/CM3, 4B/400/CM4/CM4S, 5/500/CM5**. Имя артефакта историческое (`rnsbox-rpi0-2w.img`, сборка `./build.sh rpi0-2w lite` — аргумент `lite/dvd` это внутренний вариант упаковки, в имя файла релиза он больше не входит), но сам образ универсальный.

**ARMv6/ARMv7-платы (Pi 1 / Pi 2 / Zero / Zero W) не поддерживаются и не будут**:
- Pi 1/Zero/Zero W — SoC BCM2835 (ARMv6, ARM11): не исполняет aarch64-код, а образ собирается ровно один, под ARMv8 (`Image`, aarch64-buildroot-toolchain);
- Pi 2 (early rev) — BCM2836 (ARMv7, Cortex-A7): 32-битный, aarch64 тоже не исполняет; отдельной armv7-сборки нет;
- следствие: прошивка не найдёт `kernel=Image` подходящей архитектуры, `[pi02]`-секция config.txt на Zero (не-W) не спасает — ядро просто не стартует.

## Матрица моделей

| Модель | SoC | Ядро/DTB | Чем проверено | Секция config.txt |
|---|---|---|---|---|
| Zero 2 W (и Zero 2) | BCM2710a1 (Cortex-A53) | bcm2710-rpi-zero-2-w.dtb | ядро+rootfs — QEMU raspi3b/raspi3ap (тот же класс SoC); DTB платы — на уровне сборки; «железо» (NCM-гаджет, WiFi, SLIP) — только на реальной плате | `[pi02]` |
| 3B / 3B+ | BCM2837 / 2837B0 | bcm2710-rpi-3-b*.dtb | QEMU raspi3b: ядро+rootfs+init-скрипты | `[pi3]` |
| 3A+ | BCM2837B0 | bcm2837-rpi-3-a-plus.dtb (+ алиас bcm2710-rpi-3-a-plus.dtb) | QEMU raspi3ap: ядро+rootfs+init-скрипты | `[pi3]` |
| 4B / 400 / CM4 / CM4S | BCM2711 | bcm2711-rpi-4-b/400/cm4/cm4s.dtb | только уровень сборки: в QEMU 8.2 нет машины raspi4b (появилась в QEMU ≥9.0; в apt новее нет) | `[pi4]` |
| 5 / 500 / CM5 | BCM2712 | bcm2712-rpi-5-b / bcm2712d0-rpi-5-b / bcm2712-rpi-500 / bcm2712-rpi-cm5-*.dtb | только уровень сборки: машины raspi5 в QEMU нет | `[pi5]` |

## USB-гаджет (NCM, 10.42.0.1) — только Zero 2 W

`dtoverlay=dwc2,dr_mode=peripheral` (USB-C = NCM-гаджет-сеть) стоит **только в секции `[pi02]`** config.txt — и это осознанно, у остальных плат гаджет-способного порта нет:

| Модель | Порт | Гаджет? |
|---|---|---|
| Zero 2 W | USB-C (dwc2, peripheral) | **да** — единственный способ гаджет-LAN: NCM, `10.42.0.1` |
| 3B / 3B+ | micro-USB power | нет — порт питания без линий данных |
| 3A+ | USB-A | нет — единственный порт host-only |
| 4B / 400 / CM4 | USB-C | нет — для образа USB-C только питание; гаджет не поднимается |
| 5 / 500 / CM5 | USB-C (DWC3) | нет — порт host-only |

На всех платах, кроме Zero 2 W, LAN — WiFi-точка и/или Ethernet; консоль — HDMI (`tty1`), у Pi 5 — отладочный UART `ttyAMA10`.

## Ядро — один superset-Image

- База — `arch/arm64/configs/bcm2711_defconfig` из rpi-ядра 6.12 (`raspberrypi/linux @ 21b41014…`) + наш `linux.fragment` (NCM-гаджет, nftables, SLIP, BBR, мост, I2C/RTC, brcmfmac).
- Слияние `bcm2712_defconfig` не требуется: в дереве rpi 6.12 он отличается от `bcm2711_defconfig` только размером страницы (16K против 4K), VA-битами и localversion — **все** драйверы 2712 (RP1/PCIe/GENET/SDIO/dwc3) уже включены в 2711-конфиге. Конфликтов драйверов 2711/2712 в одной сборке нет.
- Размер страницы оставлен 4K: один образ грузится и на 2710/2711, и на 2712 (RPi OS тоже вернула Pi 5 на 4K).
- Собираются все DTB: bcm2710-rpi-zero-2(-w), bcm2710-rpi-3-b(-plus), bcm2837-rpi-3-a-plus, bcm2711-rpi-4-b/400/cm4(-io)/cm4s, bcm2712-rpi-5-b, bcm2712-d-rpi-5-b, bcm2712d0-rpi-5-b, bcm2712-rpi-500, bcm2712-rpi-cm5-* (4 шт.).

## Схема config.txt

Файл читается прошивкой сверху вниз; строки применяются, только если совпал фильтр `[...]`. Один общий `kernel=Image` + `arm_64bit=1`; DTB выбирается прошивкой автоматически по ревизии платы.

- **общая часть**: `kernel=Image`, `arm_64bit=1`, `disable_overscan=1`, `enable_uart=1`, `dtparam=act_led_trigger=heartbeat` (на Pi 5 у ACT LED нет dtparam — строка игнорируется);
- **`[pi02]`** (Zero 2 W): `start_file=start_cd.elf` + `fixup_file=fixup_cd.dat` + `gpu_mem=16` (cut-down прошивка: полный start.elf держит пол GPU ~128 МБ и ARM видит только 384 МБ); `dtoverlay=disable-bt` (PL011 — под SLIP), `dtoverlay=dwc2,dr_mode=peripheral` (USB-C = NCM-гаджет), `dtoverlay=i2c-rtc,ds3231` (опциональные RTC);
- **`[pi3]`** (3B/3B+/3A+/CM3): та же cut-down пара + `gpu_mem=16`;
- **`[pi4]`** (4B/400/CM4/CM4S): `start4cd.elf`/`fixup4cd.dat` + `gpu_mem=16`; USB-C — только питание, гаджет-строк в секции нет (LAN = Ethernet/WiFi-точка);
- **`[pi5]`** (5/500/CM5): пусто, только комментарий. Прошивка Pi 5 живёт в SPI-EEPROM-загрузчике: он сам читает config.txt и грузит ядро/DTB/оверлеи с FAT — файлы start*.elf/fixup*.dat карте не нужны (лежат для остальных моделей).

`cmdline.txt` общий: `root=/dev/mmcblk0p2 rootwait console=ttyAMA10,115200 console=tty1 cma=32MB` — `ttyAMA10` это отладочный UART-разъём Pi 5 (на остальных моделях строка просто никуда не выводит), `tty1` — HDMI-консоль.

## Состав boot-раздела (FAT, 128 МБ)

- `bootcode.bin` — первая стадия для плат до 3B+ включительно (у 4/5 — EEPROM);
- прошивка: `start_cd.elf`/`fixup_cd.dat` (pi≤3), `start4cd.elf`/`fixup4cd.dat` (pi4) — выбираются config.txt; рядом полные `start.elf`/`fixup.dat` и `start4.elf`/`fixup4.dat` как fallback;
- все DTB (см. выше) + алиас `bcm2710-rpi-3-a-plus.dtb` → копия `bcm2837-rpi-3-a-plus.dtb` (прошивка на 3A+ ищет именно имя bcm2710-…, а ядро собирает bcm2837-…);
- `overlays/` — **все** оверлеи, собранные ядром (arm64-сборка строит их через симлинк `arch/arm64/boot/dts/overlays → arch/arm/boot/dts/overlays`), плюс `overlay_map.dtb`/`hat_map.dtb` (карта имён оверлеев по моделям); их копирует post-image-скрипт из каталога сборки ядра — до этого в образ попадали только 2 «случайных» dtbo из репозитория rpi-firmware;
- `Image`, `cmdline.txt`, `config.txt`, флаг-файлы `usb.*` (NCM-гаджет), `hostname`.

## Прошивка (один образ на все модели)

1. `xz -d rnsbox-rpi0-2w.img.xz` (или записывайте xz сразу).
2. Записать на карту от 1 ГБ: `dd if=rnsbox-rpi0-2w.img of=/dev/sdX bs=4M oflag=direct status=progress` (или Raspberry Pi Imager/Etcher).
3. Вставить карту в любую плату из матрицы и включить.
4. Доступ: портал — `http://10.42.0.1/` (login admin/admin — смените пароль; на дашборде будет красный баннер, пока пароль заводской); SSH выключен по умолчанию и включается в портале (Settings → SSH, пароль для пользователя `admin`; root закрыт) — см. docs/SSH-HARDENING.md; сеть: USB-C-гаджет (Zero 2 W, 4B/400/CM4 — NCM, 10.42.0.1), WiFi-точка или Ethernet (3B/3B+/3A+, 5/500/CM5 — Ethernet по GENET), HDMI-консоль `tty1` (login `admin`), у Pi 5 — отладочный UART (JST-SH, 115200).

## Известные особенности по моделям

- **3B/3B+/3A+**: USB-гаджета нет — у 3B/3B+ порт питания без линий данных, у 3A+ единственный USB — host. LAN = WiFi-точка и/или Ethernet.
- **4B/400/CM4**: USB-C — только питание (гаджет-режим образом не поднимается, см. матрицу выше); LAN = Ethernet и/или WiFi-точка.
- **Pi 5/500/CM5**: USB-C — DWC3 (только host), гаджета нет; графика по умолчанию KMS/v3d — «прошивочного» framebuffer-консоли образ не настраивает (headless-сценарий), консоль — отладочный UART `ttyAMA10`/сеть. Загрузка целиком на EEPROM-загрузчике платы.
- **CM4/CM5**: нужны несущие платы (IO board) для SD/eMMC/разъёмов.
- **`cma=32MB`** в cmdline общий для всех моделей; при включении на Pi 4/5 тяжёлой графики его, вероятно, придётся увеличить — headless-сценарию хватает.
- **QEMU**: raspi-машины эмулируют только SoC (прямая загрузка ядра, свой DTB) — обработку config.txt, выбор DTB, применение оверлеев и прошивку start*.elf QEMU не эмулирует; это проверяется только на реальном железе. Проверка в QEMU = ядро + корень + init-скрипты.

## Проверка (валидация)

Прогон в QEMU 8.2 (`-M raspi3b`): прямая загрузка нашего `Image` с нашим же `bcm2710-rpi-3-b.dtb` и корнем из самого образа (sdcard.img). Нюансы QEMU, учтённые при прогоне:

- у внутренних DTB QEMU 8.2 ядро 6.12 не стартует — нужен наш DTB с `-dtb` (заодно это валидирует сам DTB);
- на DTB rpi PL011 = `ttyAMA1` (mini-uart занимает `ttyAMA0`), консоль задавалась `console=ttyAMA1`; в образе `cmdline.txt` использует `console=tty1` — это не QEMU-путь;
- QEMU эмулирует BCM2835-wdt как полный reset машины: `S03watchdog` в тестовом корне (только в копии для QEMU, не в образе) отключался, иначе guest уходит в цикл перезагрузок; на реальном железе скрипт работает штатно;
- для трассировки в тестовый корень подкладывался `init=/sbin/init2` (обёртка, прогоняющая rcS с `sh -x` и передающая управление настоящему `/sbin/init`); образ это не затрагивает.

Результат raspi3b (BCM2837 — тот же класс SoC, что Zero 2 W/3B/3A+): ядро грузится, rootfs (ext4) монтируется, `/sbin/init` → rcS — **все** S-скрипты S01…S86 выполняются по порядку; в том числе: `S30gadget_nic` — «usb0 not present after 5s; gadget likely failed» (в QEMU нет UDC — штатный graceful-путь) и «br-lan up, 10.42.0.1/24»; `S31slip` — disabled (штатно); nftables-правила загружены (OK); `Starting network/dhcpcd/ntpd/crond/sshd: OK`; `S60routing: WAN=auto (eth0 wlan0) LAN=br-lan`; dnsmasq запущен; **`Starting uhttpd (portal): OK (pid 424)`**; **`Starting rnsd: OK (pid 537)`**; далее управление передано настоящему busybox-init — повторный прогон rcS идемпотентен («S81router: uhttpd already running»), система в устойчивом состоянии без перезагрузок. Лог: `~/rnsbox-work/logs/qemu-raspi3b-universal.log` (не коммитится).

Прогон raspi3ap (BCM2837B0, наш `bcm2710-rpi-3-a-plus.dtb`): ядро стартует, «Machine model: Raspberry Pi 3 Model A+» — DTB 3A+ валиден; userspace не доходит: QEMU 8.2 не эмулирует блок питания/доменов BCM2835 для этой машины — `synchronous external abort` в `bcm2835_power_probe` → panic. Это дыра эмуляции QEMU, не дефект образа.

- **Pi 4/400/CM4**: только уровень сборки — машины `raspi4b` нет в QEMU 8.2 (появилась в 9.0; новее в apt этого релиза нет).
- **Pi 5/500/CM5**: только уровень сборки — машины raspi5 в QEMU нет в принципе; загрузочный путь Pi 5 (EEPROM-загрузчик читает config.txt, грузит kernel/DTB/оверлеи с FAT) соответствует официальной документации.
- Обработка config.txt, автоподбор DTB и применение оверлеев прошивкой QEMU не эмулирует — эти уровни проверяются разбором содержимого boot-FAT (см. выше) и железом.
