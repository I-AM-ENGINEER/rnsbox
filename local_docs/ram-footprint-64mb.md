# RAM-футпринт RNSBox на 64М (Milk-V Duo / CV1800B) — измерения и оптимизации

> СТАТУС: работа идёт (ветка ram-optimization). Файл дополняется по мере
> измерений. Все userspace-измерения — QEMU riscv64 (mainline-ядро
> валидации 6.18.7, -M virt) на корневой ФС таргета cvitek-universal;
> kernel-часть — статический анализ + сборка vendor-ядра 5.10 (phobos),
> т.к. вендорское ядро в mainline-QEMU не стартует (машина cvitek там нет).

## Контекст бюджета 64М

- DDR2 64 МБ, C906 ~1 ГГц, одно ядро. memmap.py уже отдал Linux весь DRAM:
  ION 26.8 МБ и FreeRTOS 0.75 МБ рекультивированы (KERNEL_MEMORY_SIZE=64М).
- Multimedia-модули (soph_*.ko) на <128МБ не грузятся (S00kmod) — это были
  десятки МБ несменяемой RAM.
- SWAP НА SD ЗАПРЕЩЁН (износ карточки). Кандидат — zram (см. §zram).

## Baseline (QEMU-валидация, ядро валидации 6.18.7, uncompressed Image 27.4 МБ)

Прогон rcS в -m 64M с ядром валидации: ядро резервирует ~48 МБ из 64 МБ
(«Memory: 15692K/65536K available ... 48004K reserved»; 26.1 МБ — сам образ
ядра + ~22 МБ прочих резервов), MemTotal = 19.9 МБ, после boot свободно
~6.5 МБ. Полный rcS в таких условиях уходит в OOM-спираль (44 убийства:
dhcpcd, rnsd на импорте, rnsbox-portal — все с RSS < 1.5 МБ). Слагаемые
OOM-дампа: slab 7.2 МБ (kernfs_node_cache 2.1 МБ, shmem_inode_cache 1.4 МБ,
tracepoint-структуры ~0.35 МБ — у вендорского 5.10 всё это меньше).

Вывод: 64М-пасс в QEMU с mainline-ядром валидации измеряет НЕ бюджет платы
(вендорский Image — 7.0 МБ против 27.4 МБ, резервы memmap = 0), а поведение
rootfs в условиях жёсткой нехватки. Реальная плата с вендорским ядром имеет
на ~40 МБ больше управляемой памяти, чем QEMU-пасс; rnsd на ней стартует
(подтверждено загрузкой).

Числа userspace сняты в -m 256M (см. ниже) и переносятся на плату как есть.

## Userspace baseline (rootfs таргета, QEMU -m 256M)

(заполняется по завершении прогона)

## Kernel-сторона (vendor 5.10, phobos/CV1800B)

- Воспроизводимость: standalone `make O= ARCH=riscv
  CROSS_COMPILE=<host-tools musl>` из linux_5.10 BSP даёт Image
  **7042560 байт** — байт в байт как в потоке сборки. vmlinux: text
  4 926 352, data 2 109 864, bss 190 021.
- Уже выключено в RNSBox-конфиге: DEBUG_INFO, FTRACE, BPF_JIT, SLUB_DEBUG,
  SCHED_DEBUG, IPV6, MEDIA/VIDEO_CVITEK, SOUND; CMA_SIZE_MBYTES=0;
  CC_OPTIMIZE_FOR_SIZE=y; LOG_BUF_SHIFT=15.
- Резервы: memmap отдаёт всё; CMA=0; главный резидент — сам Image ~7 МБ
  (text+data ~6.8 МБ).
- Тримы (патч 0034, только cv1800b-дефконфиг): KALLSYMS, IKCONFIG(+PROC),
  SQUASHFS(+LZ4/XZ), UBIFS, CFG80211 (на cv1800b нет WiFi-чипа), весь
  legacy iptables/x_tables (S35iptables загружает ПУСТОЙ /etc/iptables.conf;
  весь файрвол — nft, S60routing), conntrack-хелперы AMANDA/H323/...,
  неиспользуемые nftgen'ом NFT_* (нужны только: ct, limit, nat, masq,
  sets ifname, icmp), NET_SCHED+FQ+FQ_CODEL, THERMAL_NETLINK/
  WRITABLE_TRIPS/EMULATION, ELF_CORE/COREDUMP, MODULE_FORCE_LOAD,
  LOG_BUF_SHIFT 15→13.
- **Результат: Image 7 042 560 → 5 504 512 байт (−1.47 МБ, −21.8%)** —
  столько резидентной RAM дополнительно остаётся системе. Сборка с уже
  включёнными ZSMALLOC+ZRAM+CRYPTO_LZ4 (запас под §zram).

## Python / rnsd

- qemu-user прогоны (VmHWM процесса qemu, вкл. ~19.5 МБ оверхед TCG):
  pass=19.5МБ; import RNS: 50.2МБ; PYTHONOPTIMIZE=2: 49.3МБ (−0.8МБ);
  -S: выигрыша нет (site почти пуст); rnsd-entry opt2: −0.66МБ.
  Гостевые RSS — из 256M-прогона.
- pyc: в rootfs уже 151 pyc для RNS (+48 opt-2), 99 у cryptography —
  компиляции на буте нет, precompile уже в сборке.
- nomadnet import падает: unicodedata отсутствует в сборке python (дефект
  rootfs, вне рамок RAM-задачи; lxmd не зависит).
- Shared RNS instance: уже включён (share_instance=Yes) — lxmd/nomadnet
  не плодят второй python-инстанс RNS.

## zram — вердикт

(заполняется)

## Рекомендации в порты по умолчанию

(заполняется)
