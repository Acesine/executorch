/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/core/event_tracer.h>
#include <executorch/runtime/core/exec_aten/exec_aten.h>
#include <executorch/runtime/executor/memory_manager.h>
#include <executorch/runtime/platform/compiler.h>

// Forward declare flatbuffer types. This is a public header and must not
// include the generated flatbuffer header.
namespace executorch_flatbuffer {
struct Chain;
struct ExecutionPlan;
struct EValue;
} // namespace executorch_flatbuffer

namespace torch {
namespace executor {

// Forward declare Program to avoid a circular reference.
class Program;

// Forward declare internal types.
class BackendDelegate;
struct Chain;
template <typename Fn>
class FunctionRef;
template <typename T>
class Span;
class KernelRuntimeContext;
using OpFunction = FunctionRef<void(KernelRuntimeContext&, EValue**)>;
/// A list of pointers into the master values table that together compose the
/// argument list for a single instruction
using InstructionArgs = Span<EValue*>;

/**
 * An executable method of an executorch program. Maps to a python method like
 * `forward()` on the original nn.Module.
 */
class Method final {
 public:
  Method(
      const Program* program,
      MemoryManager* memory_manager,
      EventTracer* event_tracer)
      : step_state_(),
        program_(program),
        memory_manager_(memory_manager),
        serialization_plan_(nullptr),
        event_tracer_(event_tracer),
        n_value_(0),
        values_(nullptr),
        n_delegate_(0),
        delegates_(nullptr),
        n_chains_(0),
        chains_(nullptr),
        init_state_(InitializationState::Uninitialized),
        pre_allocated_input_(false) {}

  /**
   * Move ctor. Takes ownership of resources previously owned by `rhs`,
   * and leaves `rhs` in an uninitialized state.
   */
  Method(Method&& rhs) noexcept
      : step_state_(rhs.step_state_),
        program_(rhs.program_),
        memory_manager_(rhs.memory_manager_),
        serialization_plan_(rhs.serialization_plan_),
        event_tracer_(rhs.event_tracer_),
        n_value_(rhs.n_value_),
        values_(rhs.values_),
        n_delegate_(rhs.n_delegate_),
        delegates_(rhs.delegates_),
        n_chains_(rhs.n_chains_),
        chains_(rhs.chains_),
        init_state_(rhs.init_state_),
        pre_allocated_input_(rhs.pre_allocated_input_) {
    // Required: clear out fields that the dtor looks at, so that we don't free
    // anything twice.
    rhs.n_value_ = 0;
    rhs.values_ = nullptr;
    rhs.n_delegate_ = 0;
    rhs.delegates_ = nullptr;

    // Helpful: Try to ensure that any other interactions with the old object
    // result in failures.
    rhs.init_state_ = InitializationState::Uninitialized;
    rhs.step_state_ = {};
    rhs.program_ = nullptr;
    rhs.memory_manager_ = nullptr;
    rhs.serialization_plan_ = nullptr;
    rhs.n_chains_ = 0;
    rhs.chains_ = nullptr;
    rhs.pre_allocated_input_ = false;
    rhs.event_tracer_ = nullptr;
  }

  /**
   * Initialize the method from its serialized representation.
   *
   * @returns Error::Ok on success, non-Ok on failure.
   */
  __ET_NODISCARD Error init(executorch_flatbuffer::ExecutionPlan* s_plan);

  /**
   * Sets a specific method input to the provided value.
   *
   * NOTE: Based on the memory plan of the method, the inputs may not have
   * buffer space pre-allocated for them, in this case the executor will alias
   * the memory of the tensors provided as inputs here, so the user should take
   * care that the life span of this memory outlasts the executor forward.
   *
   * @param[in] input_evalue The value to set the input to. The type of this
   *     must match the type of the corresponding input. If this value is a
   *     tensor, attempts to allow dynamic shape, but the dtype must always
   *     agree.
   * @param[in] input_idx Zero-based index of the input to set. Must be less
   *     than the value returned by inputs_size().
   *
   * @returns Error::Ok on success, non-Ok on failure.
   */
  __ET_NODISCARD Error set_input(const EValue& input_evalue, size_t input_idx);

  /**
   * Sets the values of all method inputs.
   *
   * See NOTE on set_input().
   *
   * @param[in] input_evalues The new values for all of the method inputs. The
   *     type of each element must match the type of corresponding input. If the
   *     value of an element is a tensor, attempts to allow dynamic shape, but
   *     the dtype must always agree.
   *
   * @returns Error::Ok on success, non-Ok on failure.
   */
  __ET_NODISCARD Error
  set_inputs(const exec_aten::ArrayRef<EValue>& input_evalues);

  /**
   * Copies the method's outputs into the provided array.
   *
   * WARNING: The output contains shallow copies of internal tensor outputs.
   * Please do not mutate returned Tensor elements.
   *
   * TODO(T139259264): Add checks to detect output mutation, or deep-copy
   * outputs.
   *
   * @param[in] output_evalues The array to copy the outputs into. The first
   *     `outputs_size()` elements will be set to the corresponding output
   *     values. The rest of the array will be set to the EValue value None.
   * @param[in] length The size of the `output_evalues` array in elements. Must
   *     be greater than or equal to `outputs_size()`.
   *
   * @returns Error::Ok on success, non-Ok on failure.
   */
  __ET_NODISCARD Error get_outputs(EValue* output_evalues, size_t length);

  /**
   * Execute the method.
   *
   * NOTE: Will fail if the method has been partially executed using the
   * `experimental_step()` api.
   *
   * @returns Error::Ok on success, non-Ok on failure.
   */
  __ET_NODISCARD Error execute();

  /**
   * Advances/executes a single instruction in the method.
   *
   * NOTE: Prototype API; subject to change.
   *
   * @retval Error::Ok step succeeded
   * @retval non-Ok step failed
   * @retval Error::EndOfMethod method finished executing successfully
   */
  __ET_NODISCARD Error experimental_step();

  /**
   * Resets execution state to the start of the Method. For use with the
   * `experimental_step()` API.
   *
   * NOTE: Prototype API; subject to change.
   *
   * @retval Error:Ok on success
   * @retval Error::InvalidState if called before step-based execution reached
   *     the end of the Method. This means it is not possible to recover a
   *     Method that failed mid-execution.
   */
  __ET_NODISCARD Error experimental_reset_execution();

  size_t values_size() const;
  const EValue& get_value(size_t i) const;
  EValue& mutable_value(size_t i);
  size_t inputs_size() const;
  size_t get_input_index(size_t i) const;
  const EValue& get_input(size_t i) const;
  EValue& mutable_input(size_t i);
  size_t outputs_size() const;
  size_t get_output_index(size_t i) const;
  const EValue& get_output(size_t i) const;
  EValue& mutable_output(size_t i);
  ~Method();

 private:
  // Delete other rule-of-five methods.
  Method(const Method&) = delete;
  Method& operator=(const Method&) noexcept = delete;
  Method& operator=(Method&&) = delete;

  // Let Program call load().
  friend class Program;

  enum class InitializationState : uint8_t {
    Uninitialized,
    Initialized,
    InitializationFailed,
  };

  /// Tracks what step in program execution we are on
  struct StepState {
    size_t chain_idx;
    size_t instr_idx;
  };

  /// Static factory used by Program.
  __ET_NODISCARD static Result<Method> load(
      executorch_flatbuffer::ExecutionPlan* s_plan,
      const Program* program,
      MemoryManager* memory_manager,
      EventTracer* event_tracer);

  /// Returns true if the Method was successfully initialized.
  inline bool initialized() const {
    return init_state_ == InitializationState::Initialized;
  }

  // Executes a single instruction using the state in step_state_
  __ET_NODISCARD Error execute_instruction();

  StepState step_state_;
  const Program* program_;
  MemoryManager* memory_manager_;
  executorch_flatbuffer::ExecutionPlan* serialization_plan_;
  EventTracer* event_tracer_;

  size_t n_value_;
  EValue* values_;

  size_t n_delegate_;
  BackendDelegate* delegates_;

  size_t n_chains_;
  Chain* chains_;

  InitializationState init_state_;
  bool pre_allocated_input_;

  /**
   * Parses the elements of the values_ array. On error, n_value_ will be set to
   * the number of successfully-initialized entries so that ~Method doesn't try
   * to clean up uninitialized entries.
   */
  __ET_NODISCARD Error parse_values();

  __ET_NODISCARD Error resolve_operator(
      int32_t op_index,
      OpFunction* kernels,
      size_t kernel_index,
      InstructionArgs args,
      size_t n_args);
};

} // namespace executor
} // namespace torch