//
// Metal port of game/graphics/opengl_renderer/shaders/sprite3_3d.{vert,frag}
//
// The sprite bucket: particles, the HUD, the menus. One "sprite" is four identical vertices that
// differ only in info[2], the corner index; the vertex shader turns each into a corner of a
// camera-facing quad. Which of the three transforms it uses is info[3]: 2D world sprites, HUD
// sprites, and true 3D sprites, which are oriented by a quaternion.
//
// Differences from the GLSL, all of them mechanical:
//   - the uniforms are two structs, because MSL has no free-floating uniforms. The big one is
//     per frame; the two alpha-test bounds change per draw and are their own, so a draw does not
//     re-send the 75-entry HUD offset table.
//   - SCISSOR_ADJUST and HEIGHT_SCALE were substituted into the GLSL at compile time; here they
//     are fields, so one compiled library serves every game version.
//   - the depth range is remapped from OpenGL's [-w, w] to Metal's [0, w].
//   - mat4 indexing: GLSL's mtx[3] is the fourth *column*, which is what float4x4's [3] is too.
//
// Vertex layout is Sprite3Core::SpriteVertex3D.
//

#include <metal_stdlib>
using namespace metal;

#include "metal_shader_types.h"

struct Sprite3VertexIn {
  float4 xyz_sx [[attribute(0)]];
  float4 quat_sy [[attribute(1)]];
  float4 rgba [[attribute(2)]];
  ushort2 flags_matrix [[attribute(3)]];
  ushort4 tex_info_in [[attribute(4)]];
};

struct Sprite3VertexOut {
  float4 position [[position]];
  float4 fragment_color [[flat]];
  float3 tex_coord;
  ushort2 tex_info [[flat]];
};

static float4 sprite_matrix_transform(float4x4 mtx, float3 pt) {
  return mtx[3] + mtx[0] * pt.x + mtx[1] * pt.y + mtx[2] * pt.z;
}

static float3x3 sprite_quat_to_rot(float3 quat) {
  float3x3 result;
  float qr = sqrt(abs(1.0 - (quat.x * quat.x + quat.y * quat.y + quat.z * quat.z)));
  result[0][0] = 1.0 - 2.0 * (quat.y * quat.y + quat.z * quat.z);
  result[1][0] = 2.0 * (quat.x * quat.y - quat.z * qr);
  result[2][0] = 2.0 * (quat.x * quat.z + quat.y * qr);
  result[0][1] = 2.0 * (quat.x * quat.y + quat.z * qr);
  result[1][1] = 1.0 - 2.0 * (quat.x * quat.x + quat.z * quat.z);
  result[2][1] = 2.0 * (quat.y * quat.z - quat.x * qr);
  result[0][2] = 2.0 * (quat.x * quat.z - quat.y * qr);
  result[1][2] = 2.0 * (quat.y * quat.z + quat.x * qr);
  result[2][2] = 1.0 - 2.0 * (quat.x * quat.x + quat.y * quat.y);
  return result;
}

static float4 sprite_transform2(constant Sprite3Uniforms& u,
                                float3 root,
                                float4 off,
                                float3x3 sprite_rot,
                                float sx,
                                float sy) {
  float3 pos = root;

  float3 offset = sprite_rot[0] * off.x * sx + sprite_rot[1] * off.y + sprite_rot[2] * off.z * sy;

  pos += offset;
  float4 transformed_pos = -sprite_matrix_transform(u.camera, pos);
  float Q = u.pfog0 / transformed_pos.w;
  transformed_pos.xyz *= Q;
  transformed_pos.xyz += u.hvdf_offset.xyz;

  return transformed_pos;
}

vertex Sprite3VertexOut sprite3_vert(Sprite3VertexIn in [[stage_in]],
                                     constant Sprite3Uniforms& u
                                     [[buffer(MetalBufferIndexUniforms)]]) {
  Sprite3VertexOut out;

  // STEP 1: UNPACK DATA AND CREATE READABLE VARIABLES

  float3 position = in.xyz_sx.xyz;
  float sx = in.xyz_sx.w;
  float sy = in.quat_sy.w;
  out.fragment_color = in.rgba;
  uint vert_id = in.tex_info_in.z;
  uint rendermode = in.tex_info_in.w;  // 2D, HUD, 3D
  float3 quat = in.quat_sy.xyz;
  uint matrix_idx = in.flags_matrix.y;

  float4 transformed = float4(0.0);

  // STEP 2: perspective transform for distance
  float4 transformed_pos_vf02 =
      sprite_matrix_transform(rendermode == 2 ? u.hud_matrix : u.camera, position);
  float Q = u.pfog0 / transformed_pos_vf02.w;

  // STEP 3: fade out sprite!
  float4 scales_vf01 = in.xyz_sx;  // now used for something else.
  scales_vf01.z = sy;              // start building the scale vector
  scales_vf01.zw *= Q;             // sy sx
  scales_vf01.x = scales_vf01.z;   // = sy
  scales_vf01.x *= scales_vf01.w;  // x = sx * sy
  scales_vf01.x *= u.inv_area;     // x = sx * sy * inv_area (area ratio)
  out.fragment_color.w *= min(scales_vf01.x, 1.0);

  // STEP 4: actual vertex transformation
  if (rendermode == 3) {  // 3D sprites

    float3x3 rot = sprite_quat_to_rot(quat);
    transformed = sprite_transform2(u, position, u.xyz_array[vert_id], rot, sx, sy);

  } else if (rendermode == 1) {  // 2D sprites

    transformed_pos_vf02.xyz *= Q;
    float4 offset_pos_vf10 = transformed_pos_vf02 + u.hvdf_offset;
    offset_pos_vf10.w = max(offset_pos_vf10.w, u.fog_min);

    scales_vf01.z = clamp(scales_vf01.z, u.min_scale, u.max_scale);
    scales_vf01.w = clamp(scales_vf01.w, u.min_scale, u.max_scale);

    quat.z *= u.deg_to_rad;
    float sp_sin = sin(quat.z);
    float sp_cos = cos(quat.z);

    float4 xy0_vf19 = u.xy_array[vert_id + (in.flags_matrix.x & 15u)];
    float4 vf12_rotated = (u.basis_x * sp_cos) - (u.basis_y * sp_sin);
    float4 vf13_rotated_trans = (u.basis_x * sp_sin) + (u.basis_y * sp_cos);

    vf12_rotated *= scales_vf01.w;
    vf13_rotated_trans *= scales_vf01.z;

    transformed = offset_pos_vf10 + vf12_rotated * xy0_vf19.x + vf13_rotated_trans * xy0_vf19.y;

  } else if (rendermode == 2) {  // hud sprites

    transformed_pos_vf02.xyz *= Q;
    float4 offset_pos_vf10 =
        transformed_pos_vf02 +
        (matrix_idx == 0 ? u.hud_hvdf_offset : u.hud_hvdf_user[matrix_idx - 1]);

    // NOTE: no max scale for hud
    scales_vf01.z = max(scales_vf01.z, u.min_scale);
    scales_vf01.w = max(scales_vf01.w, u.min_scale);

    quat.z *= u.deg_to_rad;
    float sp_sin = sin(quat.z);
    float sp_cos = cos(quat.z);

    float4 xy0_vf19 = u.xy_array[vert_id + (in.flags_matrix.x & 15u)];
    float4 vf12_rotated = (u.basis_x * sp_cos) - (u.basis_y * sp_sin);
    float4 vf13_rotated_trans = (u.basis_x * sp_sin) + (u.basis_y * sp_cos);

    vf12_rotated *= scales_vf01.w;
    vf13_rotated_trans *= scales_vf01.z;

    transformed = offset_pos_vf10 + vf12_rotated * xy0_vf19.x + vf13_rotated_trans * xy0_vf19.y;
  }

  out.tex_coord = u.st_array[vert_id].xyz;

  // STEP 5: final adjustments
  // correct xy offset
  transformed.xy -= 2048.0;
  // correct z scale
  transformed.z /= 8388608.0;
  transformed.z -= 1.0;
  // correct xy scale
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= u.scissor_adjust * u.height_scale;
  // OpenGL clips z to [-w, w], Metal to [0, w].
  transformed.z = (transformed.z + transformed.w) * 0.5;
  out.position = transformed;

  out.fragment_color *= 2.0;
  out.fragment_color.w *= 2.0;

  out.tex_info = ushort2(in.tex_info_in.x, in.tex_info_in.y);
  return out;
}

fragment float4 sprite3_frag(Sprite3VertexOut in [[stage_in]],
                             constant SpriteDrawUniforms& d
                             [[buffer(MetalBufferIndexUniforms)]],
                             texture2d<float> tex_T0 [[texture(0)]],
                             sampler samp [[sampler(0)]]) {
  float4 T0 = tex_T0.sample(samp, in.tex_coord.xy);
  if (in.tex_info.y == 0) {
    T0.w = 1.0;
  }
  float4 color = in.fragment_color * T0;

  if (color.a < d.alpha_min || color.a > d.alpha_max) {
    discard_fragment();
  }
  return color;
}
