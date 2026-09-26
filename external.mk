# Pull in every RNSBox package makefile. Adding a package under package/
# is enough — no registration list to maintain.
include $(sort $(wildcard $(BR2_EXTERNAL_RNSBOX_PATH)/package/*/*.mk))

# dnsmasq: the box has no RTC by default and S45ntpsync steps the clock in
# the background after dnsmasq is already up, so leases must time on
# CLOCK_MONOTONIC — otherwise leases handed out at 1970 expire at the step
# and renewals get NAKed. Mirrors the in-tree dnsmasq.mk tweak the lichee
# series carries; external.mk is included after the package makefiles, so
# this appends to the in-tree package's flags without redefining it.
DNSMASQ_COPTS += -DHAVE_BROKEN_RTC
