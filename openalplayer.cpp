#include <iostream>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <AL/al.h>
#include <AL/alc.h>
};
using std::cin;
using std::cout;
using std::cerr;
using std::string;
using namespace std;
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio
#define NUM_BUFFERS 3
static int playing;//用flag也行，就是个判断标志

//openal未设回调，用队列
typedef struct PacketQueue {
	AVPacketList* pFirstPkt;
	AVPacketList* pLastPkt;
	int numOfPackets;
} PacketQueue;
void initPacketQueue(PacketQueue* q){
	memset(q, 0, sizeof(PacketQueue));
}
int pushPacketToPacketQueue(PacketQueue* pPktQ, AVPacket* pPkt){
	AVPacketList* pPktList;
	//if (av_dup_packet(pPkt) < 0) {
	if (pPkt->size< 0) {
		return -1;
	}
	pPktList = (AVPacketList*)av_malloc(sizeof(AVPacketList));
	if (!pPktList) {
		return -1;
	}
	pPktList->pkt = *pPkt;
	pPktList->next = NULL;
	if (!pPktQ->pLastPkt) {
		pPktQ->pFirstPkt = pPktList;
	}
	else {
		pPktQ->pLastPkt->next = pPktList;
	}
	pPktQ->pLastPkt = pPktList;
	pPktQ->numOfPackets++;
	return 0;
}
static int popPacketFromPacketQueue(PacketQueue* pPQ, AVPacket* pPkt){
	AVPacketList* pPktList;
	pPktList = pPQ->pFirstPkt;
	if (pPktList) {
		pPQ->pFirstPkt = pPktList->next;
		if (!pPQ->pFirstPkt) {
			pPQ->pLastPkt = NULL;
		}
		pPQ->numOfPackets--;
		*pPkt = pPktList->pkt;
		av_free(pPktList);
		return 0;
	}
	return -1;
}

int decode(uint8_t* buf, int bufSize, AVPacket* packet, AVCodecContext* codecContext,
	SwrContext* swr, int dstRate, int dstNbChannels, enum AVSampleFormat* dstSampleFmt){

	unsigned int bufIndex = 0;
	unsigned int dataSize = 0;
	int ret = 0;
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	if (!frame) {
		cout << "Error allocating the frame" << endl;
		av_packet_unref(packet);
		return 0;
	}
	while (packet->size > 0){
		int gotFrame = 0;
		int result = avcodec_decode_audio4(codecContext, frame, &gotFrame, packet);
		if(result>=0 &&gotFrame)
		{
			packet->size -= result;
			packet->data += result;
			int dstNbSamples = av_rescale_rnd(frame->nb_samples, dstRate, codecContext->sample_rate, AV_ROUND_UP);
			uint8_t** dstData = NULL;
			int dstLineSize;
			if (av_samples_alloc_array_and_samples(&dstData, &dstLineSize, dstNbChannels, dstNbSamples, *dstSampleFmt, 0) < 0) {
				cerr << "Could not allocate destination samples" << endl;
				dataSize = 0;
				break;
			}
			int ret = swr_convert(swr, dstData, dstNbSamples, (const uint8_t**)frame->extended_data, frame->nb_samples);
			if (ret < 0) {
				cerr << "Error while converting" << endl;
				dataSize = 0;
				break;
			}
			int dstBufSize = av_samples_get_buffer_size(&dstLineSize, dstNbChannels, ret, *dstSampleFmt, 1);
			if (dstBufSize < 0) {
				cerr << "Error av_samples_get_buffer_size()" << endl;
				dataSize = 0;
				break;
			}
			if (dataSize + dstBufSize > bufSize) {
				cerr << "dataSize + dstBufSize > bufSize" << endl;
				dataSize = 0;
				break;
			}
			memcpy((uint8_t*)buf + bufIndex, dstData[0], dstBufSize);
			bufIndex += dstBufSize;
			dataSize += dstBufSize;
			if (dstData)
				av_freep(&dstData[0]);
			av_freep(&dstData);
		}
		else {
			packet->size = 0;
			packet->data = NULL;
		}
	}
	av_packet_unref(packet);
	av_free(frame);

	return dataSize;
}

int main(int argc, char* argv[])
{
	AVCodec* audioCodec;
	AVFormatContext* formatContext;
	AVStream* audioStream;
	AVCodecContext* codecContext;
	playing = 0;
	ALCdevice* dev;
	ALCcontext* ctx;
	ALuint source, buffers[NUM_BUFFERS];
	uint8_t audioBuf[AVCODEC_MAX_AUDIO_FRAME_SIZE];
	unsigned int audioBufSize = 0;

	//string url= "F:/forgit/gitrep/mp4player/mp4player/42stest.mp4";
	string url;
	cout << "Enter audio url:" << flush;
	cin >> url;

	avformat_network_init();
	formatContext = avformat_alloc_context();
	if (avformat_open_input(&formatContext, url.c_str(), NULL, NULL) != 0) {
		cerr << "Error opening the file" << endl;
		return 1;
	}
	if (avformat_find_stream_info(formatContext, NULL) < 0) {
		avformat_close_input(&formatContext);
		cerr << "Error finding the stream info" << endl;
		return 1;
	}
	av_dump_format(formatContext, 0, url.c_str(), false);
	int streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &audioCodec, 0);
	if (streamIndex < 0) {
		avformat_close_input(&formatContext);
		cerr << "Could not find any audio stream in the file" << endl;
		return 1;
	}
	//openal读取流跟
	audioStream = formatContext->streams[streamIndex];
	codecContext = avcodec_alloc_context3(NULL);
	avcodec_parameters_to_context(codecContext, audioStream->codecpar);
	audioCodec = avcodec_find_decoder(codecContext->codec_id);

	if (avcodec_open2(codecContext, audioCodec, NULL) != 0) {
		avformat_close_input(&formatContext);
		cout << "Couldn't open the context with the decoder" << endl;
		return 1;
	}

	//输出数据初始化
	uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
	int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
	int out_nb_samples = codecContext->frame_size;
	enum AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;//enum class警告
	int out_sample_rate = codecContext->sample_rate;
	
	//重采样设置
	struct SwrContext* swr = swr_alloc();
	if (!swr) {
		fprintf(stderr, "Could not allocate resampler context\n");
		avcodec_close(codecContext);
		avformat_close_input(&formatContext);
		return 1;
	}
	if (codecContext->channel_layout == 0)
		codecContext->channel_layout = av_get_default_channel_layout(codecContext->channels);
	swr = swr_alloc_set_opts(swr, out_channel_layout, out_sample_fmt, out_sample_rate,
		codecContext->channel_layout, codecContext->sample_fmt, codecContext->sample_rate, 0, NULL);
	swr_init(swr);

	//OpenAL
	//初始化
	dev = alcOpenDevice(NULL);
	if (!dev) {
		swr_free(&swr);
		avcodec_close(codecContext);
		avformat_close_input(&formatContext);
		return -1;
	}
	ctx = alcCreateContext(dev, NULL);
	alcMakeContextCurrent(ctx);
	if (!ctx) {
		swr_free(&swr);
		avcodec_close(codecContext);
		avformat_close_input(&formatContext);
		return -1;
	}
	//buffer
	alGenBuffers(NUM_BUFFERS, buffers);
	alGenSources(1, &source);
	if (alGetError() != AL_NO_ERROR) {
		swr_free(&swr);
		avcodec_close(codecContext);
		avformat_close_input(&formatContext);
		return 1;
	}

	// 队列，分解后保存
	PacketQueue pktQueue;
	initPacketQueue(&pktQueue);
	AVPacket readingPacket;
	av_init_packet(&readingPacket);

	while (av_read_frame(formatContext, &readingPacket) >= 0) {
		//查看是否对应
		if (readingPacket.stream_index == audioStream->index) {
			pushPacketToPacketQueue(&pktQueue, &readingPacket);
		}
		else {
			av_packet_unref(&readingPacket);
		}
	}

	// 缓冲
	for (int i = 0; i < NUM_BUFFERS; i++) {
		//获取队列分组
		AVPacket decodingPacket;
		if (popPacketFromPacketQueue(&pktQueue, &decodingPacket) < 0) {
			cerr << "error." << endl;
			break;
		}
		//数据包解码并转换数据
		audioBufSize = decode(&audioBuf[0], sizeof(audioBuf), &decodingPacket,
			codecContext, swr, out_sample_rate, out_channels, &out_sample_fmt);
		//写入OpenAL缓冲
		alBufferData(buffers[i], AL_FORMAT_STEREO16, audioBuf, audioBufSize, out_sample_rate);
		if (alGetError() != AL_NO_ERROR) {
			cerr << "Error Buffer :(" << endl;
			av_packet_unref(&decodingPacket);
			continue;
		}
		av_packet_unref(&decodingPacket);
	}
	//缓冲器追加到队列，开始播放
	alSourceQueueBuffers(source, NUM_BUFFERS, buffers);
	alSourcePlay(source);
	if (alGetError() != AL_NO_ERROR) {
		cerr << "Error starting." << endl;
		return 1;
	}
	else {
		cout << "Audio playing..." << endl;
		playing = 1;
	}

	//循环播放直到无缓冲存储
	while (pktQueue.numOfPackets && playing) {
		ALint val;
		alGetSourcei(source, AL_BUFFERS_PROCESSED, &val);
		if (val <= 0) {
			//应该做睡眠减少处理，可空
			continue;
		}
		//重复解码-缓冲-入队列-播放-清缓存
		AVPacket decodingPacket;
		if (popPacketFromPacketQueue(&pktQueue, &decodingPacket) < 0) {
			cerr << "error." << endl;
			break;
		}
		audioBufSize = decode(&audioBuf[0], sizeof(audioBuf), &decodingPacket,
			codecContext, swr, out_sample_rate, out_channels, &out_sample_fmt);
		if (audioBufSize <= 0) {
			continue;
		}
		ALuint buffer;
		alSourceUnqueueBuffers(source, 1, &buffer);
		alBufferData(buffer, AL_FORMAT_STEREO16, audioBuf, audioBufSize, out_sample_rate);
		if (alGetError() != AL_NO_ERROR){
			return -1;
		}
		alSourceQueueBuffers(source, 1, &buffer);
		if (alGetError() != AL_NO_ERROR){
			return -1;
		}
		alGetSourcei(source, AL_SOURCE_STATE, &val);
		if (val != AL_PLAYING)
			alSourcePlay(source);

		av_packet_unref(&decodingPacket);
	}
	// 剩余未处理直接释放
	while (pktQueue.numOfPackets) {
		AVPacket decodingPacket;
		if (popPacketFromPacketQueue(&pktQueue, &decodingPacket) < 0)
			continue;
		av_packet_unref(&decodingPacket);
	}

	//全关掉
	swr_free(&swr);
	avformat_close_input(&formatContext);
	avcodec_close(codecContext);
	avformat_free_context(formatContext);
	alDeleteSources(1, &source);
	alDeleteBuffers(NUM_BUFFERS, buffers);
	alcMakeContextCurrent(NULL);
	alcDestroyContext(ctx);
	alcCloseDevice(dev);

	return 0;
}