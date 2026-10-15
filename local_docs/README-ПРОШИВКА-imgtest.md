# RNSBox — тестовые образы с уменьшенным размером (ветка image-size, сборка 2026-10-15)

Тот же функционал, что в rnsbox-duo-test, но образы ужаты за счёт мёртвого
груза и заводской распухшей ФС. Подробности, карта состава и риски —
local_docs/image-size-reduction.md в репо (ветка image-size).
Первая загрузка дорастит раздел и ФС до размера карточки — как и раньше
(S99/S02resizefs).

## Файлы
| Образ | Плата / режим | Размер (было) | Что проверено | Что проверяется на железе |
|---|---|---|---|---|
| sg2000-duo01evb-rv64-lite.img | Duo Module 01 EVB, RV64 (C906) — основной | 144 MiB (216) | распакована и сверена с эталоном rnsbox-duo-test: S-цепочка init.d идентична (+S51pycompile); rnsd/uhttpd/портал-CGI/wpa_supplicant/hostapd/sshd/dnsmasq/nft/ntp/tcpdump/htop/pip/RNS/LXMF/nomadnet на месте; rootfs 128M: контент 93 МБ, свободно 19+1% | всё как в эталоне: fsbl/DDR, uboot, rnsd, портал, GbE, USB-C gadget, SLIP @ 2M, WiFi AIC8800 |
| cv1800b-duo64m-universal-lite.img | Milk-V Duo 64M (CV1800B) | 144 MiB (216) | та же rootfs-рецептура, boot-блобы cv1800b | fsbl/DDR (64 МБ), uboot |
| sg2002-arm64-lite.img | LicheeRV Nano / SG2002, ARM64 | 144 MiB (216) | та же rootfs-рецептура, boot-блобы sg2002 | то же + SG2002-специфика |
| rpi-universal-lite.img | RPi Zero 2 W / 3 / 4 / 5 (aarch64) | 208 MiB (528) | полный бут в QEMU (virt): вся S-цепочка до конца, «Starting uhttpd (portal): OK», «Starting rnsd: OK», sshd/dnsmasq/cron/ntp OK — лог рядом (qemu-boot-rpi-virt.log). nftables в QEMU не поднимается — у virt-ядра нет netfilter-модулей, ограничение стенда | WiFi brcm/cypress, gadget, SLIP, nftables NAT на родном ядре |

## Что изменилось внутри (кратко)
- python: только исходники (.py), без дублей-байткода; на первой загрузке
  фоновый S51pycompile отстраивает кэш (не блокирует загрузку; на
  последующих — no-op). Замер показал: pyc почти равен .py по размеру,
  так что «pyc-only» образ не уменьшал, а «py-only» отнимает ~20 МБ
- вычищены: лишние семейства прошивок aic8800 (D80/DC/X2 — не наши чипы,
  драйвер смотрит только в aic8800_and_aic8800D80 и только plain-8800
  файлы), тачскрин/вебкам/подсветка .ko, tzdata, ensurepip/idlelib/
  turtledemo/pydoc_data/lib2to3/test-модули, иконка htop
- ext4: резерв root 5% -> 1%; заводской размер rootfs = контент+запас
  (128M у CVITEK / 160M у rpi вместо 200M/400M); boot.vfat rpi 48M вместо 128M
- НЕ тронуто: pip/setuptools/pkg_resources (кнопка «Update rnsd» в портале),
  весь софт (busybox, ssh, tcpdump, tmux, htop...), WiFi-прошивки Pi,
  модули всех поколений, USB-Ethernet донглы, SLIP/gadget-цепочка
- попутный фикс rpi: rcS в оверлее был неexecutable (текущий rnsbox-duo-test
  rpi-образ этого тоже касается — на QEMU бут стопорился на «can't run rcS»);
  в этом образе исправлено (post-build chmod rcS/rcK + 755 в git)

## Прошивка
```bash
xz -d нет (образы несжатые)
sudo dd if=<образ>.img of=/dev/sdX bs=4M conv=fsync status=progress
# или BalenaEtcher
```
Проверка: `sha256sum -c SHA256SUMS`

## После первой загрузки
- CVITEK: консоль UART0, 115200, root/admin, web admin/admin на
  http://10.42.0.1 (USB-C gadget). RV64: SLIP-дефолт 2000000 как в эталоне
- rpi: ПОСЛЕДОВАТЕЛЬНЫЙ порт — это SLIP-линк, логина там НЕТ (так и в
  эталоне); вход по ssh root@10.42.0.1 (admin) или по HDMI-консоли
- первая загрузка чуть дольше: растёт раздел до объёма карточки и в фоне
  собирается python-кэш (S51pycompile; признак работы — появление
  __pycache__ в /usr/lib/python3.* через пару минут)

## Что репортить после теста
dmesg целиком с консоли, `ip a`, `ps | grep rnsd`, `curl -I
http://10.42.0.1`, `ls /usr/lib/python3.11/__pycache__ | head -3` (CVITEK)
или `ls /usr/lib/python3.14/__pycache__ | head -3` (rpi) — по ним я
локализую любой сбой.
