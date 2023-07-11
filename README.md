# RABBIT: Live Transcoding of V-PCC Point Cloud Streams

Accompaning repository to the [paper](https://dl.acm.org/doi/10.1145/3587819.3590978) **RABBIT: Live Transcoding of V-PCC Point Cloud Streams**, presented at MMSys'23: 14th Conference on ACM Multimedia Systems.

The modification of the V-PCC codec in this repository allows to transcode bitstreams, encoded with V-PCC in the V3C bitstream format.

This repository is based on [mpeg-tmc2](https://github.com/MPEGGroup/mpeg-pcc-tmc2). You can find detailed documentation on the V-PCC codec here.
Configurations are designed for the [8i VFB v2](http://plenodb.jpeg.org/pc/8ilabs/) dataset.

## Building
The repository was built and tested on Ubuntu 22.04 using gcc/g++-9.

Install libav dependencies:

```
	sudo apt-get install libavcodec-dev libavdevice-dev
```
You can run the installation script directly using
```
    ./build.sh
```

### Verify installation
Verify the correctness of the build by running a full loop for transcoding.
This will encode a pointcloud, transcode it, decode the output and compute the metrics of the bitstream.

```
    ./transcode.sh
```

## Running

For running the transcoder, follow the steps described in [transcode.sh](transcode.sh).

### Video Codec options
The following video codecs are currently implemented:

[HM test model](https://hevc.hhi.fraunhofer.de/HM-doc/) is used for Baseline comparison. This is the same codec as used for the Baseline comparison in the paper, using the full Decoder-Encoder loop.
For usage, supply the 
```
	--transcodeBaseline
```
flag. A sample can be found in [transcode_HM.sh](transcode_HM.sh)

Using [libx265](https://www.x265.org/) is used as a faster software codec implementation.
Use by not adding any additional flags to PCCAppTranscoder. A sample is found in [transcode.sh](transcode.sh).

Performing GPU-accelerated transcoding can be done using the 
```
    --useCuda
```
flag. A sample is found in [transcode_gpu.sh](transcode_gpu.sh).

