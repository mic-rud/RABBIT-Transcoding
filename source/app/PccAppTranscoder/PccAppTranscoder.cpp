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
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "PCCCommon.h"
#include "PCCChrono.h"
#include "PCCMemory.h"
#include "PCCTranscoder.h"
#include "PCCMetrics.h"
#include "PCCChecksum.h"
#include "PCCContext.h"
#include "PCCFrameContext.h"
#include "PCCBitstream.h"
#include "PCCGroupOfFrames.h"
#include "PCCBitstreamReader.h"
#include "PCCBitstreamWriter.h"
#include "PCCTranscoderParameters.h"
#include "PCCMetricsParameters.h"
#include "PCCConformanceParameters.h"
#include "PCCConformance.h"
#include <program_options_lite.h>
#include <tbb/tbb.h>

using namespace std;
using namespace pcc;
using pcc::chrono::StopwatchUserTime;

//---------------------------------------------------------------------------
// :: Command line / config parsing helpers

namespace pcc {
static std::istream& operator>>( std::istream& in, PCCColorTransform& val ) {
  unsigned int tmp;
  in >> tmp;
  val = PCCColorTransform( tmp );
  return in;
}
}  // namespace pcc

//---------------------------------------------------------------------------
// :: Command line / config parsing

bool parseParameters( int                       argc,
                      char*                     argv[],
                      PCCTranscoderParameters&     transcoderParams,
                      PCCMetricsParameters&     metricsParams,
                      PCCConformanceParameters& conformanceParams ) {
  namespace po = df::program_options_lite;

  bool   print_help = false;
  size_t ignore     = 1024;

  // The definition of the program/config options, along with default values.
  //
  // NB: when updating the following tables:
  //      (a) please keep to 80-columns for easier reading at a glance,
  //      (b) do not vertically align values -- it breaks quickly
  //
  // clang-format off
  po::Options opts;
  opts.addOptions()
    ( "help", print_help, false, "This help text")
    ( "c,config", po::parseConfigFile, "Configuration file name")

    // i/o
    ( "compressedStreamPath",
      transcoderParams.compressedStreamPath_,
      transcoderParams.compressedStreamPath_,
    "Output(encoder)/Input(decoder) compressed bitstream")
    ( "outStreamPath",
      transcoderParams.outStreamPath_,
      transcoderParams.outStreamPath_,
    "Output(encoder)/Input(decoder) compressed bitstream")
    ( "test_name",
      transcoderParams.test_name,
      transcoderParams.test_name,
    "Name of the test run")

    // sequence configuration
    ( "startFrameNumber",
      transcoderParams.startFrameNumber_,
      transcoderParams.startFrameNumber_,"Fist frame number in sequence to encode/decode")

    ( "nbThread",
      transcoderParams.nbThread_,
      transcoderParams.nbThread_,
    "Number of thread used for parallel processing")
    ( "keepIntermediateFiles",
      transcoderParams.keepIntermediateFiles_,
      transcoderParams.keepIntermediateFiles_,
      "Keep intermediate files: RGB, YUV and bin")
	  ( "shvcLayerIndex",
	    transcoderParams.shvcLayerIndex_,
	    transcoderParams.shvcLayerIndex_,
     "Decode Layer ID number using SHVC codec")
	  ( "preset",
	    transcoderParams.preset,
	    transcoderParams.preset,
     "FFMPEG preset")
	  ( "profile",
	    transcoderParams.profile,
	    transcoderParams.profile,
     "FFMPEG profile")
	  ( "tier",
	    transcoderParams.tier,
	    transcoderParams.tier,
     "FFMPEG tier")
	  ( "rate_mode",
	    transcoderParams.rate_mode,
	    transcoderParams.rate_mode,
     "FFMPEG rate mode")
	  ( "qualityValAtt",
	    transcoderParams.qualityValAtt_,
	    transcoderParams.qualityValAtt_,
     "FFMPEG Quality values for the rate mode")
	  ( "qualityValGeo",
	    transcoderParams.qualityValGeo_,
	    transcoderParams.qualityValGeo_,
     "FFMPEG Quality values for the rate mode")
	  ( "useCuda",
	    transcoderParams.useCuda_,
	    transcoderParams.useCuda_,
     "Use hardware transcoding")
	  ( "transcodeBaseline",
	    transcoderParams.transcodeBaseline_,
	    transcoderParams.transcodeBaseline_,
     "Use hardware transcoding")
	  ( "byteStreamVideoCoderGeometry",
	    transcoderParams.byteStreamVideoCoderGeometry_,
	    transcoderParams.byteStreamVideoCoderGeometry_,
     "Byte Stream Video Coder for geometry")
	  ( "byteStreamVideoCoderAttribute",
	    transcoderParams.byteStreamVideoCoderAttribute_,
	    transcoderParams.byteStreamVideoCoderAttribute_,
     "Byte Stream Video Coder for geometry")
    ( "videoDecoderGeometryPath",
      transcoderParams.videoDecoderGeometryPath_,
      transcoderParams.videoDecoderGeometryPath_, 
      "Geometry video decoder executable")
    ( "videoDecoderAttributePath",
      transcoderParams.videoDecoderAttributePath_,
      transcoderParams.videoDecoderAttributePath_, 
      "Geometry video decoder executable")
    ( "occupancyPrecision",
      transcoderParams.occupancyPrecision_,
      transcoderParams.occupancyPrecision_, 
      "Precision of occupancy map")
    ( "geometryQP",
      transcoderParams.geometryQP_,
      transcoderParams.geometryQP_, 
      "QP for geometry")
    ( "attributeQP",
      transcoderParams.attributeQP_,
      transcoderParams.attributeQP_, 
      "QP for geometry")
    ( "configurationFolder",
      transcoderParams.configurationFolder_,
      transcoderParams.configurationFolder_, 
      "Folder for configurations")
    ( "occupancyMapConfig",
      transcoderParams.occupancyMapConfig_,
      transcoderParams.occupancyMapConfig_, 
      "File for occupancyMapConfig")
    ( "geometryConfig",
      transcoderParams.geometryConfig_,
      transcoderParams.geometryConfig_, 
      "File for geometryConfig")
    ( "attributeConfig",
      transcoderParams.attributeConfig_,
      transcoderParams.attributeConfig_, 
      "File for attributeConfig")
    ( "constrainedPack",
      transcoderParams.constrainedPack_,
      transcoderParams.constrainedPack_, 
      "Constrained Packing")
    ( "globalPatchAllocation",
      transcoderParams.globalPatchAllocation_,
      transcoderParams.globalPatchAllocation_, 
      "Global Patch Allocation")
    ( "geometryMPConfig",
      transcoderParams.geometryMPConfig_,
      transcoderParams.geometryMPConfig_, 
      "Geometry Map Config")
    ( "occupancyMapConfig",
      transcoderParams.occupancyMapConfig_,
      transcoderParams.occupancyMapConfig_, 
      "Occupancy Map Config")
	  ( "pixelFormat",
	    transcoderParams.pixelFormat_,
	    transcoderParams.pixelFormat_,
     "Pixel format of the transcoding process");

  opts.addOptions()
     ( "checkConformance", 
    conformanceParams.checkConformance_, 
       conformanceParams.checkConformance_, 
       "Check conformance")
     ( "path", 
       conformanceParams.path_, 
       conformanceParams.path_, 
       "Root directory of conformance files + prefix: C:\\Test\\pcc_conformance\\Bin\\S26C03R03_")
     ( "level", 
       conformanceParams.levelIdc_, 
       conformanceParams.levelIdc_, 
       "Level indice")
     ( "fps", 
       conformanceParams.fps_, 
       conformanceParams.fps_, 
       "frame per second" );
  // clang-format on
  po::setDefaults( opts );
  po::ErrorReporter        err;
  const list<const char*>& argv_unhandled = po::scanArgv( opts, argc, (const char**)argv, err );
  for ( const auto arg : argv_unhandled ) { printf( "Unhandled argument ignored: %s \n", arg ); }

  if ( argc == 1 || print_help ) {
    po::doHelp( std::cout, opts, 78 );
    return false;
  }

  transcoderParams.completePath();
  transcoderParams.print();
  if ( !transcoderParams.check() ) {
    printf( "Error Input parameters are not correct \n" );
    return false;
  }
  metricsParams.completePath();
  metricsParams.print();
  if ( !metricsParams.check( true ) ) {
    printf( "Error Input parameters are not correct \n" );
    return false;
  }
  metricsParams.startFrameNumber_ = transcoderParams.startFrameNumber_;
  conformanceParams.print();
  if ( !conformanceParams.check() ) {
    printf( "Error Input parameters are not correct \n" );
    return false;
  }
  // report the current configuration (only in the absence of errors so
  // that errors/warnings are more obvious and in the same place).
  return !err.is_errored;
}


int decompressVideo( PCCTranscoderParameters&       transcoderParams,
                     const PCCMetricsParameters& metricsParams,
                     PCCConformanceParameters&   conformanceParams,
                     StopwatchUserTime&          clock ) {
  PCCBitstream     bitstream_in;
  PCCBitstreamStat bitstreamStat_in;
  PCCBitstreamStat bitstreamStat_out;
  PCCLogger        logger;
  logger.initilalize( removeFileExtension( transcoderParams.compressedStreamPath_ ), false );
#if defined( BITSTREAM_TRACE ) || defined( CONFORMANCE_TRACE )
  bitstream.setLogger( logger );
  bitstream.setTrace( true );
#endif
  if ( !bitstream_in.initialize( transcoderParams.compressedStreamPath_ ) ) { return -1; }
  bitstream_in.computeMD5();
  bitstreamStat_in.setHeader( bitstream_in.size() );
  size_t         frameNumber = transcoderParams.startFrameNumber_;
  PCCMetrics     metrics;
  PCCChecksum    checksum;
  PCCConformance conformance;
  if ( metricsParams.computeChecksum_ ) { checksum.read( transcoderParams.compressedStreamPath_ ); }
  PCCTranscoder transcoder;
  transcoder.setLogger( logger );
  transcoder.setParameters( transcoderParams );

  SampleStreamV3CUnit ssvu_in;
  SampleStreamV3CUnit ssvu_out;
  size_t              headerSize = pcc::PCCBitstreamReader::read( bitstream_in, ssvu_in );
  bitstreamStat_in.incrHeader( headerSize );
  bool bMoreData = true;

  while ( bMoreData ) {
    PCCGroupOfFrames reconstructs;
    PCCContext       context;
    context.setBitstreamStat( bitstreamStat_in );
    clock.start();
    PCCBitstreamReader bitstreamReader;
#ifdef BITSTREAM_TRACE
    bitstreamReader.setLogger( logger );
#endif
    if ( bitstreamReader.decode( ssvu_in, context ) == 0 ) { return 0; }
#if 1
    if ( context.checkProfile() != 0 ) {
      printf( "Profile not correct... \n" );
      return 0;
    }
#endif

    // allocate atlas structure
    context.resizeAtlas( context.getVps().getAtlasCountMinus1() + 1 );
    for ( uint32_t atlId = 0; atlId < context.getVps().getAtlasCountMinus1() + 1; atlId++ ) {
      // first allocating the structures, frames will be added as the V3C
      // units are being decoded ???
      context.setAtlasIndex( atlId );
      int retDecoding = transcoder.transcode( context, atlId );
      clock.stop();
      if ( retDecoding != 0 ) { return retDecoding; }

      context.setBitstreamStat( bitstreamStat_out );
      PCCBitstreamWriter bitstreamWriter;
      retDecoding |= bitstreamWriter.encode( context, ssvu_out );

      bMoreData = ( ssvu_in.getV3CUnitCount() > 0 );
    }
  }

  PCCBitstream bitstream_out;
  PCCBitstreamWriter bitstreamWriter;
  bitstreamStat_out.setHeader( bitstream_out.size() );
  size_t headerSize_out = bitstreamWriter.write( ssvu_out, bitstream_out, transcoderParams.forcedSsvhUnitSizePrecisionBytes_ );
  std::cout << headerSize_out << std::endl;
  bitstreamStat_out.incrHeader( headerSize );
  bitstream_out.write( transcoderParams.outStreamPath_ );
  std::cout << "Total bitstream size " << bitstream_out.size() << " B" << std::endl;

  bitstreamStat_in.trace();
  bitstreamStat_out.trace();

  bool validChecksum = true;
  //if ( metricsParams.computeChecksum_ ) { validChecksum &= checksum.compareRecDec(); }
  return !validChecksum;
}

int main( int argc, char* argv[] ) {
  std::cout << "PccAppTranscoder v" << TMC2_VERSION_MAJOR << "." << TMC2_VERSION_MINOR << std::endl << std::endl;

  PCCTranscoderParameters     transcoderParams;
  PCCMetricsParameters     metricsParams;
  PCCConformanceParameters conformanceParams;
  if ( !parseParameters( argc, argv, transcoderParams, metricsParams, conformanceParams ) ) { return -1; }
  if ( transcoderParams.nbThread_ > 0 ) { tbb::task_scheduler_init init( static_cast<int>( transcoderParams.nbThread_ ) ); }

  // Timers to count elapsed wall/user time
  pcc::chrono::Stopwatch<std::chrono::steady_clock> clockWall;
  pcc::chrono::StopwatchUserTime                    clockUser;

  clockWall.start();
  int ret = decompressVideo( transcoderParams, metricsParams, conformanceParams, clockUser );
  clockWall.stop();

  using namespace std::chrono;
  using ms            = milliseconds;
  auto totalWall      = duration_cast<ms>( clockWall.count() ).count();
  auto totalUserSelf  = duration_cast<ms>( clockUser.self.count() ).count();
  auto totalUserChild = duration_cast<ms>( clockUser.children.count() ).count();
  std::cout << "Processing time (wall): " << ( ret == 0 ? totalWall / 1000.0 : -1 ) << " s\n";
  std::cout << "Processing time (user.self): " << ( ret == 0 ? totalUserSelf / 1000.0 : -1 ) << " s\n";
  std::cout << "Processing time (user.children): " << ( ret == 0 ? totalUserChild / 1000.0 : -1 ) << " s\n";
  std::cout << "Peak memory: " << getPeakMemory() << " KB\n";

  ofstream logFile;
  logFile.open(transcoderParams.test_name + ".txt");
  logFile << totalWall / 1000.0 << ",";
  return ret;
}
