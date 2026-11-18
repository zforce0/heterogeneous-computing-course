#include "mjpeg_decoder.h"
#include "mjpeg_huffman_tables.h"

#include <csetjmp>
#include <cstdio>
#include <utility>

namespace {

struct JpegErrorManager {
    jpeg_error_mgr base;
    jmp_buf jump_buffer;
};

void jpeg_error_exit(j_common_ptr common)
{
    JpegErrorManager* error =
        reinterpret_cast<JpegErrorManager*>(common->err);
    (*common->err->output_message)(common);
    longjmp(error->jump_buffer, 1);
}

} // namespace

bool MjpegDecoder::decode(const uint8_t* compressed_data,
                          size_t compressed_size,
                          BgrFrame& destination)
{
    if (compressed_data == NULL || compressed_size == 0) {
        return false;
    }

    jpeg_decompress_struct decoder;
    JpegErrorManager error;
    volatile bool decoder_created = false;

    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump_buffer) != 0) {
        if (decoder_created) {
            jpeg_destroy_decompress(&decoder);
        }
        return false;
    }

    jpeg_create_decompress(&decoder);
    decoder_created = true;
    jpeg_mem_src(&decoder,
                 const_cast<unsigned char*>(compressed_data),
                 static_cast<unsigned long>(compressed_size));
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    mjpeg_internal::add_missing_standard_huffman_tables(&decoder);

#ifdef JCS_EXTENSIONS
    decoder.out_color_space = JCS_EXT_BGR;
#else
    decoder.out_color_space = JCS_RGB;
#endif
    decoder.do_fancy_upsampling = FALSE;
    decoder.do_block_smoothing = FALSE;

    if (!jpeg_start_decompress(&decoder) ||
        decoder.output_components != 3) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }

    destination.resize(static_cast<int>(decoder.output_width),
                       static_cast<int>(decoder.output_height));
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW row[1];
        row[0] = destination.data.data() +
                 static_cast<size_t>(decoder.output_scanline) *
                     destination.stride;
        jpeg_read_scanlines(&decoder, row, 1);
    }

    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);

#ifndef JCS_EXTENSIONS
    for (size_t i = 0; i + 2 < destination.data.size(); i += 3) {
        std::swap(destination.data[i], destination.data[i + 2]);
    }
#endif
    return true;
}
