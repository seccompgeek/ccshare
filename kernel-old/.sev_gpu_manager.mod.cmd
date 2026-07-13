savedcmd_sev_gpu_manager.mod := printf '%s\n'   sev_gpu_manager.o | awk '!x[$$0]++ { print("./"$$0) }' > sev_gpu_manager.mod
