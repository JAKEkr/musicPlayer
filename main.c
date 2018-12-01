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

int is_same_channel_cnt_in_frame(audio_entry *audio)
{
	if (audio->frame->channels == av_get_channel_layout_nb_channels(audio->frame->channel_layout))
		return TRUE;
	return FALSE;
}

int have_channel_layout(audio_entry *audio)
{
	if (audio->frame->channel_layout > 0)
		return true;
	return false;
}

int64_t get_decode_channel_layout(audio_entry *audio)
{
	if(have_channel_layout(is) && is_same_channel_cnt_in_frame(is))
		return audio->frame->channel_layout
	return av_get_default_channel_layout(audio->frame->channels);
}

int get_size_of_sample_buffer_per_channel(audio_entry *audio)
{
	return sizeof(audio->temp_buffer) / audio->target_channels / av_get_bytes_per_sample(audio->target_format);
}


/*
 * Decode one audio frame and return its uncompressed size
 *
 * The processed audio frame is decoded, counverted if required, and
 * stored in audio->audio_buf, with size in bytes given by the return
 * value.
 * audio : 오디오 파일 상태 정보를 담고 있는 변수
 *
 */
int audio_decode_frame(audio_entry *audio)
{
	int decoded_frame_len;
	int nb_sample_cnt_per_channel;
	int decoded_data_size;
    AVPacket *packet = &audio->packet;
    int got_frame = 0;	// 프레임을 얻을수 있는지 없는지 판단 여부
    int64_t deccoded_channel_layout;	
	int wanted_nb_samples_per_channel;
	int resampled_data_size;	// 최종적으로 오디오로 변환된 데이터 크기

   while(TRUE) {
        while (audio->packet_size > 0) {
			// 메모리 할당 여부 판단
            if (!audio->frame) {
				// 메모리 할당
                if (!(audio->frame = av_frame_alloc())) {
                    return AVERROR(ENOMEM);
                }
            } else // audio_frame을 default 값들로 초기화
                av_frame_unref(audio->frame);

			// packet->size 만큼 프레임을 디코딩하여 audio_frame에 저장
            decoded_frame_len = avcodec_decode_audio4(audio->stream->codec, audio->frame, &got_frame, packet);
			// 디코드 실패한경우
            if (decoded_frame_len < 0) {
                audio->packet_size = 0;
                break;
            }
			
			// 읽은 프레임 길이 만큼 남은 패킷 정보 변경
            audio->packet_data += decoded_frame_len;
            audio->packet_size -= decoded_frame_len;

			// 프레임을 디코드 할 수 없는경우
            if (!got_frame) 
                continue;

			// audio_frame에 설정되어있는 값들에 필요한 버퍼 사이즈 얻음
            decoded_data_size = av_samples_get_buffer_size(NULL,
                                audio->frame->channels,
                                audio->frame->nb_samples,
                                audio->frame->format, 1);

			deccoded_channel_layout = get_decode_channel_layout(audio);

			// 출력 하고자 하는 정보에 분석한 프레임의 채널당 오디오 샘플 수를 저장
            wanted_nb_samples =  audio->frame->nb_samples;

			// 디코딩되노 frame과 할당 받았던 자원 정보가 일치하는지 판단
            if (audio->frame->format != audio->source_format ||
                deccoded_channel_layout != audio->source_channel_layout ||
                audio->frame->sample_rate != audio->source_samplerate ||
                (wanted_nb_samples != audio->frame->nb_samples && !audio->swr_ctx)) {
				// 메모리 할당 되있다면 해제
                if (audio->swr_ctx) swr_free(&audio->swr_ctx);
				// 원하는 정보로 재할당
                audio->swr_ctx = swr_alloc_set_opts(NULL,
                                                 audio->target_channel_layout,
                                                 audio->target_format,
                                                 audio->target_samplerate,
                                                 deccoded_channel_layout,
                                                 audio->frame->format,
                                                 audio->frame->sample_rate,
                                                 0, NULL);
				// 할당된 메모리 초기화 및 확인
                if (!audio->swr_ctx || swr_init(audio->swr_ctx) < 0) {
                    fprintf(stderr, "swr_init() failed\n");
                    break;
                }
				// 오디오 원본 정보를 갱신
                audio->source_channel_layout = deccoded_channel_layout;
                audio->source_channels = audio->stream->codec->channels;
                audio->source_samplerate = audio->stream->codec->sample_rate;
                audio->source_format = audio->stream->codec->sample_fmt;
            }

			// swr_ctx가 메모리 할당 되었는지 판단
            if (audio->swr_ctx) {
                const uint8_t **in = (const uint8_t **)audio->frame->extended_data;	 // 압축된 오디오 패킷을 가리키기 위해 더블포인터 사용
                uint8_t *out[] = { audio->temp_buffer }; // 오디오로 변환된 프레임 데이터를 저장할 버퍼를 가리키는 포인터 배열
				if (wanted_nb_samples != audio->frame->nb_samples) {
					// swr_ctx에 오디오 정보들을 resampling하여 swr
					 if (swr_set_compensation(audio->swr_ctx, (wanted_nb_samples - audio->frame->nb_samples)
												 * audio->target_samplerate / audio->frame->sample_rate,
												 wanted_nb_samples * audio->target_samplerate / audio->frame->sample_rate) < 0) {
						 fprintf(stderr, "swr_set_compensation() failed\n");
						 break;
					 }
				 }

				int sample_buffer_size = get_size_of_sample_buffer_per_channel(audio);
				// 주어진 정보들을 오디오로 변환
                nb_sample_cnt_per_channel = swr_convert(audio->swr_ctx, out,
                                   sample_buffer_size, in, audio->frame->nb_samples);

                if (nb_sample_cnt_per_channel < 0) {
                    fprintf(stderr, "swr_convert() failed\n");
                    break;
                }
				// 성공적으로 오디오 변환 판단
                if (nb_sample_cnt_per_channel == sizeof(audio->temp_buffer) / audio->target_channels / av_get_bytes_per_sample(audio->target_format)) {
                    fprintf(stderr, "warning: audio buffer audio probably too small\n");
                    swr_init(audio->swr_ctx);
                }

				// 오디오로 변환된 정보를 callback함수에서 사용할 다른 버퍼에 대입
                audio->buffer = audio->temp_buffer;
				// 변환된 오디오 크기 저장
                resampled_data_size = nb_sample_cnt_per_channel * audio->target_channels * av_get_bytes_per_sample(audio->target_format);
            } else {	// sr
				resampled_data_size = decoded_data_size;
                audio->buffer = audio->frame->data[0];
            }
            // 출력된 오디오 데이터 크기를 반환
            return resampled_data_size;
        }

		// 프레임 디코딩에 문제가 생겨 초기화
        if (packet->data) av_free_packet(packet);
		memset(packet, 0, sizeof(*packet));
        if (audio->state) return -1;
        if (packet_queue_get(&audio->queue, packet, 1) < 0) return -1;

        audio->packet_data = packet->data;
        audio->packet_size = packet->size;
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
