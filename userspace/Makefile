# SEV GPU Manager - User-Space Makefile
# Builds client library and example application

CC := gcc
CFLAGS := -Wall -Wextra -O2 -fPIC
CPPFLAGS := -I../kernel
LDFLAGS := -L.

# Source files
CLIENT_SOURCES := sev_gpu_client.c
CLIENT_HEADER := sev_gpu_client.h
EXAMPLE_SOURCES := example_vm_app.c sev_gpu_client.c
EXAMPLE_HEADER := $(CLIENT_HEADER)

# Build targets
CLIENT_OBJ := sev_gpu_client.o
LIBRARY := libsev_gpu.so
EXAMPLE_APP := example_vm_app
RPC_TEST_APP := rpc_test
TUNNEL_TEST_APP := tunnel_selftest
KEYBROKER_APP := keybroker
KMB_TEST_APP := kmb_test
CC_CRYPT_TEST_APP := cc_crypt_selftest
COMPUTE_TEST_APP := compute_test

# Phony targets
.PHONY: all clean help

# Default target
all: $(LIBRARY) $(EXAMPLE_APP) $(RPC_TEST_APP) $(TUNNEL_TEST_APP) $(KEYBROKER_APP) $(KMB_TEST_APP) $(CC_CRYPT_TEST_APP) $(COMPUTE_TEST_APP)

# Build shared library
$(LIBRARY): $(CLIENT_OBJ)
	@echo "[*] Linking shared library: $@"
	$(CC) -shared $(LDFLAGS) -o $@ $^
	@echo "[+] Built: $@ (library)"
	@ls -lh $@

# Build client object
$(CLIENT_OBJ): $(CLIENT_SOURCES) $(CLIENT_HEADER)
	@echo "[*] Compiling: $<"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Build example application
$(EXAMPLE_APP): $(EXAMPLE_SOURCES) $(EXAMPLE_HEADER)
	@echo "[*] Building example app: $@"
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(EXAMPLE_SOURCES)
	@echo "[+] Built: $@ (executable)"
	@ls -lh $@

# Build RM-RPC transport self-test (standalone, no library needed)
$(RPC_TEST_APP): rpc_test.c ../kernel/sev_gpu_manager.h
	@echo "[*] Building RM-RPC test: $@"
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ rpc_test.c
	@echo "[+] Built: $@ (executable)"
	@ls -lh $@

# Build the secure-tunnel self-test (mutual TLS over shared-memory rings)
$(TUNNEL_TEST_APP): tunnel_selftest.c sev_shm_bio.c sev_shm_bio.h sev_shm_ring.h
	@echo "[*] Building secure-tunnel self-test: $@"
	$(CC) $(CFLAGS) -o $@ tunnel_selftest.c sev_shm_bio.c -lssl -lcrypto
	@echo "[+] Built: $@ (executable)"
	@ls -lh $@

# Build the key-delivery broker (TLS key tunnel over the ivshmem BAR)
$(KEYBROKER_APP): keybroker_main.c sev_keybroker.c sev_shm_bio.c \
                  sev_keybroker.h sev_shm_bio.h sev_shm_ring.h \
                  ../kernel/sev_gpu_manager.h
	@echo "[*] Building key broker: $@"
	$(CC) $(CFLAGS) -o $@ keybroker_main.c sev_keybroker.c sev_shm_bio.c -lssl -lcrypto
	@echo "[+] Built: $@ (executable)"
	@ls -lh $@

# Build the sealed-KMB exchange self-test (drives the D4 ioctls)
$(KMB_TEST_APP): kmb_test.c ../kernel/sev_gpu_manager.h
	@echo "[*] Building sealed-KMB test: $@"
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ kmb_test.c
	@echo "[+] Built: $@ (executable)"
	@ls -lh $@

# Build the host-runnable CC crypto known-answer self-test (no device needed)
$(CC_CRYPT_TEST_APP): cc_crypt_selftest.c
	@echo "[*] Building CC crypto self-test: $@"
	$(CC) $(CFLAGS) -o $@ cc_crypt_selftest.c -lssl -lcrypto
	@echo "[+] Built: $@ (executable)"
	@ls -lh $@

# Build the manager-allocated compute-channel test (drives ASSIGN_COMPUTE)
$(COMPUTE_TEST_APP): compute_test.c ../kernel/sev_gpu_manager.h
	@echo "[*] Building compute-channel test: $@"
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ compute_test.c
	@echo "[+] Built: $@ (executable)"
	@ls -lh $@

# Clean build artifacts
clean:
	@echo "[*] Cleaning build artifacts..."
	rm -f $(CLIENT_OBJ) $(LIBRARY) $(EXAMPLE_APP) $(RPC_TEST_APP) $(TUNNEL_TEST_APP) $(KEYBROKER_APP) $(KMB_TEST_APP) $(CC_CRYPT_TEST_APP) $(COMPUTE_TEST_APP)
	@echo "[+] Clean complete"

# Help target
help:
	@echo "SEV GPU Manager - User-Space Build"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build library and example app (default)"
	@echo "  library      - Build only shared library"
	@echo "  example      - Build only example application"
	@echo "  clean        - Remove build artifacts"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Environment variables:"
	@echo "  CC           - C compiler (default: gcc)"
	@echo "  CFLAGS       - Compiler flags (default: -Wall -Wextra -O2 -fPIC)"
	@echo "  CPPFLAGS     - Preprocessor flags (default: -I../kernel)"
	@echo ""
	@echo "Examples:"
	@echo "  make                 # Build everything"
	@echo "  make clean           # Clean artifacts"
	@echo "  make library         # Build library only"
	@echo "  CC=clang make        # Use clang instead of gcc"

# Individual targets for selective builds
.PHONY: library example

library: $(LIBRARY)

example: $(EXAMPLE_APP)
