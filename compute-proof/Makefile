# Makefile for the CC compute-proof (client CVM only).
#
# GPU is Blackwell RTX PRO 6000 (sm_120). CUDA CC mode on Blackwell disallows
# PTX JIT at runtime, so native sm_120 SASS is required. Requires CUDA 12.8+.
#
# Override ARCH for a different GPU, e.g. make ARCH=sm_90 (Hopper).

NVCC  ?= nvcc
ARCH  ?= sm_90
SM    := $(ARCH:sm_%=%)
TARGET        = cc_compute_proof
TARGET_CLIENT = cc_compute_proof_client

all: $(TARGET) $(TARGET_CLIENT)

$(TARGET): cc_compute_proof.cu
	$(NVCC) -O2 -gencode arch=compute_$(SM),code=$(ARCH) \
	        -gencode arch=compute_$(SM),code=compute_$(SM) \
	        -o $@ $<

$(TARGET_CLIENT): cc_compute_proof_client.cu
	$(NVCC) -O2 -gencode arch=compute_$(SM),code=$(ARCH) \
	        -gencode arch=compute_$(SM),code=compute_$(SM) \
	        -o $@ $<

clean:
	rm -f $(TARGET) $(TARGET_CLIENT)

.PHONY: all clean
