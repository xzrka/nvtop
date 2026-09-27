/*
 *
 * Fallback implementations for the queries only the NVIDIA backend answers, used
 * when NVIDIA support is not built in. All of them report "nothing to show" so
 * that the interface keeps laying out a device panel without the fields.
 *
 */

#include "nvtop/extract_gpuinfo_common.h"

unsigned nvtop_get_nvlink_info(struct gpu_info *gpu_info, struct nvlink_info *nvlink_info) {
  (void)gpu_info;
  (void)nvlink_info;
  return 0;
}

bool nvtop_get_nvlink_error_counts(struct gpu_info *gpu_info, unsigned long long *out_errors,
                                   unsigned long long *out_corrections, unsigned long long *out_ecc) {
  (void)gpu_info;
  (void)out_errors;
  (void)out_corrections;
  (void)out_ecc;
  return false;
}

void nvtop_reset_nvlink_cache(struct gpu_info *gpu_info) { (void)gpu_info; }

bool nvtop_get_ecc_support(struct gpu_info *gpu_info) {
  (void)gpu_info;
  return false;
}

enum gpu_extra_temps_support nvtop_get_extra_temps_support(struct gpu_info *gpu_info) {
  (void)gpu_info;
  return gpu_extra_temps_none;
}
