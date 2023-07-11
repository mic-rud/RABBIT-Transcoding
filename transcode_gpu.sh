ts=$(date +%s%N)  
./bin/PccAppTranscoder \
	--compressedStreamPath=./data/longdress_r5.bin \
	--outStreamPath=transcoded.bin \
    --test_name=test_transcode \
    --qualityValGeo=25 \
    --qualityValAtt=22 \
    --preset=veryfast \
    --pixelFormat=yuv420p \
    --profile=main \
    --rate_mode=crp \
    --useCuda
echo "SW transcode: $((($(date +%s%N) - $ts)/1000000)) ms" >> times.txt
./bin/PccAppDecoder \
	--compressedStreamPath=transcoded.bin \
    --uncompressedDataFolder=../../data/ \
    --computeMetrics \
    --startFrameNumber=1051 \
	--inverseColorSpaceConversionConfig=cfg/hdrconvert/yuv420torgb444.cfg \
	--reconstructedDataPath=S26C03R03_dec_%04d.ply 
./bin/PccAppMetrics \
  --uncompressedDataPath=../data/longdress/Ply/longdress_vox10_%04d.ply \
  --reconstructedDataPath=S26C03R03_dec_%04d.ply \
  --startFrameNumber=1051 \
  --resolution=1023 \
  --frameCount=300
python avg_metrics.py test 300
