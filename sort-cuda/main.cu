#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <cuda.h>

typedef unsigned int T;

// CUDA kernels
#include "sort_bottom_scan.h"
#include "sort_reduce.h"
#include "sort_top_scan.h"

bool verifySort(const T *keys, const size_t size)
{
  bool passed = true;

  for (size_t i = 0; i < size - 1; i++)
  {
    if (keys[i] > keys[i + 1])
    {
      passed = false;
#ifdef VERBOSE_OUTPUT
      std::cout << "Idx: " << i;
      std::cout << " Key: " << keys[i] << "\n";
#endif
    }
  }
  std::cout << "Test ";
  if (passed)
    std::cout << "Passed" << std::endl;
  else
    std::cout << "---FAILED---" << std::endl;

  return passed;
}

int main(int argc, char** argv) 
{

  if (argc != 3) 
  {
    printf("Usage: %s <problem size> <number of passes>\n.", argv[0]);
    return -1;
  }

  int select = atoi(argv[1]);
  int passes = atoi(argv[2]);

  // Problem Sizes
  int probSizes[4] = { 1, 8, 32, 64 };
  size_t size = probSizes[select];

  // Convert to MiB
  size = (size * 1024 * 1024) / sizeof(T);

  // Create input data on CPU
  unsigned int bytes = size * sizeof(T);

  T* h_idata = (T*) malloc (bytes); 
  T* h_odata = (T*) malloc (bytes); 

  // Initialize host memory
  std::cout << "Initializing host memory." << std::endl;
  for (int i = 0; i < size; i++)
  {
    h_idata[i] = i % 16; // Fill with some pattern
    h_odata[i] = -1;
  }

  std::cout << "Running benchmark with input array length " << size << std::endl;

  auto start = std::chrono::steady_clock::now();

  // Number of local work items per group
  const size_t local_wsize  = 256;
  // Number of global work items
  const size_t global_wsize = 16384; 
  // 64 work groups
  const size_t num_work_groups = global_wsize / local_wsize;

  // The radix width in bits
  const int radix_width = 4; // Changing this requires major kernel updates
  //const int num_digits = (int)pow((double)2, radix_width); // n possible digits
  const int num_digits = 16;

  T* d_idata;
  T* d_odata;
  T* d_isums;

  cudaMalloc((void**)&d_idata, size * sizeof(T));
  cudaMemcpy(d_idata, h_idata, size * sizeof(T), cudaMemcpyHostToDevice);
  cudaMalloc((void**)&d_odata, size * sizeof(T));
  cudaMalloc((void**)&d_isums, num_work_groups * num_digits * sizeof(T));

  for (int k = 0; k < passes; k++)
  {
    // Assuming an 8 bit byte.
    // shift is uint because Computecpp compiler has no operator>>(unsigned int, int);
    for (unsigned int shift = 0; shift < sizeof(T)*8; shift += radix_width)
    {
      // Like scan, we use a reduce-then-scan approach

      // But before proceeding, update the shift appropriately
      // for each kernel. This is how many bits to shift to the
      // right used in binning.

      // Also, the sort is not in place, so swap the input and output
      // buffers on each pass.
      bool even = ((shift / radix_width) % 2 == 0) ? true : false;

      T* d_in = even ? d_idata : d_odata;
      T* d_out = even ? d_odata : d_idata;

      reduce<<<num_work_groups, local_wsize>>> (d_in, d_isums, size);
      top_scan<<<num_work_groups, local_wsize>>>(d_isums);
      bottom_scan<<<num_work_groups, local_wsize>>>(d_out, d_isums, size);

    }
    cudaDeviceSynchronize();
  }  // passes

  cudaMemcpy(h_odata, d_out, size * sizeof(T), cudaMemcpyDeviceToHost);
  cudaFree(d_idata);
  cudaFree(d_odata);
  cudaFree(d_isums);


  auto end = std::chrono::steady_clock::now();
  auto t = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  double second = t / 1.e9; // Convert to seconds
  printf("Total elapsed time %.3f (s)\n", second);

  if (! verifySort(h_odata, size)) 
  {
    return -1;
  }

  free(h_idata);
  free(h_odata);
  return 0;
}
