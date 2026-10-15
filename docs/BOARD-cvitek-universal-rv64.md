# CVITEK RV64: одна универсальная сборка на всё семейство

`./build.sh milkv-duo-cv1800b lite` — универсальная сборка семейства CVITEK
RV64: **один buildroot-rootfs на все платы**, по-платные только загрузочные
артефакты (fip/u-boot/DTB/kernel-Image нужной архитектуры чипа). Цель — чтобы
новая родственная плата требовала только новый каталог платы в BSP, без
нового портирования («чтобы не ебаться с портами потом»).

## Схема

```
                    ┌──────────────────────────┐
                    │  buildroot rootfs.ext4   │  ← собирается ОДИН раз
                    │  (musl riscv64 rv64gc,   │    (один defconfig
                    │   rnsd, portal,          │     cvitek_SG200X_musl_riscv64_defconfig,
                    │   nomadnet/lxmd opt-in)  │     один overlay, один набор пакетов)
                    └────────────┬─────────────┘
                                 │  один и тот же target-дерево
      ┌──────────────────────────┼──────────────────────────┐
      ▼                          ▼                          ▼
 fip+uboot+Image(phobos)   fip+uboot+Image(mars)      uboot+Image(mars, sg2000)
 + DTB cv1800b             + DTB sg2000_duomodule01   + DTB sg2002_licheervnano
      │                          │                          │
      ▼                          ▼                          ▼
 rnsbox-…-cv1800b…-lite.img rnsbox-…-sg2000…-lite.img  rnsbox-…-sg2002…-lite.img
```

- **rootfs общий безусловно**: buildroot в этом BSP не собирает ядро
  (`BR2_LINUX_KERNEL is not set`), rootfs — чистый userspace; ядро и модули
  живут в загрузочном разделе (`boot.sd` → `/mnt/system/ko`). Все платы грузят
  одно и то же `/sbin/init`, одни и те же S-скрипты, rnsd и портал. Каждый
  `pack_rootfs` перезапускает mke2fs, поэтому байты rootfs.sd между платами
  отличаются (UUID/время FS), но содержимое пакетов идентично — все три
  образа собираются из одного `buildroot/output/target`.
- **Kernel — по одному Image на семейство чипов, не на плату**. Это
  ограничение ядра CVITEK: `ARCH_CV180X` и `ARCH_CV181X` — жёсткий Kconfig
  `choice` (`arch/riscv/mach-cvitek/Kconfig`), один Image не может
  поддерживать сразу phobos (CV1800B) и mars (SG2000/SG2002). Внутри
  семейства mars один Image обслуживает все платы: различаются только DTB
  (vendor-механика `kernel-dts` компилирует все `.dts` всех плат семейства;
  memory-map подставляется в DTB при компиляции — per-board pass
  `kernel-dts` + перекомпиляция DTB стоит секунды).
- **Kernel-config — надмножество, без пер-платных форков**: defconfig cv1800b
  выведен построчно из семейного RNSBox SG200X-конфига, отличаются только 3
  строки выбора чипа (`ARCH_CV180X=y`, chip="phobos", `ARCH_CV180X_ASIC=y`).
- **Сборочный поток**: vendor-механика уже раскладывает по платам —
  `KERNEL_OUTPUT_FOLDER=build/<board>`, `RAMDISK_OUTPUT_BASE=build/<board>`,
  `install/soc_<board>` — поэтому в ОДНОМ workdir живут оба kernel-билда и
  общий buildroot-output. Сборка фазовая, поштучно возобновляемая
  (`.rnsbox-done-<tag>-<stage>`), см. `rnsbox_univ_stage` в `build.sh`.
  Нюанс порядка: ramdisk-workspace (`create_ramdisk_folder`) должен
  существовать ДО `build_kernel` (pack_boot кладёт boot.itb именно туда),
  а сам `build_ramboot` требует уже сконфигурированный kernel O= —
  поэтому в универсальном flow workspace создаётся отдельно перед ядром.
- **sg2002 переиспользует sg2000**: O=каталоги копируются целиком (`cp -a`;
  именно копируются, а не хардлинкуются — vendor-make перезаписывает
  `.config` на месте, через общий inode это попортило бы sg2000), make
  пересобирает только `.config`-зависимое и DTB (memmap у sg2002 другой —
  256M против 512M), затем `build_kernel` перекатывает `boot.sd`.
  osdrv/middleware-модули (`soph_*.ko`) — общие для SG200X и приезжают
  с копией.

## Матрица семейства

| Чип | Семейство (kernel choice) | Ядра | Boot-режимы, которые собираем | RAM | RAM-бюджет Linux |
|---|---|---|---|---|---|
| CV1800B | phobos (`ARCH_CV180X`) | 1× C906 RISC-V (A53 нет) | SD (C906 boot) | 64MB DDR2 SIP | ~64MB после рекультивации memmap |
| SG2000 | mars (`ARCH_CV181X`) | C906B (наш boot) / A53 (ветка milkv-duo-module-01) | SD, RISC-V boot | 512MB DDR3 | ~509MB |
| SG2002 | mars (`ARCH_CV181X`) | C906B (наш boot) / A53 | SD, RISC-V boot | 256MB DDR3 | ~253MB |
| CV1812H/CV1813H wevb | mars | C906 | в дереве, но eMMC/SPI-NAND/SPI-NOR — не SD | — | не собираются этим entry (нужен graft по рецепту ниже) |

Платы в дереве BSP, покрываемые универсальной сборкой:
`cv1800b_sophpi_duo_sd` (Milk-V Duo 64M, first-class), `sg2000_duomodule01_sd`
(Milk-V Duo Module 01 EVB / Duo S-модуль), `sg2002_licheervnano_sd`
(LicheeRV Nano). Кроме того, DTB `sg2000_duo_sd`, `sg2002_duo_sd`,
`sg2000_wevb_sd` собираются тем же mars-ядром — новая плата семейства = новый
каталог `build/boards/<chip>/<board>/` + boot-артефакты.

## Что нужно новой родственной плате («портов больше не будет»)

1. Каталог `build/boards/<chip>/<board>/` с 8 файлами (см. sg2000-патч серии:
   top-defconfig, `memmap.py` (размер DRAM + рекультивация ION/FreeRTOS),
   `linux/<board>_defconfig` (копия семейного RNSBox-конфига, chip-строки),
   `dts_riscv/<board>.dts`, `u-boot/{defconfig,cvi_board_init.c,cvitek.h}`,
   `partition/partition_sd.xml`, `config.json`).
2. Добавить имя платы в `RNSBOX_UNIVERSAL_BOARDS` в `build.sh` (и в pass-case,
   если чип нового семейства — но для cv180x/sg200x это не требуется).
3. Больше ничего: rootfs, ядро (внутри семейства), модули, портал, rnsd —
   общие. Fip/DDR-параметры берутся из BSP по чипу автоматически.

## Бюджет 64MB (CV1800B)

Что отдали Linux:
- `memmap.py`: ION 26.8MB + FreeRTOS 0.75MB → 0 (сток отдавал ~42% DRAM
  мультимедиа-буферам, которые headless-роутер не использует). Итог:
  `KERNEL_MEMORY_SIZE = 64MB`.
- S00kmod: multimedia-стек (VI/VPSS/DWA/VO/RGN/TPU/VCODEC/JPEG/IVE) не
  грузится при MemTotal<128MB — это несменяемые (pinned) модули, десятки MB.
  Остаются watchdog/RTC (S07kmod2) + pwm/saradc.
- Все opt-in сервисы в OFF по умолчанию: nomadnet/lxmd/rnsh (`enabled=no`),
  автозапуск — только rnsd (S82) + портал.
- `CONFIG_SWAP=y` в cv1800b-ядре — подушка: `swapon` файла на SD при пиках
  RSS (осознанный компромисс: износ SD против OOM-убийств; по умолчанию swap
  НЕ включается — только вручную).

Оценка RSS: ядро ~15–20MB (Image LZMA ~10MB + структуры), rnsd (python +
Reticulum) ~25–40MB, portal CGI ~2–5MB на время запроса, uhttpd ~1MB.
Итого в 64MB: init + rnsd + портал — помещается; nomadnet/lxmd рядом с rnsd
на 64MB — НЕ помещаются (для этого есть 256MB sg2002 и swap).
Проверено эмпирически: этот же rootfs загружен в QEMU с `-m 64M` — init,
S-скрипты, `rnsd 1.5.7`, CGI портала и uhttpd-раздача живут (см. ниже).

## Портативность userspace (важно!)

Стоковый CVITEK-тулчейн собирает userspace под `rv64gc + v0p7 + xtheadc`
(T-Head расширения C906). Такой rootfs запускается только на CVITEK-кремнии —
`qemu-system-riscv64 -M virt` (чистый RV64GC) падал illegal instruction прямо
в `/init`. Патч 0032 чинит это на уровне defconfig:

- `BR2_TARGET_OPTIMIZATION="-march=rv64gc -mabi=lp64d"` (обёртка тулчейна
  прокидывает флаг в каждую компиляцию);
- `BR2_TARGET_LDFLAGS` без `-mcpu=c906fdv -march=rv64imafdcv0p7xthead`
  (иначе линковка уходила в thead-мультибиблиотеку и тянула thead-код
  libgcc — тег и инструкции возвращались даже при чистых CFLAGS);
- `# BR2_CCACHE is not set` — ключ ccache не включает флаги, инжектируемые
  обёрткой, поэтому после смены ISA кэш отдавал старые thead-объекты.

Плата от этого ничего не теряет (кроме неиспользуемых thead-инструкций),
а образ становится переносимым на любой riscv64 — в этом смысл
универсальной сборки.

## QEMU-проверка (rootfs — общий для всех плат)

Per-board fsbl/fip/uboot/DTB проверяются только на железе (QEMU `-M virt`
не воспроизводит boot-ROM CVITEK); проверке в QEMU подлежит ровно то, что
общее — rootfs и его init-стек.

- Ядро: buildroot `qemu_riscv64_virt_defconfig` (scratch `O=` = out-qemu-rv
  от ветки milkv-duo-01-rv), `-j2`.
- Boot: `qemu-system-riscv64 -M virt -m 256M/-m 64M -kernel Image
  -drive file=rootfs.ext4,if=virtio…`; реальный `/sbin/init` (sysv),
  полный прогон `/etc/init.d/rcS` (все S-скрипты; S60routing деградирует
  без nftables/сети — допустимо), затем продукт:

```
$ rnsd --version
rnsd 1.5.7
$ REQUEST_METHOD=GET QUERY_STRING='page=overview' \
    SCRIPT_NAME=/cgi-bin/portal /usr/sbin/rnsbox-portal
Status: 302 Found
Location: /cgi-bin/portal/login        # первый бут → страница настройки
$ uhttpd -h /www -p 0.0.0.0:8099 -x /cgi-bin &
$ wget -q -O - 'http://127.0.0.1:8099/cgi-bin/portal?page=overview' | head -3
<!doctype html>
<html lang="en">
<head>
```

- Прогнано дважды: `-m 256M` и `-m 64M` — второй прогон это прямой аргумент
  для бюджета CV1800B. Логи: `~/rnsbox-work/qemu-accept-cv.log` (256M) и
  `~/rnsbox-work/qemu-accept-cv-64m.log` (64M); в git не коммитятся.
- Консольный логин (`getty → /etc/console_handler.sh → login`) в QEMU не
  проверяется: это USB-gadget консоль реальных плат, на `-M virt` её нет.
- Заметка: на QEMU без сети S45ntpsync не синхронизирует часы, и S82rnsd по
  дизайну не стартует демоном («gate on /run/clock-synced» — не спамить
  анонсами из 1970 года); сам бинарник rnsd при этом работоспособен
  (`rnsd --version`), на железе с сетью демонт стартует штатно.

## Прошивка

```sh
# образ платы (пример — cv1800b / Milk-V Duo 64M):
xz -d rnsbox-milkv-duo-cv1800b-lite.img.xz   # если паковали xz
dd if=rnsbox-*-lite.img of=/dev/sdX bs=4M status=progress conv=fsync
# Windows: balenaEtcher / Win32DiskImager
```

- Console: UART0 115200 8N1 + USB CDC-NCM гаджет (`ssh root@…`, веб-портал
  `http://rnsbox.local/`).
- Первый бут: S99resizefs растит rootfs-раздел на весь объём карточки.
- BOOT-раздел (vfat) содержит fip.bin + boot.sd + usb-флаги; ROOTFS —
  общий ext4.

## Воспроизведение сборки

```sh
git checkout milkv-duo-cv1800b
./build.sh milkv-duo-cv1800b lite          # -j2 по умолчанию (RNSBOX_NPROC)
ls */install/soc_*/images/*-lite.img       # по образу на плату
# в WSL: путь /mnt/c непригоден для workdir (drvfs), и PATH с /mnt/c
# вырезается build.sh автоматически.
```

Фактическое время чистой сборки на этой машине (-j2, WSL): ~3–4 ч
(uboot/kernel×2 + osdrv + middleware + полный buildroot с host-rust);
перезапуски подхватываются со штампов. Итог artifacts (последний прогон):

```
install/soc_cv1800b_sophpi_duo_sd/images/*-lite.img    217M  (Milk-V Duo 64M)
install/soc_sg2000_duomodule01_sd/images/*-lite.img    217M  (Duo Module 01 EVB)
install/soc_sg2002_licheervnano_sd/images/*-lite.img   217M  (LicheeRV Nano)
```

Серия BSP-патчей: `patches/milkv-duo-cv1800b/` (33 штуки) — база
`LicheeRV-Nano-Build @ d4003f15`:
- 0001–0026 унаследованы от ветки milkv-duo-01-rv (board-dir sg2000 +
  headless-strip + роутер/портал/rnsd);
- 0027 cv1800b-графт (memmap-рекультивация, семейный kernel-config,
  u-boot BOOTDELAY=0, partition xml);
- 0028 общий rootfs: CV180X → SG200X buildroot defconfig/overlay;
- 0029 S00kmod: multimedia-ko не грузятся на платах <128MB;
- 0030 build-rnsbox.sh пакует cv180x-платы тоже;
- 0031 минимальный cv180x ramboot/onekernel cpio (стоковый был
  glibc-рецептом, который musl-base не удовлетворяет);
- 0032 portable-ISA: rv64gc + сброс thead-LDFLAGS + без ccache;
- 0033 перепривязка source-строк общих пакетов (0024 де-вендинг удалил
  их вместе с vendored-копиями — без этого rnsd/портал выпадали из
  rootfs молча).
