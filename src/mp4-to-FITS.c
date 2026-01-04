#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <fitsio.h>

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <color channel(s)> <time sampling [sec]> <mp4 video>\n", argv[0]);
        return 1;
    }

    const char *channel_arg = argv[1];
    const char *time_arg = argv[2];
    const char *video_path = argv[3];

    int use_rgb = 0;
    if (strcmp(channel_arg, "R") == 0) {
        use_rgb = 0;
    } else if (strcmp(channel_arg, "RGB") == 0) {
        use_rgb = 1;
    } else {
        printf("Error: Invalid color channel. Use 'R' or 'RGB'.\n");
        return 1;
    }

    float time_sampling = atof(time_arg);
    if (time_sampling <= 0.0) {
        printf("Error: Time sampling must be positive.\n");
        return 1;
    }

    // Suppress verbose logs from ffmpeg
    av_log_set_level(AV_LOG_ERROR);

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, video_path, NULL, NULL) < 0) {
        fprintf(stderr, "Error: Could not open video file '%s'\n", video_path);
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Error: Could not find stream info\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int video_stream_idx = -1;
    const AVCodec *codec = NULL;
    AVCodecParameters *codecpar = NULL;

    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            codecpar = fmt_ctx->streams[i]->codecpar;
            codec = avcodec_find_decoder(codecpar->codec_id);
            break;
        }
    }

    if (video_stream_idx == -1 || codec == NULL) {
        fprintf(stderr, "Error: Could not find video stream or decoder\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Error: Could not allocate codec context\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        fprintf(stderr, "Error: Could not copy codec params to context\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Error: Could not open codec\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int width = codec_ctx->width;
    int height = codec_ctx->height;

    // FPS
    double fps = 0.0;
    if (fmt_ctx->streams[video_stream_idx]->avg_frame_rate.den > 0) {
        fps = av_q2d(fmt_ctx->streams[video_stream_idx]->avg_frame_rate);
    } else if (fmt_ctx->streams[video_stream_idx]->r_frame_rate.den > 0) {
         fps = av_q2d(fmt_ctx->streams[video_stream_idx]->r_frame_rate);
    }

    if (fps <= 0.0) {
        fprintf(stderr, "Error: Invalid FPS %.2f detected.\n", fps);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    printf("Processing:\n");
    printf("  Resolution: %dx%d\n", width, height);
    printf("  FPS: %.2f\n", fps);

    // Prepare for reading frames
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();

    // Buffer for RGB frame
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(num_bytes * sizeof(uint8_t));
    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGB24, width, height, 1);

    struct SwsContext *sws_ctx = sws_getContext(
        width, height, codec_ctx->pix_fmt,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    // Prepare accumulation buffer
    long frame_count_in_sample = 0;
    // Buffer for accumulated values (using float to prevent overflow and easy division)
    float *accumulation_buffer = (float *)calloc(width * height, sizeof(float));

    // FITS setup
    char output_filename[1024];
    // Derive output filename from input filename
    const char *last_dot = strrchr(video_path, '.');
    if (last_dot) {
        size_t len = last_dot - video_path;
        if (len >= sizeof(output_filename) - 5) len = sizeof(output_filename) - 6;
        strncpy(output_filename, video_path, len);
        strcpy(output_filename + len, ".fits");
    } else {
        snprintf(output_filename, sizeof(output_filename), "%s.fits", video_path);
    }

    fitsfile *fptr;
    int status = 0;
    long naxes[3] = {width, height, 0}; // Time axis will grow

    // Delete if exists
    remove(output_filename);

    if (fits_create_file(&fptr, output_filename, &status)) {
        fits_report_error(stderr, status);
        return 1;
    }

    // Create primary array image (3D cube)
    // We don't know the exact number of frames yet, but we can update NAXIS3 later or just append?
    // Usually standard FITS requires NAXIS to be set.
    // Let's estimate total frames.
    double duration = (double)fmt_ctx->duration / AV_TIME_BASE;
    int estimated_fits_frames = (int)(duration / time_sampling) + 1;
    naxes[2] = estimated_fits_frames;

    // We will use FLOAT_IMG for the output as it is an average
    if (fits_create_img(fptr, FLOAT_IMG, 3, naxes, &status)) {
        fits_report_error(stderr, status);
        return 1;
    }

    double time_accumulated = 0.0;
    int fits_frame_idx = 1; // FITS matches 1-based indexing for planes? No, fits_write_img uses long fpixel[].
    // Note: cfitsio often takes 1-based coordinates for start pixel.

    long current_fits_slice = 0;

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet(codec_ctx, packet) == 0) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    // Convert to RGB
                    sws_scale(sws_ctx, (uint8_t const * const *)frame->data,
                              frame->linesize, 0, height,
                              rgb_frame->data, rgb_frame->linesize);

                    // Accumulate
                    for (int y = 0; y < height; y++) {
                        for (int x = 0; x < width; x++) {
                            // RGB24: 3 bytes per pixel
                            uint8_t *ptr = rgb_frame->data[0] + y * rgb_frame->linesize[0] + x * 3;
                            uint8_t r = ptr[0];
                            uint8_t g = ptr[1];
                            uint8_t b = ptr[2];

                            float val;
                            if (use_rgb) {
                                // Average RGB to grayscale
                                val = (r + g + b) / 3.0f;
                            } else {
                                // Red only
                                val = (float)r;
                            }

                            accumulation_buffer[y * width + x] += val;
                        }
                    }
                    frame_count_in_sample++;

                    // Check time
                    // frame->pts is presentation timestamp
                    // We can estimate time per frame as 1/fps
                    time_accumulated += (1.0 / fps);

                    if (time_accumulated >= time_sampling) {
                        // Write to FITS
                        float *output_buffer = (float *)malloc(width * height * sizeof(float));
                        for (int i = 0; i < width * height; i++) {
                            output_buffer[i] = accumulation_buffer[i] / frame_count_in_sample;
                        }

                        long fpixel[3] = {1, 1, current_fits_slice + 1};
                        long nelements = width * height;

                        if (fits_write_pix(fptr, TFLOAT, fpixel, nelements, output_buffer, &status)) {
                             fits_report_error(stderr, status);
                        }

                        free(output_buffer);

                        // Reset
                        memset(accumulation_buffer, 0, width * height * sizeof(float));
                        frame_count_in_sample = 0;
                        time_accumulated -= time_sampling; // keep residue? or reset to 0?
                        // Usually reset to 0 implies "next bin".
                        // But if frame rate doesn't match sampling perfectly, we might drift.
                        // Ideally we check timestamps. But averaging 'frames of this time sample' implies strict bins.
                        // Let's reset time_accumulated to 0 for simplicity or handle residue.
                        // If I subtract, I keep phase.

                        current_fits_slice++;
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    // Handle remaining frames if any?
    // If we have some accumulated frames but didn't reach time_sampling threshold at the end,
    // should we write them?
    // "One output frame is the average of input frames of this time sample"
    // If the last bin is incomplete, usually we dump it.

    if (frame_count_in_sample > 0) {
        float *output_buffer = (float *)malloc(width * height * sizeof(float));
        for (int i = 0; i < width * height; i++) {
            output_buffer[i] = accumulation_buffer[i] / frame_count_in_sample;
        }

        long fpixel[3] = {1, 1, current_fits_slice + 1};
        long nelements = width * height;

        if (fits_write_pix(fptr, TFLOAT, fpixel, nelements, output_buffer, &status)) {
             fits_report_error(stderr, status);
        }
        free(output_buffer);
        current_fits_slice++;
    }

    // Update NAXIS3 to actual number of slices
    if (fits_update_key(fptr, TLONG, "NAXIS3", &current_fits_slice, "number of time steps", &status)) {
         fits_report_error(stderr, status);
    }

    printf("Done. Wrote %ld frames to %s\n", current_fits_slice, output_filename);

    fits_close_file(fptr, &status);

    free(accumulation_buffer);
    av_free(buffer);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    sws_freeContext(sws_ctx);

    return 0;
}
