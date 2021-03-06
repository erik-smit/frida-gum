/*
 * Copyright (C) 2014-2015 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "guminterceptor-priv.h"

#include "gumarm64reader.h"
#include "gumarm64relocator.h"
#include "gumarm64writer.h"
#include "gumlibc.h"
#include "gummemory.h"

#include <string.h>
#include <unistd.h>

#define GUM_ARM64_LOGICAL_PAGE_SIZE 4096

#define GUM_FRAME_OFFSET_CPU_CONTEXT 8
#define GUM_FRAME_OFFSET_NEXT_HOP \
    (GUM_FRAME_OFFSET_CPU_CONTEXT + (33 * 8) + (8 * 16))

typedef struct _GumArm64FunctionContextData GumArm64FunctionContextData;

struct _GumInterceptorBackend
{
  GumCodeAllocator * allocator;

  GumArm64Writer writer;
  GumArm64Relocator relocator;

  GumCodeSlice * enter_thunk;
  GumCodeSlice * leave_thunk;
};

struct _GumArm64FunctionContextData
{
  guint redirect_code_size;
  arm64_reg scratch_reg;
};

G_STATIC_ASSERT (sizeof (GumArm64FunctionContextData)
    <= sizeof (GumFunctionContextBackendData));

static void gum_interceptor_backend_create_thunks (
    GumInterceptorBackend * self);
static void gum_interceptor_backend_destroy_thunks (
    GumInterceptorBackend * self);

static void gum_emit_enter_thunk (GumArm64Writer * aw);
static void gum_emit_leave_thunk (GumArm64Writer * aw);

static void gum_emit_prolog (GumArm64Writer * aw);
static void gum_emit_epilog (GumArm64Writer * aw);

GumInterceptorBackend *
_gum_interceptor_backend_create (GumCodeAllocator * allocator)
{
  GumInterceptorBackend * backend;

  backend = g_slice_new (GumInterceptorBackend);
  backend->allocator = allocator;

  gum_arm64_writer_init (&backend->writer, NULL);
  gum_arm64_relocator_init (&backend->relocator, NULL, &backend->writer);

  gum_interceptor_backend_create_thunks (backend);

  return backend;
}

void
_gum_interceptor_backend_destroy (GumInterceptorBackend * backend)
{
  gum_interceptor_backend_destroy_thunks (backend);

  gum_arm64_relocator_clear (&backend->relocator);
  gum_arm64_writer_clear (&backend->writer);

  g_slice_free (GumInterceptorBackend, backend);
}

static gboolean
gum_interceptor_backend_prepare_trampoline (GumInterceptorBackend * self,
                                            GumFunctionContext * ctx,
                                            gboolean * need_deflector)
{
  GumArm64FunctionContextData * data = (GumArm64FunctionContextData *)
      &ctx->backend_data;
  gpointer function_address = ctx->function_address;
  guint redirect_limit;

  *need_deflector = FALSE;

  if (gum_arm64_relocator_can_relocate (function_address, 16,
      GUM_SCENARIO_ONLINE, &redirect_limit, &data->scratch_reg))
  {
    data->redirect_code_size = 16;

    ctx->trampoline_slice = gum_code_allocator_alloc_slice (self->allocator);
  }
  else
  {
    GumAddressSpec spec;
    gsize alignment;

    if (redirect_limit >= 8)
    {
      data->redirect_code_size = 8;

      spec.near_address = GSIZE_TO_POINTER (
          GPOINTER_TO_SIZE (function_address) &
          ~((gsize) (GUM_ARM64_LOGICAL_PAGE_SIZE - 1)));
      spec.max_distance = GUM_ARM64_ADRP_MAX_DISTANCE;
      alignment = GUM_ARM64_LOGICAL_PAGE_SIZE;
    }
    else if (redirect_limit >= 4)
    {
      data->redirect_code_size = 4;

      spec.near_address = function_address;
      spec.max_distance = GUM_ARM64_B_MAX_DISTANCE;
      alignment = 0;
    }
    else
    {
      return FALSE;
    }

    ctx->trampoline_slice = gum_code_allocator_try_alloc_slice_near (
        self->allocator, &spec, alignment);
    if (ctx->trampoline_slice == NULL)
    {
      ctx->trampoline_slice = gum_code_allocator_alloc_slice (self->allocator);
      *need_deflector = TRUE;
    }
  }

  if (data->scratch_reg == ARM64_REG_INVALID)
    return FALSE;

  return TRUE;
}

gboolean
_gum_interceptor_backend_create_trampoline (GumInterceptorBackend * self,
                                            GumFunctionContext * ctx)
{
  GumArm64Writer * aw = &self->writer;
  GumArm64Relocator * ar = &self->relocator;
  gpointer function_address = ctx->function_address;
  GumArm64FunctionContextData * data = (GumArm64FunctionContextData *)
      &ctx->backend_data;
  gboolean need_deflector;
  guint reloc_bytes;

  if (!gum_interceptor_backend_prepare_trampoline (self, ctx, &need_deflector))
    return FALSE;

  gum_arm64_writer_reset (aw, ctx->trampoline_slice->data);

  ctx->on_enter_trampoline = gum_arm64_writer_cur (aw);

  if (need_deflector)
  {
    GumAddressSpec caller;
    gpointer return_address;
    gboolean dedicated;

    caller.near_address = function_address + data->redirect_code_size - 4;
    caller.max_distance = GUM_ARM64_B_MAX_DISTANCE;

    return_address = function_address + data->redirect_code_size;

    dedicated = data->redirect_code_size == 4;

    ctx->trampoline_deflector = gum_code_allocator_alloc_deflector (
        self->allocator, &caller, return_address, ctx->on_enter_trampoline,
        dedicated);
    if (ctx->trampoline_deflector == NULL)
    {
      gum_code_slice_free (ctx->trampoline_slice);
      ctx->trampoline_slice = NULL;
      return FALSE;
    }

    gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X0, ARM64_REG_LR);
  }

  gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X17, GUM_ADDRESS (ctx));
  gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X16,
      GUM_ADDRESS (self->enter_thunk->data));
  gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);

  ctx->on_leave_trampoline = gum_arm64_writer_cur (aw);

  gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X17, GUM_ADDRESS (ctx));
  gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X16,
      GUM_ADDRESS (self->leave_thunk->data));
  gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);

  gum_arm64_writer_flush (aw);
  g_assert_cmpuint (gum_arm64_writer_offset (aw),
      <=, ctx->trampoline_slice->size);

  ctx->on_invoke_trampoline = gum_arm64_writer_cur (aw);

  gum_arm64_relocator_reset (ar, function_address, aw);

  do
  {
    reloc_bytes = gum_arm64_relocator_read_one (ar, NULL);
    g_assert_cmpuint (reloc_bytes, !=, 0);
  }
  while (reloc_bytes < data->redirect_code_size);

  gum_arm64_relocator_write_all (ar);

  if (!ar->eoi)
  {
    GumAddress resume_at;

    resume_at = GUM_ADDRESS (function_address) + reloc_bytes;
    gum_arm64_writer_put_ldr_reg_address (aw, data->scratch_reg, resume_at);
    gum_arm64_writer_put_br_reg (aw, data->scratch_reg);
  }

  gum_arm64_writer_flush (aw);
  g_assert_cmpuint (gum_arm64_writer_offset (aw),
      <=, ctx->trampoline_slice->size);

  ctx->overwritten_prologue_len = reloc_bytes;
  memcpy (ctx->overwritten_prologue, function_address, reloc_bytes);

  return TRUE;
}

void
_gum_interceptor_backend_destroy_trampoline (GumInterceptorBackend * self,
                                             GumFunctionContext * ctx)
{
  gum_code_slice_free (ctx->trampoline_slice);
  gum_code_deflector_free (ctx->trampoline_deflector);
  ctx->trampoline_slice = NULL;
  ctx->trampoline_deflector = NULL;
}

void
_gum_interceptor_backend_activate_trampoline (GumInterceptorBackend * self,
                                              GumFunctionContext * ctx,
                                              gpointer prologue)
{
  GumArm64Writer * aw = &self->writer;
  GumArm64FunctionContextData * data = (GumArm64FunctionContextData *)
      &ctx->backend_data;
  GumAddress on_enter = GUM_ADDRESS (ctx->on_enter_trampoline);

  gum_arm64_writer_reset (aw, prologue);
  aw->pc = GUM_ADDRESS (ctx->function_address);

  if (ctx->trampoline_deflector != NULL)
  {
    if (data->redirect_code_size == 8)
    {
      gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X0, ARM64_REG_LR);
      gum_arm64_writer_put_bl_imm (aw,
          GUM_ADDRESS (ctx->trampoline_deflector->trampoline));
    }
    else
    {
      g_assert_cmpuint (data->redirect_code_size, ==, 4);
      gum_arm64_writer_put_b_imm (aw,
          GUM_ADDRESS (ctx->trampoline_deflector->trampoline));
    }
  }
  else
  {
    switch (data->redirect_code_size)
    {
      case 4:
        gum_arm64_writer_put_b_imm (aw, on_enter);
        break;
      case 8:
        gum_arm64_writer_put_adrp_reg_address (aw, ARM64_REG_X16, on_enter);
        gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);
        break;
      case 16:
        gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X16, on_enter);
        gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);
        break;
      default:
        g_assert_not_reached ();
    }
  }

  gum_arm64_writer_flush (aw);
  g_assert_cmpuint (gum_arm64_writer_offset (aw), <=, data->redirect_code_size);
}

void
_gum_interceptor_backend_deactivate_trampoline (GumInterceptorBackend * self,
                                                GumFunctionContext * ctx,
                                                gpointer prologue)
{
  (void) self;

  memcpy (prologue, ctx->overwritten_prologue, ctx->overwritten_prologue_len);
}

gpointer
_gum_interceptor_backend_get_function_address (GumFunctionContext * ctx)
{
  return ctx->function_address;
}

gpointer
_gum_interceptor_backend_resolve_redirect (GumInterceptorBackend * self,
                                           gpointer address)
{
  return gum_arm64_reader_try_get_relative_jump_target (address);
}

static void
gum_interceptor_backend_create_thunks (GumInterceptorBackend * self)
{
  GumArm64Writer * aw = &self->writer;

  self->enter_thunk = gum_code_allocator_alloc_slice (self->allocator);
  gum_arm64_writer_reset (aw, self->enter_thunk->data);
  gum_emit_enter_thunk (aw);
  gum_arm64_writer_flush (aw);
  g_assert_cmpuint (gum_arm64_writer_offset (aw), <=, self->enter_thunk->size);

  self->leave_thunk = gum_code_allocator_alloc_slice (self->allocator);
  gum_arm64_writer_reset (aw, self->leave_thunk->data);
  gum_emit_leave_thunk (aw);
  gum_arm64_writer_flush (aw);
  g_assert_cmpuint (gum_arm64_writer_offset (aw), <=, self->leave_thunk->size);
}

static void
gum_interceptor_backend_destroy_thunks (GumInterceptorBackend * self)
{
  gum_code_slice_free (self->leave_thunk);

  gum_code_slice_free (self->enter_thunk);
}

static void
gum_emit_enter_thunk (GumArm64Writer * aw)
{
  gum_emit_prolog (aw);

  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X1, ARM64_REG_SP,
      GUM_FRAME_OFFSET_CPU_CONTEXT);
  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X2, ARM64_REG_SP,
      GUM_FRAME_OFFSET_CPU_CONTEXT + G_STRUCT_OFFSET (GumCpuContext, lr));
  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X3, ARM64_REG_SP,
      GUM_FRAME_OFFSET_NEXT_HOP);

  gum_arm64_writer_put_call_address_with_arguments (aw,
      GUM_ADDRESS (_gum_function_context_begin_invocation), 4,
      GUM_ARG_REGISTER, ARM64_REG_X17,
      GUM_ARG_REGISTER, ARM64_REG_X1,
      GUM_ARG_REGISTER, ARM64_REG_X2,
      GUM_ARG_REGISTER, ARM64_REG_X3);

  gum_emit_epilog (aw);
}

static void
gum_emit_leave_thunk (GumArm64Writer * aw)
{
  gum_emit_prolog (aw);

  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X1, ARM64_REG_SP,
      GUM_FRAME_OFFSET_CPU_CONTEXT);
  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X2, ARM64_REG_SP,
      GUM_FRAME_OFFSET_NEXT_HOP);

  gum_arm64_writer_put_call_address_with_arguments (aw,
      GUM_ADDRESS (_gum_function_context_end_invocation), 3,
      GUM_ARG_REGISTER, ARM64_REG_X17,
      GUM_ARG_REGISTER, ARM64_REG_X1,
      GUM_ARG_REGISTER, ARM64_REG_X2);

  gum_emit_epilog (aw);
}

static void
gum_emit_prolog (GumArm64Writer * aw)
{
  /*
   * Set up our stack frame:
   *
   * [next_hop]
   * [cpu_context]
   */

  /* reserve space for next_hop */
  gum_arm64_writer_put_sub_reg_reg_imm (aw, ARM64_REG_SP, ARM64_REG_SP, 16);

  /* push {q0-q7}; store vector registers (for now only the clobberable ones) */
  gum_arm64_writer_put_instruction (aw, 0xadbf1fe6);
  gum_arm64_writer_put_instruction (aw, 0xadbf17e4);
  gum_arm64_writer_put_instruction (aw, 0xadbf0fe2);
  gum_arm64_writer_put_instruction (aw, 0xadbf07e0);

  /* upper part */
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_FP, ARM64_REG_LR);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X27, ARM64_REG_X28);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X25, ARM64_REG_X26);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X23, ARM64_REG_X24);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X21, ARM64_REG_X22);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X19, ARM64_REG_X20);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X17, ARM64_REG_X18);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X15, ARM64_REG_X16);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X13, ARM64_REG_X14);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X11, ARM64_REG_X12);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X9, ARM64_REG_X10);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X7, ARM64_REG_X8);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X5, ARM64_REG_X6);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X3, ARM64_REG_X4);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X1, ARM64_REG_X2);

  /* SP + X0 */
  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X1,
      ARM64_REG_SP, (30 * 8) + (8 * 16) + 16);
  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X1, ARM64_REG_X0);

  /* alignment padding + dummy PC */
  gum_arm64_writer_put_sub_reg_reg_imm (aw, ARM64_REG_SP,
      ARM64_REG_SP, 16);
}

static void
gum_emit_epilog (GumArm64Writer * aw)
{
  /* alignment padding + PC */
  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_SP,
      ARM64_REG_SP, 16);

  /* SP + X0 */
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X1, ARM64_REG_X0);

  /* the rest */
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X1, ARM64_REG_X2);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X3, ARM64_REG_X4);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X5, ARM64_REG_X6);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X7, ARM64_REG_X8);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X9, ARM64_REG_X10);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X11, ARM64_REG_X12);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X13, ARM64_REG_X14);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X15, ARM64_REG_X16);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X17, ARM64_REG_X18);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X19, ARM64_REG_X20);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X21, ARM64_REG_X22);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X23, ARM64_REG_X24);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X25, ARM64_REG_X26);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X27, ARM64_REG_X28);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_FP, ARM64_REG_LR);

  /* pop {q0-q7}; load vector registers (for now only the clobberable ones) */
  gum_arm64_writer_put_instruction (aw, 0xacc107e0);
  gum_arm64_writer_put_instruction (aw, 0xacc10fe2);
  gum_arm64_writer_put_instruction (aw, 0xacc117e4);
  gum_arm64_writer_put_instruction (aw, 0xacc11fe6);

  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X16, ARM64_REG_X17);
  gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);
}
