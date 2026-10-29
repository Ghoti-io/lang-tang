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

/**
 * `HALT`'s body, for the frame of function `fidx` whose `HALT` is at `offset`: the one
 * implementation, run by the interpreter's `HALT` and, after compiled code returned
 * with the halted word (jit.h), by the branch that takes a compiled return. Compiled
 * code cannot run it: runtime-core closes a budget scope only when the frames and
 * activation records above the scope's base are gone, and the compiled frame and its
 * JIT record are there until compiled code has returned. The fuel `HALT` costs is
 * already in `pending_fuel` (added by the dispatch, or by compiled code before it
 * returned) and is flushed here, once.
 *
 * The main activation finishes: its frame is popped and true is returned. A template
 * call ends: its frame comes off, its budget scope is closed, a final value that is
 * an error is recorded as lost across the template boundary, the activation is left,
 * the call's value (what it printed, each segment still tagged with its encoding) is
 * made into `*text` and the activation freed; false is returned and the caller pushes
 * `*text` into the caller's frame, or unwinds if it is ::GLTANG_V_UNWIND (making the
 * string allocates, and can run into a limit or out of memory).
 */
static bool halt_frame(GLTANG_Execution * exec, GRCORE_Stack * stack, uint32_t fidx, uint32_t offset, GLTANG_Value * text) {
  gltang_vm_flush_fuel(exec);
  if (exec->act == &exec->main_act) {
    grcore_stack_pop(stack);
    exec->state = GLTANG_EXECUTION_FINISHED;
    return true;
  }
  // The fuel the call spent was charged to its scope just now, before the scope closes.
  GLTANG_Activation * done = exec->act;
  exec->current_function = fidx;
  exec->current_offset = offset;
  grcore_stack_pop(stack);
  (void)grcore_budget_scope_close(stack, (GRCORE_BudgetScope){done->scope_id});
  // A final value that is an error is lost across the template boundary.
  gltang_vm_error_swallowed(exec, GLTANG_ERROR_HOW_TEMPLATE_RESULT, done->result, done);
  gltang_vm_activation_leave(exec, done);
  *text = gltang_vm_output_string(exec, &done->out, done->out.length);
  gltang_vm_activation_free(exec, done);
  return false;
}

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
  uint32_t halt_at;         // the HALT whose body runs (halt_frame), interpreted or after a compiled return
  GLTANG_Value halt_text;   // a template call's value, made by that body
  GLTANG_Value binary_r;    // a binary operator's result, from its call (one of three paths)
  GLTANG_Value truth_r;     // the truth of a custom value, from its type (CUSTOM_TRUTH)

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
    (void)grcore_stack_set_guest_function(stack, frame, 0);
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
// The frame as it stands while an operation that can be compiled to a call of the
// engine's own function runs (a library load, the call of a native): at the
// operation itself, with its operands still on the stack. That is the state compiled
// code's call site carries for the same operation (which an exit before the call needs
// too), so a poll inside it sees the same frame in both tiers.
#define SAVE_AT_OP() (S[GLTANG_F_PC] = pc - 1u, S[GLTANG_F_SP] = sp)
// The identity a poll inside such an operation reads (the frame header's) is the operation's own,
// as compiled code's call of it reads (natives.c). The operation only says that it stands: the
// identity is written by the runtime poll, if one happens inside it (gltang_vm_native_poll, from
// the frame's function and `current_offset`, which SYNC has set to the operation), so an operation
// that does not poll stores nothing. STOOD() ends it, right after the operation's call returns; a
// poll that writes the identity ends it too, which is how an `ATTR` whose name unwound ends it.
#define STAND_AT_OP() (exec->standing = true)
#define STOOD() (exec->standing = false)
#define PUSH(v) (S[sp++] = (v))
// The truth of the custom value on top of the operand stack: its type's `truthy`, a GC point where a
// host callback runs (and may poll). The value stays on the stack during the call, as an operation's
// operands do; the result is `truth_r` (GLTANG_V_TRUE, GLTANG_V_FALSE, or an error value, which is
// falsy and the expression's own value). A test of any other kind is gltang_vm_truthy, a pure
// function, and pays only the kind test above.
#define CUSTOM_TRUTH() \
  do { \
    SYNC(); \
    SAVE_AT_OP(); \
    STAND_AT_OP(); \
    truth_r = gltang_vm_custom_truth(exec, S[sp - 1u]); \
    STOOD(); \
    if (truth_r == GLTANG_V_UNWIND) { \
      goto unwound; \
    } \
    RELOAD(); \
  } while (0)
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
      case GLTANG_OP_HALT:
        // The end of the main activation, or of a template call (halt_frame). A compiled
        // `HALT` returns and joins here, at `halted`, so the two run one copy of it.
        halt_at = (uint32_t)(pc - 1u);
#ifdef GLTANG_WITH_JIT
      halted:  // only the JIT arm jumps here (an unused label is a warning in the other)
#endif
        halt_text = GLTANG_V_NULL;
        if (halt_frame(exec, stack, fidx, halt_at, &halt_text)) {
          return GRCORE_STEP_FINISHED;
        }
        frame = grcore_stack_top(stack);
        LOAD_FRAME();
        if (halt_text == GLTANG_V_UNWIND) {
          goto unwound;
        }
        PUSH(halt_text);
        break;

      case GLTANG_OP_POLL: {
        gltang_vm_flush_fuel(exec);
        SAVE();
        SYNC();
#ifdef GLTANG_WITH_JIT
        // Hotness: one counter per function, per execution. Crossing the
        // threshold posts a request of the engine's own kind, so that this very
        // poll runs the tier-up handler (ACT, and only if the poll continues).
        const bool jit_watching = exec->jit_threshold != 0 && fword != exec->jit_settled_fword;
        if (jit_watching) {
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
        // compiled function starts "after the entry poll, pc 1" (the hidden
        // flag it is passed says so; a compiled call makes the callee's entry
        // poll itself). Everything else about the call is the interpreter's.
        if (pc == 1u && jit_watching && exec->jit_threshold) {
          GLTANG_Value returned = 0;
          switch (gltang_jit_enter(exec, context, fword, &returned)) {
            case GLTANG_JIT_NOT_ENTERED:
              break;
            case GLTANG_JIT_RETURNED:
              if (gltang_jit_halted(returned, &halt_at)) {
                // Compiled code reached a `HALT` and returned, having charged its fuel:
                // the frame is still pushed and the JIT record is left, so `HALT`'s own
                // body runs now, in the interpreter's own `HALT` (a nested run's entry
                // frame included: the interpreter's `HALT` does not ask).
                goto halted;
              }
              grcore_stack_pop(stack);
              if (exec->run_base_frames != 0 && grcore_stack_frame_count(stack) == exec->run_base_frames) {
                // The entry frame of a nested run (AD-23) returned: the native that
                // opened it takes the value.
                exec->nested_value = returned;
                return GRCORE_STEP_FINISHED;
              }
              frame = grcore_stack_top(stack);
              LOAD_FRAME();
              PUSH(returned);
              break;
            case GLTANG_JIT_DEOPTED:
              // The deopt hook rebuilt every compiled frame of the chain into
              // its guest frame (pc, sp and the slots), and the fuel compiled
              // code counted is already in the execution; the interpreter
              // resumes in the innermost guest frame, which may be a callee's.
              frame = grcore_stack_top(stack);
              LOAD_FRAME();
              break;
            case GLTANG_JIT_PAUSED:
              // The paused context holds only interpreter frames.
              exec->state = GLTANG_EXECUTION_PAUSED;
              return GRCORE_STEP_PAUSED;
            case GLTANG_JIT_UNWOUND:
              // A poll's unwind verdict, or a chain whose rebuild was refused: the
              // run's own end. Every guest frame is popped by `unwound`.
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
        SAVE_AT_OP();
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
        if (gltang_v_is_custom(S[sp - 1u])) {
          CUSTOM_TRUTH();
          // An error the type answered is the value of `!` (it is falsy, and an expression's value).
          S[sp - 1u] = truth_r == GLTANG_V_TRUE ? GLTANG_V_FALSE : (truth_r == GLTANG_V_FALSE ? GLTANG_V_TRUE : truth_r);
          break;
        }
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
          // Two inline integers the switch above does not compute (`*`, `/`, `%`, a result
          // outside the inline range): numbers, which stand nowhere (below), so the call is
          // made without the test.
          SYNC();
          binary_r = gltang_vm_op_binary(exec, op, x, y);
          goto binary_called;
        }
        SYNC();
        // A long concatenation or repetition polls inside (its pacing), and so may a host-defined
        // type's operation: the frame there stands at the operation, the state and identity
        // compiled code's call of `+` carries, and that a guard exit before any of them leaves
        // in the rebuilt frame. Two numbers are computed inline by compiled code, which stands
        // nowhere, so neither does the interpreter (gltang_vm_binary_may_pace). The stand and its
        // end are on their own path, so an operator of two numbers stores neither.
        if (gltang_vm_binary_may_pace(x, y)) {
          SAVE_AT_OP();
          STAND_AT_OP();
          binary_r = gltang_vm_op_binary(exec, op, x, y);
          STOOD();
        }
        else {
          binary_r = gltang_vm_op_binary(exec, op, x, y);
        }
      binary_called:
        if (binary_r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        S[sp - 2u] = binary_r;
      binary_fast_done:
        S[--sp] = 0;
        break;
      }

      case GLTANG_OP_JMP:
        pc = a;
        break;
      case GLTANG_OP_JMP_FALSE: {
        if (gltang_v_is_custom(S[sp - 1u])) {
          CUSTOM_TRUTH();
          S[--sp] = 0;
          if (truth_r != GLTANG_V_TRUE) {
            pc = a;
          }
          break;
        }
        GLTANG_Value v = S[--sp];
        S[sp] = 0;
        if (!gltang_vm_truthy(v)) {
          pc = a;
        }
        break;
      }
      case GLTANG_OP_JMP_TRUE: {
        if (gltang_v_is_custom(S[sp - 1u])) {
          CUSTOM_TRUTH();
          S[--sp] = 0;
          if (truth_r == GLTANG_V_TRUE) {
            pc = a;
          }
          break;
        }
        GLTANG_Value v = S[--sp];
        S[sp] = 0;
        if (gltang_vm_truthy(v)) {
          pc = a;
        }
        break;
      }
      case GLTANG_OP_AND:
        if (gltang_v_is_custom(S[sp - 1u])) {
          CUSTOM_TRUTH();
          if (truth_r != GLTANG_V_TRUE) {
            // Falsy: the left operand is the value, or the error its type answered in its place.
            if (truth_r != GLTANG_V_FALSE) {
              S[sp - 1u] = truth_r;
            }
            pc = a;
          }
          else {
            S[--sp] = 0;
          }
          break;
        }
        if (!gltang_vm_truthy(S[sp - 1u])) {
          pc = a;
        }
        else {
          S[--sp] = 0;
        }
        break;
      case GLTANG_OP_OR:
        if (gltang_v_is_custom(S[sp - 1u])) {
          CUSTOM_TRUTH();
          if (truth_r == GLTANG_V_TRUE) {
            pc = a;
          }
          else {
            S[--sp] = 0;
          }
          break;
        }
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
        if (a == GLTANG_CAST_TYPE_STRING) {
          // A cast to string renders its operand, paced by the bytes it renders: stand at the
          // cast, as compiled code's helper does. The other casts poll nowhere and stand nowhere.
          SAVE_AT_OP();
          STAND_AT_OP();
        }
        GLTANG_Value r = gltang_vm_op_cast(exec, v, (GLTANG_Cast_Type)a);
        STOOD();
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
        // A string's grapheme is a substring, paced by its bytes: stand at the index.
        SAVE_AT_OP();
        STAND_AT_OP();
        GLTANG_Value r = gltang_vm_op_index(exec, c, i);
        STOOD();
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
        SAVE_AT_OP();
        // A retag (`.html` and the others) and `.render` poll inside.
        STAND_AT_OP();
        GLTANG_Value name = gltang_vm_constant(exec, a);
        if (name == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        GLTANG_Value r = gltang_vm_op_attr(exec, S[sp - 1u], name);
        STOOD();
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
        // Both passes of a long slice poll inside.
        SAVE_AT_OP();
        STAND_AT_OP();
        GLTANG_Value r = gltang_vm_op_slice(exec, S[sp - 1u - parts], a, &S[sp - parts]);
        STOOD();
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
          // The copy paces (a deep copy of a long container): stand at the ADOPT, as compiled
          // code's helper does. An ADOPT of anything else does nothing, and stands nowhere.
          SAVE_AT_OP();
          STAND_AT_OP();
          GLTANG_Value r = gltang_vm_op_adopt(exec, v);
          STOOD();
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
          // A key the memory budget refused to make makes the literal that failure.
          map = gltang_vm_map_fill(exec, map, &S[sp - 2u * a], a);
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
            SAVE_AT_OP();
            r = gltang_vm_call_native(exec, callee, argc, &S[sp - argc], false);
            if (exec->callk_function) {
              // A resumable native asked for a guest call and to be called again
              // with its value (AD-23): its continuation is a native bound to the
              // state it gave, left where the callee was, and the call is made
              // here, by this loop, so a pause inside it resumes like any other.
              // The `CALL` itself is run again when the callee returns, finding
              // the continuation and the value.
              GLTANG_Value kf = exec->callk_function;
              // Nothing allocates between the native's return and the continuation
              // below (which holds the state itself across its allocation), so the
              // state is not moved in between.
              GLTANG_Value kstate = exec->callk_state;
              exec->callk_function = 0;
              RELOAD();
              uint64_t ktarget = gltang_v_function_index(kf);
              if (argc != 1u || !gltang_v_is_function(kf) || ktarget >= program->function_count || program->functions[ktarget].parameter_count != 0) {
                r = gltang_vm_make_error(exec, GLTANG_ERROR_INVALID_FUNCTION_CALL);
                goto call_result;
              }
              if (grcore_context_depth(context, GRCORE_DEPTH_GUEST) >= grcore_context_guest_depth(context)) {
                r = gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
                goto call_result;
              }
              GLTANG_Value k = gltang_vm_native_continuation(exec, S[sp - 2u], kstate);
              if (k == GLTANG_V_UNWIND || k == exec->roots[GLTANG_ROOT_OOM]) {
                r = k;
                goto call_result;
              }
              RELOAD();
              S[sp - 2u] = k;
              S[sp - 1u] = kf;
              const GLTANG_Function * kfn = &program->functions[ktarget];
              SAVE();
              grcore_stack_set_identity(stack, frame, (GRCORE_PollIdentity){fword, pc - 1u, exec->engine});
              GRCORE_FrameRef kframe;
              GRCORE_Result kpushed = grcore_stack_push(stack, exec->engine, kfn->frame_slots, &kframe);
              if (kpushed != GRCORE_OK) {
                if (kpushed == GRCORE_ERR_LIMIT && GLTANG_NATIVE_POLL(exec, CALL_REFUSED, 0) != GLTANG_ST_OK) {
                  goto unwound;
                }
                r = exec->roots[GLTANG_ROOT_OOM];
                goto call_result;
              }
              (void)grcore_stack_set_guest_function(stack, kframe, (uint32_t)ktarget);
              {
                uint64_t * caller = grcore_stack_slots(stack, frame);
                uint64_t * callee_slots = grcore_stack_slots(stack, kframe);
                caller[sp - 1u] = 0;
                --sp;
                caller[GLTANG_F_SP] = sp;
                caller[GLTANG_F_PC] = pc - 1u;
                callee_slots[GLTANG_F_FUNCTION] = GLTANG_FN_WORD(GLTANG_FN_PROGRAM(fword), ktarget);
                callee_slots[GLTANG_F_PC] = 0;
                callee_slots[GLTANG_F_SP] = H + kfn->local_count;
                callee_slots[GLTANG_F_FLAGS] = exec->act->depth;
                frame = kframe;
                S = callee_slots;
                fidx = (uint32_t)ktarget;
                fword = callee_slots[GLTANG_F_FUNCTION];
                fn = kfn;
                code = kfn->code;
                pc = 0;
                sp = H + kfn->local_count;
              }
              break;
            }
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
            // The frame stands at the call before anything the call allocates can poll (the memory
            // budget refusing the activation's storage): compiled code's exit at a template call
            // leaves the rebuilt frame's identity at the call, and the interpreter's poll there
            // must read the same.
            grcore_stack_set_identity(stack, frame, (GRCORE_PollIdentity){fword, pc - 1u, exec->engine});
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
            (void)grcore_stack_set_guest_function(stack, callee_frame, 0);
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
          else if (gltang_v_is_custom(callee)) {
            // A callable instance: its type's `call`, the operands where they are on the stack,
            // as a native's (a GC point; the arguments are copied before it can collect).
            SAVE_AT_OP();
            STAND_AT_OP();
            r = gltang_vm_custom_call(exec, &S[sp - 1u - argc], argc);
            STOOD();
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
        grcore_stack_set_identity(stack, frame, (GRCORE_PollIdentity){fword, pc - 1u, exec->engine});
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
        // Poll identity is an opaque program-qualified token. Pairing a later
        // compiled frame uses the actual function index of this guest frame.
        (void)grcore_stack_set_guest_function(stack, callee_frame, (uint32_t)target);
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
        if (exec->run_base_frames != 0 && grcore_stack_frame_count(stack) == exec->run_base_frames) {
          // The entry frame of a nested run (AD-23) returned.
          exec->nested_value = v;
          return GRCORE_STEP_FINISHED;
        }
        frame = grcore_stack_top(stack);
        LOAD_FRAME();
        PUSH(v);
        break;
      }

      case GLTANG_OP_PRINT: {
        // A long print polls inside (its pacing): the frame there stands at the print,
        // the state and identity compiled code's call of the same function carries.
        SYNC();
        SAVE_AT_OP();
        STAND_AT_OP();
        GLTANG_Value r = gltang_vm_op_print(exec, S[sp - 1u]);
        STOOD();
        if (r == GLTANG_V_UNWIND) {
          goto unwound;
        }
        RELOAD();
        S[sp - 1u] = r;
        break;
      }

      case GLTANG_OP_PRINT_CONST: {
        SYNC();
        SAVE_AT_OP();
        STAND_AT_OP();
        GLTANG_Value r = gltang_vm_print_constant(exec, program->constants[a].block);
        STOOD();
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
        else if (gltang_v_is_custom(v)) {
          // A host collection: its type's `iterator_begin` answers the iterator (a custom value
          // with `iterator_next`, or an array), or the loop's value is the error (a GC point).
          SYNC();
          SAVE_AT_OP();
          STAND_AT_OP();
          GLTANG_Value it = gltang_vm_custom_iterator_begin(exec, v);
          STOOD();
          if (it == GLTANG_V_UNWIND) {
            goto unwound;
          }
          RELOAD();
          if (gltang_v_is_kind(it, GLTANG_OBJ_ARRAY)) {
            S[H + a] = it;
            S[H + a + 1u] = gltang_v_from_small_int(0);
            S[sp - 1u] = GLTANG_V_TRUE;
          }
          else if (gltang_v_is_custom(it)) {
            // A custom iterator: the second slot is not an integer, which is how ITER_NEXT tells.
            S[H + a] = it;
            S[H + a + 1u] = GLTANG_V_NULL;
            S[sp - 1u] = GLTANG_V_TRUE;
          }
          else {
            gltang_vm_set_result(exec, it, true);
            S[sp - 1u] = GLTANG_V_FALSE;
          }
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
        if (!gltang_v_is_small_int(S[H + a + 1u])) {
          // A custom iterator (ITER_INIT left a non-integer here): one step of its type's
          // `iterator_next`, a GC point. The instruction is at pc - 2 (its jump target follows it).
          exec->current_function = fidx;
          exec->current_offset = (uint32_t)(pc - 2u);
          S[GLTANG_F_PC] = pc - 2u;
          S[GLTANG_F_SP] = sp;
          STAND_AT_OP();
          GLTANG_Value item = GLTANG_V_NULL;
          GLTANG_IterStep step = gltang_vm_custom_iterator_next(exec, S[H + a], &item);
          STOOD();
          if (step == GLTANG_ITER_UNWIND) {
            goto unwound;
          }
          RELOAD();
          if (step == GLTANG_ITER_ITEM) {
            PUSH(item);
          }
          else {
            pc = target;
            if (step == GLTANG_ITER_ERROR) {
              // The error ends the loop and is its value, as an ITER_INIT error is (13.12): the
              // statement after the loop that clears the result is stepped over.
              gltang_vm_set_result(exec, item, true);
              if (GLTANG_INSTRUCTION_OP(code[pc]) == GLTANG_OP_CLEAR_RESULT) {
                ++pc;
              }
            }
          }
          break;
        }
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
  // No STOOD() here: every unwind of a standing operation came from a runtime poll inside it, and
  // that poll has already written the identity and ended the stand (gltang_vm_native_poll); an
  // unwind out of a nested run leaves through the operation's own STOOD() after its call.
  gltang_vm_flush_fuel(exec);
  if (exec->act != exec->run_base_act && exec->act->policy != GLTANG_SCOPE_PAUSE
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
  // first, and the stack is emptied (the scopes close with it). A nested run
  // (AD-23) ends at its own base: the template calls it opened and the frames
  // above its record go, and the native that opened it takes the unwind back to
  // the run that called it.
  const bool nested = exec->run_base_frames != 0;
  while (exec->act != exec->run_base_act) {
    GLTANG_Activation * open = exec->act;
    gltang_vm_activation_leave(exec, open);
    gltang_vm_activation_free(exec, open);
  }
  while (exec->temp_count > exec->run_base_temps) {
    gltang_vm_temp_pop(exec);
  }
  if (nested) {
    (void)grcore_unwind_to_activation(stack, exec->run_base_record, NULL);
#ifdef GLTANG_WITH_JIT
    gltang_jit_unwound(exec);
#endif
    return GRCORE_STEP_UNWOUND;
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
#undef SAVE_AT_OP
#undef STAND_AT_OP
#undef STOOD
#undef PUSH
#undef CUSTOM_TRUTH
#undef DROP
}

GRCORE_Step gltang_execution_entry(GRCORE_Context * context, void * state) {
  GLTANG_Execution * exec = state;
  if (!exec || exec->destroyed || !context || exec->context != context) {
    // Nothing to run: report the end rather than a pause or an unwind, which
    // would be a lie about a poll that never happened.
    return GRCORE_STEP_FINISHED;
  }
  if (!exec->activations_reserved) {
    // The first native call or entry into compiled code opens an activation
    // record, and the array of records is allocated at the first one the context
    // opens, charged to the guest's memory budget. Opening and leaving one here,
    // before the program runs, makes that allocation the same whether or not
    // anything is ever compiled, so a budget gives the same verdict on both tiers
    // (and in the build without the JIT).
    GRCORE_ActivationRef record;
    GRCORE_Stack * stack = grcore_context_stack(context);
    if (grcore_activation_enter(stack, GRCORE_ACTIVATION_INTERPRETER, exec->engine, false, NULL, &record) == GRCORE_OK) {
      (void)grcore_activation_leave(stack, record);
    }
    exec->activations_reserved = true;
  }
  GRCORE_Step step = gltang_vm_run(exec, context);
#ifdef GLTANG_WITH_JIT
  // Control goes back to the host (a pause, the end, an unwind): no compiled code is
  // open, so the float boxes made ahead for it are let go, and nothing a host can
  // see (a snapshot's root slots above all) depends on them.
  gltang_jit_release_pool(exec);
#endif
  return step;
}
