./bin/PccAppEncoder \
	--configurationFolder=cfg/ \
	--config=cfg/common/ctc-common.cfg \
	--config=cfg/condition/ctc-all-intra.cfg \
	--config=cfg/sequence/longdress_vox10.cfg \
	--config=cfg/rate/ctc-r5.cfg \
	--uncompressedDataFolder=../../data/8iVFBv2/ \
	--uncompressedDataPath=longdress/Ply/longdress_vox10_%04d.ply \
	--nbThread=16 \
	--frameCount=10 \
        --startFrameNumber=1051 \
	--reconstructedDataPath=./data/rec_%04d.ply \
	--compressedStreamPath=./data/longdress_r5.bin 
./bin/PccAppTranscoder \
     	--compressedStreamPath=./data/longdress_r5.bin \
    --outStreamPath=transcoded.bin \
    --test_name=test_transcode \
    --preset=veryfast \
    --pixelFormat=yuv420p \
    --geometryQP=32 \
    --attributeQP=42 \
    --profile=high \
    --occupancyPrecision=2 \
    --rate_mode=qp
./bin/PccAppDecoder \
	--compressedStreamPath=transcoded.bin \
    --uncompressedDataFolder=../../data/ \
    --computeMetrics \
    --startFrameNumber=1051 \
	--inverseColorSpaceConversionConfig=cfg/hdrconvert/yuv420torgb444.cfg \
	--reconstructedDataPath=S26C03R03_dec_%04d.ply 
./bin/PccAppMetrics \
  --uncompressedDataPath=../../data/8iVFBv2/longdress/Ply/longdress_vox10_%04d.ply \
  --reconstructedDataPath=S26C03R03_dec_%04d.ply \
  --startFrameNumber=1051 \
  --resolution=1023 \
  --frameCount=30
