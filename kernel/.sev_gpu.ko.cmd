savedcmd_sev_gpu.ko := ld -r -m elf_x86_64 -z noexecstack --build-id=sha1  -T /home/martin/AMDSEV/linux/guest/scripts/module.lds -o sev_gpu.ko sev_gpu.o sev_gpu.mod.o .module-common.o
