#!/bin/bash
# exp.sh NAME [icamerasrc props...]  -> exp/NAME/{payload dumps, hal.log, last.jpg}
R=${IPU7_RE_SCRATCH:-/tmp/ipu7-re}
D=$R/../s2; S=$D/stage; N=$1; shift
rm -rf $R/exp/$N; mkdir -p $R/exp/$N
cd $R && cameraDebug=4 PSYSDUMP_FROM=${FROM:-140} LD_PRELOAD=$R/psysdump.so PSYSDUMP_DIR=$R/exp/$N \
  LD_LIBRARY_PATH=$S/usr/lib64 GST_PLUGIN_PATH=$S/usr/lib64/gstreamer-1.0 CAMERA_CFG_PATH=$S/etc/camera/ipu75xa/ GST_REGISTRY=$D/gstreg.bin \
  timeout 60 gst-launch-1.0 -q icamerasrc device-name=imx471-uf num-buffers=${NB:-80} "$@" ! video/x-raw,format=NV12,width=1920,height=1080 \
  ! videoconvert ! videoscale ! video/x-raw,width=640,height=360 ! jpegenc ! multifilesink location=$R/exp/$N/img_%03d.jpg > $R/exp/$N/hal.log 2>&1
ls $R/exp/$N/img_*.jpg | tail -1 | xargs -I{} cp {} $R/exp/$N/last.jpg
echo "$N: $(ls $R/exp/$N | grep -c '^t') dumps, $(grep -c 'color_gains' $R/exp/$N/hal.log) pa logs"
