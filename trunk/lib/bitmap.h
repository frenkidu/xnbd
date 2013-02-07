/* 
 * xNBD - an enhanced Network Block Device program
 *
 * Copyright (C) 2008-2012 National Institute of Advanced Industrial Science
 * and Technology
 *
 * Author: Takahiro Hirofuchi <t.hirofuchi _at_ aist.go.jp>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "common.h"
#include "io.h"
#include "sys/mman.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>


size_t bitmap_size(unsigned long bits);
unsigned long *bitmap_alloc(unsigned long bits);
unsigned long *bitmap_open_file(const char *bitmapfile, unsigned long bits, size_t *cbitmaplen, int readonly, int zeroclear);
void bitmap_close_file(unsigned long *bitmap, size_t bitmaplen);
unsigned long *bitmap_create(char *bitmapfile, unsigned long size, int *cbitmapfd, size_t *cbitmaplen);
int bitmap_test(unsigned long *bitmap_array, unsigned long block_index);
void bitmap_on(unsigned long *bitmap_array, unsigned long block_index);
