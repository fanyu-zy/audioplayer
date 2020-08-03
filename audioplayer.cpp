// audioplayer.cpp: 定义应用程序的入口点。
//

#include "audioplayer.h"
#ifdef _WIN32
//Windows
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL/SDL.h>
};
#else
//Linux...
#ifdef __cplusplus
extern "C"
{
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#ifdef __cplusplus
};
#endif
#endif
using std::cin;
using std::cout;
using std::flush;
using std::string;
using namespace std;
#define __STDC_CONSTANT_MACROS
#define MAX_AUDIO_FRAME_SIZE 192000

static int  audio_len;
static  Uint8 *audio_chunk, *audio_pos;

void fill_audio(void* udata, Uint8* stream, int len) {
	SDL_memset(stream, 0, len);
	if (audio_len == 0)
		return;

	len = (len > audio_len ? audio_len : len);

	SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME);
	audio_pos += len;
	audio_len -= len;
}

int main(int argc, char* argv[]) {
	AVFormatContext* audioFormatCtx;
	AVCodecContext* audioCodecCtx;
	AVCodec* audioCodec;
	AVPacket* audiopacket;
	AVFrame* audioFrame;
	SDL_AudioSpec wanted_spec;
	uint8_t* out_buffer;
	int audioStream, ret;
	int64_t in_channel_layout;
	struct SwrContext* au_convert_ctx;

	string url;
	//url= "F:/forgit/gitrep/mp4player/mp4player/42stest.mp4";
	cout << "Enter audio url:" << flush;
	cin >> url;
	
	//初始化，检测文件流信息
	avformat_network_init();
	audioFormatCtx = avformat_alloc_context();
	if (avformat_open_input(&audioFormatCtx, url.c_str(), NULL, NULL) != 0) {
		cout << "Couldn't open input stream." << endl;
		return -1;
	}
	if (avformat_find_stream_info(audioFormatCtx, NULL) < 0) {
		cout << "Couldn't find stream information." << endl;
		return -1;
	}
	av_dump_format(audioFormatCtx, 0, url.c_str(), false);
	//读
	audioStream = -1;
	for (unsigned int i = 0; i < audioFormatCtx->nb_streams; i++)
		if (audioFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audioStream = i;
			break;
		}
	if (audioStream == -1) {
		cout<< "Didn't find a audio stream." << endl;
		return -1;
	}
	//找解码器
	audioCodecCtx = avcodec_alloc_context3(NULL);
	avcodec_parameters_to_context(audioCodecCtx, audioFormatCtx->streams[audioStream]->codecpar);
	audioCodec = avcodec_find_decoder(audioCodecCtx->codec_id);
	if (audioCodec == NULL) {
		cout<< "Codec not found." << endl;
		return -1;
	}
	//初始化AVCodecContext
	if (avcodec_open2(audioCodecCtx, audioCodec, NULL) < 0) {
		cout << "Could not open codec." << endl;
		return -1;
	}
	//packet初始化
	audiopacket = (AVPacket*)av_malloc(sizeof(AVPacket));
	av_init_packet(audiopacket);
	//输出数据初始化
	uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
	int out_nb_samples = audioCodecCtx->frame_size;
	enum AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;//enum class警告
	int out_sample_rate = audioCodecCtx->sample_rate;
	int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
	int out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);
	out_buffer = (uint8_t*)av_malloc(MAX_AUDIO_FRAME_SIZE * 2);
	audioFrame = av_frame_alloc();

	//SDL初始化
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		cout<<"Could not initialize SDL - "<< SDL_GetError()<<endl;
		return -1;
	}
	//设置音频信息，打开设备
	wanted_spec.freq = out_sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = out_channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = out_nb_samples;
	wanted_spec.callback = fill_audio;
	wanted_spec.userdata = audioCodecCtx;
	if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
		cout << "can't open audio." << endl;
		return -1;
	}
	
	in_channel_layout = av_get_default_channel_layout(audioCodecCtx->channels);
	//重采样
	au_convert_ctx = swr_alloc();
	au_convert_ctx = swr_alloc_set_opts(au_convert_ctx, out_channel_layout, out_sample_fmt, out_sample_rate,
		in_channel_layout, audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate, 0, NULL);
	swr_init(au_convert_ctx);

	SDL_PauseAudio(0);
	//对于音频来说，一个packet里面，可能含有多帧(frame)数据
	while (av_read_frame(audioFormatCtx, audiopacket) >= 0) {
		if (audiopacket->stream_index == audioStream) {
			ret = avcodec_send_packet(audioCodecCtx, audiopacket);
			if (ret != 0) { cout << "error" << endl; }
			while (avcodec_receive_frame(audioCodecCtx, audioFrame) == 0) {
				swr_convert(au_convert_ctx, &out_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t**)audioFrame->data, audioFrame->nb_samples);
			}
			//播放
			while (audio_len > 0)
				SDL_Delay(1);

			audio_chunk = (Uint8*)out_buffer;
			audio_len = out_buffer_size;
			audio_pos = audio_chunk;
		}
		av_packet_unref(audiopacket);
	}

	swr_free(&au_convert_ctx);
	SDL_CloseAudio();
	SDL_Quit();
	av_free(out_buffer);
	avcodec_close(audioCodecCtx);
	avformat_close_input(&audioFormatCtx);

	return 0;
}