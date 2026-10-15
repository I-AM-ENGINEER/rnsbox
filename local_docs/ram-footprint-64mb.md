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

Методика: boot с init=/bin/sh (как в QEMU-валидации потока), ручной монтаж
proc/sys/dev + fstab tmpfs, полный прогон /etc/init.d/rcS, затем поштучно
сервисы. Числа — из гостя (VmRSS/VmHWM через /proc), т.е. это ПРАВДИВЫЕ
userspace-цифры rootfs; они переносятся на реальную плату как есть.

После boot, до rcS: MemTotal 214116 кБ, free 198604 кБ, Slab 7128 кБ,
PageTables 428 кБ. (На 6.18-ядре валидации ~20 МБ висит как Reserved —
артефакт QEMU-машины virt, на плате его нет.)

После rcS (VmRSS, top):
| процесс | RSS |
|---|---|
| ntpd (S49ntp, постоянный демон) | 3628 кБ |
| sshd/dropbear | 1880 кБ |
| udevd | 1376 кБ |
| haveged | 1140 кБ (+600 кБ второй поток) |
| S46wanwatch (резидентный sh-цикл) | 1104 кБ |
| dhcpcd ×4 (мастер+3 сессии) | ~3360 кБ суммарно |
| syslogd | 972 кБ |
| S36wifiwatch (резидентный sh-цикл) | 960 кБ |
| dnsmasq | 924 кБ |
| S84cron (резидентный sh) | 896 кБ |
| klogd | 876 кБ |
| crond | 712 кБ |
| uhttpd (уже поднят rcS) | 416 кБ |
| **Итого used (free)** | **19 016 кБ** |

rnsd (Reticulum 1.5.7, python 3.11), скан по comm `rnsd` через 60 c после
S82rnsd start (оффлайн: AutoInterface без usb0, TCP-клиенты ретраят):

| фаза | VmSize | VmRSS | VmHWM |
|---|---|---|---|
| импорт (пик) | — | — | **39 244 кБ** |
| steady (+60 с) | 34 672 кБ | **31 364 кБ** | 39 244 кБ |
| PSS | | 30 066 кБ | |

Threads: 6. Логи Reticulum при redirect в файл не видны до заполнения
буфера — «пустой» /var/log/rnsd.log при живом демоне это буферизация, не
падение. Методологическая ловушка QEMU-прогонов: в образе, пережившем
предыдущий запуск rnsd, остаётся состояние shared-instance — новый rnsd
молча не стартует; на реальной плате /run — tmpfs и чистится при буте,
артефакт касается только переиспользуемых тестовых образов.

Портал: rnsbox-portal CGI (страница логина, 302) — пик VmHWM 3 956 кБ.
uhttpd после реального HTTP-запроса: 596 кБ (мастер) + 416 кБ (воркер).

`rnsd --version` (тёплый, полный импорт RNS): 3.1–3.9 c под TCG ≈ 0.3–0.5 c
на C906 (TCG ×~10). Импорт-вес: RNS.Reticulum 0.54 c, RNS.Interfaces
(тянет asyncio через I2PInterface — единственный очевидный кандидат на
отложенный импорт, но это правка апстримного RNS — не делаем), site
ничтожeн, pyc уже прекомпилированы (151 для RNS).

### Сводка бюджета 64М (вендор-ядро, расчёт)

- MemTotal платы ≈ 64 МБ − Image (~5.5 МБ после трима) − структуры/сляб
  ядра (~4–5 МБ на 5.10 без tracepoint-гирлянды) ≈ **53–55 МБ**.
- rcS-набор ≈ 19 МБ (incl. page cache, который ужмется) + rnsd 27.7 МБ +
  портал пиково 4 МБ ≈ **47–50 МБ** → работает, но запас < 5 МБ.
- Отсюда цена каждого мегабайта: kernel-трим −1.5 МБ, PYTHONOPTIMIZE −0.7
  МБ, zram как подушка — все три оправданы (ниже).

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
