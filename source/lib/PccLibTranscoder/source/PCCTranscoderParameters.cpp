/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2017, ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "PCCCommon.h"
#include "PCCTranscoderParameters.h"
#include "PCCVirtualVideoEncoder.h"

using namespace pcc;

PCCTranscoderParameters::PCCTranscoderParameters() {
  compressedStreamPath_              = {};
  outStreamPath_                     = {};
  startFrameNumber_                  = 0;
  nbThread_                          = 1;
  keepIntermediateFiles_             = false;
  forcedSsvhUnitSizePrecisionBytes_  = 0;

  shvcLayerIndex_        = 8;
  useCuda_               = false;

  transcodeBaseline_     = false;
  byteStreamVideoCoderAttribute_ = true;
  byteStreamVideoCoderGeometry_ = true;
  byteStreamVideoCoderOccupancy_ = true;
  configurationFolder_                 = {};
  geometryConfig_                          = {};
  attributeConfig_                         = {};
  videoEncoderGeometryCodecId_             = PCCVirtualVideoEncoder<uint8_t>::getDefaultCodecId();
  videoEncoderAttributeCodecId_            = PCCVirtualVideoEncoder<uint8_t>::getDefaultCodecId();
  videoEncoderOccupancyCodecId_            = PCCVirtualVideoEncoder<uint8_t>::getDefaultCodecId();
  videoEncoderGeometryPath_                = {};
  videoEncoderAttributePath_               = {};
  videoEncoderOccupancyPath_               = {};

  occupancyMapQP_                          = 5;
  geometryQP_                              = 20;
  attributeQP_                             = 27;
  deltaQPD0_                               = 0;
  deltaQPD1_                               = 2;
  deltaQPT0_                               = 0;
  deltaQPT1_                               = 2;
  use3dmc_                          = true;
  usePccRDO_                        = false;
  shvcRateX_      = 0;
  shvcRateY_      = 0;
}

PCCTranscoderParameters::~PCCTranscoderParameters() = default;
void PCCTranscoderParameters::print() {
  std::cout << "+ Parameters" << std::endl;
  std::cout << "\t compressedStreamPath                " << compressedStreamPath_ << std::endl;
  std::cout << "\t outStreamPath                       " << outStreamPath_ << std::endl;
  std::cout << "\t startFrameNumber                    " << startFrameNumber_ << std::endl;
  std::cout << "\t nbThread                            " << nbThread_ << std::endl;
  std::cout << "\t keepIntermediateFiles               " << keepIntermediateFiles_ << std::endl;
  std::cout << "\t video encoding" << std::endl;
  std::cout << "\t   shvcLayerIndex                    " << shvcLayerIndex_ << std::endl;
  std::cout << "\t   forcedSsvhPrecisionBytes          " << forcedSsvhUnitSizePrecisionBytes_ << std::endl;
  std::cout << "\t video transcoding" << std::endl;
  std::cout << "\t   preset                            " <<   preset << std::endl;
  std::cout << "\t   profile                           " <<   profile << std::endl;
  std::cout << "\t   tier                              " <<   tier << std::endl;
  std::cout << "\t   rate mode                         " <<   rate_mode << std::endl;
  std::cout << "\t   quality value for Attribute       " <<   qualityValAtt_ << std::endl;
  std::cout << "\t   quality value for Geometry        " <<   qualityValGeo_ << std::endl;
  std::cout << "\t   hardware transcoding              " <<   useCuda_ << std::endl;
  std::cout << "\t transcode baseline                " <<   transcodeBaseline_ << std::endl;
  std::cout << "\t   videDecoderGeometryPath           " <<   videoDecoderGeometryPath_ << std::endl;
  std::cout << "\t   videDecoderAttributePath          " <<   videoDecoderAttributePath_ << std::endl;
  std::cout << "\t   geometryQP                               " << geometryQP_ << std::endl;
  std::cout << "\t   attributeQP                              " << attributeQP_ << std::endl;
}

void PCCTranscoderParameters::completePath() {
  if ( !configurationFolder_.empty() ) {
    if ( !geometryConfig_.empty() ) { geometryConfig_ = configurationFolder_ + geometryConfig_; }
    if ( !attributeConfig_.empty() ) { attributeConfig_ = configurationFolder_ + attributeConfig_; }
    if ( !occupancyMapConfig_.empty() ) { occupancyMapConfig_ = configurationFolder_ + occupancyMapConfig_; }
  }
}

bool PCCTranscoderParameters::check() {
  bool ret = true;
  if ( compressedStreamPath_.empty() || !exist( compressedStreamPath_ ) ) {
    ret = false;
    std::cerr << "compressedStreamPath not set or exist\n";
  }
  return ret;
}
