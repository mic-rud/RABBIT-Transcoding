./bin/PccAppDecoder \
	--compressedStreamPath=transcoded.bin \
    --uncompressedDataFolder=../../data/ \
    --computeMetrics \
    --startFrameNumber=1051 \
	--inverseColorSpaceConversionConfig=cfg/hdrconvert/yuv420torgb444.cfg \
	--reconstructedDataPath=S26C03R03_dec_%04d.ply 
