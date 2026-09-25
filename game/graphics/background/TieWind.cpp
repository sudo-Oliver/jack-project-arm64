/*!
 * @file TieWind.cpp
 * See TieWind.h. Moved out of the OpenGL Tie3 renderer unchanged.
 */

#include "TieWind.h"

#include <algorithm>

namespace {

void vector_min_in_place(math::Vector4f& v, float val) {
  for (int i = 0; i < 4; i++) {
    if (v[i] > val) {
      v[i] = val;
    }
  }
}

math::Vector4f vector_max(const math::Vector4f& v, float val) {
  math::Vector4f result;
  for (int i = 0; i < 4; i++) {
    result[i] = std::max(val, v[i]);
  }
  return result;
}

}  // namespace

void do_wind_math(u16 wind_idx,
                  float* wind_vector_data,
                  const TieWindWork& wind_work,
                  float stiffness,
                  std::array<math::Vector4f, 4>& mat) {
  float* my_vector = wind_vector_data + (4 * wind_idx);
  const auto& work_vector = wind_work.wind_array[(wind_work.wind_time + wind_idx) & 63];
  constexpr float cx = 0.5;
  constexpr float cy = 100.0;
  constexpr float cz = 0.0166;
  constexpr float cw = -1.0;

  // ld s1, 8(s5)                    # load wind vector 1
  // pextlw s1, r0, s1               # convert to 2x 64 bits, by shifting left
  // qmtc2.i vf18, s1                # put in vf
  float vf18_x = my_vector[2];
  float vf18_z = my_vector[3];

  // ld s2, 0(s5)                    # load wind vector 0
  // pextlw s3, r0, s2               # convert to 2x 64 bits, by shifting left
  // qmtc2.i vf17, s3                # put in vf
  float vf17_x = my_vector[0];
  float vf17_z = my_vector[1];

  // lqc2 vf16, 12(s3)               # load wind vector
  math::Vector4f vf16 = work_vector;

  // vmula.xyzw acc, vf16, vf1       # acc = vf16
  // vmsubax.xyzw acc, vf18, vf19    # acc = vf16 - vf18 * wind_const.x
  // vmsuby.xyzw vf16, vf17, vf19
  // # vf16 -= (vf18 * wind_const.x) + (vf17 * wind_const.y)
  vf16.x() -= cx * vf18_x + cy * vf17_x;
  vf16.z() -= cx * vf18_z + cy * vf17_z;

  // vmulaz.xyzw acc, vf16, vf19     # acc = vf16 * wind_const.z
  // vmadd.xyzw vf18, vf1, vf18
  // # vf18 += vf16 * wind_const.z
  math::Vector4f vf18(vf18_x, 0.f, vf18_z, 0.f);
  vf18 += vf16 * cz;

  // vmulaz.xyzw acc, vf18, vf19    # acc = vf18 * wind_const.z
  // vmadd.xyzw vf17, vf17, vf1
  // # vf17 += vf18 * wind_const.z
  math::Vector4f vf17(vf17_x, 0.f, vf17_z, 0.f);
  vf17 += vf18 * cz;

  // vitof12.xyzw vf11, vf11 # normal convert
  // vitof12.xyzw vf12, vf12 # normal convert

  // vminiw.xyzw vf17, vf17, vf0
  vector_min_in_place(vf17, 1.f);

  // qmfc2.i s3, vf18
  // ppacw s3, r0, s3

  // vmaxw.xyzw vf27, vf17, vf19
  auto vf27 = vector_max(vf17, cw);

  // vmulw.xyzw vf27, vf27, vf15
  vf27 *= stiffness;

  // vmulax.yw acc, vf0, vf0
  // vmulay.xz acc, vf27, vf10
  // vmadd.xyzw vf10, vf1, vf10
  mat[0].x() += vf27.x() * mat[0].y();
  mat[0].z() += vf27.z() * mat[0].y();

  // qmfc2.i s2, vf27
  if (!wind_work.paused) {
    my_vector[0] = vf27.x();
    my_vector[1] = vf27.z();
    my_vector[2] = vf18.x();
    my_vector[3] = vf18.z();
  }

  // vmulax.yw acc, vf0, vf0
  // vmulay.xz acc, vf27, vf11
  // vmadd.xyzw vf11, vf1, vf11
  mat[1].x() += vf27.x() * mat[1].y();
  mat[1].z() += vf27.z() * mat[1].y();

  // ppacw s2, r0, s2
  // vmulax.yw acc, vf0, vf0
  // vmulay.xz acc, vf27, vf12
  // vmadd.xyzw vf12, vf1, vf12
  mat[2].x() += vf27.x() * mat[2].y();
  mat[2].z() += vf27.z() * mat[2].y();

  //
  // if not paused
  // sd s3, 8(s5)
  // sd s2, 0(s5)
}


void compute_tie_wind_matrices(const TieWindWork& wind_work,
                               const std::vector<tfrag3::TieWindInstance>& instances,
                               const std::array<math::Vector4f, 4>& camera,
                               float wind_multiplier,
                               std::vector<float>& wind_vectors,
                               std::vector<std::array<math::Vector4f, 4>>& out) {
  // The scratch has four floats per wind index, and the instances say how many there are.
  u16 max_wind_idx = 0;
  for (const auto& inst : instances) {
    max_wind_idx = std::max(max_wind_idx, inst.wind_idx);
  }
  if (wind_vectors.size() < (size_t)(4 * max_wind_idx + 4)) {
    wind_vectors.resize(4 * max_wind_idx + 4);
  }

  out.assign(instances.size(), {});
  for (size_t inst_id = 0; inst_id < instances.size(); inst_id++) {
    const auto& info = instances[inst_id];
    auto& result = out[inst_id];
    auto mat = info.matrix;

    do_wind_math(info.wind_idx, wind_vectors.data(), wind_work, info.stiffness * wind_multiplier,
                 mat);

    result[0] = camera[0] * mat[0].x() + camera[1] * mat[0].y() + camera[2] * mat[0].z();
    result[1] = camera[0] * mat[1].x() + camera[1] * mat[1].y() + camera[2] * mat[1].z();
    result[2] = camera[0] * mat[2].x() + camera[1] * mat[2].y() + camera[2] * mat[2].z();
    result[3] = camera[0] * mat[3].x() + camera[1] * mat[3].y() + camera[2] * mat[3].z() +
                camera[3];
  }
}
