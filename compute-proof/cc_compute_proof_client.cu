// cc_compute_proof_client.cu
//
// Client-CVM counterpart to cc_compute_proof.cu.
//
// Runs on a GPU-less client CVM.  CUDA is forwarded to the manager (which
// holds the real CC-enabled GPU) via the patched NVIDIA driver and
// sev_gpu_manager.ko.  The control plane (RM escapes) travels over ivshmem;
// the data plane (GPFIFO/USERD writes) lands in the client's ivshmem data
// region, which the manager also maps.
//
// The only difference from the manager proof is the explicit flush: CUDA
// writes GP_PUT into the USERD page (a normal memory write), which now maps
// to the ivshmem data region via the mmap redirect in nv-mmap.c.  The
// manager cannot know to ring the GPU doorbell until the client tells it,
// so sev_flush() calls SEV_GPU_IOC_FLUSH_CHANNELS -- the manager iterates
// all assigned compute channels and rings each one.
//
// Build:  make cc_compute_proof_client    (in compute-proof/, on the manager)
//          then scp to the client VM
// Run:    ../test.sh cuda-proof client    (on the client VM, after keybroker)

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cuda_runtime.h>

/* Minimal ioctl shim -- must match sev_gpu_manager.h exactly. */
#define SEV_GPU_IOC_MAGIC       'G'
#define SEV_GPU_IOC_FLUSH_CHANNELS  _IO(SEV_GPU_IOC_MAGIC, 23)
#define SEV_GPU_DEV             "/dev/sev_gpu_manager"

/* Deterministic transform verified on the CPU (same as the manager proof). */
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
            fprintf(stderr, "[-] CUDA error %s:%d: %s (code %d)\n",           \
                    __FILE__, __LINE__, cudaGetErrorString(_e), (int)_e);      \
            return EXIT_FAILURE;                                               \
        }                                                                      \
    } while (0)

/*
 * Ask the manager to ring the GPU doorbell for every compute channel it has
 * assigned to this client VM.  Must be called after the kernel launch (so
 * CUDA has written GP_PUT into the USERD page) and before
 * cudaDeviceSynchronize() (so the GPU actually starts executing).
 */
static int sev_flush(int sev_fd)
{
    if (ioctl(sev_fd, SEV_GPU_IOC_FLUSH_CHANNELS) < 0) {
        perror("[-] SEV_GPU_IOC_FLUSH_CHANNELS");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t n = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : (1u << 20);
    if (n == 0)
        n = 1;
    size_t bytes = (size_t)n * sizeof(uint32_t);

    /* Open the SEV manager device before touching CUDA so we fail fast if the
     * module is not loaded. */
    int sev_fd = open(SEV_GPU_DEV, O_RDWR);
    if (sev_fd < 0) {
        fprintf(stderr, "[-] open(%s): %s\n", SEV_GPU_DEV, strerror(errno));
        fprintf(stderr, "    Is sev_gpu_manager.ko loaded with manager=0?\n");
        return EXIT_FAILURE;
    }

    /* Force explicit CUDA initialization so we get a clear error if the
     * driver or RM escape forwarding is broken, rather than a generic
     * "unknown error" from cudaGetDevice. */
    {
        int ndev = 0;
        CUDA_CHECK(cudaGetDeviceCount(&ndev));
        fprintf(stderr, "[*] cudaGetDeviceCount = %d\n", ndev);
        if (ndev == 0) {
            fprintf(stderr, "[-] No CUDA devices found\n");
            close(sev_fd);
            return EXIT_FAILURE;
        }
    }
    int dev = 0;
    CUDA_CHECK(cudaGetDevice(&dev));
    fprintf(stderr, "[*] cudaGetDevice = %d\n", dev);
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, dev));
    printf("[*] Client CVM -- Device %d: %s (sm_%d%d)\n",
           dev, prop.name, prop.major, prop.minor);
    printf("[*] Launching %u-element compute kernel via manager forwarding\n", n);

    uint32_t *d_out = NULL;
    CUDA_CHECK(cudaMalloc(&d_out, bytes));

    uint32_t *h_out = (uint32_t *)malloc(bytes);
    if (!h_out) {
        fprintf(stderr, "[-] host malloc(%zu) failed\n", bytes);
        cudaFree(d_out);
        close(sev_fd);
        return EXIT_FAILURE;
    }

    const uint32_t threads = 256;
    const uint32_t blocks  = (n + threads - 1) / threads;
    fill_kernel<<<blocks, threads>>>(d_out, n);
    CUDA_CHECK(cudaGetLastError());

    /*
     * Bridge the submission plane: CUDA wrote GP_PUT into the USERD page (now
     * mapped to the ivshmem data region via the mmap redirect).  The manager
     * can't detect this write autonomously, so we ask it to ring the GPU
     * doorbell for all our assigned compute channels.
     */
    if (sev_flush(sev_fd) != 0) {
        free(h_out);
        cudaFree(d_out);
        close(sev_fd);
        return EXIT_FAILURE;
    }
    printf("[*] flush sent -- manager rang GPU doorbell\n");

    CUDA_CHECK(cudaDeviceSynchronize());

    /* Under CC, the D2H copy is transparently decrypted by the runtime. */
    CUDA_CHECK(cudaMemcpy(h_out, d_out, bytes, cudaMemcpyDeviceToHost));

    uint32_t bad = 0, first_bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (h_out[i] != expected_value(i)) {
            if (bad == 0)
                first_bad = i;
            bad++;
        }
    }

    free(h_out);
    cudaFree(d_out);
    close(sev_fd);

    if (bad != 0) {
        fprintf(stderr,
                "[-] FAIL: %u/%u elements wrong (first mismatch at index %u)\n",
                bad, n, first_bad);
        return EXIT_FAILURE;
    }

    printf("[+] PASS: manager executed the kernel, all %u results match.\n", n);
    printf("[+] Cross-VM compute-under-CC proven: client authored work, manager rang doorbell.\n");
    return EXIT_SUCCESS;
}
