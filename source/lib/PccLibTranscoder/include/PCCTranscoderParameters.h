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
#ifndef PCCTranscoderParameters_h
#define PCCTranscoderParameters_h

#include "PCCCommon.h"

namespace pcc {

class PCCTranscoderParameters {
 public:
  PCCTranscoderParameters();
  ~PCCTranscoderParameters();
  void print();
  bool check();
  void setReconstructionParameters( size_t profileReconstructionIdc );
  void completePath();

  size_t            startFrameNumber_;
  std::string       compressedStreamPath_;
  std::string       outStreamPath_;
  size_t            nbThread_;
  bool              keepIntermediateFiles_;
  uint32_t          forcedSsvhUnitSizePrecisionBytes_;
  std::string       test_name;
  
  //FFMPEG
  std::string       preset;
  std::string       profile;
  std::string       tier;
  std::string       rate_mode;
  std::string       pixelFormat_;
  std::string       qualityValAtt_;
  std::string       qualityValGeo_;
  bool              useCuda_;
  
  // Basline
  bool              transcodeBaseline_;
  bool              byteStreamVideoCoderOccupancy_;
  bool              byteStreamVideoCoderGeometry_;
  bool              byteStreamVideoCoderAttribute_;
  std::string       videoDecoderGeometryPath_;
  std::string       videoDecoderAttributePath_;
  std::string       videoDecoderOccupancyPath_;
  
  // video compression (Baseline)
  int         occupancyMapQP_;
  int         geometryQP_;
  int         attributeQP_;
  int         occupancyPrecision_;
  int         deltaQPD0_;
  int         deltaQPD1_;
  int         deltaQPT0_;
  int         deltaQPT1_;
  bool              use3dmc_;
  bool              usePccRDO_;
  size_t shvcRateX_;
  size_t shvcRateY_;
  std::string       configurationFolder_;
  std::string geometryConfig_;
  std::string attributeConfig_;
  std::string occupancyMapConfig_;
  std::string       videoEncoderGeometryPath_;
  std::string       videoEncoderAttributePath_;
  std::string       videoEncoderOccupancyPath_;
  PCCCodecId        videoEncoderGeometryCodecId_;
  PCCCodecId        videoEncoderAttributeCodecId_;
  PCCCodecId        videoEncoderOccupancyCodecId_;

  size_t shvcLayerIndex_;
  bool constrainedPack_;
  bool globalPatchAllocation_;
  std::string geometryMPConfig_;
};

};  // namespace pcc

#endif /* PCCTranscoderParameters_h */
