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
                    │  (musl riscv64, rnsd,    │    (один defconfig
                    │   portal, nomadnet/lxmd  │     cvitek_SG200X_musl_riscv64_defconfig,
                    │   opt-in, без ядра)      │     один overlay, один набор пакетов)
                    └────────────┬─────────────┘
                                 │  один и тот же ext4
      ┌──────────────────────────┼──────────────────────────┐
      ▼                          ▼                          ▼
 fip+uboot+Image(phobos)   fip+uboot+Image(mars)      uboot+Image(mars, sg2000)
 + DTB cv1800b             + DTB sg2000_duomodule01   + DTB sg2002_licheervnano
      │                          │                          │
      ▼                          ▼                          ▼
 rnsbox-…-cv1800b…-lite.img rnsbox-…-sg2000…-lite.img  rnsbox-…-sg2002…-lite.img
```

- **rootfs общий безусловно**: buildroot в этом BSP не собирает ядро
  (`BR2_LINUX_KERNEL is not set`), rootfs — чистый userspace, ядро и модули
  живут в загрузочном разделе (`boot.sd` → `/mnt/system/ko`). Поэтому
  cv1800b/sg2000/sg2002 грузят одно и то же `/sbin/init`, одни и те же
  S-скрипты, rnsd и portal.
- **Kernel — по одному Image на семейство чипов, не на плату**. Это
  ограничение ядра CVITEK: `ARCH_CV180X` и `ARCH_CV181X` — жёсткий Kconfig
  `choice` (`arch/riscv/mach-cvitek/Kconfig`), один Image не может
  поддерживать сразу phobos (CV1800B) и mars (SG2000/SG2002). Внутри
  семейства mars один Image обслуживает все платы: различаются только DTB
  (vendor-механика `kernel-dts` компилирует все `.dts` всех плат семейства;
  memory-map подставляется в DTB при компиляции — per-board pass
  `kernel-dts` + перекомпиляция DTB стоит секунды).
- **Коnfig ядра — надмножество, без пер-платных форков**: defconfig cv1800b
  выведен построчно из семейного RNSBox SG200X-конфига, отличаются только 3
  строки выбора чипа (`ARCH_CV180X=y`, chip="phobos", `ARCH_CV180X_ASIC=y`).
- **Сборочный поток**: vendor-механика уже раскладывает по платам —
  `KERNEL_OUTPUT_FOLDER=build/<board>`, `RAMDISK_OUTPUT_BASE=build/<board>`,
  `install/soc_<board>` — поэтому в ОДНОМ workdir живут оба kernel-билда и
  общий buildroot-output. Сборка фазовая, поштучно возобновляемая
  (`.rnsbox-done-<tag>-<stage>`), см. `rnsbox_univ_stage` в `build.sh`.
- **sg2002 переиспользует sg2000**: O=каталоги линкуются жёсткими ссылками
  (`cp -al`), make пересобирает только `.config`-зависимое и DTB (memmap у
  sg2002 другой — 256M против 512M), затем `build_kernel` перекатывает
  `boot.sd`. osdrv/middleware-модули (`soph_*.ko`) — общие для SG200X и
  приезжают со ссылками.

## Матрица семейства

| Чип | Семейство (kernel choice) | Ядра | Boot-режимы, которые собираем | RAM | RAM-бюджет Linux |
|---|---|---|---|---|---|
| CV1800B | phobos (`ARCH_CV180X`) | 1× C906 RISC-V (A53 нет) | SD (C906 boot) | 64MB DDR2 SIP | ~61MB после рекультивации memmap |
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
  грузится при MemTotal<128MB — это несменяемые (pinned) модули, ~десятки MB.
  Остаются watchdog/RTC (S07kmod2) + pwm/saradc.
- Все opt-in сервисы в OFF по умолчанию: nomadnet/lxmd/rnsh (`enabled=no`),
  автозапуск — только rnsd (S82) + портал.
- `CONFIG_SWAP=y` — подушка: `swapon` файла на SD при пиках RSS
  (осознанный компромисс: износ SD против OOM-убийств; по умолчанию swap
  НЕ включается — только вручную/через конфиг).

Оценка RSS: ядро ~15–20MB (Image LZMA ~10MB + структуры), rnsd (python +
Reticulum) ~25–40MB, portal CGI ~2–5MB на время запроса, uhttpd ~1MB.
Итого в 64MB: init + rnsd + портал — помещается впритык; nomadnet/lxmd
рядом с rnsd — НЕ помещаются (для этого есть 256MB sg2002 и swap).
Проверено эмпирически: этот же rootfs загружен в QEMU с `-m 64M`
(см. ниже) — init, S-скрипты, rnsd и portal живут.

## QEMU-проверка (rootfs — общий для всех плат)

Per-board fsbl/fip/uboot/DTB проверяются только на железе (QEMU `-M virt`
не воспроизводит boot-ROM CVITEK); проверке в QEMU подлежит ровно то, что
общее — rootfs и его init-стек.

- Ядро: buildroot `qemu_riscv64_virt_defconfig` (scratch `O=`), `-j2`.
- Boot: `qemu-system-riscv64 -M virt -m 256M -kernel Image -drive
  file=rootfs.ext4,if=virtio …`; реальный `/sbin/init`, S-скрипты
  (S60routing может деградировать без nftables — допустимо), затем
  `rnsd --version`, portal-CGI и uhttpd-прогон.
- Логи: `~/rnsbox-work/qemu-boot-cv.log` (256M),
  `~/rnsbox-work/qemu-boot-cv-64m.log` (64M — прямой аргумент для CV1800B).
  В git не коммитятся (большие).

Результат: см. «QEMU evidence» в конце файла / в commit-сообщении.

## Прошивка

```sh
# образ платы (пример — cv1800b / Milk-V Duo 64M):
xz -d rnsbox-milkv-duo-cv1800b-lite.img.xz   # если паковали xz
dd if=rnsbox-*-lite.img of=/dev/sdX bs=4M status=progress conv=fsync
# Windows: balenaEtcher / Win32DiskImager
```

- Console: UART0 115200 8N1 (пины платы — см. docs производителя).
- USB: CDC-NCM гаджет — `ssh root@<адрес>` через `enx02…`/dhcp, веб-портал
  `http://rnsbox.local/` (пароль по умолчанию: root/admin — сменить в UI).
- Первый бут: S99resizefs растит rootfs-раздел на весь объём карточки.
- Платы: BOOT-раздел (vfat) содержит fip.bin + boot.sd + usb-флаги;
  ROOTFS — общий ext4.

## Воспроизведение сборки

```sh
git checkout milkv-duo-cv1800b
./build.sh milkv-duo-cv1800b lite          # ~часы; -j2 по умолчанию (RNSBOX_NPROC)
ls */install/soc_*/images/*-lite.img       # по образу на плату
# в WSL: путь /mnt/c непригоден для workdir (drvfs), и PATH с /mnt/c
# вырезается build.sh автоматически.
```

Серия BSP-патчей: `patches/milkv-duo-cv1800b/` (30 штук) — база
`LicheeRV-Nano-Build @ d4003f15`: патчи 0001–0026 унаследованы от ветки
milkv-duo-01-rv (board-dir sg2000 + headless-strip + роутер/портал/rnsd),
0027–0030 — cv1800b-графт, общий rootfs-маппинг CV180X→SG200X, отсечение
multimedia-ko на <128MB, чип-агностичный pack-скрипт.
