#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avstring.h>
#include <libavutil/pixfmt.h>
#include <libavutil/log.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>
#include <stdio.h>
#include <math.h>

#define SDL_AUDIO_BUFFER_SIZE 1024 
#define MAX_AUDIOQ_SIZE (1 * 1024 * 1024)
#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

// FFMPEG 라이브러리 버전업으로 해당 매크로가 사라진 관계로 임의로 정의해놓음
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 1024 * 1024 * 4

#define TRUE 1
#define FALSE 0

typedef struct PacketQueue
{
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct audio_entry
{
    char            filename[1024];
    AVFormatContext *context;
    int             stream_index;
    AVStream        *stream;
    AVFrame         *frame;
    PacketQueue     queue;
    uint8_t         *buffer;
    unsigned int    buffer_size;
    unsigned int    buffer_index;
    AVPacket        packet;
    uint8_t         *packet_data;
    int             packet_size;
    DECLARE_ALIGNED(16,uint8_t,temp_buffer)[AVCODEC_MAX_AUDIO_FRAME_SIZE];
    enum AVSampleFormat  source_format;
    enum AVSampleFormat  target_format;
    int             source_channels;
    int             target_channels;
    int64_t         source_channel_layout;
    int64_t         target_channel_layout;
    int             source_samplerate;
    int             target_samplerate;
    struct SwrContext *swr_ctx;
    SDL_Thread      *thread_id;
    int             state;
} audio_entry;

// HACK: Not used
// VideoState *global_video_state;

void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1) {
        return -1;
    }
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt) {
        q->first_pkt = pkt1;
    } else {
        q->last_pkt->next = pkt1;
    }

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for(;;) {

        // FIXME: Not working
        /*
        if(global_video_state->quit) {
            ret = -1;
            break;
        }
        */

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;

            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);

    return ret;
}

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}

int audio_decode_frame(audio_entry *is)
{
    int len1, len2, decoded_data_size;
    AVPacket *pkt = &is->packet;
    int got_frame = 0;
    int64_t dec_channel_layout;
    int wanted_nb_samples, resampled_data_size;

    for (;;) {
        while (is->packet_size > 0) {
            if (!is->frame) {
                if (!(is->frame = av_frame_alloc())) {
                    return AVERROR(ENOMEM);
                }
            } else 
                av_frame_unref(is->frame);

            len1 = avcodec_decode_audio4(is->stream->codec, is->frame, &got_frame,  pkt);
            if (len1 < 0) {
                // error, skip the frame
                is->packet_size = 0;
                break;
            }

            is->packet_data += len1;
            is->packet_size -= len1;

            if (!got_frame) 
                continue;

            decoded_data_size = av_samples_get_buffer_size(NULL,
                                is->frame->channels,
                                is->frame->nb_samples,
                                is->frame->format, 1);

            dec_channel_layout = (is->frame->channel_layout && is->frame->channels
                                  == av_get_channel_layout_nb_channels(is->frame->channel_layout))
                                 ? is->frame->channel_layout
                                 : av_get_default_channel_layout(is->frame->channels);

            wanted_nb_samples =  is->frame->nb_samples;

            //fprintf(stderr, "wanted_nb_samples = %d\n", wanted_nb_samples);

            if (is->frame->format != is->source_format ||
                dec_channel_layout != is->source_channel_layout ||
                is->frame->sample_rate != is->source_samplerate ||
                (wanted_nb_samples != is->frame->nb_samples && !is->swr_ctx)) {
                if (is->swr_ctx) swr_free(&is->swr_ctx);
                is->swr_ctx = swr_alloc_set_opts(NULL,
                                                 is->target_channel_layout,
                                                 is->target_format,
                                                 is->target_samplerate,
                                                 dec_channel_layout,
                                                 is->frame->format,
                                                 is->frame->sample_rate,
                                                 0, NULL);
                if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
                    fprintf(stderr, "swr_init() failed\n");
                    break;
                }
                is->source_channel_layout = dec_channel_layout;
                is->source_channels = is->stream->codec->channels;
                is->source_samplerate = is->stream->codec->sample_rate;
                is->source_format = is->stream->codec->sample_fmt;
            }
            if (is->swr_ctx) {
               // const uint8_t *in[] = { is->frame->data[0] };
                const uint8_t **in = (const uint8_t **)is->frame->extended_data; 
                uint8_t *out[] = { is->temp_buffer };
				if (wanted_nb_samples != is->frame->nb_samples) {
					 if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - is->frame->nb_samples)
												 * is->target_samplerate / is->frame->sample_rate,
												 wanted_nb_samples * is->target_samplerate / is->frame->sample_rate) < 0) {
						 fprintf(stderr, "swr_set_compensation() failed\n");
						 break;
					 }
				 }

                len2 = swr_convert(is->swr_ctx, out,
                                   sizeof(is->temp_buffer)
                                   / is->target_channels
                                   / av_get_bytes_per_sample(is->target_format),
                                   in, is->frame->nb_samples);
                if (len2 < 0) {
                    fprintf(stderr, "swr_convert() failed\n");
                    break;
                }
                if (len2 == sizeof(is->temp_buffer) / is->target_channels / av_get_bytes_per_sample(is->target_format)) {
                    fprintf(stderr, "warning: audio buffer is probably too small\n");
                    swr_init(is->swr_ctx);
                }
                is->buffer = is->temp_buffer;
                resampled_data_size = len2 * is->target_channels * av_get_bytes_per_sample(is->target_format);
            } else {
				resampled_data_size = decoded_data_size;
                is->buffer = is->frame->data[0];
            }
            // We have data, return it and come back for more later
            return resampled_data_size;
        }

        if (pkt->data) av_free_packet(pkt);
		memset(pkt, 0, sizeof(*pkt));
        if (is->state) return -1;
        if (packet_queue_get(&is->queue, pkt, 1) < 0) return -1;

        is->packet_data = pkt->data;
        is->packet_size = pkt->size;
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
    audio_entry *is = (audio_entry *)userdata;
    int len1, audio_data_size;

    while (len > 0) {
        if (is->buffer_index >= is->buffer_size) {
            audio_data_size = audio_decode_frame(is);

            if(audio_data_size < 0) {
                /* silence */
                is->buffer_size = 1024;
                memset(is->buffer, 0, is->buffer_size);
            } else {
                is->buffer_size = audio_data_size;
            }
            is->buffer_index = 0;
        }

        len1 = is->buffer_size - is->buffer_index;
        if (len1 > len) {
            len1 = len;
        }

        memcpy(stream, (uint8_t *)is->buffer + is->buffer_index, len1);
        len -= len1;
        stream += len1;
        is->buffer_index += len1;
    }
}

/* 사운드 매개 변수 및 사운드 파일 설정 */
int stream_component_open(audio_entry *audio, int stream_index)
{
    AVFormatContext *audio_ctx = audio->context;
    AVCodecContext *codec_ctx;					 //stream을 디코딩 할 때 필요한 정보
    AVCodec *codec;
    SDL_AudioSpec wanted_spec;					 //오디오의 원하는 출력 형식을 나타내는 구조체
    SDL_AUdioSpec spec;						 //실제 매개 변수로 채워지는 구조체
    int64_t wanted_channel_layout = 0;				 //64비트(8바이트) 크기의 부호 있는 정수형 채널 레이아웃
    int wanted_nb_channels;
    int nb_channels_layout;
    const int next_nb_channels[] = {0, 0, 1 ,6, 2, 6, 4, 6}; //이 배열을 사용하여 지원되지 않는 채널 수를 수정

    if (stream_index < 0 || stream_index >= audio_ctx->nb_streams) {    //오디오 코덱 없을 경우
        return -1;
    }
	
    codec_ctx = audio_ctx->streams[stream_index]->codec;		 //코덱 정보 저장
	wanted_nb_channels = codec_ctx->channels;
	/* 채널 정보 저장 */
	nb_channels_layout = av_get_channel_layout_nb_channels(wanted_channel_layout);
	if(!wanted_channel_layout || wanted_nb_channels != nb_channels_layout) { //av_get_channel_layout_nb_channels(unut64_t channel_layout) : 채널 레이아웃에서 채널 수를 반환
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);			       //av_get_default_channel_layout (int nb_channels) : 지정된 채널 수에 대한 기본 채널 레이아웃을 반환
		wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
	}
	
	wanted_spec.channels = nb_channels_layout);
	wanted_spec.freq = codec_ctx->sample_rate;
	if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {		//오디오 주파수가 유효하지 않거나 채널 수가 유효하지 않다면
		fprintf(stderr, "Invalid sample rate or channel count!\n");	//에러 출력
		return -1;
	}
	/* 오디오 정보를 담는 구조체 설정 */
	wanted_spec.format = AUDIO_S16SYS;		//SDL에게 어떤 형식으로 제공할 것인지 알려준다, signed 각 샘플 길이는 16비트
	wanted_spec.silence = 0;			//오디오가 signed 이기 때문에 0
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;	//SDL이 추가 오디오 데이터를 요청할 때를 대비한 오디오 버퍼의 사이즈
	wanted_spec.callback = audio_callback;		//오디오 버퍼를 채우는 콜백함수
	wanted_spec.userdata = audio;			//콜백함수에 전달되는 유저 데이터
	
	/* 
	   실패시 출력 
	   SDL_OpenAudio(SDL_AudioSpec* desired, SDL_AudioSpec* obtained) : 오디오 장치를 원하는 매개 변수로 열고, 성공하면 0 실패 시 음수 오류 코드 반환
        */
	while(SDL_OpenAudio(&wanted_spec, &spec) < 0) {	//오디오 장치 열기 실패 시
		fprintf(stderr, "SDL_OpenAudio (%d channels): %s\n", wanted_spec.channels, SDL_GetError());
		wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];	//FFMIN() : ffmpeg에 의해 정의 된 매크로로 더 작은 수를 반환
		if(!wanted_spec.channels) {
			fprintf(stderr, "No more channel combinations to try, audio open failed\n");
			return -1;
		}
		wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
	}

	if (spec.format != AUDIO_S16SYS) { //형식이 지원 안 될 경우
		fprintf(stderr, "SDL advised audio format %d is not supported!\n", spec.format);
		return -1;
	}
	if (spec.channels != wanted_spec.channels) { //출력하고자 하는 채널과 실제 매개 변수의 채널이 다를 경우
		wanted_channel_layout = av_get_default_channel_layout(spec.channels); //실제 매개 변수의 채널 수에 대한 채널 레이아웃을 출력하고자 하는 채널 레이아웃에 넣는다
		if (!wanted_channel_layout) {
			fprintf(stderr, "SDL advised channel count %d is not supported!\n", spec.channels);
			return -1;
		}
	}

	/* 원하는 출력 형식을 나타내는 구조체의 정보 에러 출력*/ 
	fprintf(stderr, "%d: wanted_spec.format = %d\n", __LINE__, wanted_spec.format);		//__LINE__ : 현재 소스 파일의 줄번호
	fprintf(stderr, "%d: wanted_spec.samples = %d\n", __LINE__, wanted_spec.samples);
	fprintf(stderr, "%d: wanted_spec.channels = %d\n", __LINE__, wanted_spec.channels);
	fprintf(stderr, "%d: wanted_spec.freq = %d\n", __LINE__, wanted_spec.freq);

	/* 실제 매개 변수로 채워지는 구조체의 정보 에러 출력 */
	fprintf(stderr, "%d: spec.format = %d\n", __LINE__, spec.format);
	fprintf(stderr, "%d: spec.samples = %d\n", __LINE__, spec.samples);
	fprintf(stderr, "%d: spec.channels = %d\n", __LINE__, spec.channels);
	fprintf(stderr, "%d: spec.freq = %d\n", __LINE__, spec.freq);

	/* 설정된 매개변수를 구조체에 저장 */
	audio->source_format = audio->target_format = AV_SAMPLE_FMT_S16;			//샘플 형식
	audio->source_samplerate = audio->target_samplerate = spec.freq;			//주파수
	audio->source_channel_layout = audio->target_channel_layout = wanted_channel_layout;	//채널레이아웃
	audio->source_channels = audio->target_channels = spec.channels;			//채널
    
    codec = avcodec_find_decoder(codec_ctx->codec_id);	//avcodec_find_decoder(enum AVCodecID id) : 일치하는 코덱 id가 있는 등록 된 디코더를 찾는다 
    /* 
       avcodec_open2(AVCodecContext구조체, 방금찾은 AVCodec구조체, Decoder초기화에 필요한 추가옵션) 
       : 만일 디코더 정보가 존재한다면, AVCodecContext에 해당 정보를 넘겨줘서 디코더로 초기화 
    */
    if (!codec || (avcodec_open2(codec_ctx, codec, NULL) < 0)) { //지원되지 않는 코덱이거나 디코더 정보 없으면
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
	audio_ctx->streams[stream_index]->discard = AVDISCARD_DEFAULT; //AVDISCARD_DEFAULT : avi에서 0 크기 패킷과 같은 쓸데없는 패킷을 버린다
    switch(codec_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:					//패킷 생성을 위한 각종 초기화
        audio->stream_index = stream_index;
        audio->stream = audio_ctx->streams[stream_index];
        audio->buffer_size = 0;
        audio->buffer_index = 0;
        memset(&audio->packet, 0, sizeof(audio->packet));		//오디오 패킷만큼 동적할당
        packet_queue_init(&audio->queue);				//패킷큐 생성 후 링크
        SDL_PauseAudio(0);					//실질적으로 재생하는 부분, 인자가 0이면 재생
        break;
    default:
        break;
    }
}
/*
static void stream_component_close(VideoState *is, int stream_index) {
	AVFormatContext *oc = is->;
	AVCodecContext *avctx;

	if(stream_index < 0 || stream_index >= ic->nb_streams)	return;
	avctx = ic->streams[stream_index]->codec;

}
*/
static int decode_thread(void *arg)
{
    audio_entry *is = (audio_entry *)arg;
    AVFormatContext *ic = NULL;
    AVPacket pkt1, *packet = &pkt1;
    int ret, i, audio_index = -1;

    is->stream_index=-1;

    //global_video_state = is;

    if (avformat_open_input(&ic, is->filename, NULL, NULL) != 0) {
        return -1;
    }
    is->context = ic;
    if (avformat_find_stream_info(ic, NULL) < 0) {
        return -1;
    }
    av_dump_format(ic, 0, is->filename, 0);
    for (i=0; i<ic->nb_streams; i++) {
        if (ic->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO && audio_index < 0) {
            audio_index=i;
            break;
        }
    }
    if (audio_index >= 0) {
        stream_component_open(is, audio_index);
    }
    if (is->stream_index < 0) {
        fprintf(stderr, "%s: could not open codecs\n", is->filename);
        goto fail;
    }
    // main decode loop
    for(;;) {
        if(is->state) break;
        if (is->queue.size > MAX_AUDIOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        ret = av_read_frame(is->context, packet);
        if (ret < 0) {
            if(ret == AVERROR_EOF || url_feof(is->context->pb)) {
                break;
            }
            if(is->context->pb && is->context->pb->error) {
                break;
            }
            continue;
        }

        if (packet->stream_index == is->stream_index) {
            packet_queue_put(&is->queue, packet);
        } else {
            av_free_packet(packet);
        }
    }

    while (!is->state) {
        SDL_Delay(100);
    }

fail: {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }

    return 0;
}

int main(int argc, char **argv)
{
    SDL_Event event;
    audio_entry *audio;

    audio = (audio_entry *)av_mallocz(sizeof(audio_entry));

    if (argc < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }

    av_register_all();

    if (SDL_Init(SDL_INIT_AUDIO)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    av_strlcpy(audio->filename, argv[1], sizeof(audio->filename));

    audio->thread_id = SDL_CreateThread(decode_thread, audio);
    if (!audio->thread_id) {
        av_free(audio);
        return -1;
    }

    while (TRUE)
    {
        SDL_WaitEvent(&event);
        switch(event.type) {
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            audio->state = 1;
            SDL_Quit();
            exit(0);
            break;
        default:
            break;
        }
    }

    return 0;
}
