/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/tests/client_library_test_base.h"

#include <string>

#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/client/computation.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/execution_options_util.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/ptr_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/test_helpers.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"

namespace se = ::perftools::gputools;

namespace xla {
namespace {
// Wrapper function that creates a nicer error message (than a bare
// ValueOrDie()) if the platform we intend to test is not available.
Client* GetOrCreateLocalClientOrDie(const LocalClientOptions& client_options) {
  StatusOr<Client*> result =
      ClientLibrary::GetOrCreateLocalClient(client_options);
  TF_CHECK_OK(result.status()) << " could not create local client for testing";
  return result.ValueOrDie();
}
}  // namespace

ClientLibraryTestBase::ClientLibraryTestBase(
    perftools::gputools::Platform* platform,
    const LocalClientOptions& client_options)
    : client_(GetOrCreateLocalClientOrDie(client_options)),
      execution_options_(CreateDefaultExecutionOptions()) {
  CHECK_EQ(platform, client_options.platform());
  // Disabling constant_folding so that tests (usually written using Constants)
  // will exercise the intended code paths, instead of being constant folded.
  //
  // TODO(b/38354253): Constant folding is currently disabled. Change tests to
  // use Parameters instead of Constants, and re-enable constant folding by
  // default.
  execution_options_.mutable_debug_options()->add_xla_disable_hlo_passes(
      "constant_folding");
}

ClientLibraryTestBase::ClientLibraryTestBase(se::Platform* platform)
    : execution_options_(CreateDefaultExecutionOptions()) {
  LocalClientOptions default_options;
  default_options.set_platform(platform);
  client_ = GetOrCreateLocalClientOrDie(default_options);
  execution_options_.mutable_debug_options()->add_xla_disable_hlo_passes(
      "constant_folding");
}

string ClientLibraryTestBase::TestName() const {
  return ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

StatusOr<std::unique_ptr<GlobalData>> ClientLibraryTestBase::Execute(
    ComputationBuilder* builder,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments) {
  // Build the computation, as a convenience.
  TF_ASSIGN_OR_RETURN(auto computation, builder->Build());
  return client_->Execute(computation, arguments, &execution_options_);
}

StatusOr<std::unique_ptr<Literal>> ClientLibraryTestBase::ExecuteAndTransfer(
    const Computation& computation,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments,
    const Shape* shape_with_output_layout) {
  ExecutionOptions execution_options = execution_options_;
  if (shape_with_output_layout != nullptr) {
    *execution_options.mutable_shape_with_output_layout() =
        *shape_with_output_layout;
  }
  return client_->ExecuteAndTransfer(computation, arguments,
                                     &execution_options);
}

StatusOr<std::unique_ptr<Literal>> ClientLibraryTestBase::ExecuteAndTransfer(
    ComputationBuilder* builder,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments,
    const Shape* shape_with_output_layout) {
  // Build the computation, as a convenience.
  TF_ASSIGN_OR_RETURN(auto computation, builder->Build());
  return ExecuteAndTransfer(computation, arguments, shape_with_output_layout);
}

std::unique_ptr<GlobalData> ClientLibraryTestBase::ExecuteOrDie(
    ComputationBuilder* builder,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments) {
  return Execute(builder, arguments).ConsumeValueOrDie();
}

std::unique_ptr<Literal> ClientLibraryTestBase::ExecuteAndTransferOrDie(
    ComputationBuilder* builder,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments) {
  return ExecuteAndTransfer(builder, arguments).ConsumeValueOrDie();
}

string ClientLibraryTestBase::ExecuteToString(
    ComputationBuilder* builder,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments) {
  StatusOr<Computation> computation_status = builder->Build();
  if (!computation_status.ok()) {
    return computation_status.status().ToString();
  }
  Computation computation = computation_status.ConsumeValueOrDie();

  auto result =
      client_->ExecuteAndTransfer(computation, arguments, &execution_options_);
  if (!result.ok()) {
    return result.status().ToString();
  } else {
    return result.ValueOrDie()->ToString();
  }
}

void ClientLibraryTestBase::ComputeAndCompareR1(
    ComputationBuilder* builder, const tensorflow::core::Bitmap& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments) {
  std::unique_ptr<Literal> expected_literal = Literal::CreateR1(expected);
  ClientLibraryTestBase::ComputeAndCompareLiteral(builder, *expected_literal,
                                                  arguments);
}

void ClientLibraryTestBase::ComputeAndCompareLiteral(
    ComputationBuilder* builder, const Literal& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments,
    const Shape* shape_with_layout) {
  EXPECT_IS_OK(ComputeAndCompareLiteralWithStatus(builder, expected, arguments,
                                                  shape_with_layout));
}

void ClientLibraryTestBase::ComputeAndCompareLiteral(
    ComputationBuilder* builder, const Literal& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments, ErrorSpec error,
    const Shape* shape_with_layout) {
  EXPECT_IS_OK(ComputeAndCompareLiteralWithStatus(builder, expected, arguments,
                                                  error, shape_with_layout));
}

tensorflow::Status
ClientLibraryTestBase::ComputeAndCompareLiteralWithAllOutputLayouts(
    const xla::Computation& computation, const Literal& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments,
    const std::function<void(const Literal& actual,
                             const string& error_message)>& verify_output) {
  // Try with no layout requirement.
  TF_ASSIGN_OR_RETURN(auto actual, ExecuteAndTransfer(computation, arguments));
  verify_output(*actual, "");

  // Try with all output layouts.
  std::vector<int64> minor_to_major(ShapeUtil::Rank(expected.shape()));
  std::iota(minor_to_major.begin(), minor_to_major.end(), 0);
  do {
    auto layout = ShapeUtil::MakeShapeWithLayout(
        expected.shape().element_type(),
        AsInt64Slice(expected.shape().dimensions()), minor_to_major);
    TF_ASSIGN_OR_RETURN(auto actual,
                        ExecuteAndTransfer(computation, arguments, &layout));
    verify_output(*actual, tensorflow::strings::StrCat(
                               "Test with output layout: ",
                               ShapeUtil::HumanStringWithLayout(layout)));
  } while (std::next_permutation(minor_to_major.begin(), minor_to_major.end()));
  return tensorflow::Status::OK();
}

tensorflow::Status
ClientLibraryTestBase::ComputeAndCompareLiteralWithAllInputLayouts(
    const xla::Computation& computation, const Literal& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments,
    const std::function<void(const Literal& actual,
                             const string& error_message)>& verify_output,
    const Shape* output_with_layout) {
  std::vector<GlobalData*> arguments_with_layout;
  std::vector<string> layout_strings;
  // This is a recursive function. It's an std::function instead of a lambda
  // because it needs to capture itself. The index is the index of the argument
  // to try all layouts for.
  std::function<tensorflow::Status(int64)> choose;
  choose = [&, this](int64 index) -> tensorflow::Status {
    if (index < arguments.size()) {
      // Try out all layouts for the operand.
      TF_ASSIGN_OR_RETURN(auto literal,
                          client_->Transfer(*arguments[index], nullptr));
      // Skip tuples because they don't have a rank.
      if (ShapeUtil::IsTuple(literal->shape())) {
        layout_strings.push_back(
            ShapeUtil::HumanStringWithLayout(literal->shape()));
        arguments_with_layout.push_back(arguments[index]);
        TF_RETURN_IF_ERROR(choose(index + 1));
        arguments_with_layout.pop_back();
        layout_strings.pop_back();
        return tensorflow::Status::OK();
      }

      std::vector<int64> minor_to_major(ShapeUtil::Rank(literal->shape()));
      std::iota(minor_to_major.begin(), minor_to_major.end(), 0);
      do {
        auto literal_relayout =
            literal->Relayout(LayoutUtil::MakeLayout(minor_to_major));
        layout_strings.push_back(
            ShapeUtil::HumanStringWithLayout(literal_relayout->shape()));
        TF_ASSIGN_OR_RETURN(auto data,
                            client_->TransferToServer(*literal_relayout));
        arguments_with_layout.push_back(data.get());
        TF_RETURN_IF_ERROR(choose(index + 1));
        arguments_with_layout.pop_back();
        layout_strings.pop_back();
      } while (
          std::next_permutation(minor_to_major.begin(), minor_to_major.end()));
      return tensorflow::Status::OK();
    }

    // Every argument has an assigned layout.
    TF_ASSIGN_OR_RETURN(
        auto actual,
        ExecuteAndTransfer(
            computation,
            tensorflow::gtl::ArraySlice<GlobalData*>(arguments_with_layout),
            output_with_layout));
    string error_message = "Test with input layouts: ";
    for (const auto& str : layout_strings) {
      tensorflow::strings::StrAppend(&error_message, str, " ");
    }
    verify_output(*actual, error_message);
    return tensorflow::Status::OK();
  };

  return choose(0);
}

tensorflow::Status ClientLibraryTestBase::ComputeAndCompareLiteralWithStatus(
    ComputationBuilder* builder, const Literal& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments,
    const Shape* shape_with_layout) {
  TF_ASSIGN_OR_RETURN(auto computation, builder->Build());
  if (ShapeUtil::ElementIsFloating(expected.shape()) ||
      ShapeUtil::ElementIsComplex(expected.shape())) {
    LOG(WARNING) << "performing exact comparison of floating point numbers";
  } else {
    TF_RET_CHECK(ShapeUtil::ElementIsIntegral(expected.shape()) ||
                 expected.shape().element_type() == PRED)
        << ShapeUtil::HumanString(expected.shape());
  }
  auto expect_equal = [&](const Literal& actual, const string& error_message) {
    LiteralTestUtil::ExpectEqual(expected, actual, error_message);
  };
  if (execution_options_.debug_options().xla_test_all_output_layouts()) {
    return ComputeAndCompareLiteralWithAllOutputLayouts(
        computation, expected, arguments, expect_equal);
  }
  if (execution_options_.debug_options().xla_test_all_input_layouts()) {
    return ComputeAndCompareLiteralWithAllInputLayouts(
        computation, expected, arguments, expect_equal, shape_with_layout);
  }
  TF_ASSIGN_OR_RETURN(auto actual, ExecuteAndTransfer(computation, arguments,
                                                      shape_with_layout));
  LiteralTestUtil::ExpectEqual(expected, *actual);
  return tensorflow::Status::OK();
}

tensorflow::Status ClientLibraryTestBase::ComputeAndCompareLiteralWithStatus(
    ComputationBuilder* builder, const Literal& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments, ErrorSpec error,
    const Shape* shape_with_layout) {
  TF_RET_CHECK(ShapeUtil::ElementIsFloating(expected.shape()) ||
               ShapeUtil::ElementIsComplex(expected.shape()));
  TF_ASSIGN_OR_RETURN(auto computation, builder->Build());
  auto expect_near = [&](const Literal& actual, const string& error_message) {
    LiteralTestUtil::ExpectNear(expected, actual, error, error_message);
  };
  if (execution_options_.debug_options().xla_test_all_output_layouts()) {
    return ComputeAndCompareLiteralWithAllOutputLayouts(computation, expected,
                                                        arguments, expect_near);
  }
  if (execution_options_.debug_options().xla_test_all_input_layouts()) {
    return ComputeAndCompareLiteralWithAllInputLayouts(
        computation, expected, arguments, expect_near, shape_with_layout);
  }
  TF_ASSIGN_OR_RETURN(auto actual, ExecuteAndTransfer(computation, arguments,
                                                      shape_with_layout));
  LiteralTestUtil::ExpectNear(expected, *actual, error);
  return tensorflow::Status::OK();
}

void ClientLibraryTestBase::ComputeAndCompareR1U8(
    ComputationBuilder* builder, tensorflow::StringPiece expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments) {
  auto actual_status = ExecuteAndTransfer(builder, arguments);
  EXPECT_IS_OK(actual_status.status());
  if (!actual_status.ok()) {
    return;
  }
  auto actual = actual_status.ConsumeValueOrDie();

  // Turn the expected value into a literal.
  std::unique_ptr<Literal> expected_literal = Literal::CreateR1U8(expected);

  VLOG(1) << "expected: " << expected_literal->ToString();
  VLOG(1) << "actual:   " << actual->ToString();

  EXPECT_EQ(expected, actual->u8s_string());
}

void ClientLibraryTestBase::ComputeAndCompareTuple(
    ComputationBuilder* builder, const Literal& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments) {
  auto actual_status = ExecuteAndTransfer(builder, arguments);
  EXPECT_IS_OK(actual_status.status());
  if (!actual_status.ok()) {
    return;
  }
  auto actual = actual_status.ConsumeValueOrDie();
  LiteralTestUtil::ExpectEqualTuple(expected, *actual);
}

void ClientLibraryTestBase::ComputeAndCompareTuple(
    ComputationBuilder* builder, const Literal& expected,
    tensorflow::gtl::ArraySlice<GlobalData*> arguments, ErrorSpec error) {
  auto actual_status = ExecuteAndTransfer(builder, arguments);
  EXPECT_IS_OK(actual_status.status());
  if (!actual_status.ok()) {
    return;
  }
  auto actual = actual_status.ConsumeValueOrDie();
  LiteralTestUtil::ExpectNearTuple(expected, *actual, error);
}

Computation ClientLibraryTestBase::CreateScalarRelu() {
  ComputationBuilder builder(client_, "relu");
  auto z_value = builder.Parameter(0, ShapeUtil::MakeShape(F32, {}), "z_value");
  auto zero = builder.ConstantR0<float>(0.0);
  builder.Max(z_value, zero);
  auto computation_status = builder.Build();
  TF_CHECK_OK(computation_status.status());
  return computation_status.ConsumeValueOrDie();
}

Computation ClientLibraryTestBase::CreateScalarMax() {
  ComputationBuilder builder(client_, "max");
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {}), "x");
  auto y = builder.Parameter(1, ShapeUtil::MakeShape(F32, {}), "y");
  builder.Max(x, y);
  auto computation_status = builder.Build();
  TF_CHECK_OK(computation_status.status());
  return computation_status.ConsumeValueOrDie();
}

Computation ClientLibraryTestBase::CreateScalarReluSensitivity() {
  ComputationBuilder builder(client_, "relu_sensitivity");
  auto activation =
      builder.Parameter(0, ShapeUtil::MakeShape(F32, {}), "activation");
  auto backprop =
      builder.Parameter(1, ShapeUtil::MakeShape(F32, {}), "backprop");
  auto zero = builder.ConstantR0<float>(0.0);
  auto activation_gtz = builder.Gt(activation, zero);
  builder.Select(activation_gtz, /*on_true=*/backprop, /*on_false=*/zero);

  auto computation_status = builder.Build();
  TF_CHECK_OK(computation_status.status());
  return computation_status.ConsumeValueOrDie();
}

std::unique_ptr<Array2D<float>> ClientLibraryTestBase::CreatePatternedMatrix(
    int rows, int cols, float offset) {
  auto array = MakeUnique<Array2D<float>>(rows, cols);
  for (int64 row = 0; row < rows; ++row) {
    for (int64 col = 0; col < cols; ++col) {
      (*array)(row, col) = col + (row * 1000.0f) + offset;
    }
  }
  return array;
}

std::unique_ptr<Array2D<float>>
ClientLibraryTestBase::CreatePatternedMatrixWithZeroPadding(int rows, int cols,
                                                            int rows_padded,
                                                            int cols_padded) {
  CHECK_GE(rows_padded, rows);
  CHECK_GE(cols_padded, cols);
  auto array = MakeUnique<Array2D<float>>(rows_padded, cols_padded, 0.0);
  for (int64 row = 0; row < rows; ++row) {
    for (int64 col = 0; col < cols; ++col) {
      (*array)(row, col) = col + (row * 1000.0f);
    }
  }
  return array;
}

}  // namespace xla
