// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Utilities for animated images

#include "./anim_util.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <sstream>   // for 'ostringstream'.

#ifdef WEBP_HAVE_GIF
#include <gif_lib.h>
#endif
#include "webp/format_constants.h"
#include "webp/decode.h"
#include "webp/demux.h"

using std::ifstream;
using std::ios;
using std::ofstream;
using std::ostringstream;

static const int kNumChannels = 4;

// -----------------------------------------------------------------------------
// Common utilities.

// Returns true if the frame covers the full canvas.
static bool IsFullFrame(int width, int height,
                        int canvas_width, int canvas_height) {
  return (width == canvas_width && height == canvas_height);
}

static void AllocateFrames(AnimatedImage* const image, uint32_t frame_count) {
  image->frames.resize(frame_count);
  for (size_t i = 0; i < image->frames.size(); ++i) {
    const size_t rgba_size =
        image->canvas_width * kNumChannels * image->canvas_height;
    image->frames[i].rgba.resize(rgba_size);
  }
}

// Clear the canvas to transparent.
static void ZeroFillCanvas(uint8_t* rgba,
                           uint32_t canvas_width, uint32_t canvas_height) {
  memset(rgba, 0, canvas_width * kNumChannels * canvas_height);
}

// Clear given frame rectangle to transparent.
static void ZeroFillFrameRect(uint8_t* rgba, int rgba_stride, int x_offset,
                              int y_offset, int width, int height) {
  assert(width * kNumChannels <= rgba_stride);
  rgba += y_offset * rgba_stride + x_offset * kNumChannels;
  for (int j = 0; j < height; ++j) {
    memset(rgba, 0, width * kNumChannels);
    rgba += rgba_stride;
  }
}

// Copy width * height pixels from 'src' to 'dst'.
static void CopyCanvas(const uint8_t* src, uint8_t* dst,
                       uint32_t width, uint32_t height) {
  assert(src != NULL && dst != NULL);
  memcpy(dst, src, width * kNumChannels * height);
}

// Copy pixels in the given rectangle from 'src' to 'dst' honoring the 'stride'.
static void CopyFrameRectangle(const uint8_t* src, uint8_t* dst, int stride,
                               int x_offset, int y_offset,
                               int width, int height) {
  const int width_in_bytes = width * kNumChannels;
  assert(width_in_bytes <= stride);
  const size_t offset = y_offset * stride + x_offset * kNumChannels;
  src += offset;
  dst += offset;
  for (int j = 0; j < height; ++j) {
    memcpy(dst, src, width_in_bytes);
    src += stride;
    dst += stride;
  }
}

// Canonicalize all transparent pixels to transparent black to aid comparison.
static void CleanupTransparentPixels(uint32_t* rgba,
                                     uint32_t width, uint32_t height) {
  const uint32_t* const rgba_end = rgba + width * height;
  while (rgba < rgba_end) {
    const uint8_t alpha = (*rgba >> 24) & 0xff;
    if (alpha == 0) {
      *rgba = 0;
    }
    ++rgba;
  }
}

// Dump frame to a PAM file.
// Returns true on success.
static bool DumpFrame(const char filename[], const char dump_folder[],
                      uint32_t frame_num, const uint8_t rgba[],
                      int canvas_width, int canvas_height) {
  const std::string filename_str = filename;
  const size_t slash_idx = filename_str.find_last_of("/\\");
  const std::string base_name = (slash_idx != std::string::npos)
                                    ? filename_str.substr(slash_idx + 1)
                                    : filename_str;
  ostringstream dump_file;
  dump_file << dump_folder << "/" << base_name << "_frame_" << frame_num
            << ".pam";

  ofstream fout(dump_file.str().c_str(), ios::binary | ios::out);
  if (!fout.good()) {
    fprintf(stderr, "Error opening file for writing: %s\n",
            dump_file.str().c_str());
    return false;
  }

  fout << "P7\nWIDTH " << canvas_width << "\nHEIGHT " << canvas_height
       << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
  for (int y = 0; y < canvas_height; ++y) {
    fout.write(
        reinterpret_cast<const char*>(rgba) + y * canvas_width * kNumChannels,
        canvas_width * kNumChannels);
    if (!fout.good()) {
      fprintf(stderr, "Error writing to file: %s\n", dump_file.str().c_str());
      return 0;
    }
  }
  fout.close();
  return true;
}

// -----------------------------------------------------------------------------
// WebP Decoding.

// Returns true if this is a valid WebP bitstream.
static bool IsWebP(const std::string& file_str) {
  return WebPGetInfo(reinterpret_cast<const uint8_t*>(file_str.c_str()),
                     file_str.length(), NULL, NULL) != 0;
}

// Returns true if the current frame is a key-frame.
static bool IsKeyFrameWebP(const WebPIterator& curr, const WebPIterator& prev,
                           const DecodedFrame* const prev_frame,
                           int canvas_width, int canvas_height) {
  if (prev_frame == NULL) {
    return true;
  } else if ((!curr.has_alpha || curr.blend_method == WEBP_MUX_NO_BLEND) &&
             IsFullFrame(curr.width, curr.height,
                         canvas_width, canvas_height)) {
    return true;
  } else {
    return (prev.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND) &&
           (IsFullFrame(prev.width, prev.height, canvas_width, canvas_height) ||
            prev_frame->is_key_frame);
  }
}

// Blend a single channel of 'src' over 'dst', given their alpha channel values.
static uint8_t BlendChannelWebP(uint32_t src, uint8_t src_a, uint32_t dst,
                                uint8_t dst_a, uint32_t scale, int shift) {
  const uint8_t src_channel = (src >> shift) & 0xff;
  const uint8_t dst_channel = (dst >> shift) & 0xff;
  const uint32_t blend_unscaled = src_channel * src_a + dst_channel * dst_a;
  assert(blend_unscaled < (1ULL << 32) / scale);
  return (blend_unscaled * scale) >> 24;
}

// Blend 'src' over 'dst' assuming they are NOT pre-multiplied by alpha.
static uint32_t BlendPixelWebP(uint32_t src, uint32_t dst) {
  const uint8_t src_a = (src >> 24) & 0xff;

  if (src_a == 0) {
    return dst;
  } else {
    const uint8_t dst_a = (dst >> 24) & 0xff;
    // This is the approximate integer arithmetic for the actual formula:
    // dst_factor_a = (dst_a * (255 - src_a)) / 255.
    const uint8_t dst_factor_a = (dst_a * (256 - src_a)) >> 8;
    assert(src_a + dst_factor_a < 256);
    const uint8_t blend_a = src_a + dst_factor_a;
    const uint32_t scale = (1UL << 24) / blend_a;

    const uint8_t blend_r =
        BlendChannelWebP(src, src_a, dst, dst_factor_a, scale, 0);
    const uint8_t blend_g =
        BlendChannelWebP(src, src_a, dst, dst_factor_a, scale, 8);
    const uint8_t blend_b =
        BlendChannelWebP(src, src_a, dst, dst_factor_a, scale, 16);

    return (blend_r << 0) | (blend_g << 8) | (blend_b << 16) | (blend_a << 24);
  }
}

// Returns two ranges (<left, width> pairs) at row 'canvas_y', that belong to
// 'src' but not 'dst'. A point range is empty if the corresponding width is 0.
static void FindBlendRangeAtRowWebP(const WebPIterator* const src,
                                    const WebPIterator* const dst, int canvas_y,
                                    int* const left1, int* const width1,
                                    int* const left2, int* const width2) {
  const int src_max_x = src->x_offset + src->width;
  const int dst_max_x = dst->x_offset + dst->width;
  const int dst_max_y = dst->y_offset + dst->height;
  assert(canvas_y >= src->y_offset && canvas_y < (src->y_offset + src->height));
  *left1 = -1;
  *width1 = 0;
  *left2 = -1;
  *width2 = 0;

  if (canvas_y < dst->y_offset || canvas_y >= dst_max_y ||
      src->x_offset >= dst_max_x || src_max_x <= dst->x_offset) {
    *left1 = src->x_offset;
    *width1 = src->width;
    return;
  }

  if (src->x_offset < dst->x_offset) {
    *left1 = src->x_offset;
    *width1 = dst->x_offset - src->x_offset;
  }

  if (src_max_x > dst_max_x) {
    *left2 = dst_max_x;
    *width2 = src_max_x - dst_max_x;
  }
}

// Blend 'num_pixels' in 'src' over 'dst'.
static void BlendPixelRowWebP(uint32_t* const src, const uint32_t* const dst,
                              int num_pixels) {
  for (int i = 0; i < num_pixels; ++i) {
    uint32_t* const src_pixel_ptr = &src[i];
    const uint8_t src_alpha = (*src_pixel_ptr >> 24) & 0xff;
    if (src_alpha != 0xff) {
      const uint32_t dst_pixel = dst[i];
      *src_pixel_ptr = BlendPixelWebP(*src_pixel_ptr, dst_pixel);
    }
  }
}

// Read animated WebP bitstream 'file_str' into 'AnimatedImage' struct.
static bool ReadAnimatedWebP(const char filename[], const std::string& file_str,
                             AnimatedImage* const image, bool dump_frames,
                             const char dump_folder[]) {
  bool ok = true;
  const WebPData webp_data = {
    reinterpret_cast<const uint8_t*>(file_str.data()), file_str.size()
  };
  WebPDemuxer* const demux = WebPDemux(&webp_data);
  if (demux == NULL) return false;

  // Animation properties.
  image->canvas_width = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
  image->canvas_height = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
  image->loop_count = WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT);
  image->bgcolor = WebPDemuxGetI(demux, WEBP_FF_BACKGROUND_COLOR);

  const uint32_t frame_count = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
  const uint32_t canvas_width = image->canvas_width;
  const uint32_t canvas_height = image->canvas_height;

  // Allocate frames.
  AllocateFrames(image, frame_count);

  // Decode and reconstruct frames.
  WebPIterator prev_iter = WebPIterator();
  WebPIterator curr_iter = WebPIterator();

  for (uint32_t i = 0; i < frame_count; ++i) {
    prev_iter = curr_iter;

    // Get frame.
    if (!WebPDemuxGetFrame(demux, i + 1, &curr_iter)) {
      fprintf(stderr, "Error retrieving frame #%u\n", i);
      return false;
    }

    DecodedFrame* const prev_frame = (i > 0) ? &image->frames[i - 1] : NULL;
    uint8_t* const prev_rgba =
        (prev_frame != NULL) ? prev_frame->rgba.data() : NULL;
    DecodedFrame* const curr_frame = &image->frames[i];
    uint8_t* const curr_rgba = curr_frame->rgba.data();

    curr_frame->duration = curr_iter.duration;
    curr_frame->is_key_frame = IsKeyFrameWebP(curr_iter, prev_iter, prev_frame,
                                              canvas_width, canvas_height);

    // TODO(urvang): The logic of decoding and reconstructing the next animated
    // frame given the previous one should be a single library call (ideally a
    // user-facing API), which takes care of frame disposal, blending etc.

    // Initialize.
    if (curr_frame->is_key_frame) {
      ZeroFillCanvas(curr_rgba, canvas_width, canvas_height);
    } else {
      CopyCanvas(prev_rgba, curr_rgba, canvas_width, canvas_height);
      if (prev_iter.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND) {
        ZeroFillFrameRect(curr_rgba, canvas_width * kNumChannels,
                          prev_iter.x_offset, prev_iter.y_offset,
                          prev_iter.width, prev_iter.height);
      }
    }

    // Decode.
    const uint8_t* input = curr_iter.fragment.bytes;
    const size_t input_size = curr_iter.fragment.size;
    const size_t output_offset =
        (curr_iter.y_offset * canvas_width + curr_iter.x_offset) * kNumChannels;
    uint8_t* output = curr_rgba + output_offset;
    const int output_stride = kNumChannels * canvas_width;
    const size_t output_size = output_stride * curr_iter.height;

    if (WebPDecodeRGBAInto(input, input_size, output, output_size,
                           output_stride) == NULL) {
      ok = false;
      break;
    }

    // During the decoding of current frame, we may have set some pixels to be
    // transparent (i.e. alpha < 255). However, the value of each of these
    // pixels should have been determined by blending it against the value of
    // that pixel in the previous frame if blending method of is WEBP_MUX_BLEND.
    if (i > 0 && curr_iter.blend_method == WEBP_MUX_BLEND &&
        !curr_frame->is_key_frame) {
      if (prev_iter.dispose_method == WEBP_MUX_DISPOSE_NONE) {
        // Blend transparent pixels with pixels in previous canvas.
        for (int y = 0; y < curr_iter.height; ++y) {
          const size_t offset =
              (curr_iter.y_offset + y) * canvas_width + curr_iter.x_offset;
          BlendPixelRowWebP(reinterpret_cast<uint32_t*>(curr_rgba) + offset,
                            reinterpret_cast<uint32_t*>(prev_rgba) + offset,
                            curr_iter.width);
        }
      } else {
        assert(prev_iter.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND);
        // We need to blend a transparent pixel with its value just after
        // initialization. That is, blend it with:
        // * Fully transparent pixel if it belongs to prevRect <-- No-op.
        // * The pixel in the previous canvas otherwise <-- Need alpha-blending.
        for (int y = 0; y < curr_iter.height; ++y) {
          const int canvas_y = curr_iter.y_offset + y;
          int left1, width1, left2, width2;
          FindBlendRangeAtRowWebP(&curr_iter, &prev_iter, canvas_y, &left1,
                                  &width1, &left2, &width2);
          if (width1 > 0) {
            const size_t offset1 = canvas_y * canvas_width + left1;
            BlendPixelRowWebP(reinterpret_cast<uint32_t*>(curr_rgba) + offset1,
                              reinterpret_cast<uint32_t*>(prev_rgba) + offset1,
                              width1);
          }
          if (width2 > 0) {
            const size_t offset2 = canvas_y * canvas_width + left2;
            BlendPixelRowWebP(reinterpret_cast<uint32_t*>(curr_rgba) + offset2,
                              reinterpret_cast<uint32_t*>(prev_rgba) + offset2,
                              width2);
          }
        }
      }
    }

    // Needed only because we may want to compare with GIF later.
    CleanupTransparentPixels(reinterpret_cast<uint32_t*>(curr_rgba),
                             canvas_width, canvas_height);

    if (dump_frames) {
      ok = ok && DumpFrame(filename, dump_folder, i, curr_rgba,
                           canvas_width, canvas_height);
    }
  }
  WebPDemuxReleaseIterator(&prev_iter);
  WebPDemuxReleaseIterator(&curr_iter);
  WebPDemuxDelete(demux);
  return ok;
}

// -----------------------------------------------------------------------------
// GIF Decoding.

// Returns true if this is a valid GIF bitstream.
static bool IsGIF(const std::string& file_str) {
  const char* const cstr = file_str.c_str();
  return file_str.length() > GIF_STAMP_LEN &&
         (!memcmp(GIF_STAMP, cstr, GIF_STAMP_LEN) ||
          !memcmp(GIF87_STAMP, cstr, GIF_STAMP_LEN) ||
          !memcmp(GIF89_STAMP, cstr, GIF_STAMP_LEN));
}

#ifdef WEBP_HAVE_GIF

// GIFLIB_MAJOR is only defined in libgif >= 4.2.0.
#if defined(GIFLIB_MAJOR) && defined(GIFLIB_MINOR)
# define LOCAL_GIF_VERSION ((GIFLIB_MAJOR << 8) | GIFLIB_MINOR)
# define LOCAL_GIF_PREREQ(maj, min) \
    (LOCAL_GIF_VERSION >= (((maj) << 8) | (min)))
#else
# define LOCAL_GIF_VERSION 0
# define LOCAL_GIF_PREREQ(maj, min) 0
#endif

#if !LOCAL_GIF_PREREQ(5, 0)

// Added in v5.0
typedef struct GraphicsControlBlock {
  int DisposalMode;
#define DISPOSAL_UNSPECIFIED      0       // No disposal specified
#define DISPOSE_DO_NOT            1       // Leave image in place
#define DISPOSE_BACKGROUND        2       // Set area to background color
#define DISPOSE_PREVIOUS          3       // Restore to previous content
  bool UserInputFlag;      // User confirmation required before disposal
  int DelayTime;           // Pre-display delay in 0.01sec units
  int TransparentColor;    // Palette index for transparency, -1 if none
#define NO_TRANSPARENT_COLOR     -1
} GraphicsControlBlock;

static int DGifExtensionToGCB(const size_t GifExtensionLength,
                              const GifByteType* GifExtension,
                              GraphicsControlBlock* gcb) {
  if (GifExtensionLength != 4) {
    return GIF_ERROR;
  }
  gcb->DisposalMode = (GifExtension[0] >> 2) & 0x07;
  gcb->UserInputFlag = (GifExtension[0] & 0x02) != 0;
  gcb->DelayTime = GifExtension[1] | (GifExtension[2] << 8);
  if (GifExtension[0] & 0x01) {
    gcb->TransparentColor = static_cast<int>(GifExtension[3]);
  } else {
    gcb->TransparentColor = NO_TRANSPARENT_COLOR;
  }
  return GIF_OK;
}

static int DGifSavedExtensionToGCB(GifFileType* GifFile, int ImageIndex,
                                   GraphicsControlBlock* gcb) {
  int i;
  if (ImageIndex < 0 || ImageIndex > GifFile->ImageCount - 1) {
    return GIF_ERROR;
  }
  gcb->DisposalMode = DISPOSAL_UNSPECIFIED;
  gcb->UserInputFlag = false;
  gcb->DelayTime = 0;
  gcb->TransparentColor = NO_TRANSPARENT_COLOR;

  for (i = 0; i < GifFile->SavedImages[ImageIndex].ExtensionBlockCount; i++) {
    ExtensionBlock* ep = &GifFile->SavedImages[ImageIndex].ExtensionBlocks[i];
    if (ep->Function == GRAPHICS_EXT_FUNC_CODE) {
      return DGifExtensionToGCB(
          ep->ByteCount, reinterpret_cast<const GifByteType*>(ep->Bytes), gcb);
    }
  }
  return GIF_ERROR;
}

#define CONTINUE_EXT_FUNC_CODE 0x00

// Signature was changed in v5.0
#define DGifOpenFileName(a, b) DGifOpenFileName(a)

#endif  // !LOCAL_GIF_PREREQ(5, 0)

// Signature changed in v5.1
#if !LOCAL_GIF_PREREQ(5, 1)
#define DGifCloseFile(a, b) DGifCloseFile(a)
#endif

static void GIFDisplayError(const GifFileType* const gif, int gif_error) {
  // libgif 4.2.0 has retired PrintGifError() and added GifErrorString().
#if LOCAL_GIF_PREREQ(4, 2)
#if LOCAL_GIF_PREREQ(5, 0)
  // Static string actually, hence the const char* cast.
  const char* error_str = (const char*)GifErrorString(
      (gif == NULL) ? gif_error : gif->Error);
#else
  const char* error_str = (const char*)GifErrorString();
  (void)gif;
#endif
  if (error_str == NULL) error_str = "Unknown error";
  fprintf(stderr, "GIFLib Error %d: %s\n", gif_error, error_str);
#else
  (void)gif;
  fprintf(stderr, "GIFLib Error %d: ", gif_error);
  PrintGifError();
  fprintf(stderr, "\n");
#endif
}

static bool IsKeyFrameGIF(const GifImageDesc& prev_desc, int prev_dispose,
                          const DecodedFrame* const prev_frame,
                          int canvas_width, int canvas_height) {
  if (prev_frame == NULL) return true;
  if (prev_dispose == DISPOSE_BACKGROUND) {
    if (IsFullFrame(prev_desc.Width, prev_desc.Height,
                    canvas_width, canvas_height)) {
      return true;
    }
    if (prev_frame->is_key_frame) return true;
  }
  return false;
}

static int GetTransparentIndexGIF(GifFileType* gif) {
  GraphicsControlBlock first_gcb = GraphicsControlBlock();
  DGifSavedExtensionToGCB(gif, 0, &first_gcb);
  return first_gcb.TransparentColor;
}

static uint32_t GetBackgroundColorGIF(GifFileType* gif) {
  const int transparent_index = GetTransparentIndexGIF(gif);
  const ColorMapObject* const color_map = gif->SColorMap;
  if (transparent_index != NO_TRANSPARENT_COLOR &&
      gif->SBackGroundColor == transparent_index) {
    return 0x00ffffff;  // Special case: transparent white.
  } else if (color_map == NULL || color_map->Colors == NULL
             || gif->SBackGroundColor >= color_map->ColorCount) {
    return 0xffffffff;  // Invalid: assume white.
  } else {
    const GifColorType color = color_map->Colors[gif->SBackGroundColor];
    return (0xff << 24) |
           (color.Red << 16) |
           (color.Green << 8) |
           (color.Blue << 0);
  }
}

// Find appropriate app extension and get loop count from the next extension.
static uint32_t GetLoopCountGIF(const GifFileType* const gif) {
  for (int i = 0; i < gif->ImageCount; ++i) {
    const SavedImage* const image = &gif->SavedImages[i];
    for (int j = 0; (j + 1) < image->ExtensionBlockCount; ++j) {
      const ExtensionBlock* const eb1 = image->ExtensionBlocks + j;
      const ExtensionBlock* const eb2 = image->ExtensionBlocks + j + 1;
      const char* const signature = reinterpret_cast<const char*>(eb1->Bytes);
      const bool signature_is_ok =
          (eb1->Function == APPLICATION_EXT_FUNC_CODE) &&
          (eb1->ByteCount == 11) &&
          (!memcmp(signature, "NETSCAPE2.0", 11) ||
           !memcmp(signature, "ANIMEXTS1.0", 11));
      if (signature_is_ok &&
          eb2->Function == CONTINUE_EXT_FUNC_CODE && eb2->ByteCount >= 3 &&
          eb2->Bytes[0] == 1) {
        return (static_cast<uint32_t>(eb2->Bytes[2]) << 8) +
               (static_cast<uint32_t>(eb2->Bytes[1]) << 0);
      }
    }
  }
  return 0;  // Default.
}

// Get duration of 'n'th frame in milliseconds.
static int GetFrameDurationGIF(GifFileType* gif, int n) {
  GraphicsControlBlock gcb = GraphicsControlBlock();
  DGifSavedExtensionToGCB(gif, n, &gcb);
  return gcb.DelayTime * 10;
}

// Returns true if frame 'target' completely covers 'covered'.
static bool CoversFrameGIF(const GifImageDesc& target,
                           const GifImageDesc& covered) {
  return target.Left <= covered.Left &&
         covered.Left + covered.Width <= target.Left + target.Width &&
         target.Top <= covered.Top &&
         covered.Top + covered.Height <= target.Top + target.Height;
}

static void RemapPixelsGIF(const uint8_t* const src,
                           const ColorMapObject* const cmap,
                           int transparent_color, int len, uint8_t* dst) {
  int i;
  for (i = 0; i < len; ++i) {
    if (src[i] != transparent_color) {
      // If a pixel in the current frame is transparent, we don't modify it, so
      // that we can see-through the corresponding pixel from an earlier frame.
      const GifColorType c = cmap->Colors[src[i]];
      dst[4 * i + 0] = c.Red;
      dst[4 * i + 1] = c.Green;
      dst[4 * i + 2] = c.Blue;
      dst[4 * i + 3] = 0xff;
    }
  }
}

static bool ReadFrameGIF(const SavedImage* const gif_image,
                         const ColorMapObject* cmap, int transparent_color,
                         int out_stride, uint8_t* const dst) {
  const GifImageDesc& image_desc = gif_image->ImageDesc;
  if (image_desc.ColorMap) {
      cmap = image_desc.ColorMap;
  }

  if (cmap == NULL || cmap->ColorCount != (1 << cmap->BitsPerPixel)) {
      fprintf(stderr, "Potentially corrupt color map.\n");
      return false;
  }

  const uint8_t* in = reinterpret_cast<uint8_t*>(gif_image->RasterBits);
  uint8_t* out =
      dst + image_desc.Top * out_stride + image_desc.Left * kNumChannels;

  for (int j = 0; j < image_desc.Height; ++j) {
      RemapPixelsGIF(in, cmap, transparent_color, image_desc.Width, out);
      in += image_desc.Width;
      out += out_stride;
  }
  return true;
}

// Read animated GIF bitstream from 'filename' into 'AnimatedImage' struct.
static bool ReadAnimatedGIF(const char filename[], AnimatedImage* const image,
                            bool dump_frames, const char dump_folder[]) {
  GifFileType* gif = DGifOpenFileName(filename, NULL);
  if (gif == NULL) {
    fprintf(stderr, "Could not read file: %s.\n", filename);
    return false;
  }

  const int gif_error = DGifSlurp(gif);
  if (gif_error != GIF_OK) {
    fprintf(stderr, "Could not parse image: %s.\n", filename);
    GIFDisplayError(gif, gif_error);
    DGifCloseFile(gif, NULL);
    return false;
  }

  // Animation properties.
  image->canvas_width = static_cast<uint32_t>(gif->SWidth);
  image->canvas_height = static_cast<uint32_t>(gif->SHeight);
  if (image->canvas_width > MAX_CANVAS_SIZE ||
      image->canvas_height > MAX_CANVAS_SIZE) {
    fprintf(stderr, "Invalid canvas dimension: %d x %d\n",
            image->canvas_width, image->canvas_height);
    DGifCloseFile(gif, NULL);
    return false;
  }
  image->loop_count = GetLoopCountGIF(gif);
  image->bgcolor = GetBackgroundColorGIF(gif);

  const uint32_t frame_count = static_cast<uint32_t>(gif->ImageCount);
  if (frame_count == 0) {
    DGifCloseFile(gif, NULL);
    return false;
  }

  if (image->canvas_width == 0 || image->canvas_height == 0) {
    image->canvas_width = gif->SavedImages[0].ImageDesc.Width;
    image->canvas_height = gif->SavedImages[0].ImageDesc.Height;
    gif->SavedImages[0].ImageDesc.Left = 0;
    gif->SavedImages[0].ImageDesc.Top = 0;
    if (image->canvas_width == 0 || image->canvas_height == 0) {
      fprintf(stderr, "Invalid canvas size in GIF.\n");
      DGifCloseFile(gif, NULL);
      return false;
    }
  }
  // Allocate frames.
  AllocateFrames(image, frame_count);

  const uint32_t canvas_width = image->canvas_width;
  const uint32_t canvas_height = image->canvas_height;

  // Decode and reconstruct frames.
  for (uint32_t i = 0; i < frame_count; ++i) {
    const int canvas_width_in_bytes = canvas_width * kNumChannels;
    const SavedImage* const curr_gif_image = &gif->SavedImages[i];
    GraphicsControlBlock curr_gcb = GraphicsControlBlock();
    DGifSavedExtensionToGCB(gif, i, &curr_gcb);

    DecodedFrame* const curr_frame = &image->frames[i];
    uint8_t* const curr_rgba = curr_frame->rgba.data();
    curr_frame->duration = GetFrameDurationGIF(gif, i);

    if (i == 0) {  // Initialize as transparent.
      curr_frame->is_key_frame = true;
      ZeroFillCanvas(curr_rgba, canvas_width, canvas_height);
    } else {
      DecodedFrame* const prev_frame = &image->frames[i - 1];
      const GifImageDesc& prev_desc = gif->SavedImages[i - 1].ImageDesc;
      GraphicsControlBlock prev_gcb = GraphicsControlBlock();
      DGifSavedExtensionToGCB(gif, i - 1, &prev_gcb);

      curr_frame->is_key_frame =
          IsKeyFrameGIF(prev_desc, prev_gcb.DisposalMode, prev_frame,
                        canvas_width, canvas_height);

      if (curr_frame->is_key_frame) {  // Initialize as transparent.
        ZeroFillCanvas(curr_rgba, canvas_width, canvas_height);
      } else {
        // Initialize with previous canvas.
        uint8_t* const prev_rgba = image->frames[i - 1].rgba.data();
        CopyCanvas(prev_rgba, curr_rgba, canvas_width, canvas_height);

        // Dispose previous frame rectangle.
        bool prev_frame_disposed =
            (prev_gcb.DisposalMode == DISPOSE_BACKGROUND ||
             prev_gcb.DisposalMode == DISPOSE_PREVIOUS);
        bool curr_frame_opaque =
            (curr_gcb.TransparentColor == NO_TRANSPARENT_COLOR);
        bool prev_frame_completely_covered =
            curr_frame_opaque &&
            CoversFrameGIF(curr_gif_image->ImageDesc, prev_desc);

        if (prev_frame_disposed && !prev_frame_completely_covered) {
          switch (prev_gcb.DisposalMode) {
            case DISPOSE_BACKGROUND: {
              ZeroFillFrameRect(curr_rgba, canvas_width_in_bytes,
                                prev_desc.Left, prev_desc.Top,
                                prev_desc.Width, prev_desc.Height);
              break;
            }
            case DISPOSE_PREVIOUS: {
              int src_frame_num = i - 2;
              while (src_frame_num >= 0) {
                GraphicsControlBlock src_frame_gcb = GraphicsControlBlock();
                DGifSavedExtensionToGCB(gif, src_frame_num, &src_frame_gcb);
                if (src_frame_gcb.DisposalMode != DISPOSE_PREVIOUS) break;
                --src_frame_num;
              }
              if (src_frame_num >= 0) {
                // Restore pixels inside previous frame rectangle to
                // corresponding pixels in source canvas.
                uint8_t* const src_frame_rgba =
                    image->frames[src_frame_num].rgba.data();
                CopyFrameRectangle(src_frame_rgba, curr_rgba,
                                   canvas_width_in_bytes,
                                   prev_desc.Left, prev_desc.Top,
                                   prev_desc.Width, prev_desc.Height);
              } else {
                // Source canvas doesn't exist. So clear previous frame
                // rectangle to background.
                ZeroFillFrameRect(curr_rgba, canvas_width_in_bytes,
                                  prev_desc.Left, prev_desc.Top,
                                  prev_desc.Width, prev_desc.Height);
              }
              break;
            }
            default:
              break;  // Nothing to do.
          }
        }
      }
    }

    // Decode current frame.
    if (!ReadFrameGIF(curr_gif_image, gif->SColorMap, curr_gcb.TransparentColor,
                      canvas_width_in_bytes, curr_rgba)) {
      DGifCloseFile(gif, NULL);
      return false;
    }

    if (dump_frames) {
      if (!DumpFrame(filename, dump_folder, i, curr_rgba,
                     canvas_width, canvas_height)) {
        DGifCloseFile(gif, NULL);
        return false;
      }
    }
  }
  DGifCloseFile(gif, NULL);
  return true;
}

#else

static bool ReadAnimatedGIF(const char filename[], AnimatedImage* const image,
                            bool dump_frames, const char dump_folder[]) {
  (void)filename;
  (void)image;
  (void)dump_frames;
  (void)dump_folder;
  fprintf(stderr, "GIF support not compiled. Please install the libgif-dev "
          "package before building.\n");
  return false;
}

#endif  // WEBP_HAVE_GIF

// -----------------------------------------------------------------------------

static bool ReadFile(const char filename[], std::string* filestr) {
  ifstream fin(filename, ios::binary);
  if (!fin.good()) return false;
  ostringstream strout;
  strout << fin.rdbuf();
  *filestr = strout.str();
  fin.close();
  return true;
}

bool ReadAnimatedImage(const char filename[], AnimatedImage* const image,
                       bool dump_frames, const char dump_folder[]) {
  std::string file_str;
  if (!ReadFile(filename, &file_str)) {
    fprintf(stderr, "Error reading file: %s\n", filename);
    return false;
  }

  if (IsWebP(file_str)) {
    return ReadAnimatedWebP(filename, file_str, image, dump_frames,
                            dump_folder);
  } else if (IsGIF(file_str)) {
    return ReadAnimatedGIF(filename, image, dump_frames, dump_folder);
  } else {
    fprintf(stderr,
            "Unknown file type: %s. Supported file types are WebP and GIF\n",
            filename);
    return false;
  }
}

void GetDiffAndPSNR(const uint8_t rgba1[], const uint8_t rgba2[],
                    uint32_t width, uint32_t height, int* const max_diff,
                    double* const psnr) {
  const uint32_t stride = width * kNumChannels;
  *max_diff = 0;
  double sse = 0.;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < stride; ++x) {
      const size_t offset = y * stride + x;
      const int diff = abs(rgba1[offset] - rgba2[offset]);
      if (diff > *max_diff) *max_diff = diff;
      sse += diff * diff;
    }
  }
  if (*max_diff == 0) {
    *psnr = 99.;  // PSNR when images are identical.
  } else {
    sse /= stride * height;
    *psnr = 10. * log10(255. * 255. / sse);
  }
}
