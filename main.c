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
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

#define ERROR_EVENT SDL_USEREVENT
#define QUIT_EVENT (SDL_USEREVENT + 1)

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
    AVFormatContext *format_ctx;
    AVCodecContext  *codec_ctx;
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

int packet_queue_put(PacketQueue *q, AVPacket *packet)
{
    AVPacketList *packet_list;

    packet_list = (AVPacketList *)av_malloc(sizeof(AVPacketList));

    if (!packet_list)
        return -1;

    packet_list->pkt = *packet;
    packet_list->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = packet_list;
    else
        q->last_pkt->next = packet_list;

    q->last_pkt = packet_list;
    q->nb_packets++;
    q->size += packet_list->pkt.size;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);

    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *packet, int block)
{
    AVPacketList *packet_list;
    int ret;

    SDL_LockMutex(q->mutex);

    while(TRUE) {

        // FIXME: Not working
        /*
        if(global_video_state->quit) {
            ret = -1;
            break;
        }
        */

        packet_list = q->first_pkt;

        if (packet_list) {
            q->first_pkt = packet_list->next;

            if (!q->first_pkt)
                q->last_pkt = NULL;

            q->nb_packets--;
            q->size -= packet_list->pkt.size;
            *packet = packet_list->pkt;

            av_free(packet_list);
            ret = 1;

            break;
        } else if (!block) {
            ret = 0;
            break;
        } else
            SDL_CondWait(q->cond, q->mutex);
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
        av_packet_unref(&pkt->pkt);
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
		return TRUE;
	return FALSE;
}

int64_t get_decode_channel_layout(audio_entry *audio)
{
	if(have_channel_layout(audio) && is_same_channel_cnt_in_frame(audio))
		return audio->frame->channel_layout;
	return av_get_default_channel_layout(audio->frame->channels);
}

int get_size_of_sample_buffer_per_channel(audio_entry *audio)
{
	return sizeof(audio->temp_buffer) / audio->target_channels / av_get_bytes_per_sample(audio->target_format);
}


/*
 * int audio_decode_frame(audio_entry *)
 *
 * Decode one audio frame and return its uncompressed size
 *
 * The processed audio frame is decoded, counverted if required, and
 * stored in audio->audio_buf, with size in bytes given by the return value.
 * 
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
                if (!(audio->frame = av_frame_alloc()))
                    return AVERROR(ENOMEM);
            } else // audio_frame을 default 값들로 초기화
                av_frame_unref(audio->frame);

			// packet->size 만큼 프레임을 디코딩하여 audio_frame에 저장
            decoded_frame_len = avcodec_decode_audio4(audio->codec_ctx, audio->frame, &got_frame, packet);
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
            wanted_nb_samples_per_channel = audio->frame->nb_samples;

			// 디코딩되노 frame과 할당 받았던 자원 정보가 일치하는지 판단
            if (audio->frame->format != audio->source_format ||
                deccoded_channel_layout != audio->source_channel_layout ||
                audio->frame->sample_rate != audio->source_samplerate ||
                (wanted_nb_samples_per_channel != audio->frame->nb_samples && !audio->swr_ctx)) {
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
                audio->source_channels = audio->codec_ctx->channels;
                audio->source_samplerate = audio->codec_ctx->sample_rate;
                audio->source_format = audio->codec_ctx->sample_fmt;
            }

			// swr_ctx가 메모리 할당 되었는지 판단
            if (audio->swr_ctx) {
                const uint8_t **in = (const uint8_t **)audio->frame->extended_data;	 // 압축된 오디오 패킷을 가리키기 위해 더블포인터 사용
                uint8_t *out[] = { audio->temp_buffer }; // 오디오로 변환된 프레임 데이터를 저장할 버퍼를 가리키는 포인터 배열
				if (wanted_nb_samples_per_channel != audio->frame->nb_samples) {
					// swr_ctx에 오디오 정보들을 resampling하여 swr
					 if (swr_set_compensation(audio->swr_ctx, (wanted_nb_samples_per_channel - audio->frame->nb_samples)
												 * audio->target_samplerate / audio->frame->sample_rate,
												 wanted_nb_samples_per_channel * audio->target_samplerate / audio->frame->sample_rate) < 0) {
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
        if (packet->data) av_packet_unref(packet);
		memset(packet, 0, sizeof(*packet));
        if (audio->state) return -1;
        if (packet_queue_get(&audio->queue, packet, 1) < 0) return -1;

        audio->packet_data = packet->data;
        audio->packet_size = packet->size;
    }
}

/*
 * void audio_callback(void *, Uint8 *, int)
 * 
 * 다른 함수로 부터 데이터를 끌어오는 간단한 루프로써 오디오 디바이스에 출력할 데이터가 필요하면 SDL_thread에서 콜백함수가 호출되고,
 *  audio_data_stream에 필요한 만큼의 데이터를 디코딩하여 전달하는 함수.
 */
void audio_callback(void *st_audio_entry, Uint8 *audio_data_stream, int stream_buffer_length)
{
    audio_entry *audio = (audio_entry *)st_audio_entry;
    int transport_buffer_length;
    int audio_data_size;

    while (stream_buffer_length > 0) {
        if (audio->buffer_index >= audio->buffer_size) {
            audio_data_size = audio_decode_frame(audio); // "audio"를 audio_decode_frame 함수에 넘겨 데이터 사이즈를 돌려받아 audio_data_size에 저장

            if(audio_data_size < 0) {
                audio->buffer_size = 1024;
                memset(audio->buffer, 0, audio->buffer_size);
            } else
                audio->buffer_size = audio_data_size;
            
            audio->buffer_index = 0;
        }

        transport_buffer_length = audio->buffer_size - audio->buffer_index;
        
        if (transport_buffer_length > stream_buffer_length)
            transport_buffer_length = stream_buffer_length;

        // audio_data_stream에 "audio"에 저장된 버퍼의 내용을 transport_buffer_length만큼 전송
        memcpy(audio_data_stream, (uint8_t *)audio->buffer + audio->buffer_index, transport_buffer_length);

        stream_buffer_length -= transport_buffer_length;
        audio_data_stream += transport_buffer_length;
        audio->buffer_index += transport_buffer_length;
    } // 루프를 돌며 stream에서 필요한 데이터 길이만큼 분할한 후, 디코딩하여 호출한 쓰레드로 데이터 전송
}

/* 
 * int stream_component_open(audio_entry *, int)
 * 
 * audio_entry구조체와 stream index를 매개변수로 받아 사운드 매개 변수 및 사운드 파일을 설정하는 함수
 */
int stream_component_open(audio_entry *audio, int stream_index)
{
    AVFormatContext *audio_ctx = audio->format_ctx;
    SDL_AudioSpec spec, wanted_spec;		 
    int64_t wanted_channel_layout = 0;				 
    int wanted_nb_channels;
    int nb_channels_layout;
    const int next_nb_channels[] = {0, 0, 1 ,6, 2, 6, 4, 6}; //이 배열을 사용하여 지원되지 않는 채널 수를 수정

    if (stream_index < 0 || stream_index >= audio_ctx->nb_streams)
        return -1; //오디오 코덱 없을 경우
	
	wanted_nb_channels = audio->codec_ctx->channels;

	if(!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
		wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
		wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
	}

	wanted_spec.channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
	wanted_spec.freq = audio->codec_ctx->sample_rate;

	if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
		fprintf(stderr, "Invalid sample rate or channel count!\n");
		return -1;
	}

	/* 오디오 정보를 담는 구조체 설정 */
	wanted_spec.format = AUDIO_S16SYS;		
	wanted_spec.silence = 0;			
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;	
	wanted_spec.callback = audio_callback;		//오디오 버퍼를 채우는 콜백함수
	wanted_spec.userdata = audio;			
	
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
		wanted_channel_layout = av_get_default_channel_layout(spec.channels);
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
	audio->source_format = audio->target_format = AV_SAMPLE_FMT_S16;
	audio->source_samplerate = audio->target_samplerate = spec.freq;
	audio->source_channel_layout = audio->target_channel_layout = wanted_channel_layout;
	audio->source_channels = audio->target_channels = spec.channels;
    
    //codec = avcodec_find_decoder(codec_ctx->codec_id); // avcodec_find_decoder(enum AVCodecID id) : 일치하는 코덱 id가 있는 등록 된 디코더를 찾는다 
    /* 
       avcodec_open2(AVCodecContext구조체, 방금찾은 AVCodec구조체, Decoder초기화에 필요한 추가옵션) 
       : 만일 디코더 정보가 존재한다면, AVCodecContext에 해당 정보를 넘겨줘서 디코더로 초기화 
    */
    if (!audio->codec_ctx->codec || (avcodec_open2(audio->codec_ctx, audio->codec_ctx->codec, NULL) < 0)) { //지원되지 않는 코덱이거나 디코더 정보 없으면
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

	audio_ctx->streams[stream_index]->discard = AVDISCARD_DEFAULT; //AVDISCARD_DEFAULT : avi에서 0 크기 패킷과 같은 쓸데없는 패킷을 버린다
    switch(audio->codec_ctx->codec_type) {
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

    return 0;
}

int quit_thread(audio_entry *st_audio_entry, int event_number)
{
    SDL_Event event;
    
    event.type = event_number;
    event.user.data1 = st_audio_entry;

    if (SDL_PushEvent(&event) == -1)
        return FALSE;

    return TRUE;
}

/*
 * static int decode_thread(void *);
 * 
 * 전달 받은 파일을 오픈하고 헤더 정보를 가져온 뒤, 오디오 코덱이 있는 스트림을 찾아 오디오를
 * 재생시키는 별도의 쓰레드를 동작시킨 후, 해당 스트림과 일치하는 패킷을 찾아 오디오 큐에 입력하는 함수.
 * 
 * 리턴값: 정상적인 종료시 0, 에러 혹은 충돌에 의한 비정상적인 종료시 -1
 */
static int decode_thread(void *st_audio_entry)
{
    audio_entry *audio = (audio_entry *)st_audio_entry;
    AVFormatContext *audio_ctx = NULL;
    AVCodec *codec = NULL;
    AVPacket *packet = NULL;
    int i, next_frame_index;

    audio->stream_index = -1;

    if (avformat_open_input(&audio_ctx, audio->filename, NULL, NULL) != 0) {
        quit_thread(audio, ERROR_EVENT);
        return -1;
    } // 해당 오디오 파일을 오픈하고, 전달한 AVFormatContext 구조체 주소에 헤더 정보 저장

    audio->format_ctx = audio_ctx;

    if (avformat_find_stream_info(audio_ctx, NULL) < 0) {
        quit_thread(audio, ERROR_EVENT;
        return -1;
    } // 오디오 헤더로부터 스트림 정보 검색

    av_dump_format(audio_ctx, 0, audio->filename, 0); // 디버깅을 위해 파일의 헤더 정보를 표준 에러로 덤프

    for (i = 0; i < audio_ctx->nb_streams; i++) {
        if (audio_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio->stream_index = i;
            break;
        }
    } // 오디오 코덱이 존재하는 스트림 인덱스 번호 탐색

    if (audio->stream_index < 0) {
        fprintf(stderr, "%s: could not open codecs\n", audio->filename);
        quit_thread(audio, ERROR_EVENT);
        return -1
    }

    codec = avcodec_find_decoder(audio_ctx->streams[audio->stream_index]->codecpar->codec_id);
    
    if (!codec) {
		fprintf(stderr, "Failed to find decoder for stream #%u\n", audio->stream_index);
        quit_thread(audio, ERROR_EVENT);
        return -1;
    }
    
    audio->codec_ctx = avcodec_alloc_context3(codec);
    
    if (!audio->codec_ctx) {
		fprintf(stderr, "Failed to allocate the decoder context for stream #%u\n", audio->stream_index);
        quit_thread(audio, ERROR_EVENT);
        return -1;
    }

    if (avcodec_open2(audio->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        quit_thread(audio, ERROR_EVENT);
        return -1;
    }

    if (stream_component_open(audio, audio->stream_index) == -1) {
        fprintf(stderr, "Failed to call stream_component_open function.\n");
        quit_thread(audio, ERROR_EVENT);
        return -1;
    } // 추가 설정 후, 별도의 쓰레드로 오디오 재생

    packet = (AVPacket *)malloc(sizeof(AVPacket));

    // main decode loop
    while(TRUE) {
        if(audio->state)
            break; // 다른 쓰레드에서 종료 요청이 들어왔는지 검사

        if (audio->queue.size > AVCODEC_MAX_AUDIO_FRAME_SIZE) {
            SDL_Delay(10);
            continue;
        } // 오디오 패킷 큐의 오버플로우 검사

        next_frame_index = av_read_frame(audio->format_ctx, packet); // 프레임의 저장된 내용을 packet에 저장하고, 다음 프레임 인덱스 번호 리턴

        if (next_frame_index < 0) {
            // TODO: if & continue 문 간략하게 만들 것
            if(next_frame_index == AVERROR_EOF || avio_feof(audio->format_ctx->pb)) {
                break;
            }
            if(audio->format_ctx->pb && audio->format_ctx->pb->error) {
                break;
            }
            continue;
        } // 실패 요인에 파일의 끝 혹은 타 에러 발생 여부 검사

        if (packet->stream_index == audio->stream_index)
            packet_queue_put(&audio->queue, packet);
        else
            av_packet_unref(packet); // 해당 오디오 스트림이 아닐 경우 얻어온 패킷의 메모리 해제
    }

    while (!audio->state) {
        SDL_Delay(100);
    } // 다른 쓰레드와 종료 시점 동기화

    quit_thread(audio, QUIT_EVENT);

    return 0;
}

/* int main(int, char **)
 *
 * 프로그램의 메인함수이며 전체적인 프로그램의 구동을 시작하는 함수이다.
 * 좀 더 자세히 설명하자면
 * 리눅스 터미널 내부에서 파일명과 오디오파일명을 받아
 * 오디오 파일에 맞게 SDL_Thread를 생성해 오디오파일을 재생한다.
 * 그 후 SDL_Event를 기다리는 루프에 빠지는데
 * 루프내에서 종료를 알리는 이벤트를 감지하게되면 프로그램을 종료하는 함수이다.
 *
 */
int main(int argc, char **argv)
{
    SDL_Event event; //SDL_Event "event" 선언
    audio_entry *audio; //오디오에 대한 정보를 담고있는 audio_entry 구조체의 사이즈 만큼 동적할당

    audio = (audio_entry *)av_mallocz(sizeof(audio_entry)); //"audio"에 audio_entry 구조체의 사이즈만큼 동적할당

    if (argc < 2) {
        fprintf(stderr, "Usage: test <file>\n"); //사용법 안내메시지 출력
        exit(1);
    }

    av_register_all();/* 모든 libavformat을 초기화하고 muxers, demuxers, protocols 등
                      오디오 재생에 필요한 요소들을 등록하고 내부의 avcodec_register_all()을 실행한다.
                      avcodec_register_all()은 codecs, parser, bit stream filter등 오디오 코덱 관련 요소등을 등록한다.*/

    if (SDL_Init(SDL_INIT_AUDIO)) { //SDL 오디오 서브스트림을 초기화한다. 만약 초기화하는데 실패하면 1을 반환한다.
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError()); //SDL을 초기화 할 수 없었다는 에러 메시지 출력
        exit(1);
    }

    av_strlcpy(audio->filename, argv[1], sizeof(audio->filename));/*av_strlcpy는 ffmpeg에서 제공하는 strlcpy함수로써 기능은 서로 거의 같으며
                                                                  메인의 두번째 매개변수를 "audio"의 filename 인자에 복사한다. */

    audio->thread_id = SDL_CreateThread(decode_thread, audio); //SDL쓰레드를 생성하여 반환값을 "audio"의 thread_id 인자에 대입한다.
    if (!audio->thread_id) { //만일 "audio"의 thread_id가 0이면 즉, 쓰레드 생성이 비정상적인경우
        av_free(audio);
        exit(1);
    }

    while (TRUE)
    {
        SDL_WaitEvent(&event);

        switch(event.type) {
            case ERROR_EVENT:
                av_free(audio);
                exit(1);
                break;
            case QUIT_EVENT:
                SDL_Quit();
                exit(0);
                break;
            case SDL_QUIT:
                SDL_Quit();
                exit(0);
                break;
            default:
                break;
        }
    }

    return 0;
}
