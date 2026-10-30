#include "../../ps2_savestate.h"
#include "ps2_host_settings.h"
#include "ps2_debug_trace.h"
#include "Common.h"
#include "MPEG.h"
#include "runtime/ee_scheduler.h"

#if !defined(PS2X_HAS_FFMPEG)
#define PS2X_HAS_FFMPEG 1
#endif

#if PS2X_HAS_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}
#endif

#include <chrono>
#include <thread>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <memory>

#include "Syscalls/Helpers/State.h"

uint64_t ps2CurrentVsyncTick(); // ps2_pad.cpp (streamConsumerStuck; also the PS2X_MPEG_CHLOG log)

namespace ps2_stubs
{
    namespace
    {
        struct MpegDecodedFrame
        {
            int width = 0;
            int height = 0;
            int repeatPict = 0;
            int64_t pts90k = -1;
            // Video bytes fed to the decoder when this frame came out; used to model how far
            // the IPU has read into the game's bitstream ring buffer.
            uint64_t videoBytesMark = 0;
            std::vector<uint8_t> rgba;
        };

#if PS2X_HAS_FFMPEG
        std::string ffmpegErrorString(int err)
        {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
            if (av_strerror(err, buffer.data(), buffer.size()) < 0)
            {
                return "unknown FFmpeg error";
            }
            return std::string(buffer.data());
        }

        void configureFfmpegLogLevel()
        {
            static std::once_flag s_once;
            std::call_once(s_once, []
                           {
#if AGRESSIVE_LOGS
                               av_log_set_level(AV_LOG_WARNING);
#else
                               av_log_set_level(AV_LOG_ERROR);
#endif
                           });
        }

        class MpegFfmpegDecoder
        {
        public:
            MpegFfmpegDecoder() = default;

            ~MpegFfmpegDecoder()
            {
                reset();
            }

            MpegFfmpegDecoder(const MpegFfmpegDecoder &) = delete;
            MpegFfmpegDecoder &operator=(const MpegFfmpegDecoder &) = delete;

            bool feed(const uint8_t *data, size_t size, std::deque<MpegDecodedFrame> &frames, int64_t pts90k = -1, int64_t dts90k = -1)
            {
                if (!data || size == 0)
                {
                    return true;
                }

                if (!ensureInitialized())
                {
                    return false;
                }

                static uint32_t s_feedLogCount = 0u;
                const bool shouldLog = (s_feedLogCount < 32u);
                if (shouldLog)
                    ++s_feedLogCount;

                size_t totalParsed = 0u;
                size_t totalPacketsSent = 0u;
                size_t framesBefore = frames.size();

                const uint8_t *cursor = data;
                size_t remaining = size;
                int64_t parserPts = pts90k >= 0 ? pts90k : AV_NOPTS_VALUE;
                int64_t parserDts = dts90k >= 0 ? dts90k : AV_NOPTS_VALUE;
                while (remaining > 0)
                {
                    uint8_t *packetData = nullptr;
                    int packetSize = 0;
                    const int chunk = static_cast<int>(std::min<size_t>(
                        remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
                    const int used = av_parser_parse2(
                        m_parser,
                        m_codecCtx,
                        &packetData,
                        &packetSize,
                        cursor,
                        chunk,
                        parserPts,
                        parserDts,
                        0);
                    if (used < 0)
                    {
                        std::cerr << "[MPEG] parser failed: " << ffmpegErrorString(used) << std::endl;
                        return false;
                    }
                    if (used == 0 && packetSize == 0)
                    {
                        break;
                    }

                    totalParsed += static_cast<size_t>(used);
                    cursor += used;
                    remaining -= static_cast<size_t>(used);

                    if (used > 0)
                    {
                        parserPts = AV_NOPTS_VALUE;
                        parserDts = AV_NOPTS_VALUE;
                    }

                    if (packetSize > 0)
                    {
                        ++totalPacketsSent;
                        if (!sendPacket(packetData, static_cast<size_t>(packetSize), frames, m_parser->pts, m_parser->dts))
                        {
                            return false;
                        }
                    }
                }

                if (shouldLog)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:feed] inSize=" << size
                                  << " parsed=" << totalParsed
                                  << " packets=" << totalPacketsSent
                                  << " newFrames=" << (frames.size() - framesBefore)
                                  << " totalFrames=" << frames.size()
                                  << std::endl;
                    });
                }

                return true;
            }

            bool flush(std::deque<MpegDecodedFrame> &frames)
            {
                if (!m_initialized || m_drained)
                {
                    return true;
                }

                if (m_parser)
                {
                    uint8_t *packetData = nullptr;
                    int packetSize = 0;
                    const int used = av_parser_parse2(
                        m_parser,
                        m_codecCtx,
                        &packetData,
                        &packetSize,
                        nullptr,
                        0,
                        AV_NOPTS_VALUE,
                        AV_NOPTS_VALUE,
                        0);
                    (void)used;
                    if (packetSize > 0 && !sendPacket(packetData, static_cast<size_t>(packetSize), frames, m_parser->pts, m_parser->dts))
                    {
                        return false;
                    }
                }

                const int sendRet = avcodec_send_packet(m_codecCtx, nullptr);
                if (sendRet < 0 && sendRet != AVERROR_EOF)
                {
                    std::cerr << "[MPEG] decoder flush failed: " << ffmpegErrorString(sendRet) << std::endl;
                    return false;
                }

                const bool ok = receiveFrames(frames);
                m_drained = true;
                return ok;
            }

            void reset()
            {
                if (m_swsCtx)
                {
                    sws_freeContext(m_swsCtx);
                    m_swsCtx = nullptr;
                }
                if (m_frame)
                {
                    av_frame_free(&m_frame);
                }
                if (m_packet)
                {
                    av_packet_free(&m_packet);
                }
                if (m_codecCtx)
                {
                    avcodec_free_context(&m_codecCtx);
                }
                if (m_parser)
                {
                    av_parser_close(m_parser);
                    m_parser = nullptr;
                }

                m_swsWidth = 0;
                m_swsHeight = 0;
                m_swsFormat = AV_PIX_FMT_NONE;
                m_initialized = false;
                m_drained = false;
            }

        private:
            bool ensureInitialized()
            {
                if (m_initialized)
                {
                    return true;
                }

                configureFfmpegLogLevel();

                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                if (!codec)
                {
                    std::cerr << "[MPEG] FFmpeg MPEG-2 decoder not found." << std::endl;
                    return false;
                }

                m_parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
                if (!m_parser)
                {
                    std::cerr << "[MPEG] FFmpeg MPEG-video parser not found." << std::endl;
                    return false;
                }

                m_codecCtx = avcodec_alloc_context3(codec);
                m_frame = av_frame_alloc();
                m_packet = av_packet_alloc();
                if (!m_codecCtx || !m_frame || !m_packet)
                {
                    std::cerr << "[MPEG] failed to allocate FFmpeg decoder state." << std::endl;
                    reset();
                    return false;
                }

                m_codecCtx->thread_count = 1;
                m_codecCtx->pkt_timebase = AVRational{1, 90000};
                // feedElementaryStream() does not create the decoder until a valid
                // MPEG sequence header has been found.  Dropping non-key pictures
                // here therefore throws away real presentation frames and makes
                // movies finish early once the EE is fast enough to drain them.
                m_codecCtx->skip_frame = AVDISCARD_DEFAULT;
                m_codecCtx->err_recognition = 0;
                const int ret = avcodec_open2(m_codecCtx, codec, nullptr);
                if (ret < 0)
                {
                    std::cerr << "[MPEG] failed to open MPEG decoder: " << ffmpegErrorString(ret) << std::endl;
                    reset();
                    return false;
                }

                m_initialized = true;
                m_drained = false;
                return true;
            }

            bool sendPacket(const uint8_t *data,
                            size_t size,
                            std::deque<MpegDecodedFrame> &frames,
                            int64_t pts = AV_NOPTS_VALUE,
                            int64_t dts = AV_NOPTS_VALUE)
            {
                if (!data || size == 0)
                {
                    return true;
                }

                av_packet_unref(m_packet);
                const int allocRet = av_new_packet(m_packet, static_cast<int>(size));
                if (allocRet < 0)
                {
                    std::cerr << "[MPEG] failed to allocate packet: " << ffmpegErrorString(allocRet) << std::endl;
                    return false;
                }
                std::memcpy(m_packet->data, data, size);
                m_packet->pts = pts;
                m_packet->dts = dts;

                int ret = avcodec_send_packet(m_codecCtx, m_packet);
                if (ret == AVERROR(EAGAIN))
                {
                    if (!receiveFrames(frames))
                    {
                        av_packet_unref(m_packet);
                        return false;
                    }
                    ret = avcodec_send_packet(m_codecCtx, m_packet);
                }
                av_packet_unref(m_packet);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                {
                    static uint32_t s_rejectedPacketLogCount = 0u;
                    if (s_rejectedPacketLogCount < 32u)
                    {
                        std::cerr << "[MPEG] decoder rejected packet, dropping: "
                                  << ffmpegErrorString(ret) << std::endl;
                        ++s_rejectedPacketLogCount;
                    }
                    return true;
                }

                return receiveFrames(frames);
            }

            bool receiveFrames(std::deque<MpegDecodedFrame> &frames)
            {
                while (true)
                {
                    const int ret = avcodec_receive_frame(m_codecCtx, m_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    {
                        return true;
                    }
                    if (ret < 0)
                    {
                        static uint32_t s_receiveErrorLogCount = 0u;
                        if (s_receiveErrorLogCount < 32u)
                        {
                            std::cerr << "[MPEG] decoder receive failed, dropping: "
                                      << ffmpegErrorString(ret) << std::endl;
                            ++s_receiveErrorLogCount;
                        }
                        return true;
                    }

                    if (!convertFrame(frames))
                    {
                        av_frame_unref(m_frame);
                        return false;
                    }
                    av_frame_unref(m_frame);
                }
            }

            bool convertFrame(std::deque<MpegDecodedFrame> &frames)
            {
                const int width = m_frame->width;
                const int height = m_frame->height;
                const AVPixelFormat srcFormat = static_cast<AVPixelFormat>(m_frame->format);
                if (width <= 0 || height <= 0 || srcFormat == AV_PIX_FMT_NONE)
                {
                    return false;
                }

                if (!m_swsCtx ||
                    m_swsWidth != width ||
                    m_swsHeight != height ||
                    m_swsFormat != srcFormat)
                {
                    if (m_swsCtx)
                    {
                        sws_freeContext(m_swsCtx);
                        m_swsCtx = nullptr;
                    }
                    m_swsCtx = sws_getContext(
                        width,
                        height,
                        srcFormat,
                        width,
                        height,
                        AV_PIX_FMT_RGBA,
                        SWS_BILINEAR,
                        nullptr,
                        nullptr,
                        nullptr);
                    if (!m_swsCtx)
                    {
                        std::cerr << "[MPEG] failed to create FFmpeg scaler." << std::endl;
                        return false;
                    }
                    m_swsWidth = width;
                    m_swsHeight = height;
                    m_swsFormat = srcFormat;
                }

                MpegDecodedFrame decoded;
                decoded.width = width;
                decoded.height = height;
                decoded.repeatPict = std::max(0, m_frame->repeat_pict);
                decoded.pts90k = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                     ? m_frame->best_effort_timestamp
                                     : -1;
                decoded.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

                uint8_t *dstData[4] = {decoded.rgba.data(), nullptr, nullptr, nullptr};
                int dstLinesize[4] = {width * 4, 0, 0, 0};
                const int scaledRows = sws_scale(
                    m_swsCtx,
                    m_frame->data,
                    m_frame->linesize,
                    0,
                    height,
                    dstData,
                    dstLinesize);
                if (scaledRows <= 0)
                {
                    std::cerr << "[MPEG] FFmpeg scaler produced no rows." << std::endl;
                    return false;
                }

                frames.push_back(std::move(decoded));
                return true;
            }

            AVCodecParserContext *m_parser = nullptr;
            AVCodecContext *m_codecCtx = nullptr;
            AVFrame *m_frame = nullptr;
            AVPacket *m_packet = nullptr;
            SwsContext *m_swsCtx = nullptr;
            int m_swsWidth = 0;
            int m_swsHeight = 0;
            AVPixelFormat m_swsFormat = AV_PIX_FMT_NONE;
            bool m_initialized = false;
            bool m_drained = false;
        };
#else
        // TODO
        class MpegFfmpegDecoder
        {
        public:
            bool feed(const uint8_t *, size_t, std::deque<MpegDecodedFrame> &, int64_t = -1, int64_t = -1)
            {
                static bool s_warnedNoFfmpeg = false;
                if (!s_warnedNoFfmpeg)
                {
                    std::cerr << "[MPEG] runtime built without FFmpeg; MPEG video decode is disabled." << std::endl;
                    s_warnedNoFfmpeg = true;
                }
                return false;
            }

            bool flush(std::deque<MpegDecodedFrame> &)
            {
                return true;
            }

            void reset() {}
        };
#endif

        struct MpegRegisteredCallback
        {
            uint32_t type = 0u;
            uint32_t streamId = 0u;
            uint32_t func = 0u;
            uint32_t data = 0u;
            uint32_t handle = 0u;
            bool stream = false;
        };

        constexpr uint64_t kPictureClockOne = 1ull << 32u;
        // NTSC-style fields at ~59.94 Hz to keep MPEG timing yet (29.97 fps).
        constexpr uint64_t kDefaultPictureIntervalQ32 = 2ull * kPictureClockOne;
        constexpr size_t kMpegTimingScanLimit = 4096u;
        constexpr size_t kMaxDecodedPicturesAhead = 8u;
        constexpr size_t kMaxDecodedPicturesLinearDemux = 256u;

        struct MpegPlaybackState
        {
            uint32_t picturesServed = 0u;
            uint32_t width = 320u;
            uint32_t height = 240u;
            uint32_t decodeMode = 0u;
            uint32_t imageBufferAddr = 0u;
            bool sawInput = false;
            bool sawSequenceEnd = false;
            bool streamEnded = false;
            bool sawProgramEnd = false; // the PSS data itself ended (MPEG program end code)
            bool decoderFailed = false;
            uint64_t cdStreamGeneration = 0u;
            bool waitingForVideoSequenceHeader = true;
            std::vector<uint8_t> videoSequenceSyncBuffer;
            std::vector<uint8_t> pssBuffer;
            std::vector<uint32_t> pssGuestAddrs;
            std::deque<MpegDecodedFrame> decodedFrames;
            std::unique_ptr<MpegFfmpegDecoder> decoder;
            uint8_t frameRateCode = 0u;
            uint8_t frameRateExtensionN = 0u;
            uint8_t frameRateExtensionD = 0u;
            bool hasFrameRateExtension = false;
            std::vector<uint8_t> videoTimingScanBuffer;
            uint64_t pictureIntervalQ32 = kDefaultPictureIntervalQ32;
            uint64_t nextPictureTickQ32 = std::numeric_limits<uint64_t>::max();
            uint64_t presentationEndTickQ32 = std::numeric_limits<uint64_t>::max();
            int64_t firstPresentedPts90k = -1;
            uint64_t ptsPresentationBaseTickQ32 = 0u;
            uint64_t videoBytesFed = 0u;
            uint64_t videoBytesPresented = 0u;
            bool nodataCallbackDispatched = false;
            bool fedViaAddBs = false;
        };

        struct MpegStreamCallbackEvent
        {
            uint32_t mpegAddr = 0u;
            uint32_t streamType = 0u;
            uint32_t dataAddr = 0u;
            uint32_t len = 0u;
            uint64_t pts = 0xFFFFFFFFFFFFFFFFull;
            uint64_t dts = 0xFFFFFFFFFFFFFFFFull;
            std::vector<MpegRegisteredCallback> callbacks;
            // Snapshot of the packet payload: callbacks run later on the EE scheduler, when the game may already
            // have refilled its PSS ring (and a packet reassembled across calls or ring wrap is not contiguous there).
            std::vector<uint8_t> payload;
        };

        struct MpegStubState
        {
            bool initialized = false;
            uint32_t nextCallbackHandle = 1u;
            uint64_t cdStreamGeneration = 0u;
            uint64_t cdStreamBytesProduced = 0u;
            uint64_t cdStreamBytesDemuxed = 0u;
            bool cdStreamEofPending = false;
            bool currentCdStreamEofSeen = false;
            uint32_t feedEsTraceCount = 0u;
            uint32_t demuxPssTraceCount = 0u;
            uint32_t demuxRingTraceCount = 0u;
            uint32_t getPictureWaitTraceCount = 0u;
            uint32_t pictureTraceCount = 0u;
            uint32_t isEndTraceCount = 0u;
            std::unordered_map<uint32_t, std::vector<MpegRegisteredCallback>> callbacksByMpeg;
            std::unordered_map<uint32_t, MpegPlaybackState> playbackByMpeg;
            // Decoders deleted and not created again: a picture request that resumes after the delete (a thread that was
            // waiting in sceMpegGetPicture) must not mark the movie as playing again, or the window stays boxed 4:3.
            std::unordered_set<uint32_t> deletedMpeg;
        };

        std::mutex g_mpeg_stub_mutex;
        constexpr uint32_t kMpegPictureWaitType = 1u;
        MpegStubState g_mpeg_stub_state;

        // TODO this resolution should follow runtime resolution
        constexpr uint32_t kStubMovieWidth = 320u;
        constexpr uint32_t kStubMovieHeight = 240u;
        constexpr uint32_t kMpegStrM2V = 0u;
        constexpr uint32_t kMpegStrPCM = 1u;
        constexpr uint32_t kMpegStrADPCM = 2u;
        // libmpeg sceMpegStrDATA: private_stream_1 packets whose sub-stream byte is 0xFF; byte 3 carries the
        // channel the game registered (Zone of the Enders: its movie audio, channels 1 and 7).
        constexpr uint32_t kMpegStrDATA = 4u;
        constexpr uint8_t kMpegPackHeader = 0xBAu;
        constexpr uint8_t kMpegSystemHeader = 0xBBu;
        constexpr uint8_t kMpegProgramEnd = 0xB9u;
        constexpr uint8_t kMpegSequenceEnd = 0xB7u;
        constexpr uint8_t kMpegPrivateStream1 = 0xBDu;
        constexpr size_t kStartCodeNotFound = std::numeric_limits<size_t>::max();
        constexpr uint32_t kMpegCallbackDataSize = 0x20u;

        uint64_t mpegPictureIntervalQ32(uint8_t frameRateCode, uint8_t frameRateExtensionN = 0u, uint8_t frameRateExtensionD = 0u)
        {
            uint64_t frameRateNumerator = 0u;
            uint64_t frameRateDenominator = 1u;
            switch (frameRateCode)
            {
            case 1u:
                frameRateNumerator = 24000u;
                frameRateDenominator = 1001u;
                break;
            case 2u:
                frameRateNumerator = 24u;
                break;
            case 3u:
                frameRateNumerator = 25u;
                break;
            case 4u:
                frameRateNumerator = 30000u;
                frameRateDenominator = 1001u;
                break;
            case 5u:
                frameRateNumerator = 30u;
                break;
            case 6u:
                frameRateNumerator = 50u;
                break;
            case 7u:
                frameRateNumerator = 60000u;
                frameRateDenominator = 1001u;
                break;
            case 8u:
                frameRateNumerator = 60u;
                break;
            default:
                return 0u;
            }

            const uint64_t extensionNumerator = static_cast<uint64_t>(frameRateExtensionN) + 1u;
            const uint64_t extensionDenominator = static_cast<uint64_t>(frameRateExtensionD) + 1u;
            const uint64_t denominator = 1001u * frameRateNumerator * extensionNumerator;
            const uint64_t numerator = (60000u * frameRateDenominator * extensionDenominator) << 32u;
            return std::max(kPictureClockOne, (numerator + denominator / 2u) / denominator);
        }

        uint32_t readMpegBits(const uint8_t *data, size_t bitOffset, uint32_t bitCount)
        {
            uint32_t value = 0u;
            for (uint32_t bit = 0u; bit < bitCount; ++bit)
            {
                const size_t absoluteBit = bitOffset + bit;
                const uint8_t source = data[absoluteBit >> 3u];
                value = (value << 1u) | ((source >> (7u - static_cast<uint32_t>(absoluteBit & 7u))) & 1u);
            }
            return value;
        }

        void updateMpegPictureTiming(MpegPlaybackState &playback, const uint8_t *data, size_t size)
        {
            if (!data || size == 0u)
            {
                return;
            }

            playback.videoTimingScanBuffer.insert(playback.videoTimingScanBuffer.end(), data, data + size);
            if (playback.videoTimingScanBuffer.size() > kMpegTimingScanLimit)
            {
                const size_t discard = playback.videoTimingScanBuffer.size() - kMpegTimingScanLimit;
                playback.videoTimingScanBuffer.erase(playback.videoTimingScanBuffer.begin(), playback.videoTimingScanBuffer.begin() + static_cast<std::ptrdiff_t>(discard));
            }

            const std::vector<uint8_t> &buffer = playback.videoTimingScanBuffer;
            size_t lastSequenceHeader = kStartCodeNotFound;
            for (size_t i = 0u; i + 7u < buffer.size(); ++i)
            {
                if (buffer[i + 0u] == 0x00u &&
                    buffer[i + 1u] == 0x00u &&
                    buffer[i + 2u] == 0x01u &&
                    buffer[i + 3u] == 0xB3u)
                {
                    const uint8_t frameRateCode = buffer[i + 7u] & 0x0Fu;
                    if (mpegPictureIntervalQ32(frameRateCode) != 0u)
                    {
                        lastSequenceHeader = i;
                    }
                }
            }

            if (lastSequenceHeader == kStartCodeNotFound)
            {
                return;
            }

            playback.frameRateCode = buffer[lastSequenceHeader + 7u] & 0x0Fu;
            playback.frameRateExtensionN = 0u;
            playback.frameRateExtensionD = 0u;
            playback.hasFrameRateExtension = false;
            playback.pictureIntervalQ32 = mpegPictureIntervalQ32(playback.frameRateCode);

            for (size_t i = lastSequenceHeader + 8u; i + 9u < buffer.size(); ++i)
            {
                if (buffer[i + 0u] != 0x00u ||
                    buffer[i + 1u] != 0x00u ||
                    buffer[i + 2u] != 0x01u)
                {
                    continue;
                }

                if (buffer[i + 3u] == 0xB3u)
                {
                    break;
                }
                if (buffer[i + 3u] != 0xB5u)
                {
                    continue;
                }

                const uint8_t *extension = buffer.data() + i + 4u;
                if (readMpegBits(extension, 0u, 4u) != 1u)
                {
                    continue;
                }

                playback.frameRateExtensionN = static_cast<uint8_t>(readMpegBits(extension, 41u, 2u));
                playback.frameRateExtensionD = static_cast<uint8_t>(readMpegBits(extension, 43u, 5u));
                playback.hasFrameRateExtension = true;
                playback.pictureIntervalQ32 = mpegPictureIntervalQ32(
                    playback.frameRateCode,
                    playback.frameRateExtensionN,
                    playback.frameRateExtensionD);
                break;
            }

            if (playback.pictureIntervalQ32 == 0u)
            {
                playback.pictureIntervalQ32 = kDefaultPictureIntervalQ32;
            }
        }

        uint64_t decodedFrameIntervalQ32(const MpegPlaybackState &playback,
                                         const MpegDecodedFrame &frame)
        {
            const uint64_t base = playback.pictureIntervalQ32 != 0u
                                      ? playback.pictureIntervalQ32
                                      : kDefaultPictureIntervalQ32;
            const uint64_t fields = static_cast<uint64_t>(2 + std::max(0, frame.repeatPict));
            return std::max(kPictureClockOne, (base * fields + 1u) / 2u);
        }

        constexpr uint64_t kMpegPtsWrap = 1ull << 33u;
        constexpr uint64_t kMpegPtsHalfWrap = 1ull << 32u;

        int64_t mpegPtsDelta90k(int64_t fromPts, int64_t toPts)
        {
            if (fromPts < 0 || toPts < 0)
            {
                return 0;
            }

            uint64_t from = static_cast<uint64_t>(fromPts) & (kMpegPtsWrap - 1u);
            uint64_t to = static_cast<uint64_t>(toPts) & (kMpegPtsWrap - 1u);
            uint64_t delta = (to - from) & (kMpegPtsWrap - 1u);
            if (delta >= kMpegPtsHalfWrap)
            {
                return -static_cast<int64_t>(kMpegPtsWrap - delta);
            }
            return static_cast<int64_t>(delta);
        }

        uint64_t mpegPtsDeltaToVSyncQ32(uint64_t delta90k)
        {
            // 90 kHz MPEG clock -> NTSC field clock (60000/1001 Hz):
            // fields = pts * 60000 / (90000 * 1001) = pts * 2 / 3003.
            constexpr uint64_t kPtsDivisor = 3003u;
            const uint64_t whole = delta90k / kPtsDivisor;
            const uint64_t remainder = delta90k % kPtsDivisor;
            const uint64_t wholeQ32 = whole * 2u * kPictureClockOne;
            const uint64_t remainderQ32 = ((remainder * 2u * kPictureClockOne) + kPtsDivisor / 2u) / kPtsDivisor;
            return wholeQ32 + remainderQ32;
        }

        uint64_t presentationTickForFrame(MpegPlaybackState &playback, const MpegDecodedFrame &frame, uint64_t currentTickQ32)
        {
            if (frame.pts90k < 0)
            {
                if (playback.nextPictureTickQ32 == std::numeric_limits<uint64_t>::max())
                {
                    playback.nextPictureTickQ32 = currentTickQ32;
                }
                return playback.nextPictureTickQ32;
            }

            if (playback.firstPresentedPts90k < 0)
            {
                playback.firstPresentedPts90k = frame.pts90k;
                playback.ptsPresentationBaseTickQ32 = currentTickQ32;
                return currentTickQ32;
            }

            const int64_t delta = mpegPtsDelta90k(playback.firstPresentedPts90k, frame.pts90k);
            if (delta >= 0)
            {
                return playback.ptsPresentationBaseTickQ32 + mpegPtsDeltaToVSyncQ32(static_cast<uint64_t>(delta));
            }

            const uint64_t backwards = mpegPtsDeltaToVSyncQ32(static_cast<uint64_t>(-delta));
            return playback.ptsPresentationBaseTickQ32 > backwards
                       ? playback.ptsPresentationBaseTickQ32 - backwards
                       : 0u;
        }

        uint32_t align16(uint32_t value)
        {
            return (value + 15u) & ~15u;
        }

        uint32_t readStackArg(uint8_t *rdram, R5900Context *ctx, uint32_t offset)
        {
            if (!rdram || !ctx)
            {
                return 0u;
            }
            return FAST_READ32(getRegU32(ctx, 29) + offset);
        }

        uint32_t readAbiArg4(uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t regArg = getRegU32(ctx, 8);
            if (regArg != 0u)
            {
                return regArg;
            }
            return readStackArg(rdram, ctx, 0x10u);
        }

        MpegPlaybackState &getPlaybackState(uint32_t mpegAddr)
        {
            return g_mpeg_stub_state.playbackByMpeg[mpegAddr];
        }

        MpegPlaybackState makeFreshPlaybackState()
        {
            MpegPlaybackState playback{};
            playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            return playback;
        }

        MpegPlaybackState makeFreshPlaybackStatePreservingConfig(const MpegPlaybackState &oldPlayback)
        {
            MpegPlaybackState playback = makeFreshPlaybackState();
            playback.decodeMode = oldPlayback.decodeMode;
            playback.imageBufferAddr = oldPlayback.imageBufferAddr;
            playback.width = oldPlayback.width;
            playback.height = oldPlayback.height;
            return playback;
        }

        uint16_t readBe16(const uint8_t *p)
        {
            return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | static_cast<uint16_t>(p[1]));
        }

        bool isVideoStreamId(uint8_t streamId)
        {
            return streamId >= 0xE0u && streamId <= 0xEFu;
        }

        bool isAudioStreamId(uint8_t streamId)
        {
            return streamId == kMpegPrivateStream1 || (streamId >= 0xC0u && streamId <= 0xDFu);
        }

        bool isLengthPrefixedHeader(uint8_t streamId)
        {
            switch (streamId)
            {
            case kMpegSystemHeader:
            case 0xBCu: // program_stream_map
            case 0xBEu: // padding_stream
            case 0xBFu: // private_stream_2
            case 0xF0u: // ECM
            case 0xF1u: // EMM
            case 0xF2u: // DSMCC
            case 0xF8u: // ITU-T H.222.1 type E
            case 0xFFu: // program_stream_directory
                return true;
            default:
                return false;
            }
        }

        size_t findStartCode(const std::vector<uint8_t> &buffer, size_t from)
        {
            if (buffer.size() < 4 || from >= buffer.size() - 3u)
            {
                return kStartCodeNotFound;
            }

            for (size_t i = from; i + 3u < buffer.size(); ++i)
            {
                if (buffer[i] == 0x00u && buffer[i + 1u] == 0x00u && buffer[i + 2u] == 0x01u)
                {
                    return i;
                }
            }
            return kStartCodeNotFound;
        }

        bool containsMpegSequenceEnd(const uint8_t *data, size_t size)
        {
            if (!data || size < 4)
            {
                return false;
            }

            for (size_t i = 0; i + 3u < size; ++i)
            {
                if (data[i] == 0x00u &&
                    data[i + 1u] == 0x00u &&
                    data[i + 2u] == 0x01u &&
                    data[i + 3u] == kMpegSequenceEnd)
                {
                    return true;
                }
            }
            return false;
        }

        size_t findMpegSequenceHeader(const uint8_t *data, size_t size)
        {
            if (!data || size < 8u)
            {
                return kStartCodeNotFound;
            }

            for (size_t i = 0; i + 7u < size; ++i)
            {
                if (data[i] == 0x00u &&
                    data[i + 1u] == 0x00u &&
                    data[i + 2u] == 0x01u &&
                    data[i + 3u] == 0xB3u)
                {
                    const uint32_t width = (static_cast<uint32_t>(data[i + 4u]) << 4u) |
                                           (static_cast<uint32_t>(data[i + 5u]) >> 4u);
                    const uint32_t height = ((static_cast<uint32_t>(data[i + 5u]) & 0x0Fu) << 8u) |
                                            static_cast<uint32_t>(data[i + 6u]);
                    const uint8_t frameRateCode = data[i + 7u] & 0x0Fu;
                    if (width != 0u && height != 0u && width <= 4096u && height <= 4096u &&
                        mpegPictureIntervalQ32(frameRateCode) != 0u)
                    {
                        return i;
                    }
                }
            }
            return kStartCodeNotFound;
        }

        struct MpegPesHeader
        {
            size_t payloadOffset = 0u;
            int64_t pts90k = -1;
            int64_t dts90k = -1;
        };

        int64_t decodePesTimestamp90k(const uint8_t *p, size_t remaining)
        {
            if (!p || remaining < 5u)
            {
                return -1;
            }

            const uint64_t value =
                (static_cast<uint64_t>((p[0] >> 1u) & 0x07u) << 30u) |
                (static_cast<uint64_t>(p[1]) << 22u) |
                (static_cast<uint64_t>((p[2] >> 1u) & 0x7Fu) << 15u) |
                (static_cast<uint64_t>(p[3]) << 7u) |
                static_cast<uint64_t>((p[4] >> 1u) & 0x7Fu);
            return static_cast<int64_t>(value & (kMpegPtsWrap - 1u));
        }

        MpegPesHeader parsePesHeader(const uint8_t *packet, size_t packetSize)
        {
            MpegPesHeader result{};
            result.payloadOffset = packetSize;
            if (!packet || packetSize <= 6u)
            {
                return result;
            }

            size_t pos = 6u;
            if (packetSize >= 9u && (packet[pos] & 0xC0u) == 0x80u)
            {
                const uint8_t ptsDtsFlags = packet[pos + 1u] & 0xC0u;
                const size_t headerDataLength = static_cast<size_t>(packet[pos + 2u]);
                const size_t optionalStart = 9u;
                const size_t optionalEnd = std::min(packetSize, optionalStart + headerDataLength);
                if ((ptsDtsFlags == 0x80u || ptsDtsFlags == 0xC0u) && optionalEnd >= optionalStart + 5u)
                {
                    result.pts90k = decodePesTimestamp90k(packet + optionalStart, optionalEnd - optionalStart);
                }
                if (ptsDtsFlags == 0xC0u && optionalEnd >= optionalStart + 10u)
                {
                    result.dts90k = decodePesTimestamp90k(packet + optionalStart + 5u, optionalEnd - optionalStart - 5u);
                }
                result.payloadOffset = optionalEnd;
                return result;
            }

            // MPEG-1 PES.
            while (pos < packetSize && packet[pos] == 0xFFu)
            {
                ++pos;
            }
            if (pos + 1u < packetSize && (packet[pos] & 0xC0u) == 0x40u)
            {
                pos += 2u;
            }
            if (pos >= packetSize)
            {
                return result;
            }

            const uint8_t marker = packet[pos] & 0xF0u;
            if (marker == 0x20u && pos + 5u <= packetSize)
            {
                result.pts90k = decodePesTimestamp90k(packet + pos, packetSize - pos);
                pos += 5u;
            }
            else if (marker == 0x30u && pos + 10u <= packetSize)
            {
                result.pts90k = decodePesTimestamp90k(packet + pos, packetSize - pos);
                result.dts90k = decodePesTimestamp90k(packet + pos + 5u, packetSize - pos - 5u);
                pos += 10u;
            }
            else if (packet[pos] == 0x0Fu)
            {
                ++pos;
            }

            result.payloadOffset = std::min(packetSize, pos);
            return result;
        }

        void stampDecodedFrames(MpegPlaybackState &playback)
        {
            for (auto it = playback.decodedFrames.rbegin(); it != playback.decodedFrames.rend() && it->videoBytesMark == 0u; ++it)
            {
                it->videoBytesMark = playback.videoBytesFed;
            }
        }

        struct DecodedFrameStamper
        {
            MpegPlaybackState &playback;
            ~DecodedFrameStamper() { stampDecodedFrames(playback); }
        };

        void flushDecoderIfEnded(MpegPlaybackState &playback)
        {
            if (playback.streamEnded && playback.decoder)
            {
                playback.decoder->flush(playback.decodedFrames);
                stampDecodedFrames(playback);
            }
        }

        void feedElementaryStream(MpegPlaybackState &playback, const uint8_t *data, size_t size, int64_t pts90k = -1, int64_t dts90k = -1)
        {
            if (!data || size == 0)
            {
                return;
            }
            DecodedFrameStamper stamper{playback};
            playback.videoBytesFed += size;

            const uint32_t feedEsIdx = g_mpeg_stub_state.feedEsTraceCount++;
            if (feedEsIdx < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    char hexBuf[16] = {};
                    for (size_t i = 0; i < std::min<size_t>(4u, size); ++i)
                    {
                        ::snprintf(hexBuf + i * 2, 3, "%02x", data[i]);
                    }
                    std::cerr << "[MPEG:feedES] #" << feedEsIdx
                              << " size=" << size
                              << " first4=" << hexBuf
                              << " decoderFailed=" << playback.decoderFailed
                              << " waitSeq=" << playback.waitingForVideoSequenceHeader
                              << std::endl;
                });
            }

            playback.sawInput = true;
            updateMpegPictureTiming(playback, data, size);
            if (playback.waitingForVideoSequenceHeader)
            {
                playback.videoSequenceSyncBuffer.insert(
                    playback.videoSequenceSyncBuffer.end(),
                    data,
                    data + size);

                constexpr size_t kMaxVideoSequenceSyncBytes = 2u * 1024u * 1024u;
                if (playback.videoSequenceSyncBuffer.size() > kMaxVideoSequenceSyncBytes)
                {
                    const size_t keepFrom = playback.videoSequenceSyncBuffer.size() - 3u;
                    playback.videoSequenceSyncBuffer.erase(
                        playback.videoSequenceSyncBuffer.begin(),
                        playback.videoSequenceSyncBuffer.begin() + static_cast<std::ptrdiff_t>(keepFrom));
                }

                const size_t sequenceHeader = findMpegSequenceHeader(
                    playback.videoSequenceSyncBuffer.data(),
                    playback.videoSequenceSyncBuffer.size());
                if (sequenceHeader == kStartCodeNotFound)
                {
                    return;
                }

                if (sequenceHeader != 0u)
                {
                    playback.videoSequenceSyncBuffer.erase(
                        playback.videoSequenceSyncBuffer.begin(),
                        playback.videoSequenceSyncBuffer.begin() + static_cast<std::ptrdiff_t>(sequenceHeader));
                }

                data = playback.videoSequenceSyncBuffer.data();
                size = playback.videoSequenceSyncBuffer.size();
                if (playback.pictureIntervalQ32 == 0u)
                {
                    playback.pictureIntervalQ32 = kDefaultPictureIntervalQ32;
                }
                playback.nextPictureTickQ32 = std::numeric_limits<uint64_t>::max();
                playback.presentationEndTickQ32 = std::numeric_limits<uint64_t>::max();
                playback.firstPresentedPts90k = -1;
                playback.ptsPresentationBaseTickQ32 = 0u;
                playback.waitingForVideoSequenceHeader = false;
                playback.decoderFailed = false;
                playback.decoder.reset();
                playback.decodedFrames.clear();
            }

            if (containsMpegSequenceEnd(data, size))
            {
                playback.sawSequenceEnd = true;
                playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            }

            if (!playback.decoder)
            {
                playback.decoder = std::make_unique<MpegFfmpegDecoder>();
            }

            if (!playback.decoder->feed(data, size, playback.decodedFrames, pts90k, dts90k))
            {
                playback.decoder.reset();
                playback.waitingForVideoSequenceHeader = true;
                playback.videoSequenceSyncBuffer.clear();
                playback.decoderFailed = false;
                return;
            }

            playback.videoSequenceSyncBuffer.clear();
            flushDecoderIfEnded(playback);
        }

        void erasePssPrefix(MpegPlaybackState &playback, size_t count)
        {
            std::vector<uint8_t> &buffer = playback.pssBuffer;
            std::vector<uint32_t> &guestAddrs = playback.pssGuestAddrs;
            const size_t clamped = std::min(count, buffer.size());
            if (clamped == 0u)
            {
                return;
            }

            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(clamped));
            if (guestAddrs.size() >= clamped)
            {
                guestAddrs.erase(guestAddrs.begin(), guestAddrs.begin() + static_cast<std::ptrdiff_t>(clamped));
            }
            else
            {
                guestAddrs.clear();
            }
        }

        std::vector<MpegRegisteredCallback> matchingStreamCallbacks(uint32_t mpegAddr, uint32_t streamType)
        {
            std::vector<MpegRegisteredCallback> out;
            auto it = g_mpeg_stub_state.callbacksByMpeg.find(mpegAddr);
            if (it == g_mpeg_stub_state.callbacksByMpeg.end())
            {
                return out;
            }

            for (const MpegRegisteredCallback &callback : it->second)
            {
                if (callback.stream && callback.type == streamType)
                {
                    out.push_back(callback);
                }
            }
            return out;
        }

        constexpr uint32_t kMpegCbNodata = 1u;
        constexpr uint32_t kMpegCbStopDMA = 2u;
        constexpr uint32_t kMpegCbRestartDMA = 3u;
        constexpr uint32_t kMpegCbBackground = 4u;

        // sceMpegCbBackground callbacks: libmpeg calls these while it waits on the IPU inside
        // sceMpegGetPicture. Movie players do their audio work there (e.g. Sony's sample audio
        // code, AudioExec, streams the PSS audio track to the IOP from it).
        std::vector<MpegRegisteredCallback> matchingBackgroundCallbacks(uint32_t mpegAddr)
        {
            std::vector<MpegRegisteredCallback> out;
            auto it = g_mpeg_stub_state.callbacksByMpeg.find(mpegAddr);
            if (it == g_mpeg_stub_state.callbacksByMpeg.end())
                return out;
            for (const MpegRegisteredCallback &callback : it->second)
                if (!callback.stream && callback.type == kMpegCbBackground)
                    out.push_back(callback);
            return out;
        }

        // Optional Nodata (games start their IPU input DMA there), then StopDMA, then
        // RestartDMA, matching the order libmpeg invokes them.
        std::vector<MpegRegisteredCallback> matchingDmaCallbacks(uint32_t mpegAddr, bool includeNodata)
        {
            std::vector<MpegRegisteredCallback> out;
            auto it = g_mpeg_stub_state.callbacksByMpeg.find(mpegAddr);
            if (it == g_mpeg_stub_state.callbacksByMpeg.end())
            {
                return out;
            }
            for (uint32_t type : {kMpegCbNodata, kMpegCbStopDMA, kMpegCbRestartDMA})
            {
                if (type == kMpegCbNodata && !includeNodata)
                {
                    continue;
                }
                for (const MpegRegisteredCallback &callback : it->second)
                {
                    if (!callback.stream && callback.type == type)
                    {
                        out.push_back(callback);
                    }
                }
            }
            return out;
        }

        void queueStreamCallbackEvent(uint32_t mpegAddr,
                                      uint32_t streamType,
                                      uint32_t dataAddr,
                                      uint32_t len,
                                      std::vector<MpegStreamCallbackEvent> &callbackEvents,
                                      int64_t pts90k = -1,
                                      int64_t dts90k = -1,
                                      const uint8_t *hostPayload = nullptr)
        {
            MpegStreamCallbackEvent event{};
            event.mpegAddr = mpegAddr;
            event.streamType = streamType;
            event.dataAddr = dataAddr;
            event.len = len;
            event.pts = pts90k >= 0 ? static_cast<uint64_t>(pts90k) : 0xFFFFFFFFFFFFFFFFull;
            event.dts = dts90k >= 0 ? static_cast<uint64_t>(dts90k) : 0xFFFFFFFFFFFFFFFFull;
            event.callbacks = matchingStreamCallbacks(mpegAddr, streamType);
            if (!event.callbacks.empty())
            {
                if (hostPayload)
                    event.payload.assign(hostPayload, hostPayload + len);
                callbackEvents.push_back(std::move(event));
            }
        }

        void queueDataStreamCallbackEvent(uint32_t mpegAddr,
                                          uint32_t channel,
                                          uint32_t dataAddr,
                                          uint32_t len,
                                          std::vector<MpegStreamCallbackEvent> &callbackEvents,
                                          int64_t pts90k,
                                          int64_t dts90k,
                                          const uint8_t *hostPayload)
        {
            MpegStreamCallbackEvent event{};
            event.mpegAddr = mpegAddr;
            event.streamType = kMpegStrDATA;
            event.dataAddr = dataAddr;
            event.len = len;
            event.pts = pts90k >= 0 ? static_cast<uint64_t>(pts90k) : 0xFFFFFFFFFFFFFFFFull;
            event.dts = dts90k >= 0 ? static_cast<uint64_t>(dts90k) : 0xFFFFFFFFFFFFFFFFull;
            for (const MpegRegisteredCallback &callback : matchingStreamCallbacks(mpegAddr, kMpegStrDATA))
            {
                if (callback.streamId == channel)
                    event.callbacks.push_back(callback);
            }
            if (!event.callbacks.empty())
            {
                if (hostPayload)
                    event.payload.assign(hostPayload, hostPayload + len);
                callbackEvents.push_back(std::move(event));
            }
        }

        void processPssBuffer(uint32_t mpegAddr,
                              MpegPlaybackState &playback,
                              std::vector<MpegStreamCallbackEvent> &callbackEvents,
                              bool finalChunk = false)
        {
            std::vector<uint8_t> &buffer = playback.pssBuffer;

            while (true)
            {
                if (playback.streamEnded)
                {
                    erasePssPrefix(playback, buffer.size());
                    return;
                }

                const size_t start = findStartCode(buffer, 0u);
                if (start == kStartCodeNotFound)
                {
                    if (finalChunk)
                    {
                        erasePssPrefix(playback, buffer.size());
                        return;
                    }
                    if (buffer.size() > 3u)
                    {
                        erasePssPrefix(playback, buffer.size() - 3u);
                    }
                    return;
                }

                if (start > 0u)
                {
                    erasePssPrefix(playback, start);
                }

                if (buffer.size() < 4u)
                {
                    return;
                }

                const uint8_t streamId = buffer[3u];

                if (streamId == kMpegProgramEnd)
                {
                    playback.streamEnded = true;
                    playback.sawProgramEnd = true;
                    playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
                    flushDecoderIfEnded(playback);
                    erasePssPrefix(playback, buffer.size());
                    return;
                }

                if (streamId == kMpegPackHeader)
                {
                    if (buffer.size() < 12u)
                    {
                        if (finalChunk)
                        {
                            erasePssPrefix(playback, buffer.size());
                        }
                        return;
                    }

                    size_t packSize = 12u;
                    if ((buffer[4u] & 0xC0u) == 0x40u)
                    {
                        if (buffer.size() < 14u)
                        {
                            if (finalChunk)
                            {
                                erasePssPrefix(playback, buffer.size());
                            }
                            return;
                        }
                        packSize = 14u + static_cast<size_t>(buffer[13u] & 0x07u);
                    }
                    if (buffer.size() < packSize)
                    {
                        if (finalChunk)
                        {
                            erasePssPrefix(playback, buffer.size());
                        }
                        return;
                    }
                    erasePssPrefix(playback, packSize);
                    continue;
                }

                if (buffer.size() < 6u)
                {
                    if (finalChunk)
                    {
                        erasePssPrefix(playback, buffer.size());
                    }
                    return;
                }

                const uint16_t packetLength = readBe16(buffer.data() + 4u);
                if (isLengthPrefixedHeader(streamId))
                {
                    const size_t packetEnd = 6u + static_cast<size_t>(packetLength);
                    if (buffer.size() < packetEnd)
                    {
                        if (finalChunk)
                        {
                            erasePssPrefix(playback, buffer.size());
                        }
                        return;
                    }
                    erasePssPrefix(playback, packetEnd);
                    continue;
                }

                size_t packetEnd = 0u;
                if (packetLength != 0u)
                {
                    packetEnd = 6u + static_cast<size_t>(packetLength);
                    if (buffer.size() < packetEnd)
                    {
                        if (!finalChunk)
                        {
                            return;
                        }
                        packetEnd = buffer.size();
                    }
                }
                else
                {
                    const size_t next = findStartCode(buffer, 6u);
                    if (next == kStartCodeNotFound)
                    {
                        if (!finalChunk)
                        {
                            return;
                        }
                        packetEnd = buffer.size();
                    }
                    else
                    {
                        packetEnd = next;
                    }
                }

                if (isVideoStreamId(streamId))
                {
                    const MpegPesHeader pes = parsePesHeader(buffer.data(), packetEnd);
                    const size_t payloadStart = pes.payloadOffset;
                    if (payloadStart < packetEnd)
                    {
                        if (payloadStart < playback.pssGuestAddrs.size())
                        {
                            queueStreamCallbackEvent(
                                mpegAddr,
                                kMpegStrM2V,
                                playback.pssGuestAddrs[payloadStart],
                                static_cast<uint32_t>(packetEnd - payloadStart),
                                callbackEvents,
                                pes.pts90k,
                                pes.dts90k,
                                buffer.data() + payloadStart);
                        }
                        feedElementaryStream(
                            playback,
                            buffer.data() + payloadStart,
                            packetEnd - payloadStart,
                            pes.pts90k,
                            pes.dts90k);
                    }
                }
                else if (isAudioStreamId(streamId))
                {
                    const MpegPesHeader pes = parsePesHeader(buffer.data(), packetEnd);
                    const size_t payloadStart = pes.payloadOffset;
                    if (payloadStart < packetEnd && payloadStart < playback.pssGuestAddrs.size())
                    {
                        queueStreamCallbackEvent(
                            mpegAddr,
                            kMpegStrPCM,
                            playback.pssGuestAddrs[payloadStart],
                            static_cast<uint32_t>(packetEnd - payloadStart),
                            callbackEvents,
                            pes.pts90k,
                            pes.dts90k,
                            buffer.data() + payloadStart);
                        queueStreamCallbackEvent(
                            mpegAddr,
                            kMpegStrADPCM,
                            playback.pssGuestAddrs[payloadStart],
                            static_cast<uint32_t>(packetEnd - payloadStart),
                            callbackEvents,
                            pes.pts90k,
                            pes.dts90k,
                            buffer.data() + payloadStart);
                        if (streamId == kMpegPrivateStream1 && payloadStart + 4u <= packetEnd && buffer[payloadStart] == 0xFFu)
                        {
                            queueDataStreamCallbackEvent(
                                mpegAddr,
                                buffer[payloadStart + 3u],
                                playback.pssGuestAddrs[payloadStart],
                                static_cast<uint32_t>(packetEnd - payloadStart),
                                callbackEvents,
                                pes.pts90k,
                                pes.dts90k,
                                buffer.data() + payloadStart);
                        }
                    }
                }

                erasePssPrefix(playback, packetEnd);
            }
        }

        void finishPlaybackStream(uint32_t mpegAddr, MpegPlaybackState &playback)
        {
            std::vector<MpegStreamCallbackEvent> ignoredCallbacks;
            processPssBuffer(mpegAddr, playback, ignoredCallbacks, true);
            playback.streamEnded = true;
            playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            flushDecoderIfEnded(playback);
        }

        void finalizeCdStreamEofUnlocked(std::vector<uint32_t> &completedMpegIds, bool &changed)
        {
            g_mpeg_stub_state.cdStreamEofPending = false;
            g_mpeg_stub_state.currentCdStreamEofSeen = true;
            for (auto &[mpegAddr, playback] : g_mpeg_stub_state.playbackByMpeg)
            {
                completedMpegIds.push_back(mpegAddr);
                if (!playback.sawInput || playback.streamEnded)
                {
                    continue;
                }

                finishPlaybackStream(mpegAddr, playback);
                changed = true;
            }
        }

        bool mpegDemuxBackpressured(const MpegPlaybackState &playback)
        {
            // Let EOF finalization drain any tail that is already in the guest
            // ring, otherwise bound decode lead to a handful of pictures.
            //
            // Important: do not park sceMpegDemuxPss/Ring here. Code Veronica
            // explicitly wakes its video thread before every demux call and that
            // thread sleeps again after presenting one picture. A single host
            // decoder feed can enqueue more than kMaxDecodedPicturesAhead frames;
            // parking the producer then leaves the consumer asleep after draining
            // just one frame, with nobody left to issue the next WakeupThread.
            // Returning 0 bytes consumed instead leaves the guest ring intact and
            // lets the game's producer loop wake the consumer again. Backpressure
            // still propagates naturally to sceCdStRead because the ring does not
            // advance while this is true.
            return !g_mpeg_stub_state.currentCdStreamEofSeen &&
                   playback.decodedFrames.size() >= kMaxDecodedPicturesAhead;
        }

        void recordCdStreamBytesDemuxedUnlocked(
            size_t consumed,
            std::vector<uint32_t> &completedMpegIds,
            bool &changed)
        {
            g_mpeg_stub_state.cdStreamBytesDemuxed += consumed;
            if (g_mpeg_stub_state.cdStreamEofPending &&
                g_mpeg_stub_state.cdStreamBytesDemuxed >= g_mpeg_stub_state.cdStreamBytesProduced)
            {
                finalizeCdStreamEofUnlocked(completedMpegIds, changed);
            }
        }

        void appendPssBytes(uint32_t mpegAddr,
                            MpegPlaybackState &playback,
                            const uint8_t *data,
                            size_t size,
                            uint32_t guestAddr,
                            std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            if (!data || size == 0)
            {
                return;
            }

            if (playback.sawInput && playback.cdStreamGeneration != g_mpeg_stub_state.cdStreamGeneration)
            {
                playback = makeFreshPlaybackState();
            }

            if (playback.streamEnded)
            {
                if (playback.cdStreamGeneration == g_mpeg_stub_state.cdStreamGeneration)
                {
                    playback.sawInput = true;
                    return;
                }

                playback = makeFreshPlaybackState();
            }

            if (!playback.sawInput)
            {
                playback.cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
            }
            playback.sawInput = true;

            playback.pssBuffer.insert(playback.pssBuffer.end(), data, data + size);
            playback.pssGuestAddrs.reserve(playback.pssGuestAddrs.size() + size);
            for (size_t i = 0; i < size; ++i)
            {
                playback.pssGuestAddrs.push_back(guestAddr + static_cast<uint32_t>(i));
            }
            processPssBuffer(mpegAddr, playback, callbackEvents);
        }

        size_t appendGuestBytes(uint32_t mpegAddr,
                                MpegPlaybackState &playback,
                                const uint8_t *rdram,
                                uint32_t addr,
                                size_t size,
                                std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            size_t copied = 0u;
            while (copied < size)
            {
                const uint32_t curAddr = addr + static_cast<uint32_t>(copied);
                const uint32_t offset = curAddr & PS2_RAM_MASK;
                size_t chunk = std::min<size_t>(size - copied, PS2_RAM_SIZE - offset);
                if (chunk == 0u)
                {
                    break;
                }

                const uint8_t *src = getConstMemPtr(rdram, curAddr);
                if (!src)
                {
                    break;
                }

                appendPssBytes(
                    mpegAddr,
                    playback,
                    src,
                    chunk,
                    curAddr,
                    callbackEvents);
                copied += chunk;
            }
            return copied;
        }

        size_t appendGuestRingBytes(uint32_t mpegAddr,
                                    MpegPlaybackState &playback,
                                    const uint8_t *rdram,
                                    uint32_t dataAddr,
                                    uint32_t byteCount,
                                    uint32_t ringBaseAddr,
                                    uint32_t ringSize,
                                    std::vector<MpegStreamCallbackEvent> &callbackEvents)
        {
            if (byteCount == 0u)
            {
                return 0u;
            }

            const uint32_t base = ringBaseAddr & PS2_RAM_MASK;
            const uint32_t data = dataAddr & PS2_RAM_MASK;
            if (ringBaseAddr != 0u && ringSize != 0u && ringSize <= PS2_RAM_SIZE)
            {
                const uint32_t ringOffset = (data - base) & PS2_RAM_MASK;
                if (ringOffset < ringSize)
                {
                    const uint32_t first = std::min<uint32_t>(byteCount, ringSize - ringOffset);
                    size_t copied = appendGuestBytes(
                        mpegAddr,
                        playback,
                        rdram,
                        dataAddr,
                        first,
                        callbackEvents);
                    if (copied < first)
                    {
                        return copied;
                    }

                    const uint32_t remaining = byteCount - first;
                    if (remaining != 0u)
                    {
                        copied += appendGuestBytes(
                            mpegAddr,
                            playback,
                            rdram,
                            ringBaseAddr,
                            remaining,
                            callbackEvents);
                    }
                    return copied;
                }
            }

            return appendGuestBytes(mpegAddr, playback, rdram, dataAddr, byteCount, callbackEvents);
        }

        bool writeMpegCallbackData(uint8_t *rdram, uint32_t addr, const MpegStreamCallbackEvent &event)
        {
            if (!rdram || addr == 0u)
            {
                return false;
            }

            uint8_t *data = getMemPtr(rdram, addr);
            if (!data)
            {
                return false;
            }

            std::memset(data, 0, kMpegCallbackDataSize);
            *reinterpret_cast<uint32_t *>(data + 0x00u) = event.streamType;
            *reinterpret_cast<uint32_t *>(data + 0x08u) = event.dataAddr;
            *reinterpret_cast<uint32_t *>(data + 0x0Cu) = event.len;
            *reinterpret_cast<uint64_t *>(data + 0x10u) = event.pts;
            *reinterpret_cast<uint64_t *>(data + 0x18u) = event.dts;
            return true;
        }

        void dispatchGuestStreamCallback(uint8_t *rdram,
                                         R5900Context *callerCtx,
                                         PS2Runtime *runtime,
                                         const MpegStreamCallbackEvent &event,
                                         const MpegRegisteredCallback &callback)
        {
            if (!rdram || !callerCtx || !runtime || callback.func == 0u || !runtime->hasFunction(callback.func))
            {
                return;
            }

            // Pass the game's own ring bytes when they still hold this packet (the common case); only packets that
            // were reassembled across demux calls, wrap the ring, or were already overwritten get a private copy.
            bool ringStillValid = event.payload.empty();
            if (!ringStillValid)
            {
                const uint32_t phys = event.dataAddr & PS2_RAM_MASK;
                const uint8_t *src = getConstMemPtr(rdram, event.dataAddr);
                ringStillValid = src && phys + event.payload.size() <= PS2_RAM_SIZE &&
                                 std::memcmp(src, event.payload.data(), event.payload.size()) == 0;
            }
            uint32_t payloadSize = ringStillValid ? 0u : static_cast<uint32_t>((event.payload.size() + 15u) & ~static_cast<size_t>(15u));
            uint32_t cbDataAddr = runtime->guestMalloc(kMpegCallbackDataSize + payloadSize, 16u);
            if (cbDataAddr == 0u && payloadSize != 0u)
            {
                payloadSize = 0u; // heap exhausted: fall back to the ring pointer rather than dropping the packet
                cbDataAddr = runtime->guestMalloc(kMpegCallbackDataSize, 16u);
            }
            if (cbDataAddr == 0u)
            {
                return;
            }
            MpegStreamCallbackEvent guestEvent = event;
            if (payloadSize != 0u)
            {
                const uint32_t payloadAddr = cbDataAddr + kMpegCallbackDataSize;
                uint8_t *dst = getMemPtr(rdram, payloadAddr);
                if (dst)
                {
                    std::memcpy(dst, event.payload.data(), event.payload.size());
                    guestEvent.dataAddr = payloadAddr;
                }
            }
            if (!writeMpegCallbackData(rdram, cbDataAddr, guestEvent))
            {
                runtime->guestFree(cbDataAddr);
                return;
            }

            R5900Context callbackCtx = *callerCtx;
            SET_GPR_U32(&callbackCtx, 4, event.mpegAddr);
            SET_GPR_U32(&callbackCtx, 5, cbDataAddr);
            SET_GPR_U32(&callbackCtx, 6, callback.data);
            SET_GPR_U32(&callbackCtx, 7, 0u);
            SET_GPR_U32(&callbackCtx, 29, 0u);
            SET_GPR_U32(&callbackCtx, 31, 0u);
            callbackCtx.pc = callback.func;

            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::RpcCallback;
            invocation.context = callbackCtx;
            invocation.onComplete = [runtime, cbDataAddr](const R5900Context &, R5900Context &)
            {
                runtime->guestFree(cbDataAddr);
            };
            runtime->eeScheduler().queueInvocation(std::move(invocation));
        }

        void dispatchStreamCallbacks(uint8_t *rdram,
                                     R5900Context *ctx,
                                     PS2Runtime *runtime,
                                     const std::vector<MpegStreamCallbackEvent> &events)
        {
            if (events.empty())
            {
                return;
            }

            for (const MpegStreamCallbackEvent &event : events)
            {
                for (const MpegRegisteredCallback &callback : event.callbacks)
                {
                    dispatchGuestStreamCallback(rdram, ctx, runtime, event, callback);
                }
            }
        }

        void dispatchStreamCallbacksUnlocked(uint8_t *rdram,
                                             R5900Context *ctx,
                                             PS2Runtime *runtime,
                                             const std::vector<MpegStreamCallbackEvent> &events)
        {
            if (events.empty())
            {
                return;
            }

            dispatchStreamCallbacks(rdram, ctx, runtime, events);
        }

        // libmpeg runs stream callbacks synchronously inside sceMpegDemuxPss[Ring]. A callback returning 0 means "no
        // room": the packet is not consumed and is offered again on the next demux call (ZOE's DATA/M2V callbacks do
        // this whenever their own rings are full; dropping those packets desynced its cutscene and audio streams).
        // Refused packets wait here, in order, and are retried before any new input is accepted.
        struct PendingStreamEvents
        {
            uint64_t generation = 0u;
            std::deque<MpegStreamCallbackEvent> events;
        };
        std::unordered_map<uint32_t, PendingStreamEvents> g_pendingStreamEvents; // guarded by g_mpeg_stub_mutex
        // Reusable guest block for callback data + payload copies (only the demuxing thread touches it).
        uint32_t g_streamStagingAddr = 0u;
        uint32_t g_streamStagingSize = 0u;

        // Safety net for a consumer that never drains (e.g. a stream whose player stalled elsewhere): if the
        // callback's own state (its first 16 bytes, a ring header for ZOE) has not changed for 3 s while it keeps
        // refusing, drop its packets like before instead of freezing the whole movie. Activity re-arms waiting.
        struct StreamConsumerWatch
        {
            uint8_t state[16] = {};
            std::chrono::steady_clock::time_point since{};
            uint64_t sinceTick = 0u;
            uint64_t lastTick = 0u; // tick of the previous check
            bool valid = false;
        };
        std::unordered_map<uint32_t, StreamConsumerWatch> g_streamConsumerWatch;

        bool streamConsumerStuck(uint8_t *rdram, uint32_t callbackData)
        {
            const uint8_t *cur = callbackData ? getConstMemPtr(rdram, callbackData) : nullptr;
            if (!cur)
                return true;
            // Idle is measured in guest vsync ticks: a host stall (one frame blocked for seconds on the render
            // worker) is a single tick, so it no longer trips the drop while the game itself was not running.
            // PS2X_MPEG_STUCK_TICKS=<n> (default 180 = 3 s of game time); 0 = the old 3 s of host time.
            static const uint64_t s_stuckTicks = []
            {
                const char *v = std::getenv("PS2X_MPEG_STUCK_TICKS");
                return v ? std::strtoull(v, nullptr, 10) : 180ull;
            }();
            StreamConsumerWatch &w = g_streamConsumerWatch[callbackData];
            const auto now = std::chrono::steady_clock::now();
            const uint64_t tick = ps2CurrentVsyncTick();
            // A tick that went backwards (state load) re-arms the wait instead of wrapping into an instant drop.
            // So does a gap since the last refusal check (a state load or an old stream's entry for the same
            // consumer address): the wait must cover continuous refusals only.
            const bool gap = tick < w.lastTick || tick - w.lastTick > 30u;
            w.lastTick = tick;
            if (!w.valid || gap || tick < w.sinceTick || std::memcmp(w.state, cur, sizeof(w.state)) != 0)
            {
                std::memcpy(w.state, cur, sizeof(w.state));
                w.since = now;
                w.sinceTick = tick;
                w.valid = true;
                return false;
            }
            if (s_stuckTicks != 0u ? tick - w.sinceTick < s_stuckTicks : now - w.since < std::chrono::milliseconds(3000))
                return false;
            static uint32_t s_drops = 0;
            if (s_drops < 3u || (s_drops & (s_drops - 1u)) == 0u)
                std::cerr << "[mpeg] stream consumer 0x" << std::hex << callbackData << std::dec
                          << " idle 3 s while refusing: dropping packet #" << s_drops << std::endl;
            ++s_drops;
            return true;
        }

        // Runs every remaining callback of one packet on the caller's thread. Returns false when a callback refused
        // it; callbacks that accepted are removed from event.callbacks so a retry only re-offers it to the others.
        bool deliverStreamEventSync(uint8_t *rdram, const R5900Context *callerCtx, PS2Runtime *runtime,
                                    MpegStreamCallbackEvent &event)
        {
            const size_t len = event.payload.size();
            const uint32_t need = kMpegCallbackDataSize + static_cast<uint32_t>((len + 15u) & ~static_cast<size_t>(15u));
            if (g_streamStagingSize < need)
            {
                if (g_streamStagingAddr != 0u)
                    runtime->guestFree(g_streamStagingAddr);
                const uint32_t size = std::max<uint32_t>(need, 0x1000u);
                g_streamStagingAddr = runtime->guestMalloc(size, 16u);
                g_streamStagingSize = g_streamStagingAddr != 0u ? size : 0u;
            }
            if (g_streamStagingAddr == 0u)
                return true; // no guest memory at all: drop the packet rather than stall the stream forever

            MpegStreamCallbackEvent guestEvent = event;
            // Hub/Viola stall diagnostic: PS2X_MPEG_CHLOG=<channel> logs every delivery attempt of that DATA channel.
            static const int s_chLog = []
            {
                const char *v = std::getenv("PS2X_MPEG_CHLOG");
                return v ? std::atoi(v) : -1;
            }();
            bool chLogStaging = false;
            const bool chLogThis = s_chLog >= 0 && event.streamType == kMpegStrDATA && len > 3u &&
                                   event.payload[3] == static_cast<uint8_t>(s_chLog);
            if (len != 0u)
            {
                // The game's ring usually still holds the packet; if not (reassembled across calls, ring wrap, or
                // already refilled while it waited here), hand the callback a copy.
                const uint32_t phys = event.dataAddr & PS2_RAM_MASK;
                const uint8_t *src = getConstMemPtr(rdram, event.dataAddr);
                const bool ringValid = src && phys + len <= PS2_RAM_SIZE && std::memcmp(src, event.payload.data(), len) == 0;
                if (!ringValid)
                {
                    uint8_t *dst = getMemPtr(rdram, g_streamStagingAddr + kMpegCallbackDataSize);
                    if (!dst)
                        return true;
                    std::memcpy(dst, event.payload.data(), len);
                    guestEvent.dataAddr = g_streamStagingAddr + kMpegCallbackDataSize;
                    chLogStaging = true;
                }
            }
            auto chLog = [&](const char *result)
            {
                if (!chLogThis)
                    return;
                static uint64_t s_attempts = 0;
                uint32_t sum = 0x811C9DC5u; // FNV-1a over the whole payload (incl. the 4-byte DATA header)
                for (uint8_t b : event.payload)
                    sum = (sum ^ b) * 0x01000193u;
                char line[192];
                std::snprintf(line, sizeof(line),
                              "[mpeg:chlog] tick=%llu n=%llu len=%zu sum=%08x addr=0x%x given=0x%x staging=%d %s\n",
                              static_cast<unsigned long long>(ps2CurrentVsyncTick()),
                              static_cast<unsigned long long>(s_attempts++), static_cast<size_t>(len), sum,
                              event.dataAddr, guestEvent.dataAddr, chLogStaging ? 1 : 0, result);
                std::fputs(line, stderr);
            };

            for (auto it = event.callbacks.begin(); it != event.callbacks.end();)
            {
                if (it->func == 0u || !runtime->hasFunction(it->func))
                {
                    it = event.callbacks.erase(it);
                    continue;
                }
                if (!writeMpegCallbackData(rdram, g_streamStagingAddr, guestEvent))
                    return true;
                R5900Context callbackCtx = *callerCtx;
                R5900Context *cb = &callbackCtx;
                SET_GPR_U32(cb, 4, event.mpegAddr);
                SET_GPR_U32(cb, 5, g_streamStagingAddr);
                SET_GPR_U32(cb, 6, it->data);
                SET_GPR_U32(cb, 7, 0u);
                // Run on the caller's stack below its frame, as libmpeg would (its own frame + the callback frame).
                SET_GPR_U32(cb, 29, (GPR_U32(callerCtx, 29) - 0x200u) & ~0xFu);
                SET_GPR_U32(cb, 31, 0u);
                callbackCtx.pc = it->func;
                // Generated code returns to its host caller at every scheduler safe point (dispatchGuestBranch /
                // backward-edge checkpoints) with ctx->pc = where to continue, expecting the dispatcher loop to
                // resume it. Nothing resumes this nested call, so a single call could cut the callback short and
                // its stale v0 was taken as the answer: ZOE's DATA callback then reserved ring space without
                // committing it, or "accepted" a packet it never copied (Hub/Viola cutscene stall). Resume until
                // it returns to ra = 0. PS2X_MPEG_CB_RESUME=0 restores the single call.
                static const bool s_resume = []
                {
                    const char *v = std::getenv("PS2X_MPEG_CB_RESUME");
                    return !(v && v[0] == '0');
                }();
                runtime->lookupFunction(it->func)(rdram, cb, runtime);
                uint32_t resumes = 0u;
                while (s_resume && callbackCtx.pc != 0u)
                {
                    if (!runtime->hasFunction(callbackCtx.pc))
                    {
                        // No code to continue with: report once and count the packet as consumed (dropped), since
                        // treating it as refused would re-offer it to the same broken path forever.
                        static std::atomic<bool> s_reported{false};
                        if (!s_reported.exchange(true))
                            std::cerr << "[mpeg] stream callback 0x" << std::hex << it->func << " stopped at unknown pc 0x"
                                      << callbackCtx.pc << std::dec << ": packet dropped" << std::endl;
                        SET_GPR_U32(cb, 2, 1u);
                        break;
                    }
                    // Other EE threads never run inside this nested call, so a callback that polls state only they
                    // change would spin here forever (ZOE's callbacks need < 20 resumes). Give up and drop.
                    if (resumes >= 100000u)
                    {
                        static std::atomic<bool> s_capReported{false};
                        if (!s_capReported.exchange(true))
                            std::cerr << "[mpeg] stream callback 0x" << std::hex << it->func << " still running at pc 0x"
                                      << callbackCtx.pc << std::dec << " after " << resumes << " resumes: packet dropped"
                                      << std::endl;
                        SET_GPR_U32(cb, 2, 1u);
                        break;
                    }
                    runtime->lookupFunction(callbackCtx.pc)(rdram, cb, runtime);
                    ++resumes;
                }
                if (resumes != 0u)
                {
                    static std::atomic<uint32_t> s_resumed{0};
                    const uint32_t n = s_resumed.fetch_add(1u);
                    if (n < 5u || (n & (n - 1u)) == 0u)
                        std::cerr << "[mpeg] stream callback resumed after a safe point #" << n << " func=0x" << std::hex
                                  << it->func << std::dec << " resumes=" << resumes << std::endl;
                }
                // Video is the exception: this runtime decodes M2V straight from the demuxed stream, so the game's
                // own bitstream ring (drained by its nodata callback -> sceMpegAddBs) is not what feeds the decoder.
                // Waiting on it would stall the demux (and starve audio), so a refused video packet is dropped.
                if (GPR_U32(cb, 2) == 0u && event.streamType != kMpegStrM2V && !streamConsumerStuck(rdram, it->data))
                {
                    chLog("refused");
                    return false;
                }
                chLog(GPR_U32(cb, 2) == 0u ? "dropped" : "accepted");
                it = event.callbacks.erase(it);
            }
            return true;
        }

        // Offers earlier refused packets, then `fresh`, in stream order. Returns true when nothing is left pending.
        bool deliverStreamEventsSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t mpegAddr,
                                     std::vector<MpegStreamCallbackEvent> &fresh)
        {
            std::deque<MpegStreamCallbackEvent> queue;
            uint64_t generation = 0u;
            {
                std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
                generation = getPlaybackState(mpegAddr).cdStreamGeneration;
                auto it = g_pendingStreamEvents.find(mpegAddr);
                if (it != g_pendingStreamEvents.end())
                {
                    if (it->second.generation == generation)
                        queue = std::move(it->second.events);
                    g_pendingStreamEvents.erase(it);
                }
            }
            for (MpegStreamCallbackEvent &event : fresh)
                queue.push_back(std::move(event));
            fresh.clear();

            while (!queue.empty() && deliverStreamEventSync(rdram, ctx, runtime, queue.front()))
                queue.pop_front();
            if (queue.empty())
                return true;

            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            PendingStreamEvents &pending = g_pendingStreamEvents[mpegAddr];
            pending.generation = generation;
            pending.events = std::move(queue);
            return false;
        }

        // sceMpegDemuxPssRing the way libmpeg does it: walk whole packets in the game's ring, run each packet's stream
        // callbacks synchronously with a pointer INTO the ring (ZOE's DATA callback turns that pointer back into an
        // offset inside its PSS ring), and stop at the first packet a callback refuses. Only whole, accepted packets
        // are consumed, so a refused or incomplete packet stays in the ring and is offered again on the next call.
        // The consumed prefix then goes through the normal host path (video decode, EOF), minus its callbacks.
        size_t demuxRingPacketsSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t mpegAddr,
                                    uint32_t dataAddr, uint32_t avail, uint32_t ringBase, uint32_t ringSize,
                                    std::vector<uint32_t> &completedMpegIds, bool &eofChanged)
        {
            // Host view of the available bytes (the ring may wrap: at most two contiguous pieces, copied with memcpy -
            // a byte-at-a-time copy of the whole available ring on every call cost ~5% of the frame in ZOE's cutscene).
            std::vector<uint8_t> bytes;
            bytes.reserve(avail);
            bytes.clear();
            const bool isRing = ringBase != 0u && ringSize != 0u && ringSize <= PS2_RAM_SIZE;
            auto guestAddr = [&](size_t i) -> uint32_t
            {
                if (!isRing)
                    return dataAddr + static_cast<uint32_t>(i);
                return ringBase + static_cast<uint32_t>(((dataAddr - ringBase) + i) % ringSize);
            };
            auto copyPiece = [&](uint32_t addr, uint32_t len) -> bool
            {
                if (len == 0u)
                    return true;
                const uint8_t *first = getConstMemPtr(rdram, addr);
                const uint8_t *last = getConstMemPtr(rdram, addr + len - 1u);
                if (!first || !last || last - first != static_cast<std::ptrdiff_t>(len - 1u))
                    return false;
                bytes.insert(bytes.end(), first, first + len);
                return true;
            };
            bool fast = true;
            if (isRing)
            {
                const uint32_t off0 = (dataAddr - ringBase) % ringSize;
                const uint32_t len1 = std::min<uint32_t>(avail, ringSize - off0);
                fast = copyPiece(ringBase + off0, len1) && copyPiece(ringBase, avail - len1);
            }
            else
                fast = copyPiece(dataAddr, avail);
            if (!fast)
            {
                bytes.clear();
                for (uint32_t i = 0; i < avail; ++i)
                {
                    const uint8_t *src = getConstMemPtr(rdram, guestAddr(i));
                    if (!src)
                        break;
                    bytes.push_back(*src);
                }
            }

            std::vector<MpegRegisteredCallback> m2v, pcm, adpcm, data;
            {
                std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
                m2v = matchingStreamCallbacks(mpegAddr, kMpegStrM2V);
                pcm = matchingStreamCallbacks(mpegAddr, kMpegStrPCM);
                adpcm = matchingStreamCallbacks(mpegAddr, kMpegStrADPCM);
                data = matchingStreamCallbacks(mpegAddr, kMpegStrDATA);
            }

            // Returns false if a callback refused the packet (and its consumer is still active).
            auto offer = [&](uint32_t type, const std::vector<MpegRegisteredCallback> &cbs, uint32_t channelFilter,
                             size_t payloadStart, size_t payloadEnd, const MpegPesHeader &pes) -> bool
            {
                if (cbs.empty())
                    return true;
                MpegStreamCallbackEvent event{};
                event.mpegAddr = mpegAddr;
                event.streamType = type;
                event.dataAddr = guestAddr(payloadStart);
                event.len = static_cast<uint32_t>(payloadEnd - payloadStart);
                event.pts = pes.pts90k >= 0 ? static_cast<uint64_t>(pes.pts90k) : 0xFFFFFFFFFFFFFFFFull;
                event.dts = pes.dts90k >= 0 ? static_cast<uint64_t>(pes.dts90k) : 0xFFFFFFFFFFFFFFFFull;
                for (const MpegRegisteredCallback &cb : cbs)
                    if (channelFilter == 0xFFFFFFFFu || cb.streamId == channelFilter)
                        event.callbacks.push_back(cb);
                if (event.callbacks.empty())
                    return true;
                // Payload snapshot only for the ring-validity check inside deliverStreamEventSync (always valid here).
                event.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payloadStart),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(payloadEnd));
                return deliverStreamEventSync(rdram, ctx, runtime, event);
            };

            size_t pos = 0u;
            size_t consumed = 0u;
            const size_t n = bytes.size();
            while (pos + 4u <= n)
            {
                if (!(bytes[pos] == 0u && bytes[pos + 1u] == 0u && bytes[pos + 2u] == 1u))
                {
                    ++pos; // not a start code: skip (consumed with the next whole packet)
                    continue;
                }
                const uint8_t id = bytes[pos + 3u];
                if (id == kMpegProgramEnd)
                {
                    pos += 4u;
                    consumed = pos;
                    break;
                }
                if (id == kMpegPackHeader)
                {
                    if (pos + 14u > n)
                        break;
                    const size_t packSize = ((bytes[pos + 4u] & 0xC0u) == 0x40u) ? 14u + (bytes[pos + 13u] & 0x07u) : 12u;
                    if (pos + packSize > n)
                        break;
                    pos += packSize;
                    consumed = pos;
                    continue;
                }
                if (pos + 6u > n)
                    break;
                const size_t packetEnd = pos + 6u + readBe16(bytes.data() + pos + 4u);
                if (packetEnd > n)
                    break; // incomplete: leave it in the ring
                if (isVideoStreamId(id) || isAudioStreamId(id))
                {
                    const MpegPesHeader pes = parsePesHeader(bytes.data() + pos, packetEnd - pos);
                    const size_t payloadStart = pos + pes.payloadOffset;
                    if (payloadStart < packetEnd)
                    {
                        bool accepted = true;
                        if (isVideoStreamId(id))
                        {
                            accepted = offer(kMpegStrM2V, m2v, 0xFFFFFFFFu, payloadStart, packetEnd, pes);
                        }
                        else
                        {
                            accepted = offer(kMpegStrPCM, pcm, 0xFFFFFFFFu, payloadStart, packetEnd, pes) &&
                                       offer(kMpegStrADPCM, adpcm, 0xFFFFFFFFu, payloadStart, packetEnd, pes);
                            if (accepted && id == kMpegPrivateStream1 && payloadStart + 4u <= packetEnd &&
                                bytes[payloadStart] == 0xFFu)
                                accepted = offer(kMpegStrDATA, data, bytes[payloadStart + 3u], payloadStart, packetEnd, pes);
                        }
                        if (!accepted)
                            break; // refused: this packet and everything after it stay in the ring
                    }
                }
                pos = packetEnd;
                consumed = pos;
            }

            if (consumed == 0u)
                return 0u;

            // Host side (video elementary stream, stream end) for the consumed prefix only; its callbacks already ran.
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            std::vector<MpegStreamCallbackEvent> alreadyDelivered;
            const size_t appended = appendGuestRingBytes(mpegAddr, playback, rdram, dataAddr, static_cast<uint32_t>(consumed),
                                                         ringBase, ringSize, alreadyDelivered);
            recordCdStreamBytesDemuxedUnlocked(appended, completedMpegIds, eofChanged);
            return appended;
        }

        void writeBlankMpegFrame(uint8_t *rdram, uint32_t destAddr, uint32_t width, uint32_t height)
        {
            if (!rdram || destAddr == 0u)
            {
                return;
            }

            const uint32_t outWidth = align16(width == 0u ? kStubMovieWidth : width);
            const uint32_t outHeight = align16(height == 0u ? kStubMovieHeight : height);
            const uint32_t macroblockColumns = outWidth / 16u;
            for (uint32_t mbx = 0u; mbx < macroblockColumns; ++mbx)
            {
                const size_t stripOffset =
                    static_cast<size_t>(mbx) * static_cast<size_t>(outHeight) * 16u * 4u;
                for (uint32_t y = 0u; y < outHeight; ++y)
                {
                    uint8_t *dst = getMemPtr(
                        rdram,
                        destAddr + static_cast<uint32_t>(stripOffset + static_cast<size_t>(y) * 16u * 4u));
                    if (!dst)
                    {
                        continue;
                    }
                    for (uint32_t x = 0u; x < 16u; ++x)
                    {
                        dst[x * 4u + 0u] = 0u;
                        dst[x * 4u + 1u] = 0u;
                        dst[x * 4u + 2u] = 0u;
                        dst[x * 4u + 3u] = 0x80u;
                    }
                }
            }
        }

        void writeDecodedFrameToGuest(uint8_t *rdram, uint32_t destAddr, const MpegDecodedFrame &frame)
        {
            if (!rdram || destAddr == 0u || frame.rgba.empty() || frame.width <= 0 || frame.height <= 0)
            {
                return;
            }

            const uint32_t width = static_cast<uint32_t>(frame.width);
            const uint32_t height = static_cast<uint32_t>(frame.height);
            {
                static uint32_t s_frameTrace = 0u;
                if (ps2_debug::traceSampled() && (s_frameTrace++ % 60u) == 0u)
                {
                    uint64_t sum = 0;
                    for (size_t i = 0; i < frame.rgba.size(); i += 4)
                        sum += frame.rgba[i] + frame.rgba[i + 1] + frame.rgba[i + 2];
                    std::cout << "[mpeg:frame] dest=0x" << std::hex << destAddr << std::dec << " " << width << "x" << height
                              << " avg=" << (sum / std::max<size_t>(1, frame.rgba.size() / 4 * 3)) << std::endl;
                }
            }
            const uint32_t outWidth = align16(width);
            const uint32_t outHeight = align16(height);
            const uint32_t macroblockColumns = outWidth / 16u;

            for (uint32_t mbx = 0u; mbx < macroblockColumns; ++mbx)
            {
                const size_t stripOffset =
                    static_cast<size_t>(mbx) * static_cast<size_t>(outHeight) * 16u * 4u;
                for (uint32_t y = 0u; y < outHeight; ++y)
                {
                    uint8_t *dst = getMemPtr(
                        rdram,
                        destAddr + static_cast<uint32_t>(stripOffset + static_cast<size_t>(y) * 16u * 4u));
                    if (!dst)
                    {
                        continue;
                    }

                    for (uint32_t x = 0u; x < 16u; ++x)
                    {
                        const uint32_t srcX = mbx * 16u + x;
                        const uint8_t *src = nullptr;
                        if (srcX < width && y < height)
                        {
                            src = frame.rgba.data() +
                                  (static_cast<size_t>(y) * static_cast<size_t>(width) + srcX) * 4u;
                        }

                        if (src)
                        {
                            dst[x * 4u + 0u] = src[0u];
                            dst[x * 4u + 1u] = src[1u];
                            dst[x * 4u + 2u] = src[2u];
                            dst[x * 4u + 3u] = 0x80u;
                        }
                        else
                        {
                            dst[x * 4u + 0u] = 0u;
                            dst[x * 4u + 1u] = 0u;
                            dst[x * 4u + 2u] = 0u;
                            dst[x * 4u + 3u] = 0x80u;
                        }
                    }
                }
            }
        }

        void resetMpegStubStateUnlocked()
        {
            g_mpeg_stub_state.initialized = false;
            g_mpeg_stub_state.nextCallbackHandle = 1u;
            g_mpeg_stub_state.cdStreamGeneration = 0u;
            g_mpeg_stub_state.cdStreamBytesProduced = 0u;
            g_mpeg_stub_state.cdStreamBytesDemuxed = 0u;
            g_mpeg_stub_state.cdStreamEofPending = false;
            g_mpeg_stub_state.currentCdStreamEofSeen = false;
            g_mpeg_stub_state.feedEsTraceCount = 0u;
            g_mpeg_stub_state.demuxPssTraceCount = 0u;
            g_mpeg_stub_state.demuxRingTraceCount = 0u;
            g_mpeg_stub_state.getPictureWaitTraceCount = 0u;
            g_mpeg_stub_state.pictureTraceCount = 0u;
            g_mpeg_stub_state.isEndTraceCount = 0u;
            g_mpeg_stub_state.callbacksByMpeg.clear();
            g_mpeg_stub_state.playbackByMpeg.clear();
            g_mpeg_stub_state.deletedMpeg.clear();
            ps2MovieReset();
        }
    }

    void resetMpegStubState()
    {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        resetMpegStubStateUnlocked();
    }

    void enqueueMpegDecodedFrameForTesting(uint32_t mpegAddr)
    {
        constexpr int kTestFrameWidth = 16;
        constexpr int kTestFrameHeight = 16;

        MpegDecodedFrame frame;
        frame.width = kTestFrameWidth;
        frame.height = kTestFrameHeight;
        frame.rgba.resize(static_cast<size_t>(kTestFrameWidth * kTestFrameHeight * 4), 0x80u);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        playback.sawInput = true;
        playback.decodedFrames.push_back(std::move(frame));
    }

    void notifyMpegCdStreamStart(PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        ++g_mpeg_stub_state.cdStreamGeneration;
        g_mpeg_stub_state.cdStreamBytesProduced = 0u;
        g_mpeg_stub_state.cdStreamBytesDemuxed = 0u;
        g_mpeg_stub_state.cdStreamEofPending = false;
        g_mpeg_stub_state.currentCdStreamEofSeen = false;
        g_mpeg_stub_state.feedEsTraceCount = 0u;
        g_mpeg_stub_state.demuxPssTraceCount = 0u;
        g_mpeg_stub_state.demuxRingTraceCount = 0u;
        g_mpeg_stub_state.getPictureWaitTraceCount = 0u;
        g_mpeg_stub_state.pictureTraceCount = 0u;
        g_mpeg_stub_state.isEndTraceCount = 0u;

        for (auto &[mpegAddr, playback] : g_mpeg_stub_state.playbackByMpeg)
        {
            playback = makeFreshPlaybackStatePreservingConfig(playback);
        }
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[MPEG:CdStreamStart] generation=" << g_mpeg_stub_state.cdStreamGeneration
                      << " reopened=" << g_mpeg_stub_state.playbackByMpeg.size() << std::endl;
        });
    }

    void notifyMpegCdStreamDataProduced(uint32_t byteCount, bool endOfStream)
    {
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.cdStreamBytesProduced += byteCount;
        if (endOfStream)
        {
            g_mpeg_stub_state.cdStreamEofPending = true;
        }
    }

    void notifyMpegCdStreamEof(PS2Runtime *runtime)
    {
        std::vector<uint32_t> completedMpegIds;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            finalizeCdStreamEofUnlocked(completedMpegIds, changed);
        }

        if (changed)
        {
            static uint32_t s_eofLogCount = 0u;
            if (s_eofLogCount < 8u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[MPEG:CdStreamEof] finalized active MPEG playback" << std::endl;
                });
                ++s_eofLogCount;
            }
        }
        if (runtime)
        {
            for (const uint32_t mpegAddr : completedMpegIds)
            {
                runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
            }
        }
    }

    void sceMpegFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        bool wakePictureWaiter = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            const size_t framesBefore = playback.decodedFrames.size();
            if (playback.decoder)
            {
                playback.decoder->flush(playback.decodedFrames);
                stampDecodedFrames(playback);
            }
            wakePictureWaiter = playback.decodedFrames.size() != framesBefore ||
                                playback.streamEnded ||
                                playback.decoderFailed;
        }
        if (wakePictureWaiter)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        setReturnS32(ctx, 0);
    }

    void sceMpegAddBs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t dataAddr = getRegU32(ctx, 5);
        const uint32_t byteCount = getRegU32(ctx, 6);

        size_t copied = 0u;
        bool wakePictureWaiter = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            playback.fedViaAddBs = true;
            const size_t framesBefore = playback.decodedFrames.size();
            while (copied < byteCount)
            {
                const uint32_t curAddr = dataAddr + static_cast<uint32_t>(copied);
                const uint32_t offset = curAddr & PS2_RAM_MASK;
                const size_t chunk = std::min<size_t>(static_cast<size_t>(byteCount) - copied, PS2_RAM_SIZE - offset);
                const uint8_t *src = getConstMemPtr(rdram, curAddr);
                if (!src || chunk == 0u)
                {
                    break;
                }
                feedElementaryStream(playback, src, chunk);
                copied += chunk;
            }
            wakePictureWaiter = playback.decodedFrames.size() != framesBefore || playback.streamEnded || playback.decoderFailed;
        }

        if (wakePictureWaiter)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        setReturnS32(ctx, static_cast<int32_t>(copied));
    }

    void sceMpegAddCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t callbackType = getRegU32(ctx, 5);
        const uint32_t callbackFunc = getRegU32(ctx, 6);
        const uint32_t callbackData = getRegU32(ctx, 7);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        (void)getPlaybackState(mpegAddr);

        const uint32_t handle = g_mpeg_stub_state.nextCallbackHandle++;
        g_mpeg_stub_state.callbacksByMpeg[mpegAddr].push_back(
            MpegRegisteredCallback{callbackType, 0u, callbackFunc, callbackData, handle, false});

        setReturnU32(ctx, handle);
    }

    void sceMpegAddStrCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t streamType = getRegU32(ctx, 5);
        const uint32_t streamId = getRegU32(ctx, 6);
        const uint32_t callbackFunc = getRegU32(ctx, 7);
        const uint32_t callbackData = readAbiArg4(rdram, ctx);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        (void)getPlaybackState(mpegAddr);
        const uint32_t handle = g_mpeg_stub_state.nextCallbackHandle++;
        g_mpeg_stub_state.callbacksByMpeg[mpegAddr].push_back(
            MpegRegisteredCallback{streamType, streamId, callbackFunc, callbackData, handle, true});
        setReturnU32(ctx, 0u);
    }

    void sceMpegClearRefBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        (void)runtime;
        static const uint32_t kRefGlobalAddrs[] = {
            0x171800u, 0x17180Cu, 0x171818u, 0x171804u, 0x171810u, 0x17181Cu};
        for (uint32_t addr : kRefGlobalAddrs)
        {
            uint8_t *p = getMemPtr(rdram, addr);
            if (!p)
                continue;
            uint32_t ptr = *reinterpret_cast<uint32_t *>(p);
            if (ptr != 0u)
            {
                uint8_t *q = getMemPtr(rdram, ptr + 0x28u);
                if (q)
                    *reinterpret_cast<uint32_t *>(q) = 0u;
            }
        }
        setReturnU32(ctx, 1u);
    }

    static void mpegGuestWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (uint8_t *p = getMemPtr(rdram, addr))
            *reinterpret_cast<uint32_t *>(p) = value;
    }
    static void mpegGuestWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        if (uint8_t *p = getMemPtr(rdram, addr))
        {
            *reinterpret_cast<uint32_t *>(p) = static_cast<uint32_t>(value);
            *reinterpret_cast<uint32_t *>(p + 4) = static_cast<uint32_t>(value >> 32);
        }
    }

    void sceMpegCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t param_1 = getRegU32(ctx, 4); // a0
        const uint32_t param_2 = getRegU32(ctx, 5); // a1
        const uint32_t param_3 = getRegU32(ctx, 6); // a2

        const uint32_t uVar3 = (param_2 + 3u) & 0xFFFFFFFCu;
        const int32_t iVar2_signed = static_cast<int32_t>(param_3) - static_cast<int32_t>(uVar3 - param_2);

        if (iVar2_signed <= 0x117)
        {
            setReturnU32(ctx, 0u);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            getPlaybackState(param_1) = makeFreshPlaybackState();
            g_mpeg_stub_state.deletedMpeg.erase(param_1);
        }
        ps2MovieSetPlaying(param_1, false); // a decoder created over an old address starts as not playing

        const uint32_t puVar4 = uVar3 + 0x108u;
        const uint32_t innerSize = static_cast<uint32_t>(iVar2_signed) - 0x118u;

        mpegGuestWrite32(rdram, param_1 + 0x40, uVar3);

        const uint32_t a1_init = uVar3 + 0x118u;
        mpegGuestWrite32(rdram, puVar4 + 0x0, a1_init);
        mpegGuestWrite32(rdram, puVar4 + 0x4, innerSize);
        mpegGuestWrite32(rdram, puVar4 + 0x8, a1_init);
        mpegGuestWrite32(rdram, puVar4 + 0xC, a1_init);

        const uint32_t allocResult = runtime ? runtime->guestMalloc(0x600, 8u) : (uVar3 + 0x200u);
        mpegGuestWrite32(rdram, uVar3 + 0x44, allocResult);

        // param_1[0..2] = 0; param_1[4..0xe] = 0xffffffff/0 as per decompilation
        mpegGuestWrite32(rdram, param_1 + 0x00, 0);
        mpegGuestWrite32(rdram, param_1 + 0x04, 0);
        mpegGuestWrite32(rdram, param_1 + 0x08, 0);
        mpegGuestWrite64(rdram, param_1 + 0x10, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x18, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x20, 0);
        mpegGuestWrite64(rdram, param_1 + 0x28, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x30, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite64(rdram, param_1 + 0x38, 0);

        static const unsigned s_zeroOffsets[] = {
            0xB4, 0xB8, 0xBC, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD4, 0xD8, 0xDC, 0xE0, 0xE4, 0xE8, 0xF8,
            0x0C, 0x14, 0x2C, 0x34, 0x3C,
            0x48, 0xFC, 0x100, 0x104, 0x70, 0x90, 0xAC};
        for (unsigned off : s_zeroOffsets)
            mpegGuestWrite32(rdram, uVar3 + off, 0u);
        mpegGuestWrite64(rdram, uVar3 + 0x78, 0);
        mpegGuestWrite64(rdram, uVar3 + 0x88, 0);

        mpegGuestWrite64(rdram, uVar3 + 0xF0, 0xFFFFFFFFFFFFFFFFULL);
        mpegGuestWrite32(rdram, uVar3 + 0x1C, 0x1209F8u);
        mpegGuestWrite32(rdram, uVar3 + 0x24, 0x120A08u);
        mpegGuestWrite32(rdram, uVar3 + 0xB0, 1u);
        mpegGuestWrite32(rdram, uVar3 + 0x9C, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x80, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x94, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, uVar3 + 0x98, 0xFFFFFFFFu);

        mpegGuestWrite32(rdram, 0x1717BCu, param_1);

        static const uint32_t s_refValues[] = {
            0x171A50u, 0x171C58u, 0x171CC0u, 0x171D28u, 0x171D90u,
            0x171AB8u, 0x171B20u, 0x171B88u, 0x171BF0u};
        for (unsigned i = 0; i < 9u; ++i)
            mpegGuestWrite32(rdram, 0x171800u + i * 4u, s_refValues[i]);

        uint32_t setDynamicRet = a1_init;
        if (uint8_t *p = getMemPtr(rdram, puVar4 + 8))
            setDynamicRet = *reinterpret_cast<uint32_t *>(p);
        mpegGuestWrite32(rdram, puVar4 + 12, setDynamicRet);

        setReturnU32(ctx, setDynamicRet);
    }

    void sceMpegDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;

        const uint32_t mpegAddr = getRegU32(ctx, 4);
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            g_mpeg_stub_state.callbacksByMpeg.erase(mpegAddr);
            g_mpeg_stub_state.playbackByMpeg.erase(mpegAddr);
            g_mpeg_stub_state.deletedMpeg.insert(mpegAddr);
        }
        ps2MovieSetPlaying(mpegAddr, false); // the movie ends with its decoder
        runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_WAIT_DELETE);
        setReturnU32(ctx, 0u);
    }

    void sceMpegDemuxPss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t dataAddr = getRegU32(ctx, 5);
        const uint32_t byteCount = getRegU32(ctx, 6);

        std::vector<MpegStreamCallbackEvent> callbackEvents;
        std::vector<uint32_t> completedMpegIds;
        size_t consumed = 0u;
        size_t decodedBefore = 0u;
        size_t decodedCount = 0u;
        uint32_t traceIdx = 0u;
        bool eofChanged = false;
        bool backpressured = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            decodedBefore = playback.decodedFrames.size();
            // Unlike sceMpegDemuxPssRing, callers of the linear variant typically ignore the
            // return value and move on to the next buffer, so declining input here drops video
            // data and corrupts the stream. Such games already pace input through their own
            // ring buffer (see ipu_ring), so only apply backpressure at a generous hard cap.
            backpressured = playback.decodedFrames.size() >= kMaxDecodedPicturesLinearDemux;
            if (!backpressured)
            {
                consumed = appendGuestBytes(mpegAddr, playback, rdram, dataAddr, byteCount, callbackEvents);
                recordCdStreamBytesDemuxedUnlocked(consumed, completedMpegIds, eofChanged);
            }
            decodedCount = playback.decodedFrames.size();
            traceIdx = g_mpeg_stub_state.demuxPssTraceCount++;
        }

        if (backpressured)
        {
            if (traceIdx < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[MPEG:DemuxPss:BACKPRESSURE] mpeg=0x" << std::hex << mpegAddr << std::dec << " decoded=" << decodedCount << std::endl;
                });
            }
            setReturnS32(ctx, 0);
            return;
        }
        const bool currentStreamCompleted = std::find(completedMpegIds.begin(), completedMpegIds.end(), mpegAddr) != completedMpegIds.end();
        if (decodedCount != decodedBefore || eofChanged || currentStreamCompleted)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        for (const uint32_t completedMpegId : completedMpegIds)
        {
            if (completedMpegId != mpegAddr)
            {
                runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, completedMpegId, KE_OK);
            }
        }

        if (traceIdx < 32u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:DemuxPss] mpeg=0x" << std::hex << mpegAddr
                          << " data=0x" << dataAddr << std::dec
                          << " bytes=" << byteCount
                          << " consumed=" << consumed
                          << " decoded=" << decodedCount
                          << " callbacks=" << callbackEvents.size()
                          << std::endl;
            });
        }

        (void)deliverStreamEventsSync(rdram, ctx, runtime, mpegAddr, callbackEvents);
        setReturnS32(ctx, static_cast<int32_t>(consumed));
    }

    void sceMpegDemuxPssRing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint32_t> s_demuxRingEntryCount{0u};
        const uint32_t entryIdx = s_demuxRingEntryCount.fetch_add(1u, std::memory_order_relaxed);
        if (entryIdx < 4u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:DemuxPssRing:ENTER] call #" << entryIdx
                          << " pc=0x" << std::hex << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            });
        }

        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t dataAddr = getRegU32(ctx, 5);
        const uint32_t availableBytes = getRegU32(ctx, 6);
        const uint32_t ringBaseAddr = getRegU32(ctx, 7);
        const uint32_t ringSize = readAbiArg4(rdram, ctx);

        std::vector<MpegStreamCallbackEvent> callbackEvents;
        std::vector<uint32_t> completedMpegIds;
        size_t consumed = 0u;
        size_t decodedBefore = 0u;
        size_t decodedCount = 0u;
        uint32_t traceIdx = 0u;
        bool eofChanged = false;
        bool backpressured = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            decodedBefore = playback.decodedFrames.size();
            backpressured = mpegDemuxBackpressured(playback);
            traceIdx = g_mpeg_stub_state.demuxRingTraceCount++;
        }
        if (!backpressured)
            consumed = demuxRingPacketsSync(rdram, ctx, runtime, mpegAddr, dataAddr, availableBytes, ringBaseAddr, ringSize,
                                            completedMpegIds, eofChanged);
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            decodedCount = getPlaybackState(mpegAddr).decodedFrames.size();
        }

        if (backpressured)
        {
            if (traceIdx < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[MPEG:DemuxPssRing:BACKPRESSURE] mpeg=0x" << std::hex << mpegAddr
                              << std::dec << " decoded=" << decodedCount
                              << " avail=" << availableBytes << std::endl;
                });
            }
            setReturnS32(ctx, 0);
            return;
        }
        const bool currentStreamCompleted = std::find(completedMpegIds.begin(), completedMpegIds.end(), mpegAddr) != completedMpegIds.end();
        if (decodedCount != decodedBefore || eofChanged || currentStreamCompleted)
        {
            runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, mpegAddr, KE_OK);
        }
        for (const uint32_t completedMpegId : completedMpegIds)
        {
            if (completedMpegId != mpegAddr)
            {
                runtime->eeScheduler().completeExternalWait(kMpegPictureWaitType, completedMpegId, KE_OK);
            }
        }

        if (traceIdx < 32u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:DemuxPssRing] mpeg=0x" << std::hex << mpegAddr
                          << " data=0x" << dataAddr
                          << " ring=0x" << ringBaseAddr << std::dec
                          << " avail=" << availableBytes
                          << " consumed=" << consumed
                          << " decoded=" << decodedCount
                          << " callbacks=" << callbackEvents.size()
                          << std::endl;
            });
        }

        setReturnS32(ctx, static_cast<int32_t>(consumed));
    }

    void sceMpegDispCenterOffX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegDispCenterOffY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegDispHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).height);
    }

    void sceMpegDispWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).width);
    }

    void sceMpegGetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        setReturnU32(ctx, getPlaybackState(mpegAddr).decodeMode);
    }

    void sceMpegGetPicture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t imageAddr = getRegU32(ctx, 5);
        // A decoder that asks for pictures is a movie from this first call on, not from the first delivered picture: the
        // stream needs several frames after creation before the first picture is ready, and the game already shows the
        // movie screen (blank or fading) in that time; the window must be boxed 4:3 for it, or a movie that follows another
        // one flashes 16:9 in between. A decoder that is only created and fed (audio-only) never calls this.
        bool deleted = false;
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            deleted = g_mpeg_stub_state.deletedMpeg.count(mpegAddr) != 0u;
        }
        if (!deleted)
            ps2MovieSetPlaying(mpegAddr, true);
        uint32_t width = kStubMovieWidth;
        uint32_t height = kStubMovieHeight;
        uint32_t frameCount = 0u;
        bool haveFrame = false;
        MpegDecodedFrame frame;
        std::vector<MpegRegisteredCallback> backgroundCallbacks;
        std::vector<MpegRegisteredCallback> dmaCallbacks;
        {
            std::unique_lock<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(mpegAddr);
            backgroundCallbacks = matchingBackgroundCallbacks(mpegAddr);
            // Bitstreams handed over whole with sceMpegAddBs have no end marker, so the
            // decoder would keep its last reordered pictures forever; drain it instead of
            // waiting for input that will never come.
            if (playback.decodedFrames.empty() && playback.fedViaAddBs && playback.decoder)
            {
                playback.decoder->flush(playback.decodedFrames);
                stampDecodedFrames(playback);
            }
            if (playback.decodedFrames.empty() &&
                !playback.fedViaAddBs &&
                !g_mpeg_stub_state.currentCdStreamEofSeen &&
                !playback.streamEnded &&
                !playback.decoderFailed)
            {
                if (g_mpeg_stub_state.getPictureWaitTraceCount < 32u)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:GetPicture] waiting for frames, mpeg=0x" << std::hex << mpegAddr
                                  << std::dec << " ended=" << playback.streamEnded
                                  << " failed=" << playback.decoderFailed
                                  << " sawInput=" << playback.sawInput << std::endl;
                    });
                    ++g_mpeg_stub_state.getPictureWaitTraceCount;
                }
                lock.unlock();
                runtime->eeScheduler().waitExternal(
                    EeWaitReason::Mpeg,
                    kMpegPictureWaitType,
                    mpegAddr,
                    [rdram, runtime](R5900Context &resumeContext)
                    {
                        if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
                        {
                            return;
                        }
                        sceMpegGetPicture(rdram, &resumeContext, runtime);
                    });
            }

            if (!playback.decodedFrames.empty())
            {
                const uint64_t currentTick = runtime->eeScheduler().currentVSyncTick();
                const uint64_t currentTickQ32 = currentTick << 32u;
                const MpegDecodedFrame &nextFrame = playback.decodedFrames.front();
                const uint64_t frameIntervalQ32 = decodedFrameIntervalQ32(playback, nextFrame);
                uint64_t presentationTargetQ32 = presentationTickForFrame(playback, nextFrame, currentTickQ32);

                if (currentTickQ32 > presentationTargetQ32 && currentTickQ32 - presentationTargetQ32 >= frameIntervalQ32)
                {
                    const uint64_t correction = currentTickQ32 - presentationTargetQ32;
                    presentationTargetQ32 = currentTickQ32;
                    if (nextFrame.pts90k >= 0 && playback.firstPresentedPts90k >= 0)
                    {
                        playback.ptsPresentationBaseTickQ32 += correction;
                    }
                    playback.nextPictureTickQ32 = currentTickQ32;
                }

                if (currentTickQ32 < presentationTargetQ32)
                {
                    const uint64_t eligibleTick = (presentationTargetQ32 + kPictureClockOne - 1u) >> 32u;
                    lock.unlock();
                    runtime->eeScheduler().waitVSync(
                        eligibleTick - 1u,
                        -1,
                        [rdram, runtime](R5900Context &resumeContext)
                        {
                            if (static_cast<int32_t>(getRegU32(&resumeContext, 2)) < 0)
                            {
                                return;
                            }
                            sceMpegGetPicture(rdram, &resumeContext, runtime);
                        });
                }

                frame = std::move(playback.decodedFrames.front());
                playback.decodedFrames.pop_front();
                if (frame.videoBytesMark > playback.videoBytesPresented)
                {
                    ps2_stubs::ipu_ring::addConsumedBytes(frame.videoBytesMark - playback.videoBytesPresented);
                    playback.videoBytesPresented = frame.videoBytesMark;
                }
                dmaCallbacks = matchingDmaCallbacks(mpegAddr, !playback.nodataCallbackDispatched);
                playback.nodataCallbackDispatched = true;
                playback.width = static_cast<uint32_t>(frame.width);
                playback.height = static_cast<uint32_t>(frame.height);
                width = playback.width;
                height = playback.height;
                frameCount = playback.picturesServed;
                playback.picturesServed += 1u;
                playback.nextPictureTickQ32 = presentationTargetQ32 + frameIntervalQ32;
                playback.presentationEndTickQ32 = playback.nextPictureTickQ32;
                haveFrame = true;
                if (g_mpeg_stub_state.pictureTraceCount < 32u)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[MPEG:GetPicture:FRAME] mpeg=0x" << std::hex << mpegAddr
                                  << std::dec << " generation=" << g_mpeg_stub_state.cdStreamGeneration
                                  << " frame=" << frameCount
                                  << " queued=" << playback.decodedFrames.size()
                                  << " size=" << width << "x" << height << std::endl;
                    });
                    ++g_mpeg_stub_state.pictureTraceCount;
                }
            }
            else
            {
                width = playback.width;
                height = playback.height;
                frameCount = playback.picturesServed;
            }
        }

        mpegGuestWrite32(rdram, mpegAddr + 0x00u, width);
        mpegGuestWrite32(rdram, mpegAddr + 0x04u, height);
        mpegGuestWrite32(rdram, mpegAddr + 0x08u, frameCount);

        if (uint8_t *base = getMemPtr(rdram, mpegAddr))
        {
            const uint32_t iVar1 = *reinterpret_cast<uint32_t *>(base + 0x40);
            if (uint8_t *inner = getMemPtr(rdram, iVar1))
            {
                *reinterpret_cast<uint32_t *>(inner + 0xb0) = 1;
                *reinterpret_cast<uint32_t *>(inner + 0xd8) = (getRegU32(ctx, 5) & 0x0FFFFFFFu) | 0x20000000u;
                *reinterpret_cast<uint32_t *>(inner + 0xe4) = getRegU32(ctx, 6);
                *reinterpret_cast<uint32_t *>(inner + 0xdc) = 0;
                *reinterpret_cast<uint32_t *>(inner + 0xe0) = 0;
            }
        }

        {
            static uint32_t s_gpTrace = 0u;
            if (ps2_debug::traceSampled() && (s_gpTrace++ % 60u) == 0u)
            {
                std::cout << "[mpeg:getpic] have=" << haveFrame << " image=0x" << std::hex << imageAddr << std::dec
                          << " rgba=" << frame.rgba.size() << " " << frame.width << "x" << frame.height << std::endl;
            }
        }
        if (haveFrame)
        {
            writeDecodedFrameToGuest(rdram, imageAddr, frame);

            // Real libmpeg brackets IPU use with the StopDMA/RestartDMA callbacks; games that
            // stream through their own ring buffer read the IPU's input position there.
            for (const MpegRegisteredCallback &callback : dmaCallbacks)
            {
                MpegStreamCallbackEvent event{};
                event.mpegAddr = mpegAddr;
                event.streamType = callback.type;
                dispatchGuestStreamCallback(rdram, ctx, runtime, event, callback);
            }
        }
        else if (frameCount == 0u)
        {
            writeBlankMpegFrame(rdram, imageAddr, width, height);
        }

        // Every sceMpegGetPicture on hardware spends time waiting on the IPU, during which libmpeg
        // runs the Background callbacks; give them one turn per call.
        for (const MpegRegisteredCallback &callback : backgroundCallbacks)
        {
            MpegStreamCallbackEvent event{};
            event.mpegAddr = mpegAddr;
            event.streamType = callback.type;
            dispatchGuestStreamCallback(rdram, ctx, runtime, event, callback);
        }

        setReturnS32(ctx, 0);
    }

    void sceMpegGetPictureRAW8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceMpegGetPictureRAW8", rdram, ctx, runtime);
    }

    void sceMpegGetPictureRAW8xy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceMpegGetPictureRAW8xy", rdram, ctx, runtime);
    }

    void sceMpegInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        const uint64_t cdStreamGeneration = g_mpeg_stub_state.cdStreamGeneration;
        const uint64_t cdStreamBytesProduced = g_mpeg_stub_state.cdStreamBytesProduced;
        const uint64_t cdStreamBytesDemuxed = g_mpeg_stub_state.cdStreamBytesDemuxed;
        const bool cdStreamEofPending = g_mpeg_stub_state.cdStreamEofPending;
        const bool currentCdStreamEofSeen = g_mpeg_stub_state.currentCdStreamEofSeen;
        resetMpegStubStateUnlocked();
        g_mpeg_stub_state.initialized = true;
        g_mpeg_stub_state.cdStreamGeneration = cdStreamGeneration;
        g_mpeg_stub_state.cdStreamBytesProduced = cdStreamBytesProduced;
        g_mpeg_stub_state.cdStreamBytesDemuxed = cdStreamBytesDemuxed;
        g_mpeg_stub_state.cdStreamEofPending = cdStreamEofPending;
        g_mpeg_stub_state.currentCdStreamEofSeen = currentCdStreamEofSeen;
        setReturnU32(ctx, 0u);
    }

    void sceMpegIsEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t mpegAddr = getRegU32(ctx, 4);

        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        g_mpeg_stub_state.initialized = true;
        MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        // Only the producer/demux EOF is authoritative. A sequence_end_code can
        // be observed while more PSS data is still buffered, and a decoder
        // failure before producer EOF may still recover on a later sequence.
        const bool producerEnded =
            g_mpeg_stub_state.currentCdStreamEofSeen &&
            playback.cdStreamGeneration == g_mpeg_stub_state.cdStreamGeneration;
        // The stream's own program end code after the video's sequence end is also the end, whatever fed the data:
        // ZOE's Viola movie is not read through the sceCdSt stream, so no producer EOF ever arrives and the game
        // (which polls sceMpegIsEnd every frame) waited forever on the last picture.
        const bool dataEnded = playback.sawProgramEnd && playback.sawSequenceEnd && playback.pssBuffer.empty();
        const bool ended = (producerEnded &&
                            (playback.streamEnded || (playback.decoderFailed && playback.sawInput))) ||
                           dataEnded;
        const uint64_t presentationEnd = playback.presentationEndTickQ32;
        const uint64_t currentTickQ32 = runtime != nullptr
                                            ? (runtime->eeScheduler().currentVSyncTick() << 32u)
                                            : std::numeric_limits<uint64_t>::max();
        const bool presentationComplete =
            presentationEnd == std::numeric_limits<uint64_t>::max() ||
            currentTickQ32 >= presentationEnd;

        if (g_mpeg_stub_state.isEndTraceCount < 16u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[MPEG:IsEnd] mpeg=0x" << std::hex << mpegAddr << std::dec
                          << " ended=" << ended
                          << " producerEof=" << producerEnded
                          << " seqEnd=" << playback.sawSequenceEnd
                          << " streamEnded=" << playback.streamEnded
                          << " presentationComplete=" << presentationComplete
                          << " frames=" << playback.decodedFrames.size()
                          << " sawInput=" << playback.sawInput << std::endl;
            });
            ++g_mpeg_stub_state.isEndTraceCount;
        }

        setReturnS32(ctx, (ended && playback.decodedFrames.empty() && presentationComplete) ? 1 : 0);
    }

    void sceMpegIsRefBuffEmpty(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        const MpegPlaybackState &playback = getPlaybackState(mpegAddr);
        setReturnS32(ctx, playback.decodedFrames.empty() ? 1 : 0);
    }

    void sceMpegReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t param_1 = getRegU32(ctx, 4);
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegPlaybackState &playback = getPlaybackState(param_1);
            MpegPlaybackState resetState = makeFreshPlaybackStatePreservingConfig(playback);
            if (playback.streamEnded || playback.decoderFailed)
            {
                resetState.sawInput = true;
                resetState.streamEnded = true;
                resetState.cdStreamGeneration = playback.cdStreamGeneration;
            }
            playback = std::move(resetState);
        }
        uint8_t *base = getMemPtr(rdram, param_1);
        if (!base)
        {
            return;
        }
        uint32_t inner = *reinterpret_cast<uint32_t *>(base + 0x40);
        if (inner == 0u)
            return;
        mpegGuestWrite32(rdram, param_1 + 0x00u, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x04u, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x08u, 0u);
        mpegGuestWrite32(rdram, inner + 0x00, 0u);
        mpegGuestWrite32(rdram, inner + 0x04, 0u);
        mpegGuestWrite32(rdram, inner + 0x08, 0u);
        mpegGuestWrite32(rdram, param_1 + 0x08, 0u);
        mpegGuestWrite32(rdram, inner + 0x80, 0xFFFFFFFFu);
        mpegGuestWrite32(rdram, inner + 0xAC, 0u);
        mpegGuestWrite32(rdram, 0x171904u, 0u);
    }

    void sceMpegResetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t mode = getRegU32(ctx, 5);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        getPlaybackState(mpegAddr).decodeMode = mode;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceMpegSetImageBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        const uint32_t imageBufferAddr = getRegU32(ctx, 5);
        std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
        getPlaybackState(mpegAddr).imageBufferAddr = imageBufferAddr;
        setReturnS32(ctx, 0);
    }
}

namespace ps2_stubs
{
    namespace
    {
        // The decoder (ffmpeg) state cannot be saved: no state while a movie stream exists.
        const char *mpegBlocker()
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            if (!g_mpeg_stub_state.playbackByMpeg.empty())
                return "a movie stream is open";
            for (const auto &[mpeg, pending] : g_pendingStreamEvents)
                if (!pending.events.empty())
                    return "movie stream callbacks are pending";
            return nullptr;
        }
        void saveMpegPart(ps2_state::Writer &w)
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            const MpegStubState &s = g_mpeg_stub_state;
            w.boolean(s.initialized);
            w.u32(s.nextCallbackHandle);
            w.u64(s.cdStreamGeneration);
            w.u64(s.cdStreamBytesProduced);
            w.u64(s.cdStreamBytesDemuxed);
            w.boolean(s.cdStreamEofPending);
            w.boolean(s.currentCdStreamEofSeen);
            w.u64(s.callbacksByMpeg.size());
            for (const auto &[mpeg, callbacks] : s.callbacksByMpeg)
            {
                w.u32(mpeg);
                w.podVector(callbacks);
            }
            w.u32(g_streamStagingAddr);
            w.u32(g_streamStagingSize);
        }
        void loadMpegPart(ps2_state::Reader &r)
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            MpegStubState &s = g_mpeg_stub_state;
            s.playbackByMpeg.clear();
            s.deletedMpeg.clear();
            g_pendingStreamEvents.clear();
            // Movie state is not saved: a state loaded during (or into) a movie starts as "no movie" and the decoder's
            // next picture request marks it again. Without this, a load during a movie left the window boxed 4:3 (and the
            // widescreen HUD off) for the rest of the session.
            ps2MovieReset();
            s.initialized = r.boolean();
            s.nextCallbackHandle = r.u32();
            s.cdStreamGeneration = r.u64();
            s.cdStreamBytesProduced = r.u64();
            s.cdStreamBytesDemuxed = r.u64();
            s.cdStreamEofPending = r.boolean();
            s.currentCdStreamEofSeen = r.boolean();
            s.callbacksByMpeg.clear();
            const uint64_t n = r.u64();
            for (uint64_t i = 0; i < n && r.ok(); ++i)
            {
                const uint32_t mpeg = r.u32();
                r.podVector(s.callbacksByMpeg[mpeg]);
            }
            g_streamStagingAddr = r.u32();
            g_streamStagingSize = r.u32();
        }
        // Diagnostics when a save is refused (for example during a movie that stopped advancing).
        void describeMpegPart()
        {
            std::lock_guard<std::mutex> lock(g_mpeg_stub_mutex);
            for (const auto &[mpeg, p] : g_mpeg_stub_state.playbackByMpeg)
                std::cout << "[state]   mpeg 0x" << std::hex << mpeg << std::dec << " served=" << p.picturesServed
                          << " decoded=" << p.decodedFrames.size() << " " << p.width << "x" << p.height << " input=" << p.sawInput
                          << " seqEnd=" << p.sawSequenceEnd << " ended=" << p.streamEnded << " failed=" << p.decoderFailed
                          << " waitSeqHdr=" << p.waitingForVideoSequenceHeader << " pss=" << p.pssBuffer.size()
                          << " nextPicTick=" << (p.nextPictureTickQ32 >> 32u) << " endTick=" << (p.presentationEndTickQ32 >> 32u)
                          << " firstPts=" << p.firstPresentedPts90k << " fed=" << p.videoBytesFed
                          << " presented=" << p.videoBytesPresented << " addBs=" << p.fedViaAddBs << std::endl;
            for (const auto &[mpeg, pending] : g_pendingStreamEvents)
                std::cout << "[state]   mpeg 0x" << std::hex << mpeg << std::dec << " pending stream events=" << pending.events.size()
                          << std::endl;
            std::cout << "[state]   mpeg cd stream: generation=" << g_mpeg_stub_state.cdStreamGeneration
                      << " produced=" << g_mpeg_stub_state.cdStreamBytesProduced << " demuxed=" << g_mpeg_stub_state.cdStreamBytesDemuxed
                      << " eofPending=" << g_mpeg_stub_state.cdStreamEofPending << std::endl;
        }
        const bool s_mpegPart = ps2_state::registerPart({"MPEG", &saveMpegPart, &loadMpegPart, &mpegBlocker, &describeMpegPart});
    }
}
