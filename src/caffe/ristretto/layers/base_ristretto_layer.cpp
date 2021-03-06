#include <math.h>
#include <algorithm>
#include <stdlib.h>
#include <time.h>
//#include <direct.h>

#include "ristretto/base_ristretto_layer.hpp"


namespace caffe {

template <typename Dtype>
BaseRistrettoLayer<Dtype>::BaseRistrettoLayer() {
  // Initialize random number generator
  srand(time(NULL));
}

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::QuantizeWeights_cpu(
      vector<shared_ptr<Blob<Dtype> > > weights_quantized, const int rounding,
      const bool bias_term) {
  Dtype* weight = weights_quantized[0]->mutable_cpu_data();
  const int cnt_weight = weights_quantized[0]->count();


  switch (precision_) {
  case QuantizationParameter_Precision_MINIFLOAT:
    Trim2MiniFloat_cpu(weight, cnt_weight, fp_mant_, fp_exp_, rounding);
    if (bias_term) {
      Trim2MiniFloat_cpu(weights_quantized[1]->mutable_cpu_data(),
          weights_quantized[1]->count(), fp_mant_, fp_exp_, rounding);
    }
    break;
  case QuantizationParameter_Precision_DYNAMIC_FIXED_POINT:
  	if(int8_term_)
  	{
  		const int cnt = cnt_weight/weight_scale_.size();
  		for(int i = 0; i < weight_scale_.size(); i++){
  			Trim2int8_data_cpu((weight + i * cnt), cnt , weight_scale_[i]);
  		}
  	}
  	else{
		Trim2FixedPoint_cpu(weight, cnt_weight, bw_params_, rounding, fl_params_);
		if (bias_term) {
//		  Trim2FixedPoint_cpu(weights_quantized[1]->mutable_cpu_data(),
//			  weights_quantized[1]->count(), bw_bias_params_, rounding, fl_params_+fl_layer_in_);
			Trim2FixedPoint_cpu(weights_quantized[1]->mutable_cpu_data(),
					weights_quantized[1]->count(), bw_bias_params_, rounding, fl_bias_params_);

			Dtype* bias = weights_quantized[1]->mutable_cpu_data();

			for(int i = 0; i < weights_quantized[1]->count(); i++)
			{
//				bias[i] /= pow(2, -fl_params_);
//				bias[i] = round(bias[i]);

				bias[i] *= pow(2, (fl_params_+fl_layer_in_-fl_bias_params_));
			}

		}
  	}
    break;
  case QuantizationParameter_Precision_INTEGER_POWER_OF_2_WEIGHTS:
    Trim2IntegerPowerOf2_cpu(weight, cnt_weight, pow_2_min_exp_, pow_2_max_exp_,
        rounding);
    // Don't trim bias
    break;
  default:
    LOG(FATAL) << "Unknown trimming mode: " << precision_;
    break;
  }
}

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::QuantizeLayerInputs_cpu(Dtype* data,
      const int count) {
  switch (precision_) {
    case QuantizationParameter_Precision_INTEGER_POWER_OF_2_WEIGHTS:
      break;
    case QuantizationParameter_Precision_DYNAMIC_FIXED_POINT:
    	if(int8_term_)
    	{
    		Trim2int8_data_cpu(data, count, data_scale_);
    	}
    	else{
    		Trim2FixedPoint_cpu(data, count, bw_layer_in_, rounding_, fl_layer_in_);
    	}
      break;
    case QuantizationParameter_Precision_MINIFLOAT:
      Trim2MiniFloat_cpu(data, count, fp_mant_, fp_exp_, rounding_);
      break;
    default:
      LOG(FATAL) << "Unknown trimming mode: " << precision_;
      break;
  }
}

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::QuantizeLayerOutputs_cpu(
      Dtype* data, const int count) {
  switch (precision_) {
    case QuantizationParameter_Precision_INTEGER_POWER_OF_2_WEIGHTS:
      break;
    case QuantizationParameter_Precision_DYNAMIC_FIXED_POINT:
    	if(int8_term_)
    	{
    		for(int i = 0; i < weight_scale_.size(); i++ ){
    		Trim2int8_output_cpu(data, count, weight_scale_[i], data_scale_);
    		}
    	}
    	else
    	{
    		reQuantize_cpu(data, count, fl_params_+fl_layer_in_);
    	}
      break;
    case QuantizationParameter_Precision_MINIFLOAT:
      Trim2MiniFloat_cpu(data, count, fp_mant_, fp_exp_, rounding_);
      break;
    default:
      LOG(FATAL) << "Unknown trimming mode: " << precision_;
      break;
  }
}

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::Trim2FixedPoint_cpu(Dtype* data, const int cnt,
      const int bit_width, const int rounding, int fl) {
  for (int index = 0; index < cnt; ++index)
  {
		// Saturate data

		Dtype max_data = (pow(2, bit_width - 1) - 1) * pow(2, -fl);
		Dtype min_data = -pow(2, bit_width - 1) * pow(2, -fl);
		data[index] = std::max(std::min(data[index], max_data), min_data);
		// Round data

		data[index] /= pow(2, -fl);
		switch (rounding) {
		case QuantizationParameter_Rounding_NEAREST:
		  data[index] = round(data[index]);
		  break;
		case QuantizationParameter_Rounding_STOCHASTIC:
		  data[index] = floor(data[index] + RandUniform_cpu());
		  break;
		default:
		  break;
		}
		//data[index] *= pow(2, -fl);
	}
}

typedef union {
  float d;
  struct {
    unsigned int mantisa : 23;
    unsigned int exponent : 8;
    unsigned int sign : 1;
  } parts;
} float_cast;

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::Trim2MiniFloat_cpu(Dtype* data, const int cnt,
      const int bw_mant, const int bw_exp, const int rounding) {
  for (int index = 0; index < cnt; ++index) {
    int bias_out = pow(2, bw_exp - 1) - 1;
    float_cast d2;
    // This casts the input to single precision
    d2.d = (float)data[index];
    int exponent=d2.parts.exponent - 127 + bias_out;
    double mantisa = d2.parts.mantisa;
    // Special case: input is zero or denormalized number
    if (d2.parts.exponent == 0) {
      data[index] = 0;
      return;
    }
    // Special case: denormalized number as output
    if (exponent < 0) {
      data[index] = 0;
      return;
    }
    // Saturation: input float is larger than maximum output float
    int max_exp = pow(2, bw_exp) - 1;
    int max_mant = pow(2, bw_mant) - 1;
    if (exponent > max_exp) {
      exponent = max_exp;
      mantisa = max_mant;
    } else {
      // Convert mantissa from long format to short one. Cut off LSBs.
      double tmp = mantisa / pow(2, 23 - bw_mant);
      switch (rounding) {
      case QuantizationParameter_Rounding_NEAREST:
        mantisa = round(tmp);
        break;
      case QuantizationParameter_Rounding_STOCHASTIC:
        mantisa = floor(tmp + RandUniform_cpu());
        break;
      default:
        break;
      }
    }
    // Assemble result
    data[index] = pow(-1, d2.parts.sign) * ((mantisa + pow(2, bw_mant)) /
        pow(2, bw_mant)) * pow(2, exponent - bias_out);
	}
}

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::Trim2IntegerPowerOf2_cpu(Dtype* data,
      const int cnt, const int min_exp, const int max_exp, const int rounding) {
	for (int index = 0; index < cnt; ++index) {
    float exponent = log2f((float)fabs(data[index]));
    int sign = data[index] >= 0 ? 1 : -1;
    switch (rounding) {
    case QuantizationParameter_Rounding_NEAREST:
      exponent = round(exponent);
      break;
    case QuantizationParameter_Rounding_STOCHASTIC:
      exponent = floorf(exponent + RandUniform_cpu());
      break;
    default:
      break;
    }
    exponent = std::max(std::min(exponent, (float)max_exp), (float)min_exp);
    data[index] = sign * pow(2, exponent);
	}
}

template <typename Dtype>
double BaseRistrettoLayer<Dtype>::RandUniform_cpu(){
  return rand() / (RAND_MAX+1.0);
}

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::op_data(const Dtype* data,const int cnt,char* name)
{
	char x[100];

	sprintf(x,"%s%s","/home/sun/caffe_mobile/qdata/",name);
	std::ofstream f(x,ios::binary);
	if(!f){
		LOG(INFO) <<"failed to create the file"<< "\n";
	}
	else
	{
		f.write((char*)data,cnt*sizeof(Dtype));
		f.close();
	}
}


template <typename Dtype>
void BaseRistrettoLayer<Dtype>::Trim2int8_data_cpu(Dtype* data, const int cnt,
		float scale_){
	for(int index = 0; index < cnt; ++index)
	{

		Dtype max_data = 127;
		Dtype min_data = -128;
		data[index] = round(data[index]*scale_);
		data[index] = std::max(std::min(data[index], max_data), min_data);

		if(scale_ != 0)
			data[index] = data[index]/scale_;
	}
}

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::Trim2int8_output_cpu(Dtype* data, const int cnt,
		float weight_scale_, float data_scale_){
	for(int index = 0; index < cnt; ++index)
	{
		//data[index] = data[index]/(2*2);
		data[index] = data[index]/(weight_scale_* data_scale_);
	}
}

template <typename Dtype>
void BaseRistrettoLayer<Dtype>::reQuantize_cpu(Dtype* data, const int count,const int fl)
{
	for(int index = 0; index < count; ++index)
	{

		data[index] /= pow(2, fl);
	}
}


template BaseRistrettoLayer<double>::BaseRistrettoLayer();
template BaseRistrettoLayer<float>::BaseRistrettoLayer();
template void BaseRistrettoLayer<double>::QuantizeWeights_cpu(
    vector<shared_ptr<Blob<double> > > weights_quantized, const int rounding,
    const bool bias_term);
template void BaseRistrettoLayer<float>::QuantizeWeights_cpu(
    vector<shared_ptr<Blob<float> > > weights_quantized, const int rounding,
    const bool bias_term);
template void BaseRistrettoLayer<double>::QuantizeLayerInputs_cpu(double* data,
    const int count);
template void BaseRistrettoLayer<float>::QuantizeLayerInputs_cpu(float* data,
    const int count);
template void BaseRistrettoLayer<double>::QuantizeLayerOutputs_cpu(double* data,
    const int count);
template void BaseRistrettoLayer<float>::QuantizeLayerOutputs_cpu(float* data,
    const int count);
template void BaseRistrettoLayer<double>::Trim2FixedPoint_cpu(double* data,
    const int cnt, const int bit_width, const int rounding, int fl);
template void BaseRistrettoLayer<float>::Trim2FixedPoint_cpu(float* data,
    const int cnt, const int bit_width, const int rounding, int fl);
template void BaseRistrettoLayer<double>::Trim2MiniFloat_cpu(double* data,
    const int cnt, const int bw_mant, const int bw_exp, const int rounding);
template void BaseRistrettoLayer<float>::Trim2MiniFloat_cpu(float* data,
    const int cnt, const int bw_mant, const int bw_exp, const int rounding);
template void BaseRistrettoLayer<double>::Trim2IntegerPowerOf2_cpu(double* data,
    const int cnt, const int min_exp, const int max_exp, const int rounding);
template void BaseRistrettoLayer<float>::Trim2IntegerPowerOf2_cpu(float* data,
    const int cnt, const int min_exp, const int max_exp, const int rounding);
template double BaseRistrettoLayer<double>::RandUniform_cpu();
template double BaseRistrettoLayer<float>::RandUniform_cpu();

template void BaseRistrettoLayer<float>::Trim2int8_output_cpu(float* data, const int cnt,
		float weight_scale_, float data_scale_);
template void BaseRistrettoLayer<double>::Trim2int8_output_cpu(double* data, const int cnt,
		float weight_scale_, float data_scale_);
template void BaseRistrettoLayer<float>::Trim2int8_data_cpu(float* data, const int cnt,
		float scale_);
template void BaseRistrettoLayer<double>::Trim2int8_data_cpu(double* data, const int cnt,
		float scale_);

template void BaseRistrettoLayer<float>::reQuantize_cpu(float* data,
		const int count,int fl);
template void BaseRistrettoLayer<double>::reQuantize_cpu(double* data,
		const int count,int fl);
}  // namespace caffe
