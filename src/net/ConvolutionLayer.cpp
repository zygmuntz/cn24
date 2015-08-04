/*
 * This file is part of the CN24 semantic segmentation software,
 * copyright (C) 2015 Clemens-Alexander Brust (ikosa dot de at gmail dot com).
 *
 * For licensing information, see the LICENSE file included with this project.
 */
#include <cstring>
#include <algorithm>

#ifdef BUILD_OPENCL
#define BUILD_OPENCL_CONV
#endif

#ifdef BUILD_OPENCL_CONV
#include <iomanip>
#endif

#include "Config.h"
#include "Log.h"
#include "Net.h"
#include "CLHelper.h"
#include "TensorMath.h"

#include "ConvolutionLayer.h"

namespace Conv {

ConvolutionLayer::ConvolutionLayer (const unsigned int kwidth,
                                    const unsigned int kheight,
                                    const unsigned int output_maps,
                                    const unsigned int stride,
                                    const int seed, const datum dropout_fraction) :
  output_maps_ (output_maps), kernel_width_ (kwidth), kernel_height_ (kheight),
  rand_ (seed), stride_(stride), dropout_fraction_(dropout_fraction) {
  // Validate kernel dimensions. These are very important because the
  // FeedForward and BackPropagate implementations rely on some assumptions.

  // The kernels must not be of zero size in any dimension.
  if (kernel_width_ == 0 || kernel_height_ == 0) {
    FATAL ("Kernels cannot have zero dimensions");
  }
    
  if(stride == 0) {
    FATAL("Stride needs to be at least 1!");
  }
  
  // Using a zero seed on more than one layer may introduce symmetries and
  // influence the gain of a network in a negative way
  if (seed == 0) {
    LOGWARN << "Random seed is zero";
  }

  LOGDEBUG << "Instance created. " << output_maps_ << " output maps with " <<
           kernel_width_ << "x" << kernel_height_ << " kernels, stride: "<<  stride_ << ".";
  LOGDEBUG << "Dropout fraction: " << dropout_fraction_;
}

bool ConvolutionLayer::CreateOutputs (
  const std::vector< CombinedTensor* >& inputs,
  std::vector< CombinedTensor* >& outputs) {
  // This is a simple layer, only one input
  if (inputs.size() != 1) {
    LOGERROR << "Only one input supported!";
    return false;
  }

  // Save input node pointer
  CombinedTensor* input = inputs[0];

  // Check if input node pointer is null
  if (input == nullptr) {
    LOGERROR << "Null pointer input node!";
    return false;
  }

  // Validate dimensions
  // The input maps have to be larger or at least as large as the kernel,
  // because we only do 'valid' convolutions.
  // For MNIST recognition, LeCun says that 'same' convolutions perform better
  // as a first layer. He adds a border around the training and test sets to
  // achieve the same thing without changing the code.
  if (input->data.height() < kernel_height_ || input->data.width() < kernel_width_) {
    LOGERROR << "Unsupported input dimensions " << input->data;
    return false;
  }

  // Create output
  const unsigned int output_width = (input->data.width() - (kernel_width_ - 1)) / stride_;
  const unsigned int output_height = (input->data.height() - (kernel_height_ - 1)) / stride_;
  CombinedTensor* output = new CombinedTensor (input->data.samples(),
      output_width, output_height, output_maps_);

  // Tell network about the output
  outputs.push_back (output);

  return true;
}

bool ConvolutionLayer::Connect (const CombinedTensor* input,
                                CombinedTensor* output) {
  bool valid =
    input->data.width() >= kernel_width_ && input->data.height() >=  kernel_height_ &&
    output->data.width() == (input->data.width() - (kernel_width_ - 1)) / stride_ &&
    output->data.height() == (input->data.height() - (kernel_height_ - 1)) / stride_;

  if (!valid) {
    return false;
  }

  // Save parameters
  input_maps_ = input->data.maps();
  input_width_ = input->data.width();
  input_height_ = input->data.height();
  output_width_ = output->data.width();
  output_height_ = output->data.height();

  LOGDEBUG << "Local learning rate is now " << local_lr_;
  
  // Create im2col output buffer
  im2col_ff_buffer.Resize (input->data.samples(), kernel_width_ * kernel_height_, input_maps_,
                          output_width_ * output_height_);

  bp_deltax_buffer.Resize (input->data.samples(), kernel_width_ * kernel_height_, input_maps_,
                           output_width_ * output_height_);

  // This is faster than adding manually...
  ones_.Resize (1, output_width_ * output_height_);

  for (unsigned int i = 0; i < ones_.elements(); i++) {
    ones_[i] = 1;
  }

  // Create kernels
  weights_ = new CombinedTensor (output_maps_, kernel_width_, kernel_height_, input_maps_);
  bias_ = new CombinedTensor (1, output_maps_);

  // Initialize weights to zero so the net won't work if Net::InitializeWeights
  // is not called. Random memory junk may work but is certainly not optimal.
  bias_->data.Clear();
  weights_->data.Clear();
  
  // Initialize the dropout mask tensor
  dropout_mask_.Resize(input->data.samples(),output_maps_);

  // Tell the net about our parameters
  parameters_.push_back (weights_);
  parameters_.push_back (bias_);

  return true;
}

void ConvolutionLayer::FeedForward() {
  const datum p = net_->IsTesting() ? 0.0 : dropout_fraction_;
  const datum w = net_->IsTesting() ? (1.0 - dropout_fraction_) : 1.0;
  
  TensorMath::IM2COL(input_->data, input_width_, input_height_, input_maps_, input_->data.samples(),
        kernel_width_, kernel_height_, 1, 1, 0, 0, im2col_ff_buffer);
  
  output_->data.hint_ignore_content_ = true;

  for(unsigned int sample = 0; sample < input_->data.samples(); sample++) {
    // Convolve
    TensorMath::GEMM(true, false, false, output_maps_,
          output_width_ * output_height_,
          kernel_width_ * kernel_height_ * input_maps_,
          w, weights_->data, 0, kernel_width_ * kernel_height_ * input_maps_,
          im2col_ff_buffer, sample, output_width_ * output_height_,
          0.0, output_->data, sample, output_width_ * output_height_);

    // Add bias
    TensorMath::GEMM (true, false, false, output_maps_,
          output_width_ * output_height_, 1, w, bias_->data, 0, 1,
          ones_, 0, output_width_ * output_height_,
          1.0, output_->data, sample, output_width_ * output_height_);

  }

  // Very simple dropout FF implementation
  // This could be optimized a _lot_
  if(p == 0.0) {
    //dropout_mask_.Clear(1.0);
  } else {
    FATAL("Dropout is not yet TensorMath compatible");
  /*
    std::uniform_real_distribution<datum> dist(0.0, 1.0);
    for(unsigned int e = 0; e < input_->data.samples() * output_maps_; e++) {
      dropout_mask_[e] = dist(rand_) < p ? 0.0 : 1.0;
    }
  }
  
  unsigned int sk_id = 0;
  for(unsigned int s = 0; s < input_->data.samples(); s++) {
    for(unsigned int m = 0; m < output_maps_; m++) {
      datum* target_map = output_->data.data_ptr(0,0,m,s);
      for(unsigned int e = 0; e < output_width_ * output_height_; e++) {
        if(dropout_mask_[sk_id] == 0.0)
          target_map[e] = 0.0;
      }
    }
    sk_id++;*/
  }
}

void ConvolutionLayer::BackPropagate() {
  if(stride_ != 1) {
    FATAL("Strided convolutions are only supported for prediction!");
  }

  static datum one = 1.0;

  // Very simple dropout backprop implementation
  // This could be optimized a _lot_
  /*unsigned int sk_id = 0;
  for(unsigned int s = 0; s < input_->data.samples(); s++) {
    for(unsigned int m = 0; m < output_maps_; m++) {
      datum* target_map = output_->delta.data_ptr(0,0,m,s);
      if(dropout_mask_[sk_id] == 0.0)
        for(unsigned int e = 0; e < output_width_ * output_height_; e++) {
          target_map[e] = 0.0;
        }
    }
    sk_id++;
  }*/

  TensorMath::SETSAMPLE(weights_->delta, -1, 0.0);
  TensorMath::SETSAMPLE(bias_->delta, -1, 0.0);
  
  bp_deltax_buffer.hint_ignore_content_ = true;
  
  for(unsigned int sample = 0; sample < input_->data.samples(); sample++) {
    /*
    * 1. Backpropagation
    */
    if (backprop_enabled_)
      TensorMath::GEMM (true, true, false,
            kernel_width_ * kernel_height_ * input_maps_,
            output_width_ * output_height_,
            output_maps_,
            1.0, weights_->data, 0, kernel_width_ * kernel_height_ * input_maps_,
            output_->delta, sample, output_width_ * output_height_,
            0.0, bp_deltax_buffer, sample, output_width_ * output_height_);

    /*
    * 2. Weight gradient calculation
    */
    TensorMath::GEMM (true, false, true, output_maps_,
          kernel_width_ * kernel_height_ * input_maps_,
          output_width_ * output_height_,
          1.0, output_->delta, sample, output_width_ * output_height_,
          im2col_ff_buffer, sample, output_width_ * output_height_,
          1.0, weights_->delta, 0, kernel_width_ * kernel_height_ * input_maps_);
    
    /*
    * 3. Bias gradient calculation
    */
    TensorMath::GEMV(true, false, output_maps_, output_width_ * output_height_, 1.0,
          output_->delta, sample, output_width_ * output_height_,
          ones_, 0, 1, 1.0, bias_->delta, 0, 1);
  }
  
  if(backprop_enabled_)
    TensorMath::COL2IM(input_->delta, input_width_, input_height_, input_maps_, input_->data.samples(),
        kernel_width_, kernel_height_, 1, 1, 0, 0, bp_deltax_buffer);
}


void ConvolutionLayer::OnLayerConnect (const std::vector<Layer*> next_layers) {
	unsigned int next_layer_gain = 0;
	for (Layer* next_layer: next_layers)
		next_layer_gain += next_layer->Gain();

  unsigned int this_layer_gain = Gain();

  const datum range = sqrt (6) / sqrt (next_layer_gain + this_layer_gain);

#ifdef BUILD_OPENCL
  weights_->data.MoveToCPU();
#endif
  std::uniform_real_distribution<datum> dist_weights (-range , range);

  for (std::size_t i = 0; i < weights_->data.elements(); i++) {
    weights_->data[i] = dist_weights (rand_);
  }

  LOGDEBUG << "Updating weights: " << this_layer_gain << " -> "
           << next_layer_gain;
}

bool ConvolutionLayer::IsOpenCLAware() {
#ifdef BUILD_OPENCL_CONV
  return true;
#else
  return false;
#endif
}

}

