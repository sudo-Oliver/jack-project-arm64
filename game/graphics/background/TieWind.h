#pragma once

/*!
 * @file TieWind.h
 * The wind animation for TIE instances, with no graphics API in it.
 *
 * Some TIE instances sway: the palm trees, the banners, the tall grass. The game sends a wind
 * state once per frame and each swaying instance carries a wind index and a stiffness; the VU
 * program turns those into a per-instance matrix, and the instance is drawn with that matrix in
 * place of the camera.
 *
 * That is a transcription of the VU program and identical for every backend, so it lives here.
 */

#include <array>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"

/*!
 * The wind state the game DMAs once per frame, in the TIE bucket. The layout is the GOAL one.
 */
struct TieWindWork {
  u32 paused;
  u32 pad[3];
  math::Vector4f wind_array[64];
  math::Vector4f wind_normal;
  math::Vector4f wind_temp;
  float wind_force[64];
  u32 wind_time;
  u32 pad2[3];
};

/*!
 * Run the VU program's wind math for one instance, folding the sway into `mat`.
 *
 * `wind_vector_data` is the renderer's persistent scratch, four floats per wind index: the sway
 * is integrated over time, so it has to survive between frames. It is written back unless the
 * game has paused the wind.
 */
void do_wind_math(u16 wind_idx,
                  float* wind_vector_data,
                  const TieWindWork& wind_work,
                  float stiffness,
                  std::array<math::Vector4f, 4>& mat);

/*!
 * Build the matrix each swaying instance is drawn with: its own matrix, swayed, then multiplied
 * by the camera. `out` is resized to one entry per instance.
 *
 * `wind_vectors` is the persistent scratch described above, and is grown to fit.
 */
void compute_tie_wind_matrices(const TieWindWork& wind_work,
                               const std::vector<tfrag3::TieWindInstance>& instances,
                               const std::array<math::Vector4f, 4>& camera,
                               float wind_multiplier,
                               std::vector<float>& wind_vectors,
                               std::vector<std::array<math::Vector4f, 4>>& out);
