/* Copyright (c) Microsoft Corporation.

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

#include "tensorflow/core/common_runtime/dml/dml_util.h"

#include "tensorflow/core/common_runtime/dml/dml_bfc_allocator.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/stream_executor/platform/default/dso_loader.h"

namespace tensorflow {

// Helper to call a DX-style Create* function from a module that is loaded at
// runtime. Assumes the function returns an HRESULT and its last two parameters
// are an IID and void** (e.g. D3D12CreateDevice, DMLCreateDevice, etc.).
template <typename TDeviceOrFactory, typename F, typename... Args>
Microsoft::WRL::ComPtr<TDeviceOrFactory> DxExportHelper(
    StatusOr<void*> module_handle, const char* module_name,
    const char* function_name, DxCallErrorHandling call_error_handling,
    DxCallErrorHandling module_error_handling, Args&&... args) {
  // Lazily load the module.
  if (!module_handle.ok()) {
    switch (module_error_handling) {
      case DxCallErrorHandling::Warning:
        LOG(WARNING) << "Could not load '" << module_name << "' module.";
        break;
      case DxCallErrorHandling::Fatal:
        LOG(FATAL) << "Could not load '" << module_name << "' module.";
        break;
    }
    return nullptr;
  }

  // Load the function.
  F* create_function;
  auto get_symbol_status = Env::Default()->GetSymbolFromLibrary(
      module_handle.ValueOrDie(), function_name, (void**)&create_function);
  if (!get_symbol_status.ok()) {
    switch (module_error_handling) {
      case DxCallErrorHandling::Warning:
        LOG(WARNING) << "Could not find symbol '" << function_name << "' in '"
                     << module_name << "' module.";
        break;
      case DxCallErrorHandling::Fatal:
        LOG(FATAL) << "Could not find symbol '" << function_name << "' in '"
                   << module_name << "' module.";
        break;
    }
    return nullptr;
  }

  // Call the function.
  Microsoft::WRL::ComPtr<TDeviceOrFactory> device_or_factory;
  HRESULT hr = create_function(std::forward<Args>(args)...,
                               IID_PPV_ARGS(&device_or_factory));
  if (FAILED(hr)) {
    switch (call_error_handling) {
      case DxCallErrorHandling::Warning:
        LOG(WARNING) << "'" << function_name << "' failed with HRESULT " << hr;
        break;
      case DxCallErrorHandling::Fatal:
        LOG(FATAL) << "'" << function_name << "' failed with HRESULT " << hr;
        break;
    }
    return nullptr;
  }

  return device_or_factory;
}

Microsoft::WRL::ComPtr<ID3D12Device> TryCreateD3d12Device(
    IUnknown* adapter, D3D_FEATURE_LEVEL minimum_feature_level,
    DxCallErrorHandling call_error_handling,
    DxCallErrorHandling module_error_handling) {
  return DxExportHelper<ID3D12Device, decltype(D3D12CreateDevice)>(
      stream_executor::internal::CachedDsoLoader::GetD3d12DsoHandle(),
      "D3D12",                // module name (for logging messages only)
      "D3D12CreateDevice",    // function name
      call_error_handling,    // error handling when function call fails
      module_error_handling,  // error handling when module load fails
      adapter,                // D3D12CreateDevice arguments
      minimum_feature_level   // D3D12CreateDevice arguments
  );
}

#ifdef _WIN32
Microsoft::WRL::ComPtr<IDXGIFactory4> TryCreateDxgiFactory(
    DxCallErrorHandling call_error_handling,
    DxCallErrorHandling module_error_handling) {
  return DxExportHelper<IDXGIFactory4, decltype(CreateDXGIFactory)>(
      stream_executor::internal::CachedDsoLoader::GetDxgiDsoHandle(),
      "DXGI",                // module name (for logging messages only)
      "CreateDXGIFactory",   // function name
      call_error_handling,   // error handling when function call fails
      module_error_handling  // error handling when module load fails
  );
}
#endif  // _WIN32

#ifndef _WIN32
Microsoft::WRL::ComPtr<IDXCoreAdapterFactory> TryCreateDxCoreAdapterFactory(
    DxCallErrorHandling call_error_handling,
    DxCallErrorHandling module_error_handling) {
  // DXCoreCreateAdapterFactory has a C++ overload so we must be explicit in the
  // function signature.
  using FuncType = HRESULT __stdcall(REFIID, void**);
  return DxExportHelper<IDXCoreAdapterFactory, FuncType>(
      stream_executor::internal::CachedDsoLoader::GetDxCoreDsoHandle(),
      "DXCore",                      // module name (for logging messages only)
      "DXCoreCreateAdapterFactory",  // function name
      call_error_handling,           // error handling when function call fails
      module_error_handling          // error handling when module load fails
  );
}
#endif  // !_WIN32

Microsoft::WRL::ComPtr<IDMLDevice> TryCreateDmlDevice(
    ID3D12Device* d3d12_device, DML_CREATE_DEVICE_FLAGS dml_flags,
    DxCallErrorHandling call_error_handling,
    DxCallErrorHandling module_error_handling) {
  auto module_handle =
      stream_executor::internal::CachedDsoLoader::GetDirectMLDsoHandle();
  if (!module_handle.ok() && getenv("TF_DIRECTML_PATH")) {
    LOG(WARNING) << "Could not find DirectML with TF_DIRECTML_PATH set.";
  }

  return DxExportHelper<IDMLDevice, decltype(DMLCreateDevice)>(
      module_handle,
      "DirectML",             // module name (for logging messages only)
      "DMLCreateDevice",      // function name
      call_error_handling,    // error handling when function call fails
      module_error_handling,  // error handling when module load fails
      d3d12_device,           // DMLCreateDevice arguments
      dml_flags               // DMLCreateDevice arguments
  );
}

DataType GetTfDataTypeFromDmlDataType(DML_TENSOR_DATA_TYPE type) {
  switch (type) {
    case DML_TENSOR_DATA_TYPE_FLOAT32:
      return DT_FLOAT;
    case DML_TENSOR_DATA_TYPE_FLOAT16:
      return DT_HALF;
    case DML_TENSOR_DATA_TYPE_UINT64:
      return DT_UINT64;
    case DML_TENSOR_DATA_TYPE_UINT32:
      return DT_UINT32;
    case DML_TENSOR_DATA_TYPE_UINT16:
      return DT_UINT16;
    case DML_TENSOR_DATA_TYPE_UINT8:
      return DT_UINT8;
    case DML_TENSOR_DATA_TYPE_INT64:
      return DT_INT64;
    case DML_TENSOR_DATA_TYPE_INT32:
      return DT_INT32;
    case DML_TENSOR_DATA_TYPE_INT16:
      return DT_INT16;
    case DML_TENSOR_DATA_TYPE_INT8:
      return DT_INT8;
    default:
      LOG(FATAL) << "Invalid or unsupported data type.";
  }
}

DML_TENSOR_DATA_TYPE GetDmlDataTypeFromTfDataType(DataType type) {
  switch (type) {
    case DT_UINT64:
    case DT_UINT64_REF:
      return DML_TENSOR_DATA_TYPE_UINT64;
    case DT_INT64:
    case DT_INT64_REF:
      return DML_TENSOR_DATA_TYPE_INT64;
    case DT_FLOAT:
    case DT_FLOAT_REF:
      return DML_TENSOR_DATA_TYPE_FLOAT32;
    case DT_HALF:
    case DT_HALF_REF:
      return DML_TENSOR_DATA_TYPE_FLOAT16;
    case DT_UINT32:
    case DT_UINT32_REF:
      return DML_TENSOR_DATA_TYPE_UINT32;
    case DT_UINT16:
    case DT_UINT16_REF:
      return DML_TENSOR_DATA_TYPE_UINT16;
    case DT_UINT8:
    case DT_UINT8_REF:
    case DT_BOOL:
    case DT_BOOL_REF:
      return DML_TENSOR_DATA_TYPE_UINT8;
    case DT_INT32:
    case DT_INT32_REF:
      return DML_TENSOR_DATA_TYPE_INT32;
    case DT_INT16:
    case DT_INT16_REF:
      return DML_TENSOR_DATA_TYPE_INT16;
    case DT_INT8:
    case DT_INT8_REF:
      return DML_TENSOR_DATA_TYPE_INT8;
    default:
      LOG(FATAL) << "Invalid or unsupported data type.";
  }
}

uint32_t GetDmlDimensionIndex(DmlTensorAxis axis,
                              uint32_t dml_dimension_count) {
  using namespace DmlTensorAxes;

  if (dml_dimension_count == kNchwDimensionCount) {
    switch (axis) {
      case N:
        return 0;
      case C:
        return 1;
      case H:
        return 2;
      case W:
        return 3;
      default:
        assert(false);
        LOG(FATAL) << "Invalid tensor axis";
    }
  } else {
    assert(dml_dimension_count == kNcdhwDimensionCount);

    switch (axis) {
      case N:
        return 0;
      case C:
        return 1;
      case D:
        return 2;
      case H:
        return 3;
      case W:
        return 4;
      default:
        assert(false);
        LOG(FATAL) << "Invalid tensor axis";
    }
  }
}

DmlTensorLayout GetDmlTensorLayout(TensorFormat format, uint32_t rank) {
  CHECK(rank <= DML_TENSOR_DIMENSION_COUNT_MAX);

  DmlTensorLayout tensor_layout;

  // When converting TF tensor formats to DML tensor layouts, we by default drop
  // dimensions from the left if the dimension count < 4. e.g. if the format is
  // NHWC and rank is 2, we return a layout of WC.

  switch (format) {
    case FORMAT_NHWC:
      if (rank >= 4) {
        tensor_layout.push_back(DmlTensorAxis::N);
      }
      if (rank >= 5) {
        tensor_layout.push_back(DmlTensorAxis::D);
      }
      if (rank >= 3) {
        tensor_layout.push_back(DmlTensorAxis::H);
      }
      if (rank >= 2) {
        tensor_layout.push_back(DmlTensorAxis::W);
      }
      if (rank >= 1) {
        tensor_layout.push_back(DmlTensorAxis::C);
      }
      break;
    case FORMAT_NCHW:
      if (rank >= 4) {
        tensor_layout.push_back(DmlTensorAxis::N);
      }
      if (rank >= 3) {
        tensor_layout.push_back(DmlTensorAxis::C);
      }
      if (rank >= 5) {
        tensor_layout.push_back(DmlTensorAxis::D);
      }
      if (rank >= 2) {
        tensor_layout.push_back(DmlTensorAxis::H);
      }
      if (rank >= 1) {
        tensor_layout.push_back(DmlTensorAxis::W);
      }
      break;
    default:
      LOG(FATAL) << "Unsupported tensor layout";
  }

  return tensor_layout;
}

dml::TensorPolicy GetDmlXTensorPolicy(TensorFormat format) {
  switch (format) {
    case FORMAT_NHWC:
      return dml::TensorPolicy::InterleavedChannel();
    case FORMAT_NCHW:
      return dml::TensorPolicy::Default();
    default:
      LOG(FATAL) << "Unsupported tensor layout";
  }
}

dml::TensorStrides ComputePackedStrides(
    const dml::Span<const uint32_t>& sizes) {
  dml::TensorStrides strides(sizes.size());
  uint32_t stride = 1;
  for (int i = sizes.size() - 1; i >= 0; --i) {
    strides[i] = stride;
    stride *= sizes[i];
  }
  return strides;
}

namespace dml_util {

void CopyTensorInSameDevice(OpKernelContext* op_ctx, Tensor* dst,
                            const Tensor& src) {
  auto* device = static_cast<Device*>(op_ctx->device());
  op_ctx->op_device_context()->CopyTensorInSameDevice(
      &src, device, dst,
      [op_ctx](const Status& s) { OP_REQUIRES_OK(op_ctx, s); });
}

absl::InlinedVector<absl::optional<DML_BUFFER_BINDING>, 8> GetBufferBindings(
    absl::Span<const D3D12BufferRegion> buffers) {
  absl::InlinedVector<absl::optional<DML_BUFFER_BINDING>, 8> bindings;
  bindings.reserve(buffers.size());

  for (const auto& buffer : buffers) {
    if (buffer) {
      bindings.push_back(buffer.GetBufferBinding());
    } else {
      bindings.push_back(absl::nullopt);
    }
  }

  return bindings;
}

}  // namespace dml_util

}  // namespace tensorflow