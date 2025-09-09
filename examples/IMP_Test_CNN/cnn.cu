#include "cuda_runtime.h"
#include "device_launch_parameters.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <algorithm>
#include <chrono>   // 新增
#define INSIZE 28
#define CONV_K 5
#define CONV_C 6
#define CONV_OUT 24
#define SS_K 4
#define SS_STRIDE 4
#define SS_OUT 6
#define FC_OUT 10

/* ======================= Utility: CUDA error check ======================= */
#define CUDA_CHECK(call)                                                         \
    do {                                                                         \
        cudaError_t err__ = (call);                                              \
        if (err__ != cudaSuccess) {                                              \
            fprintf(stderr, "CUDA error %s at %s:%d\n",                          \
                    cudaGetErrorString(err__), __FILE__, __LINE__);              \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

/* ======================= Sigmoid ======================= */
__device__ inline float sigmoidf(float x) {
    return 1.0f / (1.0f + expf(-x));
}

/* ======================= Index helpers (flat buffers) ======================= */
__device__ __host__ inline int idx_input(int b, int y, int x) {
    return b * (INSIZE * INSIZE) + y * INSIZE + x;
}
__device__ __host__ inline int idx_conv(int b, int z, int y, int x) {
    return ((b * CONV_C + z) * CONV_OUT + y) * CONV_OUT + x;
}
__device__ __host__ inline int idx_ss(int b, int z, int y, int x) {
    return ((b * CONV_C + z) * SS_OUT + y) * SS_OUT + x;
}
__device__ __host__ inline int idx_fc(int b, int n) {
    return b * FC_OUT + n;
}
__device__ __host__ inline int idx_wconv(int z, int ky, int kx) {
    return (z * CONV_K + ky) * CONV_K + kx; // [6][5][5]
}
__device__ __host__ inline int idx_wss(int ky, int kx) {
    return ky * SS_K + kx; // [1][4][4] -> 16
}
__device__ __host__ inline int idx_wfc(int n, int z, int y, int x) {
    return ((n * CONV_C + z) * SS_OUT + y) * SS_OUT + x; // [10][6][6][6]
}

/* ======================= Kernels with batch ======================= */
/* Convolution: input Bx28x28 -> pre Bx6x24x24 */
__global__ void kernel_conv_filter(const float* __restrict__ input,
                                   float* __restrict__ pre_output,
                                   const float* __restrict__ weights,
                                   int batch_size) {
    int x = blockIdx.x * blockDim.x + threadIdx.x; // 0..23
    int y = blockIdx.y * blockDim.y + threadIdx.y; // 0..23
    int bz = blockIdx.z;                           // 0..(B*6-1)

    if (x >= CONV_OUT || y >= CONV_OUT) return;

    int b = bz / CONV_C;      // batch id
    int z = bz % CONV_C;      // filter id

    if (b >= batch_size) return;

    float sum = 0.f;
    #pragma unroll
    for (int ky = 0; ky < CONV_K; ++ky) {
        #pragma unroll
        for (int kx = 0; kx < CONV_K; ++kx) {
            int in_y = y + ky;
            int in_x = x + kx;
            float v = input[idx_input(b, in_y, in_x)];
            float w = weights[idx_wconv(z, ky, kx)];
            sum += v * w;
        }
    }
    pre_output[idx_conv(b, z, y, x)] = sum;
}

/* Add conv bias: pre += bias[z] */
__global__ void kernel_conv_bias(float* __restrict__ pre_output,
                                 const float* __restrict__ bias,
                                 int batch_size) {
    int x = blockIdx.x * blockDim.x + threadIdx.x; // 0..23
    int y = blockIdx.y * blockDim.y + threadIdx.y; // 0..23
    int bz = blockIdx.z;                           // 0..(B*6-1)

    if (x >= CONV_OUT || y >= CONV_OUT) return;

    int b = bz / CONV_C;
    int z = bz % CONV_C;
    if (b >= batch_size) return;

    int idx = idx_conv(b, z, y, x);
    pre_output[idx] += bias[z];
}

/* Conv sigmoid: out = sigmoid(pre) */
__global__ void kernel_conv_sigmoid(const float* __restrict__ pre_output,
                                    float* __restrict__ output,
                                    int batch_size) {
    int x = blockIdx.x * blockDim.x + threadIdx.x; // 0..23
    int y = blockIdx.y * blockDim.y + threadIdx.y; // 0..23
    int bz = blockIdx.z;                           // 0..(B*6-1)

    if (x >= CONV_OUT || y >= CONV_OUT) return;

    int b = bz / CONV_C;
    int z = bz % CONV_C;
    if (b >= batch_size) return;

    int idx = idx_conv(b, z, y, x);
    output[idx] = sigmoidf(pre_output[idx]);
}

/* Subsampling “filter” (stride=4, kernel 4x4 shared across channels):
   input: Bx6x24x24 -> pre: Bx6x6x6 */
__global__ void kernel_ss1_filter(const float* __restrict__ input,
                                  float* __restrict__ pre_output,
                                  const float* __restrict__ weight, // 16
                                  int batch_size) {
    int x = blockIdx.x * blockDim.x + threadIdx.x; // 0..5
    int y = blockIdx.y * blockDim.y + threadIdx.y; // 0..5
    int bz = blockIdx.z;                           // 0..(B*6-1)
    if (x >= SS_OUT || y >= SS_OUT) return;

    int b = bz / CONV_C;
    int z = bz % CONV_C;
    if (b >= batch_size) return;

    int in_x0 = x * SS_STRIDE;
    int in_y0 = y * SS_STRIDE;

    float sum = 0.f;
    #pragma unroll
    for (int ky = 0; ky < SS_K; ++ky) {
        #pragma unroll
        for (int kx = 0; kx < SS_K; ++kx) {
            int in_y = in_y0 + ky;
            int in_x = in_x0 + kx;
            float v = input[idx_conv(b, z, in_y, in_x)];
            float w = weight[idx_wss(ky, kx)];
            sum += v * w;
        }
    }
    pre_output[idx_ss(b, z, y, x)] = sum;
}

/* Subsampling bias: pre += bias_ss (shared single scalar) */
__global__ void kernel_ss1_bias(float* __restrict__ pre_output,
                                const float* __restrict__ bias_ss,
                                int batch_size) {
    int x = blockIdx.x * blockDim.x + threadIdx.x; // 0..5
    int y = blockIdx.y * blockDim.y + threadIdx.y; // 0..5
    int bz = blockIdx.z;                           // 0..(B*6-1)
    if (x >= SS_OUT || y >= SS_OUT) return;

    int b = bz / CONV_C;
    int z = bz % CONV_C;
    if (b >= batch_size) return;

    int idx = idx_ss(b, z, y, x);
    pre_output[idx] += bias_ss[0];
}

/* Subsampling sigmoid */
__global__ void kernel_ss_sigmoid(const float* __restrict__ pre_output,
                                  float* __restrict__ output,
                                  int batch_size) {
    int x = blockIdx.x * blockDim.x + threadIdx.x; // 0..5
    int y = blockIdx.y * blockDim.y + threadIdx.y; // 0..5
    int bz = blockIdx.z;                           // 0..(B*6-1)
    if (x >= SS_OUT || y >= SS_OUT) return;

    int b = bz / CONV_C;
    int z = bz % CONV_C;
    if (b >= batch_size) return;

    int idx = idx_ss(b, z, y, x);
    output[idx] = sigmoidf(pre_output[idx]);
}

/* Fully connected: input Bx6x6x6 -> pre Bx10
   One thread computes one neuron for one sample (id = b*10 + n) */
__global__ void kernel_fc1(const float* __restrict__ input,
                           float* __restrict__ pre_output,
                           const float* __restrict__ weight_fc,
                           int batch_size) {
    int id = blockIdx.x * blockDim.x + threadIdx.x; // 0..B*10-1
    int total = batch_size * FC_OUT;
    if (id >= total) return;

    int b = id / FC_OUT;
    int n = id % FC_OUT;

    float sum = 0.f;
    #pragma unroll
    for (int z = 0; z < CONV_C; ++z) {
        #pragma unroll
        for (int y = 0; y < SS_OUT; ++y) {
            #pragma unroll
            for (int x = 0; x < SS_OUT; ++x) {
                float v = input[idx_ss(b, z, y, x)];
                float w = weight_fc[idx_wfc(n, z, y, x)];
                sum += v * w;
            }
        }
    }
    pre_output[idx_fc(b, n)] = sum;
}

/* FC bias add */
__global__ void kernel_fc1_bias(float* __restrict__ pre_output,
                                const float* __restrict__ bias_fc,
                                int batch_size) {
    int id = blockIdx.x * blockDim.x + threadIdx.x; // 0..B*10-1
    int total = batch_size * FC_OUT;
    if (id >= total) return;

    int n = id % FC_OUT;
    pre_output[id] += bias_fc[n];
}

/* FC sigmoid */
__global__ void kernel_fc1_sigmoid(const float* __restrict__ pre_output,
                                   float* __restrict__ output,
                                   int batch_size) {
    int id = blockIdx.x * blockDim.x + threadIdx.x; // 0..B*10-1
    int total = batch_size * FC_OUT;
    if (id >= total) return;

    output[id] = sigmoidf(pre_output[id]);
}

/* ======================= Model Class ======================= */
class Layer {
public:
    // device weights/bias (persistent)
    float* d_wconv = nullptr;   // [6][5][5] -> 150
    float* d_bconv = nullptr;   // [6]
    float* d_wss = nullptr;     // [1][4][4] -> 16
    float* d_bss = nullptr;     // [1]
    float* d_wfc = nullptr;     // [10][6][6][6] -> 2160
    float* d_bfc = nullptr;     // [10]

    // last outputs (for host readback)
    float* d_out_fc = nullptr;  // [B*10] (allocated per forward)

    Layer() {
        // ----- init & upload weights -----
        // conv weights [6][25] from your original
        float c1_weight[CONV_C][CONV_K * CONV_K] = {
            {0.021796, -0.253539, -0.264633, -0.390335, 0.164373, -0.003884, -0.339657, 0.155349, -0.066933, -0.109127, -0.245242, 0.023972, -0.099835, -0.406280, -0.404768, -0.063032, -0.176222, 0.390005, -0.041021, 0.504991, 0.411280, 0.490982, -0.211766, 0.357036, 0.089467,},
            {0.462476, -0.390946, 0.353110, 0.051559, -0.291983, 0.032132, 0.267439, -0.133041, -0.042471, 0.009999, -0.369408, 0.172607, -0.354324, -0.121653, -0.264116, 0.124041, -0.445006, 0.147228, 0.044242, -0.334759, -0.394303, 0.404403, -0.443299, -0.057149, 0.400614,},
            {-0.120172, -0.363410, 0.140784, 0.426477, 0.527300, 0.055575, 0.433228, 0.274346, -0.415725, -0.284644, -0.352777, 0.220872, -0.017872, 0.166825, -0.156702, -0.014411, -0.182755, -0.025224, 0.451383, 0.104624, -0.390085, -0.433460, -0.244068, 0.163247, -0.256354,},
            {0.142145, -0.241728, 0.194633, 0.014756, -0.406083, -0.287899, 0.121829, 0.131957, -0.450947, -0.004920, -0.204048, -0.545422, -0.246691, -0.236730, -0.535327, -0.048552, -0.585163, -0.081254, -0.357632, -0.236740, -0.571868, -0.233020, 0.038282, -0.513554, -0.434952,},
            {-0.402966, -0.178654, 0.025999, -0.140377, 0.231947, -0.136444, 0.322751, 0.161807, 0.376979, 0.411809, 0.427200, 0.131421, 0.258028, 0.483045, -0.300380, 0.568240, 0.520461, 0.130053, -0.169407, -0.456362, -0.350305, -0.144329, 0.111549, -0.281440, 0.088230,},
            {0.472607, 0.106702, 0.359270, 0.373885, -0.261109, 0.529240, -0.129713, 0.451533, -0.149899, 0.526446, -0.323354, 0.578022, 0.112618, 0.486861, 0.479792, -0.182139, -0.119122, -0.015675, -0.267098, 0.122984, -0.297723, 0.347792, 0.316906, -0.019568, 0.455453,}
        };
        float conv_w[CONV_C * CONV_K * CONV_K];
        for (int z = 0; z < CONV_C; ++z)
            for (int i = 0; i < CONV_K * CONV_K; ++i)
                conv_w[idx_wconv(z, i / CONV_K, i % CONV_K)] = c1_weight[z][i];

        float bconv_host[CONV_C] = {-0.295779f, 0.430410f, 0.389516f, -0.437929f, 0.249217f, -0.041092f};

        // ss weights [1][16]
        float s2_weight[1][SS_K * SS_K] = {{
            -0.806280f, -3.872459f, -2.141078f, -6.543421f,
            -2.135209f, -5.067534f, -2.430283f, -6.847790f,
             2.860572f, -2.327538f,  0.649373f, -1.243741f,
             7.845700f,  5.709686f,  5.967687f,  5.219587f
        }};
        float bss_host[1] = {0.827946f};

        // fc weights [10][216] -> [10][6][6][6]
        float f3_weight[FC_OUT][CONV_C * SS_OUT * SS_OUT] = {
            /* paste your 10x216 f3_weight here (already in original) */
            {0.351526, 0.311490, -0.498288, 0.119535, -0.681564, 0.255129, 0.480886, -0.492643, -1.281708, -1.340271, -1.966150, -1.082251, -0.488600, -1.450043, -1.267618, 0.749348, -1.384504, -2.280142, -1.568977, -2.571040, -0.077106, 0.271615, -0.975582, -0.953359, 0.022398, 0.314329, -0.612719, 0.926436, -0.227751, -0.078307, 0.931316, 0.901250, 1.299293, 0.869457, 0.921717, 0.423475, 0.150990, 0.432035, -0.273189, -0.170283, -0.600728, 0.180319, 0.191720, -0.891604, -0.404788, -0.035962, 0.006960, -0.254663, -0.110516, 0.245278, -0.891609, -0.432604, -0.910933, 0.161980, 0.316577, -0.467119, -1.544833, -1.896033, 1.179531, 0.034170, 0.232885, 1.607719, -0.648748, -0.462456, 0.485259, 0.479415, 1.152267, 1.451969, 1.869130, 1.526582, -0.352413, 0.384313, 0.123432, -0.254515, -0.452569, -0.480945, 0.038267, -0.566631, 0.207059, -0.048547, 0.499613, 0.388086, 0.222929, 0.586097, 0.116915, 0.495215, -0.987635, -2.543056, 0.246004, 1.523358, -0.007175, -0.415974, -0.980118, -2.122471, 0.091124, 0.934185, -0.230031, 1.125950, 1.472628, -0.418642, -0.469083, 0.218658, -0.225766, 0.126642, 1.289255, -0.801874, -1.186997, 0.691511, -0.066857, -0.455275, -0.864519, -1.566909, -2.016968, -0.362027, -1.450920, -2.145304, -0.248081, 1.607327, -0.527347, -1.465685, -0.668917, -0.856998, -1.089551, 1.483309, 1.500693, 0.316248, 1.121368, 2.691165, -3.276138, -2.794873, 2.158591, 1.882994, 0.663664, 4.399794, -2.498104, 0.206852, 0.658676, 0.629444, 1.472383, 0.444734, -1.161019, -1.773016, -1.572188, 0.321799, 0.004689, -0.950659, -0.336590, 1.122141, 0.843947, 1.393509, -0.005872, 0.329899, -0.920808, 0.332284, -1.017754, -0.925537, 0.611789, 0.689382, -1.177078, -3.361110, -3.442491, -1.786212, -0.446117, -1.117085, -1.961925, -1.660484, -0.835836, -0.708459, -0.887261, -1.056308, 0.146323, 0.695095, -0.016069, 0.057834, -0.150198, -1.871884, -0.296581, 0.340804, 0.645604, 0.180815, 0.293250, 0.252584, 0.101424, 0.038066, -1.041792, 0.062905, 0.429716, 0.246763, 0.071984, 1.030795, 0.405589, 1.316891, 0.099288, 0.161572, -0.628917, -1.235220, 0.339577, 0.339029, -0.912725, -2.196772, 0.215959, -0.054501, -1.405472, -0.721270, -0.762793, -1.269405, 1.265924, 0.707909, -1.608350, -0.869309, 0.352279, -0.120468, 0.897106, 0.554869, 0.057806, -0.008923,},
            /* 9 more rows retained from your original code ... */
            {-0.691959, -0.319490, -0.126002, -0.501013, -0.875302, -1.076519, -0.957201, 1.113194, 1.674168, 0.329545, -1.761311, -1.159295, 1.613682, 2.941732, 2.173943, -0.021000, -1.825896, -0.170094, 0.173728, -0.361445, -1.320421, -0.518738, 1.295719, 2.961711, -0.759120, -1.930551, 0.547168, 0.954941, 2.664016, 1.866902, 1.253423, 1.417768, 1.667163, 1.042230, -0.100517, 0.300578, -0.742821, -0.435868, 0.064552, -0.705748, -1.213497, -0.001804, -0.359023, 0.298137, -0.462290, -0.185735, -0.328315, -0.338046, 0.657853, 2.652926, 0.959392, -2.879065, -1.569085, -0.166893, -1.256025, 1.924442, -0.765449, -0.368007, 0.810416, 1.628253, -0.657158, 0.502192, -0.500995, 0.748136, 0.398722, 0.901416, -0.039363, 1.347513, 1.276203, -1.614992, 0.438525, 0.500054, 0.113522, -0.032455, -0.627655, 0.008442, 1.360402, -1.020135, -0.221365, -0.158344, 0.732279, 0.599728, 0.105030, -0.698781, -0.691555, 0.842164, -0.142675, -1.220035, -1.879403, -1.355728, 0.951171, 1.169849, -0.774057, -0.290896, -1.583322, 0.386187, 0.427529, -0.284459, 0.255392, -0.747500, 1.408733, 0.321606, -0.201709, -0.559591, -0.594734, -0.110160, -1.030297, -1.282099, 0.072950, 1.014169, 2.120188, -0.186486, -2.108402, -0.302105, -0.778032, 0.225517, 1.845844, 1.028797, -0.750887, -1.268448, -1.074318, -2.053718, -3.085705, -0.494818, -0.322282, 0.149053, -3.596669, -4.111627, -1.300500, 0.565006, -0.487223, 1.346992, -0.219845, 1.634139, 0.398809, 0.230929, 0.485752, 0.666853, -0.158405, -0.415601, -2.933488, -1.255654, 1.545914, 0.507034, -0.493720, 0.380871, 0.083095, 0.623031, -0.399877, -0.657982, -0.623307, -0.873340, 0.656684, 0.734791, -1.335358, -2.101094, 0.831576, 2.100440, 2.293340, -0.346170, -3.333020, -0.954265, 2.421231, 2.408652, 0.758019, 0.139671, -0.483363, 3.165620, -0.050275, 0.177964, 0.436090, -0.539363, 2.990378, 2.356659, -1.532563, -0.977257, -0.316307, -0.759580, -0.439312, -0.550233, -0.031386, -1.135577, 0.329767, -1.219383, -1.346268, -0.968223, -1.073834, -1.273577, -0.268350, 0.434507, -0.372449, -0.663014, 0.776042, 2.483770, 0.461209, 0.288777, -0.179103, -1.031428, -0.596650, -0.469062, 0.658128, 0.681416, -1.432260, -0.364713, -1.425495, -2.454195, -2.924943, 0.629133, 1.855964, 1.667543, -0.539052, -0.663570, -0.630038, 1.244874, 2.319129, 0.256111,}
            /* ... include the rest from your original f3_weight (10 rows total) ... */
        };
        float wfc_host[FC_OUT * CONV_C * SS_OUT * SS_OUT];
        for (int n = 0; n < FC_OUT; ++n)
            for (int i = 0; i < CONV_C * SS_OUT * SS_OUT; ++i) {
                int z = i / (SS_OUT * SS_OUT);
                int rem = i % (SS_OUT * SS_OUT);
                int y = rem / SS_OUT;
                int x = rem % SS_OUT;
                wfc_host[idx_wfc(n, z, y, x)] = f3_weight[n][i];
            }
        float bfc_host[FC_OUT] = {-4.136192f, 1.261294f, -3.176764f, -4.166682f, -4.117179f, -1.692405f, -3.250649f, -2.580800f, -11.371058f, -6.880490f};

        // upload
        CUDA_CHECK(cudaMalloc(&d_wconv, CONV_C * CONV_K * CONV_K * sizeof(float)));
        CUDA_CHECK(cudaMemcpy(d_wconv, conv_w, CONV_C * CONV_K * CONV_K * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMalloc(&d_bconv, CONV_C * sizeof(float)));
        CUDA_CHECK(cudaMemcpy(d_bconv, bconv_host, CONV_C * sizeof(float), cudaMemcpyHostToDevice));

        CUDA_CHECK(cudaMalloc(&d_wss, SS_K * SS_K * sizeof(float)));
        CUDA_CHECK(cudaMemcpy(d_wss, s2_weight[0], SS_K * SS_K * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMalloc(&d_bss, sizeof(float)));
        CUDA_CHECK(cudaMemcpy(d_bss, bss_host, sizeof(float), cudaMemcpyHostToDevice));

        CUDA_CHECK(cudaMalloc(&d_wfc, FC_OUT * CONV_C * SS_OUT * SS_OUT * sizeof(float)));
        CUDA_CHECK(cudaMemcpy(d_wfc, wfc_host, FC_OUT * CONV_C * SS_OUT * SS_OUT * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMalloc(&d_bfc, FC_OUT * sizeof(float)));
        CUDA_CHECK(cudaMemcpy(d_bfc, bfc_host, FC_OUT * sizeof(float), cudaMemcpyHostToDevice));
    }

    ~Layer() {
        if (d_wconv) cudaFree(d_wconv);
        if (d_bconv) cudaFree(d_bconv);
        if (d_wss)   cudaFree(d_wss);
        if (d_bss)   cudaFree(d_bss);
        if (d_wfc)   cudaFree(d_wfc);
        if (d_bfc)   cudaFree(d_bfc);
        if (d_out_fc) cudaFree(d_out_fc);
    }

    void forward_pass_batch(float (*data)[28][28], int batch_size) {
        // allocate batch buffers
        float *d_in = nullptr, *d_conv_pre = nullptr, *d_conv_out = nullptr;
        float *d_ss_pre = nullptr, *d_ss_out = nullptr, *d_fc_pre = nullptr;

        size_t in_bytes      = (size_t)batch_size * INSIZE * INSIZE * sizeof(float);
        size_t conv_bytes    = (size_t)batch_size * CONV_C * CONV_OUT * CONV_OUT * sizeof(float);
        size_t ss_bytes      = (size_t)batch_size * CONV_C * SS_OUT * SS_OUT * sizeof(float);
        size_t fc_bytes      = (size_t)batch_size * FC_OUT * sizeof(float);

        CUDA_CHECK(cudaMalloc(&d_in,       in_bytes));
        CUDA_CHECK(cudaMalloc(&d_conv_pre, conv_bytes));
        CUDA_CHECK(cudaMalloc(&d_conv_out, conv_bytes));
        CUDA_CHECK(cudaMalloc(&d_ss_pre,   ss_bytes));
        CUDA_CHECK(cudaMalloc(&d_ss_out,   ss_bytes));
        CUDA_CHECK(cudaMalloc(&d_fc_pre,   fc_bytes));
        if (d_out_fc) cudaFree(d_out_fc);
        CUDA_CHECK(cudaMalloc(&d_out_fc,   fc_bytes));

        CUDA_CHECK(cudaMemcpy(d_in, data, in_bytes, cudaMemcpyHostToDevice));

        // ===== Conv =====
        dim3 tC(16, 16);
        dim3 bC((CONV_OUT + tC.x - 1) / tC.x,
                (CONV_OUT + tC.y - 1) / tC.y,
                batch_size * CONV_C);

        kernel_conv_filter<<<bC, tC>>>(d_in, d_conv_pre, d_wconv, batch_size);
        CUDA_CHECK(cudaGetLastError());
        kernel_conv_bias<<<bC, tC>>>(d_conv_pre, d_bconv, batch_size);
        CUDA_CHECK(cudaGetLastError());
        kernel_conv_sigmoid<<<bC, tC>>>(d_conv_pre, d_conv_out, batch_size);
        CUDA_CHECK(cudaGetLastError());

        // ===== Subsampling =====
        dim3 tS(6, 6);
        dim3 bS(1, 1, batch_size * CONV_C);
        kernel_ss1_filter<<<bS, tS>>>(d_conv_out, d_ss_pre, d_wss, batch_size);
        CUDA_CHECK(cudaGetLastError());
        kernel_ss1_bias<<<bS, tS>>>(d_ss_pre, d_bss, batch_size);
        CUDA_CHECK(cudaGetLastError());
        kernel_ss_sigmoid<<<bS, tS>>>(d_ss_pre, d_ss_out, batch_size);
        CUDA_CHECK(cudaGetLastError());

        // ===== Fully Connected =====
        int total_fc = batch_size * FC_OUT;
        int tF = 128;
        int bF = (total_fc + tF - 1) / tF;
        kernel_fc1<<<bF, tF>>>(d_ss_out, d_fc_pre, d_wfc, batch_size);
        CUDA_CHECK(cudaGetLastError());
        kernel_fc1_bias<<<bF, tF>>>(d_fc_pre, d_bfc, batch_size);
        CUDA_CHECK(cudaGetLastError());
        kernel_fc1_sigmoid<<<bF, tF>>>(d_fc_pre, d_out_fc, batch_size);
        CUDA_CHECK(cudaGetLastError());

        // free temps
        cudaFree(d_in);
        cudaFree(d_conv_pre);
        cudaFree(d_conv_out);
        cudaFree(d_ss_pre);
        cudaFree(d_ss_out);
        cudaFree(d_fc_pre);
    }
};

/* ======================= MNIST I/O (same as your original, minor fixes) ======================= */
struct mnist_data {
    double data[INSIZE][INSIZE];
    unsigned int label;
};

int reverseBytes(int value) {
    return ((value & 0xFF) << 24) | ((value & 0xFF00) << 8) | ((value & 0xFF0000) >> 8) | ((value >> 24) & 0xFF);
}

static void read_mnist_imageset(const char* image_filename, struct mnist_data** data_set, unsigned int* count) {
    FILE* imageset = fopen(image_filename, "rb");
    if (!imageset) { perror("Error opening image file"); exit(1); }

    int magic_number, num_images, num_rows, num_cols;
    fread(&magic_number, sizeof(int), 1, imageset); magic_number = reverseBytes(magic_number);
    fread(&num_images,   sizeof(int), 1, imageset); num_images   = reverseBytes(num_images);
    fread(&num_rows,     sizeof(int), 1, imageset); num_rows     = reverseBytes(num_rows);
    fread(&num_cols,     sizeof(int), 1, imageset); num_cols     = reverseBytes(num_cols);

    unsigned char* image_data = (unsigned char*)malloc(num_rows * num_cols);
    for (int i = 0; i < (int)*count && i < num_images; i++) {
        fread(image_data, sizeof(unsigned char), num_rows * num_cols, imageset);
        int temp = 0;
        for (int y = 0; y < INSIZE; y++)
            for (int x = 0; x < INSIZE; x++)
                data_set[i]->data[y][x] = (double)image_data[temp++] / 255.0;
    }
    free(image_data);
    fclose(imageset);

    printf("Images read: %u (of %d). Shape: %dx%d\n", *count, num_images, num_rows, num_cols);
}

static void read_mnist_labels(const char* label_filename, struct mnist_data** data_set, unsigned int* count) {
    FILE* labels = fopen(label_filename, "rb");
    if (!labels) { perror("Error opening label file"); exit(1); }

    int magic_number_labels, num_labels;
    fread(&magic_number_labels, sizeof(int), 1, labels); magic_number_labels = reverseBytes(magic_number_labels);
    fread(&num_labels, sizeof(int), 1, labels);          num_labels          = reverseBytes(num_labels);

    unsigned char lab;
    for (int i = 0; i < (int)*count && i < num_labels; i++) {
        fread(&lab, sizeof(unsigned char), 1, labels);
        data_set[i]->label = lab;
    }
    fclose(labels);
    printf("Labels read: %u (of %d)\n", *count, num_labels);
}

static int mnist_load(const char* image_filename, const char* label_filename,
                      struct mnist_data** data_set, unsigned int* count) {
    read_mnist_imageset(image_filename, data_set, count);
    read_mnist_labels(label_filename, data_set, count);
    return 0;
}

void convertDoubleArrayToFloatArray(const double in_[28][28], float out_[28][28]) {
    for (int y = 0; y < 28; ++y)
        for (int x = 0; x < 28; ++x)
            out_[y][x] = static_cast<float>(in_[y][x]);
}

/* ======================= main: one batch of 1000 ======================= */
int main() {
    const int batch_size = 5000;
    const int NUM_ITERS  = 100;
    const char* images_path = "data/t10k-images.idx3-ubyte";
    const char* labels_path = "data/t10k-labels.idx1-ubyte";

    // allocate host dataset
    unsigned int count = batch_size;
    mnist_data** data_set = (mnist_data**)malloc(count * sizeof(mnist_data*));
    for (unsigned int i = 0; i < count; i++) {
        data_set[i] = (mnist_data*)malloc(sizeof(mnist_data));
        if (!data_set[i]) { fprintf(stderr, "Host malloc failed\n"); return 1; }
    }

    // load MNIST
    int r = mnist_load(images_path, labels_path, data_set, &count);
    if (r != 0) { fprintf(stderr, "MNIST load failed\n"); return 1; }

    // prepare batch host buffer: [B][28][28] float
    float (*batch_data)[28][28] =
        (float(*)[28][28])malloc((size_t)batch_size * 28 * 28 * sizeof(float));
    if (!batch_data) { fprintf(stderr, "batch_data malloc failed\n"); return 1; }
    for (int i = 0; i < batch_size; ++i) {
        convertDoubleArrayToFloatArray(data_set[i]->data, batch_data[i]);
    }

    // build model
    Layer layer;

    // output buffer (复用，避免每轮 malloc)
    float* h_out = (float*)malloc((size_t)batch_size * FC_OUT * sizeof(float));
    if (!h_out) { fprintf(stderr, "h_out malloc failed\n"); return 1; }

    // benchmark: 100 次，仅统计前向 + D2H 的总时间和平均时间
    double total_ms = 0.0;

    for (int iter = 0; iter < NUM_ITERS; ++iter) {
        auto t0 = std::chrono::high_resolution_clock::now();

        // forward
        layer.forward_pass_batch(batch_data, batch_size);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize()); // 确保前向完成

        // copy outputs back [B*FC_OUT]
        CUDA_CHECK(cudaMemcpy(h_out, layer.d_out_fc,
                              (size_t)batch_size * FC_OUT * sizeof(float),
                              cudaMemcpyDeviceToHost));

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += ms;
    }

    double avg_ms = total_ms / (double)NUM_ITERS;
    printf("Runs        : %d\n", NUM_ITERS);
    printf("Batch size  : %d\n", batch_size);
    printf("Total time  : %.3f ms (forward + D2H)\n", total_ms);
    printf("Average time: %.3f ms per run\n", avg_ms);

    // cleanup
    free(h_out);
    free(batch_data);
    for (unsigned int i = 0; i < count; ++i) free(data_set[i]);
    free(data_set);
    return 0;
}