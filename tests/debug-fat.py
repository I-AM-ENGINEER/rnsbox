#!/usr/bin/env python3
"""Debug FatReader against a mkfs.vfat image."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_sysupdate as T

mod = T.load_worker_module()
fat = T.make_fat(T.SysupdateTest.BOOT_FILES)
open("/tmp/testfat.img", "wb").write(fat)
fr = mod.FatReader(fat)
print("bps", fr.bps, "spc", fr.spc, "reserved", fr.reserved, "nfats",
      fr.nfats, "fatsz", fr.fatsz, "rootclus", fr.root_cluster,
      "fat12", fr.fat12, "fat16", fr.fat16)
print("data_off", hex(fr.data_off))
entries = fr.listdir("/")
print("entries:", entries)
