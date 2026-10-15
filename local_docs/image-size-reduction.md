# Уменьшение системного образа RNSBox (image-size)

Ветка `image-size`. Дата: 2026-10-15. Задача: уменьшить .img, не теряя
функционал (rnsd/lxmd/nomadnet, портал, ssh, dnsmasq, nftables, WiFi,
SLIP, gadget) и пользовательский опыт.

## Итог (было -> стало)

| Что | Было | Стало | Экономия |
|---|---|---|---|
| CVITEK universal lite .img (sg2000/sg2002/cv1800b) | 216 MiB | **144 MiB** | **-72 MiB (-33%)** |
| - rootfs.ext4 в образе | 200 MiB | 128 MiB | -72 MiB |
| rpi-universal lite .img | 528 MiB | ~208 MiB | ~-320 MiB (-61%) |
| - rootfs.ext4 | 400 MiB | 160 MiB | -240 MiB |
| - boot.vfat | 128 MiB | 48 MiB | -80 MiB |

(цифра rpi — расчётная до пересборки, см. «Состояние» в конце)

## Ключевой факт, на котором всё держится

Заводской размер rootfs НЕ ограничивает пользователя: на первой загрузке
S99resizefs (CVITEK: `parted resizepart 2 -0` + фоновый `resize2fs`) и
S02resizefs (rpi: fdisk-скрипт + онлайн resize2fs) растят раздел и ФС до
размера карточки. До этого момента образ имеет 15-20 МБ свободного места —
достаточно, чтобы система поднялась и доросла. Значит фиксированный размер
rootfs в .img — это ТОЛЬКО размер скачивания, и его можно резать до
«контент + запас».

Паддинга в .img нет: размер = boot.vfat + rootfs.ext4 ровно.

## Карта состава до (P1)

CVITEK universal rootfs (общая для sg2000/sg2002/cv1800b), .img 216 MiB:

- boot.vfat 16 MiB: boot.sd 9.2 МБ + fip.bin 0.4 МБ + usb.* флаги (используется 10 МБ)
- rootfs.ext4 200 MiB: контент 132 МБ, свободно 37 МБ + 10 МБ reserved (5%)
- python3.11 — 73 МБ: .py ~55 МБ + .pyc ~61 МБ реальных байт; pip 14,
  setuptools 7, pkg_resources 2, ensurepip 4, turtledemo/idlelib/
  pydoc_data/lib2to3 ~3, stdlib test ~1
- aic8800 firmware — 5 МБ: ТРИ семейства (aic8800_and_aic8800D80 2.9,
  aic8800DC 0.55, aic8800D80X2 0.77); нужны только файлы plain-8800
- /mnt/system/ko — 5 МБ: hynitron_touch{,_new} 0.73, gspca_main 0.05,
  backlight+pwm_bl 0.045, efivarfs 0.019, tcp_{bic,htcp,westwood} 0.026 —
  ничем не загружаются; usbnet-семейство 0.4 — оставлено (USB-донглы)
- zoneinfo 2 МБ (нет /etc/TZ, портал TZ не использует), htop.png 1 МБ

rpi-universal rootfs, .img 528 MiB:

- boot.vfat 128 MiB: контент 34 МБ (Image 24, start*.elf ~8, overlays 2,
  dtbs ~1.6)
- rootfs.ext4 400 MiB: контент 125 МБ, свободно 217 МБ + 20 МБ reserved
- python3.14 ~44 МБ (site-packages 24: RNS/LXMF/nomadnet/urwid/
  cryptography/psutil/nftables; .pyc-дубль ~22 МБ), kernel modules
  6.12.61-v8 28 МБ, brcm 10 МБ + cypress 6 МБ (радиомодули Pi 0-3 и Pi 4/5)

## Что выкинуто (P2) — и чем это проверено

| Кандидат | MB | Проверка безопасности |
|---|---|---|
| aic8800DC/ + aic8800D80X2/ + *8800d80* firmware | ~3.6 | драйвер собран с CONFIG_AIC_FW_PATH=".../aic8800_sdio/aic8800_and_aic8800D80"; D80/DC/X2 — другие chipid (USB-донглы), на наших платах их нет |
| hynitron_touch{,_new}, gspca_main, backlight, pwm_bl, efivarfs, tcp_{bic,htcp,westwood} .ko | ~0.85 | S00kmod грузит soph_* (в ядре), S35wifi ищет только cfg80211/rfkill/aic; ни один S-скрипт эти .ko не упоминает |
| ensurepip | ~4 | pip уже предустановлен (фича портала), ensurepip нужен только для bootstrap в venv |
| idlelib, turtledemo, turtle, pydoc_data, lib2to3, stdlib test, site-packages tests | ~4 | grep импортов по site-packages/{RNS,LXMF,nomadnet}: ни одного |
| zoneinfo (tzdata) | ~2 | нет /etc/TZ; ни один S-скрипт и портал не трогают TZ |
| htop.png | ~1 | иконка; htop работает без неё |
| .pyc-дубль всего python | ~20 (du) | см. следующий раздел |

## Python: py-only + прогрев кэша (главный неочевидный результат)

Промежуточная сборка с BR2_PACKAGE_PYTHON3_PYC_ONLY показала: байткод
занимает примерно столько же, сколько исходники (61 МБ .pyc против ~55 МБ
.py; pip: 14 МБ .py -> 17 МБ .pyc). «Pyc-only» НЕ уменьшает payload —
python-дерево остаётся ~74 МБ.

Поэтому выбран PY_ONLY (BR2_PACKAGE_PYTHON3_PY_ONLY): в образ едут только
исходники (-20 МБ du на cvitek, -14 МБ на rpi), а дубль .pyc — ровно тот
«мёртвый груз», который разрешено резать. Чтобы первая загрузка не
тормозила (иначе python компилирует каждый импортируемый модуль на лету),
добавлен S51pycompile: один niced фоновый проход `py_compile` по
/usr/lib/python3.*, стартует на S51, rcS никогда не блокирует; python
пишет .pyc атомарно (tmp+rename), гонок с параллельным rnsd нет;
последующие загрузки — с тёплым кэшем. pip при обновлении rnsd сам ставит
.py+.pyc — поведение прежнее.

Цена: ~30-40 МБ записи на SD при первой загрузке (регенерация кэша).
На размер .img и скорость старта rnsd — без влияния.

## Что НЕ тронуто сознательно

- pip 17 / setuptools 7 / pkg_resources 2 (pyc) — кнопка «Update rnsd»
  портала и общий пакетный менеджмент (патчи серии 0018/0019) — это ФИЧА;
- stdlib distutils (2) — нужен setuptools-hack и on-device sdist-сборкам;
- unittest, encodings, lib-dynload — мелко и/или нужно тулзам;
- busybox и весь userspace-софт (strace/tcpdump/tmux/ssh/socat/ntp/
  wpa_supplicant/hostapd/nano/htop...) — полезный софт не выкидываем;
  strip проверен — все бинари стрипнуты buildroot'ом;
- brcm+cypress firmware и полный набор модулей 6.12.61-v8 — универсальность
  rpi-образа (4 поколения плат) — это функционал, не мёртвый груз;
- usbnet/asix/ax88179/cdc_*/rndis .ko — USB-Ethernet донглы.

## Упаковка (P3)

- BR2_TARGET_ROOTFS_EXT2_RESBLKS 5% -> 1% (после resize на карточке это
  невидимо; +12-19 МБ к доступному месту);
- размеры: cvitek lite 200M -> 128M (контент 93 МБ + журнал/иноды + запас;
  buildroot громко падает, если контент перерастёт — не молчаливая
  деградация), rpi rootfs 400M -> 160M, rpi boot.vfat 128M -> 48M (контент
  34 МБ + запас под dvd-флаги и будущий kernel);
- иноды/журнал — авто; в собранном cvitek-образе 32768 инодов на 5705
  файлов (запас более чем вдвое);
- strip всей userspace-обвязки — уже сделан buildroot (проверено `file`
  по всем бинарям >200 КБ);
- squashfs read-only root: ОЦЕНКА, не внедрено. mksquashfs -comp zstd -19
  на cvitek target: 149 МБ -> 49 МБ (3x). Честный трейд: /etc становится
  нередактируемым (портал пишет /etc/rnsbox/*), обновление rnsd = перепаковка
  образа вместо pip install, нужен tmpfs-оверлей и миграция S-скриптов.
  Для SD-платы выгода спорна, UX-потеря реальна. Рекомендация: не делать
  по умолчанию; вернуться при переносе на NOR/eMMC-платы.

## Механика (где что лежит)

- board/rnsbox/slim-target.sh — общий post-build-чистильщик
  (BR2_ROOTFS_POST_BUILD_SCRIPT): удаляет мёртвый груз; идемпотентен,
  только delete;
- board/rnsbox/S51pycompile — прогрев pyc (для BSP-дерева синчит build.sh,
  для rpi лежит в rootfs-overlay);
- build.sh: rnsbox_slim_bsp() — синчит оба скрипта в BSP-дерево, правит
  cvitek_*.defconfig (PY_ONLY, RESBLKS=1, POST_BUILD_SCRIPT) и литерал
  200M в build-rnsbox.sh / build-rnsbox-arm.sh;
- configs/rnsbox_rpi0_2w_64_defconfig: PY_ONLY, EXT2_SIZE 160M, RESBLKS=1,
  slim-target.sh в цепочке POST_BUILD;
- board/raspberrypi/rnsbox-0-2w/genimage.cfg.in: boot.vfat 48M.

Одинаковый механизм для всех портов: BSP-потоки получают всё через
build.sh (без новых патчей в сериях), rpi — правками defconfig/board.

## Риски и откат

- Контент перерос фиксированный размер -> buildroot падает с «Maybe you
  need to increase the filesystem size»: поднять 128M/160M в build.sh /
  defconfig (одно число).
- Новое железо с aic8800 D80-донглом потребует вернуть firmware: убрать
  соответствующие rm в slim-target.sh.
- S51pycompile: если у пользователя read-only SD-переходник или ФС в
  read-only — python просто компилирует в память на каждый импорт
  (медленнее первый старт, как без кэша); не ломается.

## Пересечение с ram-optimization

Runtime-конфиги не тронуты; S51pycompile — фоновая задача, завершается
через минуты после загрузки (RAM-замерам после старта мешать не должна;
если мешает — замерять после её завершения).

## Состояние

- [x] P1 карта состава (выше)
- [x] slim-target.sh + PY_ONLY + RESBLKS + размеры (ветка image-size)
- [x] CVITEK sg2000_duomodule01_lite пересобран: 216 -> 144 MiB; rootfs
      128M: used 93 / free 19+1% reserved / инодов 32768 (файлов 5705);
      S-цепочка 38 скриптов на месте; rnsd/uhttpd/portal-cgi/pip/RNS/
      LXMF/nomadnet в ФС проверены
- [ ] пересборка cv1800b/sg2002 вариантов (тот же rootfs, другие boot-блобы)
- [ ] rpi0-2w пересборка + замер
- [ ] QEMU: rpi raspi3b полный бут (aarch64 исполняется mainline QEMU);
      CVITEK RV64 — xthead-бинари mainline QEMU не исполняет, проверка
      структурная (методика duo01-rv-validate)
