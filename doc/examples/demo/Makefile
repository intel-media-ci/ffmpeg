# use pkg-config for getting CFLAGS and LDLIBS
FFMPEG_LIBS=    libavdevice                        \
                libavformat                        \
                libavfilter                        \
                libavcodec                         \
                libswresample                      \
                libswscale                         \
                libavutil                          \

CFLAGS += -Wall -g -lpthread
CFLAGS := $(shell pkg-config --cflags $(FFMPEG_LIBS) libmfx) $(CFLAGS)
LDLIBS := $(shell pkg-config --libs $(FFMPEG_LIBS) libmfx) $(LDLIBS)
LDLIBS += -lpthread

EXAMPLES=      qsvdec_mthread\
               qsvdec_mthread_infinite_file\
               qsvdec_mthread_free_resource\

OBJS=$(addsuffix .o,$(EXAMPLES))

# the following examples make explicit use of the math library
avcodec:           LDLIBS += -lm
encode_audio:      LDLIBS += -lm
muxing:            LDLIBS += -lm
resampling_audio:  LDLIBS += -lm

.phony: all clean-test clean

all: $(OBJS) $(EXAMPLES)

clean-test:
	$(RM) test*.pgm test.h264 test.mp2 test.sw test.mpg

clean: clean-test
	$(RM) $(EXAMPLES) $(OBJS)
