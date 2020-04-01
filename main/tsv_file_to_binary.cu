#pragma once
#include <CLI11/CLI11.hpp>
#include <SparseDNN/utility/reader.hpp>


int main(int argc, char* argv[]) {

  // usage: ./tsv_file_to_binary
  //          --neurons(-n) 1024, 4096, or 16384
  //          --layers(-l)  120, 480, or 1920
  //          --weight_path(-w) path of weight
  //          --input_path(-i)  path of input
  //          --golden_path(-g) path of golden
  //          --golden_all  Convert all golden files if true

  // example1:
  //        ./tsv_file_to_binary 
  // example2:
  //        ./tsv_file_to_binary -n 1024 -l 1920 -w ../sample_data/weight/neuron1024/ -i ../sample_data/MNIST/ -g ../sample_data/MNIST/ --golden_all true

  // COL_BLK, N_SLAB would be caculated automatically, based on GPU architecture.

  CLI::App app{"Converter"};

  size_t num_neurons_per_layer = 1024;
  app.add_option("-n, --neurons", 
    num_neurons_per_layer, 
    "select number of neurons, default is 1024");

  size_t num_layers = 120;
  app.add_option("-l, --layers", 
    num_layers, 
    "select number of layers, default is 120");

  bool golden_all = true;
  app.add_option("--golden_all", 
    golden_all, 
    "this would convert all golden files with the same neurons. Otherwise only specific num_layers and num_neurons would be converted. Default is true");

  std::fs::path weight_path("../sample_data/weight/neuron1024/");
  app.add_option("-w, --weight_path", 
    weight_path, 
    "select directory of weights. Output binary files would also be generated here. Default is ../sample_data/weight/neuron1024/");

  std::fs::path input_path("../sample_data/MNIST/");
  app.add_option("-i, --input_path", 
    input_path, 
    "select input path. Output binary files would also be generated here. Default is ../sample_data/MNIST/");

  std::fs::path golden_path("../sample_data/MNIST/");
  app.add_option("-g, --golden_path", 
    golden_path, 
    "select golden path. Output binary files would also be generated here. Default is ../sample_data/MNIST/");

  CLI11_PARSE(app, argc, argv);

  size_t COL_BLK;
  size_t N_SLAB;

  cudaDeviceProp props;
  cudaGetDeviceProperties(&props, 0);
  size_t max_num_per_block = props.sharedMemPerBlock / sizeof(float);

  if(num_neurons_per_layer <= max_num_per_block) {
    COL_BLK = num_neurons_per_layer;
  }
  else{
    int max_divisor = 2;
    while((num_neurons_per_layer % max_divisor != 0) || (max_num_per_block < (num_neurons_per_layer / max_divisor))) {
      ++max_divisor;
    }
    COL_BLK = num_neurons_per_layer / max_divisor;
  }

  N_SLAB = num_neurons_per_layer / COL_BLK; 

  std::cout << "Transforming weight files...\n";

  sparse_dnn::tsv_file_to_binary_file<float>(
    weight_path,
    num_layers,
    num_neurons_per_layer,
    num_neurons_per_layer,
    COL_BLK,
    N_SLAB,
    100000
  ); 

  std::cout << "Transforming input files...\n";

  sparse_dnn::tsv_file_to_binary_file<float>(
    input_path,
    60000,
    num_neurons_per_layer
  );

  std::cout << "Transforming golden files...\n";

  if(!golden_all){
    sparse_dnn::tsv_file_to_binary_file<float>(
      golden_path,
      num_neurons_per_layer,
      num_layers,
      60000
    );
  }
  else{
    for(int i = 120; i <= 1920; i *= 4){
      sparse_dnn::tsv_file_to_binary_file<float>(
        golden_path,
        num_neurons_per_layer,
        i,
        60000
      );
    }
  }
}