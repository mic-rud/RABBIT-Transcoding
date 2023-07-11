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
#include "PCCHighLevelSyntax.h"
#include "PCCBitstream.h"
#include "PCCVideoBitstream.h"
#include "PCCContext.h"
#include "PCCFrameContext.h"
#include "PCCPatch.h"
#include "PCCVideoDecoder.h"
#include "PCCVideoEncoder.h"
#include "PCCGroupOfFrames.h"
#include <tbb/tbb.h>
#include "PCCTranscoder.h"

extern "C" {
  #include <libavcodec/avcodec.h>
  #include <libavformat/avformat.h>
  #include <libavutil/opt.h>
  #include <libavutil/hwcontext.h>
  #include <libavutil/imgutils.h>
  #include <libswscale/swscale.h>
}

using namespace pcc;
using namespace std;

PCCTranscoder::PCCTranscoder() {
#ifdef ENABLE_PAPI_PROFILING
  initPapiProfiler();
#endif
}
PCCTranscoder::~PCCTranscoder() = default;
void PCCTranscoder::setParameters( const PCCTranscoderParameters& params ) { params_ = params; }

int PCCTranscoder::transcode( PCCContext& context, int32_t atlasIndex = 0 ) {
  if ( params_.nbThread_ > 0 ) { tbb::task_scheduler_init init( static_cast<int>( params_.nbThread_ ) ); }
  //createPatchFrameDataStructure( context );

  transcodeData(context);
  addEndTile(context);
  return 0;
}

static int readFunction(void* opaque, uint8_t* buf, int buf_size) {
    auto& me = *reinterpret_cast<std::istream*>(opaque);
    me.read(reinterpret_cast<char*>(buf), buf_size);
    return me.gcount();
}

static int read_packet(void* opaque, uint8_t* buf, int buf_size) {
    struct buffer_data *bd = (struct buffer_data *)opaque;
    buf_size = FFMIN(buf_size, bd->room);

    memcpy(buf, bd->ptr, buf_size);
    bd->ptr += buf_size;
    bd->room -= buf_size;

    //printf("Read packet: buff %p ptr %p size %zu room %zu \n", bd->buf, bd->ptr, bd->size, bd->room);
    return buf_size;
}

static int write_packet(void* opaque, uint8_t* buf, int buf_size) {
    struct buffer_data *bd = (struct buffer_data *)opaque;
    while (buf_size > bd->room) {
      int64_t offset = bd->ptr - bd->buf;
      bd->buf = (uint8_t *) av_realloc_f(bd->buf, 2, bd->size);
      if (!bd ->buf)
        return AVERROR(ENOMEM);
      bd->size *= 2;
      bd->ptr = bd->buf + offset;
      bd->room = bd->size - offset;
    }
    memcpy(bd->ptr, buf, buf_size);
    bd->ptr += buf_size;
    bd->room -= buf_size;

    printf("Write packet: ptr %p size %zu room %zu \n", bd->ptr, bd->size, bd->room);
    return buf_size;
}

static int64_t seek (void *opaque, int64_t offset, int whence) {
    struct buffer_data *bd = (struct buffer_data *)opaque;
    switch(whence){
        case SEEK_SET:
        {
            bd->ptr = bd->buf + offset;
            return (int64_t) bd->ptr;
        }
            break;
        case SEEK_CUR:
        {
            bd->ptr += offset;
            break;
        }
        case SEEK_END:
        {
            bd->ptr = (bd->buf + bd->size) + offset;
            return (int64_t) bd->ptr;
            break;
        }
        case AVSEEK_SIZE:
        {
            return bd->size;
            break;
        }
        default:
           return -1;
    }
    //printf("Seek: ptr %p size %zu\n", bd->ptr, bd->size);
    return 1;
}


void PCCTranscoder::transcodeData(PCCContext& context) {
  if (params_.transcodeBaseline_) {
    transcodeBaseline(context);
  } else {
    // Transcode Occupancy video
    if (params_.occupancyPrecision_ == 4) {
      auto& occVideoBitstream = context.getVideoBitstream( VIDEO_OCCUPANCY );
      occVideoBitstream.sampleStreamToByteStream();
      transcodeVideo(occVideoBitstream, VIDEO_OCCUPANCY);
      context.setOccupancyPrecision(params_.occupancyPrecision_);
    }

    // Transcode Geometry video
    auto& geoVideoBitstream = context.getVideoBitstream( VIDEO_GEOMETRY );
    geoVideoBitstream.sampleStreamToByteStream();
    transcodeVideo(geoVideoBitstream, VIDEO_GEOMETRY);

    // Transcode Attribute video
    auto& attVideoBitstream = context.getVideoBitstream( VIDEO_ATTRIBUTE );
    attVideoBitstream.sampleStreamToByteStream();
    transcodeVideo(attVideoBitstream, VIDEO_ATTRIBUTE);

  }
}

void PCCTranscoder::transcodeBaseline(PCCContext& context) {
  pcc::PCCVideoEncoder videoEncoder;

  std::stringstream path;
  auto&             sps              = context.getVps();
  auto&             asps             = context.getAtlasSequenceParameterSet(0);
  size_t            frameHeight      = asps.getFrameHeight();
  size_t            frameWidth       = asps.getFrameWidth();


  // Occupancy map
  size_t            occupancyPrecision = 2; // Not available in bitstream?

  // Decode occupancy map
  auto&             oi               = sps.getOccupancyInformation(0);
  int               occupancyBitDepth = oi.getOccupancy2DBitdepthMinus1() + 1;
  auto& occVideoBitstream = context.getVideoBitstream( VIDEO_OCCUPANCY );
  auto occupancyCodecId  = getCodedCodecId( context, oi.getOccupancyCodecId(), params_.videoDecoderOccupancyPath_ );
  occVideoBitstream.sampleStreamToByteStream();
  occVideoBitstream.write("occ_vid.bin");

  string decodeCmd = "./bin/PccAppVideoDecoder";
  decodeCmd += " --bin=occ_vid.bin ";
  decodeCmd += " --codecId=4";
  decodeCmd += " --width=" + std::to_string(frameWidth / occupancyPrecision); 
  decodeCmd += " --height=" + std::to_string(frameHeight / occupancyPrecision); 
  decodeCmd += " --nbyte=1"; 
  decodeCmd += " --path=" + params_.videoEncoderGeometryPath_;

  std::cout << decodeCmd << std::endl;
  std::system(decodeCmd.c_str());
  std::cout << "Decoding done" << std::endl;
  std::cout << "Res: " << frameWidth / occupancyPrecision << std::endl;

  PCCVideo<uint8_t, 3> occVideo;
  occVideo.read("occ_vid_dec.yuv", frameWidth / occupancyPrecision, frameHeight / occupancyPrecision, YUV420, 1);
  occupancyPrecision = frameWidth / occVideo.getFrame(0).getWidth();

  PCCVideo<uint8_t, 3> occVideoResized;
  occVideoResized.resize(occVideo.getFrameCount());
  size_t factor = params_.occupancyPrecision_ / occupancyPrecision;
  std::cout << "Factor: " << factor << std::endl;
  resizeOccupancyMap(occVideo, occVideoResized, frameWidth /occupancyPrecision , frameHeight / occupancyPrecision, factor);

  videoEncoder.compress( occVideoResized,                         // video
                         path.str(),                                // path
                         params_.occupancyMapQP_,                   // QP
                         occVideoBitstream,                            // bitstream
                         params_.occupancyMapConfig_,               // config file
                         params_.videoEncoderOccupancyPath_,        // encoder path
                         params_.videoEncoderOccupancyCodecId_,     // Codec id
                         params_.byteStreamVideoCoderOccupancy_,    // byteStreamVideoCoder
                         context,                                   // context
                         1,                                         // nByte
                         false,                                     // use444CodecIo
                         false,                                     // use3dmv
                         false,                                     // usePccRDO
                         0,                                         // SHVC Layer Index
                         0,                                         // SHVC ratio X
                         0,                                         // SHVC ratio Y
                         8,                                         // internalBitDepth
                         false,                                     // useConversion
                         params_.keepIntermediateFiles_ );          // keepIntermediateFiles


  // Decode Video with AppDecoder to prevent linking errors....
  path << removeFileExtension( params_.compressedStreamPath_ ) << "_dec_GOF" << sps.getV3CParameterSetId() << "_";

  auto&             gi               = sps.getGeometryInformation( 0 );
  int               geometryBitDepth = gi.getGeometry2dBitdepthMinus1() + 1;

  auto& geoVideoBitstream = context.getVideoBitstream( VIDEO_GEOMETRY );
  auto geometryCodecId  = getCodedCodecId( context, gi.getGeometryCodecId(), params_.videoDecoderGeometryPath_ );
  geoVideoBitstream.sampleStreamToByteStream();
  geoVideoBitstream.write("geo_vid.bin");

  decodeCmd = "./bin/PccAppVideoDecoder";
  decodeCmd += " --bin=geo_vid.bin ";
  decodeCmd += " --codecId=4";
  decodeCmd += " --width=" + std::to_string(frameWidth); 
  decodeCmd += " --height=" + std::to_string(frameHeight); 
  decodeCmd += " --nbyte=1"; 
  decodeCmd += " --path=" + params_.videoEncoderGeometryPath_;

  std::cout << decodeCmd << std::endl;
  std::system(decodeCmd.c_str());
  std::cout << "Decoding done" << std::endl;

  PCCVideo<uint16_t, 3> geoVideo;
  geoVideo.read("geo_vid_dec.yuv", frameWidth, frameHeight, YUV420, 1);
  std::cout << "Reading done" << std::endl;

  size_t internalBitDepth        = 10;
  size_t nbyteGeo                = ( geometryBitDepth <= 8 ) ? 1 : 2;
  geoVideoBitstream.resize(0);
  videoEncoder.compress( geoVideo,                             // video
                         path.str(),                                // path
                         params_.geometryQP_ + params_.deltaQPD0_,  // QP
                         geoVideoBitstream,                          // bitstream
                         params_.geometryConfig_,                        // config file
                         params_.videoEncoderGeometryPath_,         // encoder path
                         params_.videoEncoderGeometryCodecId_,      // Codec id
                         params_.byteStreamVideoCoderGeometry_,     // byteStreamVideoCoder
                         context,                                   // context
                         nbyteGeo,                                  // nbyte
                         false,                                     // use444CodecIo
                         false,                                     // use3dmv
                         params_.usePccRDO_,                        // usePccRDO
                         params_.shvcLayerIndex_,                   // SHVC layer index
                         params_.shvcRateX_,                        // SHVC rate X
                         params_.shvcRateY_,                        // SHVC rate Y
                         internalBitDepth,                          // internalBitDepth
                         false,                                     // useConversion
                         params_.keepIntermediateFiles_ );          // keep intermediate


  path.str(std::string()); //Clear path
  path << removeFileExtension( params_.compressedStreamPath_ ) << "_dec_GOF" << sps.getV3CParameterSetId() << "_";

  auto&             ai               = sps.getAttributeInformation( 0 );
  int               attributeBitDepth = ai.getAttribute2dBitdepthMinus1(0) + 1;


  // Decode Video with AppDecoder to prevent linking errors....
  auto& attVideoBitstream = context.getVideoBitstream( VIDEO_ATTRIBUTE );
  auto attributeCodecId  = getCodedCodecId( context, ai.getAttributeCodecId(0), params_.videoDecoderAttributePath_ );
  attVideoBitstream.sampleStreamToByteStream();
  attVideoBitstream.write("att_vid.bin");

  decodeCmd = "./bin/PccAppVideoDecoder";
  decodeCmd += " --bin=att_vid.bin ";
  decodeCmd += " --codecId=4";
  decodeCmd += " --width=" + std::to_string(frameWidth);
  decodeCmd += " --height=" + std::to_string(frameHeight); 
  decodeCmd += " --nbyte=1";
  decodeCmd += " --path=" + params_.videoEncoderAttributePath_;

  std::cout << decodeCmd << std::endl;
  std::system(decodeCmd.c_str());
  std::cout << "Decoding done" << std::endl;

  PCCVideo<uint16_t, 3> attVideo;
  attVideo.read("att_vid_dec.yuv", frameWidth, frameHeight, YUV420, 1);
  std::cout << "Reading done" << std::endl;

  internalBitDepth        = 10;
  size_t nbyteAtt         = ( attributeBitDepth<= 8 ) ? 1 : 2;
  attVideoBitstream.resize(0);
  videoEncoder.compress( attVideo,                             // video
                         path.str(),                                // path
                         params_.attributeQP_ + params_.deltaQPD0_,  // QP
                         attVideoBitstream,                          // bitstream
                         params_.attributeConfig_,                        // config file
                         params_.videoEncoderAttributePath_,         // encoder path
                         params_.videoEncoderAttributeCodecId_,      // Codec id
                         params_.byteStreamVideoCoderAttribute_,     // byteStreamVideoCoder
                         context,                                   // context
                         nbyteAtt,                                  // nbyte
                         false,                                     // use444CodecIo
                         false,                                     // use3dmv
                         params_.usePccRDO_,                        // usePccRDO
                         params_.shvcLayerIndex_,                   // SHVC layer index
                         params_.shvcRateX_,                        // SHVC rate X
                         params_.shvcRateY_,                        // SHVC rate Y
                         internalBitDepth,                          // internalBitDepth
                         false,                                     // useConversion
                         params_.keepIntermediateFiles_ );          // keep intermediate

}


void PCCTranscoder::resizeOccupancyMap(PCCVideo<uint8_t, 3>& sourceVideo, PCCVideo<uint8_t, 3>& targetVideo, size_t frameWidth, size_t frameHeight, size_t factor){
  for (size_t frameIdx = 0; frameIdx < sourceVideo.getFrameCount(); ++ frameIdx) {
    auto& sourceFrame = sourceVideo.getFrame(frameIdx);
    auto& targetFrame = targetVideo.getFrame(frameIdx);
    std::cout << frameWidth / factor << std::endl;
    targetFrame.resize(frameWidth / factor, frameHeight / factor, sourceFrame.getColorFormat());

    size_t targetWidth = frameWidth / factor;
    size_t targetHeigth= frameHeight / factor;

    // Loop
    for (size_t u = 0; u < targetWidth; ++u){
      for (size_t v = 0; v < targetHeigth; ++v){
        bool isValue = false;
        for (size_t u1 = 0; u1 < factor; ++u1){
          for (size_t v1 = 0; v1 < factor; ++v1){
            uint8_t pixel = sourceFrame.getValue(0, u*factor+u1, v*factor+v1);
            if (pixel > 0){
              isValue = true;
            }
          
          }
        }
        if (isValue) {
          targetFrame.setValue(0, u, v, 1);
        } else {
          targetFrame.setValue(0, u, v, 0);
        }
      }
    }
  }
}

void PCCTranscoder::transcodeVideo(PCCVideoBitstream& videoBitstream, PCCVideoType type) {
  printf("Starting Transcoding of sequence\n");
  StreamingContext *decoder = (StreamingContext*) calloc(1, sizeof(StreamingContext));
  StreamingContext *encoder = (StreamingContext*) calloc(1, sizeof(StreamingContext));

  uint8_t *buffer=NULL;
  uint8_t *in_avio_ctx_buffer = NULL;
  uint8_t *out_avio_ctx_buffer = NULL;
  size_t avio_ctx_buffer_size = 2*8192;
  size_t bd_buf_size = 2*8192;

  struct buffer_data in_bd = { 0 };
  struct buffer_data out_bd = { 0 };
  int ret=0;

  av_register_all();

  // Set up output buffer 
  in_bd.ptr = in_bd.buf = videoBitstream.buffer();
  in_bd.size = in_bd.room = videoBitstream.size();

  // Set up output buffer
  auto out_vec = std::vector<uint8_t>(videoBitstream.size() * 2);
  auto out_buffer = out_vec.data();
  auto out_size = out_vec.size();
  out_bd.ptr = out_bd.buf = out_buffer;
  out_bd.size = out_bd.room = out_size;
  printf("Buffers set.\n");

  // Set up the decoder
  initDecoder(decoder, &in_bd);
  //initEncoder(encoder, decoder, &out_bd);


  // Allocate frame and input packet
  AVFrame *input_frame = av_frame_alloc();
  if (!input_frame) {
    printf("Could not allocate frame\n");
    exit(0);
  }
  AVFrame *output_frame = av_frame_alloc();
  if (!input_frame) {
    printf("Could not allocate frame\n");
    exit(0);
  }
  AVPacket *input_packet = av_packet_alloc();
  if (!input_packet) {
    printf("Could not allocate packet\n");
    exit(0);
  }
  struct SwsContext *swsCtx;

  // Transcoding loop
  int frameIdx = 0;
  while (av_read_frame(decoder->avFmtCtx, input_packet) >= 0) {
    printf("Transcoding frame %u\n", frameIdx);

    //Transcode packet
    int response = avcodec_send_packet(decoder->avCdcCtx, input_packet);
    if (response < 0) {
      printf("Error while sending packet to decoder\n");
      exit(-1);
    }

    while (response >= 0) {
      // Receive frame from decoder
      response = avcodec_receive_frame(decoder->avCdcCtx, input_frame);
      if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
        //End of packet
        break;
      } else if (response < 0) {
        //Error handling
        printf("Error while receiving frame from decoder\n");
        exit(-1);
      }



      if (!encoder->initialized) {
        initEncoder(encoder, decoder, &out_bd, type);

        //Write header
        if (avformat_write_header(encoder->avFmtCtx, NULL)) {
          printf("Error with writing to output\n");
          exit(1);
        }
      }

      if (response >= 0) {
        //Rescale the video
        if (type == VIDEO_OCCUPANCY && params_.occupancyPrecision_ == 4 ) { //TODO set params properly to decide if downscaling is required
          printf("Resizing frame\n");
          resize_frame2(decoder, input_frame, output_frame, 2);

          // Encode video frame
          if (output_frame){
            output_frame->pict_type = AV_PICTURE_TYPE_NONE;
            output_frame->pts = frameIdx++;
          }
          encodeVideo(output_frame, encoder, decoder, &out_bd);
        } else {
          // Encode video frame
          if (input_frame){
            input_frame->pict_type = AV_PICTURE_TYPE_NONE;
            input_frame->pts = frameIdx++;
          }
          encodeVideo(input_frame, encoder, decoder, &out_bd);
        }

      }
      av_frame_unref(input_frame);
      //av_frame_unref(output_frame);
    }
    av_packet_unref(input_packet);
  };

  // Clean up
  if (input_frame != NULL) {
    printf("Freeing input frame\n");
    av_frame_free(&input_frame);
  }
  if (output_frame != NULL) {
    printf("Freeing output frame\n");
    av_frame_free(&input_frame);
  }

  if (input_packet != NULL) {
    printf("Freeing input packet\n");
    av_packet_free(&input_packet);
  }

  //Flush encoder
  encodeVideo(NULL, encoder, decoder, &out_bd);
  //Write trailer data
  av_write_trailer(encoder->avFmtCtx);

  // Resize output buffer and set to videoBuffer
  int out_stream_size = out_bd.size - out_bd.room;
  out_vec.resize(out_stream_size);
  printf("Copying to videoBitstream\n");
  videoBitstream.vector() = out_vec;
  videoBitstream.resize(out_stream_size);
  printf("Resizing videoBitstream\n");
  videoBitstream.byteStreamToSampleStream();

  printf("Done ... cleaning up\n");

  // In fmt ctx
  avformat_close_input(&decoder->avFmtCtx);
  if (decoder->avIoCtx) {
    av_freep(&decoder->avIoCtx->buffer);
    av_freep(&decoder->avIoCtx);
  }

  // Out fmt ctx
  avformat_close_input(&encoder->avFmtCtx);
  if (encoder->avIoCtx) {
    av_freep(&encoder->avIoCtx->buffer);
    av_freep(&encoder->avIoCtx);
  }

  sws_freeContext(swsCtx);

  avformat_free_context(decoder->avFmtCtx); decoder->avFmtCtx = NULL;
  avformat_free_context(encoder->avFmtCtx); encoder->avFmtCtx = NULL;

  avcodec_free_context(&decoder->avCdcCtx); decoder->avCdcCtx = NULL;
  avcodec_free_context(&encoder->avCdcCtx); encoder->avCdcCtx = NULL;

  if (params_.useCuda_) {
    av_buffer_unref(&decoder->hwDevCtx);
  }
}

int PCCTranscoder::encodeVideo( AVFrame* input_frame, StreamingContext* encoder, StreamingContext* decoder, buffer_data* out_bd) {
  if (!input_frame) {
    printf("Flushing encoder\n");
  }

  // Allocate output packet
  AVPacket *output_packet = av_packet_alloc();
  if (!output_packet){
    printf("Could not allocate memory for output packet\n");
    exit(-1);
  }
  
  //Send packet to encoder
  int response = avcodec_send_frame(encoder->avCdcCtx, input_frame);

  while (response >= 0) {
    //Receive packet from encoder
    response = avcodec_receive_packet(encoder->avCdcCtx, output_packet);
    if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
      // All packets received
      break;
    } else if (response < 0) {
      //Error handling
      printf("Error while receiving packet from encoder\n");
      exit(-1);
    }

    
    // Get time scale and metadata
    output_packet->stream_index = 0; 
    output_packet->duration = encoder->avStrm->time_base.den / encoder->avStrm->time_base.num \
    / decoder->avStrm->avg_frame_rate.num * decoder->avStrm->avg_frame_rate.den;
    //av_packet_rescale_ts(output_packet, in_avs->time_base, out_avs->time_base);

    response = av_write_frame(encoder->avFmtCtx, output_packet);

    if (response != 0) {
      printf("Error while sending packet to decoder\n");
      exit(-1);
    }
  }
  av_packet_unref(output_packet);
  av_packet_free(&output_packet);
  return 0;
}

int PCCTranscoder::resize_frame2(StreamingContext* decoder, AVFrame* input_frame, AVFrame* output_frame, int factor) {
  int resized_width = input_frame->width/factor;
  int resized_height = input_frame->height/factor;
  output_frame->width = resized_width;
  output_frame->height = resized_height;
  output_frame->format = decoder->avCdcCtx->pix_fmt;

  printf("Resizing to %u x %u\n", resized_width, resized_height);
  printf("Pixel format: %s\n", av_get_pix_fmt_name(decoder->avCdcCtx->pix_fmt));

  int size = avpicture_get_size(decoder->avCdcCtx->pix_fmt, resized_width, resized_height);
  printf("Input width: %u\n", input_frame->width);
  printf("Input linesize: %u\n", input_frame->linesize[0]);
  printf("Output width: %u\n", output_frame->width);
  printf("Output linesize: %u\n", output_frame->linesize[0]);

  // Init buffers
  uint8_t* buffer = (uint8_t*) av_malloc(size);
  avpicture_fill((AVPicture *)output_frame, buffer, 
                  decoder->avCdcCtx->pix_fmt, resized_width, resized_height);
  // Loop
  for (int u = 0; u < resized_width; ++u){
    for (int v = 0; v < resized_height; ++v){ // Height times linesize!!
      bool isValue = false;
      uint8_t pixel = input_frame->data[0][(v) * input_frame->linesize[0] + u];
      //output_frame->data[0][(v) * output_frame->linesize[0] + u] = pixel;
      for (size_t u1 = 0; u1 < factor; ++u1){
        for (size_t v1 = 0; v1 < factor; ++v1){
          uint8_t pixel = input_frame->data[0][(v*factor+v1) * input_frame->linesize[0] + u*factor+u1];
          if (pixel > 0){
            isValue = true;
          }
        
        }
      }
      if (isValue) {
        output_frame->data[0][v * output_frame->linesize[0] + u] = 1;
        //printf("%u", 1);
      } else {
        output_frame->data[0][v * output_frame->linesize[0] + u] = 0;
        //printf("%u", 0);
      }
    }
  }
  /*
  memcpy(output_frame->data[1], input_frame->data[1], input_frame->linesize[1]);
  memcpy(output_frame->data[2], input_frame->data[2], input_frame->linesize[2]);
  */
  printf("Output width: %u\n", output_frame->width);
  printf("Output linesize: %u\n", output_frame->linesize[0]);
  printf("Rescaling done\n");
  return 0;
}

int PCCTranscoder::resize_frame(SwsContext* swsCtx, StreamingContext* decoder, AVFrame* input_frame, AVFrame* output_frame) {
  printf("Rescaling\n.");
  int resized_width = input_frame->width/2;
  int resized_height = input_frame->height/2;
  printf("Resizing to %u x %u\n", resized_width, resized_height);

  printf("Pixel format: %s\n", av_get_pix_fmt_name(decoder->avCdcCtx->pix_fmt));
  swsCtx = sws_getContext(input_frame->width, input_frame->height, decoder->avCdcCtx->pix_fmt,
                          resized_width, resized_height, decoder->avCdcCtx->pix_fmt, 
                          SWS_POINT, NULL, NULL, NULL);
  if (!swsCtx) {
    printf("Failed to allocate swsContext\n");
    return -1;
  }

  //av_image_alloc(output_frame->data, output_frame->linesize, resized_width, resized_height, decoder->avCdcCtx->pix_fmt, 32);
  output_frame->width = resized_width;
  output_frame->height = resized_height;
  output_frame->format = decoder->avCdcCtx->pix_fmt;
  //av_frame_get_buffer(output_frame, 32);
  int size = avpicture_get_size(decoder->avCdcCtx->pix_fmt, resized_width, resized_height);
  uint8_t* tmp_buffer = (uint8_t*) malloc(size);
  avpicture_fill((AVPicture *)output_frame, tmp_buffer, 
                  decoder->avCdcCtx->pix_fmt, resized_width, resized_height);
  
  int ret = sws_scale(swsCtx, (const uint8_t* const*)input_frame->data, input_frame->linesize, 0,
                      input_frame->height, 
                      output_frame->data, output_frame->linesize);

  printf("Output width: %u\n", output_frame->width);
  printf("Output linesize: %u\n", output_frame->linesize);
  printf("Rescaling done\n");
  return 0;
}

int PCCTranscoder::initEncoder(StreamingContext* encoder, StreamingContext* decoder, buffer_data* bd, PCCVideoType type){
  int ret = 0;
  printf("Init Encoder\n");
  int ctx_buffer_size = 8192; //TODO make parameter
  encoder->ctxBuffer = (uint8_t*) av_malloc(ctx_buffer_size);
  if (!encoder->ctxBuffer) {
    ret = AVERROR(ENOMEM);
    exit(1);
  }

  if (params_.useCuda_) {
    encoder->avCdc = avcodec_find_encoder_by_name("hevc_nvenc"); //Maybe h265_nvenc?
    encoder->avCdcCtx = avcodec_alloc_context3(encoder->avCdc);
    if (!encoder->avCdc) {
      printf("Encoder not found\n");
      exit(-1);
    }
    if (!decoder->avCdcCtx->hw_frames_ctx) {
      printf("No decoder hw context\n");
      exit(-1);
    }
    encoder->avCdcCtx->hw_frames_ctx = av_buffer_ref(decoder->avCdcCtx->hw_frames_ctx);
  } else {
    encoder->avCdc = avcodec_find_encoder_by_name("libx265");
    encoder->avCdcCtx = avcodec_alloc_context3(encoder->avCdc);
  }

  if (!encoder->avCdc) {
    printf("Failed to find encoder.\n");
    exit(1);
  }

  encoder->avIoCtx = avio_alloc_context(encoder->ctxBuffer, ctx_buffer_size, 1, bd, NULL, &write_packet, &seek);
  if (!encoder->avIoCtx) {
    ret = AVERROR(ENOMEM);
    exit(1);
  }

  AVOutputFormat* outFmt = av_guess_format("hevc", NULL, NULL);
  ret = avformat_alloc_output_context2(&encoder->avFmtCtx, outFmt, NULL, NULL);
  if (ret < 0){
    exit(1);
  }

  encoder->avStrm = avformat_new_stream(encoder->avFmtCtx, NULL);

  // Set options for Encoder
  setEncoderOptions(encoder, decoder, type);

  encoder->avFmtCtx->pb = encoder->avIoCtx;
  encoder->avFmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
  encoder->avFmtCtx->oformat = outFmt;

  if (avcodec_open2(encoder->avCdcCtx, encoder->avCdc, NULL) < 0){
    exit(1);
  }

  avcodec_parameters_from_context(encoder->avStrm->codecpar, encoder->avCdcCtx);
  printf("Parameters set\n");


  if (encoder->avFmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
    encoder->avFmtCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; //What does this do?
  }

  encoder->initialized = true;
  printf("Encoder initialized\n");


  return ret;
}

int PCCTranscoder::initDecoder(StreamingContext* decoder, buffer_data* bd){
  int ret = 0;
  printf("Init Decoder ... \n");


  if (!(decoder->avFmtCtx = avformat_alloc_context())) {
    printf("Can't allocate context");
    ret = -1;
  }

  int ctx_buf_size = 8192;
  decoder->ctxBuffer = (uint8_t *) av_malloc(ctx_buf_size); //make Param buffer size
  if (!decoder->ctxBuffer) {
    printf("Can't allocate IO buffer");
    ret = -1;
  }
  printf("Bufer allocated\n");

  decoder->avIoCtx = avio_alloc_context(decoder->ctxBuffer, ctx_buf_size, 0, bd, &read_packet, NULL, &seek);
  if (!decoder->avIoCtx) {
    printf("Can't allocate avio buffer");
    ret = -1;
  }

  decoder->avFmtCtx->pb = decoder->avIoCtx;
  ret = avformat_open_input(&decoder->avFmtCtx, NULL, NULL, NULL);
  if (ret < 0) {
    printf("Could not open input");
  }
  ret = avformat_find_stream_info(decoder->avFmtCtx, NULL);
  if (ret < 0) {
    printf("Could not find stream info");
  }

  printf("Looking for streams\n");
  decoder->avStrm = decoder->avFmtCtx->streams[0]; //Just one stream expected
  if (params_.useCuda_) {
    //decoder->avCdc = avcodec_find_decoder_by_name("hevc_cuvid");
    decoder->avCdc = avcodec_find_decoder(decoder->avStrm->codecpar->codec_id);
  } else {
    decoder->avCdc = avcodec_find_decoder(decoder->avStrm->codecpar->codec_id);
  }
  //printf("Decoder: %s\t HardwareConfig: %i\n", decoder->avCdc->name, decoder->avCdc->hw_configs);
  decoder->avCdcCtx= avcodec_alloc_context3(decoder->avCdc);

  if (params_.useCuda_) {
    ret = av_hwdevice_ctx_create(&decoder->hwDevCtx, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0);
    if (ret < 0) {
      fprintf(stderr, "Failed to create a CUDA device \n");
      return -1;
    }

    decoder->avCdcCtx->hw_device_ctx = av_buffer_ref(decoder->hwDevCtx);
    printf("Test\n");
  } 
  
  avcodec_parameters_to_context(decoder->avCdcCtx, decoder->avStrm->codecpar);
  avcodec_open2(decoder->avCdcCtx, decoder->avCdc, NULL);
 
  // Set some nvdec specific options
  if (params_.useCuda_) { 
    // Required to increase the Buffer for nvdec->nvenc
    decoder->avCdcCtx->extra_hw_frames = 8;
  }

  decoder->initialized = true;
  printf("Decoder initialized\n");
  return ret;
}

int PCCTranscoder::setEncoderOptions(StreamingContext* encoder, StreamingContext* decoder, PCCVideoType type){
  // Pixel info from Decoder
  encoder->avCdcCtx->sample_aspect_ratio = decoder->avCdcCtx->sample_aspect_ratio;
  encoder->avCdcCtx->pix_fmt = decoder->avCdcCtx->pix_fmt;

  if (type == VIDEO_OCCUPANCY) {
    //Occupancy map
    encoder->avCdcCtx->height = decoder->avCdcCtx->height / (params_.occupancyPrecision_ / 2);
    encoder->avCdcCtx->width = decoder->avCdcCtx->width / (params_.occupancyPrecision_ / 2);

    // Lossless all-intra
    encoder->avCdcCtx->gop_size = 1;
    encoder->avCdcCtx->max_b_frames = 0;
    if (params_.useCuda_) {
      av_opt_set(encoder->avCdcCtx->priv_data, "preset", "10", 0);  // 10 = lossless
    } else {
      av_opt_set(encoder->avCdcCtx->priv_data, "x265-params", "lossless=1", 0);
      av_opt_set(encoder->avCdcCtx->priv_data, "profile", "main", 0); 
      av_opt_set(encoder->avCdcCtx->priv_data, "output-depth", "8", 0); 
    }
  } else {
    // Attribute and Geometry
    encoder->avCdcCtx->height = decoder->avCdcCtx->height;
    encoder->avCdcCtx->width = decoder->avCdcCtx->width;
    // All-Intra with no B-Frames and GOP of 2
    encoder->avCdcCtx->gop_size = 2;
    encoder->avCdcCtx->max_b_frames = 0;

    if (params_.useCuda_) {
      // Hardware Encoding 
      // Disable B-Frames and lookahead for All-Intra Coding
      av_opt_set(encoder->avCdcCtx->priv_data, "rc", "constqp", 0); 
      av_opt_set(encoder->avCdcCtx->priv_data, "rc-lookahead", "0", 0);
      av_opt_set(encoder->avCdcCtx->priv_data, "bf", "0", 0); 
      if (type == VIDEO_ATTRIBUTE) {
        av_opt_set(encoder->avCdcCtx->priv_data, "qp", std::to_string(params_.attributeQP_).c_str(), 0);
        av_opt_set(encoder->avCdcCtx->priv_data, "min_qp", std::to_string(params_.attributeQP_ -3).c_str(), 0);
        av_opt_set(encoder->avCdcCtx->priv_data, "init_cqp_i", std::to_string(params_.attributeQP_).c_str(), 0); 
        av_opt_set(encoder->avCdcCtx->priv_data, "init_cqp_p", std::to_string(params_.attributeQP_).c_str(), 0); 
      } else if (type == VIDEO_GEOMETRY) {
        av_opt_set(encoder->avCdcCtx->priv_data, "qp", std::to_string(params_.geometryQP_).c_str(), 0);
        av_opt_set(encoder->avCdcCtx->priv_data, "min_qp", std::to_string(params_.geometryQP_ -3).c_str(), 0);
        av_opt_set(encoder->avCdcCtx->priv_data, "init_cqp_i", std::to_string(params_.geometryQP_).c_str(), 0); 
        av_opt_set(encoder->avCdcCtx->priv_data, "init_cqp_p", std::to_string(params_.geometryQP_).c_str(), 0); 
      } else {
        av_opt_set(encoder->avCdcCtx->priv_data, "qp", std::to_string(params_.occupancyMapQP_).c_str(), 0);
        av_opt_set(encoder->avCdcCtx->priv_data, "min_qp", std::to_string(params_.occupancyMapQP_ -3).c_str(), 0);
        av_opt_set(encoder->avCdcCtx->priv_data, "init_cqp_i", std::to_string(params_.occupancyMapQP_).c_str(), 0); 
        av_opt_set(encoder->avCdcCtx->priv_data, "init_cqp_p", std::to_string(params_.occupancyMapQP_).c_str(), 0); 
      }
      av_opt_set(encoder->avCdcCtx->priv_data, "tier", params_.tier.c_str(), 0); 
      av_opt_set(encoder->avCdcCtx->priv_data, "profile", params_.profile.c_str(), 0); 
      av_opt_set(encoder->avCdcCtx->priv_data, "preset", params_.preset.c_str(), 0); 

      encoder->avCdcCtx->extra_hw_frames = 8;
    } else {
      // Software Encoding 
      // Preset
      av_opt_set(encoder->avCdcCtx->priv_data, "preset", params_.preset.c_str(), 0); 
      av_opt_set(encoder->avCdcCtx->priv_data, "vprofile", params_.profile.c_str(), 0); 

      av_opt_set(encoder->avCdcCtx->priv_data, "tune", "ssim", 0); 
      
      // Quality
      if (type == VIDEO_ATTRIBUTE) {
        av_opt_set(encoder->avCdcCtx->priv_data, "qp", std::to_string(params_.attributeQP_).c_str(), 0);
      } else if (type == VIDEO_GEOMETRY) {
        av_opt_set(encoder->avCdcCtx->priv_data, "qp", std::to_string(params_.geometryQP_).c_str(), 0);
      } else {
        av_opt_set(encoder->avCdcCtx->priv_data, "qp", std::to_string(params_.occupancyMapQP_).c_str(), 0);
      }
    }
  }
  AVRational input_framerate = av_guess_frame_rate(decoder->avFmtCtx, decoder->avStrm, NULL);
  encoder->avCdcCtx->time_base = av_inv_q(input_framerate);
  encoder->avStrm->time_base = encoder->avCdcCtx->time_base;

  return 0;

}

void PCCTranscoder::addEndTile(PCCContext& context) {
  auto&         atglus              = context.getAtlasTileLayerList();
  for (auto& atglu : atglus){
    auto&         ath                = atglu.getHeader();
    auto&         atgdu              = atglu.getDataUnit();
    uint8_t patchType = static_cast<uint8_t>( ( ath.getType() == I_TILE ) ? I_END : P_END );
    atgdu.addPatchInformationData( patchType );
  }
}



void PCCTranscoder::setPointLocalReconstruction( PCCContext& context ) {
  auto& asps = context.getAtlasSequenceParameterSet( 0 );
  TRACE_PATCH( "PLR = %d \n", asps.getPLREnabledFlag() );
  PointLocalReconstructionMode mode = {false, false, 0, 1};
  context.addPointLocalReconstructionMode( mode );
  if ( asps.getPLREnabledFlag() ) {
    auto& plri = asps.getPLRInformation( 0 );
    for ( size_t i = 0; i < plri.getNumberOfModesMinus1(); i++ ) {
      mode.interpolate_ = plri.getInterpolateFlag( i );
      mode.filling_     = plri.getFillingFlag( i );
      mode.minD1_       = plri.getMinimumDepth( i );
      mode.neighbor_    = plri.getNeighbourMinus1( i ) + 1;
      context.addPointLocalReconstructionMode( mode );
    }
#ifdef CODEC_TRACE
    for ( size_t i = 0; i < context.getPointLocalReconstructionModeNumber(); i++ ) {
      auto& mode = context.getPointLocalReconstructionMode( i );
      TRACE_PATCH( "Plrm[%zu]: Inter = %d Fill = %d minD1 = %u neighbor = %u \n", i, mode.interpolate_, mode.filling_,
                   mode.minD1_, mode.neighbor_ );
    }
#endif
  }
}

void PCCTranscoder::setPLRData( PCCFrameContext& tile, PCCPatch& patch, PLRData& plrd, size_t occupancyPackingBlockSize ) {
  patch.allocOneLayerData();
  TRACE_PATCH( "WxH = %zu x %zu \n", plrd.getBlockToPatchMapWidth(), plrd.getBlockToPatchMapHeight() );
  patch.getPointLocalReconstructionLevel() = static_cast<uint8_t>( plrd.getLevelFlag() );
  TRACE_PATCH( "  LevelFlag = %d \n", plrd.getLevelFlag() );
  if ( plrd.getLevelFlag() ) {
    if ( plrd.getPresentFlag() ) {
      patch.setPointLocalReconstructionMode( plrd.getModeMinus1() + 1 );
    } else {
      patch.setPointLocalReconstructionMode( 0 );
    }
    TRACE_PATCH( "  ModePatch: Present = %d ModeMinus1 = %2d \n", plrd.getPresentFlag(),
                 plrd.getPresentFlag() ? (int32_t)plrd.getModeMinus1() : -1 );
  } else {
    for ( size_t v0 = 0; v0 < plrd.getBlockToPatchMapHeight(); ++v0 ) {
      for ( size_t u0 = 0; u0 < plrd.getBlockToPatchMapWidth(); ++u0 ) {
        size_t index = v0 * plrd.getBlockToPatchMapWidth() + u0;
        if ( plrd.getBlockPresentFlag( index ) ) {
          patch.setPointLocalReconstructionMode( u0, v0, plrd.getBlockModeMinus1( index ) + 1 );
        } else {
          patch.setPointLocalReconstructionMode( u0, v0, 0 );
        }
        TRACE_PATCH( "  Mode[%3u]: Present = %d ModeMinus1 = %2d \n", index, plrd.getBlockPresentFlag( index ),
                     plrd.getBlockPresentFlag( index ) ? (int32_t)plrd.getBlockModeMinus1( index ) : -1 );
      }
    }
  }
#ifdef CODEC_TRACE
  for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
    for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
      TRACE_PATCH(
          "Block[ %2lu %2lu <=> %4zu ] / [ %2lu %2lu ]: Level = %d Present = "
          "%d mode = %zu \n",
          u0, v0, v0 * patch.getSizeU0() + u0, patch.getSizeU0(), patch.getSizeV0(),
          patch.getPointLocalReconstructionLevel(), plrd.getBlockPresentFlag( v0 * patch.getSizeU0() + u0 ),
          patch.getPointLocalReconstructionMode( u0, v0 ) );
    }
  }
#endif
}


void PCCTranscoder::setPostProcessingSeiParameters( GeneratePointCloudParameters& params,
                                                 PCCContext&                   context,
                                                 size_t                        atglIndex ) {
  auto&   sps                   = context.getVps();
  int32_t atlasIndex            = 0;
  auto&   oi                    = sps.getOccupancyInformation( atlasIndex );
  auto&   gi                    = sps.getGeometryInformation( atlasIndex );
  auto&   asps                  = context.getAtlasSequenceParameterSet( 0 );
  auto&   plt                   = sps.getProfileTierLevel();
  params.flagGeometrySmoothing_ = false;
  params.gridSmoothing_         = false;
  params.gridSize_              = 0;
  params.thresholdSmoothing_    = 0;
  params.pbfEnableFlag_         = false;
  params.pbfPassesCount_        = 0;
  params.pbfFilterSize_         = 0;
  params.pbfLog2Threshold_      = 0;
  params.occupancyResolution_    = size_t( 1 ) << asps.getLog2PatchPackingBlockSize();
  params.occupancyPrecision_     = context.getOccupancyPrecision();
  params.enableSizeQuantization_ = context.getAtlasSequenceParameterSet( 0 ).getPatchSizeQuantizerPresentFlag();
  params.rawPointColorFormat_ =
      size_t( plt.getProfileCodecGroupIdc() == CODEC_GROUP_HEVC444 ? COLOURFORMAT444 : COLOURFORMAT420 );
  params.nbThread_   = params_.nbThread_;
  params.absoluteD1_ = sps.getMapCountMinus1( atlasIndex ) == 0 || sps.getMapAbsoluteCodingEnableFlag( atlasIndex, 1 );
  params.multipleStreams_          = sps.getMultipleMapStreamsPresentFlag( atlasIndex );
  params.surfaceThickness_         = asps.getAspsVpccExtension().getSurfaceThicknessMinus1() + 1;
  params.thresholdColorSmoothing_  = 0.;
  params.flagColorSmoothing_       = false;
  params.cgridSize_                = 0;
  params.thresholdColorDifference_ = 0;
  params.thresholdColorVariation_  = 0;
  params.thresholdLossyOM_ = static_cast<size_t>( oi.getLossyOccupancyCompressionThreshold() );
  params.mapCountMinus1_                = sps.getMapCountMinus1( atlasIndex );
  params.EOMFixBitCount_                = asps.getEomFixBitCountMinus1() + 1;
  params.geometry3dCoordinatesBitdepth_ = gi.getGeometry3dCoordinatesBitdepthMinus1() + 1;
  params.geometryBitDepth3D_            = gi.getGeometry3dCoordinatesBitdepthMinus1() + 1;
}

void PCCTranscoder::setGeneratePointCloudParameters( GeneratePointCloudParameters& params,
                                                  PCCContext&                   context,
                                                  size_t                        atglIndex ) {
  auto&   sps                   = context.getVps();
  int32_t atlasIndex            = 0;
  auto&   oi                    = sps.getOccupancyInformation( atlasIndex );
  auto&   gi                    = sps.getGeometryInformation( atlasIndex );
  auto&   asps                  = context.getAtlasSequenceParameterSet( 0 );
  auto&   plt                   = sps.getProfileTierLevel();
  params.flagGeometrySmoothing_ = false;
  params.gridSmoothing_         = false;
  params.gridSize_              = 0;
  params.thresholdSmoothing_    = 0;
  params.pbfEnableFlag_         = false;
  params.pbfPassesCount_        = 0;
  params.pbfFilterSize_         = 0;
  params.pbfLog2Threshold_      = 0;

  params.occupancyResolution_    = size_t( 1 ) << asps.getLog2PatchPackingBlockSize();
  params.occupancyPrecision_     = context.getOccupancyPrecision();
  params.enableSizeQuantization_ = context.getAtlasSequenceParameterSet( 0 ).getPatchSizeQuantizerPresentFlag();
  params.rawPointColorFormat_ =
      size_t( plt.getProfileCodecGroupIdc() == CODEC_GROUP_HEVC444 ? COLOURFORMAT444 : COLOURFORMAT420 );
  params.nbThread_   = params_.nbThread_;
  params.absoluteD1_ = sps.getMapCountMinus1( atlasIndex ) == 0 || sps.getMapAbsoluteCodingEnableFlag( atlasIndex, 1 );
  params.multipleStreams_          = sps.getMultipleMapStreamsPresentFlag( atlasIndex );
  params.surfaceThickness_         = asps.getAspsVpccExtension().getSurfaceThicknessMinus1() + 1;
  params.flagColorSmoothing_       = false;
  params.cgridSize_                = 0;
  params.thresholdColorSmoothing_  = 0.;
  params.thresholdColorDifference_ = 0;
  params.thresholdColorVariation_  = 0;
  params.thresholdLossyOM_ = static_cast<size_t>( oi.getLossyOccupancyCompressionThreshold() );
  params.mapCountMinus1_                = sps.getMapCountMinus1( atlasIndex );
  params.useAuxSeperateVideo_           = asps.getAuxiliaryVideoEnabledFlag();
  params.EOMFixBitCount_                = asps.getEomFixBitCountMinus1() + 1;
  params.geometry3dCoordinatesBitdepth_ = gi.getGeometry3dCoordinatesBitdepthMinus1() + 1;
  params.geometryBitDepth3D_            = gi.getGeometry3dCoordinatesBitdepthMinus1() + 1;
}

void PCCTranscoder::createPatchFrameDataStructure( PCCContext& context ) {
  TRACE_PATCH( "createPatchFrameDataStructure GOP start \n" );
  size_t frameCount = 0;
  auto&  atlList    = context.getAtlasTileLayerList();

  // partition information derivation
  setTilePartitionSizeAfti( context );
  for ( size_t i = 0; i < atlList.size(); i++ ) {
    size_t afocVal = context.calculateAFOCval( atlList, i );
    frameCount     = std::max( frameCount, ( afocVal + 1 ) );
    atlList[i].getHeader().setFrameIndex( afocVal );
  }
  context.resize( frameCount );
  setPointLocalReconstruction( context );
  for ( size_t atglIndex = 0; atglIndex < atlList.size(); atglIndex++ ) {
    auto& atgl = atlList[atglIndex];
    if ( atglIndex == 0 || atgl.getAtlasFrmOrderCntVal() != atlList[atglIndex - 1].getAtlasFrmOrderCntVal() ) {
      setTileSizeAndLocation( context, atgl.getHeader().getFrameIndex(), atgl.getHeader() );
    }
    auto&  atlu       = context.getAtlasTileLayer( atglIndex );
    auto&  ath        = atlu.getHeader();
    size_t frameIndex = ath.getFrameIndex();
    createPatchFrameDataStructure( context, atglIndex );

#ifdef CONFORMANCE_TRACE
    if ( atgl.getSEI().seiIsPresent( NAL_PREFIX_ESEI, GEOMETRY_SMOOTHING ) ) {
      auto* sei = static_cast<SEIGeometrySmoothing*>( atgl.getSEI().getSei( NAL_PREFIX_ESEI, GEOMETRY_SMOOTHING ) );
      auto& vec = sei->getMD5ByteStrData();
      if ( vec.size() > 0 ) {
        TRACE_HLS( "**********GEOMETRY_SMOOTHING_ESEI***********\n" );
        TRACE_HLS( "SEI%02dMD5 = ", sei->getPayloadType() );
        SEIMd5Checksum( context, vec );
      }
    }
    if ( atgl.getSEI().seiIsPresent( NAL_PREFIX_ESEI, OCCUPANCY_SYNTHESIS ) ) {
      auto* sei = static_cast<SEIOccupancySynthesis*>( atgl.getSEI().getSei( NAL_PREFIX_ESEI, OCCUPANCY_SYNTHESIS ) );
      auto& vec = sei->getMD5ByteStrData();
      if ( vec.size() > 0 ) {
        TRACE_HLS( "**********OCCUPANCY_SYNTHESIS_ESEI***********\n" );
        TRACE_HLS( "SEI%02dMD5 = ", sei->getPayloadType() );
        SEIMd5Checksum( context, vec );
      }
    }
    if ( atgl.getSEI().seiIsPresent( NAL_PREFIX_ESEI, ATTRIBUTE_SMOOTHING ) ) {
      auto* sei = static_cast<SEIAttributeSmoothing*>( atgl.getSEI().getSei( NAL_PREFIX_ESEI, ATTRIBUTE_SMOOTHING ) );
      auto& vec = sei->getMD5ByteStrData();
      if ( vec.size() > 0 ) {
        TRACE_HLS( "**********ATTRIBUTE_SMOOTHING_ESEI***********\n" );
        TRACE_HLS( "SEI%02dMD5 = ", sei->getPayloadType() );
        SEIMd5Checksum( context, vec );
      }
    }
    if ( atgl.getSEI().seiIsPresent( NAL_PREFIX_ESEI, COMPONENT_CODEC_MAPPING ) ) {
      auto* sei =
          static_cast<SEIOccupancySynthesis*>( atgl.getSEI().getSei( NAL_PREFIX_ESEI, COMPONENT_CODEC_MAPPING ) );
      auto& temp = sei->getMD5ByteStrData();
      if ( temp.size() > 0 ) {
        TRACE_HLS( "**********CODEC_COMPONENT_MAPPING_ESEI***********\n" );
        TRACE_HLS( "SEI%02dMD5 = ", sei->getPayloadType() );
        SEIMd5Checksum( context, temp );
      }
    }
#endif
    bool isLastTileOfTheFrames = atglIndex + 1 == atlList.size() ||
                                 atgl.getAtlasFrmOrderCntVal() != atlList[atglIndex + 1].getAtlasFrmOrderCntVal();
    if ( isLastTileOfTheFrames && atgl.getSEI().seiIsPresent( NAL_SUFFIX_NSEI, DECODED_ATLAS_INFORMATION_HASH ) ) {
      auto* sei = static_cast<SEIDecodedAtlasInformationHash*>(
          atgl.getSEI().getSei( NAL_SUFFIX_NSEI, DECODED_ATLAS_INFORMATION_HASH ) );
      TRACE_PATCH( "create Hash SEI \n" );
      auto& atlu = context.getAtlasTileLayer( atglIndex );
      auto& ath  = atlu.getHeader();
      createHashSEI( context, ath.getFrameIndex(), *sei );
    }
#ifdef CONFORMANCE_TRACE
    if ( isLastTileOfTheFrames ) { createHlsAtlasTileLogFiles( context, frameIndex ); }
#endif
  }
}

void PCCTranscoder::createPatchFrameDataStructure( PCCContext& context, size_t atglIndex ) {
  TRACE_PATCH( "createPatchFrameDataStructure Tile %zu \n", atglIndex );
  auto&  sps                = context.getVps();
  size_t atlasIndex         = context.getAtlasIndex();
  auto&  atlu               = context.getAtlasTileLayer( atglIndex );
  auto&  ath                = atlu.getHeader();
  auto&  afps               = context.getAtlasFrameParameterSet( ath.getAtlasFrameParameterSetId() );
  auto&  asps               = context.getAtlasSequenceParameterSet( afps.getAtlasSequenceParameterSetId() );
  auto&  afti               = afps.getAtlasFrameTileInformation();
  auto&  atgdu              = atlu.getDataUnit();
  auto   geometryBitDepth2D = asps.getGeometry2dBitdepthMinus1() + 1;
  auto   geometryBitDepth3D = asps.getGeometry3dBitdepthMinus1() + 1;
  size_t frameIndex         = ath.getFrameIndex();
  size_t tileIndex          = afti.getSignalledTileIdFlag() ? afti.getTileId( ath.getId() ) : ath.getId();

  printf( "createPatchFrameDataStructure Frame = %zu Tiles = %zu atlasIndex = %zu atglIndex %zu \n", frameIndex,
          tileIndex, context.getAtlasIndex(), atglIndex );
  fflush( stdout );
  PCCFrameContext& tile = context[frameIndex].getTile( tileIndex );
  tile.setFrameIndex( frameIndex );
  tile.setAtlasFrmOrderCntVal( atlu.getAtlasFrmOrderCntVal() );
  tile.setAtlasFrmOrderCntMsb( atlu.getAtlasFrmOrderCntMsb() );
  tile.setTileIndex( tileIndex );
  tile.setAtlIndex( atglIndex );
  tile.setUseRawPointsSeparateVideo( sps.getAuxiliaryVideoPresentFlag( atlasIndex ) &&
                                     asps.getAuxiliaryVideoEnabledFlag() );
  tile.setRawPatchEnabledFlag( asps.getRawPatchEnabledFlag() );
  if ( tile.getFrameIndex() > 0 && ath.getType() != I_TILE ) {
    tile.setRefAfocList( context, ath, ath.getAtlasFrameParameterSetId() );
    TRACE_PATCH( "\tframe[%zu]\tRefAfocList:", frameIndex );
    for ( size_t i = 0; i < tile.getRefAfocListSize(); i++ ) { TRACE_PATCH( "\t%zu", tile.getRefAfoc( i ) ); }
    TRACE_PATCH( "\n" );
  }

  // local variable initialization
  auto&        patches        = tile.getPatches();
  auto&        rawPatches     = tile.getRawPointsPatches();
  auto&        eomPatches     = tile.getEomPatches();
  int64_t      predIndex      = 0;
  const size_t minLevel       = pow( 2., ath.getPosMinDQuantizer() );
  size_t       numRawPatches  = 0;
  size_t       numNonRawPatch = 0;
  size_t       numEomPatch    = 0;
  PCCTileType  tileType       = ath.getType();
  size_t       patchCount     = atgdu.getPatchCount();
  for ( size_t i = 0; i < patchCount; i++ ) {
    PCCPatchType currPatchType = getPatchType( tileType, atgdu.getPatchMode( i ) );
    if ( currPatchType == RAW_PATCH ) {
      numRawPatches++;
    } else if ( currPatchType == EOM_PATCH ) {
      numEomPatch++;
    }
  }
  TRACE_PATCH( "Patches size                      = %zu \n", patches.size() );
  TRACE_PATCH( "non-regular Patches(raw, eom)     = %zu, %zu \n", numRawPatches, numEomPatch );
  TRACE_PATCH( "Tile Type                         = %zu (0.P_TILE 1.I_TILE 2.SKIP_TILE)\n", (size_t)ath.getType() );
  size_t  totalNumberOfRawPoints = 0;
  size_t  totalNumberOfEomPoints = 0;
  size_t  patchIndex             = 0;
  int32_t packingBlockSize       = 1 << asps.getLog2PatchPackingBlockSize();
  double  packingBlockSizeD      = static_cast<double>( packingBlockSize );
  int32_t quantizerSizeX         = 1 << ath.getPatchSizeXinfoQuantizer();
  int32_t quantizerSizeY         = 1 << ath.getPatchSizeYinfoQuantizer();
  tile.setLog2PatchQuantizerSizeX( ath.getPatchSizeXinfoQuantizer() );
  tile.setLog2PatchQuantizerSizeY( ath.getPatchSizeYinfoQuantizer() );
  for ( patchIndex = 0; patchIndex < patchCount; patchIndex++ ) {
    auto&        pid           = atgdu.getPatchInformationData( patchIndex );
    PCCPatchType currPatchType = getPatchType( tileType, atgdu.getPatchMode( patchIndex ) );
    if ( currPatchType == INTRA_PATCH ) {
      PCCPatch patch;
      auto&    pdu = pid.getPatchDataUnit();
      patch.setOccupancyResolution( size_t( 1 ) << asps.getLog2PatchPackingBlockSize() );
      patch.setU0( pdu.get2dPosX() );
      patch.setV0( pdu.get2dPosY() );
      patch.setU1( pdu.get3dOffsetU() );
      patch.setV1( pdu.get3dOffsetV() );
      bool lodEnableFlag = pdu.getLodEnableFlag();
      if ( lodEnableFlag ) {
        patch.setLodScaleX( pdu.getLodScaleXMinus1() + 1 );
        patch.setLodScaleYIdc( pdu.getLodScaleYIdc() + ( patch.getLodScaleX() > 1 ? 1 : 2 ) );
      } else {
        patch.setLodScaleX( 1 );
        patch.setLodScaleYIdc( 1 );
      }
      patch.setSizeD( pdu.get3dRangeD() == 0 ? 0 : ( pdu.get3dRangeD() * minLevel - 1 ) );
      if ( asps.getPatchSizeQuantizerPresentFlag() ) {
        patch.setPatchSize2DXInPixel( ( pdu.get2dSizeXMinus1() + 1 ) * quantizerSizeX );
        patch.setPatchSize2DYInPixel( ( pdu.get2dSizeYMinus1() + 1 ) * quantizerSizeY );
        patch.setSizeU0( ceil( static_cast<double>( patch.getPatchSize2DXInPixel() ) / packingBlockSizeD ) );
        patch.setSizeV0( ceil( static_cast<double>( patch.getPatchSize2DYInPixel() ) / packingBlockSizeD ) );
      } else {
        patch.setSizeU0( pdu.get2dSizeXMinus1() + 1 );
        patch.setSizeV0( pdu.get2dSizeYMinus1() + 1 );
      }
      patch.setPatchOrientation( pdu.getOrientationIndex() );
      patch.setViewId( pdu.getProjectionId() );
      TRACE_PATCH( "patch %zu / %zu: Intra \n", patchIndex, patchCount );
      const size_t max3DCoordinate = size_t( 1 ) << geometryBitDepth3D;
      if ( patch.getProjectionMode() == 0 ) {
        patch.setD1( static_cast<int32_t>( pdu.get3dOffsetD() ) * minLevel );
      } else {
        patch.setD1( max3DCoordinate - static_cast<int32_t>( pdu.get3dOffsetD() ) * minLevel );
      }
      if ( patch.getNormalAxis() == 0 ) {
        patch.setTangentAxis( 2 );
        patch.setBitangentAxis( 1 );
      } else if ( patch.getNormalAxis() == 1 ) {
        patch.setTangentAxis( 2 );
        patch.setBitangentAxis( 0 );
      } else {
        patch.setTangentAxis( 0 );
        patch.setBitangentAxis( 1 );
      }
      TRACE_PATCH(
          "patch(Intra) %zu: UV0 %4zu %4zu UV1 %4zu %4zu D1=%4zu S=%4zu %4zu %4zu(%4zu) P=%zu O=%zu A=%u%u%u Lod "
          "=(%zu) %zu,%zu 45=%d ProjId=%4zu Axis=%zu \n",
          patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
          patch.getSizeV0(), patch.getSizeD(), pdu.get3dRangeD(), patch.getProjectionMode(),
          patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(), patch.getBitangentAxis(),
          (size_t)lodEnableFlag, patch.getLodScaleX(), patch.getLodScaleY(), asps.getExtendedProjectionEnabledFlag(),
          pdu.getProjectionId(), patch.getAxisOfAdditionalPlane() );
      patch.allocOneLayerData();
      if ( asps.getPLREnabledFlag() ) {
        setPLRData( tile, patch, pdu.getPLRData(), size_t( 1 ) << asps.getLog2PatchPackingBlockSize() );
      }
      patches.push_back( patch );
    } else if ( currPatchType == INTER_PATCH ) {
      PCCPatch patch;
      patch.setOccupancyResolution( size_t( 1 ) << asps.getLog2PatchPackingBlockSize() );
      auto& ipdu = pid.getInterPatchDataUnit();
      TRACE_PATCH( "patch %zu / %zu: Inter \n", patchIndex, patchCount );
      TRACE_PATCH(
          "\tIPDU: refAtlasFrame= %d refPatchIdx = %d pos2DXY = %ld %ld pos3DXYZW = %ld %ld %ld %ld size2D = %ld %ld "
          "\n",
          ipdu.getRefIndex(), ipdu.getRefPatchIndex(), ipdu.get2dPosX(), ipdu.get2dPosY(), ipdu.get3dOffsetU(),
          ipdu.get3dOffsetV(), ipdu.get3dOffsetD(), ipdu.get3dRangeD(), ipdu.get2dDeltaSizeX(),
          ipdu.get2dDeltaSizeY() );
      patch.setBestMatchIdx( static_cast<int32_t>( ipdu.getRefPatchIndex() + predIndex ) );
      predIndex += ipdu.getRefPatchIndex() + 1;
      patch.setRefAtlasFrameIndex( ipdu.getRefIndex() );
      size_t      refPOC   = (size_t)tile.getRefAfoc( patch.getRefAtlasFrameIndex() );
      const auto& refPatch = context.getFrame( refPOC ).getTile( tileIndex ).getPatches()[patch.getBestMatchIdx()];
      TRACE_PATCH(
          "\trefPatch: refIndex = %zu, refFrame = %zu, Idx = %zu/%zu UV0 = %zu %zu  UV1 = %zu %zu Size = %zu %zu %zu "
          "Lod = %u,%u\n",
          patch.getRefAtlasFrameIndex(), refPOC, patch.getBestMatchIdx(),
          context.getFrame( refPOC ).getTile( tileIndex ).getPatches().size(), refPatch.getU0(), refPatch.getV0(),
          refPatch.getU1(), refPatch.getV1(), refPatch.getSizeU0(), refPatch.getSizeV0(), refPatch.getSizeD(),
          refPatch.getLodScaleX(), refPatch.getLodScaleY() );
      patch.setProjectionMode( refPatch.getProjectionMode() );
      patch.setViewId( refPatch.getViewId() );
      patch.setU0( ipdu.get2dPosX() + refPatch.getU0() );
      patch.setV0( ipdu.get2dPosY() + refPatch.getV0() );
      patch.setPatchOrientation( refPatch.getPatchOrientation() );
      patch.setU1( ipdu.get3dOffsetU() + refPatch.getU1() );
      patch.setV1( ipdu.get3dOffsetV() + refPatch.getV1() );
      if ( asps.getPatchSizeQuantizerPresentFlag() ) {
        patch.setPatchSize2DXInPixel( refPatch.getPatchSize2DXInPixel() + ( ipdu.get2dDeltaSizeX() ) * quantizerSizeX );
        patch.setPatchSize2DYInPixel( refPatch.getPatchSize2DYInPixel() + ( ipdu.get2dDeltaSizeY() ) * quantizerSizeY );
        patch.setSizeU0( ceil( static_cast<double>( patch.getPatchSize2DXInPixel() ) / packingBlockSizeD ) );
        patch.setSizeV0( ceil( static_cast<double>( patch.getPatchSize2DYInPixel() ) / packingBlockSizeD ) );
      } else {
        patch.setSizeU0( ipdu.get2dDeltaSizeX() + refPatch.getSizeU0() );
        patch.setSizeV0( ipdu.get2dDeltaSizeY() + refPatch.getSizeV0() );
      }
      patch.setNormalAxis( refPatch.getNormalAxis() );
      patch.setTangentAxis( refPatch.getTangentAxis() );
      patch.setBitangentAxis( refPatch.getBitangentAxis() );
      patch.setAxisOfAdditionalPlane( refPatch.getAxisOfAdditionalPlane() );
      const size_t max3DCoordinate = size_t( 1 ) << geometryBitDepth3D;
      if ( patch.getProjectionMode() == 0 ) {
        patch.setD1( ( ipdu.get3dOffsetD() + ( refPatch.getD1() / minLevel ) ) * minLevel );
      } else {
        patch.setD1( max3DCoordinate -
                     ( ipdu.get3dOffsetD() + ( ( max3DCoordinate - refPatch.getD1() ) / minLevel ) ) * minLevel );
      }
      const int64_t delta_DD = ipdu.get3dRangeD() == 0 ? 0 : ( ipdu.get3dRangeD() * minLevel - 1 );
      patch.setSizeD( refPatch.getSizeD() + delta_DD );
      patch.setLodScaleX( refPatch.getLodScaleX() );
      patch.setLodScaleYIdc( refPatch.getLodScaleY() );
      TRACE_PATCH(
          "\tpatch(Inter) %zu: UV0 %4zu %4zu UV1 %4zu %4zu D1=%4zu S=%4zu %4zu %4zu from DeltaSize = %4ld %4ld P=%zu "
          "O=%zu A=%u%u%u Lod = %zu,%zu \n",
          patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
          patch.getSizeV0(), patch.getSizeD(), ipdu.get2dDeltaSizeX(), ipdu.get2dDeltaSizeY(),
          patch.getProjectionMode(), patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(),
          patch.getBitangentAxis(), patch.getLodScaleX(), patch.getLodScaleY() );

      patch.allocOneLayerData();
      if ( asps.getPLREnabledFlag() ) {
        setPLRData( tile, patch, ipdu.getPLRData(), size_t( 1 ) << asps.getLog2PatchPackingBlockSize() );
      }
      patches.push_back( patch );
    } else if ( currPatchType == MERGE_PATCH ) {
      assert( -2 );
      PCCPatch patch;
      patch.setOccupancyResolution( size_t( 1 ) << asps.getLog2PatchPackingBlockSize() );
      auto&        mpdu            = pid.getMergePatchDataUnit();
      bool         overridePlrFlag = false;
      const size_t max3DCoordinate = size_t( 1 ) << geometryBitDepth3D;
      TRACE_PATCH( "patch %zu / %zu: Inter \n", patchIndex, patchCount );
      TRACE_PATCH(
          "MPDU: refAtlasFrame= %d refPatchIdx = ?? pos2DXY = %ld %ld pos3DXYZW = %ld %ld %ld %ld size2D = %ld %ld \n",
          mpdu.getRefIndex(), mpdu.get2dPosX(), mpdu.get2dPosY(), mpdu.get3dOffsetU(), mpdu.get3dOffsetV(),
          mpdu.get3dOffsetD(), mpdu.get3dRangeD(), mpdu.get2dDeltaSizeX(), mpdu.get2dDeltaSizeY() );

      patch.setBestMatchIdx( patchIndex );
      predIndex = patchIndex;
      patch.setRefAtlasFrameIndex( mpdu.getRefIndex() );
      size_t      refPOC   = (size_t)tile.getRefAfoc( patch.getRefAtlasFrameIndex() );
      const auto& refPatch = context.getFrame( refPOC ).getTile( tileIndex ).getPatches()[patch.getBestMatchIdx()];
      if ( mpdu.getOverride2dParamsFlag() ) {
        patch.setU0( mpdu.get2dPosX() + refPatch.getU0() );
        patch.setV0( mpdu.get2dPosY() + refPatch.getV0() );
        if ( asps.getPatchSizeQuantizerPresentFlag() ) {
          patch.setPatchSize2DXInPixel( refPatch.getPatchSize2DXInPixel() + mpdu.get2dDeltaSizeX() * quantizerSizeX );
          patch.setPatchSize2DYInPixel( refPatch.getPatchSize2DYInPixel() + mpdu.get2dDeltaSizeY() * quantizerSizeY );
          patch.setSizeU0( ceil( static_cast<double>( patch.getPatchSize2DXInPixel() ) / packingBlockSizeD ) );
          patch.setSizeV0( ceil( static_cast<double>( patch.getPatchSize2DYInPixel() ) / packingBlockSizeD ) );
        } else {
          patch.setSizeU0( mpdu.get2dDeltaSizeX() + refPatch.getSizeU0() );
          patch.setSizeV0( mpdu.get2dDeltaSizeY() + refPatch.getSizeV0() );
        }

        if ( asps.getPLREnabledFlag() ) { overridePlrFlag = true; }
      } else {
        if ( mpdu.getOverride3dParamsFlag() ) {
          patch.setU1( mpdu.get3dOffsetU() + refPatch.getU1() );
          patch.setV1( mpdu.get3dOffsetV() + refPatch.getV1() );
          if ( patch.getProjectionMode() == 0 ) {
            patch.setD1( ( mpdu.get3dOffsetD() + ( refPatch.getD1() / minLevel ) ) * minLevel );
          } else {
            patch.setD1( max3DCoordinate -
                         ( mpdu.get3dOffsetD() + ( ( max3DCoordinate - refPatch.getD1() ) / minLevel ) ) * minLevel );
          }
          const int64_t delta_DD = mpdu.get3dRangeD() == 0 ? 0 : ( mpdu.get3dRangeD() * minLevel - 1 );
          patch.setSizeD( refPatch.getSizeD() + delta_DD );
          if ( asps.getPLREnabledFlag() ) { overridePlrFlag = ( mpdu.getOverridePlrFlag() != 0 ); }
        }
      }
      patch.setProjectionMode( refPatch.getProjectionMode() );
      patch.setViewId( refPatch.getViewId() );
      patch.setPatchOrientation( refPatch.getPatchOrientation() );
      patch.setNormalAxis( refPatch.getNormalAxis() );
      patch.setTangentAxis( refPatch.getTangentAxis() );
      patch.setBitangentAxis( refPatch.getBitangentAxis() );
      patch.setAxisOfAdditionalPlane( refPatch.getAxisOfAdditionalPlane() );
      patch.setLodScaleX( refPatch.getLodScaleX() );
      patch.setLodScaleYIdc( refPatch.getLodScaleY() );
      TRACE_PATCH(
          "patch(Inter) %zu: UV0 %4zu %4zu UV1 %4zu %4zu D1=%4zu S=%4zu %4zu %4zu from DeltaSize = %4ld %4ld P=%zu "
          "O=%zu A=%u%u%u Lod = %zu,%zu \n",
          patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
          patch.getSizeV0(), patch.getSizeD(), mpdu.get2dDeltaSizeX(), mpdu.get2dDeltaSizeY(),
          patch.getProjectionMode(), patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(),
          patch.getBitangentAxis(), patch.getLodScaleX(), patch.getLodScaleY() );

      patch.allocOneLayerData();
      if ( asps.getPLREnabledFlag() ) {
        setPLRData( tile, patch, mpdu.getPLRData(), size_t( 1 ) << asps.getLog2PatchPackingBlockSize() );
      }
      patches.push_back( patch );
    } else if ( currPatchType == SKIP_PATCH ) {
      assert( -1 );
      PCCPatch patch;
      TRACE_PATCH( "patch %zu / %zu: Inter \n", patchIndex, patchCount );
      TRACE_PATCH( "SDU: refAtlasFrame= 0 refPatchIdx = %d \n", patchIndex );
      patch.setBestMatchIdx( static_cast<int32_t>( patchIndex ) );
      predIndex += patchIndex;
      patch.setRefAtlasFrameIndex( 0 );
      size_t      refPOC   = (size_t)tile.getRefAfoc( patch.getRefAtlasFrameIndex() );
      const auto& refPatch = context.getFrame( refPOC ).getTile( tileIndex ).getPatches()[patch.getBestMatchIdx()];
      TRACE_PATCH( "\trefPatch: Idx = %zu UV0 = %zu %zu  UV1 = %zu %zu Size = %zu %zu %zu  Lod = %u,%u \n",
                   patch.getBestMatchIdx(), refPatch.getU0(), refPatch.getV0(), refPatch.getU1(), refPatch.getV1(),
                   refPatch.getSizeU0(), refPatch.getSizeV0(), refPatch.getSizeD(), refPatch.getLodScaleX(),
                   refPatch.getLodScaleY() );
      patch.setProjectionMode( refPatch.getProjectionMode() );
      patch.setViewId( refPatch.getViewId() );
      patch.setU0( refPatch.getU0() );
      patch.setV0( refPatch.getV0() );
      patch.setPatchOrientation( refPatch.getPatchOrientation() );
      patch.setU1( refPatch.getU1() );
      patch.setV1( refPatch.getV1() );
      if ( asps.getPatchSizeQuantizerPresentFlag() ) {
        patch.setPatchSize2DXInPixel( refPatch.getPatchSize2DXInPixel() );
        patch.setPatchSize2DYInPixel( refPatch.getPatchSize2DYInPixel() );
      }
      patch.setSizeU0( refPatch.getSizeU0() );
      patch.setSizeV0( refPatch.getSizeV0() );
      patch.setNormalAxis( refPatch.getNormalAxis() );
      patch.setTangentAxis( refPatch.getTangentAxis() );
      patch.setBitangentAxis( refPatch.getBitangentAxis() );
      patch.setAxisOfAdditionalPlane( refPatch.getAxisOfAdditionalPlane() );
      patch.setD1( refPatch.getD1() );
      patch.setSizeD( refPatch.getSizeD() );
      patch.setLodScaleX( refPatch.getLodScaleX() );
      patch.setLodScaleYIdc( refPatch.getLodScaleY() );
      TRACE_PATCH(
          "patch(skip) %zu: UV0 %4zu %4zu UV1 %4zu %4zu D1=%4zu S=%4zu %4zu %4zu P=%zu O=%zu A=%u%u%u Lod = %zu,%zu \n",
          patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
          patch.getSizeV0(), patch.getSizeD(), patch.getProjectionMode(), patch.getPatchOrientation(),
          patch.getNormalAxis(), patch.getTangentAxis(), patch.getBitangentAxis(), patch.getLodScaleX(),
          patch.getLodScaleY() );
      patch.allocOneLayerData();
      patches.push_back( patch );
    } else if ( currPatchType == RAW_PATCH ) {
      TRACE_PATCH( "patch %zu / %zu: raw \n", patchIndex, patchCount );
      auto&             rpdu = pid.getRawPatchDataUnit();
      PCCRawPointsPatch rawPointsPatch;
      rawPointsPatch.isPatchInAuxVideo_ = rpdu.getPatchInAuxiliaryVideoFlag();
      rawPointsPatch.u0_                = rpdu.get2dPosX();
      rawPointsPatch.v0_                = rpdu.get2dPosY();
      rawPointsPatch.sizeU0_            = rpdu.get2dSizeXMinus1() + 1;
      rawPointsPatch.sizeV0_            = rpdu.get2dSizeYMinus1() + 1;
      if ( afps.getRaw3dOffsetBitCountExplicitModeFlag() ) {
        rawPointsPatch.u1_ = rpdu.get3dOffsetU();
        rawPointsPatch.v1_ = rpdu.get3dOffsetV();
        rawPointsPatch.d1_ = rpdu.get3dOffsetD();
      } else {
        const size_t pcmU1V1D1Level = size_t( 1 ) << geometryBitDepth2D;
        rawPointsPatch.u1_          = rpdu.get3dOffsetU() * pcmU1V1D1Level;
        rawPointsPatch.v1_          = rpdu.get3dOffsetV() * pcmU1V1D1Level;
        rawPointsPatch.d1_          = rpdu.get3dOffsetD() * pcmU1V1D1Level;
      }
      rawPointsPatch.setNumberOfRawPoints( rpdu.getRawPointsMinus1() + 1 );
      rawPointsPatch.occupancyResolution_ = size_t( 1 ) << asps.getLog2PatchPackingBlockSize();
      totalNumberOfRawPoints += rawPointsPatch.getNumberOfRawPoints();
      rawPatches.push_back( rawPointsPatch );
      TRACE_PATCH( "Raw :UV = %zu %zu  size = %zu %zu  uvd1 = %zu %zu %zu numPoints = %zu ocmRes = %zu \n",
                   rawPointsPatch.u0_, rawPointsPatch.v0_, rawPointsPatch.sizeU0_, rawPointsPatch.sizeV0_,
                   rawPointsPatch.u1_, rawPointsPatch.v1_, rawPointsPatch.d1_, rawPointsPatch.numberOfRawPoints_,
                   rawPointsPatch.occupancyResolution_ );
    } else if ( currPatchType == EOM_PATCH ) {
      TRACE_PATCH( "patch %zu / %zu: EOM \n", patchIndex, patchCount );
      auto&       epdu       = pid.getEomPatchDataUnit();
      auto&       eomPatches = tile.getEomPatches();
      PCCEomPatch eomPatch;
      eomPatch.isPatchInAuxVideo_ = epdu.getPatchInAuxiliaryVideoFlag();
      eomPatch.u0_                = epdu.get2dPosX();
      eomPatch.v0_                = epdu.get2dPosY();
      eomPatch.sizeU_             = epdu.get2dSizeXMinus1() + 1;
      eomPatch.sizeV_             = epdu.get2dSizeYMinus1() + 1;
      eomPatch.memberPatches_.resize( epdu.getPatchCountMinus1() + 1 );
      eomPatch.eomCountPerPatch_.resize( epdu.getPatchCountMinus1() + 1 );
      eomPatch.eomCount_ = 0;
      for ( size_t i = 0; i < eomPatch.memberPatches_.size(); i++ ) {
        eomPatch.memberPatches_[i]    = epdu.getAssociatedPatchesIdx( i );
        eomPatch.eomCountPerPatch_[i] = epdu.getPoints( i );
        eomPatch.eomCount_ += eomPatch.eomCountPerPatch_[i];
      }
      eomPatch.occupancyResolution_ = size_t( 1 ) << asps.getLog2PatchPackingBlockSize();
      eomPatches.push_back( eomPatch );
      totalNumberOfEomPoints += eomPatch.eomCount_;
      TRACE_PATCH( "EOM: U0V0 %zu,%zu\tSizeU0V0 %zu,%zu\tN= %zu,%zu\n", eomPatch.u0_, eomPatch.v0_, eomPatch.sizeU_,
                   eomPatch.sizeV_, eomPatch.memberPatches_.size(), eomPatch.eomCount_ );
      for ( size_t i = 0; i < eomPatch.memberPatches_.size(); i++ ) {
        TRACE_PATCH( "%zu, %zu\n", eomPatch.memberPatches_[i], eomPatch.eomCountPerPatch_[i] );
      }
      TRACE_PATCH( "\n" );
    } else if ( currPatchType == END_PATCH ) {
      break;
    } else {
      std::printf( "Error: unknow frame/patch type \n" );
      TRACE_PATCH( "Error: unknow frame/patch type \n" );
    }
  }
  TRACE_PATCH( "patch %zu / %zu: end \n", patches.size(), patches.size() );
  tile.setTotalNumberOfRawPoints( totalNumberOfRawPoints );
  tile.setTotalNumberOfEOMPoints( totalNumberOfEomPoints );
}

bool PCCTranscoder::compareHashSEIMD5( std::vector<uint8_t>& encMD5, std::vector<uint8_t>& decMD5 ) {
  bool equal = true;
  for ( size_t i = 0; i < 16; i++ ) {
    if ( encMD5[i] != decMD5[i] ) {
      equal = false;
      break;
    }
  }
  for ( auto& e : encMD5 ) TRACE_SEI( "%02x", e );
  TRACE_SEI( ", " );
  for ( auto& d : decMD5 ) TRACE_SEI( "%02x", d );
  return equal;
}
bool PCCTranscoder::compareHashSEICrc( uint16_t encCrc, uint16_t decCrc ) {
  bool equal = true;
  if ( encCrc != decCrc ) equal = false;
  TRACE_SEI( "%04x", encCrc );
  TRACE_SEI( ", " );
  TRACE_SEI( "%04x", decCrc );
  return equal;
}

bool PCCTranscoder::compareHashSEICheckSum( uint32_t encCheckSum, uint32_t decCheckSum ) {
  bool equal = true;
  if ( encCheckSum != decCheckSum ) equal = false;
  TRACE_SEI( "%08x", encCheckSum );
  TRACE_SEI( ", " );
  TRACE_SEI( "%08x", decCheckSum );
  return equal;
}

void PCCTranscoder::createHashSEI( PCCContext& context, int frameIndex, SEIDecodedAtlasInformationHash& sei ) {
  bool                                           seiHashCancelFlag = sei.getCancelFlag();
  std::vector<PatchParams>                       atlasPatchParams;
  std::vector<std::vector<PatchParams>>          tilePatchParams;
  std::vector<std::vector<std::vector<int64_t>>> tileB2PPatchParams;
  std::vector<std::vector<int64_t>>              atlasB2PPatchParams;
  TRACE_SEI( "*** Hash SEI Frame (%d) ***\n", frameIndex );

  if ( !seiHashCancelFlag && sei.getDecodedHighLevelHashPresentFlag() ) {
    size_t               atlIdx     = context[frameIndex].getTile( 0 ).getAtlIndex();
    auto&                tileHeader = context.getAtlasTileLayerList()[atlIdx].getHeader();
    size_t               afpsIndex  = tileHeader.getAtlasFrameParameterSetId();
    size_t               aspsIndex  = context.getAtlasFrameParameterSet( afpsIndex ).getAtlasSequenceParameterSetId();
    auto&                asps       = context.getAtlasSequenceParameterSet( aspsIndex );
    auto&                afps       = context.getAtlasFrameParameterSet( afpsIndex );
    std::vector<uint8_t> highLevelAtlasData;
    aspsCommonByteString( highLevelAtlasData, asps );
    aspsApplicationByteString( highLevelAtlasData, asps, afps );
    afpsCommonByteString( highLevelAtlasData, context, afpsIndex, frameIndex );
    afpsApplicationByteString( highLevelAtlasData, asps, afps );

    if ( sei.getHashType() == 0 ) {
      std::vector<uint8_t> encMD5( 16 ), decMD5( 16 );
      encMD5 = context.computeMD5( highLevelAtlasData.data(), highLevelAtlasData.size() );
      TRACE_SEI( " Derived (MD5) = " );
      for ( int j = 0; j < 16; j++ ) {
        decMD5[j] = sei.getHighLevelMd5( j );
        TRACE_SEI( "%02x", encMD5[j] );
      }
      TRACE_SEI( "\t Derived vs. SEI (MD5) : " );
      TRACE_SEI( "HLS MD5: " );
      bool equal = compareHashSEIMD5( encMD5, decMD5 );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    } else if ( sei.getHashType() == 1 ) {
      uint16_t crc = context.computeCRC( highLevelAtlasData.data(), highLevelAtlasData.size() );
      TRACE_SEI( " Derived (CRC): %d ", crc );
      TRACE_SEI( "HLS CRC: " );
      bool equal = compareHashSEICrc( crc, sei.getHighLevelCrc() );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    } else if ( sei.getHashType() == 2 ) {
      uint32_t checkSum = context.computeCheckSum( highLevelAtlasData.data(), highLevelAtlasData.size() );
      TRACE_SEI( " Derived (CheckSum): %d ", checkSum );
      TRACE_SEI( "HLS CheckSum: " );
      bool equal = compareHashSEICheckSum( checkSum, sei.getHighLevelCheckSum() );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    }
    highLevelAtlasData.clear();
  }

  if ( !seiHashCancelFlag && ( sei.getDecodedAtlasTilesHashPresentFlag() || sei.getDecodedAtlasHashPresentFlag() ) ) {
    size_t numTilesInPatchFrame = context[frameIndex].getNumTilesInAtlasFrame();
    if ( sei.getDecodedAtlasTilesHashPresentFlag() ) { tilePatchParams.resize( numTilesInPatchFrame ); }
    for ( size_t tileIdx = 0; tileIdx < numTilesInPatchFrame; tileIdx++ ) {
      getHashPatchParams( context, frameIndex, tileIdx, tilePatchParams, atlasPatchParams );
    }
  }
  if ( !seiHashCancelFlag &&
       ( sei.getDecodedAtlasB2pHashPresentFlag() || sei.getDecodedAtlasTilesB2pHashPresentFlag() ) ) {
    getB2PHashPatchParams( context, frameIndex, tileB2PPatchParams, atlasB2PPatchParams );
  }
  // frame
  TRACE_SEI( "\n" );
  if ( !seiHashCancelFlag && sei.getDecodedAtlasHashPresentFlag() ) {
    std::vector<uint8_t> atlasData;
    size_t               patchCount = atlasPatchParams.size();
    for ( size_t patchIdx = 0; patchIdx < patchCount; patchIdx++ ) {
      atlasPatchCommonByteString( atlasData, patchIdx, atlasPatchParams );
      atlasPatchApplicationByteString( atlasData, patchIdx, atlasPatchParams );
    }
    printf( "AtlasPatchHash: frame(%d) (#patches %zu)\n", frameIndex, patchCount );

    if ( sei.getHashType() == 0 ) {
      std::vector<uint8_t> encMD5( 16 ), decMD5( 16 );
      encMD5 = context.computeMD5( atlasData.data(), atlasData.size() );
      TRACE_SEI( " Derived Atlas MD5 = " );
      TRACE_SEI( "Atlas MD5: " );
      for ( int j = 0; j < 16; j++ ) {
        decMD5[j] = sei.getAtlasMd5( j );
        TRACE_SEI( "%02x", encMD5[j] );
      }
      TRACE_SEI( "\n\t**sei** (MD5): " );
      bool equal = compareHashSEIMD5( encMD5, decMD5 );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    } else if ( sei.getHashType() == 1 ) {
      uint16_t crc = context.computeCRC( atlasData.data(), atlasData.size() );
      TRACE_SEI( "\n Derived (CRC): %d", crc );
      TRACE_SEI( "Atlas CRC: " );
      bool equal = compareHashSEICrc( crc, sei.getAtlasCrc() );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    } else if ( sei.getHashType() == 2 ) {
      uint32_t checkSum = context.computeCheckSum( atlasData.data(), atlasData.size() );
      TRACE_SEI( "\n Derived (CheckSum): %d", checkSum );
      TRACE_SEI( "Atlas CheckSum: " );
      bool equal = compareHashSEICheckSum( checkSum, sei.getAtlasCheckSum() );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    }
    atlasData.clear();
  }
  if ( sei.getDecodedAtlasB2pHashPresentFlag() && !seiHashCancelFlag ) {
    std::vector<uint8_t> atlasB2PData;
    atlasBlockToPatchByteString( atlasB2PData, atlasB2PPatchParams );

    TRACE_SEI( "**sei** AtlasBlockToPatchHash: frame(%d)", frameIndex );
    if ( sei.getHashType() == 0 ) {
      bool                 equal = true;
      std::vector<uint8_t> encMD5( 16 ), decMD5( 16 );
      encMD5 = context.computeMD5( atlasB2PData.data(), atlasB2PData.size() );
      TRACE_SEI( " Derived Atlas B2P MD5 = " );
      TRACE_SEI( "Atlas B2P MD5: " );
      for ( int j = 0; j < 16; j++ ) {
        decMD5[j] = sei.getAtlasB2pMd5( j );
        TRACE_SEI( "%02x", encMD5[j] );
      }
      equal = compareHashSEIMD5( encMD5, decMD5 );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    } else if ( sei.getHashType() == 1 ) {
      uint16_t crc = context.computeCRC( atlasB2PData.data(), atlasB2PData.size() );
      TRACE_SEI( "\n Derived (CRC): %d ", crc );
      TRACE_SEI( "Atlas B2P CRC: " );
      bool equal = compareHashSEICrc( crc, sei.getAtlasB2pCrc() );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    } else if ( sei.getHashType() == 2 ) {
      uint32_t checkSum = context.computeCheckSum( atlasB2PData.data(), atlasB2PData.size() );
      TRACE_SEI( "\n Derived (CheckSum): %d ", checkSum );
      TRACE_SEI( "Atlas B2P CheckSum: " );
      bool equal = compareHashSEICheckSum( checkSum, sei.getAtlasB2pCheckSum() );
      TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
    }
    atlasB2PData.clear();
    TRACE_SEI( "\n" );
  }

  // for tiles
  if ( !seiHashCancelFlag && sei.getDecodedAtlasTilesHashPresentFlag() ||
       sei.getDecodedAtlasTilesB2pHashPresentFlag() ) {
    size_t numTilesInPatchFrame = context[frameIndex].getNumTilesInAtlasFrame();
    printf( "**sei** AtlasTilesHash: frame(%d) (#Tiles %zu)", frameIndex, numTilesInPatchFrame );
    for ( size_t tileIdx = 0; tileIdx < numTilesInPatchFrame; tileIdx++ ) {
      auto&       tile     = context[frameIndex].getTile( tileIdx );
      auto&       atlu     = context.getAtlasTileLayer( tile.getAtlIndex() );
      auto&       ath      = atlu.getHeader();
      size_t      tileId   = ath.getId();
      PCCTileType tileType = ath.getType();
      auto&       afps     = context.getAtlasFrameParameterSet( ath.getAtlasFrameParameterSetId() );
      auto&       afti     = afps.getAtlasFrameTileInformation();
      if ( sei.getDecodedAtlasTilesHashPresentFlag() ) {
        std::vector<uint8_t> atlasTileData;
        for ( size_t patchIdx = 0; patchIdx < atlu.getDataUnit().getPatchCount(); patchIdx++ ) {
          tilePatchCommonByteString( atlasTileData, tileId, patchIdx, tilePatchParams );
          tilePatchApplicationByteString( atlasTileData, tileId, patchIdx, tilePatchParams );
        }
        printf( "**sei** TilesPatchHash: frame(%d), tile(tileIdx %zu, tileId %zu)\n", frameIndex, tileIdx, tileId );
        if ( sei.getHashType() == 0 ) {
          std::vector<uint8_t> encMD5( 16 ), decMD5( 16 );
          encMD5 = context.computeMD5( atlasTileData.data(), atlasTileData.size() );
          TRACE_SEI( " Derived Tile MD5 = " );
          TRACE_SEI( "Tile( id = %d, idx = %d ) MD5: ", tileId, tileIdx );
          for ( int j = 0; j < 16; j++ ) {
            decMD5[j] = sei.getAtlasTilesMd5( tileId, j );
            TRACE_SEI( "%02x", encMD5[j] );
          }
          bool equal = compareHashSEIMD5( encMD5, decMD5 );
          TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
        } else if ( sei.getHashType() == 1 ) {
          uint16_t crc = context.computeCRC( atlasTileData.data(), atlasTileData.size() );
          // TRACE_SEI( "\n Derived  (CRC): %d ", crc );
          TRACE_SEI( "Tile( id = %d, idx = %d ) CRC: ", tileId, tileIdx );
          bool equal = compareHashSEICrc( crc, sei.getAtlasTilesCrc( tileId ) );
          TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
        } else if ( sei.getHashType() == 2 ) {
          uint32_t checkSum = context.computeCheckSum( atlasTileData.data(), atlasTileData.size() );
          TRACE_SEI( "\n Derived CheckSum: %d ", checkSum );
          TRACE_SEI( "Tile( id = %d, idx = %d ) CheckSum: ", tileId, tileIdx );
          bool equal = compareHashSEICheckSum( checkSum, sei.getAtlasTilesCheckSum( tileId ) );
          TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
        }
        atlasTileData.clear();
      }
      if ( sei.getDecodedAtlasTilesB2pHashPresentFlag() ) {
        std::vector<uint8_t> tileB2PData;
        tileBlockToPatchByteString( tileB2PData, tileId, tileB2PPatchParams );
        printf( "\n**sei** TilesBlockToPatchHash: frame(%d), tile(tileIdx %zu, tileId %zu)", frameIndex, tileIdx,
                tileId );
        if ( sei.getHashType() == 0 ) {
          std::vector<uint8_t> encMD5( 16 ), decMD5( 16 );
          encMD5 = context.computeMD5( tileB2PData.data(), tileB2PData.size() );
          TRACE_SEI( " Derived Tile B2P MD5 = " );
          TRACE_SEI( "Tile B2P( id = %d, idx = %d ) MD5: ", tileId, tileIdx );
          for ( int j = 0; j < 16; j++ ) {
            decMD5[j] = sei.getAtlasTilesB2pMd5( tileId, j );
            TRACE_SEI( "%02x", encMD5[j] );
          }
          bool equal = compareHashSEIMD5( encMD5, decMD5 );
          TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
        } else if ( sei.getHashType() == 1 ) {
          uint16_t crc = context.computeCRC( tileB2PData.data(), tileB2PData.size() );
          TRACE_SEI( "\n Derived Tile B2P CRC: %d ", crc );
          TRACE_SEI( "Tile B2P( id = %d, idx = %d ) CRC: ", tileId, tileIdx );
          bool equal = compareHashSEICrc( crc, sei.getAtlasTilesB2pCrc( tileId ) );
          TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
        } else if ( sei.getHashType() == 2 ) {
          uint32_t checkSum = context.computeCheckSum( tileB2PData.data(), tileB2PData.size() );
          TRACE_SEI( "\n Derived Tile B2P CheckSum: %d ", checkSum );
          TRACE_SEI( "Tile( id = %d, idx = %d ) CheckSum: ", tileId, tileIdx );
          bool equal = compareHashSEICheckSum( checkSum, sei.getAtlasTilesB2pCheckSum( tileId ) );
          TRACE_SEI( " (%s) \n", equal ? "OK" : "DIFF" );
        }
        tileB2PData.clear();
      }
      TRACE_SEI( "\n" );
    }  // tileIdx
  }

  if ( atlasPatchParams.size() != 0 ) atlasPatchParams.clear();
  if ( tilePatchParams.size() != 0 ) {
    for ( size_t ti = 0; ti < tilePatchParams.size(); ti++ )
      if ( tilePatchParams[ti].size() != 0 ) tilePatchParams[ti].clear();
  }
  tilePatchParams.clear();
  for ( auto& e : atlasB2PPatchParams ) e.clear();
  atlasB2PPatchParams.clear();
  for ( auto& e : tileB2PPatchParams ) {
    for ( auto d : e ) d.clear();
    e.clear();
  }
  tileB2PPatchParams.clear();

#ifdef CONFORMANCE_TRACE
  auto& temp = sei.getMD5ByteStrData();
  if ( temp.size() > 0 ) {  // ajt:: An example of how to generate md5 checksum for hash SEI message - could be computed
                            // different ways!
    TRACE_HLS( "**********DECODED_ATLAS_INFORMATION_HASH_NSEI***********\n" );
    TRACE_HLS( "SEI%02dMD5 = ", sei.getPayloadType() );
    SEIMd5Checksum( context, temp );
  }
#endif
}

void PCCTranscoder::createHlsAtlasTileLogFiles( PCCContext& context, int frameIndex ) {
  size_t atlIdx     = context[frameIndex].getTile( 0 ).getAtlIndex();
  auto&  tileHeader = context.getAtlasTileLayerList()[atlIdx].getHeader();
  auto&  atlu       = context.getAtlasTileLayer( atlIdx );
  size_t afpsIndex  = tileHeader.getAtlasFrameParameterSetId();
  size_t aspsIndex  = context.getAtlasFrameParameterSet( afpsIndex ).getAtlasSequenceParameterSetId();
  auto&  asps       = context.getAtlasSequenceParameterSet( aspsIndex );
  auto&  afps       = context.getAtlasFrameParameterSet( afpsIndex );
  auto&  vps        = context.getVps();

  TRACE_HLS( "AtlasFrameIndex = %d\n", frameIndex );
  /*TRACE_HLS( "Atlas Frame Parameter Set Index = %d\n", afpsIndex );*/
  std::vector<uint8_t> decMD5( 16 );
  std::vector<uint8_t> highLevelAtlasData;
  aspsCommonByteString( highLevelAtlasData, asps );
  /*decMD5 = context.computeMD5( highLevelAtlasData.data(), highLevelAtlasData.size() );
  TRACE_HLS( " HLSMD5 = " );
  for ( int j = 0; j < 16; j++ ) TRACE_HLS( "%02x", decMD5[j] );
  TRACE_HLS( "\n" );*/
  aspsApplicationByteString( highLevelAtlasData, asps, afps );
  /*decMD5 = context.computeMD5( highLevelAtlasData.data(), highLevelAtlasData.size() );
  TRACE_HLS( " HLSMD5 = " );
  for ( int j = 0; j < 16; j++ ) TRACE_HLS( "%02x", decMD5[j] );
  TRACE_HLS( "\n" );*/
  afpsCommonByteString( highLevelAtlasData, context, afpsIndex, frameIndex );
  /*decMD5 = context.computeMD5( highLevelAtlasData.data(), highLevelAtlasData.size() );
  TRACE_HLS( " HLSMD5 = " );
  for ( int j = 0; j < 16; j++ ) TRACE_HLS( "%02x", decMD5[j] );
  TRACE_HLS( "\n" );*/
  afpsApplicationByteString( highLevelAtlasData, asps, afps );
  decMD5 = context.computeMD5( highLevelAtlasData.data(), highLevelAtlasData.size() );
  TRACE_HLS( "HLSMD5 = " );
  for ( int j = 0; j < 16; j++ ) TRACE_HLS( "%02x", decMD5[j] );
  TRACE_HLS( "\n" );
  highLevelAtlasData.clear();

  std::vector<PatchParams>                       atlasPatchParams;
  std::vector<std::vector<PatchParams>>          tilePatchParams;
  std::vector<std::vector<std::vector<int64_t>>> tileB2PPatchParams;
  std::vector<std::vector<int64_t>>              atlasB2PPatchParams;
  size_t                                         numTilesInPatchFrame = context[frameIndex].getNumTilesInAtlasFrame();
  tilePatchParams.resize( numTilesInPatchFrame );
  for ( size_t tileIdx = 0; tileIdx < numTilesInPatchFrame; tileIdx++ ) {
    getHashPatchParams( context, frameIndex, tileIdx, tilePatchParams, atlasPatchParams );
  }
  getB2PHashPatchParams( context, frameIndex, tileB2PPatchParams, atlasB2PPatchParams );
  // frame
  size_t numProjPatches = 0, numRawPatches = 0, numEomPatches = 0;
  size_t numProjPoints = 0, numRawPoints = 0, numEomPoints = 0;
  int    atlasFrameOrderCnt = atlu.getAtlasFrmOrderCntVal();
  for ( size_t tileIdx = 0; tileIdx < numTilesInPatchFrame; tileIdx++ ) {
    auto& tile = context[frameIndex].getTile( tileIdx );
    numProjPatches += tile.getPatches().size();
    numEomPatches += tile.getEomPatches().size();
    numRawPatches += tile.getRawPointsPatches().size();
    numProjPoints += tile.getTotalNumberOfRegularPoints();
    numEomPoints += tile.getTotalNumberOfEOMPoints();
    numRawPoints += tile.getTotalNumberOfRawPoints();
  }
  TRACE_ATLAS( "AtlasFrameIndex = %d\n", frameIndex );
  TRACE_ATLAS(
      "AtlasFrameOrderCntVal = %d,  AtlasFrameWidthMax =  %d, AtlasFrameHeightMax = %d, AtlasID = %d, "
      "ASPSFrameSize = %d, VPSMapCount = %d, AttributeCount = %d, AttributeDimension = %d, NumTilesAtlasFrame = %d, "
      "AtlasTotalNumProjPatches = %d, AtlasTotalNumRawPatches = %d, AtlasTotalNumEomPatches = %d, ",
      atlasFrameOrderCnt, asps.getFrameWidth(), asps.getFrameHeight(), vps.getAtlasId( 0 ),
      asps.getFrameWidth() * asps.getFrameHeight(), vps.getMapCountMinus1( 0 ) + 1,
      vps.getAttributeInformation( 0 ).getAttributeCount(),
      vps.getAttributeInformation( 0 ).getAttributeCount() > 0
          ? vps.getAttributeInformation( 0 ).getAttributeDimensionMinus1( 0 ) + 1
          : 0,
      afps.getAtlasFrameTileInformation().getNumTilesInAtlasFrameMinus1() + 1, numProjPatches, numRawPatches,
      numEomPatches );
  std::vector<uint8_t> atlasData;
  size_t               patchCount = atlasPatchParams.size();
  for ( size_t patchIdx = 0; patchIdx < patchCount; patchIdx++ ) {
    atlasPatchCommonByteString( atlasData, patchIdx, atlasPatchParams );
    atlasPatchApplicationByteString( atlasData, patchIdx, atlasPatchParams );
  }
  decMD5 = context.computeMD5( atlasData.data(), atlasData.size() );
  TRACE_ATLAS( " Atlas MD5 = " );
  for ( int j = 0; j < 16; j++ ) { TRACE_ATLAS( "%02x", decMD5[j] ); }
  TRACE_ATLAS( "," );
  atlasData.clear();
  std::vector<uint8_t> atlasB2PData;
  atlasBlockToPatchByteString( atlasB2PData, atlasB2PPatchParams );
  decMD5 = context.computeMD5( atlasB2PData.data(), atlasB2PData.size() );
  TRACE_ATLAS( " Atlas B2P MD5 = " );
  for ( int j = 0; j < 16; j++ ) TRACE_ATLAS( "%02x", decMD5[j] );
  atlasB2PData.clear();
  TRACE_ATLAS( "\n" );

  // for tiles
  TRACE_TILE( "AtlasFrameIndex = %d\n", frameIndex );
  for ( size_t tileIdx = 0; tileIdx < numTilesInPatchFrame; tileIdx++ ) {
    auto&       tile          = context[frameIndex].getTile( tileIdx );
    auto&       atlu          = context.getAtlasTileLayer( tile.getAtlIndex() );  // ajt::why atlIdx?
    auto&       ath           = atlu.getHeader();
    size_t      tileId        = ath.getId();
    PCCTileType tileType      = ath.getType();
    auto&       afps          = context.getAtlasFrameParameterSet( ath.getAtlasFrameParameterSetId() );
    auto&       afti          = afps.getAtlasFrameTileInformation();
    size_t      topLeftColumn = afti.getTopLeftPartitionIdx( tileIdx ) % ( afti.getNumPartitionColumnsMinus1() + 1 );
    size_t      topLeftRow    = afti.getTopLeftPartitionIdx( tileIdx ) / ( afti.getNumPartitionColumnsMinus1() + 1 );
    size_t      tileOffsetX   = context[frameIndex].getPartitionPosX( topLeftColumn );
    size_t      tileOffsetY   = context[frameIndex].getPartitionPosY( topLeftRow );
    TRACE_TILE(
        "TileID = %d, AtlasFrameOrderCntVal = %d, TileType = %d, TileOffsetX = %d, TileOffsetY = %d, TileWidth = %d, "
        "TileHeight = %d, ",
        tileId, ath.getAtlasFrmOrderCntLsb(), tileType, tileOffsetX, tileOffsetY, tile.getWidth(), tile.getHeight() );
    std::vector<uint8_t> atlasTileData;
    for ( size_t patchIdx = 0; patchIdx < atlu.getDataUnit().getPatchCount(); patchIdx++ ) {
      tilePatchCommonByteString( atlasTileData, tileId, patchIdx, tilePatchParams );
      tilePatchApplicationByteString( atlasTileData, tileId, patchIdx, tilePatchParams );
    }
    decMD5 = context.computeMD5( atlasTileData.data(), atlasTileData.size() );
    TRACE_TILE( " Tile MD5 = " );
    for ( int j = 0; j < 16; j++ ) TRACE_TILE( "%02x", decMD5[j] );
    TRACE_TILE( "," );
    atlasTileData.clear();
    std::vector<uint8_t> tileB2PData;
    tileBlockToPatchByteString( tileB2PData, tileId, tileB2PPatchParams );
    decMD5 = context.computeMD5( tileB2PData.data(), tileB2PData.size() );
    TRACE_TILE( " Tile B2P MD5 = " );
    for ( int j = 0; j < 16; j++ ) TRACE_TILE( "%02x", decMD5[j] );
    tileB2PData.clear();
    TRACE_TILE( "\n" );
  }  // tileIdx

  if ( atlasPatchParams.size() != 0 ) { atlasPatchParams.clear(); }
  if ( tilePatchParams.size() != 0 ) {
    for ( size_t ti = 0; ti < tilePatchParams.size(); ti++ ) {
      if ( tilePatchParams[ti].size() != 0 ) { tilePatchParams[ti].clear(); }
    }
  }
  tilePatchParams.clear();
  for ( auto& e : atlasB2PPatchParams ) { e.clear(); }
  atlasB2PPatchParams.clear();
  for ( auto& e : tileB2PPatchParams ) {
    for ( auto d : e ) { d.clear(); }
    e.clear();
  }
  tileB2PPatchParams.clear();
}

void PCCTranscoder::setTilePartitionSizeAfti( PCCContext& context ) {  // decoder

  for ( size_t afpsIdx = 0; afpsIdx < context.getAtlasFrameParameterSetList().size(); afpsIdx++ ) {
    auto&  afps             = context.getAtlasFrameParameterSet( afpsIdx );
    auto&  asps             = context.getAtlasSequenceParameterSet( afps.getAtlasSequenceParameterSetId() );
    auto&  afti             = afps.getAtlasFrameTileInformation();
    size_t frameWidth       = asps.getFrameWidth();
    size_t frameHeight      = asps.getFrameHeight();
    size_t numPartitionCols = afti.getNumPartitionColumnsMinus1() + 1;
    size_t numPartitionRows = afti.getNumPartitionRowsMinus1() + 1;
    auto&  partitionWidth   = afti.getPartitionWidth();  // ajt::should be
    auto&  partitionHeight  = afti.getPartitionHeight();
    auto&  partitionPosX    = afti.getPartitionPosX();
    auto&  partitionPosY    = afti.getPartitionPosY();
    partitionWidth.resize( numPartitionCols );
    partitionHeight.resize( numPartitionRows );
    partitionPosX.resize( numPartitionCols );
    partitionPosY.resize( numPartitionRows );

    if ( afti.getSingleTileInAtlasFrameFlag() ) {
      partitionWidth[0]  = asps.getFrameWidth();
      partitionHeight[0] = asps.getFrameHeight();
      partitionPosX[0]   = 0;
      partitionPosY[0]   = 0;
    } else {
      if ( afti.getUniformPartitionSpacingFlag() ) {
        size_t uniformPatitionWidth  = 64 * ( afti.getPartitionColumnWidthMinus1( 0 ) + 1 );
        size_t uniformPatitionHeight = 64 * ( afti.getPartitionRowHeightMinus1( 0 ) + 1 );
        partitionPosX[0]             = 0;
        partitionWidth[0]            = uniformPatitionWidth;
        for ( size_t col = 1; col < numPartitionCols - 1; col++ ) {
          partitionPosX[col]  = partitionPosX[col - 1] + partitionWidth[col - 1];
          partitionWidth[col] = uniformPatitionWidth;
        }
        if ( numPartitionCols > 1 ) {
          partitionPosX[numPartitionCols - 1] =
              partitionPosX[numPartitionCols - 2] + partitionWidth[numPartitionCols - 2];
          partitionWidth[numPartitionCols - 1] = frameWidth - partitionPosX[numPartitionCols - 1];
        }

        partitionPosY[0]   = 0;
        partitionHeight[0] = uniformPatitionHeight;
        for ( size_t row = 1; row < numPartitionRows - 1; row++ ) {
          partitionPosY[row]   = partitionPosY[row - 1] + partitionHeight[row - 1];
          partitionHeight[row] = uniformPatitionHeight;
        }
        if ( numPartitionRows > 1 ) {
          partitionPosY[numPartitionRows - 1] =
              partitionPosY[numPartitionRows - 2] + partitionHeight[numPartitionRows - 2];
          partitionHeight[numPartitionRows - 1] = frameHeight - partitionPosY[numPartitionRows - 1];
        }
      } else {
        partitionPosX[0]  = 0;
        partitionWidth[0] = 64 * ( afti.getPartitionColumnWidthMinus1( 0 ) + 1 );
        for ( size_t col = 1; col < numPartitionCols - 1; col++ ) {
          partitionPosX[col]  = partitionPosX[col - 1] + partitionWidth[col - 1];
          partitionWidth[col] = 64 * ( afti.getPartitionColumnWidthMinus1( col ) + 1 );
        }
        if ( numPartitionCols > 1 ) {
          partitionPosX[numPartitionCols - 1] =
              partitionPosX[numPartitionCols - 2] + partitionWidth[numPartitionCols - 2];
          partitionWidth[numPartitionCols - 1] = frameWidth - partitionPosX[numPartitionCols - 1];
        }

        partitionPosY[0]   = 0;
        partitionHeight[0] = 64 * ( afti.getPartitionRowHeightMinus1( 0 ) + 1 );
        for ( size_t row = 1; row < numPartitionRows - 1; row++ ) {
          partitionPosY[row]   = partitionPosY[row - 1] + partitionHeight[row - 1];
          partitionHeight[row] = 64 * ( afti.getPartitionRowHeightMinus1( row ) + 1 );
        }
        if ( numPartitionRows > 1 ) {
          partitionPosY[numPartitionRows - 1] =
              partitionPosY[numPartitionRows - 2] + partitionHeight[numPartitionRows - 2];
          partitionHeight[numPartitionRows - 1] = frameHeight - partitionPosY[numPartitionRows - 1];
        }
      }
    }
  }  // afpsIdx
}

size_t PCCTranscoder::setTileSizeAndLocation( PCCContext& context, size_t frameIndex, AtlasTileHeader& ath ) {  // decoder
  size_t afpsIdx   = ath.getAtlasFrameParameterSetId();
  auto&  afps      = context.getAtlasFrameParameterSet( afpsIdx );
  auto&  asps      = context.getAtlasSequenceParameterSet( afps.getAtlasSequenceParameterSetId() );
  auto&  afti      = afps.getAtlasFrameTileInformation();
  size_t tileIndex = 0;
  printf( "setTileSizeAndLocation frameIndex = %zu \n", frameIndex );
  fflush( stdout );
  // this is for the hash functions
  context[frameIndex].getPartitionPosX().clear();
  context[frameIndex].getPartitionPosY().clear();
  context[frameIndex].getPartitionWidth().clear();
  context[frameIndex].getPartitionHeight().clear();
  for ( auto v : afti.getPartitionWidth() ) { context[frameIndex].getPartitionWidth().push_back( v ); }
  for ( auto v : afti.getPartitionHeight() ) { context[frameIndex].getPartitionHeight().push_back( v ); }
  for ( auto v : afti.getPartitionPosX() ) { context[frameIndex].getPartitionPosX().push_back( v ); }
  for ( auto v : afti.getPartitionPosY() ) { context[frameIndex].getPartitionPosY().push_back( v ); }

  if ( afti.getSingleTileInAtlasFrameFlag() ) {
    if ( afti.getNumTilesInAtlasFrameMinus1() == 0 ) {
      context[frameIndex].setAtlasFrameWidth( asps.getFrameWidth() );
      context[frameIndex].setAtlasFrameHeight( asps.getFrameHeight() );
      context[frameIndex].setNumTilesInAtlasFrame( 1 );
    } else {
      assert( context[frameIndex].getAtlasFrameWidth() == ( asps.getFrameWidth() ) );
      assert( context[frameIndex].getAtlasFrameHeight() == ( asps.getFrameHeight() ) );
      assert( context[frameIndex].getNumTilesInAtlasFrame() == 1 );
    }
    auto& tile = context[frameIndex].getTile( 0 );
    tile.setTileIndex( tileIndex );
    tile.setLeftTopXInFrame( 0 );
    tile.setLeftTopYInFrame( 0 );
    tile.setWidth( asps.getFrameWidth() );
    tile.setHeight( asps.getFrameHeight() );
  } else {
    context[frameIndex].setAtlasFrameWidth( asps.getFrameWidth() );
    context[frameIndex].setAtlasFrameHeight( asps.getFrameHeight() );
    context[frameIndex].setNumTilesInAtlasFrame( afti.getNumTilesInAtlasFrameMinus1() + 1 );

    context[frameIndex].initNumTiles( context[frameIndex].getNumTilesInAtlasFrame() );
    for ( size_t tileId = 0; tileId <= afti.getNumTilesInAtlasFrameMinus1(); tileId++ ) {  // tileId = ath.getId()
      tileIndex   = afti.getSignalledTileIdFlag() ? afti.getTileId( tileId ) : tileId;  // ajt:: tileId vs. tileIndex?
      auto&  tile = context[frameIndex].getTile( tileIndex );
      size_t TopLeftPartitionColumn =
          afti.getTopLeftPartitionIdx( tileIndex ) % ( afti.getNumPartitionColumnsMinus1() + 1 );
      size_t TopLeftPartitionRow =
          afti.getTopLeftPartitionIdx( tileIndex ) / ( afti.getNumPartitionColumnsMinus1() + 1 );
      size_t BottomRightPartitionColumn =
          TopLeftPartitionColumn + afti.getBottomRightPartitionColumnOffset( tileIndex );
      size_t BottomRightPartitionRow = TopLeftPartitionRow + afti.getBottomRightPartitionRowOffset( tileIndex );

      size_t tileStartX = afti.getPartitionPosX( TopLeftPartitionColumn );
      size_t tileStartY = afti.getPartitionPosY( TopLeftPartitionRow );
      size_t tileWidth  = 0;
      size_t tileHeight = 0;
      for ( size_t j = TopLeftPartitionColumn; j <= BottomRightPartitionColumn; j++ ) {
        tileWidth += ( afti.getPartitionWidth( j ) );
      }
      for ( size_t j = TopLeftPartitionRow; j <= BottomRightPartitionRow; j++ ) {
        tileHeight += ( afti.getPartitionHeight( j ) );
      }
      tile.setLeftTopXInFrame( tileStartX );
      tile.setLeftTopYInFrame( tileStartY );

      if ( ( tile.getLeftTopXInFrame() + tileWidth ) > context[frameIndex].getAtlasFrameWidth() )
        tileWidth = context[0].getAtlasFrameWidth() - tile.getLeftTopXInFrame();
      if ( ( tile.getLeftTopYInFrame() + tileHeight ) > context[frameIndex].getAtlasFrameHeight() )
        tileHeight = context[0].getAtlasFrameHeight() - tile.getLeftTopYInFrame();

      tile.setWidth( tileWidth );
      tile.setHeight( tileHeight );

      assert( tile.getLeftTopXInFrame() < asps.getFrameWidth() );
      assert( tile.getLeftTopYInFrame() < asps.getFrameHeight() );

      auto& atlasFrame = context[frameIndex];
      printf( "dec:%zu frame %zu tile:(%zu,%zu), %zux%zu -> leftIdx(%zu,%zu), bottom(%zu,%zu) -> %u,%u,%u\n",
              frameIndex, tileIndex, atlasFrame.getTile( tileIndex ).getLeftTopXInFrame(),
              atlasFrame.getTile( tileIndex ).getLeftTopYInFrame(), atlasFrame.getTile( tileIndex ).getWidth(),
              atlasFrame.getTile( tileIndex ).getHeight(), TopLeftPartitionColumn, TopLeftPartitionRow,
              BottomRightPartitionColumn, BottomRightPartitionRow, afti.getTopLeftPartitionIdx( tileIndex ),
              afti.getBottomRightPartitionColumnOffset( tileIndex ),
              afti.getBottomRightPartitionRowOffset( tileIndex ) );
    }
  }

  if ( asps.getAuxiliaryVideoEnabledFlag() ) {
    context[frameIndex].setAuxVideoWidth( ( afti.getAuxiliaryVideoTileRowWidthMinus1() + 1 ) * 64 );
    context[frameIndex].resizeAuxTileLeftTopY( afti.getNumTilesInAtlasFrameMinus1() + 1, 0 );
    context[frameIndex].resizeAuxTileHeight( afti.getNumTilesInAtlasFrameMinus1() + 1, 0 );
    for ( size_t ti = 0; ti <= afti.getNumTilesInAtlasFrameMinus1(); ti++ ) {
      context[frameIndex].setAuxTileHeight( ti, afti.getAuxiliaryVideoTileRowHeight( ti ) * 64 );
      if ( ti < afti.getNumTilesInAtlasFrameMinus1() )
        context[frameIndex].setAuxTileLeftTopY(
            ti + 1, context[frameIndex].getAuxTileLeftTopY( ti ) + context[frameIndex].getAuxTileHeight( ti ) );
    }
    for ( size_t ti = 0; ti <= afti.getNumTilesInAtlasFrameMinus1(); ti++ ) {
      auto& atlasFrame = context[frameIndex];
      printf( "decAux:%zu frame %zu tile:(%zu,%zu), %zux%zu\n", frameIndex, ti, size_t( 0 ),
              atlasFrame.getAuxTileLeftTopY( ti ), atlasFrame.getAuxVideoWidth(), atlasFrame.getAuxTileHeight( ti ) );
    }
  }
  return tileIndex;
}

void PCCTranscoder::setConsitantFourCCCode( PCCContext& context, size_t atglIndex ) {
  if ( context.seiIsPresentInReceivedData( NAL_PREFIX_ESEI, COMPONENT_CODEC_MAPPING, atglIndex ) ) {
    auto* sei = static_cast<SEIComponentCodecMapping*>(
        context.getSeiInReceivedData( NAL_PREFIX_ESEI, COMPONENT_CODEC_MAPPING, atglIndex ) );
    consitantFourCCCode_.resize( 256, std::string( "" ) );
    for ( size_t i = 0; i <= sei->getCodecMappingsCountMinus1(); i++ ) {
      auto codecId                  = sei->getCodecId( i );
      consitantFourCCCode_[codecId] = sei->getCodec4cc( codecId );
      printf( "setConsitantFourCCCode: codecId = %3u  fourCCCode = %s \n", codecId,
              consitantFourCCCode_[codecId].c_str() );
    }
  }
}

PCCCodecId PCCTranscoder::getCodedCodecId( PCCContext&        context,
                                        const uint8_t      codecCodecId,
                                        const std::string& videoDecoderPath ) {
  auto& sps = context.getVps();
  auto& plt = sps.getProfileTierLevel();
  printf( "getCodedCodecId profileCodecGroupIdc = %d codecCodecId = %u \n", plt.getProfileCodecGroupIdc(),
          codecCodecId );
  fflush( stdout );
  switch ( plt.getProfileCodecGroupIdc() ) {
    case CODEC_GROUP_AVC_PROGRESSIVE_HIGH:
#if defined( USE_JMAPP_VIDEO_CODEC ) && defined( USE_JMLIB_VIDEO_CODEC )
      return videoDecoderPath.empty() ? JMLIB : JMAPP;
#elif defined( USE_JMLIB_VIDEO_CODEC )
      return JMLIB;
#elif defined( USE_JMAPP_VIDEO_CODEC )
      if ( videoDecoderPath.empty() ) {
        fprintf( stderr, "video decoder path not set and JMAPP video codec is used \n" );
        exit( -1 );
      }
      return JMAPP;
#else
      fprintf( stderr, "JM Codec not supported \n" );
      exit( -1 );
#endif
      break;
    case CODEC_GROUP_HEVC_MAIN10:
    case CODEC_GROUP_HEVC444:
#if defined( USE_HMAPP_VIDEO_CODEC ) && defined( USE_HMLIB_VIDEO_CODEC )
      return videoDecoderPath.empty() ? HMLIB : HMAPP;
#elif defined( USE_HMLIB_VIDEO_CODEC )
      return HMLIB;
#elif defined( USE_HMAPP_VIDEO_CODEC )
      if ( videoDecoderPath.empty() ) {
        fprintf( stderr, "video decoder path not set and HMAPP video codec is used \n" );
        exit( -1 );
      }
      return HMAPP;
#else
      fprintf( stderr, "HM Codec not supported \n" );
      exit( -1 );
#endif
      break;
    case CODEC_GROUP_VVC_MAIN10:
#if defined( USE_VTMLIB_VIDEO_CODEC )
      return VTMLIB;
#else
      fprintf( stderr, "VTM Codec not supported \n" );
      exit( -1 );
#endif
      break;
    case CODEC_GROUP_MP4RA:
      if ( consitantFourCCCode_.size() > codecCodecId && !consitantFourCCCode_[codecCodecId].empty() ) {
        std::string codec4cc = consitantFourCCCode_[codecCodecId];
        printf( "=> codecId = %u => codec4cc = %s \n", codecCodecId, codec4cc.c_str() );
        if ( codec4cc.compare( "avc3" ) == 0 ) {
#if defined( USE_JMAPP_VIDEO_CODEC ) && defined( USE_JMLIB_VIDEO_CODEC )
          return videoDecoderPath.empty() ? JMLIB : JMAPP;
#elif defined( USE_JMLIB_VIDEO_CODEC )
          return JMLIB;
#elif defined( USE_JMAPP_VIDEO_CODEC )
          if ( videoDecoderPath.empty() ) {
            fprintf( stderr, "video decoder path not set and JMAPP video codec is used \n" );
            exit( -1 );
          }
          return JMAPP;
#else
          fprintf( stderr, "JM Codec not supported \n" );
          exit( -1 );
#endif
        } else if ( codec4cc.compare( "hev1" ) == 0 ) {
#if defined( USE_HMAPP_VIDEO_CODEC ) && defined( USE_HMLIB_VIDEO_CODEC )
          return videoDecoderPath.empty() ? HMLIB : HMAPP;
#elif defined( USE_HMLIB_VIDEO_CODEC )
          return HMLIB;
#elif defined( USE_HMAPP_VIDEO_CODEC )
          if ( videoDecoderPath.empty() ) {
            fprintf( stderr, "video decoder path not set and HMAPP video codec is used \n" );
            exit( -1 );
          }
          return HMAPP;
#else
          fprintf( stderr, "HM Codec not supported \n" );
          exit( -1 );
#endif
        } else if ( codec4cc.compare( "svc1" ) == 0 ) {
#if defined( USE_SHMAPP_VIDEO_CODEC )
          if ( videoDecoderPath.empty() ) {
            fprintf( stderr, "video decoder path not set and SHMAPP video codec is used \n" );
            exit( -1 );
          }
          return SHMAPP;
#else
          fprintf( stderr, "SHM Codec not supported \n" );
          exit( -1 );
#endif
        } else if ( codec4cc.compare( "vvi1" ) == 0 ) {
#if defined( USE_VTMLIB_VIDEO_CODEC )
          return VTMLIB;
#else
          fprintf( stderr, "VTM Codec not supported \n" );
          exit( -1 );
#endif
        } else {
          fprintf( stderr, "CODEC_GROUP_MP4RA but codec4cc \"%s\" not supported \n", codec4cc.c_str() );
          exit( -1 );
        }
      } else {
        fprintf( stderr, "CODEC_GROUP_MP4RA but component codec mapping SEI not present or codec index = %u not set \n",
                 codecCodecId );
        exit( -1 );
      }
      break;
    default:
      fprintf( stderr, "ProfileCodecGroupIdc = %d not supported \n", plt.getProfileCodecGroupIdc() );
      exit( -1 );
      break;
  }
  return PCCCodecId::UNKNOWN_CODEC;
}
