
// #include <cuda_runtime.h>
// #include <stdio.h>

// __global__ void add(int *a, int *b, int *c) {
//     *c = *a + *b;
// }

// extern "C" int launch_add(int *d_a, int *d_b, int *d_c) {
//     add<<<1, 1>>>(d_a, d_b, d_c);
//     cudaError_t err = cudaDeviceSynchronize(); // Check for kernel errors
//     return (int)err; // Return 0 (cudaSuccess) if no error
// }

#include <cuda_runtime.h>
#include <iostream>
#include <cublas_v2.h>
#include <stdio.h>

extern "C" {
    __global__ void add_kernel(int *a, int *b, int *c) {
        *c = *a + *b;
    }

    // __global__ void conv_kernel(float *input, float *weights, float *output, int width, int height) {
    //     int x = blockIdx.x * blockDim.x + threadIdx.x;
    //     int y = blockIdx.y * blockDim.y + threadIdx.y;
    //     int z = blockIdx.z;  // Filter index

    //     if (x < width - 1 && y < height - 1) {  // Avoid out-of-bounds access
    //         float sum = 0.0f;
    //         for (int i = 0; i < 2; ++i) {
    //             for (int j = 0; j < 2; ++j) {
    //                 sum += input[(x + i) * width + (y + j)] * weights[z * 4 + i * 2 + j];
    //             }
    //         }
    //         output[z * width * height + x * width + y] = sum;
    //     }
    // }
    __global__ void conv_kernel(float input[2][2], float pre_output[2][2][2], float weights[2][2][2],int width, int height) {
        int x = blockIdx.x * blockDim.x + threadIdx.x;
        int y = blockIdx.y * blockDim.y + threadIdx.y;
        int z = blockIdx.z; // For the filter index, which is designed to be max 6 

        if (x < 2 && y < 2) { // Checking if the thread is within the bounds for convolution
            float sum = 0.0f; 
            for (int i = 0; i < 2; ++i) {
                for (int j = 0; j < 2; ++j) {
                    sum += input[x + i][y + j] * weights[z][i][j];
                }
            }
            pre_output[z][x][y] = sum;
        }
    }

    void simple_add(int *a, int *b, int *c) {
        int *d_a, *d_b, *d_c;

        // Allocate memory on the GPU
        cudaMalloc((void**)&d_a, sizeof(int));
        cudaMalloc((void**)&d_b, sizeof(int));
        cudaMalloc((void**)&d_c, sizeof(int));

        // Copy inputs to GPU
        cudaMemcpy(d_a, a, sizeof(int), cudaMemcpyHostToDevice);
        cudaMemcpy(d_b, b, sizeof(int), cudaMemcpyHostToDevice);

        // Launch kernel
        add_kernel<<<1, 1>>>(d_a, d_b, d_c);

        // Copy result back to CPU
        cudaMemcpy(c, d_c, sizeof(int), cudaMemcpyDeviceToHost);

        // Free GPU memory
        cudaFree(d_a);
        cudaFree(d_b);
        cudaFree(d_c);
    }

    // Function to perform matrix multiplication using cuBLAS
    void matrix_multiply(float *A, float *B, float *C, int M, int N, int K) {
        float *d_A, *d_B, *d_C;
        const float alpha = 1.0f;
        const float beta = 0.0f;

        // Allocate device memory
        cudaMalloc((void**)&d_A, M * K * sizeof(float));
        cudaMalloc((void**)&d_B, K * N * sizeof(float));
        cudaMalloc((void**)&d_C, M * N * sizeof(float));

        // Copy matrices from host to device
        cudaMemcpy(d_A, A, M * K * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(d_B, B, K * N * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(d_C, C, M * N * sizeof(float), cudaMemcpyHostToDevice);

        // Create cuBLAS handle
        cublasHandle_t handle;
        cublasCreate(&handle);

        // Perform matrix multiplication: C = alpha * A * B + beta * C
        cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N, M, N, K, &alpha, d_A, M, d_B, K, &beta, d_C, M);

        // Copy result back to host
        cudaMemcpy(C, d_C, M * N * sizeof(float), cudaMemcpyDeviceToHost);

        // Free resources
        cublasDestroy(handle);
        cudaFree(d_A);
        cudaFree(d_B);
        cudaFree(d_C);
    }

    // void conv(float *a, float *b, float *c) {
        // float *d_a, *d_b, *d_c;
    void conv(float (*a)[2], float (*b)[2][2], float (*c)[2][2]){
        float (*d_a)[2], (*d_b)[2][2], (*d_c)[2][2];

        int size_a = 2 * 2 * sizeof(float);    // 2x2 input
        int size_b = 2 * 2 * 2 * sizeof(float); // 2x2x2 filters
        int size_c = 2 * 2 * 2 * sizeof(float); // 2x2x2 output

        // Allocate GPU memory
        cudaMalloc((void**)&d_a, size_a);
        cudaMalloc((void**)&d_b, size_b);
        cudaMalloc((void**)&d_c, size_c);

        // Copy inputs to GPU
        cudaMemcpy(d_a, a, size_a, cudaMemcpyHostToDevice);
        cudaMemcpy(d_b, b, size_b, cudaMemcpyHostToDevice);

        // Launch kernel (1 block, 2x2 threads per block)
        dim3 threadsPerBlock(2, 2);
        dim3 numBlocks(1);
        conv_kernel<<<numBlocks, threadsPerBlock>>>(d_a, d_b, d_c, 2, 2);

        // Copy result back to CPU
        cudaMemcpy(c, d_c, size_c, cudaMemcpyDeviceToHost);

        // Free GPU memory
        cudaFree(d_a);
        cudaFree(d_b);
        cudaFree(d_c);
    }
}