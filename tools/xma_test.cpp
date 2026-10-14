// Decodes a file of raw 2048-byte XMA packets (HC09_XMA_DUMP output) with FFmpeg's XMA2
// decoder under every sample rate / channel count, reporting errors and samples produced.
// Usage: XmaTest build/xma_dump.bin

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return 1;
    FILE* file = std::fopen(argv[1], "rb");
    if (!file)
        return 1;
    std::vector<uint8_t> data;
    uint8_t chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
        data.insert(data.end(), chunk, chunk + got);
    std::fclose(file);
    const size_t packets = data.size() / 2048;
    av_log_set_level(AV_LOG_QUIET);

    for (int rate : { 24000, 32000, 44100, 48000 })
    {
        for (int channels : { 1, 2 })
        {
            const AVCodec* xma = avcodec_find_decoder(AV_CODEC_ID_XMA2);
            AVCodecContext* codec = avcodec_alloc_context3(xma);
            codec->sample_rate = rate;
            codec->block_align = 2048;
            av_channel_layout_default(&codec->ch_layout, channels);
            codec->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
            codec->extradata_size = 34;
            codec->extradata[0] = 1;
            const uint32_t mask = channels == 2 ? 3 : 4;
            std::memcpy(codec->extradata + 2, &mask, 4);
            if (avcodec_open2(codec, xma, nullptr) < 0)
            {
                std::printf("%5d Hz %d ch: open failed\n", rate, channels);
                continue;
            }
            AVPacket* packet = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();
            int errors = 0;
            long samples = 0;
            float peak = 0;
            for (size_t i = 0; i < packets; i++)
            {
                av_new_packet(packet, 2048);
                std::memcpy(packet->data, data.data() + i * 2048, 2048);
                if (avcodec_send_packet(codec, packet) < 0)
                    errors++;
                av_packet_unref(packet);
                while (avcodec_receive_frame(codec, frame) == 0)
                {
                    samples += frame->nb_samples;
                    for (int s = 0; s < frame->nb_samples; s++)
                    {
                        const float v = reinterpret_cast<float*>(frame->extended_data[0])[s];
                        peak = v > peak ? v : (-v > peak ? -v : peak);
                    }
                    av_frame_unref(frame);
                }
            }
            std::printf("%5d Hz %d ch: %zu packets, %d errors, %ld samples, peak %.3f\n", rate, channels, packets, errors, samples, peak);
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&codec);
        }
    }
    return 0;
}
