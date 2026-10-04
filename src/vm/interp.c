/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2024-2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-tang.
 *
 * Ghoti.io Lang-tang is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-tang is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The interpreter: a switch-dispatched loop over a function's instructions.
 *
 * All the state it needs to continue is in the top frame of the context's
 * guest stack: the function, the program counter, the stack height, the
 * locals and the operand stack. So a pause is "return from `run`" with
 * nothing else to save, and a resume, on any thread, loads the top frame and
 * carries on (AD-8, AD-20). A call to a Tang function pushes a frame and
 * continues the loop; the interpreter never recurses in C for it.
 *
 * `S` points at the top frame's slots, and the stack moves when it grows. It
 * is reloaded after every GC point: a push, a pop, a poll, an allocation, a
 * call. Nothing that holds `S` survives one, so an operation that needs an
 * operand after it allocates reads the operand before and reloads `S` after.
 * Operands stay on the operand stack until the operation is done, so the
 * collector, which reads the frame precisely, keeps them alive.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "vm_internal.h"
#ifdef GLTANG_WITH_JIT
#include "../jit/jit.h"
#endif

#define H GLTANG_FRAME_HEADER

GRCORE_Step gltang_vm_run(GLTANG_Execution * exec, GRCORE_Context * context) {
  GRCORE_Stack * stack = grcore_context_stack(context);
  const GLTANG_Program * program;
  GRCORE_FrameRef frame;
  uint64_t * S;
  const GLTANG_Function * fn;
  const uint32_t * code;
  uint32_t fidx;
  uint64_t fword;
  size_t pc;
  size_t sp;

  if (grcore_stack_frame_count(stack) == 0) {
    if (exec->state != GLTANG_EXECUTION_NEW) {
      return exec->state == GLTANG_EXECUTION_UNWOUND ? GRCORE_STEP_UNWOUND : GRCORE_STEP_FINISHED;
    }
    const GLTANG_Function * top = &exec->program->functions[0];
    GRCORE_Result pushed = grcore_stack_push(stack, exec->engine, top->frame_slots, &frame);
    if (pushed != GRCORE_OK) {
      // The program cannot even start: no depth, or no memory for its frame.
      // It finishes with the error as its result.
      exec->current_function = 0;
      exec->current_offset = 0;
      exec->act->result = pushed == GRCORE_ERR_LIMIT && grcore_context_depth(context, GRCORE_DEPTH_GUEST) >= grcore_context_guest_depth(context)
        ? gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT)
        : exec->roots[GLTANG_ROOT_OOM];
      if (exec->act->result == GLTANG_V_UNWIND) {
        exec->act->result = GLTANG_V_NULL;
      }
      exec->state = GLTANG_EXECUTION_FINISHED;
      return GRCORE_STEP_FINISHED;
    }
    S = grcore_stack_slots(stack, frame);
    S[GLTANG_F_FUNCTION] = GLTANG_FN_WORD(0, 0);
    S[GLTANG_F_PC] = 0;
    S[GLTANG_F_SP] = H + top->local_count;
    S[GLTANG_F_FLAGS] = exec->act->depth;
  }

  exec->state = GLTANG_EXECUTION_RUNNING;
  frame = grcore_stack_top(stack);

#define RELOAD() (S = grcore_stack_slots(stack, frame))
#define LOAD_FRAME() \
  do { \
    RELOAD(); \
    fword = S[GLTANG_F_FUNCTION]; \
    fidx = GLTANG_FN_INDEX(fword); \
    program = exec->programs[GLTANG_FN_PROGRAM(fword)].program; \
    fn = &program->functions[fidx]; \
    code = fn->code; \
    pc = (size_t)S[GLTANG_F_PC]; \
    sp = (size_t)S[GLTANG_F_SP]; \
  } while (0)
#define SYNC() (exec->current_function = fidx, exec->current_offset = (uint32_t)(pc - 1u))
#define SAVE() (S[GLTANG_F_PC] = pc, S[GLTANG_F_SP] = sp)
#define PUSH(v) (S[sp++] = (v))
#define DROP(n) \
  do { \
    for (size_t drop_i = 0; drop_i < (n); ++drop_i) { \
      S[--sp] = 0; \
    } \
  } while (0)

  LOAD_FRAME();

resume_loop:
  for (;;) {
    uint32_t word = code[pc++];
    GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(word);
    uint32_t a = GLTANG_INSTRUCTION_A(word);
    exec->pending_fuel += gltang_opcode_cost_table[op];
    switch (op) {
      case GLTANG_OP_HALT: {
        gltang_vm_flush_fuel(exec);
        if (exec->act == &exec->main_act) {
          grcore_stack_pop(stack);
          exec->state = GLTANG_EXECUTION_FINISHED;
          return GRCORE_STEP_FINISHED;
        }
        // The end of a template call: its frame comes off, its budget scope is
        // closed, and the call's value is what it printed, each segment still
        // tagged with its encoding. The fuel it spent was charged to the scope
        // just now, before the scope closed.
        GLTANG_Activation * done = exec->act;
        SYNC();
        grcore_stack_pop(stack);
        (void)grcore_budget_scope_close(stack, (GRCORE_BudgetScope){done->scope_id});
        // A final value that is an error is lost across the template boundary.
        gltang_vm_error_swallowed(exec, GLTANG_ERROR_HOW_TEMPLATE_RESULT, done->result, done);
        gltang_vm_activation_leave(exec, done);
        GLTANG_Value text = gltang_vm_output_string(exec, &done->out, done->out.length);
        gltang_vm_activation_free(exec, done);
        frame = grcore_stack_top(stack);
        LOAD_FRAME();
        if (text == GLTANG_V_UNWIND) {
          goto unwound;
        }
        PUSH(text);
        break;
      }

      case GLTANG_OP_POLL: {
        gltang_vm_flush_fuel(exec);
        SAVE();
        SYNC();
#ifdef GLTANG_WITH_JIT
        // Hotness: one counter per function, per execution. Crossing the
        // threshold posts a request of the engine's own kind, so that this very
        // poll runs the tier-up handler (ACT, and only if the poll continues).
        if (exec->jit_threshold) {
          gltang_jit_note_poll(exec, fword, pc == 1u);
        }
#endif
        GRCORE_Verdict verdict = grcore_stack_poll(context, fword, pc - 1u);
        RELOAD();
        if (verdict == GRCORE_VERDICT_PAUSE) {
          exec->state = GLTANG_EXECUTION_PAUSED;
          return GRCORE_STEP_PAUSED;
        }
        if (verdict == GRCORE_VERDICT_UNWIND) {
          goto unwound;
        }
#ifdef GLTANG_WITH_JIT
        // Compiled code is entered right after a function's entry poll, with
        // the guest frame the interpreter pushed already in place: the
        // compiled function starts "after the entry poll, pc 1". Everything
        // else about the call is the interpreter's.
        if (pc == 1u && exec->jit_threshold) {
          GLTANG_Value returned = 0;
          switch (gltang_jit_enter(exec, context, fword, &returned)) {
            case GLTANG_JIT_NOT_ENTERED:
              break;
            case GLTANG_JIT_RETURNED:
              grcore_stack_pop(stack);
              frame = grcore_stack_top(stack);
              LOAD_FRAME();
              PUSH(returned);
              break;
            case GLTANG_JIT_DEOPTED:
              // The deoptimizer wrote pc, sp and the slots into the guest
              // frame, and the fuel the compiled code had not yet charged
              // into the execution; the interpreter resumes there.
              LOAD_FRAME();
              break;
            case GLTANG_JIT_PAUSED:
              exec->state = GLTANG_EXECUTION_PAUSED;
              return GRCORE_STEP_PAUSED;
            case GLTANG_JIT_UNWOUND:
              goto unwound;
          }
        }
#endif
        break;
      }

      case GLTANG_OP_LINE: {
        // A statement boundary: costs no fuel, and polls only when the host
        // asked for statement polls on this execution. With the option off
        // this is one load and one branch, and nothing else changes.
        if (!exec->statement_polls) {
          break;
        }
        gltang_vm_flush_fuel(exec);
        SAVE();
        SYNC();
        GRCORE_Verdict verdict = grcore_stack_poll(context, fword, pc - 1u);
        RELOAD();
        if (verdict == GRCORE_VERDICT_PAUSE) {
          exec->state = GLTANG_EXECUTION_PAUSED;
          return GRCORE_STEP_PAUSED;
        }
        if (verdict == GRCORE_VERDICT_UNWIND) {
          goto unwound;
        }
        break;
      }

      case GLTANG_OP_POP:
        S[--sp] = 0;
        break;

      case GLTANG_OP_DUP:
        S[sp] = S[sp - 1u];
        ++sp;
        break;

      case GLTANG_OP_NULL:
        PUSH(GLTANG_V_NULL);
        break;
      case GLTANG_OP_TRUE:
        PUSH(GLTANG_V_TRUE);
        break;
      case GLTANG_OP_FALSE:
        PUSH(GLTANG_V_FALSE);
        break;

      case GLTANG_OP_CONST: {
        GLTANG_Value v = exec->constants[a];
        if (!v) {
          SYNC();
          v = gltang_vm_constant(exec, a);
          if (v == GLTANG_V_UNWIND) {
            goto unwound;
          }
          RELOAD();
        }
        PUSH(v);
        break;
      }

      case GLTANG_OP_LOAD_LOCAL:
        PUSH(S[H + a]);
        break;
      case GLTANG_OP_STORE_LOCAL:
        S[H + a] = S[sp - 1u];
        break;
      case GLTANG_OP_LOAD_GLOBAL:
        PUSH(exec->globals[a]);
        break;
      case GLTANG_OP_STORE_GLOBAL:
        exec->globals[a] = S[sp - 1u];
        break;

      case GLTANG_OP_FUNC:
        PUSH(gltang_v_from_function(a));
        break;

      case GLTANG_OP_SET_RESULT:
        gltang_vm_set_result(exec, S[sp - 1u], (a & 1u) != 0);
        S[--sp] = 0;
        break;
      case GLTANG_OP_CLEAR_RESULT:
        gltang_vm_set_result(exec, GLTANG_V_NULL, false);
        break;

      case GLTANG_OP_DISCARD: {
        // The value of an expression statement that nothing holds: an error
        // here is swallowed, and the host is told.
        GLTANG_Value v = S[sp - 1u];
        S[--sp] = 0;
        if (gltang_vm_is_error(v)) {
          SYNC();
          gltang_vm_error_swallowed(exec, GLTANG_ERROR_HOW_DISCARDED, v, exec->act);
        }
        break;
      }

      case GLTANG_OP_USE: {
        SYNC();
        GLTANG_Value v = gltang_vm_resolve(exec, program->constants[a].block);
        if (v == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        PUSH(v);
        break;
      }

      case GLTANG_OP_NEG: {
        GLTANG_Value v = S[sp - 1u];
        SYNC();
        GLTANG_Value r = gltang_vm_op_neg(exec, v);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        S[sp - 1u] = r;
        break;
      }
      case GLTANG_OP_NOT:
        S[sp - 1u] = gltang_v_from_bool(!gltang_vm_truthy(S[sp - 1u]));
        break;

      case GLTANG_OP_ADD:
      case GLTANG_OP_SUB:
      case GLTANG_OP_MUL:
      case GLTANG_OP_DIV:
      case GLTANG_OP_MOD:
      case GLTANG_OP_LT:
      case GLTANG_OP_LE:
      case GLTANG_OP_GT:
      case GLTANG_OP_GE:
      case GLTANG_OP_EQ:
      case GLTANG_OP_NE: {
        GLTANG_Value x = S[sp - 2u];
        GLTANG_Value y = S[sp - 1u];
        if (gltang_v_is_small_int(x) && gltang_v_is_small_int(y)) {
          // Inline integers: the common case, with no call.
          int64_t m = gltang_v_small_int(x);
          int64_t n = gltang_v_small_int(y);
          int64_t r = 0;
          bool done = true;
          switch (op) {
            case GLTANG_OP_ADD: r = m + n; break;
            case GLTANG_OP_SUB: r = m - n; break;
            case GLTANG_OP_LT: S[sp - 2u] = gltang_v_from_bool(m < n); goto binary_fast_done;
            case GLTANG_OP_LE: S[sp - 2u] = gltang_v_from_bool(m <= n); goto binary_fast_done;
            case GLTANG_OP_GT: S[sp - 2u] = gltang_v_from_bool(m > n); goto binary_fast_done;
            case GLTANG_OP_GE: S[sp - 2u] = gltang_v_from_bool(m >= n); goto binary_fast_done;
            case GLTANG_OP_EQ: S[sp - 2u] = gltang_v_from_bool(m == n); goto binary_fast_done;
            case GLTANG_OP_NE: S[sp - 2u] = gltang_v_from_bool(m != n); goto binary_fast_done;
            default: done = false; break;
          }
          if (done && r >= GLTANG_SMALL_INT_MIN && r <= GLTANG_SMALL_INT_MAX) {
            S[sp - 2u] = gltang_v_from_small_int(r);
            goto binary_fast_done;
          }
        }
        SYNC();
        {
          GLTANG_Value r = gltang_vm_op_binary(exec, op, x, y);
          if (r == GLTANG_V_UNWIND) {
            goto unwound;
          }
          RELOAD();
          S[sp - 2u] = r;
        }
      binary_fast_done:
        S[--sp] = 0;
        break;
      }

      case GLTANG_OP_JMP:
        pc = a;
        break;
      case GLTANG_OP_JMP_FALSE: {
        GLTANG_Value v = S[--sp];
        S[sp] = 0;
        if (!gltang_vm_truthy(v)) {
          pc = a;
        }
        break;
      }
      case GLTANG_OP_JMP_TRUE: {
        GLTANG_Value v = S[--sp];
        S[sp] = 0;
        if (gltang_vm_truthy(v)) {
          pc = a;
        }
        break;
      }
      case GLTANG_OP_AND:
        if (!gltang_vm_truthy(S[sp - 1u])) {
          pc = a;
        }
        else {
          S[--sp] = 0;
        }
        break;
      case GLTANG_OP_OR:
        if (gltang_vm_truthy(S[sp - 1u])) {
          pc = a;
        }
        else {
          S[--sp] = 0;
        }
        break;

      case GLTANG_OP_CAST: {
        GLTANG_Value v = S[sp - 1u];
        SYNC();
        GLTANG_Value r = gltang_vm_op_cast(exec, v, (GLTANG_Cast_Type)a);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        S[sp - 1u] = r;
        break;
      }

      case GLTANG_OP_INDEX: {
        GLTANG_Value c = S[sp - 2u];
        GLTANG_Value i = S[sp - 1u];
        SYNC();
        GLTANG_Value r = gltang_vm_op_index(exec, c, i);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        S[sp - 2u] = r;
        S[--sp] = 0;
        break;
      }

      case GLTANG_OP_ATTR: {
        SYNC();
        GLTANG_Value name = gltang_vm_constant(exec, a);
        if (name == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        GLTANG_Value r = gltang_vm_op_attr(exec, S[sp - 1u], name);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        S[sp - 1u] = r;
        break;
      }

      case GLTANG_OP_SLICE: {
        size_t parts = (a & 1u) + ((a >> 1) & 1u) + ((a >> 2) & 1u);
        SYNC();
        GLTANG_Value r = gltang_vm_op_slice(exec, S[sp - 1u - parts], a, &S[sp - parts]);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        DROP(parts);
        S[sp - 1u] = r;
        break;
      }

      case GLTANG_OP_SET_INDEX: {
        GLTANG_Value c = S[sp - 3u];
        GLTANG_Value i = S[sp - 2u];
        GLTANG_Value v = S[sp - 1u];
        SYNC();
        GLTANG_Value r = gltang_vm_op_set_index(exec, c, i, v, (a & 1u) != 0);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        DROP(2);
        S[sp - 1u] = r;
        break;
      }

      case GLTANG_OP_SET_ATTR: {
        SYNC();
        GLTANG_Value name = gltang_vm_constant(exec, a >> 1);
        if (name == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        GLTANG_Value r = gltang_vm_op_set_attr(exec, S[sp - 2u], name, S[sp - 1u], (a & 1u) != 0);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        DROP(1);
        S[sp - 1u] = r;
        break;
      }

      case GLTANG_OP_ADOPT: {
        GLTANG_Value v = S[sp - 1u];
        if (gltang_vm_is_container(v)) {
          SYNC();
          GLTANG_Value r = gltang_vm_op_adopt(exec, v);
          if (r == GLTANG_V_UNWIND) {
            goto unwound;
          }
          RELOAD();
          S[sp - 1u] = r;
        }
        break;
      }

      case GLTANG_OP_ARRAY: {
        SYNC();
        GLTANG_Value array = gltang_vm_array_new(exec, a);
        if (array == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        if (gltang_v_is_kind(array, GLTANG_OBJ_ARRAY)) {
          gltang_vm_array_fill(exec, array, &S[sp - a], a);
        }
        DROP(a);
        PUSH(array);
        break;
      }

      case GLTANG_OP_MAP: {
        SYNC();
        GLTANG_Value map = gltang_vm_map_new(exec, a);
        if (map == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        if (gltang_v_is_kind(map, GLTANG_OBJ_MAP)) {
          gltang_vm_map_fill(exec, map, &S[sp - 2u * a], a);
        }
        DROP(2u * a);
        PUSH(map);
        break;
      }

      case GLTANG_OP_CALL: {
        size_t argc = a;
        GLTANG_Value callee = S[sp - 1u - argc];
        GLTANG_Value r = GLTANG_V_NULL;
        SYNC();
        if (!gltang_v_is_function(callee)) {
          if (gltang_v_is_kind(callee, GLTANG_OBJ_NATIVE)) {
            // A host function, or one of the engine's own: synchronous, with
            // the arguments where they are on the operand stack.
            r = gltang_vm_call_native(exec, callee, argc, &S[sp - argc]);
          }
          else if (gltang_v_is_kind(callee, GLTANG_OBJ_TEMPLATE)) {
            const GLTANG_LibraryMember * member = gltang_vm_template_member(callee);
            if (argc != 0) {
              r = gltang_vm_make_error(exec, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
              goto call_result;
            }
            if (grcore_context_depth(context, GRCORE_DEPTH_GUEST) >= grcore_context_guest_depth(context)) {
              r = gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
              goto call_result;
            }
            uint32_t callee_program;
            if (!gltang_vm_program_index(exec, member->program, &callee_program)) {
              r = exec->roots[GLTANG_ROOT_OOM];
              goto call_result;
            }
            GLTANG_Activation * act = gltang_vm_activation_new(exec, callee_program, member->name);
            if (!act) {
              r = exec->roots[GLTANG_ROOT_OOM];
              goto call_result;
            }
            const GLTANG_Function * top = &member->program->functions[0];
            SAVE();
            grcore_stack_set_identity(stack, frame, (GRCORE_PollIdentity){fword, pc - 1u});
            // Before the callee's frame is pushed: what has been spent so far
            // belongs to the caller, so the fuel is flushed, and then the
            // callee's scope is opened at the top of the stack as it stands.
            gltang_vm_flush_fuel(exec);
            GRCORE_BudgetScope scope;
            GRCORE_ScopePolicy policy = member->policy == GLTANG_SCOPE_PAUSE ? GRCORE_SCOPE_POLICY_PAUSE : GRCORE_SCOPE_POLICY_UNWIND;
            GRCORE_Result opened = grcore_budget_scope_open(stack, member->scope_fuel, policy, &scope);
            GRCORE_FrameRef callee_frame;
            GRCORE_Result pushed = GRCORE_ERR_INTERNAL;
            if (opened == GRCORE_OK) {
              pushed = grcore_stack_push(stack, exec->engine, top->frame_slots, &callee_frame);
              if (pushed != GRCORE_OK) {
                (void)grcore_budget_scope_close(stack, scope);
              }
            }
            if (opened != GRCORE_OK || pushed != GRCORE_OK) {
              gltang_vm_activation_free(exec, act);
              // The memory budget's verdict comes first, as for any call.
              GRCORE_Result refused = opened != GRCORE_OK ? opened : pushed;
              if (refused == GRCORE_ERR_LIMIT && GLTANG_NATIVE_POLL(exec, CALL_REFUSED, 0) != GLTANG_ST_OK) {
                goto unwound;
              }
              r = exec->roots[GLTANG_ROOT_OOM];
              goto call_result;
            }
            act->scope_id = scope.id;
            act->policy = member->policy;
            gltang_vm_activation_enter(exec, act, fidx, (uint32_t)(pc - 1u));
            {
              uint64_t * caller = grcore_stack_slots(stack, frame);
              uint64_t * callee_slots = grcore_stack_slots(stack, callee_frame);
              caller[sp - 1u] = 0;
              --sp;
              caller[GLTANG_F_SP] = sp;
              callee_slots[GLTANG_F_FUNCTION] = GLTANG_FN_WORD(callee_program, 0);
              callee_slots[GLTANG_F_PC] = 0;
              callee_slots[GLTANG_F_SP] = H + top->local_count;
              callee_slots[GLTANG_F_FLAGS] = act->depth;
              frame = callee_frame;
              S = callee_slots;
              program = member->program;
              fidx = 0;
              fword = callee_slots[GLTANG_F_FUNCTION];
              fn = top;
              code = top->code;
              pc = 0;
              sp = H + top->local_count;
            }
            break;
          }
          else {
            r = gltang_vm_make_error(exec, GLTANG_ERROR_INVALID_FUNCTION_CALL);
          }
          goto call_result;
        }
        if (gltang_v_function_index(callee) >= program->function_count) {
          r = gltang_vm_make_error(exec, GLTANG_ERROR_INVALID_FUNCTION_CALL);
          goto call_result;
        }
        uint64_t target = gltang_v_function_index(callee);
        const GLTANG_Function * cf = &program->functions[target];
        if (cf->parameter_count != argc) {
          r = gltang_vm_make_error(exec, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
          goto call_result;
        }
        // The call depth is the context's guest-depth budget, counted in
        // frames as ctang counts calls; past it the call is not made and its
        // value is the error (reference 10.3).
        if (grcore_context_depth(context, GRCORE_DEPTH_GUEST) >= grcore_context_guest_depth(context)) {
          r = gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
          goto call_result;
        }
        SAVE();
        grcore_stack_set_identity(stack, frame, (GRCORE_PollIdentity){fword, pc - 1u});
        GRCORE_FrameRef callee_frame;
        GRCORE_Result pushed = grcore_stack_push(stack, exec->engine, cf->frame_slots, &callee_frame);
        if (pushed != GRCORE_OK) {
          // The stack could not grow: the memory budget refused it, or the
          // allocator did. The budget's verdict comes first.
          if (pushed == GRCORE_ERR_LIMIT && GLTANG_NATIVE_POLL(exec, CALL_REFUSED, 0) != GLTANG_ST_OK) {
            goto unwound;
          }
          r = exec->roots[GLTANG_ROOT_OOM];
          goto call_result;
        }
        {
          // The push may have moved the stack: both frames are read afresh.
          uint64_t * caller = grcore_stack_slots(stack, frame);
          uint64_t * callee_slots = grcore_stack_slots(stack, callee_frame);
          for (size_t i = 0; i < argc; ++i) {
            callee_slots[H + i] = caller[sp - argc + i];
            caller[sp - argc + i] = 0;
          }
          caller[sp - 1u - argc] = 0;
          sp -= argc + 1u;
          caller[GLTANG_F_SP] = sp;
          callee_slots[GLTANG_F_FUNCTION] = GLTANG_FN_WORD(GLTANG_FN_PROGRAM(fword), target);
          callee_slots[GLTANG_F_PC] = 0;
          callee_slots[GLTANG_F_SP] = H + cf->local_count;
          callee_slots[GLTANG_F_FLAGS] = exec->act->depth;
          frame = callee_frame;
          S = callee_slots;
          fidx = (uint32_t)target;
          fword = callee_slots[GLTANG_F_FUNCTION];
          fn = cf;
          code = cf->code;
          pc = 0;
          sp = H + cf->local_count;
        }
        break;

      call_result:
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        DROP(argc + 1u);
        PUSH(r);
        break;
      }

      case GLTANG_OP_RET: {
        GLTANG_Value v = S[sp - 1u];
        S[sp - 1u] = 0;
        grcore_stack_pop(stack);
        frame = grcore_stack_top(stack);
        LOAD_FRAME();
        PUSH(v);
        break;
      }

      case GLTANG_OP_PRINT: {
        SYNC();
        GLTANG_Value r = gltang_vm_op_print(exec, S[sp - 1u]);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        S[sp - 1u] = r;
        break;
      }

      case GLTANG_OP_PRINT_CONST: {
        SYNC();
        GLTANG_Value r = gltang_vm_print_constant(exec, program->constants[a].block);
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        break;
      }

      case GLTANG_OP_ITER_INIT: {
        GLTANG_Value v = S[sp - 1u];
        if (gltang_v_is_kind(v, GLTANG_OBJ_ARRAY)) {
          S[H + a] = v;
          S[H + a + 1u] = gltang_v_from_small_int(0);
          S[sp - 1u] = GLTANG_V_TRUE;
        }
        else {
          // Only arrays are iterable; a string says so (5.6). The error is
          // the loop's value (13.12: an expression that cannot be iterated is
          // an error and stays one).
          SYNC();
          GLTANG_Value e = gltang_vm_make_error(exec, gltang_v_is_kind(v, GLTANG_OBJ_STRING) ? GLTANG_ERROR_NOT_IMPLEMENTED : GLTANG_ERROR_NOT_SUPPORTED);
          if (e == GLTANG_V_UNWIND) {
            goto unwound;
          }
          RELOAD();
          gltang_vm_set_result(exec, e, true);
          S[sp - 1u] = GLTANG_V_FALSE;
        }
        break;
      }

      case GLTANG_OP_ITER_NEXT: {
        size_t target = code[pc++];
        const GLTANG_ArrayObject * array = gltang_vm_array(S[H + a]);
        int64_t at = gltang_v_small_int(S[H + a + 1u]);
        if ((uint64_t)at < array->length) {
          PUSH(array->store.typed->slots[at]);
          S[H + a + 1u] = gltang_v_from_small_int(at + 1);
        }
        else {
          pc = target;
        }
        break;
      }

      case GLTANG_OP_COUNT:
        break;
    }
  }

unwound:
  gltang_vm_flush_fuel(exec);
  if (exec->act != &exec->main_act && exec->act->policy != GLTANG_SCOPE_PAUSE
      && grcore_budget_scope_exhausted(stack, (GRCORE_BudgetScope){exec->act->scope_id})) {
    // A template call's own budget ran out, and nothing else voted to stop
    // (not the request's ceiling, not a terminate, not memory): the unwind is
    // the call's and not the run's (AD-21). It is reached from a poll
    // instruction and from the runtime poll inside a native operation alike.
    GLTANG_Activation * stopped = exec->act;
    gltang_vm_error_scope_limit(exec, stopped);
    size_t popped = 0;
    if (grcore_budget_scope_unwind(stack, (GRCORE_BudgetScope){stopped->scope_id}, &popped) == GRCORE_OK) {
      // The callee's half-built objects are released: whatever it had pinned
      // while it was stopped is let go, and the frames are gone.
      while (exec->temp_count > stopped->temp_base) {
        gltang_vm_temp_pop(exec);
      }
      exec->unwinding = false;
      gltang_vm_activation_leave(exec, stopped);
      // The call's value, made where the call was: EMPTY is the limit error,
      // SEGMENTS is what the callee had finished printing, up to a whole print.
      exec->current_function = stopped->call_function;
      exec->current_offset = stopped->call_offset;
      GLTANG_Value value = stopped->policy == GLTANG_SCOPE_SEGMENTS
        ? gltang_vm_output_string(exec, &stopped->out, stopped->out.committed)
        : gltang_vm_make_limit_error(exec);
      gltang_vm_activation_free(exec, stopped);
      if (value != GLTANG_V_UNWIND) {
        frame = grcore_stack_top(stack);
        LOAD_FRAME();
        PUSH(value);
        goto resume_loop;
      }
      // Making the value ran into a limit that is the run's: fall through.
    }
    else {
      // Cannot happen: the scope is open. Treat it as the run's own end.
      exec->unwinding = true;
    }
  }
  // The run's own end. Every template call still open is let go of, innermost
  // first, and the stack is emptied (the scopes close with it).
  while (exec->act != &exec->main_act) {
    GLTANG_Activation * open = exec->act;
    gltang_vm_activation_leave(exec, open);
    gltang_vm_activation_free(exec, open);
  }
  while (exec->temp_count) {
    gltang_vm_temp_pop(exec);
  }
  grcore_unwind_all(stack, NULL);
  // The program did not finish, so what an earlier statement left as its
  // result is not an answer.
  exec->act->result = GLTANG_V_NULL;
  exec->act->result_lost = false;
  if (exec->halt_registered) {
    // The halt request has been carried out; leave nothing pending.
    (void)grcore_context_clear_request(context, exec->halt_kind);
  }
#ifdef GLTANG_WITH_JIT
  gltang_jit_unwound(exec);
#endif
  exec->state = GLTANG_EXECUTION_UNWOUND;
  return GRCORE_STEP_UNWOUND;

#undef RELOAD
#undef LOAD_FRAME
#undef SYNC
#undef SAVE
#undef PUSH
#undef DROP
}

GRCORE_Step gltang_execution_entry(GRCORE_Context * context, void * state) {
  GLTANG_Execution * exec = state;
  if (!exec || exec->destroyed || !context || exec->context != context) {
    // Nothing to run: report the end rather than a pause or an unwind, which
    // would be a lie about a poll that never happened.
    return GRCORE_STEP_FINISHED;
  }
  return gltang_vm_run(exec, context);
}
