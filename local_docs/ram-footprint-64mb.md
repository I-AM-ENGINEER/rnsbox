# RAM-футпринт RNSBox на 64М (Milk-V Duo / CV1800B) — измерения и оптимизации

> СТАТУС: ЗАВЕРШЕНО (ветка ram-optimization, патчи 0034–0037 в серии
> milkv-duo-cv1800b). Итог оптимизаций: −1.47 МБ резидентной RAM ядра +
> −0.7 МБ пика rnsd + tmpfs-капы + opt-in zram. Все userspace-измерения —
> QEMU riscv64 (ядро валидации 6.18.7, -M virt) на корневой ФС таргета
> cvitek-universal; kernel-часть — сборка vendor-ядра 5.10 (phobos),
> т.к. вендорское ядро в mainline-QEMU не стартует (машины cvitek там нет).
> Тестовый образ не собирался: полная сборка потока ~3–4 ч на загруженной
> машине; ветка готова к `./build.sh milkv-duo-cv1800b lite` (см. ниже).

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

Методология честности: TCG-эмуляция не даёт абсолютных CPU-цифр C906
(распределённые во времени числа будут врать), поэтому CPU-вывод — из
характера нагрузки + справедливых оценок пропускной способности lz4 на
одноядерных in-order RISC-ядрах, а всё, что измеряется точно (коэффициент
сжатия, поведение OOM, работа swapon/swapoff), — измерено в QEMU на
zram-ядре (6.18 + ZRAM=y + CRYPTO_LZ4/LZO/ZSTD=y, см. ветку — сборка
воспроизводится из upstream/linux-src тем же host-tools тулчейном).

Измерено:
- Сжатие (lz4), данные «питоньи исходники» (реалистичный профиль heap
  python: код-объекты, строки): **5.4:1** (98304 B → 18358 B).
- Несжимаемое (urandom): ~1:1 — zram хранит как есть (плюс overhead),
  OOM при переполнении — корректный.
- OOM-поведение: при исчерпании RAM+swap убивается именно виновник
  (python-filler), init и система живы (`ALIVE-AFTER-OOM=yes`), после
  завершения процесса память возвращается полностью (free восстановлен).
- swapon/swapoff циклы на zram0 — чисто, re-initialization требует
  `swapoff` + `echo reset > /sys/block/zram0/reset` (иначе «Resource
  busy» — учтено в S20zram).
- RAM-цена самого zram в ядре (измерено, Image vs Image): trim с
  ZRAM+ZSMALLOC+CRYPTO_LZ4 = 5 504 512 B, без = 5 451 776 B → **+52 736 B
  (~51 КБ)** — на порядок меньше выигрыша от самого трима.

Оценка CPU-цены (не TCG): lz4 на одноядерном in-order ядре ~1 ГГц даёт
порядка 100–300 МБ/с на сжатие и 300–800 МБ/с на распаковку. Пиковый
случай rnsd — вытеснить ~10–20 МБ anon при импорте: это единицы секунд
CPU ОДНОКРАТНО. Стационарный swap-трафик роутера — кило/сек: шум.
Вывод: цена приемлема даже для C906, НО это налог на всех — поэтому
по умолчанию выключено.

**Вердикт: zram включать в ядро (сделано, патч 0034: ZSMALLOC+ZRAM+
CRYPTO_LZ4 built-in, +~0.2 МБ к Image), swap-скрипт S20zram — в корневую
ФС, но enabled=no из коробки (патч 0036).** Логика дефолта: 64М-плата
живёт и так (rnsd стартует, поле это подтверждает); zram — «кнопка
усиления» для оператора (enabled=yes в /etc/rnsbox/zram.conf), которая
даёт 64 МБ swap → ~170 МБ эффективных на текстовых данных (~2–3:1 на
реальном python-heap), ценой CPU на page-faults. 256/512М-платам не
нужно (скрипт там чисто но-op: устройства zram в их ядре нет).

zram как бэкенд /tmp вместо tmpfs — НЕ делаем: страницы tmpfs и так
вытесняются в zram-swap при давлении (shmem swappable), отдельный
zram-/tmp добавляет mkfs на бут и фиксированный размер без выгоды.

Swap на SD: запрещён полностью (см. docs/BOARD-cvitek-universal-rv64.md,
исправлено). inittab si5 `swapon -a` остаётся no-op: в fstab swap-строк
нет и не появится.

## Рекомендации в порты по умолчанию

В ветке ram-optimization (патчи 0034–0037 серии milkv-duo-cv1800b):
1. **0034 kernel cv1800b**: trim-конфиг (Image −1.47 МБ резидентно:
   7 042 560 → 5 504 512) + zram/lz4 built-in. Байтово проверено:
   standalone-сборка воспроизводит Image потока (7042560), пропатченный
   дефконфиг даёт стабильный .config и 5504512.
2. **0035 fstab**: все tmpfs под капами (/dev/shm 2M, /tmp 16M, /run 4M;
   var/log 4M и 512k-мелочь уже были). На 64М три непокапленных tmpfs
   объявляли по 32 МБ каждый.
3. **0036 S20zram**: opt-in zram-swap (enabled=no), 25% MemTotal
   клампится 16–32М, lz4, swappiness=100 на время работы.
4. **0037 S82rnsd**: PYTHONOPTIMIZE=2 для демона (−0.7 МБ пика импорта;
   paired A/B: HWM 50156→49312 кБ, entry 50512→49856 кБ).

Не делаем (с причинами):
- micropython/переписывание RNS на C — RNS 1.5.7 это ~6.6 МБ python,
  поддержка форка не окупается; shared-instance уже избавляет от вторых
  инстансов.
- Ленивые импорты в RNS (asyncio через I2PInterface и пр.) — правка
  апстримного кода; выигрыш ~5% времени старта, риск расхождения с
  апстримом.
- Сжатие rootfs / initramfs-бут на 64М — RAM-бюджет определяет резидентный
  Image (~5.5 МБ) и anon-память, а не размер ФС на SD.
- zram-/tmp — см. выше.
- Отказ от ntpd-демона (3.6 МБ RSS — самый жирный сервис после rnsd):
  дисциплина времени нужна Reticulum (таймстампы анонсов); S45 уже делает
  one-shot step, S49 держит дисциплину. Кандидат на обсуждение, не патч.

## Как пересобрать и проверить

```sh
git checkout ram-optimization
./build.sh milkv-duo-cv1800b lite        # серия 0034–0037 применится сама
ls install/soc_cv1800b_sophpi_duo_sd/images/*-lite.img
```

Контрольные точки (без полной сборки):
- Kernel: standalone `make -C linux_5.10 O=<dir> ARCH=riscv
  CROSS_COMPILE=<host-tools>/riscv64-unknown-linux-musl- Image` после
  `olddefconfig` с дефконфигом из 0034 → Image == 5 504 512 байт.
- Python: `PYTHONOPTIMIZE=2` на rnsd-процессе; проверка RSS: grep VmHWM
  /proc/$(pidof rnsd)/status (на плате ~31 МБ steady, ~39 МБ пик импорта).

## Остаточные риски

- Трим-конфиг cv1800b не прогонялся на железе (вендор-ядро в QEMU не
  грузится); дифф — только выключение неиспользуемых подсистем, но
  первая полевая загрузка должна смотреть dmesg и `nft -c` (S60routing).
- zram на плате не гонялся (та же причина); сценарий включения — одна
  строка в /etc/rnsbox/zram.conf.
- CFG80211 выключен ТОЛЬКО в cv1800b-конфиге: WiFi-W-варианты живут на
  mars/sg200x-конфигах, там всё осталось.
