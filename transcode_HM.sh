./bin/PccAppTranscoder \
	--compressedStreamPath=./longdress_r5.bin \
	--outStreamPath=transcoded.bin \
	--configurationFolder=cfg/ \
	--config=cfg/condition/ctc-all-intra.cfg \
	--geometryQP=16 \
    --attributeQP=22 \
    --occupancyPrecision=4 \
    --transcodeBaseline
./bin/PccAppDecoder \
	--compressedStreamPath=transcoded.bin \
    --uncompressedDataFolder=../../data/ \
    --computeMetrics \
    --startFrameNumber=1051 \
	--inverseColorSpaceConversionConfig=cfg/hdrconvert/yuv420torgb444.cfg \
	--reconstructedDataPath=S26C03R03_dec_%04d.ply 
./bin/PccAppMetrics \
  --uncompressedDataPath=../../data/8i/8iVFBv2/longdress/Ply/longdress_vox10_%04d.ply \
  --reconstructedDataPath=S26C03R03_dec_%04d.ply \
  --startFrameNumber=1051 \
  --resolution=1023 \
  --frameCount=30
python avg_metrics.py tmp.txt 30
