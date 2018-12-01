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
void audio_callback(void *st_audio_entry, Uint8 *audio_data_stream, int stream_buffer_length)
{
	/*
	다른 함수로 부터 데이터를 끌어오는 간단한 루프로써
	이 함수는 오디오 디바이스에 출력할 데이터가 필요하면
	SDL_thread에서 콜백함수가 호출되고 audio_data_stream에 필요한 만큼의 데이터를
	디코딩하여 전달하기 위해 사용된다.
	*/
    audio_entry *audio = (audio_entry *)st_audio_entry; //audio_entry 구조체 포인터 "audio"에 audio_data_stream을 통해 넘어온 void 포인터 st_audio_entry가 가리키는 audio 포인터 구조체 할당
    int transport_buffer_length, audio_data_size; //넘길 버퍼의 길이 "transport_buffer_length", 디코딩 된 프레임 크기가 저장될 "audio_data_size" 변수 선언

    while (stream_buffer_length > 0) {
        if (audio->buffer_index >= audio->buffer_size) {
            audio_data_size = audio_decode_frame(audio); // "audio"를 audio_decode_frame 함수에 넘겨 데이터 사이즈를 돌려받아 audio_data_size에 저장

            if(audio_data_size < 0) {
                /* silence */
                audio->buffer_size = 1024;
                memset(audio->buffer, 0, audio->buffer_size); //"audio"의 buffer인자의 시작주소부터 "audio"의 buffer_size인자만큼 0으로 초기화한다.
            } else {
                audio->buffer_size = audio_data_size;
            }
            audio->buffer_index = 0;
        }

        transport_buffer_length = audio->buffer_size - audio->buffer_index; //넘겨줄 버퍼의 길이 "transport_buffer_length"에 "audio"의 buffer_size 인자 = buffer_index인자를 대입한다.
        if (transport_buffer_length > stream_buffer_length) { //넘겨줄 버퍼의 길이 "transport_buffer_length"이 audio_data_stream의 전송량의 길이 "stream_buffer_length"보다 큰 경우
            transport_buffer_length = stream_buffer_length; //넘겨줄 버퍼의 길이 transport_buffer_length을 audio_data_stream의 전송량의 길이 stream_buffer_length만큼으로 고정한다.
        }

        memcpy(audio_data_stream, (uint8_t *)audio->buffer + audio->buffer_index, transport_buffer_length); /* audio_data_stream에 "audio"의 buffer_index+버퍼 위치부터 transport_buffer_length만큼의 길이만큼 버퍼를 복사
									즉, audio_data_stream에 "audio"에 저장된 버퍼의 내용을 transport_buffer_length만큼 전송*/
        stream_buffer_length -= transport_buffer_length; //다음에 전송할 길이 "stream_buffer_length"을 이번에 전송한 길이 "transport_buffer_length"만큼 빼준다.
        audio_data_stream += transport_buffer_length; //audio_data_stream의 포인터를 이번에 전송한 길이 transport_buffer_length만큼 증가
        audio->buffer_index += transport_buffer_length; //"audio"의 buffer_index 인자를 이번에 전송한 길이 transport_buffer_length만큼 증가
    }
}

int stream_component_open(audio_entry *is, int stream_index)
{
    AVFormatContext *ic = is->context;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;
    int64_t wanted_channel_layout = 0;
    int wanted_nb_channels;
	const int next_nb_channels[] = {0, 0, 1 ,6, 2, 6, 4, 6};

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return -1;
    }

    codecCtx = ic->streams[stream_index]->codec;
	wanted_nb_channels = codecCtx->channels;
	if(!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
	}

	wanted_spec.channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
	wanted_spec.freq = codecCtx->sample_rate;
	if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
		fprintf(stderr, "Invalid sample rate or channel count!\n");
		return -1;
	}
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.silence = 0;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = is;

	while(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
		fprintf(stderr, "SDL_OpenAudio (%d channels): %s\n", wanted_spec.channels, SDL_GetError());
		wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
		if(!wanted_spec.channels) {
			fprintf(stderr, "No more channel combinations to tyu, audio open failed\n");
			return -1;
		}
		wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
	}

	if (spec.format != AUDIO_S16SYS) {
		fprintf(stderr, "SDL advised audio format %d is not supported!\n", spec.format);
		return -1;
	}
	if (spec.channels != wanted_spec.channels) {
		wanted_channel_layout = av_get_default_channel_layout(spec.channels);
		if (!wanted_channel_layout) {
			fprintf(stderr, "SDL advised channel count %d is not supported!\n", spec.channels);
			return -1;
		}
	}

	fprintf(stderr, "%d: wanted_spec.format = %d\n", __LINE__, wanted_spec.format);
	fprintf(stderr, "%d: wanted_spec.samples = %d\n", __LINE__, wanted_spec.samples);
	fprintf(stderr, "%d: wanted_spec.channels = %d\n", __LINE__, wanted_spec.channels);
	fprintf(stderr, "%d: wanted_spec.freq = %d\n", __LINE__, wanted_spec.freq);

	fprintf(stderr, "%d: spec.format = %d\n", __LINE__, spec.format);
	fprintf(stderr, "%d: spec.samples = %d\n", __LINE__, spec.samples);
	fprintf(stderr, "%d: spec.channels = %d\n", __LINE__, spec.channels);
	fprintf(stderr, "%d: spec.freq = %d\n", __LINE__, spec.freq);

	is->source_format = is->target_format = AV_SAMPLE_FMT_S16;
	is->source_samplerate = is->target_samplerate = spec.freq;
	is->source_channel_layout = is->target_channel_layout = wanted_channel_layout;
	is->source_channels = is->target_channels = spec.channels;

    codec = avcodec_find_decoder(codecCtx->codec_id);
    if (!codec || (avcodec_open2(codecCtx, codec, NULL) < 0)) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch(codecCtx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->stream_index = stream_index;
        is->stream = ic->streams[stream_index];
        is->buffer_size = 0;
        is->buffer_index = 0;
        memset(&is->packet, 0, sizeof(is->packet));
        packet_queue_init(&is->queue);
        SDL_PauseAudio(0);
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
