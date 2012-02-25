/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2007,2008,2009 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl-private.h"
#include "cogl-bitmap-private.h"

#include <string.h>

#define component_type guint8
/* We want to specially optimise the packing when we are converting
   to/from an 8-bit type so that it won't do anything. That way for
   example if we are just doing a swizzle conversion then the inner
   loop for the conversion will be really simple */
#define UNPACK_BYTE(b) (b)
#define PACK_BYTE(b) (b)
#include "cogl-bitmap-packing.h"
#undef PACK_BYTE
#undef UNPACK_BYTE
#undef component_type

#define component_type guint16
#define UNPACK_BYTE(b) (((b) * 65535 + 127) / 255)
#define PACK_BYTE(b) (((b) * 255 + 32767) / 65535)
#include "cogl-bitmap-packing.h"
#undef PACK_BYTE
#undef UNPACK_BYTE
#undef component_type

/* (Un)Premultiplication */

inline static void
_cogl_unpremult_alpha_0 (guint8 *dst)
{
  dst[0] = 0;
  dst[1] = 0;
  dst[2] = 0;
  dst[3] = 0;
}

inline static void
_cogl_unpremult_alpha_last (guint8 *dst)
{
  guint8 alpha = dst[3];

  dst[0] = (dst[0] * 255) / alpha;
  dst[1] = (dst[1] * 255) / alpha;
  dst[2] = (dst[2] * 255) / alpha;
}

inline static void
_cogl_unpremult_alpha_first (guint8 *dst)
{
  guint8 alpha = dst[0];

  dst[1] = (dst[1] * 255) / alpha;
  dst[2] = (dst[2] * 255) / alpha;
  dst[3] = (dst[3] * 255) / alpha;
}

/* No division form of floor((c*a + 128)/255) (I first encountered
 * this in the RENDER implementation in the X server.) Being exact
 * is important for a == 255 - we want to get exactly c.
 */
#define MULT(d,a,t)                             \
  G_STMT_START {                                \
    t = d * a + 128;                            \
    d = ((t >> 8) + t) >> 8;                    \
  } G_STMT_END

inline static void
_cogl_premult_alpha_last (guint8 *dst)
{
  guint8 alpha = dst[3];
  /* Using a separate temporary per component has given slightly better
   * code generation with GCC in the past; it shouldn't do any worse in
   * any case.
   */
  unsigned int t1, t2, t3;
  MULT(dst[0], alpha, t1);
  MULT(dst[1], alpha, t2);
  MULT(dst[2], alpha, t3);
}

inline static void
_cogl_premult_alpha_first (guint8 *dst)
{
  guint8 alpha = dst[0];
  unsigned int t1, t2, t3;

  MULT(dst[1], alpha, t1);
  MULT(dst[2], alpha, t2);
  MULT(dst[3], alpha, t3);
}

#undef MULT

/* Use the SSE optimized version to premult four pixels at once when
   it is available. The same assembler code works for x86 and x86-64
   because it doesn't refer to any non-SSE registers directly */
#if defined(__SSE2__) && defined(__GNUC__) \
  && (defined(__x86_64) || defined(__i386))
#define COGL_USE_PREMULT_SSE2
#endif

#ifdef COGL_USE_PREMULT_SSE2

inline static void
_cogl_premult_alpha_last_four_pixels_sse2 (guint8 *p)
{
  /* 8 copies of 128 used below */
  static const gint16 eight_halves[8] __attribute__ ((aligned (16))) =
    { 128, 128, 128, 128, 128, 128, 128, 128 };
  /* Mask of the rgb components of the four pixels */
  static const gint8 just_rgb[16] __attribute__ ((aligned (16))) =
    { 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00,
      0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00 };
  /* Each SSE register only holds two pixels because we need to work
     with 16-bit intermediate values. We still do four pixels by
     interleaving two registers in the hope that it will pipeline
     better */
  asm (/* Load eight_halves into xmm5 for later */
       "movdqa (%1), %%xmm5\n"
       /* Clear xmm3 */
       "pxor %%xmm3, %%xmm3\n"
       /* Load two pixels from p into the low half of xmm0 */
       "movlps (%0), %%xmm0\n"
       /* Load the next set of two pixels from p into the low half of xmm1 */
       "movlps 8(%0), %%xmm1\n"
       /* Unpack 8 bytes from the low quad-words in each register to 8
          16-bit values */
       "punpcklbw %%xmm3, %%xmm0\n"
       "punpcklbw %%xmm3, %%xmm1\n"
       /* Copy alpha values of the first pixel in xmm0 to all
          components of the first pixel in xmm2 */
       "pshuflw $255, %%xmm0, %%xmm2\n"
       /* same for xmm1 and xmm3 */
       "pshuflw $255, %%xmm1, %%xmm3\n"
       /* The above also copies the second pixel directly so we now
          want to replace the RGB components with copies of the alpha
          components */
       "pshufhw $255, %%xmm2, %%xmm2\n"
       "pshufhw $255, %%xmm3, %%xmm3\n"
       /* Multiply the rgb components by the alpha */
       "pmullw %%xmm2, %%xmm0\n"
       "pmullw %%xmm3, %%xmm1\n"
       /* Add 128 to each component */
       "paddw %%xmm5, %%xmm0\n"
       "paddw %%xmm5, %%xmm1\n"
       /* Copy the results to temporary registers xmm4 and xmm5 */
       "movdqa %%xmm0, %%xmm4\n"
       "movdqa %%xmm1, %%xmm5\n"
       /* Divide the results by 256 */
       "psrlw $8, %%xmm0\n"
       "psrlw $8, %%xmm1\n"
       /* Add the temporaries back in */
       "paddw %%xmm4, %%xmm0\n"
       "paddw %%xmm5, %%xmm1\n"
       /* Divide again */
       "psrlw $8, %%xmm0\n"
       "psrlw $8, %%xmm1\n"
       /* Pack the results back as bytes */
       "packuswb %%xmm1, %%xmm0\n"
       /* Load just_rgb into xmm3 for later */
       "movdqa (%2), %%xmm3\n"
       /* Reload all four pixels into xmm2 */
       "movups (%0), %%xmm2\n"
       /* Mask out the alpha from the results */
       "andps %%xmm3, %%xmm0\n"
       /* Mask out the RGB from the original four pixels */
       "andnps %%xmm2, %%xmm3\n"
       /* Combine the two to get the right alpha values */
       "orps %%xmm3, %%xmm0\n"
       /* Write to memory */
       "movdqu %%xmm0, (%0)\n"
       : /* no outputs */
       : "r" (p), "r" (eight_halves), "r" (just_rgb)
       : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5");
}

#endif /* COGL_USE_PREMULT_SSE2 */

static void
_cogl_bitmap_premult_unpacked_span_guint8 (guint8 *data,
                                           int width)
{
#ifdef COGL_USE_PREMULT_SSE2

  /* Process 4 pixels at a time */
  while (width >= 4)
    {
      _cogl_premult_alpha_last_four_pixels_sse2 (data);
      data += 4 * 4;
      width -= 4;
    }

  /* If there are any pixels left we will fall through and
     handle them below */

#endif /* COGL_USE_PREMULT_SSE2 */

  while (width-- > 0)
    {
      _cogl_premult_alpha_last (data);
      data += 4;
    }
}

static void
_cogl_bitmap_unpremult_unpacked_span_guint8 (guint8 *data,
                                             int width)
{
  int x;

  for (x = 0; x < width; x++)
    {
      if (data[3] == 0)
        _cogl_unpremult_alpha_0 (data);
      else
        _cogl_unpremult_alpha_last (data);
      data += 4;
    }
}

static void
_cogl_bitmap_unpremult_unpacked_span_guint16 (guint16 *data,
                                              int width)
{
  while (width-- > 0)
    {
      guint16 alpha = data[3];

      if (alpha == 0)
        memset (data, 0, sizeof (guint16) * 3);
      else
        {
          data[0] = (data[0] * 65535) / alpha;
          data[1] = (data[1] * 65535) / alpha;
          data[2] = (data[2] * 65535) / alpha;
        }
    }
}

static void
_cogl_bitmap_premult_unpacked_span_guint16 (guint16 *data,
                                            int width)
{
  while (width-- > 0)
    {
      guint16 alpha = data[3];

      data[0] = (data[0] * alpha) / 65535;
      data[1] = (data[1] * alpha) / 65535;
      data[2] = (data[2] * alpha) / 65535;
    }
}

static gboolean
_cogl_bitmap_can_fast_premult (CoglPixelFormat format)
{
  switch (format & ~COGL_PREMULT_BIT)
    {
    case COGL_PIXEL_FORMAT_RGBA_8888:
    case COGL_PIXEL_FORMAT_BGRA_8888:
    case COGL_PIXEL_FORMAT_ARGB_8888:
    case COGL_PIXEL_FORMAT_ABGR_8888:
      return TRUE;

    default:
      return FALSE;
    }
}

static gboolean
_cogl_bitmap_needs_short_temp_buffer (CoglPixelFormat format)
{
  /* If the format is using more than 8 bits per component then we'll
     unpack into a 16-bit per component buffer instead of 8-bit so we
     won't lose as much precision. If we ever add support for formats
     with more than 16 bits for at least one of the components then we
     should probably do something else here, maybe convert to
     floats */
  switch (format)
    {
    case COGL_PIXEL_FORMAT_ANY:
    case COGL_PIXEL_FORMAT_YUV:
      g_assert_not_reached ();

    case COGL_PIXEL_FORMAT_A_8:
    case COGL_PIXEL_FORMAT_RGB_565:
    case COGL_PIXEL_FORMAT_RGBA_4444:
    case COGL_PIXEL_FORMAT_RGBA_5551:
    case COGL_PIXEL_FORMAT_G_8:
    case COGL_PIXEL_FORMAT_RGB_888:
    case COGL_PIXEL_FORMAT_BGR_888:
    case COGL_PIXEL_FORMAT_RGBA_8888:
    case COGL_PIXEL_FORMAT_BGRA_8888:
    case COGL_PIXEL_FORMAT_ARGB_8888:
    case COGL_PIXEL_FORMAT_ABGR_8888:
    case COGL_PIXEL_FORMAT_RGBA_8888_PRE:
    case COGL_PIXEL_FORMAT_BGRA_8888_PRE:
    case COGL_PIXEL_FORMAT_ARGB_8888_PRE:
    case COGL_PIXEL_FORMAT_ABGR_8888_PRE:
    case COGL_PIXEL_FORMAT_RGBA_4444_PRE:
    case COGL_PIXEL_FORMAT_RGBA_5551_PRE:
      return FALSE;

    case COGL_PIXEL_FORMAT_RGBA_1010102:
    case COGL_PIXEL_FORMAT_BGRA_1010102:
    case COGL_PIXEL_FORMAT_ARGB_2101010:
    case COGL_PIXEL_FORMAT_ABGR_2101010:
    case COGL_PIXEL_FORMAT_RGBA_1010102_PRE:
    case COGL_PIXEL_FORMAT_BGRA_1010102_PRE:
    case COGL_PIXEL_FORMAT_ARGB_2101010_PRE:
    case COGL_PIXEL_FORMAT_ABGR_2101010_PRE:
      return TRUE;
    }

  g_assert_not_reached ();
}

gboolean
_cogl_bitmap_convert_into_bitmap (CoglBitmap *src_bmp,
                                  CoglBitmap *dst_bmp)
{
  guint8          *src_data;
  guint8          *dst_data;
  guint8          *src;
  guint8          *dst;
  void            *tmp_row;
  int              src_rowstride;
  int              dst_rowstride;
  int              y;
  int              width, height;
  CoglPixelFormat  src_format;
  CoglPixelFormat  dst_format;
  gboolean         use_16;
  gboolean         need_premult;

  src_format = _cogl_bitmap_get_format (src_bmp);
  src_rowstride = _cogl_bitmap_get_rowstride (src_bmp);
  dst_format = _cogl_bitmap_get_format (dst_bmp);
  dst_rowstride = _cogl_bitmap_get_rowstride (dst_bmp);
  width = _cogl_bitmap_get_width (src_bmp);
  height = _cogl_bitmap_get_height (src_bmp);

  _COGL_RETURN_VAL_IF_FAIL (width == _cogl_bitmap_get_width (dst_bmp), FALSE);
  _COGL_RETURN_VAL_IF_FAIL (height == _cogl_bitmap_get_height (dst_bmp), FALSE);

  need_premult
    = ((src_format & COGL_PREMULT_BIT) != (dst_format & COGL_PREMULT_BIT) &&
       src_format != COGL_PIXEL_FORMAT_A_8 &&
       dst_format != COGL_PIXEL_FORMAT_A_8 &&
       (src_format & dst_format & COGL_A_BIT));

  /* If the base format is the same then we can just copy the bitmap
     instead */
  if ((src_format & ~COGL_PREMULT_BIT) == (dst_format & ~COGL_PREMULT_BIT) &&
      (!need_premult || _cogl_bitmap_can_fast_premult (dst_format)))
    {
      if (!_cogl_bitmap_copy_subregion (src_bmp, dst_bmp,
                                        0, 0, /* src_x / src_y */
                                        0, 0, /* dst_x / dst_y */
                                        width, height))
        return FALSE;

      if (need_premult)
        {
          if ((dst_format & COGL_PREMULT_BIT))
            {
              if (!_cogl_bitmap_premult (dst_bmp))
                return FALSE;
            }
          else
            {
              if (!_cogl_bitmap_unpremult (dst_bmp))
                return FALSE;
            }
        }

      return TRUE;
    }

  src_data = _cogl_bitmap_map (src_bmp, COGL_BUFFER_ACCESS_READ, 0);
  if (src_data == NULL)
    return FALSE;
  dst_data = _cogl_bitmap_map (dst_bmp,
                               COGL_BUFFER_ACCESS_WRITE,
                               COGL_BUFFER_MAP_HINT_DISCARD);
  if (dst_data == NULL)
    {
      _cogl_bitmap_unmap (src_bmp);
      return FALSE;
    }

  use_16 = _cogl_bitmap_needs_short_temp_buffer (dst_format);

  /* Allocate a buffer to hold a temporary RGBA row */
  tmp_row = g_malloc (width *
                      (use_16 ? sizeof (guint16) : sizeof (guint8)) * 4);

  /* FIXME: Optimize */
  for (y = 0; y < height; y++)
    {
      src = src_data + y * src_rowstride;
      dst = dst_data + y * dst_rowstride;

      if (use_16)
        _cogl_unpack_guint16 (src_format, src, tmp_row, width);
      else
        _cogl_unpack_guint8 (src_format, src, tmp_row, width);

      /* Handle premultiplication */
      if (need_premult)
        {
          if (dst_format & COGL_PREMULT_BIT)
            {
              if (use_16)
                _cogl_bitmap_premult_unpacked_span_guint16 (tmp_row, width);
              else
                _cogl_bitmap_premult_unpacked_span_guint8 (tmp_row, width);
            }
          else
            {
              if (use_16)
                _cogl_bitmap_unpremult_unpacked_span_guint16 (tmp_row, width);
              else
                _cogl_bitmap_unpremult_unpacked_span_guint8 (tmp_row, width);
            }
        }

      if (use_16)
        _cogl_pack_guint16 (dst_format, tmp_row, dst, width);
      else
        _cogl_pack_guint8 (dst_format, tmp_row, dst, width);
    }

  _cogl_bitmap_unmap (src_bmp);
  _cogl_bitmap_unmap (dst_bmp);

  g_free (tmp_row);

  return TRUE;
}

CoglBitmap *
_cogl_bitmap_convert (CoglBitmap *src_bmp,
                      CoglPixelFormat dst_format)
{
  int dst_bpp;
  int dst_rowstride;
  guint8 *dst_data;
  CoglBitmap *dst_bmp;
  int width, height;

  width = _cogl_bitmap_get_width (src_bmp);
  height = _cogl_bitmap_get_height (src_bmp);
  dst_bpp = _cogl_pixel_format_get_bytes_per_pixel (dst_format);
  dst_rowstride = (sizeof (guint8) * dst_bpp * width + 3) & ~3;

  /* Allocate a new buffer to hold converted data */
  dst_data = g_malloc (height * dst_rowstride);

  dst_bmp = _cogl_bitmap_new_from_data (dst_data,
                                        dst_format,
                                        width, height,
                                        dst_rowstride,
                                        (CoglBitmapDestroyNotify) g_free,
                                        NULL);

  if (!_cogl_bitmap_convert_into_bitmap (src_bmp, dst_bmp))
    {
      cogl_object_unref (dst_bmp);
      return NULL;
    }

  return dst_bmp;
}

gboolean
_cogl_bitmap_unpremult (CoglBitmap *bmp)
{
  guint8          *p, *data;
  guint16         *tmp_row;
  int              x,y;
  CoglPixelFormat  format;
  int              width, height;
  int              rowstride;

  format = _cogl_bitmap_get_format (bmp);
  width = _cogl_bitmap_get_width (bmp);
  height = _cogl_bitmap_get_height (bmp);
  rowstride = _cogl_bitmap_get_rowstride (bmp);

  if ((data = _cogl_bitmap_map (bmp,
                                COGL_BUFFER_ACCESS_READ |
                                COGL_BUFFER_ACCESS_WRITE,
                                0)) == NULL)
    return FALSE;

  /* If we can't directly unpremult the data inline then we'll
     allocate a temporary row and unpack the data. This assumes if we
     can fast premult then we can also fast unpremult */
  if (_cogl_bitmap_can_fast_premult (format))
    tmp_row = NULL;
  else
    tmp_row = g_malloc (sizeof (guint16) * 4 * width);

  for (y = 0; y < height; y++)
    {
      p = (guint8*) data + y * rowstride;

      if (tmp_row)
        {
          _cogl_unpack_guint16 (format, p, tmp_row, width);
          _cogl_bitmap_unpremult_unpacked_span_guint16 (tmp_row, width);
          _cogl_pack_guint16 (format, tmp_row, p, width);
        }
      else
        {
          if (format & COGL_AFIRST_BIT)
            {
              for (x = 0; x < width; x++)
                {
                  if (p[0] == 0)
                    _cogl_unpremult_alpha_0 (p);
                  else
                    _cogl_unpremult_alpha_first (p);
                  p += 4;
                }
            }
          else
            _cogl_bitmap_unpremult_unpacked_span_guint8 (p, width);
        }
    }

  g_free (tmp_row);

  _cogl_bitmap_unmap (bmp);

  _cogl_bitmap_set_format (bmp, format & ~COGL_PREMULT_BIT);

  return TRUE;
}

gboolean
_cogl_bitmap_premult (CoglBitmap *bmp)
{
  guint8          *p, *data;
  guint16         *tmp_row;
  int              x,y;
  CoglPixelFormat  format;
  int              width, height;
  int              rowstride;

  format = _cogl_bitmap_get_format (bmp);
  width = _cogl_bitmap_get_width (bmp);
  height = _cogl_bitmap_get_height (bmp);
  rowstride = _cogl_bitmap_get_rowstride (bmp);

  if ((data = _cogl_bitmap_map (bmp,
                                COGL_BUFFER_ACCESS_READ |
                                COGL_BUFFER_ACCESS_WRITE,
                                0)) == NULL)
    return FALSE;

  /* If we can't directly premult the data inline then we'll allocate
     a temporary row and unpack the data. */
  if (_cogl_bitmap_can_fast_premult (format))
    tmp_row = NULL;
  else
    tmp_row = g_malloc (sizeof (guint16) * 4 * width);

  for (y = 0; y < height; y++)
    {
      p = (guint8*) data + y * rowstride;

      if (tmp_row)
        {
          _cogl_unpack_guint16 (format, p, tmp_row, width);
          _cogl_bitmap_premult_unpacked_span_guint16 (tmp_row, width);
          _cogl_pack_guint16 (format, tmp_row, p, width);
        }
      else
        {
          if (format & COGL_AFIRST_BIT)
            {
              for (x = 0; x < width; x++)
                {
                  _cogl_premult_alpha_first (p);
                  p += 4;
                }
            }
          else
            _cogl_bitmap_premult_unpacked_span_guint8 (p, width);
        }
    }

  g_free (tmp_row);

  _cogl_bitmap_unmap (bmp);

  _cogl_bitmap_set_format (bmp, format | COGL_PREMULT_BIT);

  return TRUE;
}