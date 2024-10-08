#include "common.h"

// include kernels
#include "downscaleKernel.hpp"
#include "upscaleKernel.hpp"
#include "warpingKernel.hpp"
#include "derivativesKernel.hpp"
#include "solverKernel.hpp"
#include "addKernel.hpp"


///////////////////////////////////////////////////////////////////////////////
/// \brief method logic
///
/// handles memory allocations, control flow
/// \param[in]  I0           source image
/// \param[in]  I1           tracked image
/// \param[in]  width        images width
/// \param[in]  height       images height
/// \param[in]  stride       images stride
/// \param[in]  alpha        degree of displacement field smoothness
/// \param[in]  nLevels      number of levels in a pyramid
/// \param[in]  nWarpIters   number of warping iterations per pyramid level
/// \param[in]  nSolverIters number of solver iterations (Jacobi iterations)
/// \param[out] u            horizontal displacement
/// \param[out] v            vertical displacement
///////////////////////////////////////////////////////////////////////////////
void ComputeFlowSYCL(const float *I0, const float *I1, int width, int height,
                     int stride, float alpha, int nLevels, int nWarpIters,
                     int nSolverIters, float *u, float *v) {
  printf("Computing optical flow on Device...\n");

  sycl::queue q{aspect_selector(sycl::aspect::ext_intel_legacy_image), sycl::property::queue::in_order()};

  std::cout << "\nRunning on "
            << q.get_device().get_info<sycl::info::device::name>() << "\n";
  // pI0 and pI1 will hold device pointers
  const float **pI0 = new const float *[nLevels];
  const float **pI1 = new const float *[nLevels];

  int *pW = new int[nLevels];
  int *pH = new int[nLevels];
  int *pS = new int[nLevels];

  // device memory pointers
  float *d_tmp;
  float *d_du0;
  float *d_dv0;
  float *d_du1;
  float *d_dv1;

  float *d_Ix;
  float *d_Iy;
  float *d_Iz;

  float *d_u;
  float *d_v;
  float *d_nu;
  float *d_nv;

  const int dataSize = stride * height * sizeof(float);

  CHECK_ERROR(d_tmp = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_du0 = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_dv0 = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_du1 = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_dv1 = (float *)sycl::malloc_device(dataSize, q));

  CHECK_ERROR(d_Ix = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_Iy = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_Iz = (float *)sycl::malloc_device(dataSize, q));

  CHECK_ERROR(d_u = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_v = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_nu = (float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(d_nv = (float *)sycl::malloc_device(dataSize, q));

  // prepare pyramid

  int currentLevel = nLevels - 1;
  // allocate GPU memory for input images
  CHECK_ERROR(*(pI0 + currentLevel) = (const float *)sycl::malloc_device(dataSize, q));
  CHECK_ERROR(*(pI1 + currentLevel) = (const float *)sycl::malloc_device(dataSize, q));

  float *pI0_h = (float *)sycl::malloc_host(stride * height * sizeof(sycl::float4), q);
  float *I0_h = (float *)sycl::malloc_host(dataSize, q);

  float *pI1_h = (float *)sycl::malloc_host(stride * height * sizeof(sycl::float4), q);
  float *I1_h = (float *)sycl::malloc_host(dataSize, q);

  float *src_d0 = (float *)sycl::malloc_device(stride * height * sizeof(sycl::float4), q);
  float *src_d1 = (float *)sycl::malloc_device(stride * height * sizeof(sycl::float4), q);

  q.memcpy((void *)I0_h, I0, dataSize);
  q.memcpy((void *)I1_h, I1, dataSize);

  q.memcpy((void *)pI0[currentLevel], I0, dataSize);
  q.memcpy((void *)pI1[currentLevel], I1, dataSize);

  q.wait();

  pW[currentLevel] = width;
  pH[currentLevel] = height;
  pS[currentLevel] = stride;

  for (; currentLevel > 0; --currentLevel) {
    int nw = pW[currentLevel] / 2;
    int nh = pH[currentLevel] / 2;
    int ns = iAlignUp(nw);

    CHECK_ERROR(
        *(pI0 + currentLevel - 1) = (const float *)sycl::malloc_device(
            ns * nh * sizeof(float), q));
    CHECK_ERROR(
        *(pI1 + currentLevel - 1) = (const float *)sycl::malloc_device(
            ns * nh * sizeof(float), q));

    Downscale(pI0[currentLevel], pI0_h, I0_h, src_d0, pW[currentLevel],
              pH[currentLevel], pS[currentLevel], nw, nh, ns,
              (float *)pI0[currentLevel - 1], q);

    Downscale(pI1[currentLevel], pI0_h, I0_h, src_d0, pW[currentLevel],
              pH[currentLevel], pS[currentLevel], nw, nh, ns,
              (float *)pI1[currentLevel - 1], q);

    pW[currentLevel - 1] = nw;
    pH[currentLevel - 1] = nh;
    pS[currentLevel - 1] = ns;
  }

  CHECK_ERROR(q.memset(d_u, 0, stride * height * sizeof(float)));
  CHECK_ERROR(q.memset(d_v, 0, stride * height * sizeof(float)));
  CHECK_ERROR(q.wait());

  // compute flow
  for (; currentLevel < nLevels; ++currentLevel) {
    for (int warpIter = 0; warpIter < nWarpIters; ++warpIter) {
      CHECK_ERROR(q.memset(d_du0, 0, dataSize));
      CHECK_ERROR(q.memset(d_dv0, 0, dataSize));

      CHECK_ERROR(q.memset(d_du1, 0, dataSize));
      CHECK_ERROR(q.memset(d_dv1, 0, dataSize));

      // on current level we compute optical flow
      // between frame 0 and warped frame 1
       WarpImage(pI1[currentLevel], pI0_h, I0_h, src_d0, pW[currentLevel], pH[currentLevel],
                pS[currentLevel], d_u, d_v, d_tmp, q);

      ComputeDerivatives(pI0[currentLevel], d_tmp, pI0_h, pI1_h, I0_h, I1_h,
                         src_d0, src_d1, pW[currentLevel],
                         pH[currentLevel], pS[currentLevel], d_Ix, d_Iy, d_Iz, q);

      for (int iter = 0; iter < nSolverIters; ++iter) {
        SolveForUpdate(d_du0, d_dv0, d_Ix, d_Iy, d_Iz, pW[currentLevel],
                       pH[currentLevel], pS[currentLevel], alpha, d_du1, d_dv1, q);

        Swap(d_du0, d_du1);
        Swap(d_dv0, d_dv1);
      }

      // update u, v
      Add(d_u, d_du0, pH[currentLevel] * pS[currentLevel], d_u, q);
      Add(d_v, d_dv0, pH[currentLevel] * pS[currentLevel], d_v, q);
    }

    if (currentLevel != nLevels - 1) {
      // prolongate solution
      float scaleX = (float)pW[currentLevel + 1] / (float)pW[currentLevel];

      Upscale(d_u, pI0_h, I0_h, src_d0, pW[currentLevel], pH[currentLevel], pS[currentLevel],
              pW[currentLevel + 1], pH[currentLevel + 1], pS[currentLevel + 1],
              scaleX, d_nu, q);

      float scaleY = (float)pH[currentLevel + 1] / (float)pH[currentLevel];

      Upscale(d_v, pI0_h, I0_h, src_d0, pW[currentLevel], pH[currentLevel], pS[currentLevel],
              pW[currentLevel + 1], pH[currentLevel + 1], pS[currentLevel + 1],
              scaleY, d_nv, q);

      Swap(d_u, d_nu);
      Swap(d_v, d_nv);
    }
  }

  CHECK_ERROR(q.memcpy(u, d_u, dataSize));
  CHECK_ERROR(q.memcpy(v, d_v, dataSize));
  CHECK_ERROR(q.wait());

  // cleanup
  for (int i = 0; i < nLevels; ++i) {
    CHECK_ERROR(sycl::free((void *)pI0[i], q));
    CHECK_ERROR(sycl::free((void *)pI1[i], q));
  }

  delete[] pI0;
  delete[] pI1;
  delete[] pW;
  delete[] pH;
  delete[] pS;

  CHECK_ERROR(sycl::free(d_tmp, q));
  CHECK_ERROR(sycl::free(d_du0, q));
  CHECK_ERROR(sycl::free(d_dv0, q));
  CHECK_ERROR(sycl::free(d_du1, q));
  CHECK_ERROR(sycl::free(d_dv1, q));
  CHECK_ERROR(sycl::free(d_Ix, q));
  CHECK_ERROR(sycl::free(d_Iy, q));
  CHECK_ERROR(sycl::free(d_Iz, q));
  CHECK_ERROR(sycl::free(d_nu, q));
  CHECK_ERROR(sycl::free(d_nv, q));
  CHECK_ERROR(sycl::free(d_u, q));
  CHECK_ERROR(sycl::free(d_v, q));
}
