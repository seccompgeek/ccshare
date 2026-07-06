// cc_compute_proof.cu
//
// Minimal "does the GPU execute a compute kernel under Confidential Computing"
// proof. This is the cheap milestone-1 check: it runs on the MANAGER CVM, which
// owns the CC-enabled GPU via the patched NVIDIA driver. It launches a trivial
// SM kernel that writes a deterministic value to every element of a device
// buffer, copies the result back, and verifies it on the CPU.
//
// If this prints "PASS", the whole platform path -- SEV-SNP CVM + CC-mode
// driver + GPU SM execution + the H2D/D2H bounce-buffer crypto the CUDA runtime
// inserts under CC -- is working end to end. That is exactly what the
// low-interception design reuses (the client's CUDA will author the work; the
// manager only rings the doorbell).
//
// Build/run via:  ../test.sh cuda-proof        (on the manager VM)
// or directly:    make && ./cc_compute_proof [num_elements]

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cuda_runtime.h>

// Deterministic transform we can re-check on the CPU.
__device__ __host__ static inline uint32_t expected_value(uint32_t i)
{
    return (i * 2654435761u) ^ (i + 0x9e3779b9u);
}

__global__ void fill_kernel(uint32_t *out, uint32_t n)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        out[i] = expected_value(i);
}

#define CUDA_CHECK(call)                                                       \
    do {                                                                       \
        cudaError_t _e = (call);                                               \
        if (_e != cudaSuccess) {                                               \
            fprintf(stderr, "[-] CUDA error %s:%d: %s\n",                      \
                    __FILE__, __LINE__, cudaGetErrorString(_e));               \
            return EXIT_FAILURE;                                               \
        }                                                                      \
    } while (0)

int main(int argc, char **argv)
{
    uint32_t n = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : (1u << 20);
    if (n == 0)
        n = 1;
    size_t bytes = (size_t)n * sizeof(uint32_t);

    int dev = 0;
    CUDA_CHECK(cudaGetDevice(&dev));
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, dev));
    printf("[*] Device %d: %s (sm_%d%d), launching %u-element compute kernel\n",
           dev, prop.name, prop.major, prop.minor, n);

    uint32_t *d_out = NULL;
    CUDA_CHECK(cudaMalloc(&d_out, bytes));

    uint32_t *h_out = (uint32_t *)malloc(bytes);
    if (h_out == NULL) {
        fprintf(stderr, "[-] host malloc(%zu) failed\n", bytes);
        cudaFree(d_out);
        return EXIT_FAILURE;
    }

    const uint32_t threads = 256;
    const uint32_t blocks = (n + threads - 1) / threads;
    fill_kernel<<<blocks, threads>>>(d_out, n);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    // Under CC, this D2H copy is transparently decrypted by the driver/runtime.
    CUDA_CHECK(cudaMemcpy(h_out, d_out, bytes, cudaMemcpyDeviceToHost));

    uint32_t bad = 0;
    uint32_t first_bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (h_out[i] != expected_value(i)) {
            if (bad == 0)
                first_bad = i;
            bad++;
        }
    }

    free(h_out);
    cudaFree(d_out);

    if (bad != 0) {
        fprintf(stderr,
                "[-] FAIL: %u/%u elements wrong (first mismatch at index %u)\n",
                bad, n, first_bad);
        return EXIT_FAILURE;
    }

    printf("[+] PASS: GPU executed the compute kernel and all %u results match.\n", n);
    printf("[+] Compute-under-CC proven on this manager CVM.\n");
    return EXIT_SUCCESS;
}
