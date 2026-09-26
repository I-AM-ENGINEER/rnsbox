# Pull in every RNSBox package makefile. Adding a package under package/
# is enough — no registration list to maintain.
include $(sort $(wildcard $(BR2_EXTERNAL_RNSBOX_PATH)/package/*/*.mk))
