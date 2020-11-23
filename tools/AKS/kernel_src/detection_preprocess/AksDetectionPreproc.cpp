/*
 * Copyright 2019 Xilinx Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <iostream>
#include <vector>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <aks/AksKernelBase.h>
#include <aks/AksDataDescriptor.h>
#include <aks/AksNodeParams.h>

class DetectionImreadPre : public AKS::KernelBase
{
  public:
    int exec_async (
        std::vector<AKS::DataDescriptor*> &in,
        std::vector<AKS::DataDescriptor*> &out,
        AKS::NodeParams* nodeParams,
        AKS::DynamicParamValues* dynParams);
};

extern "C" { /// Add this to make this available for python bindings

  AKS::KernelBase* getKernel (AKS::NodeParams *params)
  {
    return new DetectionImreadPre();
  }

}//externC

void embedImage(cv::Mat& source, cv::Mat& dest, int dx, int dy)
{
  float * srcData = (float*)source.data;
  float * dstData = (float*)dest.data;
  unsigned int dstStart;
  /// Fill image data
  unsigned int size = source.rows * source.cols * source.channels();
  if (dx == 0) {
    /// Horizontal letterbox (source.cols == dst.cols)
    unsigned int dstStart = source.cols * dy * source.channels();
    for (unsigned int i = 0; i < size; ++i) {
      dstData[dstStart+i] = srcData[i];
    }
  } else {
    /// Vertial letterbox (source.rows == dst.rows)
    int moveBytes = source.cols * source.channels() * sizeof(float);
    for (int rows = 0; rows < source.rows; ++rows) {
      unsigned long srcOffset = rows * source.cols * source.channels();
      unsigned long dstOffset = rows * dest.cols * dest.channels() + (dx * dest.channels());
      memcpy(dstData + dstOffset, srcData+srcOffset, moveBytes);
    }
  }
}

void letterBoxImage(cv::Mat &inImage, int resizeH, int resizeW, cv::Mat& outImage)
{
  int new_w = inImage.cols;
  int new_h = inImage.rows;

  /// Find max dim
  if (((float)resizeW / inImage.cols) < ((float)resizeH / inImage.rows)) {
    new_w = resizeW;
    new_h = (inImage.rows * resizeW) / inImage.cols;
  } else {
    new_h = resizeH;
    new_w = (inImage.cols * resizeH) / inImage.rows;
  }
  /// Resize image (keeping aspect ratio)
  cv::Mat resizedImage = cv::Mat(new_h, new_w, CV_8UC3);
  cv::resize(inImage, resizedImage, cv::Size(new_w, new_h));

  /// Convert image from BGR to RGB
  cv::cvtColor(resizedImage, resizedImage, cv::COLOR_BGR2RGB);

  /// Normalize image
  cv::Mat scaledImage = cv::Mat(new_h, new_w, CV_32FC3);
  resizedImage.convertTo(scaledImage, CV_32FC3, 1/255.0);

  /// Fill output image with 0.5 (for letterbox)
  outImage.setTo(cv::Scalar(0.5f, 0.5f, 0.5f));
  embedImage(scaledImage, outImage, (resizeW-new_w)/2, (resizeH-new_h)/2);

#if DUMP_DATA
  float * tmp = (float*)outImage.data;
  FILE * fp1 = fopen ("letterbox-out.txt", "w");
  for (int h = 0; h < outImage.rows * outImage.cols * outImage.channels(); h++)
    fprintf (fp1, "%f\n", tmp[h]);
  fclose(fp1);
#endif
}


int DetectionImreadPre::exec_async (
    std::vector<AKS::DataDescriptor*> &in,
    std::vector<AKS::DataDescriptor*> &out,
    AKS::NodeParams* nodeParams,
    AKS::DynamicParamValues* dynParams)
{
  /// Get input and output data shapes
  /// Input could be batch array or batch of images
  const std::vector<int>& inShape = in[0]->getShape();
  int batchSize = inShape[0];

  /// Get output Dimensions (Network input dim)
  int outHeight    = nodeParams->_intParams["net_h"];
  int outWidth     = nodeParams->_intParams["net_w"];
  int outChannels  = 3;
  int nOutElemsPerImg = outChannels * outHeight * outWidth;

  /// Create output data buffer
  std::vector<int> shape      = { batchSize, outChannels, outHeight, outWidth };
  AKS::DataDescriptor * outDD = new AKS::DataDescriptor(shape, AKS::DataType::FLOAT32);
  float * outData = (float*) outDD->data();

  /// Get input dims incase batch array
  std::vector<int> imgDims; imgDims.reserve(3*batchSize);
  int inChannel = 3;
  int inHeight  = 0;
  int inWidth   = 0;
  const uint8_t* inData = nullptr;
  for(ssize_t b=0; b<batchSize; ++b) {
    if(in[0]->dtype() == AKS::DataType::AKSDD) {
      const auto& dd = in[0]->data<AKS::DataDescriptor>()[b];
      inData   = dd.const_data<uint8_t>();
      // Shape = (Batch=1, H, W, C=3)
      inHeight = dd.getShape()[1];
      inWidth  = dd.getShape()[2];
    }
    else {
      inHeight = inShape[1];
      inWidth  = inShape[2];
      int nInElemsPerImg  = inChannel * inHeight * inWidth;
      inData   = in[0]->const_data<uint8_t>() + nInElemsPerImg;
    }

    /// Create a cv::Mat with input data
    /// TODO : cv::Mat doesn't have a constructor that accepts const_pointer
    /// So using \const_cast\ to avoid copy.
    cv::Mat inImage(inHeight, inWidth, CV_8UC3, (void*)const_cast<uint8_t*>(inData));

    /// Resize the image to Network Shape (LetterBox)
    cv::Mat outImage = cv::Mat(outHeight, outWidth, CV_32FC3);
    /// Crop Letter-Box
    letterBoxImage(inImage, outHeight, outWidth, outImage);

    /// Transpose: HWC-->CHW
    float* out = outData + b * nOutElemsPerImg;
    for (int c = 0; c < 3; c++) {
      for (int h = 0; h < outHeight; h++) {
        for (int w = 0; w < outWidth; w++) {
          out[ (c*outHeight*outWidth)
            + (h*outWidth) + w]
            = outImage.at<cv::Vec3f>(h,w)[c];
        }
      }
    }

#if DUMP_DATA
    FILE * fp = fopen ("preprocess-out.txt", "w");
    for (int h = 0; h < outImage.rows * outImage.cols * outImage.channels(); h++)
      fprintf (fp, "%f\n", outData[h]);
    fclose(fp);
#endif
    imgDims.insert(imgDims.end(), {inChannel, inHeight, inWidth});
  }

  /// Write back image shape to dynParams for PostProc
  dynParams->_intVectorParams["img_dims"] = std::move(imgDims);

  /// Push back output
  out.push_back(outDD);
  return 0;
}
