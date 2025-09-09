#include "cuda_runtime.h"
#include "device_launch_parameters.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <iostream>
#include <cublas_v2.h>

extern "C" {
    /*CONVOLUTION*/
    __global__ void kernel_conv_filter(float input[28][28], float pre_output[6][24][24], float weights[6][5][5]) {
        int x = blockIdx.x * blockDim.x + threadIdx.x;
        int y = blockIdx.y * blockDim.y + threadIdx.y;
        int z = blockIdx.z; // For the filter index, which is designed to be max 6 

        if (x < 24 && y < 24) { // Checking if the thread is within the bounds for convolution
            float sum = 0.0f; 
            for (int i = 0; i < 5; ++i) {
                for (int j = 0; j < 5; ++j) {
                    sum += input[x + i][y + j] * weights[z][i][j];
                }
            }
            pre_output[z][x][y] = sum;
        }
    }

    __global__ void kernel_conv_bias(float pre_output[6][24][24], float bias[6]) {

        int x = blockIdx.x * blockDim.x + threadIdx.x;
        int y = blockIdx.y * blockDim.y + threadIdx.y;
        int z = blockIdx.z; //similar, max z is 6

        if (x < 24 && y < 24) {
            pre_output[z][y][x] += bias[z]; //add bias to each element of the pre_output
        }

    };

    // Sigmoid function
    __device__ float sigmoid(float x) {
        return 1.0 / (1.0 + expf(-x));
    }

    __global__ void kernel_conv_sigmoid(float pre_output[6][24][24], float output[6][24][24]) {
        int x = blockIdx.x * blockDim.x + threadIdx.x;
        int y = blockIdx.y * blockDim.y + threadIdx.y;
        int z = blockIdx.z; //I decided to assign 1 block per convolution filter

        if (x < 24 && y < 24) {
            output[z][y][x] = sigmoid(pre_output[z][y][x]);
        }
    }

    /*SUBSAMPLING*/
    __global__ void kernel_ss1_filter(float input[6][24][24], float pre_output[6][6][6], float weight[1][4][4]) {
        int x = blockIdx.x * blockDim.x + threadIdx.x;
        int y = blockIdx.y * blockDim.y + threadIdx.y;
        int z = blockIdx.z;

        //stride 4
        int in_x = x * 4;
        int in_y = y * 4;

        // checking if the calculated indices are within bounds of the output size.
        if (x < 6 && y < 6) {
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    // making sure we don't go out of bounds on the input
                    if ((in_x + i) < 24 && (in_y + j) < 24) {
                        sum += input[z][in_y + j][in_x + i] * weight[0][j][i];
                    }
                }
            }
            pre_output[z][y][x] = sum;
        }
    }

    __global__ void kernel_ss1_bias(float pre_output[6][6][6], float bias[1]) {
        int x = threadIdx.x + blockIdx.x * blockDim.x;
        int y = threadIdx.y + blockIdx.y * blockDim.y;
        int z = blockIdx.z;

        if (x < 6 && y < 6 && z < 6) {
            pre_output[z][y][x] += bias[0];
        }
    }

    __global__ void kernel_ss2_sigmoid(float pre_output[6][6][6], float output[6][6][6]) {
        int x = threadIdx.x + blockIdx.x * blockDim.x;
        int y = threadIdx.y + blockIdx.y * blockDim.y;
        int z = blockIdx.z;

        // Ensure the thread indices are within the bounds of the array
        if (x < 6 && y < 6 && z < 6) {
            output[z][y][x] = sigmoid(pre_output[z][y][x]);
        }
    }


    /*Dense layer*/
    __global__ void kernel_fc1(float input[6][6][6], float pre_output[10], float weight[10][6][6][6]) {
        int neuron = blockIdx.x * blockDim.x + threadIdx.x; // Neuron index for the pre_output
        //since our dense layer has 10 neurons
        if (neuron < 10) {
            float sum = 0.0f;
            //accumulate weighted sum
            for (int z = 0; z < 6; ++z) {
                for (int y = 0; y < 6; ++y) {
                    for (int x = 0; x < 6; ++x) {
                        sum += weight[neuron][z][y][x] * input[z][y][x];
                    }
                }
            }
            //we have to store accumulated sum to each neuron
            pre_output[neuron] = sum;
        }
    }


    __global__ void kernel_fc1_bias(float pre_output[10], float bias[10]) {
        int neuron = threadIdx.x; // I thought that 1 thread per neuron should be enough

        if (neuron < 10) {
            pre_output[neuron] += bias[neuron]; // add bias to pre_output
        }
    }


    __global__ void kernel_fc1_sigmoid(float pre_output[10], float output[10]) {
        int idx = threadIdx.x + blockIdx.x * blockDim.x;

        if (idx < 10) {
            output[idx] = sigmoid(pre_output[idx]);
        }
    }

    void forward_pass(float (*data)[28], float *output, float (*weights)[5][5], float *bias, float (*weights_ss1)[4][4], float *bias_ss1, float (*weights_fc)[6][6][6], float *bias_fc) {
        // Memory allocations and copying
        
        float (*input)[28];
        float (*d_weights)[5][5];
        float (*d_bias);
        float (*d_output)[24][24];
        float (*d_pre_output)[24][24];
        float (*d_pre_output_ss1)[6][6];
        float (*d_weights_ss1)[4][4];
        float (*d_bias_ss1);
        float (*d_output_ss1)[6][6];
        float (*d_weights_fc)[6][6][6];
        float (*d_pre_output_fc);
        float (*d_bias_fc);
        float (*d_output_fc);

		cudaError_t err =cudaMalloc((void**)&input, 28 * 28 * sizeof(float));
        if (err != cudaSuccess) {
            printf("CUDA malloc failed: %s\n", cudaGetErrorString(err));
        }
		cudaMemcpy(input, data, 28 * 28 * sizeof(float), cudaMemcpyHostToDevice);

        cudaMalloc((void**)&d_pre_output, 6 * 24 * 24 * sizeof(float));
		cudaMalloc((void**)&d_output, 6 * 24 * 24 * sizeof(float));
		cudaMalloc((void**)&d_weights, 6 * 5 * 5 * sizeof(float));
		cudaMalloc((void**)&d_bias, 6 * sizeof(float));

        cudaMemcpy(d_weights, weights, 6 * 5 * 5 * sizeof(float), cudaMemcpyHostToDevice);
		cudaMemcpy(d_bias, bias, 6 * sizeof(float), cudaMemcpyHostToDevice);
        
        cudaMalloc((void**)&d_pre_output_ss1, 6 * 6 * 6 * sizeof(float));
		cudaMalloc((void**)&d_weights_ss1, 1 * 4 * 4 * sizeof(float));
        
        cudaMemcpy(d_weights_ss1, weights_ss1, 1 * 4 * 4 * sizeof(float), cudaMemcpyHostToDevice);

        cudaMalloc((void**)&d_bias_ss1, 1 * sizeof(float));
		cudaMemcpy(d_bias_ss1, bias_ss1, 1 * sizeof(float), cudaMemcpyHostToDevice);

		cudaMalloc((void**)&d_output_ss1, 6 * 6 * 6 * sizeof(float));
        cudaMalloc((void**)&d_weights_fc, 10 * 6 * 6 * 6 * sizeof(float));

        cudaMemcpy(d_weights_fc, weights_fc, 10 * 6 * 6 * 6 * sizeof(float), cudaMemcpyHostToDevice);

		cudaMalloc((void**)&d_pre_output_fc, 10 * sizeof(float));
		cudaMalloc((void**)&d_bias_fc, 10 * sizeof(float));

		cudaMemcpy(d_bias_fc, bias_fc, 10 * sizeof(float), cudaMemcpyHostToDevice);

		cudaMalloc((void**)&d_output_fc, 10 * sizeof(float));

        dim3 threadsPerBlock(12, 12); 
		dim3 numBlocks(2, 2, 6); 

		kernel_conv_filter << <numBlocks, threadsPerBlock >> > (input, d_pre_output, d_weights);
		cudaDeviceSynchronize();
        kernel_conv_bias << <numBlocks, threadsPerBlock >> > (d_pre_output, d_bias);
		cudaDeviceSynchronize();
        kernel_conv_sigmoid << <numBlocks, threadsPerBlock >> > (d_pre_output, d_output);
		cudaDeviceSynchronize();
        dim3 threadsPerBlockSS(2, 2); 
		dim3 numBlocksSS(3, 3, 6); 
		kernel_ss1_filter << <numBlocksSS, threadsPerBlockSS >> > (d_output, d_pre_output_ss1, d_weights_ss1);
		cudaDeviceSynchronize();
		kernel_ss1_bias << <numBlocksSS, threadsPerBlockSS >> > (d_pre_output_ss1, d_bias_ss1);
		cudaDeviceSynchronize();
		kernel_ss2_sigmoid << <numBlocksSS, threadsPerBlockSS >> > (d_pre_output_ss1, d_output_ss1);
		cudaDeviceSynchronize();
		kernel_fc1 << <numBlocks, threadsPerBlock >> > (d_output_ss1, d_pre_output_fc, d_weights_fc);
		cudaDeviceSynchronize();
		kernel_fc1_bias << <1, 10 >> > (d_pre_output_fc, d_bias_fc);
		cudaDeviceSynchronize();
		kernel_fc1_sigmoid << <1, 10 >> > (d_pre_output_fc, d_output_fc);
		cudaDeviceSynchronize();
		
		cudaMemcpy(output, d_output_fc, 10 * sizeof(float), cudaMemcpyDeviceToHost);

        cudaFree(input);
        cudaFree(d_pre_output);
		cudaFree(d_output);
		cudaFree(d_weights);
		cudaFree(d_bias);
		cudaFree(d_pre_output_ss1);
		cudaFree(d_weights_ss1);
		cudaFree(d_bias_ss1);
        cudaFree(d_output_ss1);
        cudaFree(d_pre_output_fc);
		cudaFree(d_weights_fc);
		cudaFree(d_bias_fc);
        cudaFree(d_output_fc);
    }
} 
