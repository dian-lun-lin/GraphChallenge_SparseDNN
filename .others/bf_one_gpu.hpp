#pragma once
#include <Eigen/Dense>
#include <SNIG/utility/reader.hpp>
#include <SNIG/utility/matrix_format.h>
#include <SNIG/utility/cuda_error.hpp>
#include <SNIG/utility/scoring.hpp>
#include <SNIG/bf/kernel.hpp>
#include <chrono>

namespace std {
  namespace fs = experimental::filesystem;  
}

namespace snig{

template <typename T>  
class BFOneGpu {

  static_assert(
    std::is_same<T, float>::value || std::is_same<T, double>::value,
    "data type must be either float or double"
  );
  
  private:
    int* _h_pinned_weight;
    T _bias;
    size_t _num_neurons_per_layer;
    size_t _num_layers;

    size_t _max_nnz_per_layer;
    size_t _COL_BLK;
    size_t _pad {0};
    size_t _N_SLAB;

    size_t _p_w_index_len;
    size_t _pp_w_index_len;
    size_t _pp_wlen;
    size_t _pp_wsize;

    void _infer_BF(
      std::vector<int*>& d_W,
      std::vector<int*>& rowsY,
      std::vector<int*>& rlenY,
      std::vector<T*>& Y,
      size_t nerowsY,
      const size_t num_inputs,
      int* results
    ) const;

    void _non_empty_rows(
      const size_t num_inputs,
      int* rlenY,
      int* rowsY,
      size_t& nnz
    ) const;

  public:

    BFOneGpu(
      const std::fs::path& weight_path,
      const T bias = -.3f,
      const size_t num_neurons_per_layer = 1024,
      const size_t num_layers = 120
    );

    ~BFOneGpu();
    
    size_t num_neurons_per_layer() const;

    size_t num_layers() const;

    Eigen::Matrix<int, Eigen::Dynamic, 1> infer(
      const std::fs::path& input_path,
      const size_t num_inputs
    ) const;

};

// ----------------------------------------------------------------------------
// Definition of BFOneGpu
// ----------------------------------------------------------------------------

template <typename T>
BFOneGpu<T>::BFOneGpu(
  const std::fs::path& weight_path,
  const T bias,
  const size_t num_neurons_per_layer,
  const size_t num_layers
):
  _bias{bias},
  _num_neurons_per_layer{num_neurons_per_layer},
  _num_layers{num_layers},
  _pad{0}
{
  std::cout << "Constructing a GPU parallel network.\n";

  //get tuned shared memory size
  //num_neurons_per_layer must be divisible by shared memory (a.k.a. COL_BLK)
  //only for single GPU
  //only for double float
  cudaDeviceProp props;
  cudaGetDeviceProperties(&props, 0);
  size_t max_num_per_block = props.sharedMemPerBlock / sizeof(T);
  if(num_neurons_per_layer <= max_num_per_block) {
    _COL_BLK = num_neurons_per_layer;
  }
  else{
    int max_divisor = 2;
    while((num_neurons_per_layer % max_divisor != 0) || (max_num_per_block < (num_neurons_per_layer / max_divisor))) {
      ++max_divisor;
    }
    _COL_BLK = num_neurons_per_layer / max_divisor;
  }


  std::cout << "Loading the weight........................." << std::flush;
  auto reading_beg = std::chrono::steady_clock::now();

  _N_SLAB = num_neurons_per_layer / _COL_BLK; 

  _max_nnz_per_layer = find_max_nnz_binary(
                         weight_path,
                         num_layers,
                         num_neurons_per_layer
                       );

  // total length of row and col index
  // value index should consider sizeof(T)
  _p_w_index_len  = num_neurons_per_layer * _N_SLAB + _max_nnz_per_layer + 1;

  //handle aligned (only deal with double and float)
  if(_p_w_index_len % sizeof(T) != 0){
    ++_pad;
  }

  _pp_w_index_len = _p_w_index_len + _pad;

  //pad packed weight length
  _pp_wlen = _pp_w_index_len + (sizeof(T) / sizeof(float)) * _max_nnz_per_layer;
  //pad packed weight size
  _pp_wsize = sizeof(int) * (_pp_w_index_len) + sizeof(T) * _max_nnz_per_layer;

  checkCuda(cudaMallocHost(
    (void**)&_h_pinned_weight,
    _pp_wsize * num_layers
  ));

  std::memset(
    _h_pinned_weight,
    0,
    _pp_wsize * num_layers
  );

  read_weight_binary<T>(
    weight_path,
    num_neurons_per_layer,
    _max_nnz_per_layer,
    num_layers,
    _N_SLAB,
    _pad,
    _h_pinned_weight
  );

  auto reading_end = std::chrono::steady_clock::now();
  std::cout << "finished reading DNN layers with " 
            << std::chrono::duration_cast<std::chrono::milliseconds>(reading_end - reading_beg).count()
            << "ms"
            << '\n';
}

template <typename T>
BFOneGpu<T>:: ~BFOneGpu() {
  checkCuda(cudaFreeHost(_h_pinned_weight));
}

template <typename T>
size_t BFOneGpu<T>::num_neurons_per_layer() const {
 return _num_neurons_per_layer; 
}

template <typename T>
size_t BFOneGpu<T>::num_layers() const { 
  return _num_layers; 
}

template <typename T>
Eigen::Matrix<int, Eigen::Dynamic, 1> BFOneGpu<T>::infer(
const std::fs::path& input_path,
  const size_t num_inputs
) const {

  std::cout << "Preprocessing.............................." << std::flush;
  auto pp_beg = std::chrono::steady_clock::now();
  
  
  // d_W[0]: present layer 
  // d_W[1]: next layer
  std::vector<int*> d_W(2, nullptr);
  checkCuda(cudaMalloc(
    &d_W[0],
    _pp_wsize
  ));
  checkCuda(cudaMalloc(
    &d_W[1],
    _pp_wsize
  ));
  checkCuda(cudaMemcpy(
    d_W[0],
    _h_pinned_weight,
    _pp_wsize,
    cudaMemcpyHostToDevice
  ));

  std::vector<T*> Y(2, nullptr);  
  std::vector<int*> rowsY(2, nullptr);
  std::vector<int*> rlenY(2, nullptr);

  checkCuda(cudaMallocManaged(&Y[0], sizeof(T) * num_inputs * _num_neurons_per_layer));
  checkCuda(cudaMallocManaged(&Y[1], sizeof(T) * num_inputs * _num_neurons_per_layer));
  checkCuda(cudaMallocManaged(&rowsY[0], sizeof(int) * num_inputs));
  checkCuda(cudaMallocManaged(&rowsY[1], sizeof(int) * num_inputs));
  checkCuda(cudaMallocManaged(&rlenY[0], sizeof(int) * num_inputs));
  checkCuda(cudaMallocManaged(&rlenY[1], sizeof(int) * num_inputs));
  checkCuda(cudaMemset(rowsY[0], 0, sizeof(int) * num_inputs));
  checkCuda(cudaMemset(rowsY[1], 0, sizeof(int) * num_inputs));
  checkCuda(cudaMemset(rlenY[0], 0, sizeof(int) * num_inputs));
  checkCuda(cudaMemset(rlenY[1], 0, sizeof(int) * num_inputs));

  size_t nerowsY{0};
  read_input_binary<T>(input_path, Y[0], rlenY[0], rowsY[0], nerowsY);

  //final results allocation
  int* results;
  checkCuda(cudaMallocManaged(&results, sizeof(int) * num_inputs));
  checkCuda(cudaMemset(results, 0, sizeof(int) * num_inputs));

  auto pp_end = std::chrono::steady_clock::now();
  
  std::cout << "finished preprocessing with " 
            << std::chrono::duration_cast<std::chrono::milliseconds>(pp_end - pp_beg).count()
            << "ms"
            << std::endl;

  std::cout << "Start inferencing and Identifying categories......................." << std::flush;
  auto exec_beg = std::chrono::steady_clock::now();

  _infer_BF(d_W, rowsY, rlenY, Y, nerowsY, num_inputs, results);

  auto exec_end = std::chrono::steady_clock::now();
  std::cout << "finished execution and identification with "
            << std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_beg).count()
            << "ms"
            << std::endl;

  auto results_eigen =  arr_to_Eigen_int(results, num_inputs);

  checkCuda(cudaFree(Y[0]));
  checkCuda(cudaFree(Y[1]));
  checkCuda(cudaFree(rowsY[0]));
  checkCuda(cudaFree(rowsY[1]));
  checkCuda(cudaFree(rlenY[0]));
  checkCuda(cudaFree(rlenY[1]));
  checkCuda(cudaFree(d_W[0]));
  checkCuda(cudaFree(d_W[1]));
  checkCuda(cudaFree(results));

  return results_eigen;
}

template <typename T>
void BFOneGpu<T>::_infer_BF(
  std::vector<int*>& d_W,
  std::vector<int*>& rowsY,
  std::vector<int*>& rlenY,
  std::vector<T*>& Y,
  size_t nerowsY,
  const size_t num_inputs,
  int* results
) const {
  dim3 threads(2, 512, 1);

  cudaStream_t stream[2];
  checkCuda(cudaStreamCreate(&stream[0]));
  checkCuda(cudaStreamCreate(&stream[1]));

  for(size_t cur_layer = 0; cur_layer < _num_layers; ++cur_layer) {
    if(cur_layer != _num_layers - 1) {
      checkCuda(cudaMemcpyAsync(
        d_W[(cur_layer + 1) % 2],
        _h_pinned_weight + (cur_layer + 1) * (_pp_wlen),
        _pp_wsize,
        cudaMemcpyHostToDevice,
        stream[0]
      ));
    }

    bf_inference<T><<<nerowsY, threads, sizeof(T) * _COL_BLK, stream[1]>>>(
      Y[cur_layer % 2],
      nerowsY,
      rowsY[cur_layer % 2],
      rlenY[cur_layer % 2],
      _COL_BLK,
      _N_SLAB,
      _num_neurons_per_layer,
      d_W[cur_layer % 2],
      d_W[cur_layer % 2] + _num_neurons_per_layer * _N_SLAB + 1,
      (T*)(d_W[cur_layer % 2] + _p_w_index_len),
      _bias,
      Y[(cur_layer + 1) % 2],
      rlenY[(cur_layer + 1) % 2]
    );

    checkCuda(cudaStreamSynchronize(stream[1]));

    _non_empty_rows(
      num_inputs,
      rlenY[(cur_layer + 1) % 2],
      rowsY[(cur_layer + 1) % 2],
      nerowsY
    );

    checkCuda(cudaMemset(
      Y[cur_layer % 2],
      0,
      sizeof(T) * num_inputs * _num_neurons_per_layer
    ));
    checkCuda(cudaStreamSynchronize(stream[0]));
  }

  identify<T><<<16, 512>>>(Y[0], num_inputs, _num_neurons_per_layer, results);
  checkCuda(cudaDeviceSynchronize());

  checkCuda(cudaStreamDestroy(stream[0]));
  checkCuda(cudaStreamDestroy(stream[1]));
}

template <typename T>
void BFOneGpu<T>::_non_empty_rows(
  const size_t num_inputs,
  int* rlenY,
  int* rowsY,
  size_t& nnz
) const {
  nnz = 0;

  for(size_t i = 0; i < num_inputs; ++i) {
    if(rlenY[i] > 0) {
      rowsY[nnz++] = i;
    }
  }
}


}// end of namespace snig ----------------------------------------------
