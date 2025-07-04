FATE_FLVENC_FFMPEG_FFPROBE-$(call TRANSCODE, FLV, FLV, RAWVIDEO_DECODER SCALE_FILTER TESTSRC_FILTER LAVFI_INDEV) += fate-flv-add_keyframe_index
fate-flv-add_keyframe_index: CMD = transcode "lavfi -graph testsrc=r=7:n=2:d=20" "foo" flv "-vf scale -c:v flv1 -dct int -g 7 -flvflags add_keyframe_index" "-c copy -t 0.1" "-show_entries format_tags"

FATE_ENHANCED_FLVENC_FFMPEG-$(call REMUX, FLV MOV, FLV_DEMUXER HEVC_PARSER) += fate-enhanced-flv-hevc
fate-enhanced-flv-hevc: CMD = transcode mov $(TARGET_SAMPLES)/hevc/dv84.mov\
		flv "-c copy" "-c copy"

FATE_ENHANCED_FLVENC_FFMPEG-$(call REMUX, FLV IVF, FLV_DEMUXER VP9_PARSER) += fate-enhanced-flv-vp9
fate-enhanced-flv-vp9: CMD = transcode ivf $(TARGET_SAMPLES)/vp9-test-vectors/vp90-2-05-resize.ivf\
		flv "-c copy" "-c copy"

FATE_ENHANCED_FLVENC_FFMPEG-$(call REMUX, FLV IVF, FLV_DEMUXER AV1_DECODER AV1_PARSER EXTRACT_EXTRADATA_BSF) += fate-enhanced-flv-av1
fate-enhanced-flv-av1: CMD = stream_remux ivf $(TARGET_SAMPLES)/av1/seq_hdr_op_param_info.ivf "-c:v av1" \
		flv "-c copy" "-c:v av1" "-c copy"

FATE_ENHANCED_FLVENC_FFMPEG_FFPROBE-$(call REMUX, FLV HEVC, FLV_DEMUXER HEVC_DECODER HEVC_PARSER) += fate-enhanced-flv-hevc-hdr10
fate-enhanced-flv-hevc-hdr10: CMD = stream_remux hevc $(TARGET_SAMPLES)/hevc/hdr10_plus_h265_sample.hevc "-c:v hevc" \
        flv "-c copy" "-c:v hevc" "-c copy" "-show_frames"

FATE_ENHANCED_FLVENC_FFMPEG_FFPROBE-$(call REMUX, FLV, FLV_DEMUXER AAC_PARSER AC3_PARSER OPUS_PARSER FLAC_PARSER VP9_PARSER AV1_PARSER HEVC_PARSER H264_PARSER) += fate-enhanced-flv-multitrack
fate-enhanced-flv-multitrack: CMD = stream_remux flv $(TARGET_SAMPLES)/flv/multitrack.flv "" flv "-map 0" "" "-c copy -map 0" \
		"-show_entries stream_group=index,id,nb_streams,type:stream_group_components:stream_group_stream=index"

FATE_FFMPEG_FFPROBE += $(FATE_FLVENC_FFMPEG_FFPROBE-yes)
FATE_SAMPLES_FFMPEG += $(FATE_ENHANCED_FLVENC_FFMPEG-yes)
FATE_SAMPLES_FFMPEG_FFPROBE += $(FATE_ENHANCED_FLVENC_FFMPEG_FFPROBE-yes)
fate-flvenc: $(FATE_FLVENC_FFMPEG_FFPROBE-yes) $(FATE_ENHANCED_FLVENC_FFMPEG-yes) $(FATE_ENHANCED_FLVENC_FFMPEG_FFPROBE-yes)
