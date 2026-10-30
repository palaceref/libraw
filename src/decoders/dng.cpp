/* -*- C++ -*-
 * Copyright 2019-2025 LibRaw LLC (info@libraw.org)
 *
 LibRaw uses code from dcraw.c -- Dave Coffin's raw photo decoder,
 dcraw.c is copyright 1997-2018 by Dave Coffin, dcoffin a cybercom o net.
 LibRaw do not use RESTRICTED code from dcraw.c

 LibRaw is free software; you can redistribute it and/or modify
 it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

/* Modified by PalaceRef, 2026-10-24 (CDDL 1.0 section 3.3): lossless_dng_load_raw's plain tiles decoded side by side
   on the parallel runner (palace_lossless_dng). The changes are marked "PalaceRef"; vendor/libraw/SOURCE.md lists them. */

#include "../../internal/dcraw_defs.h"
#include <atomic>
#include <vector>

void LibRaw::vc5_dng_load_raw_placeholder()
{
    // placeholder only, real decoding implemented in GPR SDK
    if(!dng_version)
    	throw LIBRAW_EXCEPTION_IO_EOF; // never reached
    throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT;
}
void LibRaw::jxl_dng_load_raw_placeholder()
{
  // placeholder only, real decoding implemented in DNG SDK
  throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT;
}

void LibRaw::adobe_copy_pixel(unsigned row, unsigned col, ushort **rp)
{
  int c;

  if (tiff_samples == 2 && shot_select)
    (*rp)++;
  if (raw_image)
  {
    if (row < raw_height && col < raw_width)
      RAW(row, col) = curve[**rp];
    *rp += tiff_samples;
  }
  else
  {
    if (row < raw_height && col < raw_width)
      FORC(int(tiff_samples))
    image[row * raw_width + col][c] = curve[(*rp)[c]];
    *rp += tiff_samples;
  }
  if (tiff_samples == 2 && shot_select)
    (*rp)--;
}
/* PalaceRef: lossless_dng_load_raw's tiles decoded side by side on the
   parallel runner (set_parallel_runner) where each is a plain lossless JPEG
   scan of one CFA sample a pixel: Huffman-coded (0xc3), no restart interval,
   no sRAW layout, its samples written as LibRaw's own loop writes them. The
   scan is read from memory, its stuffed bytes undone up to the first marker,
   by a 64-bit bit window: each difference's code and bits from one 32-bit
   peek, the samples those ljpeg_row (ljpeg_row_unrolled) makes. A tile in
   which ljpeg_row would report a data error (derror: a sample past its bits,
   the scan's bits run out), read a code its table lacks as nothing, or any
   other layout leaves the image to LibRaw's own loop, which writes every
   tile again. */
namespace
{
inline uint64_t palace_be64(const uchar *p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v = v << 8 | p[i];
  return v;
}
/* One tile's entropy-coded bytes from `at`: 0xff 0x00 as 0xff, up to the
   first marker or the file's end, then eight zero bytes; `bits` the bits
   before them. */
void palace_scan_bytes(LibRaw_abstract_datastream *input, INT64 at, std::vector<uchar> &out, size_t &bits)
{
  out.clear();
  std::vector<uchar> chunk(1 << 16);
  bool pending = false; // A 0xff whose next byte decides.
  for (bool done = false; !done;)
  {
    input->lock();
    input->seek(at, SEEK_SET);
    const int got = input->read(chunk.data(), 1, chunk.size());
    input->unlock();
    if (got <= 0)
      break;
    at += got;
    const uchar *p = chunk.data(), *end = p + got;
    if (pending)
    {
      pending = false;
      if (*p++)
        break;
      out.push_back(0xff);
    }
    while (p < end)
    {
      const uchar *ff = (const uchar *)memchr(p, 0xff, size_t(end - p));
      if (!ff)
      {
        out.insert(out.end(), p, end);
        break;
      }
      out.insert(out.end(), p, ff);
      if (ff + 1 == end)
      {
        pending = true;
        break;
      }
      if (ff[1])
      {
        done = true;
        break;
      }
      out.push_back(0xff);
      p = ff + 2;
    }
  }
  bits = out.size() * 8;
  out.insert(out.end(), 8, 0);
}
} // namespace

int LibRaw::palace_lossless_dng()
{
  if (!raw_image || tiff_samples != 1 || !(filters || colors == 1))
    return 0;
  const INT64 start = ftell(ifp);
  const bool short16 = !dng_version || dng_version >= 0x1010000; // A 16-bit difference: -32768.
  const unsigned span = MIN(unsigned(tile_width), unsigned(raw_width)); // A tile row's samples.
  struct Tile
  {
    struct jhead jh;
    INT64 data;
    unsigned trow, tcol;
  };
  std::vector<Tile> tiles;
  unsigned trow = 0, tcol = 0;
  bool usable = span > 0;
  std::atomic<bool> failed(false);
  const auto end_tiles = [&] {
    for (auto &t : tiles)
      ljpeg_end(&t.jh);
    tiles.clear();
  };
  /* Tiles a batch at a time, a few a worker: their headers in order as
     LibRaw's loop reads them (its seeks the same), then their scans side by side. */
  while (usable && trow < raw_height)
  {
    checkCancel();
    while (trow < raw_height && int(tiles.size()) < parallel_workers * 2)
    {
      const INT64 save = ftell(ifp);
      if (tile_length < INT_MAX)
        fseek(ifp, get4(), SEEK_SET);
      tiles.emplace_back();
      Tile &t = tiles.back();
      int started = 0;
      try
      {
        started = ljpeg_start(&t.jh, 0);
      }
      catch (...)
      {
        ljpeg_end(&t.jh);
        tiles.pop_back();
        end_tiles();
        throw;
      }
      if (!started)
      {
        ljpeg_end(&t.jh);
        tiles.pop_back();
        usable = false;
        break;
      }
      t.data = ftell(ifp);
      t.trow = trow;
      t.tcol = tcol;
      const struct jhead &jh = t.jh;
      bool tables = true;
      for (int c = 0; c < jh.clrs; c++)
        tables = tables && jh.huff[c] && jh.huff[c][0] >= 1 && jh.huff[c][0] <= 16;
      const uint64_t samples = uint64_t(jh.wide) * jh.clrs * jh.high;
      if (jh.algo != 0xc3 || jh.sraw || jh.restart != INT_MAX || jh.bits < 1 || !tables ||
          (jh.clrs > 1 && unsigned(jh.clrs * jh.wide * jh.clrs) == unsigned(raw_width)) ||
          (tile_length < INT_MAX && (samples + span - 1) / span > uint64_t(tile_length)))
        usable = false;
      fseek(ifp, save + 4, SEEK_SET);
      if ((tcol += tile_width) >= raw_width)
        trow += tile_length + (tcol = 0);
      if (!usable)
        break;
    }
    if (!usable)
      break;
    const INT64 resume = ftell(ifp);
    auto decode = [&](int index, int)
    {
      if (failed.load(std::memory_order_relaxed))
        return;
      const Tile &t = tiles[index];
      const struct jhead &jh = t.jh;
      std::vector<uchar> scan;
      size_t available = 0;
      palace_scan_bytes(ifp, t.data, scan, available);
      const int clrs = jh.clrs, n = jh.wide * clrs;
      std::vector<ushort> rows(size_t(2) * n);
      int vpred[6];
      for (int c = 0; c < clrs; c++)
        vpred[c] = 1 << (jh.bits - 1);
      const uchar *u = scan.data();
      size_t bit = 0;
      unsigned row = 0, col = 0; // Where the next sample goes, in the tile.
      for (int jrow = 0; jrow < jh.high; jrow++)
      {
        ushort *now = rows.data() + size_t(n) * (jrow & 1), *above = rows.data() + size_t(n) * (~jrow & 1);
        for (int i = 0, c = 0; i < n; i++, c = c + 1 == clrs ? 0 : c + 1)
        {
          if (bit > available)
          {
            failed = true;
            return;
          }
          const ushort *huff = jh.huff[c];
          const uint32_t peek = uint32_t((palace_be64(u + (bit >> 3)) << (bit & 7)) >> 32);
          const ushort code = huff[1 + (peek >> (32 - huff[0]))];
          const unsigned length = code >> 8, size = code & 255;
          if (!length || size > 16)
          {
            failed = true;
            return;
          }
          int diff;
          if (size == 16 && short16)
          {
            diff = -32768;
            bit += length;
          }
          else
          {
            diff = size ? int((peek << length) >> (32 - size)) : 0;
            if (size && !(diff & (1 << (size - 1))))
              diff -= (1 << size) - 1;
            bit += length + size;
          }
          int pred;
          if (i < clrs)
            pred = (vpred[c] += diff) - diff;
          else if (!jrow)
            pred = now[i - clrs];
          else
            switch (jh.psv)
            {
            case 1:
              pred = now[i - clrs];
              break;
            case 2:
              pred = above[i];
              break;
            case 3:
              pred = above[i - clrs];
              break;
            case 4:
              pred = now[i - clrs] + above[i] - above[i - clrs];
              break;
            case 5:
              pred = now[i - clrs] + ((above[i] - above[i - clrs]) >> 1);
              break;
            case 6:
              pred = above[i] + ((now[i - clrs] - above[i - clrs]) >> 1);
              break;
            case 7:
              pred = (now[i - clrs] + above[i]) >> 1;
              break;
            default:
              pred = 0;
            }
          if ((now[i] = ushort(pred + diff)) >> jh.bits)
          {
            failed = true;
            return;
          }
        }
        // adobe_copy_pixel's for one sample a pixel, the row's samples in order.
        for (int jcol = 0; jcol < n; jcol++)
        {
          const unsigned r = t.trow + row, x = t.tcol + col;
          if (r < raw_height && x < raw_width)
            RAW(r, x) = curve[now[jcol]];
          if (++col >= tile_width || col >= raw_width)
            row += 1 + (col = 0);
        }
      }
      if (bit > available)
        failed = true;
    };
    try
    {
      parallel_for(int(tiles.size()), decode);
    }
    catch (...)
    {
      end_tiles();
      throw;
    }
    end_tiles();
    fseek(ifp, resume, SEEK_SET);
    if (failed)
      usable = false;
  }
  end_tiles();
  if (!usable)
  {
    fseek(ifp, start, SEEK_SET);
    return 0;
  }
  return 1;
}

void LibRaw::lossless_dng_load_raw()
{
  unsigned trow = 0, tcol = 0, jwide, jrow, jcol, row, col, i, j;
  INT64 save;
  struct jhead jh;
  ushort *rp;

  int ss = shot_select;
  shot_select = libraw_internal_data.unpacker_data.dng_frames[LIM(ss,0,(LIBRAW_IFD_MAXCOUNT*2-1))] & 0xff;
  /* PalaceRef: the tiles side by side where they are plain (above). */
  if (palace_lossless_dng())
  {
    shot_select = ss;
    return;
  }

  while (trow < raw_height)
  {
    checkCancel();
    save = ftell(ifp);
    if (tile_length < INT_MAX)
      fseek(ifp, get4(), SEEK_SET);
    if (!ljpeg_start(&jh, 0))
      break;
    jwide = jh.wide;
    if (filters || colors == 1)
      jwide *= jh.clrs;

    if(filters && (tiff_samples == 2)) // Fuji Super CCD
        jwide /= 2;
    try
    {
      switch (jh.algo)
      {
      case 0xc1:
        jh.vpred[0] = 16384;
        getbits(-1);
        for (jrow = 0; jrow + 7 < (unsigned)jh.high; jrow += 8)
        {
          checkCancel();
          for (jcol = 0; jcol + 7 < (unsigned)jh.wide; jcol += 8)
          {
            ljpeg_idct(&jh);
            rp = jh.idct;
            row = trow + jcol / tile_width + jrow * 2;
            col = tcol + jcol % tile_width;
            for (i = 0; i < 16; i += 2)
              for (j = 0; j < 8; j++)
                adobe_copy_pixel(row + i, col + j, &rp);
          }
        }
        break;
      case 0xc3:
        for (row = col = jrow = 0; jrow < (unsigned)jh.high; jrow++)
        {
          checkCancel();
          rp = ljpeg_row(jrow, &jh);
          if (tiff_samples == 1 && jh.clrs > 1 && jh.clrs * jwide == raw_width)
            for (jcol = 0; jcol < jwide * jh.clrs; jcol++)
            {
              adobe_copy_pixel(trow + row, tcol + col, &rp);
              if (++col >= tile_width || col >= raw_width)
                row += 1 + (col = 0);
            }
          else
            for (jcol = 0; jcol < jwide; jcol++)
            {
              adobe_copy_pixel(trow + row, tcol + col, &rp);
              if (++col >= tile_width || col >= raw_width)
                row += 1 + (col = 0);
            }
        }
      }
    }
    catch (...)
    {
      ljpeg_end(&jh);
      shot_select = ss;
      throw;
    }
    fseek(ifp, save + 4, SEEK_SET);
    if ((tcol += tile_width) >= raw_width)
      trow += tile_length + (tcol = 0);
    ljpeg_end(&jh);
  }
  shot_select = ss;
}

void LibRaw::packed_dng_load_raw()
{
  ushort *pixel, *rp;
  unsigned row, col;

  if (tile_length < INT_MAX)
  {
      packed_tiled_dng_load_raw();
      return;
  }

  int ss = shot_select;
  shot_select = libraw_internal_data.unpacker_data.dng_frames[LIM(ss,0,(LIBRAW_IFD_MAXCOUNT*2-1))] & 0xff;

  pixel = (ushort *)calloc(raw_width, tiff_samples * sizeof *pixel);
  try
  {
    for (row = 0; row < raw_height; row++)
    {
      checkCancel();
      if (tiff_bps == 16)
        read_shorts(pixel, raw_width * tiff_samples);
      else
      {
        getbits(-1);
        for (col = 0; col < raw_width * tiff_samples; col++)
          pixel[col] = getbits(tiff_bps);
      }
      for (rp = pixel, col = 0; col < raw_width; col++)
        adobe_copy_pixel(row, col, &rp);
    }
  }
  catch (...)
  {
    free(pixel);
    shot_select = ss;
    throw;
  }
  free(pixel);
  shot_select = ss;
}
#ifdef NO_JPEG
void LibRaw::lossy_dng_load_raw() {}
#else

static void jpegErrorExit_d(j_common_ptr /*cinfo*/)
{
  throw LIBRAW_EXCEPTION_DECODE_JPEG;
}

void LibRaw::lossy_dng_load_raw()
{
  if (!image)
    throw LIBRAW_EXCEPTION_IO_CORRUPT;
  struct jpeg_decompress_struct cinfo;

  unsigned sorder = order, ntags, opcode, deg, i, j, c;
  unsigned trow = 0, tcol = 0, row, col;
  INT64 save = data_offset - 4;
  ushort cur[4][256];
  double coeff[9], tot;

  if (meta_offset)
  {
    fseek(ifp, meta_offset, SEEK_SET);
    order = 0x4d4d;
    ntags = get4();
    while (ntags--)
    {
      opcode = get4();
      get4();
      get4();
      if (opcode != 8)
      {
        fseek(ifp, get4(), SEEK_CUR);
        continue;
      }
      fseek(ifp, 20, SEEK_CUR);
      if ((c = get4()) > 3)
        break;
      fseek(ifp, 12, SEEK_CUR);
      if ((deg = get4()) > 8)
        break;
      for (i = 0; i <= deg && i < 9; i++)
        coeff[i] = getreal(LIBRAW_EXIFTAG_TYPE_DOUBLE);
      for (i = 0; i < 256; i++)
      {
        for (tot = j = 0; j <= deg; j++)
          tot += coeff[j] * pow(i / 255.0, (int)j);
        cur[c][i] = (ushort)(tot * 0xffff);
      }
    }
    order = sorder;
  }
  else
  {
    gamma_curve(1 / 2.4, 12.92, 1, 255);
    FORC4 memcpy(cur[c], curve, sizeof cur[0]);
  }

  struct jpeg_error_mgr pub;
  cinfo.err = jpeg_std_error(&pub);
  pub.error_exit = jpegErrorExit_d;

  std::vector<JSAMPLE> buf;

  jpeg_create_decompress(&cinfo);

  while (trow < raw_height)
  {
    fseek(ifp, save += 4, SEEK_SET);
    if (tile_length < INT_MAX)
      fseek(ifp, get4(), SEEK_SET);
    if (libraw_internal_data.internal_data.input->jpeg_src(&cinfo) == -1)
    {
      jpeg_destroy_decompress(&cinfo);
      throw LIBRAW_EXCEPTION_DECODE_JPEG;
    }
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);
	if (cinfo.output_components != colors)
		throw LIBRAW_EXCEPTION_DECODE_JPEG;

	if (buf.size() < cinfo.output_width * cinfo.output_components)
		buf = std::vector<JSAMPLE>(cinfo.output_width * cinfo.output_components,0);

    try
    {
      JSAMPLE *buffer_array[1];
      buffer_array[0] = buf.data();
      while (cinfo.output_scanline < cinfo.output_height &&
             (row = trow + cinfo.output_scanline) < height)
      {
        checkCancel();
        jpeg_read_scanlines(&cinfo, buffer_array, 1);
        for (col = 0; col < cinfo.output_width && tcol + col < width; col++)
        {
          FORC(colors) image[row * width + tcol + col][c] = cur[c][buf[col*colors+c]];
        }
      }
    }
    catch (...)
    {
      jpeg_destroy_decompress(&cinfo);
      throw;
    }
    jpeg_abort_decompress(&cinfo);
    if ((tcol += tile_width) >= raw_width)
      trow += tile_length + (tcol = 0);
  }
  jpeg_destroy_decompress(&cinfo);
  maximum = 0xffff;
}
#endif
