

# nvcc -shared -Xcompiler -fPIC -o libextension.so extension.cu -L ${GVIRTUS_HOME}/lib/frontend -L ${GVIRTUS_HOME}/lib/ -lcudart -lcublas 

import ctypes
import numpy as np

# Load GVirtuS CUDA library
GVIRTUS_HOME = "/home/GVirtuS"

# Load compiled CUDA library
libextension = ctypes.CDLL("./libtest.so")

def add():
    # Define function prototype (example: addition of two integers)
    libextension.simple_add.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]

    # Prepare input data
    a = np.array([5], dtype=np.int32)
    b = np.array([16], dtype=np.int32)
    c = np.array([0], dtype=np.int32)  # Output

    # Call the CUDA function
    libextension.simple_add(a.ctypes.data_as(ctypes.POINTER(ctypes.c_int)),
                        b.ctypes.data_as(ctypes.POINTER(ctypes.c_int)),
                        c.ctypes.data_as(ctypes.POINTER(ctypes.c_int)))

    print(f"Addition result: {c[0]}")

def multiply():
    libextension.matrix_multiply.argtypes = [
    ctypes.POINTER(ctypes.c_float),  # Pointer to matrix A
    ctypes.POINTER(ctypes.c_float),  # Pointer to matrix B
    ctypes.POINTER(ctypes.c_float),  # Pointer to matrix C (output)
    ctypes.c_int,  # Rows of A and C (M)
    ctypes.c_int,  # Columns of B and C (N)
    ctypes.c_int,  # Columns of A and rows of B (K)
    ]

    # Use matrix_multiply (matrix multiplication example)
    A = np.array([1, 2, 3, 4, 5, 6], dtype=np.float32).reshape(2, 3)  # 2x3 matrix
    B = np.array([7, 8, 9, 10, 11, 12], dtype=np.float32).reshape(3, 2)  # 3x2 matrix
    C = np.zeros((2, 2), dtype=np.float32)  # 2x2 result matrix

    libextension.matrix_multiply(A.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                B.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                C.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                2, 2, 3)  # M=2, N=2, K=3

    print("Matrix multiplication result:")
    print(C)

def conv():
    libextension.conv.argtypes = [
    ctypes.POINTER(ctypes.c_float),  # Pointer to matrix A
    ctypes.POINTER(ctypes.c_float),  # Pointer to matrix B
    ctypes.POINTER(ctypes.c_float),  # Pointer to matrix C (output)
    ]

    # Use matrix_multiply (matrix multiplication example)
    A = np.array([1, 2, 3, 4], dtype=np.float32).reshape(2, 2)  # 2x3 matrix
    B = np.array([7, 8, 9, 10,11,12,13,14], dtype=np.float32).reshape(2, 2,2)  # 3x2 matrix
    C = np.zeros((2, 2,2), dtype=np.float32)  # 2x2 result matrix

    libextension.conv(A.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                B.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                C.ctypes.data_as(ctypes.POINTER(ctypes.c_float))) 

    print("Conv result:")
    print(C)

def main():
    add()
    multiply()
    conv()

if __name__ == "__main__":
    main()
 
