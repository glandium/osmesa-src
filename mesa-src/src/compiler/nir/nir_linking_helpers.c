/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"
#include "util/set.h"
#include "util/hash_table.h"

/* This file contains various little helpers for doing simple linking in
 * NIR.  Eventually, we'll probably want a full-blown varying packing
 * implementation in here.  Right now, it just deletes unused things.
 */

/**
 * Returns the bits in the inputs_read, outputs_written, or
 * system_values_read bitfield corresponding to this variable.
 */
static uint64_t
get_variable_io_mask(nir_variable *var, gl_shader_stage stage)
{
   if (var->data.location < 0)
      return 0;

   unsigned location = var->data.patch ?
      var->data.location - VARYING_SLOT_PATCH0 : var->data.location;

   assert(var->data.mode == nir_var_shader_in ||
          var->data.mode == nir_var_shader_out ||
          var->data.mode == nir_var_system_value);
   assert(var->data.location >= 0);

   const struct glsl_type *type = var->type;
   if (nir_is_per_vertex_io(var, stage) || var->data.per_view) {
      assert(glsl_type_is_array(type));
      type = glsl_get_array_element(type);
   }

   unsigned slots = glsl_count_attribute_slots(type, false);
   return ((1ull << slots) - 1) << location;
}

static uint8_t
get_num_components(nir_variable *var)
{
   if (glsl_type_is_struct_or_ifc(glsl_without_array(var->type)))
      return 4;

   return glsl_get_vector_elements(glsl_without_array(var->type));
}

static void
tcs_add_output_reads(nir_shader *shader, uint64_t *read, uint64_t *patches_read)
{
   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      nir_foreach_block(block, function->impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            if (intrin->intrinsic != nir_intrinsic_load_deref)
               continue;

            nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
            if (deref->mode != nir_var_shader_out)
               continue;

            nir_variable *var = nir_deref_instr_get_variable(deref);
            for (unsigned i = 0; i < get_num_components(var); i++) {
               if (var->data.patch) {
                  patches_read[var->data.location_frac + i] |=
                     get_variable_io_mask(var, shader->info.stage);
               } else {
                  read[var->data.location_frac + i] |=
                     get_variable_io_mask(var, shader->info.stage);
               }
            }
         }
      }
   }
}

/**
 * Helper for removing unused shader I/O variables, by demoting them to global
 * variables (which may then by dead code eliminated).
 *
 * Example usage is:
 *
 * progress = nir_remove_unused_io_vars(producer, nir_var_shader_out,
 *                                      read, patches_read) ||
 *                                      progress;
 *
 * The "used" should be an array of 4 uint64_ts (probably of VARYING_BIT_*)
 * representing each .location_frac used.  Note that for vector variables,
 * only the first channel (.location_frac) is examined for deciding if the
 * variable is used!
 */
bool
nir_remove_unused_io_vars(nir_shader *shader,
                          nir_variable_mode mode,
                          uint64_t *used_by_other_stage,
                          uint64_t *used_by_other_stage_patches)
{
   bool progress = false;
   uint64_t *used;

   assert(mode == nir_var_shader_in || mode == nir_var_shader_out);

   nir_foreach_variable_with_modes_safe(var, shader, mode) {
      if (var->data.patch)
         used = used_by_other_stage_patches;
      else
         used = used_by_other_stage;

      if (var->data.location < VARYING_SLOT_VAR0 && var->data.location >= 0)
         continue;

      if (var->data.always_active_io)
         continue;

      if (var->data.explicit_xfb_buffer)
         continue;

      uint64_t other_stage = used[var->data.location_frac];

      if (!(other_stage & get_variable_io_mask(var, shader->info.stage))) {
         /* This one is invalid, make it a global variable instead */
         var->data.location = 0;
         var->data.mode = nir_var_shader_temp;

         progress = true;
      }
   }

   if (progress)
      nir_fixup_deref_modes(shader);

   return progress;
}

bool
nir_remove_unused_varyings(nir_shader *producer, nir_shader *consumer)
{
   assert(producer->info.stage != MESA_SHADER_FRAGMENT);
   assert(consumer->info.stage != MESA_SHADER_VERTEX);

   uint64_t read[4] = { 0 }, written[4] = { 0 };
   uint64_t patches_read[4] = { 0 }, patches_written[4] = { 0 };

   nir_foreach_shader_out_variable(var, producer) {
      for (unsigned i = 0; i < get_num_components(var); i++) {
         if (var->data.patch) {
            patches_written[var->data.location_frac + i] |=
               get_variable_io_mask(var, producer->info.stage);
         } else {
            written[var->data.location_frac + i] |=
               get_variable_io_mask(var, producer->info.stage);
         }
      }
   }

   nir_foreach_shader_in_variable(var, consumer) {
      for (unsigned i = 0; i < get_num_components(var); i++) {
         if (var->data.patch) {
            patches_read[var->data.location_frac + i] |=
               get_variable_io_mask(var, consumer->info.stage);
         } else {
            read[var->data.location_frac + i] |=
               get_variable_io_mask(var, consumer->info.stage);
         }
      }
   }

   /* Each TCS invocation can read data written by other TCS invocations,
    * so even if the outputs are not used by the TES we must also make
    * sure they are not read by the TCS before demoting them to globals.
    */
   if (producer->info.stage == MESA_SHADER_TESS_CTRL)
      tcs_add_output_reads(producer, read, patches_read);

   bool progress = false;
   progress = nir_remove_unused_io_vars(producer, nir_var_shader_out, read,
                                        patches_read);

   progress = nir_remove_unused_io_vars(consumer, nir_var_shader_in, written,
                                        patches_written) || progress;

   return progress;
}

static uint8_t
get_interp_type(nir_variable *var, const struct glsl_type *type,
                bool default_to_smooth_interp)
{
   if (glsl_type_is_integer(type))
      return INTERP_MODE_FLAT;
   else if (var->data.interpolation != INTERP_MODE_NONE)
      return var->data.interpolation;
   else if (default_to_smooth_interp)
      return INTERP_MODE_SMOOTH;
   else
      return INTERP_MODE_NONE;
}

#define INTERPOLATE_LOC_SAMPLE 0
#define INTERPOLATE_LOC_CENTROID 1
#define INTERPOLATE_LOC_CENTER 2

static uint8_t
get_interp_loc(nir_variable *var)
{
   if (var->data.sample)
      return INTERPOLATE_LOC_SAMPLE;
   else if (var->data.centroid)
      return INTERPOLATE_LOC_CENTROID;
   else
      return INTERPOLATE_LOC_CENTER;
}

static bool
is_packing_supported_for_type(const struct glsl_type *type)
{
   /* We ignore complex types such as arrays, matrices, structs and bitsizes
    * other then 32bit. All other vector types should have been split into
    * scalar variables by the lower_io_to_scalar pass. The only exception
    * should be OpenGL xfb varyings.
    * TODO: add support for more complex types?
    */
   return glsl_type_is_scalar(type) && glsl_type_is_32bit(type);
}

struct assigned_comps
{
   uint8_t comps;
   uint8_t interp_type;
   uint8_t interp_loc;
   bool is_32bit;
};

/* Packing arrays and dual slot varyings is difficult so to avoid complex
 * algorithms this function just assigns them their existing location for now.
 * TODO: allow better packing of complex types.
 */
static void
get_unmoveable_components_masks(nir_shader *shader,
                                nir_variable_mode mode,
                                struct assigned_comps *comps,
                                gl_shader_stage stage,
                                bool default_to_smooth_interp)
{
   nir_foreach_variable_with_modes_safe(var, shader, mode) {
      assert(var->data.location >= 0);

      /* Only remap things that aren't built-ins. */
      if (var->data.location >= VARYING_SLOT_VAR0 &&
          var->data.location - VARYING_SLOT_VAR0 < MAX_VARYINGS_INCL_PATCH) {

         const struct glsl_type *type = var->type;
         if (nir_is_per_vertex_io(var, stage) || var->data.per_view) {
            assert(glsl_type_is_array(type));
            type = glsl_get_array_element(type);
         }

         /* If we can pack this varying then don't mark the components as
          * used.
          */
         if (is_packing_supported_for_type(type))
            continue;

         unsigned location = var->data.location - VARYING_SLOT_VAR0;

         unsigned elements =
            glsl_type_is_vector_or_scalar(glsl_without_array(type)) ?
            glsl_get_vector_elements(glsl_without_array(type)) : 4;

         bool dual_slot = glsl_type_is_dual_slot(glsl_without_array(type));
         unsigned slots = glsl_count_attribute_slots(type, false);
         unsigned dmul = glsl_type_is_64bit(glsl_without_array(type)) ? 2 : 1;
         unsigned comps_slot2 = 0;
         for (unsigned i = 0; i < slots; i++) {
            if (dual_slot) {
               if (i & 1) {
                  comps[location + i].comps |= ((1 << comps_slot2) - 1);
               } else {
                  unsigned num_comps = 4 - var->data.location_frac;
                  comps_slot2 = (elements * dmul) - num_comps;

                  /* Assume ARB_enhanced_layouts packing rules for doubles */
                  assert(var->data.location_frac == 0 ||
                         var->data.location_frac == 2);
                  assert(comps_slot2 <= 4);

                  comps[location + i].comps |=
                     ((1 << num_comps) - 1) << var->data.location_frac;
               }
            } else {
               comps[location + i].comps |=
                  ((1 << (elements * dmul)) - 1) << var->data.location_frac;
            }

            comps[location + i].interp_type =
               get_interp_type(var, type, default_to_smooth_interp);
            comps[location + i].interp_loc = get_interp_loc(var);
            comps[location + i].is_32bit =
               glsl_type_is_32bit(glsl_without_array(type));
         }
      }
   }
}

struct varying_loc
{
   uint8_t component;
   uint32_t location;
};

static void
mark_all_used_slots(nir_variable *var, uint64_t *slots_used,
                    uint64_t slots_used_mask, unsigned num_slots)
{
   unsigned loc_offset = var->data.patch ? VARYING_SLOT_PATCH0 : 0;

   slots_used[var->data.patch ? 1 : 0] |= slots_used_mask &
      BITFIELD64_RANGE(var->data.location - loc_offset, num_slots);
}

static void
mark_used_slot(nir_variable *var, uint64_t *slots_used, unsigned offset)
{
   unsigned loc_offset = var->data.patch ? VARYING_SLOT_PATCH0 : 0;

   slots_used[var->data.patch ? 1 : 0] |=
      BITFIELD64_BIT(var->data.location - loc_offset + offset);
}

static void
remap_slots_and_components(nir_shader *shader, nir_variable_mode mode,
                           struct varying_loc (*remap)[4],
                           uint64_t *slots_used, uint64_t *out_slots_read,
                           uint32_t *p_slots_used, uint32_t *p_out_slots_read)
 {
   const gl_shader_stage stage = shader->info.stage;
   uint64_t out_slots_read_tmp[2] = {0};
   uint64_t slots_used_tmp[2] = {0};

   /* We don't touch builtins so just copy the bitmask */
   slots_used_tmp[0] = *slots_used & BITFIELD64_RANGE(0, VARYING_SLOT_VAR0);

   nir_foreach_variable_with_modes(var, shader, mode) {
      assert(var->data.location >= 0);

      /* Only remap things that aren't built-ins */
      if (var->data.location >= VARYING_SLOT_VAR0 &&
          var->data.location - VARYING_SLOT_VAR0 < MAX_VARYINGS_INCL_PATCH) {

         const struct glsl_type *type = var->type;
         if (nir_is_per_vertex_io(var, stage) || var->data.per_view) {
            assert(glsl_type_is_array(type));
            type = glsl_get_array_element(type);
         }

         unsigned num_slots = glsl_count_attribute_slots(type, false);
         bool used_across_stages = false;
         bool outputs_read = false;

         unsigned location = var->data.location - VARYING_SLOT_VAR0;
         struct varying_loc *new_loc = &remap[location][var->data.location_frac];

         unsigned loc_offset = var->data.patch ? VARYING_SLOT_PATCH0 : 0;
         uint64_t used = var->data.patch ? *p_slots_used : *slots_used;
         uint64_t outs_used =
            var->data.patch ? *p_out_slots_read : *out_slots_read;
         uint64_t slots =
            BITFIELD64_RANGE(var->data.location - loc_offset, num_slots);

         if (slots & used)
            used_across_stages = true;

         if (slots & outs_used)
            outputs_read = true;

         if (new_loc->location) {
            var->data.location = new_loc->location;
            var->data.location_frac = new_loc->component;
         }

         if (var->data.always_active_io) {
            /* We can't apply link time optimisations (specifically array
             * splitting) to these so we need to copy the existing mask
             * otherwise we will mess up the mask for things like partially
             * marked arrays.
             */
            if (used_across_stages)
               mark_all_used_slots(var, slots_used_tmp, used, num_slots);

            if (outputs_read) {
               mark_all_used_slots(var, out_slots_read_tmp, outs_used,
                                   num_slots);
            }
         } else {
            for (unsigned i = 0; i < num_slots; i++) {
               if (used_across_stages)
                  mark_used_slot(var, slots_used_tmp, i);

               if (outputs_read)
                  mark_used_slot(var, out_slots_read_tmp, i);
            }
         }
      }
   }

   *slots_used = slots_used_tmp[0];
   *out_slots_read = out_slots_read_tmp[0];
   *p_slots_used = slots_used_tmp[1];
   *p_out_slots_read = out_slots_read_tmp[1];
}

struct varying_component {
   nir_variable *var;
   uint8_t interp_type;
   uint8_t interp_loc;
   bool is_32bit;
   bool is_patch;
   bool is_intra_stage_only;
   bool initialised;
};

static int
cmp_varying_component(const void *comp1_v, const void *comp2_v)
{
   struct varying_component *comp1 = (struct varying_component *) comp1_v;
   struct varying_component *comp2 = (struct varying_component *) comp2_v;

   /* We want patches to be order at the end of the array */
   if (comp1->is_patch != comp2->is_patch)
      return comp1->is_patch ? 1 : -1;

   /* We want to try to group together TCS outputs that are only read by other
    * TCS invocations and not consumed by the follow stage.
    */
   if (comp1->is_intra_stage_only != comp2->is_intra_stage_only)
      return comp1->is_intra_stage_only ? 1 : -1;

   /* We can only pack varyings with matching interpolation types so group
    * them together.
    */
   if (comp1->interp_type != comp2->interp_type)
      return comp1->interp_type - comp2->interp_type;

   /* Interpolation loc must match also. */
   if (comp1->interp_loc != comp2->interp_loc)
      return comp1->interp_loc - comp2->interp_loc;

   /* If everything else matches just use the original location to sort */
   return comp1->var->data.location - comp2->var->data.location;
}

static void
gather_varying_component_info(nir_shader *producer, nir_shader *consumer,
                              struct varying_component **varying_comp_info,
                              unsigned *varying_comp_info_size,
                              bool default_to_smooth_interp)
{
   unsigned store_varying_info_idx[MAX_VARYINGS_INCL_PATCH][4] = {{0}};
   unsigned num_of_comps_to_pack = 0;

   /* Count the number of varying that can be packed and create a mapping
    * of those varyings to the array we will pass to qsort.
    */
   nir_foreach_shader_out_variable(var, producer) {

      /* Only remap things that aren't builtins. */
      if (var->data.location >= VARYING_SLOT_VAR0 &&
          var->data.location - VARYING_SLOT_VAR0 < MAX_VARYINGS_INCL_PATCH) {

         /* We can't repack xfb varyings. */
         if (var->data.always_active_io)
            continue;

         const struct glsl_type *type = var->type;
         if (nir_is_per_vertex_io(var, producer->info.stage) || var->data.per_view) {
            assert(glsl_type_is_array(type));
            type = glsl_get_array_element(type);
         }

         if (!is_packing_supported_for_type(type))
            continue;

         unsigned loc = var->data.location - VARYING_SLOT_VAR0;
         store_varying_info_idx[loc][var->data.location_frac] =
            ++num_of_comps_to_pack;
      }
   }

   *varying_comp_info_size = num_of_comps_to_pack;
   *varying_comp_info = rzalloc_array(NULL, struct varying_component,
                                      num_of_comps_to_pack);

   nir_function_impl *impl = nir_shader_get_entrypoint(consumer);

   /* Walk over the shader and populate the varying component info array */
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_deref &&
             intr->intrinsic != nir_intrinsic_interp_deref_at_centroid &&
             intr->intrinsic != nir_intrinsic_interp_deref_at_sample &&
             intr->intrinsic != nir_intrinsic_interp_deref_at_offset &&
             intr->intrinsic != nir_intrinsic_interp_deref_at_vertex)
            continue;

         nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
         if (deref->mode != nir_var_shader_in)
            continue;

         /* We only remap things that aren't builtins. */
         nir_variable *in_var = nir_deref_instr_get_variable(deref);
         if (in_var->data.location < VARYING_SLOT_VAR0)
            continue;

         unsigned location = in_var->data.location - VARYING_SLOT_VAR0;
         if (location >= MAX_VARYINGS_INCL_PATCH)
            continue;

         unsigned var_info_idx =
            store_varying_info_idx[location][in_var->data.location_frac];
         if (!var_info_idx)
            continue;

         struct varying_component *vc_info =
            &(*varying_comp_info)[var_info_idx-1];

         if (!vc_info->initialised) {
            const struct glsl_type *type = in_var->type;
            if (nir_is_per_vertex_io(in_var, consumer->info.stage) ||
                in_var->data.per_view) {
               assert(glsl_type_is_array(type));
               type = glsl_get_array_element(type);
            }

            vc_info->var = in_var;
            vc_info->interp_type =
               get_interp_type(in_var, type, default_to_smooth_interp);
            vc_info->interp_loc = get_interp_loc(in_var);
            vc_info->is_32bit = glsl_type_is_32bit(type);
            vc_info->is_patch = in_var->data.patch;
            vc_info->is_intra_stage_only = false;
            vc_info->initialised = true;
         }
      }
   }

   /* Walk over the shader and populate the varying component info array
    * for varyings which are read by other TCS instances but are not consumed
    * by the TES.
    */
   if (producer->info.stage == MESA_SHADER_TESS_CTRL) {
      impl = nir_shader_get_entrypoint(producer);

      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_deref)
               continue;

            nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
            if (deref->mode != nir_var_shader_out)
               continue;

            /* We only remap things that aren't builtins. */
            nir_variable *out_var = nir_deref_instr_get_variable(deref);
            if (out_var->data.location < VARYING_SLOT_VAR0)
               continue;

            unsigned location = out_var->data.location - VARYING_SLOT_VAR0;
            if (location >= MAX_VARYINGS_INCL_PATCH)
               continue;

            unsigned var_info_idx =
               store_varying_info_idx[location][out_var->data.location_frac];
            if (!var_info_idx) {
               /* Something went wrong, the shader interfaces didn't match, so
                * abandon packing. This can happen for example when the
                * inputs are scalars but the outputs are struct members.
                */
               *varying_comp_info_size = 0;
               break;
            }

            struct varying_component *vc_info =
               &(*varying_comp_info)[var_info_idx-1];

            if (!vc_info->initialised) {
               const struct glsl_type *type = out_var->type;
               if (nir_is_per_vertex_io(out_var, producer->info.stage)) {
                  assert(glsl_type_is_array(type));
                  type = glsl_get_array_element(type);
               }

               vc_info->var = out_var;
               vc_info->interp_type =
                  get_interp_type(out_var, type, default_to_smooth_interp);
               vc_info->interp_loc = get_interp_loc(out_var);
               vc_info->is_32bit = glsl_type_is_32bit(type);
               vc_info->is_patch = out_var->data.patch;
               vc_info->is_intra_stage_only = true;
               vc_info->initialised = true;
            }
         }
      }
   }

   for (unsigned i = 0; i < *varying_comp_info_size; i++ ) {
      struct varying_component *vc_info = &(*varying_comp_info)[i];
      if (!vc_info->initialised) {
         /* Something went wrong, the shader interfaces didn't match, so
          * abandon packing. This can happen for example when the outputs are
          * scalars but the inputs are struct members.
          */
         *varying_comp_info_size = 0;
         break;
      }
   }
}

static void
assign_remap_locations(struct varying_loc (*remap)[4],
                       struct assigned_comps *assigned_comps,
                       struct varying_component *info,
                       unsigned *cursor, unsigned *comp,
                       unsigned max_location)
{
   unsigned tmp_cursor = *cursor;
   unsigned tmp_comp = *comp;

   for (; tmp_cursor < max_location; tmp_cursor++) {

      if (assigned_comps[tmp_cursor].comps) {
         /* We can only pack varyings with matching interpolation types,
          * interpolation loc must match also.
          * TODO: i965 can handle interpolation locations that don't match,
          * but the radeonsi nir backend handles everything as vec4s and so
          * expects this to be the same for all components. We could make this
          * check driver specfific or drop it if NIR ever become the only
          * radeonsi backend.
          */
         if (assigned_comps[tmp_cursor].interp_type != info->interp_type ||
             assigned_comps[tmp_cursor].interp_loc != info->interp_loc) {
            tmp_comp = 0;
            continue;
         }

         /* We can only pack varyings with matching types, and the current
          * algorithm only supports packing 32-bit.
          */
         if (!assigned_comps[tmp_cursor].is_32bit) {
            tmp_comp = 0;
            continue;
         }

         while (tmp_comp < 4 &&
                (assigned_comps[tmp_cursor].comps & (1 << tmp_comp))) {
            tmp_comp++;
         }
      }

      if (tmp_comp == 4) {
         tmp_comp = 0;
         continue;
      }

      unsigned location = info->var->data.location - VARYING_SLOT_VAR0;

      /* Once we have assigned a location mark it as used */
      assigned_comps[tmp_cursor].comps |= (1 << tmp_comp);
      assigned_comps[tmp_cursor].interp_type = info->interp_type;
      assigned_comps[tmp_cursor].interp_loc = info->interp_loc;
      assigned_comps[tmp_cursor].is_32bit = info->is_32bit;

      /* Assign remap location */
      remap[location][info->var->data.location_frac].component = tmp_comp++;
      remap[location][info->var->data.location_frac].location =
         tmp_cursor + VARYING_SLOT_VAR0;

      break;
   }

   *cursor = tmp_cursor;
   *comp = tmp_comp;
}

/* If there are empty components in the slot compact the remaining components
 * as close to component 0 as possible. This will make it easier to fill the
 * empty components with components from a different slot in a following pass.
 */
static void
compact_components(nir_shader *producer, nir_shader *consumer,
                   struct assigned_comps *assigned_comps,
                   bool default_to_smooth_interp)
{
   struct varying_loc remap[MAX_VARYINGS_INCL_PATCH][4] = {{{0}, {0}}};
   struct varying_component *varying_comp_info;
   unsigned varying_comp_info_size;

   /* Gather varying component info */
   gather_varying_component_info(producer, consumer, &varying_comp_info,
                                 &varying_comp_info_size,
                                 default_to_smooth_interp);

   /* Sort varying components. */
   qsort(varying_comp_info, varying_comp_info_size,
         sizeof(struct varying_component), cmp_varying_component);

   unsigned cursor = 0;
   unsigned comp = 0;

   /* Set the remap array based on the sorted components */
   for (unsigned i = 0; i < varying_comp_info_size; i++ ) {
      struct varying_component *info = &varying_comp_info[i];

      assert(info->is_patch || cursor < MAX_VARYING);
      if (info->is_patch) {
         /* The list should be sorted with all non-patch inputs first followed
          * by patch inputs.  When we hit our first patch input, we need to
          * reset the cursor to MAX_VARYING so we put them in the right slot.
          */
         if (cursor < MAX_VARYING) {
            cursor = MAX_VARYING;
            comp = 0;
         }

         assign_remap_locations(remap, assigned_comps, info,
                                &cursor, &comp, MAX_VARYINGS_INCL_PATCH);
      } else {
         assign_remap_locations(remap, assigned_comps, info,
                                &cursor, &comp, MAX_VARYING);

         /* Check if we failed to assign a remap location. This can happen if
          * for example there are a bunch of unmovable components with
          * mismatching interpolation types causing us to skip over locations
          * that would have been useful for packing later components.
          * The solution is to iterate over the locations again (this should
          * happen very rarely in practice).
          */
         if (cursor == MAX_VARYING) {
            cursor = 0;
            comp = 0;
            assign_remap_locations(remap, assigned_comps, info,
                                   &cursor, &comp, MAX_VARYING);
         }
      }
   }

   ralloc_free(varying_comp_info);

   uint64_t zero = 0;
   uint32_t zero32 = 0;
   remap_slots_and_components(consumer, nir_var_shader_in, remap,
                              &consumer->info.inputs_read, &zero,
                              &consumer->info.patch_inputs_read, &zero32);
   remap_slots_and_components(producer, nir_var_shader_out, remap,
                              &producer->info.outputs_written,
                              &producer->info.outputs_read,
                              &producer->info.patch_outputs_written,
                              &producer->info.patch_outputs_read);
}

/* We assume that this has been called more-or-less directly after
 * remove_unused_varyings.  At this point, all of the varyings that we
 * aren't going to be using have been completely removed and the
 * inputs_read and outputs_written fields in nir_shader_info reflect
 * this.  Therefore, the total set of valid slots is the OR of the two
 * sets of varyings;  this accounts for varyings which one side may need
 * to read/write even if the other doesn't.  This can happen if, for
 * instance, an array is used indirectly from one side causing it to be
 * unsplittable but directly from the other.
 */
void
nir_compact_varyings(nir_shader *producer, nir_shader *consumer,
                     bool default_to_smooth_interp)
{
   assert(producer->info.stage != MESA_SHADER_FRAGMENT);
   assert(consumer->info.stage != MESA_SHADER_VERTEX);

   struct assigned_comps assigned_comps[MAX_VARYINGS_INCL_PATCH] = {{0}};

   get_unmoveable_components_masks(producer, nir_var_shader_out,
                                   assigned_comps,
                                   producer->info.stage,
                                   default_to_smooth_interp);
   get_unmoveable_components_masks(consumer, nir_var_shader_in,
                                   assigned_comps,
                                   consumer->info.stage,
                                   default_to_smooth_interp);

   compact_components(producer, consumer, assigned_comps,
                      default_to_smooth_interp);
}

/*
 * Mark XFB varyings as always_active_io in the consumer so the linking opts
 * don't touch them.
 */
void
nir_link_xfb_varyings(nir_shader *producer, nir_shader *consumer)
{
   nir_variable *input_vars[MAX_VARYING] = { 0 };

   nir_foreach_shader_in_variable(var, consumer) {
      if (var->data.location >= VARYING_SLOT_VAR0 &&
          var->data.location - VARYING_SLOT_VAR0 < MAX_VARYING) {

         unsigned location = var->data.location - VARYING_SLOT_VAR0;
         input_vars[location] = var;
      }
   }

   nir_foreach_shader_out_variable(var, producer) {
      if (var->data.location >= VARYING_SLOT_VAR0 &&
          var->data.location - VARYING_SLOT_VAR0 < MAX_VARYING) {

         if (!var->data.always_active_io)
            continue;

         unsigned location = var->data.location - VARYING_SLOT_VAR0;
         if (input_vars[location]) {
            input_vars[location]->data.always_active_io = true;
         }
      }
   }
}

static bool
does_varying_match(nir_variable *out_var, nir_variable *in_var)
{
   return in_var->data.location == out_var->data.location &&
          in_var->data.location_frac == out_var->data.location_frac;
}

static nir_variable *
get_matching_input_var(nir_shader *consumer, nir_variable *out_var)
{
   nir_foreach_shader_in_variable(var, consumer) {
      if (does_varying_match(out_var, var))
         return var;
   }

   return NULL;
}

static bool
can_replace_varying(nir_variable *out_var)
{
   /* Skip types that require more complex handling.
    * TODO: add support for these types.
    */
   if (glsl_type_is_array(out_var->type) ||
       glsl_type_is_dual_slot(out_var->type) ||
       glsl_type_is_matrix(out_var->type) ||
       glsl_type_is_struct_or_ifc(out_var->type))
      return false;

   /* Limit this pass to scalars for now to keep things simple. Most varyings
    * should have been lowered to scalars at this point anyway.
    */
   if (!glsl_type_is_scalar(out_var->type))
      return false;

   if (out_var->data.location < VARYING_SLOT_VAR0 ||
       out_var->data.location - VARYING_SLOT_VAR0 >= MAX_VARYING)
      return false;

   return true;
}

static bool
replace_constant_input(nir_shader *shader, nir_intrinsic_instr *store_intr)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_variable *out_var =
      nir_deref_instr_get_variable(nir_src_as_deref(store_intr->src[0]));

   bool progress = false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_deref)
            continue;

         nir_deref_instr *in_deref = nir_src_as_deref(intr->src[0]);
         if (in_deref->mode != nir_var_shader_in)
            continue;

         nir_variable *in_var = nir_deref_instr_get_variable(in_deref);

         if (!does_varying_match(out_var, in_var))
            continue;

         b.cursor = nir_before_instr(instr);

         nir_load_const_instr *out_const =
            nir_instr_as_load_const(store_intr->src[1].ssa->parent_instr);

         /* Add new const to replace the input */
         nir_ssa_def *nconst = nir_build_imm(&b, store_intr->num_components,
                                             intr->dest.ssa.bit_size,
                                             out_const->value);

         nir_ssa_def_rewrite_uses(&intr->dest.ssa, nir_src_for_ssa(nconst));

         progress = true;
      }
   }

   return progress;
}

static bool
replace_duplicate_input(nir_shader *shader, nir_variable *input_var,
                         nir_intrinsic_instr *dup_store_intr)
{
   assert(input_var);

   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_variable *dup_out_var =
      nir_deref_instr_get_variable(nir_src_as_deref(dup_store_intr->src[0]));

   bool progress = false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_deref)
            continue;

         nir_deref_instr *in_deref = nir_src_as_deref(intr->src[0]);
         if (in_deref->mode != nir_var_shader_in)
            continue;

         nir_variable *in_var = nir_deref_instr_get_variable(in_deref);

         if (!does_varying_match(dup_out_var, in_var) ||
             in_var->data.interpolation != input_var->data.interpolation ||
             get_interp_loc(in_var) != get_interp_loc(input_var))
            continue;

         b.cursor = nir_before_instr(instr);

         nir_ssa_def *load = nir_load_var(&b, input_var);
         nir_ssa_def_rewrite_uses(&intr->dest.ssa, nir_src_for_ssa(load));

         progress = true;
      }
   }

   return progress;
}

bool
nir_link_opt_varyings(nir_shader *producer, nir_shader *consumer)
{
   /* TODO: Add support for more shader stage combinations */
   if (consumer->info.stage != MESA_SHADER_FRAGMENT ||
       (producer->info.stage != MESA_SHADER_VERTEX &&
        producer->info.stage != MESA_SHADER_TESS_EVAL))
      return false;

   bool progress = false;

   nir_function_impl *impl = nir_shader_get_entrypoint(producer);

   struct hash_table *varying_values = _mesa_pointer_hash_table_create(NULL);

   /* If we find a store in the last block of the producer we can be sure this
    * is the only possible value for this output.
    */
   nir_block *last_block = nir_impl_last_block(impl);
   nir_foreach_instr_reverse(instr, last_block) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

      if (intr->intrinsic != nir_intrinsic_store_deref)
         continue;

      nir_deref_instr *out_deref = nir_src_as_deref(intr->src[0]);
      if (out_deref->mode != nir_var_shader_out)
         continue;

      nir_variable *out_var = nir_deref_instr_get_variable(out_deref);
      if (!can_replace_varying(out_var))
         continue;

      if (intr->src[1].ssa->parent_instr->type == nir_instr_type_load_const) {
         progress |= replace_constant_input(consumer, intr);
      } else {
         struct hash_entry *entry =
               _mesa_hash_table_search(varying_values, intr->src[1].ssa);
         if (entry) {
            progress |= replace_duplicate_input(consumer,
                                                (nir_variable *) entry->data,
                                                intr);
         } else {
            nir_variable *in_var = get_matching_input_var(consumer, out_var);
            if (in_var) {
               _mesa_hash_table_insert(varying_values, intr->src[1].ssa,
                                       in_var);
            }
         }
      }
   }

   _mesa_hash_table_destroy(varying_values, NULL);

   return progress;
}

/* TODO any better helper somewhere to sort a list? */

static void
insert_sorted(struct exec_list *var_list, nir_variable *new_var)
{
   nir_foreach_variable_in_list(var, var_list) {
      if (var->data.location > new_var->data.location) {
         exec_node_insert_node_before(&var->node, &new_var->node);
         return;
      }
   }
   exec_list_push_tail(var_list, &new_var->node);
}

static void
sort_varyings(nir_shader *shader, nir_variable_mode mode,
              struct exec_list *sorted_list)
{
   exec_list_make_empty(sorted_list);
   nir_foreach_variable_with_modes_safe(var, shader, mode) {
      exec_node_remove(&var->node);
      insert_sorted(sorted_list, var);
   }
}

void
nir_assign_io_var_locations(nir_shader *shader, nir_variable_mode mode,
                            unsigned *size, gl_shader_stage stage)
{
   unsigned location = 0;
   unsigned assigned_locations[VARYING_SLOT_TESS_MAX];
   uint64_t processed_locs[2] = {0};

   struct exec_list io_vars;
   sort_varyings(shader, mode, &io_vars);

   int UNUSED last_loc = 0;
   bool last_partial = false;
   nir_foreach_variable_in_list(var, &io_vars) {
      const struct glsl_type *type = var->type;
      if (nir_is_per_vertex_io(var, stage) || var->data.per_view) {
         assert(glsl_type_is_array(type));
         type = glsl_get_array_element(type);
      }

      int base;
      if (var->data.mode == nir_var_shader_in && stage == MESA_SHADER_VERTEX)
         base = VERT_ATTRIB_GENERIC0;
      else if (var->data.mode == nir_var_shader_out &&
               stage == MESA_SHADER_FRAGMENT)
         base = FRAG_RESULT_DATA0;
      else
         base = VARYING_SLOT_VAR0;

      unsigned var_size;
      if (var->data.compact) {
         /* If we are inside a partial compact,
          * don't allow another compact to be in this slot
          * if it starts at component 0.
          */
         if (last_partial && var->data.location_frac == 0) {
            location++;
         }

         /* compact variables must be arrays of scalars */
         assert(glsl_type_is_array(type));
         assert(glsl_type_is_scalar(glsl_get_array_element(type)));
         unsigned start = 4 * location + var->data.location_frac;
         unsigned end = start + glsl_get_length(type);
         var_size = end / 4 - location;
         last_partial = end % 4 != 0;
      } else {
         /* Compact variables bypass the normal varying compacting pass,
          * which means they cannot be in the same vec4 slot as a normal
          * variable. If part of the current slot is taken up by a compact
          * variable, we need to go to the next one.
          */
         if (last_partial) {
            location++;
            last_partial = false;
         }
         var_size = glsl_count_attribute_slots(type, false);
      }

      /* Builtins don't allow component packing so we only need to worry about
       * user defined varyings sharing the same location.
       */
      bool processed = false;
      if (var->data.location >= base) {
         unsigned glsl_location = var->data.location - base;

         for (unsigned i = 0; i < var_size; i++) {
            if (processed_locs[var->data.index] &
                ((uint64_t)1 << (glsl_location + i)))
               processed = true;
            else
               processed_locs[var->data.index] |=
                  ((uint64_t)1 << (glsl_location + i));
         }
      }

      /* Because component packing allows varyings to share the same location
       * we may have already have processed this location.
       */
      if (processed) {
         unsigned driver_location = assigned_locations[var->data.location];
         var->data.driver_location = driver_location;

         /* An array may be packed such that is crosses multiple other arrays
          * or variables, we need to make sure we have allocated the elements
          * consecutively if the previously proccessed var was shorter than
          * the current array we are processing.
          *
          * NOTE: The code below assumes the var list is ordered in ascending
          * location order.
          */
         assert(last_loc <= var->data.location);
         last_loc = var->data.location;
         unsigned last_slot_location = driver_location + var_size;
         if (last_slot_location > location) {
            unsigned num_unallocated_slots = last_slot_location - location;
            unsigned first_unallocated_slot = var_size - num_unallocated_slots;
            for (unsigned i = first_unallocated_slot; i < var_size; i++) {
               assigned_locations[var->data.location + i] = location;
               location++;
            }
         }
         continue;
      }

      for (unsigned i = 0; i < var_size; i++) {
         assigned_locations[var->data.location + i] = location + i;
      }

      var->data.driver_location = location;
      location += var_size;
   }

   if (last_partial)
      location++;

   exec_list_append(&shader->variables, &io_vars);
   *size = location;
}

static uint64_t
get_linked_variable_location(unsigned location, bool patch)
{
   if (!patch)
      return location;

   /* Reserve locations 0...3 for special patch variables
    * like tess factors and bounding boxes, and the generic patch
    * variables will come after them.
    */
   if (location >= VARYING_SLOT_PATCH0)
      return location - VARYING_SLOT_PATCH0 + 4;
   else if (location >= VARYING_SLOT_TESS_LEVEL_OUTER &&
            location <= VARYING_SLOT_BOUNDING_BOX1)
      return location - VARYING_SLOT_TESS_LEVEL_OUTER;
   else
      unreachable("Unsupported variable in get_linked_variable_location.");
}

static uint64_t
get_linked_variable_io_mask(nir_variable *variable, gl_shader_stage stage)
{
   const struct glsl_type *type = variable->type;

   if (nir_is_per_vertex_io(variable, stage)) {
      assert(glsl_type_is_array(type));
      type = glsl_get_array_element(type);
   }

   unsigned slots = glsl_count_attribute_slots(type, false);
   if (variable->data.compact) {
      unsigned component_count = variable->data.location_frac + glsl_get_length(type);
      slots = DIV_ROUND_UP(component_count, 4);
   }

   uint64_t mask = u_bit_consecutive64(0, slots);
   return mask;
}

nir_linked_io_var_info
nir_assign_linked_io_var_locations(nir_shader *producer, nir_shader *consumer)
{
   assert(producer);
   assert(consumer);

   uint64_t producer_output_mask = 0;
   uint64_t producer_patch_output_mask = 0;

   nir_foreach_shader_out_variable(variable, producer) {
      uint64_t mask = get_linked_variable_io_mask(variable, producer->info.stage);
      uint64_t loc = get_linked_variable_location(variable->data.location, variable->data.patch);

      if (variable->data.patch)
         producer_patch_output_mask |= mask << loc;
      else
         producer_output_mask |= mask << loc;
   }

   uint64_t consumer_input_mask = 0;
   uint64_t consumer_patch_input_mask = 0;

   nir_foreach_shader_in_variable(variable, consumer) {
      uint64_t mask = get_linked_variable_io_mask(variable, consumer->info.stage);
      uint64_t loc = get_linked_variable_location(variable->data.location, variable->data.patch);

      if (variable->data.patch)
         consumer_patch_input_mask |= mask << loc;
      else
         consumer_input_mask |= mask << loc;
   }

   uint64_t io_mask = producer_output_mask | consumer_input_mask;
   uint64_t patch_io_mask = producer_patch_output_mask | consumer_patch_input_mask;

   nir_foreach_shader_out_variable(variable, producer) {
      uint64_t loc = get_linked_variable_location(variable->data.location, variable->data.patch);

      if (variable->data.patch)
         variable->data.driver_location = util_bitcount64(patch_io_mask & u_bit_consecutive64(0, loc)) * 4;
      else
         variable->data.driver_location = util_bitcount64(io_mask & u_bit_consecutive64(0, loc)) * 4;
   }

   nir_foreach_shader_in_variable(variable, consumer) {
      uint64_t loc = get_linked_variable_location(variable->data.location, variable->data.patch);

      if (variable->data.patch)
         variable->data.driver_location = util_bitcount64(patch_io_mask & u_bit_consecutive64(0, loc)) * 4;
      else
         variable->data.driver_location = util_bitcount64(io_mask & u_bit_consecutive64(0, loc)) * 4;
   }

   nir_linked_io_var_info result = {
      .num_linked_io_vars = util_bitcount64(io_mask),
      .num_linked_patch_io_vars = util_bitcount64(patch_io_mask),
   };

   return result;
}
