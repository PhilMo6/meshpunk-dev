// Modified for Meshpunk (2026-11-12) from conformer-stt-s3 libs/tlib/inc/
// tlib_tensor.h (Apache-2.0, Copyright 2026 lspr98): shapes and strides are
// a fixed-capacity Dims instead of std::vector (the module links no C++
// runtime library); a view can name a model-file tensor whose data stays on
// SD (StreamId); the Summary printers are gone.

#ifndef TLIB_TENSOR_H_
#define TLIB_TENSOR_H_

#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <initializer_list>
#include <utility>

#include <tlib_heap.h>

namespace tlib
{

    /**
     * Fixed-capacity shape/stride container. The engine's tensors have at most
     * 4 dimensions; the fifth slot leaves room for an Unsqueeze of a 4-D view.
     */
    class Dims
    {
    public:
        static constexpr uint8_t kMax{5};

        Dims(void) : n_{0}, d_{} {}

        Dims(std::initializer_list<uint32_t> init) : n_{0}, d_{}
        {
            assert(init.size() <= kMax);
            for (const uint32_t v : init)
            {
                d_[n_++] = v;
            }
        }

        // n zero-filled entries
        explicit Dims(const size_t n) : n_{static_cast<uint8_t>(n)}, d_{}
        {
            assert(n <= kMax);
        }

        size_t size(void) const { return n_; }
        uint32_t &at(const size_t i) { assert(i < n_); return d_[i]; }
        const uint32_t &at(const size_t i) const { assert(i < n_); return d_[i]; }
        uint32_t &operator[](const size_t i) { return d_[i]; }
        const uint32_t &operator[](const size_t i) const { return d_[i]; }
        uint32_t *data(void) { return d_; }
        const uint32_t *data(void) const { return d_; }
        uint32_t *begin(void) { return d_; }
        uint32_t *end(void) { return d_ + n_; }
        const uint32_t *begin(void) const { return d_; }
        const uint32_t *end(void) const { return d_ + n_; }

        void resize(const size_t n)
        {
            assert(n <= kMax);
            for (size_t i = n_; i < n; i++)
            {
                d_[i] = 0;
            }
            n_ = static_cast<uint8_t>(n);
        }

        void insert(const uint32_t *pos, const uint32_t v)
        {
            const size_t i = static_cast<size_t>(pos - d_);
            assert(n_ < kMax && i <= n_);
            for (size_t j = n_; j > i; j--)
            {
                d_[j] = d_[j - 1];
            }
            d_[i] = v;
            n_++;
        }

        void erase(const uint32_t *pos)
        {
            const size_t i = static_cast<size_t>(pos - d_);
            assert(i < n_);
            for (size_t j = i; j + 1 < n_; j++)
            {
                d_[j] = d_[j + 1];
            }
            n_--;
        }

        uint64_t Product(void) const
        {
            uint64_t p = 1;
            for (size_t i = 0; i < n_; i++)
            {
                p *= d_[i];
            }
            return p;
        }

    private:
        uint8_t n_;
        uint32_t d_[kMax];
    };

    /**
     * Non-data-owning read-only interface for tensors.
     */
    template <typename dtype>
    class TensorView
    {
    public:
        /**
         * Constructs an empty tensor view
         */
        explicit TensorView(void) {};

        /**
         * Constructs a tensor view ontop of the given data with the given parameters.
         *
         * @param   shape       Shape of the tensor
         * @param   stride      Stride of the tensor
         * @param   data        Pointer to tensor data
         * @param   shift       Quantization shift of the tensor
         * @param   quantized   Flag to indicate if this tensor is quantized or not
         */
        explicit TensorView(
            const Dims &&shape,
            const Dims &&stride,
            const dtype *data,
            const uint8_t shift,
            const bool quantized)
            : shape_{shape}, stride_{stride}, data_immutable_{data}, shift_{shift}, quantized_{quantized}
        {
            n_elements_ = shape_.Product();
        };

        /**
         * Constructs a view of a model-file tensor whose data stays on SD: no
         * data pointer; the kernels read it tile by tile (tlib_linear_impl).
         *
         * @param   shape       Shape of the tensor
         * @param   shift       Quantization shift of the tensor
         * @param   quantized   Flag to indicate if this tensor is quantized or not
         * @param   stream_id   The tensor's id in the model file
         */
        explicit TensorView(const Dims &&shape, const uint8_t shift, const bool quantized, const uint32_t stream_id)
            : shape_{shape}, shift_{shift}, quantized_{quantized}, stream_id_{static_cast<int32_t>(stream_id)}
        {
            UpdateStride();
            n_elements_ = shape_.Product();
        }

        /**
         * View this tensor view as a tensor view of different shape. The resulting number of elements
         * must stay the same. Causes a recomputation of strides.
         *
         * @param   shape   The new shape of the tensor
         */
        inline void View(const Dims &&shape)
        {
            const uint64_t n_elements = shape.Product();
            assert(n_elements_ == n_elements);
            shape_ = shape;
            UpdateStride();
        }

        /**
         * Performs the unsqueeze operation at the given dimension.
         * Example:
         * x.Shape() -> [5, 6, 7]
         * x.Unsqueeze(0)
         * x.Shape() -> [1, 5, 6, 7]
         *
         * @param   dim     Position at which to insert new dimension
         */
        inline void Unsqueeze(const uint8_t dim)
        {
            assert(dim < shape_.size());
            shape_.insert(shape_.begin() + dim, 1);
            UpdateStride();
        }

        /**
         * Performs the flatten operation between the specified dimensions.
         * Example:
         * x.Shape() -> [5, 6, 7, 8]
         * x.Flatten(2, 3)
         * x.Shape() -> [5, 6, 56]
         *
         * @param   dim0    Start dimension for flattening
         * @param   dim1    End dimension for flattening
         */
        inline void Flatten(const uint8_t dim0, const uint8_t dim1)
        {
            assert(dim0 < dim1);
            assert(dim1 < shape_.size());
            shape_.at(dim0) *= shape_.at(dim1);
            shape_.erase(shape_.begin() + dim1);
            UpdateStride();
        }

        /**
         * Access the underlying tensor data (read-only). nullptr for a
         * streamed tensor (Streamed()).
         *
         * @return  The underlying tensor data
         */
        inline constexpr const dtype *DataImm(void) const { return data_immutable_; };

        /**
         * Get the size of the underlying tensor data buffer in bytes.
         *
         * @return  The tensor data buffer size in bytes
         */
        inline constexpr size_t Bytes(void) const { return n_elements_ * sizeof(dtype); };

        /**
         * Get the number of dimensions.
         *
         * @return  The number of dimensions
         */
        inline size_t Dim(void) const { return shape_.size(); };

        /**
         * Get the shape of this tensor.
         *
         * @return  The shape of this tensor
         */
        inline Dims Shape(void) const { return shape_; };

        /**
         * Get the strides of this tensor.
         *
         * @return  The strides of this tensor
         */
        inline Dims Stride(void) const { return stride_; };

        /**
         * Get the dimension size at a specific index of this tensor.
         *
         * @param   index   Index of dimension
         *
         * @return  Size of dimension at index
         */
        inline uint32_t Shape(const uint8_t index) const { return shape_.at(index); };

        /**
         * Get the stride size at a specific index of this tensor.
         *
         * @param   index   Index of stride
         *
         * @return  Size of stride at index
         */
        inline uint32_t Stride(const uint8_t index) const { return stride_.at(index); };

        /**
         * Get the number of elements of this tensor.
         *
         * @return  The number of elements
         */
        inline constexpr size_t Numel(void) const { return n_elements_; };

        /**
         * Get the quantization shift of this tensor.
         *
         * @return  The quantization shift
         */
        inline constexpr uint8_t Shift(void) const { return shift_; };

        /**
         * Query if this tensor is quantized.
         *
         * @return  True, if this tensor is quantized, false otherwise
         */
        inline constexpr bool Quantized(void) const { return quantized_; };

        /**
         * The model-file tensor id of a streamed tensor, -1 otherwise.
         */
        inline constexpr int32_t StreamId(void) const { return stream_id_; };

    protected:
        /**
         * Constructs a tensor view based on shape. Should only be used if the
         * underlying buffer is populated otherwise.
         *
         * @param   shape   The shape of this tensor
         */
        explicit TensorView(const Dims &&shape) : shape_{shape}
        {
            UpdateStride();
            shift_ = 0;
            quantized_ = false;
            n_elements_ = shape_.Product();
        }

        Dims shape_;                           /*!< Shape of tensor */
        Dims stride_;                          /*!< Strides of tensor */
        uint64_t n_elements_{0};               /*!< Number of elements in tensor */
        const dtype *data_immutable_{nullptr}; /*!< Read-only pointer to tensor data */
        uint8_t shift_{0};                     /*!< Quantization shift of tensor */
        bool quantized_{false};                /*!< Indicates if tensor is quantized */
        int32_t stream_id_{-1};                /*!< Model-file tensor id when the data stays on SD */

    private:
        /**
         * Adjusts the strides to match the current shape.
         * This function assumes that the underlying memory layout is contiguous.
         */
        inline void UpdateStride(void)
        {
            const size_t dims = shape_.size();
            stride_.resize(dims);
            if (dims == 0)
            {
                return;
            }
            uint32_t s = 1;
            for (size_t i = dims; i-- > 0;)
            {
                stride_[i] = s;
                s *= shape_[i];
            }
        }
    };

    /**
     * The tensor class is a wrapper around a contiguous data buffer, with additional information about how to read data from that buffer.
     */
    template <typename dtype>
    class Tensor : public TensorView<dtype>
    {
    public:
        /**
         * Constructs an empty tensor.
         */
        explicit Tensor(void) {}

        /**
         * Initialize an empty tensor of the given shape on the given heap.
         *
         * @param   shape   Shape of the tensor
         * @param   quantized   Indicates if this tensor is quantized
         * @param   shift       Quantization shift of this tensor
         * @param   heap_type   Heap location for tensor data
         */
        explicit Tensor(const Dims &&shape, const bool quantized, const uint8_t shift, const heap::Type heap_type = heap::kDefaultLocation)
            : TensorView<dtype>(std::move(shape))
        {
            data_ = reinterpret_cast<dtype *>(heap::Allocate(heap_type, TensorView<dtype>::Bytes()));
            assert(data_ != nullptr);
            TensorView<dtype>::data_immutable_ = data_;
            TensorView<dtype>::shift_ = shift;
            TensorView<dtype>::quantized_ = quantized;
        }

        /*
         * Move construct a Tensor from a TensorView.
         */
        explicit Tensor(TensorView<dtype> &&tensor_view, const heap::Type heap_type = heap::Type::PSRAM)
            : TensorView<dtype>(std::move(tensor_view))
        {
            assert(TensorView<dtype>::stream_id_ < 0);
            data_ = reinterpret_cast<dtype *>(heap::Allocate(heap_type, TensorView<dtype>::Bytes()));
            assert(data_ != nullptr);
            std::memcpy(data_, TensorView<dtype>::data_immutable_, TensorView<dtype>::Bytes());
            TensorView<dtype>::data_immutable_ = data_;
        }

        /*
         * Copy Constructor
         */
        Tensor(const Tensor<dtype> &tensor)
            : TensorView<dtype>(std::move(tensor))
        {
            data_ = reinterpret_cast<dtype *>(heap::Allocate(heap::Type::PSRAM, TensorView<dtype>::Bytes()));
            assert(data_ != nullptr);
            std::memcpy(data_, tensor.data_, TensorView<dtype>::Bytes());
            TensorView<dtype>::data_immutable_ = data_;
        }

        /*
         * Move Constructor
         */
        Tensor(Tensor<dtype> &&tensor)
            : TensorView<dtype>(std::move(tensor))
        {
            assert(data_ == nullptr);
            data_ = tensor.data_;
            TensorView<dtype>::data_immutable_ = tensor.data_immutable_;
            tensor.data_ = nullptr;
            tensor.data_immutable_ = nullptr;
        }

        /*
         * Copy assignment
         */
        Tensor<dtype> &operator=(const Tensor<dtype> &tensor)
        {
            if (this == &tensor)
                return *this;

            TensorView<dtype>::shape_ = tensor.shape_;
            TensorView<dtype>::stride_ = tensor.stride_;
            if (TensorView<dtype>::n_elements_ != tensor.n_elements_)
            {
                const auto location = heap::Locate(data_);
                heap::Free(data_);
                TensorView<dtype>::n_elements_ = tensor.n_elements_;
                data_ = reinterpret_cast<dtype *>(heap::Allocate(location, TensorView<dtype>::Bytes()));
                assert(data_ != nullptr);
                TensorView<dtype>::data_immutable_ = data_;
            }
            TensorView<dtype>::shift_ = tensor.shift_;
            TensorView<dtype>::quantized_ = tensor.quantized_;
            std::memcpy(data_, tensor.data_, TensorView<dtype>::Bytes());
            return *this;
        }

        /*
         * Move assignment
         */
        Tensor<dtype> &operator=(Tensor<dtype> &&tensor)
        {
            if (this == &tensor)
                return *this;

            if (data_ != nullptr)
            {
                heap::Free(data_);
            }

            TensorView<dtype>::shape_ = std::move(tensor.shape_);
            TensorView<dtype>::stride_ = std::move(tensor.stride_);
            TensorView<dtype>::n_elements_ = tensor.n_elements_;
            TensorView<dtype>::shift_ = tensor.shift_;
            TensorView<dtype>::quantized_ = tensor.quantized_;
            data_ = tensor.data_;
            TensorView<dtype>::data_immutable_ = data_;
            tensor.data_ = nullptr;
            tensor.data_immutable_ = nullptr;
            return *this;
        }

        /*
         * Destructor
         */
        ~Tensor(void)
        {
            if (data_ != nullptr)
            {
                heap::Free(data_);
                data_ = nullptr;
            }
        }

        /**
         * Access the underlying tensor data.
         *
         * @return  The underlying tensor data
         */
        inline constexpr dtype *Data(void) const { return data_; };

    private:
        dtype *data_{nullptr}; /*!< Pointer to tensor data */
    };

} // tlib

#endif
